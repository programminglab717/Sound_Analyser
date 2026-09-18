#pragma once

#include <sa/core/Result.h>
#include <sa/core/Types.h>

#include <cmath>

namespace sa::dsp {

/// One-pole coefficient for a time constant, in samples of `rate`.
///
/// The convention is the physicist's: after `seconds` the smoother has covered
/// 1 - 1/e = 63.2% of a step. Compressor front panels often mean the 10%-to-90%
/// time instead, which is 2.2 time constants -- so leaving this unstated makes
/// the number in the code and the number on the panel differ by more than a
/// factor of two. It is stated here, and the tests measure the 63.2% point.
///
/// A non-positive time gives a coefficient of zero, which is instantaneous
/// rather than frozen.
[[nodiscard]] double smoothingCoefficient(double seconds, SampleRate rate) noexcept;

/// State below which a smoother is snapped to its target, for the same reason
/// Biquad has one: a decaying one-pole spends forever in the denormal range.
inline constexpr double kSmootherFloor = 1e-25;

/// One-pole smoother with independent rise and fall rates.
///
/// Deliberately not tied to what it is smoothing. Pointed at |x| it is a level
/// detector; pointed at a gain-reduction curve in decibels it is a compressor's
/// attack/release. Both are the same filter, and writing it once means the
/// 63.2% convention above holds identically in both places.
class AttackReleaseSmoother {
public:
    /// An unconfigured smoother is instantaneous in both directions -- a wire,
    /// which is the harmless default. Frozen would be the dangerous one.
    AttackReleaseSmoother() = default;

    void setTimes(double attackSeconds, double releaseSeconds, SampleRate rate) noexcept {
        attack_ = smoothingCoefficient(attackSeconds, rate);
        release_ = smoothingCoefficient(releaseSeconds, rate);
    }

    void reset(double value = 0.0) noexcept { state_ = value; }

    [[nodiscard]] double value() const noexcept { return state_; }

    /// Moving towards a higher target uses the attack rate, a lower one the
    /// release. Callers choose which direction means "attack" by choosing what
    /// they feed in: a compressor smooths gain *reduction*, so reduction rising
    /// is the attack; a gate smooths gain, so the gate opening is the attack.
    [[nodiscard]] double process(double target) noexcept {
        const double coefficient = (target > state_) ? attack_ : release_;
        state_ = target + coefficient * (state_ - target);
        if (std::abs(state_) < kSmootherFloor) {
            state_ = 0.0;
        }
        return state_;
    }

private:
    double attack_ = 0.0;
    double release_ = 0.0;
    double state_ = 0.0;
};

/// Amplitude envelope of a signal.
///
/// The detector choice is a real one, not a preference: peak follows the
/// waveform and catches the transient that would clip, RMS follows the energy
/// and tracks how loud something sounds. Peak is right for a limiter, RMS for a
/// levelling compressor.
/// Detector and settings live at namespace scope, not nested in
/// EnvelopeFollower: a nested type's default member initialisers are not
/// complete until the enclosing class is, so `const Settings& settings = {}`
/// below would not compile. The aliases inside the class keep
/// `EnvelopeFollower::Settings` working at every call site.
enum class EnvelopeDetector {
    Peak,
    Rms,
};

struct EnvelopeSettings {
    double attackSeconds = 0.005;
    double releaseSeconds = 0.050;
    EnvelopeDetector detector = EnvelopeDetector::Peak;
};

class EnvelopeFollower {
public:
    using Detector = EnvelopeDetector;
    using Settings = EnvelopeSettings;

    [[nodiscard]] static Result<EnvelopeFollower> create(SampleRate rate,
                                                         const Settings& settings = {});

    [[nodiscard]] const Settings& settings() const noexcept { return settings_; }

    /// Changes the time constants and detector without clearing the envelope,
    /// so a parameter change does not restart the detector.
    [[nodiscard]] Status setSettings(const Settings& settings);

    void reset() noexcept { smoother_.reset(0.0); }

    /// Returns the envelope as a linear amplitude.
    ///
    /// In RMS mode the smoothing runs on the square, so the time constants
    /// describe the power envelope -- the conventional definition, and the one
    /// that makes an RMS detector's release sound the same as a peak
    /// detector's. The square root is taken only on the way out.
    [[nodiscard]] double process(float input) noexcept {
        const double x = static_cast<double>(input);
        const double target = (settings_.detector == Detector::Rms) ? x * x : std::abs(x);
        const double smoothed = smoother_.process(target);
        return (settings_.detector == Detector::Rms) ? std::sqrt(smoothed) : smoothed;
    }

    [[nodiscard]] double value() const noexcept {
        const double smoothed = smoother_.value();
        return (settings_.detector == Detector::Rms) ? std::sqrt(smoothed) : smoothed;
    }

private:
    EnvelopeFollower(SampleRate rate, const Settings& settings) noexcept;

    SampleRate rate_;
    Settings settings_;
    AttackReleaseSmoother smoother_;
};

} // namespace sa::dsp
