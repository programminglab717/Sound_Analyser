#pragma once

#include <sa/core/Result.h>
#include <sa/core/Types.h>

namespace sa::analysis {

/// Biquad coefficients in transposed direct form II, a0 normalised to 1.
struct BiquadCoefficients {
    double b0 = 1.0;
    double b1 = 0.0;
    double b2 = 0.0;
    double a1 = 0.0;
    double a2 = 0.0;
};

/// One channel's worth of biquad state.
///
/// Coefficients live outside the state so that a multichannel meter holds one
/// copy of them and N small state objects, which is also what keeps the inner
/// loop's working set in cache for high channel counts.
class BiquadState {
public:
    void reset() noexcept {
        s1_ = 0.0;
        s2_ = 0.0;
    }

    [[nodiscard]] double process(const BiquadCoefficients& c, double x) noexcept {
        const double y = c.b0 * x + s1_;
        s1_ = c.b1 * x - c.a1 * y + s2_;
        s2_ = c.b2 * x - c.a2 * y;
        return y;
    }

private:
    double s1_ = 0.0;
    double s2_ = 0.0;
};

/// The two cascaded stages of the BS.1770 K-weighting filter.
struct KWeightingCoefficients {
    /// Stage 1: a ~+4 dB high shelf, modelling the acoustic effect of a head in
    /// the sound field.
    BiquadCoefficients shelf;
    /// Stage 2: the RLB high pass, which removes the low frequencies that
    /// contribute to measured energy far more than to perceived loudness.
    BiquadCoefficients highPass;
};

/// BS.1770-4 Table 1 -- the stage 1 coefficients as the standard prints them.
///
/// The standard tabulates 48 kHz and no other rate, so these exist to *check*
/// the rate-adaptive derivation below, not to be used directly. A test asserts
/// that kWeightingFor(48 kHz) reproduces them.
inline constexpr BiquadCoefficients kReferenceShelfAt48kHz{
    1.53512485958697, -2.69169618940638, 1.19839281085285, -1.69065929318241, 0.73248077421585};

/// BS.1770-4 Table 2 -- the stage 2 coefficients as the standard prints them.
inline constexpr BiquadCoefficients kReferenceHighPassAt48kHz{1.0, -2.0, 1.0, -1.99004745483398,
                                                              0.99007225036621};

/// Analogue prototype parameters for the two stages.
///
/// These are what makes rate adaptation possible. BS.1770-4 publishes only the
/// 48 kHz digital coefficients, and reusing them at 44.1 or 96 kHz moves both
/// corner frequencies by the rate ratio -- at 44.1 kHz the RLB corner lands at
/// 35 Hz instead of 38 Hz, which is a real and rate-dependent error in the
/// reading. The fix is to recover the continuous-time filter the table came
/// from and re-run the bilinear transform at the target rate: solving the
/// 48 kHz table for (f0, Q, gain) yields the values below, and substituting
/// them back reproduces the published table to within 1e-15 (asserted by test),
/// which is how we know the recovered prototype is the right one.
///
/// This is the same derivation used by libebur128 and by Brecht De Man's
/// transcription of the K-weighting filter; the constants are not in BS.1770-4
/// itself.
struct KWeightingPrototype {
    double shelfFrequencyHz = 1681.974450955533;
    double shelfGainDb = 3.999843853973347;
    double shelfQ = 0.7071752369554196;
    /// Exponent relating the shelf's mid-band gain to its high-band gain. Not a
    /// round number because it, too, is a fit to the published table.
    double shelfBandwidthGainExponent = 0.4996667741545416;
    double highPassFrequencyHz = 38.13547087602444;
    double highPassQ = 0.5003270373238773;
};

inline constexpr KWeightingPrototype kKWeightingPrototype{};

/// Lowest rate the derivation is defined at.
///
/// The bilinear transform's tan(pi * f0 / rate) term is singular once the shelf
/// corner reaches Nyquist, at 2 * 1681.97 = 3364 Hz. 8 kHz keeps a wide margin
/// and is already below any rate a loudness meter has business seeing.
inline constexpr double kMinimumSampleRateHz = 8000.0;

/// K-weighting coefficients for `rate`, derived from the analogue prototype.
///
/// Fails for a rate outside [kMinimumSampleRateHz, SampleRate::isValid()].
[[nodiscard]] Result<KWeightingCoefficients> kWeightingFor(SampleRate rate);

} // namespace sa::analysis
