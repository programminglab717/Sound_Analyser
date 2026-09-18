#include <sa/analysis/KWeighting.h>

#include <cmath>
#include <numbers>

namespace sa::analysis {

namespace {

/// Bilinear-transformed high shelf, RBJ cookbook form.
///
/// Vb is the gain at the shelf's corner and Vh the gain well above it. The
/// standard's shelf is not a textbook one -- its corner gain is very slightly
/// off sqrt(Vh) -- so the exponent comes from the prototype fit rather than
/// being hard-coded to 0.5.
BiquadCoefficients highShelf(double frequencyHz, double gainDb, double q, double gainExponent,
                             double rateHz) noexcept {
    const double k = std::tan(std::numbers::pi * frequencyHz / rateHz);
    const double vh = std::pow(10.0, gainDb / 20.0);
    const double vb = std::pow(vh, gainExponent);
    const double a0 = 1.0 + k / q + k * k;

    BiquadCoefficients c;
    c.b0 = (vh + vb * k / q + k * k) / a0;
    c.b1 = 2.0 * (k * k - vh) / a0;
    c.b2 = (vh - vb * k / q + k * k) / a0;
    c.a1 = 2.0 * (k * k - 1.0) / a0;
    c.a2 = (1.0 - k / q + k * k) / a0;
    return c;
}

/// Bilinear-transformed second-order high pass. The numerator is exactly
/// (1, -2, 1) at every rate, which is why BS.1770-4 prints it that way.
BiquadCoefficients highPass(double frequencyHz, double q, double rateHz) noexcept {
    const double k = std::tan(std::numbers::pi * frequencyHz / rateHz);
    const double a0 = 1.0 + k / q + k * k;

    BiquadCoefficients c;
    c.b0 = 1.0;
    c.b1 = -2.0;
    c.b2 = 1.0;
    c.a1 = 2.0 * (k * k - 1.0) / a0;
    c.a2 = (1.0 - k / q + k * k) / a0;
    return c;
}

} // namespace

Result<KWeightingCoefficients> kWeightingFor(SampleRate rate) {
    if (!rate.isValid()) {
        return Error{ErrorCode::InvalidArgument, "sample rate is not a rate we can process"};
    }
    if (rate.hz() < kMinimumSampleRateHz) {
        return Error{ErrorCode::OutOfRange,
                     "K-weighting is not defined below 8 kHz -- the shelf corner reaches Nyquist"};
    }

    const KWeightingPrototype& p = kKWeightingPrototype;
    KWeightingCoefficients coefficients;
    coefficients.shelf = highShelf(p.shelfFrequencyHz, p.shelfGainDb, p.shelfQ,
                                   p.shelfBandwidthGainExponent, rate.hz());
    coefficients.highPass = highPass(p.highPassFrequencyHz, p.highPassQ, rate.hz());
    return coefficients;
}

} // namespace sa::analysis
