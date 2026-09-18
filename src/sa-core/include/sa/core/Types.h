#pragma once

#include <cstddef>
#include <cstdint>

/// Sound Analyser core types.
///
/// Everything here is header-only, allocation-free and safe to use from the
/// audio thread. See docs/03-architecture.md §3.
namespace sa {

/// Index of a single sample frame within a stream. Signed so that relative
/// offsets and "not found" sentinels are expressible without surprises.
using SampleIndex = std::int64_t;

/// A count of sample frames. Same representation as SampleIndex by design --
/// the distinction is documentary.
using SampleCount = std::int64_t;

/// Hard ceiling on channels in a single buffer. Bounds every inline array in
/// the core types so that nothing here needs to allocate.
inline constexpr int kMaxChannels = 64;

/// Alignment for sample storage, in bytes. 32 = AVX2; also satisfies SSE and
/// NEON. Raising this to 64 for AVX-512 would be ABI-compatible.
inline constexpr std::size_t kSampleAlignment = 32;

/// A sample rate in hertz.
///
/// Wrapped rather than a bare double so that a rate cannot be silently passed
/// where a duration was expected -- historically a rich source of bugs.
class SampleRate {
public:
    constexpr SampleRate() noexcept = default;

    constexpr explicit SampleRate(double hz) noexcept : hz_(hz) {}

    [[nodiscard]] constexpr double hz() const noexcept { return hz_; }

    /// True for rates we are willing to process. The upper bound covers 768 kHz
    /// (DSD-derived material); the lower bound rejects zero and negatives.
    [[nodiscard]] constexpr bool isValid() const noexcept { return hz_ > 0.0 && hz_ <= 768000.0; }

    [[nodiscard]] friend constexpr bool operator==(SampleRate a, SampleRate b) noexcept {
        return a.hz_ == b.hz_;
    }

private:
    double hz_ = 0.0;
};

inline constexpr SampleRate kSampleRate44100{44100.0};
inline constexpr SampleRate kSampleRate48000{48000.0};
inline constexpr SampleRate kSampleRate96000{96000.0};

/// Convert a frame count to seconds. Returns 0 for an invalid rate rather than
/// producing an infinity that would propagate silently.
[[nodiscard]] constexpr double samplesToSeconds(SampleCount frames, SampleRate rate) noexcept {
    return rate.isValid() ? static_cast<double>(frames) / rate.hz() : 0.0;
}

/// Convert seconds to a frame count, rounding to nearest.
[[nodiscard]] constexpr SampleCount secondsToSamples(double seconds, SampleRate rate) noexcept {
    if (!rate.isValid()) {
        return 0;
    }
    const double frames = seconds * rate.hz();
    return static_cast<SampleCount>(frames < 0.0 ? frames - 0.5 : frames + 0.5);
}

} // namespace sa
