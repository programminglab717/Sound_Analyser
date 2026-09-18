#pragma once

#include <sa/core/Result.h>
#include <sa/core/Types.h>

#include <cmath>
#include <complex>

namespace sa::dsp {

/// Second-order filter shape -- the RBJ cookbook set.
///
/// These eight cover everything a channel strip needs. Anything higher order is
/// a BiquadCascade of them rather than a bigger difference equation: cascaded
/// second-order sections are the numerically well-conditioned way to realise a
/// high-order filter, and a direct 8th-order form is not usable at audio
/// sample rates regardless of the arithmetic it is written in.
enum class FilterType {
    LowPass,
    HighPass,
    BandPass,
    Notch,
    Peaking,
    LowShelf,
    HighShelf,
    AllPass,
};

/// Q of a maximally flat (Butterworth) second-order section.
inline constexpr double kButterworthQ = 0.7071067811865476; // 1 / sqrt(2)

/// A filter shape and its parameters. `gainDb` applies to Peaking, LowShelf and
/// HighShelf only and is ignored by the other shapes.
struct FilterSpec {
    FilterType type = FilterType::Peaking;
    double frequency = 1000.0;
    double q = kButterworthQ;
    double gainDb = 0.0;
};

/// Normalised second-order transfer function coefficients:
///
///     H(z) = (b0 + b1 z^-1 + b2 z^-2) / (1 + a1 z^-1 + a2 z^-2)
///
/// a0 is divided out at design time, so the audio path never performs a
/// division. The struct is trivially copyable and holds no state, which is what
/// lets a newly designed set be handed to the audio thread by value -- no
/// ownership, no lifetime, nothing to delete on the wrong thread.
struct BiquadCoefficients {
    double b0 = 1.0;
    double b1 = 0.0;
    double b2 = 0.0;
    double a1 = 0.0;
    double a2 = 0.0;

    /// Unity gain at every frequency. The identity element for a cascade.
    [[nodiscard]] static constexpr BiquadCoefficients passthrough() noexcept { return {}; }

    /// Cookbook designers.
    ///
    /// Each rejects rather than clamps: `frequency` must lie strictly inside
    /// (0, Nyquist) because the formulas degenerate at both ends, and `q` must
    /// be positive. Clamping silently would hide a units mistake -- a cutoff
    /// passed in radians instead of hertz -- behind a filter that still runs.
    [[nodiscard]] static Result<BiquadCoefficients> lowPass(SampleRate rate, double frequency,
                                                            double q = kButterworthQ);
    [[nodiscard]] static Result<BiquadCoefficients> highPass(SampleRate rate, double frequency,
                                                             double q = kButterworthQ);
    /// Unity gain at the centre frequency, -3 dB at the edges of the band that
    /// `q` describes. The cookbook's other band-pass has a peak gain of Q
    /// instead, which makes the band level depend on its width -- surprising
    /// under a Q control, so it is not what this returns.
    [[nodiscard]] static Result<BiquadCoefficients> bandPass(SampleRate rate, double frequency,
                                                             double q = kButterworthQ);
    [[nodiscard]] static Result<BiquadCoefficients> notch(SampleRate rate, double frequency,
                                                          double q = kButterworthQ);
    [[nodiscard]] static Result<BiquadCoefficients> allPass(SampleRate rate, double frequency,
                                                            double q = kButterworthQ);
    [[nodiscard]] static Result<BiquadCoefficients> peaking(SampleRate rate, double frequency,
                                                            double q, double gainDb);
    /// Shelves parameterised by Q, not by the cookbook's shelf slope S. The two
    /// coincide at Q = 1/sqrt(2) (S = 1, the steepest slope that does not
    /// overshoot); higher Q deliberately overshoots at the shelf corner.
    [[nodiscard]] static Result<BiquadCoefficients> lowShelf(SampleRate rate, double frequency,
                                                             double q, double gainDb);
    [[nodiscard]] static Result<BiquadCoefficients> highShelf(SampleRate rate, double frequency,
                                                              double q, double gainDb);

    /// Dispatches on spec.type.
    [[nodiscard]] static Result<BiquadCoefficients> design(SampleRate rate, const FilterSpec& spec);

    /// H(e^(j 2 pi f)) with f in cycles per sample: DC is 0, Nyquist is 0.5.
    /// Evaluates the rational function directly -- it shares no code with the
    /// difference equation, so agreement between the two is evidence rather
    /// than tautology.
    [[nodiscard]] std::complex<double> response(double normalisedFrequency) const noexcept;

    [[nodiscard]] double magnitudeDb(SampleRate rate, double frequency) const noexcept;

    /// True when both poles lie strictly inside the unit circle. This is the
    /// Jury stability test for a monic quadratic, not a sampled measurement, so
    /// it is exact.
    [[nodiscard]] bool isStable() const noexcept {
        return std::abs(a2) < 1.0 && std::abs(a1) < 1.0 + a2;
    }
};

/// State below which a decaying filter is snapped to zero.
///
/// A filter fed silence after a loud passage decays towards zero forever. Long
/// before it gets there the state reaches the denormal range, where every
/// multiply costs a hundred-odd cycles on x86 -- a dropout that appears only
/// after the music stops, which is the hardest possible thing to reproduce from
/// a bug report. 1e-25 is about -500 dBFS, so cutting the tail short there is
/// inaudible by an enormous margin, and it also keeps the float32 output clear
/// of denormals for whatever is downstream.
inline constexpr double kDenormalFloor = 1e-25;

/// One second-order section, direct form II transposed.
///
/// DF-II transposed rather than DF-I or DF-II, because its two state variables
/// hold partial sums of the *output* rather than of the input. In DF-I the
/// recursive part accumulates in the input's range and the rounding error of
/// each state update is amplified by the pole gain; in plain DF-II the shared
/// intermediate w[n] can be far larger than either the input or the output. A
/// 20 Hz high-pass at 96 kHz sits at a normalised frequency of 2e-4, which puts
/// both poles within 0.0013 of z = 1, and that geometry is precisely where the
/// difference shows up as low-frequency noise or an overflowing intermediate.
///
/// The state is double even though the signal is float32. That is not belt and
/// braces: at a pole radius of 0.9987 the coefficient quantisation of float32
/// alone shifts the cutoff by a noticeable fraction and the state rounding
/// raises the noise floor into the audible range. Doubles cost one register
/// and remove the entire class of problem.
class Biquad {
public:
    Biquad() = default;

    explicit Biquad(const BiquadCoefficients& coefficients) noexcept
        : coefficients_(coefficients) {}

    /// Replaces the coefficients and leaves the state alone, so a parameter
    /// change does not restart the filter. It is not click-free -- that needs
    /// interpolation, which belongs a level up where the smoothing rate and the
    /// block boundaries are known.
    void setCoefficients(const BiquadCoefficients& coefficients) noexcept {
        coefficients_ = coefficients;
    }

    [[nodiscard]] const BiquadCoefficients& coefficients() const noexcept { return coefficients_; }

    /// Clears the delay line, so the output afterwards depends only on future
    /// input. Call when seeking, or before reusing the filter on new material.
    void reset() noexcept {
        s1_ = 0.0;
        s2_ = 0.0;
    }

    [[nodiscard]] float processSample(float input) noexcept {
        const double x = static_cast<double>(input);
        const double y = coefficients_.b0 * x + s1_;
        s1_ = coefficients_.b1 * x - coefficients_.a1 * y + s2_;
        s2_ = coefficients_.b2 * x - coefficients_.a2 * y;

        if (std::abs(s1_) < kDenormalFloor && std::abs(s2_) < kDenormalFloor) {
            s1_ = 0.0;
            s2_ = 0.0;
        }
        return static_cast<float>(y);
    }

    /// `input` and `output` may alias. A non-positive count is a no-op.
    void process(const float* input, float* output, SampleCount count) noexcept {
        for (SampleCount i = 0; i < count; ++i) {
            output[i] = processSample(input[i]);
        }
    }

    void processInPlace(float* samples, SampleCount count) noexcept {
        process(samples, samples, count);
    }

private:
    BiquadCoefficients coefficients_;
    double s1_ = 0.0;
    double s2_ = 0.0;
};

} // namespace sa::dsp
