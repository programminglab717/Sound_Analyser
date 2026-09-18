#pragma once

#include <sa/core/Result.h>
#include <sa/core/Types.h>
#include <sa/dsp/Biquad.h>

#include <array>
#include <complex>

namespace sa::dsp {

/// A chain of second-order sections run in series.
///
/// This is how every filter steeper than 12 dB/octave and every multi-band EQ
/// is realised. Factoring a high-order transfer function into second-order
/// sections keeps each pole pair's coefficients away from the cancellation that
/// makes a direct high-order form unusable -- an 8th-order Butterworth written
/// as one difference equation is not stable in double precision at audio
/// cutoffs, while the same filter as four biquads is untroubled.
///
/// Storage is a fixed inline array, so the whole object is allocation-free,
/// copyable, and safe to build on a worker thread and hand to the audio thread
/// by value.
class BiquadCascade {
public:
    /// Sixteen sections is a 32nd-order filter or a 16-band EQ -- past the
    /// point where a cascade is still the right structure. Bounding it here is
    /// what keeps the object allocation-free.
    static constexpr int kMaxSections = 16;

    BiquadCascade() = default;

    /// A Butterworth low- or high-pass of `order`, realised as order/2 sections
    /// at the same cutoff but different Qs.
    ///
    /// The section Qs are *not* all 1/sqrt(2). Butterworth poles are evenly
    /// spaced on a semicircle, so section k takes Q = 1/(2 cos((2k+1) pi / 2N));
    /// cascading identical 1/sqrt(2) sections instead -- the usual mistake --
    /// gives a response already 6 dB down at the nominal cutoff of a 4th-order
    /// filter. Odd orders need a first-order section, which the cookbook set
    /// does not produce, so they are rejected rather than silently rounded up.
    [[nodiscard]] static Result<BiquadCascade> butterworth(FilterType type, int order,
                                                           SampleRate rate, double frequency);

    [[nodiscard]] int sectionCount() const noexcept { return sectionCount_; }

    [[nodiscard]] bool isEmpty() const noexcept { return sectionCount_ == 0; }

    /// Appends a section with cleared state. Fails when the cascade is full
    /// rather than growing, because growing would allocate.
    [[nodiscard]] Status append(const BiquadCoefficients& coefficients);

    /// Replaces one section's coefficients, leaving its state alone -- a
    /// parameter change, not a restart.
    [[nodiscard]] Status setSection(int index, const BiquadCoefficients& coefficients);

    /// Coefficients of a section, or nullptr if `index` is out of range.
    [[nodiscard]] const BiquadCoefficients* sectionCoefficients(int index) const noexcept;

    /// Drops every section. The cascade then passes audio through untouched.
    void clear() noexcept { sectionCount_ = 0; }

    /// Clears the state of every section, keeping the coefficients.
    void reset() noexcept;

    [[nodiscard]] float processSample(float input) noexcept {
        float sample = input;
        for (int i = 0; i < sectionCount_; ++i) {
            sample = sections_[static_cast<std::size_t>(i)].processSample(sample);
        }
        return sample;
    }

    /// `input` and `output` may alias. A non-positive count is a no-op.
    void process(const float* input, float* output, SampleCount count) noexcept;

    void processInPlace(float* samples, SampleCount count) noexcept {
        process(samples, samples, count);
    }

    /// Product of the sections' responses -- the response of the whole chain.
    [[nodiscard]] std::complex<double> response(double normalisedFrequency) const noexcept;

    [[nodiscard]] double magnitudeDb(SampleRate rate, double frequency) const noexcept;

    /// True when every section is stable. One bad section poisons the chain.
    [[nodiscard]] bool isStable() const noexcept;

private:
    std::array<Biquad, static_cast<std::size_t>(kMaxSections)> sections_{};
    int sectionCount_ = 0;
};

} // namespace sa::dsp
