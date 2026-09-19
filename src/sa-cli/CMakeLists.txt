add_executable(auscult-cli src/main.cpp)
target_link_libraries(auscult-cli PRIVATE sa-analysis sa-dsp sa-engine sa-io sa-spectral)
target_link_libraries(auscult-cli PRIVATE sa-warnings)
