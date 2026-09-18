#pragma once

#include <sa/core/Result.h>
#include <sa/core/Types.h>
#include <sa/dsp/Biquad.h>
#include <sa/dsp/BiquadCascade.h>

#include <array>

namespace sa::dsp {

/// One band of a parametric EQ: a filter shape plus a bypass flag.
///
/// A disabled band keeps its slot and its parameters and contributes unity
/// gain, so toggling a band cannot disturb the others' state or renumber
/// anything the UI is holding an index to.
struct EqBand {
    FilterSpec filter;
    bool enabled = true;
};

/// Multi-band parametric equaliser.
///
/// A BiquadCascade with one section per band, plus the band bookkeeping and the
/// response curve the UI draws over the analyser. Bands are addressed by index;
/// an index stays valid until a band before it is removed.
///
/// Adding, removing and editing bands is control-thread work and is
/// allocation-free, so a parameter change can be applied wherever it arrives.
/// process() is the audio-thread entry point.
class ParametricEq {
public:
    static constexpr int kMaxBands = BiquadCascade::kMaxSections;

    [[nodiscard]] static Result<ParametricEq> create(SampleRate rate);

    [[nodiscard]] SampleRate sampleRate() const noexcept { return rate_; }

    [[nodiscard]] int bandCount() const noexcept { return bandCount_; }

    /// The band at `index`, or nullptr if there is none.
    [[nodiscard]] const EqBand* band(int index) const noexcept;

    /// Appends a band and returns its index. Fails if the band's parameters are
    /// not realisable at this sample rate, or if the EQ is full.
    [[nodiscard]] Result<int> addBand(const EqBand& band);

    /// Replaces a band's parameters, leaving its filter state alone so the
    /// change does not restart the band.
    [[nodiscard]] Status setBand(int index, const EqBand& band);

    /// Removes a band, shifting the later ones down.
    ///
    /// This clears the filter state of every band: a section's state belongs to
    /// the transfer function that produced it, and after a shift the states no
    /// longer line up with the coefficients. Suppressing the resulting
    /// discontinuity is a crossfade at the node level, not something a filter
    /// can do for itself.
    [[nodiscard]] Status removeBand(int index);

    void clearBands() noexcept;

    /// Clears filter state without touching the band list.
    void reset() noexcept { cascade_.reset(); }

    /// Re-designs every band for a new rate and clears the state. The band
    /// parameters are in hertz, so they survive the change unaltered; a band
    /// whose frequency is above the new Nyquist makes this fail, leaving the
    /// EQ untouched.
    [[nodiscard]] Status setSampleRate(SampleRate rate);

    [[nodiscard]] float processSample(float input) noexcept { return cascade_.processSample(input); }

    /// `input` and `output` may alias. A non-positive count is a no-op.
    void process(const float* input, float* output, SampleCount count) noexcept {
        cascade_.process(input, output, count);
    }

    void processInPlace(float* samples, SampleCount count) noexcept {
        cascade_.processInPlace(samples, count);
    }

    /// Combined response of every enabled band at `frequency`. This is the
    /// curve drawn over the spectrum analyser, and it is computed from the
    /// coefficients actually loaded into the filters -- so what is drawn is
    /// what is heard, rather than a second model of it that can drift.
    [[nodiscard]] double magnitudeDbAt(double frequency) const noexcept {
        return cascade_.magnitudeDb(rate_, frequency);
    }

    [[nodiscard]] const BiquadCascade& cascade() const noexcept { return cascade_; }

private:
    explicit ParametricEq(SampleRate rate) noexcept : rate_(rate) {}

    [[nodiscard]] static Result<BiquadCoefficients> designBand(SampleRate rate, const EqBand& band);

    SampleRate rate_;
    std::array<EqBand, static_cast<std::size_t>(kMaxBands)> bands_{};
    int bandCount_ = 0;
    BiquadCascade cascade_;
};

} // namespace sa::dsp
