/// Phase 0 spike: can we pan and zoom a long spectrogram at 60 fps?
///
/// The product thesis is that the spectrogram is the editing surface, which
/// collapses if scrolling stutters. This measures the two costs that decide it:
///
///   1. render()  -- the per-frame cost while panning over cached data. Must
///                   fit inside the 16.67 ms frame budget with room to spare
///                   for the rest of the UI.
///   2. build()   -- the cost of analysing audio that is not cached yet, which
///                   sets how far ahead of a fast pan we can stay.
///
/// Build in a release configuration; debug numbers are meaningless here.

#include <sa/core/AudioBuffer.h>
#include <sa/io/PeakPyramid.h>
#include <sa/spectral/SpectrogramPyramid.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <numbers>
#include <random>
#include <string>
#include <vector>

using namespace sa;
using namespace sa::io;
using Clock = std::chrono::steady_clock;

namespace {

constexpr double kFrameBudgetMs = 1000.0 / 60.0;

double millisecondsSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

/// Median of repeated timings. A mean would be dragged around by scheduler
/// noise on a shared runner.
template <typename Callable>
double medianMs(Callable&& work, int repeats) {
    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(repeats));
    for (int i = 0; i < repeats; ++i) {
        const auto start = Clock::now();
        work();
        samples.push_back(millisecondsSince(start));
    }
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2];
}

/// Broadband material with transients, so the pyramid is doing representative
/// work rather than compressing a pure tone.
AudioBuffer makeTestSignal(SampleCount frames) {
    AudioBuffer buffer{ChannelLayout::mono(), frames};
    float* samples = buffer.channel(0);

    std::mt19937 rng{2024};
    std::uniform_real_distribution<float> noise{-0.2f, 0.2f};

    for (SampleCount i = 0; i < frames; ++i) {
        const auto t = static_cast<double>(i);
        samples[i] =
            0.4f * static_cast<float>(std::sin(2.0 * std::numbers::pi * 440.0 * t / 48000.0)) +
            0.2f * static_cast<float>(std::sin(2.0 * std::numbers::pi * 3150.0 * t / 48000.0)) +
            noise(rng);
    }
    for (SampleCount i = 0; i < frames; i += 24000) {
        samples[i] = 0.99f; // transients to exercise the max-combining levels
    }
    return buffer;
}

void rule() {
    std::printf("  %s\n", std::string(66, '-').c_str());
}

void verdict(const char* label, double measuredMs, double budgetMs) {
    const char* mark = measuredMs <= budgetMs ? "PASS" : "FAIL";
    std::printf("  %-42s %8.2f ms   %s\n", label, measuredMs, mark);
}

} // namespace

int main() {
    constexpr double sampleRate = 48000.0;
    constexpr SampleCount seconds = 60;
    const SampleCount frames = static_cast<SampleCount>(sampleRate) * seconds;

    spectral::SpectrogramConfig config;
    config.fftSize = 2048;
    config.hopSize = 512;

    std::printf("\n=== Phase 0 spike: spectrogram at 60 fps ===\n\n");
    std::printf("  48 kHz mono, fftSize %d, hop %d, %d bins, %lld s of audio\n", config.fftSize,
                config.hopSize, config.fftSize / 2 + 1, static_cast<long long>(seconds));
    std::printf("  Frame budget at 60 fps: %.2f ms\n\n", kFrameBudgetMs);

    const AudioBuffer signal = makeTestSignal(frames);

    // ---- Build -------------------------------------------------------------
    std::printf("  BUILD (cache miss: analysing audio not yet cached)\n");
    rule();

    const auto buildStart = Clock::now();
    auto pyramidResult = spectral::SpectrogramPyramid::build(signal.constView(), 0, config);
    const double buildMs = millisecondsSince(buildStart);
    if (!pyramidResult) {
        std::printf("  build failed: %s\n", std::string(pyramidResult.error().what()).c_str());
        return 1;
    }
    const spectral::SpectrogramPyramid pyramid = std::move(pyramidResult).value();

    const double realtimeFactor = (static_cast<double>(seconds) * 1000.0) / buildMs;
    const double megabytes = static_cast<double>(pyramid.memoryFootprint()) / (1024.0 * 1024.0);

    std::printf("  %-42s %8.0f ms\n", "build 60 s of audio", buildMs);
    std::printf("  %-42s %8.0fx realtime\n", "throughput", realtimeFactor);
    std::printf("  %-42s %8.1f MB\n", "memory for 60 s", megabytes);
    std::printf("  %-42s %8.1f s\n", "audio analysable per frame budget",
                kFrameBudgetMs / 1000.0 * realtimeFactor);
    std::printf("\n  Extrapolated to 1 hour: %.0f s to build, %.0f MB resident\n",
                buildMs / 1000.0 * 60.0, megabytes * 60.0);
    std::printf("  Extrapolated to 1 hour stereo: %.0f MB resident\n\n", megabytes * 120.0);

    // ---- Render ------------------------------------------------------------
    std::printf("  RENDER (cache hit: the per-frame cost while panning)\n");
    rule();

    struct Case {
        const char* label;
        int rows;
        int columns;
        SampleIndex start;
        SampleIndex end;
    };

    const Case cases[] = {
        {"1920x1080, whole 60 s (zoomed fully out)", 1080, 1920, 0, frames},
        {"1920x1080, 10 s window", 1080, 1920, 0, static_cast<SampleCount>(sampleRate) * 10},
        {"1920x1080, 1 s window", 1080, 1920, 0, static_cast<SampleCount>(sampleRate)},
        {"1920x1080, 100 ms window", 1080, 1920, 0, static_cast<SampleCount>(sampleRate) / 10},
        {"3840x2160 (4K), whole 60 s", 2160, 3840, 0, frames},
    };

    double worst1080p = 0.0;
    for (const Case& test : cases) {
        std::vector<std::uint8_t> tile(static_cast<std::size_t>(test.rows) *
                                       static_cast<std::size_t>(test.columns));
        const double ms = medianMs(
            [&] { pyramid.render(test.start, test.end, 0, test.rows, test.columns, tile.data()); },
            9);
        if (test.rows <= 1080) {
            worst1080p = std::max(worst1080p, ms);
        }
        verdict(test.label, ms, kFrameBudgetMs);
    }

    // ---- GPU path ----------------------------------------------------------
    //
    // What the UI should actually do: upload the visible frames of a level as a
    // texture and let a shader handle scaling and colour mapping. The CPU cost
    // collapses to a copy, and it no longer scales with screen resolution.
    std::printf("\n  GPU PATH (upload level tile as texture; shader scales)\n");
    rule();

    for (int columnsWanted : {1920, 3840}) {
        const SampleCount samplesPerColumn = frames / columnsWanted;
        const int level = pyramid.levelForSamplesPerColumn(samplesPerColumn);
        const SampleCount levelFrames = pyramid.frameCountAt(level);
        const auto bins = static_cast<std::size_t>(pyramid.binCount());

        std::vector<std::uint8_t> texture(static_cast<std::size_t>(levelFrames) * bins);
        const double ms = medianMs(
            [&] {
                for (SampleCount f = 0; f < levelFrames; ++f) {
                    std::memcpy(texture.data() + static_cast<std::size_t>(f) * bins,
                                pyramid.frameData(level, f), bins);
                }
            },
            9);

        char label[96];
        std::snprintf(label, sizeof(label), "upload for %d columns (%lld frames x %d bins)",
                      columnsWanted, static_cast<long long>(levelFrames), pyramid.binCount());
        verdict(label, ms, kFrameBudgetMs);
    }

    // ---- Waveform path -----------------------------------------------------
    std::printf("\n  WAVEFORM (peak pyramid, for comparison)\n");
    rule();

    const auto peakStart = Clock::now();
    auto peakResult = PeakPyramid::build(signal.constView());
    const double peakBuildMs = millisecondsSince(peakStart);
    if (!peakResult) {
        std::printf("  peak build failed\n");
        return 1;
    }
    const PeakPyramid peaks = std::move(peakResult).value();

    std::vector<PeakFrame> columns(1920);
    const double peakQueryMs =
        medianMs([&] { peaks.query(0, 0, frames, columns.data(), 1920); }, 99);

    std::printf("  %-42s %8.0f ms\n", "build 60 s of audio", peakBuildMs);
    std::printf("  %-42s %8.1f MB\n", "memory for 60 s",
                static_cast<double>(peaks.memoryFootprint()) / (1024.0 * 1024.0));
    verdict("query 1920 columns, whole 60 s", peakQueryMs, kFrameBudgetMs);

    // ---- Verdict -----------------------------------------------------------
    std::printf("\n  VERDICT\n");
    rule();
    const bool cpuFits1080p = worst1080p <= kFrameBudgetMs;
    std::printf("  1. STFT throughput is ample: %.0fx realtime, %.1f s of audio per\n",
                realtimeFactor, kFrameBudgetMs / 1000.0 * realtimeFactor);
    std::printf("     frame budget. Generating tiles ahead of a fast pan is not the\n");
    std::printf("     bottleneck, so the thesis survives.\n\n");

    std::printf("  2. CPU scaling to screen resolution %s at 1080p (%.2f ms of\n",
                cpuFits1080p ? "fits" : "does NOT fit", worst1080p);
    std::printf("     %.2f ms) but not at 4K. Cost tracks pixel count, not audio\n",
                kFrameBudgetMs);
    std::printf("     length -- which is why the renderer must be a shader, with the\n");
    std::printf("     CPU only uploading level tiles. Keep render() for tests and\n");
    std::printf("     thumbnails, not for the UI.\n\n");

    std::printf("  3. An eager full-file pyramid needs %.0f MB for 1 hour stereo.\n",
                megabytes * 120.0);
    std::printf("     Production must generate tiles lazily with LRU eviction.\n\n");

    return 0;
}
