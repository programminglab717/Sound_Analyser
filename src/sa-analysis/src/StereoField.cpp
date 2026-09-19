#include <sa/analysis/StereoField.h>

#include <algorithm>
#include <cmath>

namespace sa::analysis {

namespace {

/// Ratio of two energies, in decibels, with both degenerate ends named.
///
/// The two ends are not the same thing and cannot share an answer. No energy
/// on top is the floor -- no side means mono, no right means hard left. No
/// energy underneath is the opposite end, and returning the floor for it (as
/// this did until a test for two opposed channels caught it) reports the
/// widest possible signal as the narrowest.
[[nodiscard]] double ratioDb(double numerator, double denominator) noexcept {
    constexpr double kCeiling = -kDecibelFloor;
    if (!(numerator > 0.0)) {
        return kDecibelFloor;
    }
    if (!(denominator > 0.0)) {
        return kCeiling;
    }
    const double decibels = 10.0 * std::log10(numerator / denominator);
    return std::clamp(decibels, kDecibelFloor, kCeiling);
}

} // namespace

Result<StereoFieldMeter> StereoFieldMeter::create(int channelCount) {
    if (channelCount != 2) {
        return Error{ErrorCode::InvalidArgument,
                     "the stereo field is only defined for two channels"};
    }
    return StereoFieldMeter{};
}

void StereoFieldMeter::process(ConstAudioBufferView block) noexcept {
    if (block.channelCount() != 2 || block.frames() <= 0) {
        return;
    }
    const float* left = block.channel(0);
    const float* right = block.channel(1);

    for (SampleCount i = 0; i < block.frames(); ++i) {
        const double l = left[i];
        const double r = right[i];
        sumLeftSquared_ += l * l;
        sumRightSquared_ += r * r;
        sumProduct_ += l * r;

        // Mid and side at the conventional halved scale, so that a programme
        // with both channels carrying the same thing has mid equal to that
        // thing rather than twice it.
        const double mid = 0.5 * (l + r);
        const double side = 0.5 * (l - r);
        sumMidSquared_ += mid * mid;
        sumSideSquared_ += side * side;
    }
    frames_ += block.frames();
}

void StereoFieldMeter::reset() noexcept {
    sumLeftSquared_ = 0.0;
    sumRightSquared_ = 0.0;
    sumProduct_ = 0.0;
    sumMidSquared_ = 0.0;
    sumSideSquared_ = 0.0;
    frames_ = 0;
}

StereoField StereoFieldMeter::field() const noexcept {
    StereoField out;
    out.frames = frames_;
    if (frames_ <= 0) {
        return out;
    }

    // Silence has no stereo field, and saying "perfectly correlated" about it
    // would be a lie of the kind a meter is there to prevent. The threshold is
    // an energy sum, so it scales with length: this is a hundredth of a decibel
    // above nothing for any realistic programme, and catches a genuinely empty
    // buffer.
    const double energy = sumLeftSquared_ + sumRightSquared_;
    if (!(energy > 0.0)) {
        return out;
    }
    out.valid = true;

    const double denominator = std::sqrt(sumLeftSquared_ * sumRightSquared_);
    if (denominator > 0.0) {
        // Clamped because the arithmetic can land a hair outside the range on
        // perfectly correlated material, and a correlation meter reading 1.0000001
        // is a bug report waiting to happen.
        const double raw = sumProduct_ / denominator;
        out.correlation = raw > 1.0 ? 1.0 : (raw < -1.0 ? -1.0 : raw);
    } else {
        // One channel is silent. The two are then neither correlated nor
        // opposed; there is nothing to correlate with.
        out.correlation = 0.0;
    }

    out.widthDb = ratioDb(sumSideSquared_, sumMidSquared_);
    out.balanceDb = ratioDb(sumRightSquared_, sumLeftSquared_);

    // The mono sum against the stereo programme's average channel power. Two
    // identical channels give 0 dB; two uncorrelated channels of equal power
    // give -3, which is arithmetic and not damage; below that is cancellation.
    // The denominator cannot be zero here -- `energy` was checked above.
    out.monoLossDb = ratioDb(sumMidSquared_, 0.5 * energy);
    return out;
}

Result<StereoField> StereoFieldMeter::measure(ConstAudioBufferView audio) {
    auto meter = create(audio.channelCount());
    if (!meter) {
        return meter.error();
    }
    meter.value().process(audio);
    return meter.value().field();
}

} // namespace sa::analysis
