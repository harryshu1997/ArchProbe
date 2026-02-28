// ArchProbe Analyzer — Reads ArchProbe CSV/JSON output and produces
// ASCII graphs + architecture summary text.
// No external dependencies. C++14. Cross-compiles to Android.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────
// Utility helpers
// ─────────────────────────────────────────────────────────────────────

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> tokens;
    std::istringstream iss(s);
    std::string tok;
    while (std::getline(iss, tok, delim)) tokens.push_back(trim(tok));
    return tokens;
}

static std::string formatBytes(double bytes) {
    const char* units[] = {"B", "KB", "MB", "GB"};
    int u = 0;
    double v = bytes;
    while (v >= 1024.0 && u < 3) { v /= 1024.0; u++; }
    char buf[64];
    if (v == (int)v) snprintf(buf, sizeof(buf), "%d %s", (int)v, units[u]);
    else             snprintf(buf, sizeof(buf), "%.1f %s", v, units[u]);
    return buf;
}

static std::string formatNumber(double v) {
    char buf[64];
    if (v >= 1e9)      snprintf(buf, sizeof(buf), "%.2f G", v / 1e9);
    else if (v >= 1e6) snprintf(buf, sizeof(buf), "%.2f M", v / 1e6);
    else if (v >= 1e3) snprintf(buf, sizeof(buf), "%.2f K", v / 1e3);
    else if (v == (int)v) snprintf(buf, sizeof(buf), "%d", (int)v);
    else               snprintf(buf, sizeof(buf), "%.2f", v);
    return buf;
}

// ─────────────────────────────────────────────────────────────────────
// Simple JSON parser (handles ArchProbeReport.json structure)
// Supports: nested objects, string/number/bool values
// ─────────────────────────────────────────────────────────────────────

struct JsonValue {
    enum Type { NONE, STRING, NUMBER, BOOL, OBJECT };
    Type type = NONE;
    std::string sval;
    double nval = 0;
    bool bval = false;
    std::map<std::string, JsonValue> obj;

    bool isObj() const { return type == OBJECT; }
    double num(const std::string& k, double def = 0) const {
        auto it = obj.find(k);
        return (it != obj.end() && it->second.type == NUMBER) ? it->second.nval : def;
    }
    std::string str(const std::string& k, const std::string& def = "") const {
        auto it = obj.find(k);
        return (it != obj.end() && it->second.type == STRING) ? it->second.sval : def;
    }
    bool boolean(const std::string& k, bool def = false) const {
        auto it = obj.find(k);
        return (it != obj.end() && it->second.type == BOOL) ? it->second.bval : def;
    }
    const JsonValue& child(const std::string& k) const {
        static JsonValue empty;
        auto it = obj.find(k);
        return (it != obj.end()) ? it->second : empty;
    }
};

class JsonParser {
    const std::string& src;
    size_t pos;

    void skipWs() {
        while (pos < src.size() && (src[pos] == ' ' || src[pos] == '\t' ||
               src[pos] == '\r' || src[pos] == '\n')) pos++;
    }
    char peek() { skipWs(); return pos < src.size() ? src[pos] : 0; }
    char next() { skipWs(); return pos < src.size() ? src[pos++] : 0; }

    std::string parseString() {
        next(); // skip "
        std::string r;
        while (pos < src.size() && src[pos] != '"') {
            if (src[pos] == '\\') { pos++; if (pos < src.size()) r += src[pos]; }
            else r += src[pos];
            pos++;
        }
        pos++; // skip "
        return r;
    }

    JsonValue parseValue() {
        JsonValue v;
        char c = peek();
        if (c == '"') {
            v.type = JsonValue::STRING;
            v.sval = parseString();
        } else if (c == '{') {
            v = parseObject();
        } else if (c == 't' || c == 'f') {
            std::string w;
            while (pos < src.size() && src[pos] >= 'a' && src[pos] <= 'z') w += src[pos++];
            v.type = JsonValue::BOOL;
            v.bval = (w == "true");
        } else if (c == 'n') {
            while (pos < src.size() && src[pos] >= 'a' && src[pos] <= 'z') pos++;
            v.type = JsonValue::NONE;
        } else {
            // number
            std::string w;
            while (pos < src.size() && (src[pos] == '-' || src[pos] == '+' ||
                   src[pos] == '.' || src[pos] == 'e' || src[pos] == 'E' ||
                   (src[pos] >= '0' && src[pos] <= '9'))) w += src[pos++];
            v.type = JsonValue::NUMBER;
            v.nval = std::stod(w);
        }
        return v;
    }

    JsonValue parseObject() {
        JsonValue obj;
        obj.type = JsonValue::OBJECT;
        next(); // skip {
        while (peek() != '}' && pos < src.size()) {
            std::string key = parseString();
            next(); // skip :
            obj.obj[key] = parseValue();
            if (peek() == ',') next();
        }
        next(); // skip }
        return obj;
    }

public:
    JsonParser(const std::string& s) : src(s), pos(0) {}
    JsonValue parse() { return parseObject(); }
};

// ─────────────────────────────────────────────────────────────────────
// CSV data structures
// ─────────────────────────────────────────────────────────────────────

struct BandwidthRow { double range; double time_us; double bandwidth; };
struct GflopsRow    { int width; int ncomp; int niter; double time_us; };
struct WarpARow     { int nthread; int nascend; };
struct WarpBRow     { int nthread; double time_us; };
struct CacheRow     { double range; double stride; int niter; double time_us; };

static std::vector<BandwidthRow> loadBandwidthCsv(const std::string& path) {
    std::vector<BandwidthRow> rows;
    std::ifstream f(path);
    if (!f.is_open()) return rows;
    std::string line;
    std::getline(f, line); // header
    while (std::getline(f, line)) {
        if (trim(line).empty()) continue;
        auto t = split(line, ',');
        if (t.size() >= 3) {
            rows.push_back({std::stod(t[0]), std::stod(t[1]), std::stod(t[2])});
        }
    }
    return rows;
}

static std::vector<GflopsRow> loadGflopsCsv(const std::string& path) {
    std::vector<GflopsRow> rows;
    std::ifstream f(path);
    if (!f.is_open()) return rows;
    std::string line;
    std::getline(f, line);
    while (std::getline(f, line)) {
        if (trim(line).empty()) continue;
        auto t = split(line, ',');
        if (t.size() >= 4) {
            rows.push_back({std::stoi(t[0]), std::stoi(t[1]),
                            std::stoi(t[2]), std::stod(t[3])});
        }
    }
    return rows;
}

static std::vector<WarpARow> loadWarpACsv(const std::string& path) {
    std::vector<WarpARow> rows;
    std::ifstream f(path);
    if (!f.is_open()) return rows;
    std::string line;
    std::getline(f, line);
    while (std::getline(f, line)) {
        if (trim(line).empty()) continue;
        auto t = split(line, ',');
        if (t.size() >= 2) {
            rows.push_back({std::stoi(t[0]), std::stoi(t[1])});
        }
    }
    return rows;
}

static std::vector<WarpBRow> loadWarpBCsv(const std::string& path) {
    std::vector<WarpBRow> rows;
    std::ifstream f(path);
    if (!f.is_open()) return rows;
    std::string line;
    std::getline(f, line);
    while (std::getline(f, line)) {
        if (trim(line).empty()) continue;
        auto t = split(line, ',');
        if (t.size() >= 2) {
            rows.push_back({std::stoi(t[0]), std::stod(t[1])});
        }
    }
    return rows;
}

// Load the initial sweep (stride=16) from cache hierarchy P-Chase CSV
static std::vector<CacheRow> loadCacheHierarchyCsv(const std::string& path) {
    std::vector<CacheRow> rows;
    std::ifstream f(path);
    if (!f.is_open()) return rows;
    std::string line;
    std::getline(f, line); // header
    bool inInitialSweep = true;
    double prevRange = -1;
    while (std::getline(f, line)) {
        if (trim(line).empty()) continue;
        auto t = split(line, ',');
        if (t.size() >= 4) {
            double range  = std::stod(t[0]);
            double stride = std::stod(t[1]);
            int    niter  = std::stoi(t[2]);
            double time   = std::stod(t[3]);
            // Only take the initial sweep (increasing range with stride=16)
            if (inInitialSweep) {
                if (stride == 16 && range > prevRange) {
                    rows.push_back({range, stride, niter, time});
                    prevRange = range;
                } else if (range <= prevRange || stride != 16) {
                    // initial sweep is done
                    inInitialSweep = false;
                }
            }
        }
    }
    return rows;
}

// ─────────────────────────────────────────────────────────────────────
// ASCII Graph Rendering
// ─────────────────────────────────────────────────────────────────────

// Unicode block elements for sub-character bar widths
static const char* kBlocks[] = {
    " ", "\xe2\x96\x8f", "\xe2\x96\x8e", "\xe2\x96\x8d",
    "\xe2\x96\x8c", "\xe2\x96\x8b", "\xe2\x96\x8a", "\xe2\x96\x89",
    "\xe2\x96\x88"
};

static void drawHBar(double val, double maxVal, int maxWidth) {
    if (maxVal <= 0) { std::cout << "\n"; return; }
    double frac = (val / maxVal) * maxWidth;
    int full = (int)frac;
    int sub  = (int)((frac - full) * 8);
    if (sub > 8) sub = 8;
    for (int i = 0; i < full; i++) std::cout << kBlocks[8];
    if (sub > 0 && full < maxWidth) std::cout << kBlocks[sub];
}

static void printHeader(const std::string& title) {
    std::string bar(60, '=');  // Use = instead of unicode for Android terminal compat
    std::cout << "\n" << bar << "\n";
    std::cout << "  " << title << "\n";
    std::cout << bar << "\n";
}

static void printSubHeader(const std::string& title) {
    std::string bar(50, '-');
    std::cout << "\n  " << title << "\n  " << bar << "\n";
}

// Bar chart: label | ████████ value
static void printBarChart(const std::string& title,
                          const std::vector<std::pair<std::string, double>>& data,
                          const std::string& unit, int barWidth = 40) {
    if (data.empty()) return;
    printSubHeader(title);

    // Find max value and max label width
    double maxVal = 0;
    size_t maxLabel = 0;
    for (auto& p : data) {
        maxVal = std::max(maxVal, p.second);
        maxLabel = std::max(maxLabel, p.first.size());
    }
    if (maxVal <= 0) return;

    for (auto& p : data) {
        std::cout << "  " << std::setw((int)maxLabel) << std::right << p.first << " |";
        drawHBar(p.second, maxVal, barWidth);
        char buf[32];
        snprintf(buf, sizeof(buf), " %.2f", p.second);
        std::cout << buf << " " << unit << "\n";
    }
}

// Line chart: simple ASCII plot showing value changes over X axis
static void printLineChart(const std::string& title,
                           const std::vector<std::pair<std::string, double>>& data,
                           const std::string& yLabel, int height = 15, int width = 60) {
    if (data.empty()) return;
    printSubHeader(title);

    double minVal = data[0].second, maxVal = data[0].second;
    for (auto& p : data) {
        minVal = std::min(minVal, p.second);
        maxVal = std::max(maxVal, p.second);
    }
    double range = maxVal - minVal;
    if (range < 1e-9) range = 1;

    // Resample data to fit width
    std::vector<int> plotY(width);
    for (int x = 0; x < width; x++) {
        size_t idx = (size_t)((double)x / width * data.size());
        if (idx >= data.size()) idx = data.size() - 1;
        plotY[x] = (int)((data[idx].second - minVal) / range * (height - 1));
    }

    // Draw top-down
    for (int y = height - 1; y >= 0; y--) {
        // Y-axis label
        if (y == height - 1) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%8.1f", maxVal);
            std::cout << buf << " |";
        } else if (y == 0) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%8.1f", minVal);
            std::cout << buf << " |";
        } else if (y == height / 2) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%8.1f", (maxVal + minVal) / 2);
            std::cout << buf << " |";
        } else {
            std::cout << "         |";
        }

        for (int x = 0; x < width; x++) {
            if (plotY[x] == y) std::cout << "*";
            else if (plotY[x] > y) std::cout << " ";
            else std::cout << " ";
        }
        std::cout << "\n";
    }
    // X-axis
    std::cout << "         +";
    for (int x = 0; x < width; x++) std::cout << "-";
    std::cout << "\n";

    // X labels
    if (!data.empty()) {
        std::cout << "          " << data.front().first;
        int pad = width - (int)data.front().first.size() - (int)data.back().first.size();
        if (pad > 0) for (int i = 0; i < pad; i++) std::cout << " ";
        std::cout << data.back().first << "\n";
    }
    std::cout << "          " << yLabel << "\n";
}

// Warp chart: shows nthread vs nascend, highlighting where nascend stops increasing
static void printWarpChart(const std::string& title,
                           const std::vector<WarpARow>& data) {
    if (data.empty()) return;
    printSubHeader(title);

    // Find the warp size: where nascend stops equaling nthread
    int warpSize = 0;
    for (size_t i = 0; i < data.size(); i++) {
        if (data[i].nthread != data[i].nascend) {
            warpSize = data[i].nascend;
            break;
        }
    }
    if (warpSize == 0 && !data.empty()) warpSize = data.back().nascend;

    // Show a compact view
    int start = std::max(0, warpSize - 5);
    int end   = std::min((int)data.size(), warpSize + 5);

    std::cout << "  nthread | nascend | Status\n";
    std::cout << "  --------+---------+-------\n";
    for (int i = start; i < end && i < (int)data.size(); i++) {
        std::cout << "  " << std::setw(7) << data[i].nthread << " | "
                  << std::setw(7) << data[i].nascend << " | ";
        if (data[i].nthread == data[i].nascend) std::cout << "=";
        else std::cout << "<-- WARP BOUNDARY (warp=" << warpSize << ")";
        std::cout << "\n";
    }
}

// Warp Method B: shows time jumps
static void printWarpBChart(const std::string& title,
                            const std::vector<WarpBRow>& data) {
    if (data.empty()) return;
    printSubHeader(title);

    // Find significant time jumps
    std::vector<std::pair<int, double>> jumps; // nthread where jump happens
    for (size_t i = 1; i < data.size(); i++) {
        double diff = data[i].time_us - data[i-1].time_us;
        double pct = (data[i-1].time_us > 0) ? diff / data[i-1].time_us * 100 : 0;
        if (pct > 20) { // significant jump
            jumps.push_back({data[i].nthread, pct});
        }
    }

    // Show as bar chart around jump points
    // Sample evenly
    int step = std::max(1, (int)data.size() / 20);
    std::vector<std::pair<std::string, double>> sampled;
    for (size_t i = 0; i < data.size(); i += step) {
        char label[16];
        snprintf(label, sizeof(label), "%d", data[i].nthread);
        sampled.push_back({label, data[i].time_us});
    }
    // Always include the last
    if (!data.empty()) {
        char label[16];
        snprintf(label, sizeof(label), "%d", data.back().nthread);
        sampled.push_back({label, data.back().time_us});
    }
    printBarChart(title + " (time vs nthread)", sampled, "us", 35);

    if (!jumps.empty()) {
        std::cout << "\n  Detected time jumps (warp boundaries):\n";
        for (auto& j : jumps) {
            std::cout << "    nthread=" << j.first
                      << " (+" << std::fixed << std::setprecision(1)
                      << j.second << "%)\n";
        }
    }
}

// ─────────────────────────────────────────────────────────────────────
// Cache hierarchy analysis: detect cache levels from latency steps
// ─────────────────────────────────────────────────────────────────────

struct CacheLevel {
    std::string name;
    double sizeBytes;
    double latency_us;
};

static std::vector<CacheLevel> detectCacheLevels(const std::vector<CacheRow>& rows) {
    std::vector<CacheLevel> levels;
    if (rows.size() < 5) return levels;

    // Use a simple step-detection: find where latency increases significantly
    double baseLatency = rows[0].time_us;
    double prevLatency = baseLatency;
    int levelNum = 1;

    struct Segment { double startRange; double endRange; double avgLatency; };
    std::vector<Segment> segments;
    Segment cur = {rows[0].range, rows[0].range, rows[0].time_us};
    int count = 1;

    for (size_t i = 1; i < rows.size(); i++) {
        double diff = std::abs(rows[i].time_us - cur.avgLatency / count * count);
        double pctChange = std::abs(rows[i].time_us - prevLatency) / prevLatency;

        if (pctChange > 0.08 && rows[i].time_us > prevLatency * 1.05) {
            // New level detected
            cur.avgLatency /= count;
            cur.endRange = rows[i-1].range;
            segments.push_back(cur);
            cur = {rows[i].range, rows[i].range, rows[i].time_us};
            count = 1;
            prevLatency = rows[i].time_us;
        } else {
            cur.avgLatency += rows[i].time_us;
            count++;
            prevLatency = rows[i].time_us;
        }
    }
    cur.avgLatency /= count;
    cur.endRange = rows.back().range;
    segments.push_back(cur);

    for (size_t i = 0; i < segments.size() && i < 4; i++) {
        char name[32];
        snprintf(name, sizeof(name), "L%d Cache", (int)(i + 1));
        levels.push_back({name, segments[i].endRange, segments[i].avgLatency});
    }

    return levels;
}

// ─────────────────────────────────────────────────────────────────────
// Main analysis and output
// ─────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    std::string dataDir = ".";
    if (argc > 1) dataDir = argv[1];

    // Ensure trailing /
    if (!dataDir.empty() && dataDir.back() != '/') dataDir += '/';

    // ── Load JSON report ──
    std::string jsonPath = dataDir + "ArchProbeReport.json";
    std::ifstream jf(jsonPath);
    JsonValue report;
    if (jf.is_open()) {
        std::stringstream ss;
        ss << jf.rdbuf();
        std::string jsonStr = ss.str();
        JsonParser parser(jsonStr);
        report = parser.parse();
    } else {
        std::cerr << "Warning: Cannot open " << jsonPath << "\n";
    }

    // ── Load CSV files ──
    auto bufBw     = loadBandwidthCsv(dataDir + "BufferBandwidth.csv");
    auto imgBw     = loadBandwidthCsv(dataDir + "ImageBandwidth.csv");
    auto localBw   = loadBandwidthCsv(dataDir + "LocalMemBandwidth.csv");
    auto constBw   = loadBandwidthCsv(dataDir + "ConstMemBandwidth.csv");
    auto gflops    = loadGflopsCsv(dataDir + "Gflops.csv");
    auto warpA     = loadWarpACsv(dataDir + "WarpSizeMethodA.csv");
    auto warpB     = loadWarpBCsv(dataDir + "WarpSizeMethodB.csv");
    auto bufCache  = loadCacheHierarchyCsv(dataDir + "BufferCacheHierarchyPChase.csv");
    auto imgCache  = loadCacheHierarchyCsv(dataDir + "ImageCacheHierarchyPChase.csv");

    // ════════════════════════════════════════════════════════════════
    // Architecture Summary
    // ════════════════════════════════════════════════════════════════
    printHeader("GPU Architecture Summary");

    auto& dev = report.child("Device");
    auto& gfl = report.child("Gflops");
    auto& reg = report.child("RegCount");
    auto& wA  = report.child("WarpSizeMethodA");
    auto& wB  = report.child("WarpSizeMethodB");
    auto& bvw = report.child("BufferVecWidth");
    auto& bcl = report.child("BufferCachelineSize");
    auto& icl = report.child("ImageCachelineSize");
    auto& bbw = report.child("BufferBandwidth");
    auto& ibw = report.child("ImageBandwidth");
    auto& lbw = report.child("LocalMemBandwidth");
    auto& cbw = report.child("ConstMemBandwidth");

    if (dev.isObj()) {
        std::cout << "\n  [Device Info]\n";
        std::cout << "  Shader Cores (SM):         " << (int)dev.num("SmCount") << "\n";
        std::cout << "  Max Threads per SM:         " << (int)dev.num("LogicThreadCount") << "\n";
        std::cout << "  L2 Cache Size:              " << formatBytes(dev.num("CacheSize")) << "\n";
        std::cout << "  Cacheline Size:             " << (int)dev.num("CachelineSize") << " bytes\n";
        std::cout << "  Max Buffer Size:            " << formatBytes(dev.num("MaxBufferSize")) << "\n";
        std::cout << "  Max Constant Mem:           " << formatBytes(dev.num("MaxConstMemSize")) << "\n";
        std::cout << "  Max Local Mem:              " << formatBytes(dev.num("MaxLocalMemSize")) << "\n";
        std::cout << "  Max Image Dim:              " << (int)dev.num("MaxImageWidth")
                  << " x " << (int)dev.num("MaxImageHeight") << "\n";
        double pageSize = dev.num("PageSize_QCOM");
        if (pageSize > 0)
            std::cout << "  QCOM Page Size:             " << (int)pageSize << " bytes\n";
    }

    if (gfl.isObj()) {
        std::cout << "\n  [Compute Performance]\n";
        std::cout << "  FP32 GFLOPS:                " << std::fixed << std::setprecision(1)
                  << gfl.num("FloatGflops") << " (" << gfl.str("FloatArch") << ")\n";
        std::cout << "  FP16 GFLOPS:                " << std::fixed << std::setprecision(1)
                  << gfl.num("HalfGflops") << " (" << gfl.str("HalfArch") << ")\n";
        std::cout << "  FP16/FP32 ratio:            " << std::fixed << std::setprecision(2)
                  << (gfl.num("FloatGflops") > 0 ? gfl.num("HalfGflops") / gfl.num("FloatGflops") : 0)
                  << "x\n";
        std::cout << "  FP32 Vec Components:        " << (int)gfl.num("FloatVecComponentCount") << "\n";
        std::cout << "  FP16 Vec Components:        " << (int)gfl.num("HalfVecComponentCount") << "\n";
    }

    if (reg.isObj()) {
        std::cout << "\n  [Register File]\n";
        std::cout << "  Register Count:             " << (int)reg.num("RegCount") << "\n";
        std::cout << "  Register Type:              " << reg.str("RegType") << "\n";
        std::cout << "  Full-Reg Workgroups:        " << (int)reg.num("FullRegConcurWorkgroupCount") << "\n";
        std::cout << "  Half-Reg Workgroups:        " << (int)reg.num("HalfRegConcurWorkgroupCount") << "\n";
    }

    if (wA.isObj() || wB.isObj()) {
        int wsA = (int)wA.num("WarpThreadCount");
        int wsB = (int)wB.num("WarpThreadCount");
        std::cout << "\n  [Warp/Wave Size]\n";
        std::cout << "  Method A (ascending):       " << wsA << " threads\n";
        std::cout << "  Method B (timing):          " << wsB << " threads\n";
        if (wsA != wsB) {
            std::cout << "  NOTE: Methods disagree.     " << std::min(wsA, wsB)
                      << "-" << std::max(wsA, wsB) << " (sub-warp possible)\n";
        }
    }

    if (bvw.isObj()) {
        std::cout << "\n  [Vector Width]\n";
        std::cout << "  Buffer Vec Size:            " << (int)bvw.num("BufferVecSize") << " components\n";
    }

    if (bcl.isObj()) {
        std::cout << "\n  [Buffer Cacheline]\n";
        std::cout << "  Top-Level Cacheline:        " << (int)bcl.num("BufTopLevelCachelineSize") << " bytes\n";
    }

    if (icl.isObj()) {
        std::cout << "\n  [Image Cacheline]\n";
        std::cout << "  Cacheline Dim:              " << icl.str("ImgCachelineDim") << "\n";
        std::cout << "  Cacheline Size:             " << (int)icl.num("ImgCachelineSize") << " pixels\n";
        std::cout << "  Min-Time Thread Count X:    " << (int)icl.num("ImgMinTimeConcurThreadCountX") << "\n";
        std::cout << "  Min-Time Thread Count Y:    " << (int)icl.num("ImgMinTimeConcurThreadCountY") << "\n";
    }

    // ════════════════════════════════════════════════════════════════
    // Memory Bandwidth Comparison
    // ════════════════════════════════════════════════════════════════
    printHeader("Memory Bandwidth");

    {
        std::vector<std::pair<std::string, double>> bwData;
        if (bbw.isObj()) {
            bwData.push_back({"Buffer Max", bbw.num("MaxBandwidth")});
            bwData.push_back({"Buffer Min", bbw.num("MinBandwidth")});
        }
        if (ibw.isObj()) {
            bwData.push_back({"Image Max", ibw.num("MaxBandwidth")});
            bwData.push_back({"Image Min", ibw.num("MinBandwidth")});
        }
        if (lbw.isObj()) {
            bwData.push_back({"Local Max", lbw.num("MaxBandwidth")});
            bwData.push_back({"Local Min", lbw.num("MinBandwidth")});
        }
        if (cbw.isObj()) {
            bwData.push_back({"Const Max", cbw.num("MaxBandwidth")});
            bwData.push_back({"Const Min", cbw.num("MinBandwidth")});
        }
        printBarChart("Peak & Minimum Bandwidth by Memory Type", bwData, "GB/s");
    }

    // Buffer bandwidth vs size
    if (!bufBw.empty()) {
        std::vector<std::pair<std::string, double>> plot;
        for (auto& r : bufBw) {
            plot.push_back({formatBytes(r.range), r.bandwidth});
        }
        printLineChart("Buffer Bandwidth vs Working Set Size", plot, "GB/s");
    }

    // Image bandwidth vs size
    if (!imgBw.empty()) {
        std::vector<std::pair<std::string, double>> plot;
        for (auto& r : imgBw) {
            plot.push_back({formatBytes(r.range), r.bandwidth});
        }
        printLineChart("Image Bandwidth vs Working Set Size", plot, "GB/s");
    }

    // Local memory bandwidth vs size
    if (!localBw.empty()) {
        std::vector<std::pair<std::string, double>> plot;
        for (auto& r : localBw) {
            plot.push_back({formatBytes(r.range), r.bandwidth});
        }
        printBarChart("Local Memory Bandwidth vs Range", plot, "GB/s");
    }

    // Constant memory bandwidth
    if (!constBw.empty()) {
        std::vector<std::pair<std::string, double>> plot;
        for (auto& r : constBw) {
            plot.push_back({formatBytes(r.range), r.bandwidth});
        }
        printBarChart("Constant Memory Bandwidth vs Range", plot, "GB/s");
    }

    // ════════════════════════════════════════════════════════════════
    // Compute Performance
    // ════════════════════════════════════════════════════════════════
    printHeader("Compute Performance (GFLOPS)");

    if (!gflops.empty()) {
        // Separate FP16 (width=16) and FP32 (width=32), first entry of each ncomp
        std::vector<std::pair<std::string, double>> fp16Data, fp32Data;
        std::map<int, double> fp16Best, fp32Best; // best time per ncomp

        for (auto& r : gflops) {
            auto& m = (r.width == 16) ? fp16Best : fp32Best;
            auto it = m.find(r.ncomp);
            if (it == m.end() || r.time_us < it->second) {
                m[r.ncomp] = r.time_us;
            }
        }

        // Calculate GFLOPS for each: niter * ncomp * 1024 * SM_count * 2 (FMA) / time_us / 1e3
        double smCount = dev.isObj() ? dev.num("SmCount", 1) : 1;
        double threads = dev.isObj() ? dev.num("LogicThreadCount", 1024) : 1024;

        for (auto& r : gflops) {
            // Simple: use the GFLOPS from report, but show the per-component breakdown
            // The actual GFLOPS varies by vectorization
        }

        // Just show the summary from report
        std::vector<std::pair<std::string, double>> computeData;
        if (gfl.isObj()) {
            computeData.push_back({"FP16", gfl.num("HalfGflops")});
            computeData.push_back({"FP32", gfl.num("FloatGflops")});
        }
        printBarChart("Peak Compute Throughput", computeData, "GFLOPS");

        // Show raw timing data
        std::vector<std::pair<std::string, double>> rawData;
        for (auto& r : gflops) {
            char label[32];
            snprintf(label, sizeof(label), "fp%d vec%d", r.width, r.ncomp);
            rawData.push_back({label, r.time_us});
        }
        printBarChart("Kernel Execution Time (lower=faster)", rawData, "us");
    }

    // ════════════════════════════════════════════════════════════════
    // Warp Size Analysis
    // ════════════════════════════════════════════════════════════════
    printHeader("Warp/Wave Size Analysis");

    if (!warpA.empty()) {
        printWarpChart("Method A: Ascending Thread Count", warpA);
    }

    if (!warpB.empty()) {
        printWarpBChart("Method B: Timing-based Detection", warpB);
    }

    // ════════════════════════════════════════════════════════════════
    // Cache Hierarchy
    // ════════════════════════════════════════════════════════════════
    printHeader("Cache Hierarchy Analysis");

    if (!bufCache.empty()) {
        auto levels = detectCacheLevels(bufCache);
        if (!levels.empty()) {
            printSubHeader("Buffer Cache Hierarchy (P-Chase)");
            std::cout << "  Level     | Size           | Avg Latency\n";
            std::cout << "  ----------+----------------+------------\n";
            for (auto& lv : levels) {
                std::cout << "  " << std::setw(9) << std::left << lv.name
                          << " | " << std::setw(14) << formatBytes(lv.sizeBytes)
                          << " | " << std::fixed << std::setprecision(1) << lv.latency_us << " us\n";
            }

            // Also as bar chart
            std::vector<std::pair<std::string, double>> latencyBars;
            for (auto& lv : levels) {
                latencyBars.push_back({lv.name + " (" + formatBytes(lv.sizeBytes) + ")", lv.latency_us});
            }
            printBarChart("Buffer Cache Latency by Level", latencyBars, "us");
        }

        // Show bandwidth curve from the initial P-Chase sweep
        if (bufCache.size() > 10) {
            // Sample to reasonable size
            int step = std::max(1, (int)bufCache.size() / 30);
            std::vector<std::pair<std::string, double>> plot;
            for (size_t i = 0; i < bufCache.size(); i += step) {
                plot.push_back({formatBytes(bufCache[i].range), bufCache[i].time_us});
            }
            printLineChart("Buffer P-Chase Latency vs Working Set", plot, "latency (us)");
        }
    }

    if (!imgCache.empty()) {
        auto levels = detectCacheLevels(imgCache);
        if (!levels.empty()) {
            printSubHeader("Image Cache Hierarchy (P-Chase)");
            std::cout << "  Level     | Size           | Avg Latency\n";
            std::cout << "  ----------+----------------+------------\n";
            for (auto& lv : levels) {
                std::cout << "  " << std::setw(9) << std::left << lv.name
                          << " | " << std::setw(14) << formatBytes(lv.sizeBytes)
                          << " | " << std::fixed << std::setprecision(1) << lv.latency_us << " us\n";
            }

            std::vector<std::pair<std::string, double>> latencyBars;
            for (auto& lv : levels) {
                latencyBars.push_back({lv.name + " (" + formatBytes(lv.sizeBytes) + ")", lv.latency_us});
            }
            printBarChart("Image Cache Latency by Level", latencyBars, "us");
        }
    }

    // ════════════════════════════════════════════════════════════════
    // Architecture Diagram (text-based)
    // ════════════════════════════════════════════════════════════════
    printHeader("Architecture Diagram");

    {
        int smCount   = dev.isObj() ? (int)dev.num("SmCount", 1) : 1;
        int threads   = dev.isObj() ? (int)dev.num("LogicThreadCount", 1024) : 1024;
        int warp      = (wB.isObj()) ? (int)wB.num("WarpThreadCount", 64) : 64;
        int regCnt    = reg.isObj() ? (int)reg.num("RegCount", 0) : 0;
        std::string regType = reg.isObj() ? reg.str("RegType", "Unknown") : "Unknown";
        double cacheSize = dev.isObj() ? dev.num("CacheSize", 0) : 0;
        double localMem  = dev.isObj() ? dev.num("MaxLocalMemSize", 0) : 0;
        double constMem  = dev.isObj() ? dev.num("MaxConstMemSize", 0) : 0;
        double bufBwMax  = bbw.isObj() ? bbw.num("MaxBandwidth", 0) : 0;
        double imgBwMax  = ibw.isObj() ? ibw.num("MaxBandwidth", 0) : 0;
        double localBwMax = lbw.isObj() ? lbw.num("MaxBandwidth", 0) : 0;
        double constBwMax = cbw.isObj() ? cbw.num("MaxBandwidth", 0) : 0;
        double fp32Gflops = gfl.isObj() ? gfl.num("FloatGflops", 0) : 0;
        double fp16Gflops = gfl.isObj() ? gfl.num("HalfGflops", 0) : 0;
        int cacheLine = dev.isObj() ? (int)dev.num("CachelineSize", 64) : 64;

        std::cout << "\n";
        std::cout << "  " << smCount << " Shader Cores  |  " << threads
                  << " ALUs/core  |  warp_size = " << warp << "\n";
        std::cout << "\n";

        // Draw SM blocks
        std::string smLine = "  ";
        for (int i = 0; i < smCount && i < 16; i++) {
            smLine += "+--------+ ";
        }
        if (smCount > 16) smLine += "...";
        std::cout << smLine << "\n";

        smLine = "  ";
        for (int i = 0; i < smCount && i < 16; i++) {
            char buf[16];
            snprintf(buf, sizeof(buf), "| SM %-3d |", i);
            smLine += buf;
            smLine += " ";
        }
        if (smCount > 16) smLine += "...";
        std::cout << smLine << "\n";

        smLine = "  ";
        for (int i = 0; i < smCount && i < 16; i++) {
            smLine += "+--------+ ";
        }
        if (smCount > 16) smLine += "...";
        std::cout << smLine << "\n";

        std::cout << "       |" << std::string(std::min(smCount, 16) * 11 - 2, ' ') << "|\n";

        // Register file
        if (regCnt > 0) {
            std::cout << "  " << regCnt << " x 32-bit " << regType << " registers per thread\n";
        }

        std::cout << "\n";

        // FP performance
        char perfLine[128];
        snprintf(perfLine, sizeof(perfLine),
                 "  Compute: %.1f FP32 GFLOPS  |  %.1f FP16 GFLOPS",
                 fp32Gflops, fp16Gflops);
        std::cout << perfLine << "\n";
        std::cout << "\n";

        // Memory hierarchy
        std::cout << "  Memory Hierarchy:\n";
        std::cout << "  +--------------------------------------------------+\n";
        if (localMem > 0) {
            char buf[128];
            snprintf(buf, sizeof(buf),
                     "  | Local Memory:  %-10s  BW: %.1f GB/s           |",
                     formatBytes(localMem).c_str(), localBwMax);
            std::cout << buf << "\n";
        }
        std::cout << "  +--------------------------------------------------+\n";
        std::cout << "         |                          |\n";

        // L1/Texture cache (from cacheline info)
        {
            int imgCachelineSize = icl.isObj() ? (int)icl.num("ImgCachelineSize", 0) : 0;
            char buf[128];
            snprintf(buf, sizeof(buf),
                     "  | Texture L1     %-4d B CL |  | L1 cache  %-4d B CL  |",
                     imgCachelineSize * 4, cacheLine); // pixels * 4 bytes
            std::cout << "  +--------------------------+  +----------------------+\n";
            std::cout << buf << "\n";
            std::cout << "  +--------------------------+  +----------------------+\n";
        }

        std::cout << "         |                          |\n";

        // L2 cache
        {
            char buf[128];
            snprintf(buf, sizeof(buf),
                     "  +--------------------------------------------------+");
            std::cout << buf << "\n";
            snprintf(buf, sizeof(buf),
                     "  | Unified L2 Cache:  %-10s  CL: %d bytes       |",
                     formatBytes(cacheSize).c_str(), cacheLine);
            std::cout << buf << "\n";
            std::cout << "  +--------------------------------------------------+\n";
        }

        std::cout << "         |\n";

        // Global memory
        {
            char buf[128];
            snprintf(buf, sizeof(buf),
                     "  +--------------------------------------------------+");
            std::cout << buf << "\n";
            snprintf(buf, sizeof(buf),
                     "  | Global Memory     BW: %.1f GB/s (buffer)        |",
                     bufBwMax);
            std::cout << buf << "\n";
            snprintf(buf, sizeof(buf),
                     "  |                   BW: %.1f GB/s (image)         |",
                     imgBwMax);
            std::cout << buf << "\n";
            snprintf(buf, sizeof(buf),
                     "  | Constant Memory:  %-10s  BW: %.1f GB/s     |",
                     formatBytes(constMem).c_str(), constBwMax);
            std::cout << buf << "\n";
            std::cout << "  +--------------------------------------------------+\n";
        }
    }

    // ════════════════════════════════════════════════════════════════
    // Quick Reference Summary
    // ════════════════════════════════════════════════════════════════
    printHeader("Quick Reference");

    std::cout << "\n";
    std::cout << "  For optimal kernel design on this GPU:\n\n";

    if (wB.isObj()) {
        std::cout << "  * Workgroup size: multiples of " << (int)wB.num("WarpThreadCount")
                  << " threads\n";
    }
    if (dev.isObj()) {
        std::cout << "  * Max workgroup size: " << (int)dev.num("LogicThreadCount") << " threads\n";
    }
    if (bvw.isObj()) {
        int vecSize = (int)bvw.num("BufferVecSize");
        if (vecSize > 1) {
            std::cout << "  * Use float" << vecSize << " for vectorized buffer access\n";
        } else {
            std::cout << "  * Scalar buffer access is optimal (vec width = 1)\n";
        }
    }
    if (bcl.isObj()) {
        std::cout << "  * Buffer cacheline: " << (int)bcl.num("BufTopLevelCachelineSize")
                  << " bytes — align buffer offsets to this\n";
    }
    if (reg.isObj()) {
        std::cout << "  * " << (int)reg.num("RegCount") << " registers available ("
                  << reg.str("RegType") << ")\n";
        if (reg.str("RegType") == "Pooled") {
            std::cout << "    -> Using fewer registers allows more concurrent workgroups\n";
        }
    }
    if (gfl.isObj()) {
        double ratio = gfl.num("FloatGflops") > 0 ? gfl.num("HalfGflops") / gfl.num("FloatGflops") : 0;
        if (ratio > 1.5) {
            std::cout << "  * FP16 is " << std::fixed << std::setprecision(1) << ratio
                      << "x faster — prefer half precision when possible\n";
        } else {
            std::cout << "  * FP16/FP32 similar speed — no strong half-precision advantage\n";
        }
    }
    if (bbw.isObj() && ibw.isObj()) {
        double bufMax = bbw.num("MaxBandwidth");
        double imgMax = ibw.num("MaxBandwidth");
        if (imgMax > bufMax * 1.1) {
            std::cout << "  * Image memory is faster than buffer ("
                      << std::fixed << std::setprecision(0) << imgMax << " vs " << bufMax
                      << " GB/s) — prefer images for read-only data\n";
        } else if (bufMax > imgMax * 1.1) {
            std::cout << "  * Buffer memory is faster than image ("
                      << std::fixed << std::setprecision(0) << bufMax << " vs " << imgMax
                      << " GB/s)\n";
        } else {
            std::cout << "  * Buffer and image memory have similar bandwidth\n";
        }
    }

    std::cout << "\n" << std::string(60, '=') << "\n";
    std::cout << "  Analysis complete.\n\n";

    return 0;
}
