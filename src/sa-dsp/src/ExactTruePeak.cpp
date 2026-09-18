#include <sa/dsp/ExactTruePeak.h>
#include <sa/dsp/Fft.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

namespace sa::dsp {

namespace {

/// Block length. Long enough that the edges are a small fraction of it, short
/// enough that the oversampled transform stays a sensible size: at 16x this is
/// a 65536-point inverse per block.
constexpr int kBlock = 4096;

/// Fraction of each reconstructed block discarded at either end.
///
/// A block transformed and inverted is treated as periodic, so its two ends
/// meet at a discontinuity and ring. Blocks overlap by half, so every sample is
/// covered by some block's interior and nothing is missed by throwing these
/// away.
constexpr int kEdgeDivisor = 16;

} // namespace

Result<double> exactTruePeak(const float* samples, SampleCount count, int factor) {
    if (samples == nullptr) {
        return Error{ErrorCode::InvalidArgument, "no samples"};
    }
    if (count <= 0) {
        return 0.0;
    }
    if (factor < 2 || factor > 64 || !RealFft::isSupportedSize(kBlock * factor)) {
        return Error{ErrorCode::InvalidArgument, "factor must be a power of two from 2 to 64"};
    }

    // Shorter than a block: the plain sample peak is the honest answer, because
    // there is not enough signal to reconstruct between.
    if (count < kBlock) {
        double peak = 0.0;
        for (SampleCount i = 0; i < count; ++i) {
            peak = std::max(peak, std::abs(static_cast<double>(samples[i])));
        }
        return peak;
    }

    const RealFft forward{kBlock};
    const RealFft inverse{kBlock * factor};

    std::vector<std::complex<float>> spectrum(static_cast<std::size_t>(forward.binCount()));
    std::vector<std::complex<float>> padded(static_cast<std::size_t>(inverse.binCount()));
    std::vector<float> fine(static_cast<std::size_t>(kBlock) * static_cast<std::size_t>(factor));
    std::vector<float> block(static_cast<std::size_t>(kBlock));

    const auto margin = static_cast<SampleCount>(fine.size()) / kEdgeDivisor;
    double peak = 0.0;

    // Blocks start half a block *before* the signal and run half a block past
    // its end, reading zeros outside it. That is not a fudge: a file really is
    // silent either side, so the zero-padded reconstruction is the correct one,
    // and it puts every real sample inside some block's interior.
    //
    // The interior is what matters. A block is transformed as though periodic,
    // so its two ends meet at a discontinuity and ring; an earlier version kept
    // the outer edge of the first and last blocks to "avoid losing signal
    // there" and read 4.17 dB high at 0.47 of the sample rate as a result.
    // Every edge is discarded now, and the overlap means nothing is lost.
    const SampleCount step = kBlock / 2;
    for (SampleIndex start = -step; start < count; start += step) {
        for (SampleCount i = 0; i < kBlock; ++i) {
            const SampleIndex index = start + i;
            block[static_cast<std::size_t>(i)] =
                (index >= 0 && index < count) ? samples[index] : 0.0f;
        }
        forward.forward(block.data(), spectrum.data());

        // Zero-padding a spectrum is interpolation in the other domain: the
        // inverse of the longer transform is the same band-limited signal
        // sampled `factor` times more finely.
        std::fill(padded.begin(), padded.end(), std::complex<float>{});
        std::copy(spectrum.begin(), spectrum.end(), padded.begin());
        inverse.inverse(padded.data(), fine.data());

        // The inverse normalises for its own length, so undo that.
        const auto scale = static_cast<double>(factor);
        for (SampleCount i = margin; i < static_cast<SampleCount>(fine.size()) - margin; ++i) {
            peak = std::max(
                peak, std::abs(static_cast<double>(fine[static_cast<std::size_t>(i)]) * scale));
        }
    }

    // A true peak is never below the sample peak, so the raw samples are always
    // valid candidates. Including them costs one pass and makes the result
    // impossible to under-report, whatever the blocking does.
    for (SampleCount i = 0; i < count; ++i) {
        peak = std::max(peak, std::abs(static_cast<double>(samples[i])));
    }
    return peak;
}

Result<double> exactTruePeakDbtp(const float* samples, SampleCount count, int factor) {
    auto peak = exactTruePeak(samples, count, factor);
    if (!peak) {
        return peak.error();
    }
    constexpr double kFloor = -200.0;
    return peak.value() > 0.0 ? std::max(kFloor, 20.0 * std::log10(peak.value())) : kFloor;
}

} // namespace sa::dsp
