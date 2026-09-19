add_executable(auscultate-cli src/main.cpp)
target_link_libraries(auscultate-cli PRIVATE sa-analysis sa-dsp sa-engine sa-io sa-spectral)
target_link_libraries(auscultate-cli PRIVATE sa-warnings)

sa_add_version_resource(auscultate-cli
    DESCRIPTION "Auscultate headless driver"
    FILENAME    "auscultate-cli.exe")
