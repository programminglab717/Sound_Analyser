add_executable(sa-cli src/main.cpp)
target_link_libraries(sa-cli PRIVATE sa-analysis sa-dsp sa-engine sa-io sa-spectral)
target_link_libraries(sa-cli PRIVATE sa-warnings)
