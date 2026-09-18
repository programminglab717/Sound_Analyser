# Benchmarks are excluded from the default build: they are run deliberately, and
# only a release configuration produces meaningful numbers.
add_executable(sa-spectrogram-spike EXCLUDE_FROM_ALL SpectrogramSpike.cpp)
target_link_libraries(sa-spectrogram-spike PRIVATE sa-spectral sa-io sa-warnings)
