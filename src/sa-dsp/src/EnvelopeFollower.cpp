#include <sa/dsp/EnvelopeFollower.h>

#include <cmath>

namespace sa::dsp {

namespace {

[[nodiscard]] Status checkTimes(double attackSeconds, double releaseSeconds) {
    if (!std::isfinite(attackSeconds) || attackSeconds < 0.0) {
        return Error{ErrorCode::InvalidArgument, "attack must be finite and not negative"};
    }
    if (!std::isfinite(releaseSeconds) || releaseSeconds < 0.0) {
        return Error{ErrorCode::InvalidArgument, "release must be finite and not negative"};
    }
    return {};
}

} // namespace

double smoothingCoefficient(double seconds, SampleRate rate) noexcept {
    if (!rate.isValid() || !(seconds > 0.0)) {
        return 0.0;
    }
    const double samples = seconds * rate.hz();
    // exp(-1/N) rather than the 1 - 1/N approximation: the two diverge badly
    // for the sub-millisecond attacks a limiter uses, where N is single digits.
    return std::exp(-1.0 / samples);
}

EnvelopeFollower::EnvelopeFollower(SampleRate rate, const Settings& settings) noexcept
    : rate_(rate), settings_(settings) {
    smoother_.setTimes(settings.attackSeconds, settings.releaseSeconds, rate);
}

Result<EnvelopeFollower> EnvelopeFollower::create(SampleRate rate, const Settings& settings) {
    if (!rate.isValid()) {
        return Error{ErrorCode::InvalidArgument, "sample rate is not a usable audio rate"};
    }
    if (const Status status = checkTimes(settings.attackSeconds, settings.releaseSeconds);
        !status) {
        return status.error();
    }
    return EnvelopeFollower{rate, settings};
}

Status EnvelopeFollower::setSettings(const Settings& settings) {
    if (const Status status = checkTimes(settings.attackSeconds, settings.releaseSeconds);
        !status) {
        return status;
    }
    settings_ = settings;
    smoother_.setTimes(settings.attackSeconds, settings.releaseSeconds, rate_);
    return {};
}

} // namespace sa::dsp
