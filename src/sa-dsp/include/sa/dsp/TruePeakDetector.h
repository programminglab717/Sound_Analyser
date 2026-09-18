#pragma once

#include <sa/core/Result.h>
#include <sa/core/Types.h>

#include <vector>

namespace sa::dsp {

/// Inter-sample peak detector: the level a reconstructed waveform reaches
/// *between* the samples, which is what a converter or a lossy decoder sees and
/// what a sample-peak meter cannot know about.
///
/// One channel, streaming, allocation-free once constructed. sa-analysis has
/// the metering counterpart, which reports dBTP over a whole programme; this
/// one exists because a limiter needs the same number sample by sample, on the
/// audio thread, and sa-dsp cannot depend on sa-analysis.
///
/// **This is not a BS.1770-4 measurement.** BS.1770-4 Annex 2 specifies a
/// particular 48-tap polyphase FIR in its Table 3. That table has never been
/// transcribed into this project -- writing it down from memory would be worse
/// than not using it at all -- so what runs here is a Kaiser-windowed sinc of
/// kTapsPerPhase taps per phase, chosen and measured on its own terms. No
/// conformance with the standard is claimed anywhere, and a reading from this
/// class must not be quoted as a dBTP measurement to a specification.
///
/// **What it does under-read, and by how much.** Two separate errors, and both
/// are under-reads:
///
///   * **The grid.** Oversampling by L samples the reconstruction on a grid L
///     times finer than the input, and a peak between two grid points is missed
///     by however much the waveform curves in between. No reading can beat
///     cos(pi f / L) of the truth. Measured worst for a steady tone: 0.42 dB at
///     4x and 0.09 dB at 8x, both at 0.4 of the sample rate, against geometric
///     limits of 0.44 and 0.11 (see TruePeakTests).
///   * **The filter.** The interpolator has to turn over somewhere below
///     Nyquist, and content above that point reconstructs low. Measured
///     worst-phase magnitude: -0.03 dB at 0.45 of the sample rate, -1.6 dB at
///     0.47, -6 dB at 0.5. By 16x it is the filter and no longer the grid that
///     decides -- 0.02 dB, at 0.48 -- which is why going past 8x buys so
///     little. Material dense right up to Nyquist, which is a hard-limited
///     square wave rather than a mix, is therefore under-read; no practical
///     interpolator does better, and BS.1770-4's own 12-tap filter does rather
///     worse.
///
/// Which is why a limiter here asks for 8x while a meter is content with the
/// standard's 4x: a meter reading a little low is cosmetic, a limiter reading a
/// little low is an overshoot past a ceiling it promised.
class TruePeakDetector {
public:
    /// Taps per polyphase branch. Odd, so that the prototype's length is odd
    /// too and its centre falls on a whole input sample: phase 0 is then an
    /// exact unit impulse and every other phase sits at an exact p/L offset. An
    /// even-length prototype shifts the whole grid by half an output sample and
    /// quietly misses peaks.
    ///
    /// 33 rather than something cheaper because a short interpolator droops
    /// where the music is. Measured worst-phase magnitude at 0.45 of the sample
    /// rate -- 19.8 kHz at 44.1 kHz, where a loud master still has energy -- is
    /// -3.2 dB for 17 taps and -0.03 dB for these 33. A detector that reads
    /// 3 dB low there is not a true-peak detector at all, and the error is
    /// invisible to a steady-tone test because the raw sample covers for it.
    static constexpr int kTapsPerPhase = 33;

    /// BS.1770-4 sets 4x as the floor for rates from 48 kHz up, and that is the
    /// floor a meter should use. See the class note on why a limiter asks for
    /// more.
    static constexpr int kDefaultOversampling = 4;
    static constexpr int kMinimumOversampling = 2;
    static constexpr int kMaximumOversampling = 16;

    [[nodiscard]] static Result<TruePeakDetector> create(int oversampling = kDefaultOversampling);

    [[nodiscard]] int oversampling() const noexcept { return oversampling_; }

    /// Samples between feeding a sample and being told about the peak around
    /// it. The filter is centred, so it cannot describe a sample until it has
    /// seen half a filter past it.
    [[nodiscard]] SampleCount latencySamples() const noexcept { return (kTapsPerPhase - 1) / 2; }

    void reset() noexcept;

    /// Feeds one sample and returns the largest magnitude the reconstructed
    /// waveform reaches over the sample latencySamples() back -- including that
    /// sample itself, so the answer is never below the sample peak.
    [[nodiscard]] double process(float input) noexcept;

private:
    explicit TruePeakDetector(int oversampling);

    int oversampling_ = 0;
    /// Polyphase coefficients, phase-major: phase p tap k at p * kTapsPerPhase + k.
    std::vector<double> phases_;
    /// The last kTapsPerPhase samples, stored twice back to back so that the
    /// inner loop walks contiguous memory and needs no modulo.
    std::vector<double> delay_;
    int writeIndex_ = 0;
};

} // namespace sa::dsp
