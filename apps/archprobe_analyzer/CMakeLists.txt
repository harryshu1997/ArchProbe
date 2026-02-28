set(APP_NAME ArchProbeAnalyzer)

add_executable(${APP_NAME} "main.cpp")
target_compile_features(${APP_NAME} PRIVATE cxx_std_14)
