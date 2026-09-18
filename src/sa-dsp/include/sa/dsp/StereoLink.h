#pragma once

#include <sa/core/Result.h>
#include <sa/core/Types.h>

#include <algorithm>
#include <cmath>
#include <utility>

/// Stereo linking for dynamics processors.
///
/// Two instances of a processor across a stereo pair are two independent gains,
/// and independent gains move the image. A transient hard-panned to the left
/// pulls the left channel down and leaves the right where it was, so everything
/// else in the mix -- the centred vocal, the reverb, the bass -- swings right
/// for as long as the release takes. The effect is strongest on exactly the
/// material a limiter is there for.
///
/// The fix is one sidechain for both channels. This wrapper runs each
/// processor's detector, combines the two levels, and hands the combined level
/// back to both gain computers, so at full link the gain applied to the two
/// channels is identical sample for sample and the image cannot move at all.
///
/// The blend between "independent" and "linked" happens on the linear levels
/// rather than in decibels. A silent channel is at the decibel silence floor,
/// and blending towards -200 dB would make a link amount of 0.5 mean something
/// wildly different for a near-silent channel than for a loud one.
namespace sa::dsp {

/// How the two detectors become one sidechain.
enum class StereoLinkMode {
    /// The louder channel decides for both. The safe choice for a limiter: the
    /// combined level is never below either channel's own, so the ceiling holds
    /// on both whatever the balance between them.
    Maximum,
    /// The average of the two detector levels -- the "summed" sidechain, and
    /// the gentler choice for a compressor, which is being asked to follow the
    /// programme rather than to catch every peak.
    ///
    /// The average of the *levels*, not of the waveforms. Summing the waveforms
    /// would cancel out-of-phase material and let an out-of-phase transient
    /// through untouched.
    Average,
};

struct StereoLinkSettings {
    /// 0 is two independent processors. 1 is one gain applied to both channels.
    /// In between, each channel's detector is pulled part of the way towards
    /// the combined level, which is what a "stereo link 60%" control means.
    double amount = 1.0;
    StereoLinkMode mode = StereoLinkMode::Maximum;
};

[[nodiscard]] inline Status validateStereoLink(const StereoLinkSettings& link) {
    if (!std::isfinite(link.amount) || link.amount < 0.0 || link.amount > 1.0) {
        return Error{ErrorCode::InvalidArgument, "link amount must be between 0 and 1"};
    }
    if (link.mode != StereoLinkMode::Maximum && link.mode != StereoLinkMode::Average) {
        return Error{ErrorCode::InvalidArgument, "unknown stereo link mode"};
    }
    return {};
}

/// A stereo pair of any of the dynamics processors, sharing one sidechain.
///
/// Templated rather than written four times because the four processors differ
/// only in what their detector does and what their gain computer says, and both
/// of those are already behind Processor::detect() and
/// Processor::applyDetected(). Nothing here knows which processor it is
/// driving.
template <typename Processor>
class StereoLink {
public:
    using Settings = typename Processor::Settings;

    [[nodiscard]] static Result<StereoLink> create(SampleRate rate, const Settings& settings,
                                                   const StereoLinkSettings& link = {}) {
        if (const Status status = validateStereoLink(link); !status) {
            return status.error();
        }
        Result<Processor> left = Processor::create(rate, settings);
        if (!left) {
            return left.error();
        }
        Result<Processor> right = Processor::create(rate, settings);
        if (!right) {
            return right.error();
        }
        return StereoLink{std::move(left).value(), std::move(right).value(), link};
    }

    [[nodiscard]] const StereoLinkSettings& link() const noexcept { return link_; }

    /// Changes the link without touching either processor's state, so the
    /// amount can be moved under a signal.
    [[nodiscard]] Status setLink(const StereoLinkSettings& link) {
        if (const Status status = validateStereoLink(link); !status) {
            return status;
        }
        link_ = link;
        return {};
    }

    [[nodiscard]] Status setSettings(const Settings& settings) {
        if (const Status status = left_.setSettings(settings); !status) {
            return status;
        }
        return right_.setSettings(settings);
    }

    /// The two processors, for the gain reduction each is reporting and for the
    /// settings they were built with. Both are configured identically -- a
    /// linked pair with different thresholds would move the image by design.
    [[nodiscard]] const Processor& left() const noexcept { return left_; }

    [[nodiscard]] const Processor& right() const noexcept { return right_; }

    void reset() noexcept {
        left_.reset();
        right_.reset();
    }

    void processSample(float leftInput, float rightInput, float& leftOutput,
                       float& rightOutput) noexcept {
        // Both detectors run before either gain computer does. That ordering is
        // the whole point: a gain cannot be computed until the level of the
        // other channel is known.
        const double leftLevel = left_.detect(leftInput);
        const double rightLevel = right_.detect(rightInput);
        const double combined = link_.mode == StereoLinkMode::Maximum
                                    ? std::max(leftLevel, rightLevel)
                                    : 0.5 * (leftLevel + rightLevel);

        leftOutput =
            left_.applyDetected(leftInput, leftLevel + link_.amount * (combined - leftLevel));
        rightOutput =
            right_.applyDetected(rightInput, rightLevel + link_.amount * (combined - rightLevel));
    }

    /// `input` and `output` pointers may alias channel for channel. A
    /// non-positive count is a no-op.
    void process(const float* leftInput, const float* rightInput, float* leftOutput,
                 float* rightOutput, SampleCount count) noexcept {
        for (SampleCount i = 0; i < count; ++i) {
            processSample(leftInput[i], rightInput[i], leftOutput[i], rightOutput[i]);
        }
    }

    void processInPlace(float* left, float* right, SampleCount count) noexcept {
        process(left, right, left, right, count);
    }

private:
    StereoLink(Processor&& left, Processor&& right, const StereoLinkSettings& link) noexcept
        : left_(std::move(left)), right_(std::move(right)), link_(link) {}

    Processor left_;
    Processor right_;
    StereoLinkSettings link_;
};

} // namespace sa::dsp
