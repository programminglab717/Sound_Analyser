#pragma once

#include <sa/core/Result.h>

#include <cstddef>
#include <string_view>

namespace sa::analysis {

/// Delivery specifications we check against.
///
/// Netflix's -27 LKFS dialogue-gated target is deliberately absent: dialogue
/// gating is a different measurement, not a different number, and listing it
/// here would imply a check this module cannot make.
enum class LoudnessPlatform {
    Spotify,
    AppleMusic,
    YouTube,
    AmazonMusic,
    Tidal,
    Podcast,
    EbuR128,
    AtscA85,
};

/// A delivery target: where the loudness should land and how high the peaks may
/// go.
struct ComplianceTarget {
    LoudnessPlatform platform = LoudnessPlatform::EbuR128;
    std::string_view name;
    double integratedLufs = -23.0;
    double truePeakCeilingDbtp = -1.0;
    /// How far from the target still counts as on target.
    ///
    /// EBU R128's +/-0.5 LU and ATSC A/85's +/-2 dB come from those standards.
    /// The streaming services publish a target but no tolerance -- they simply
    /// normalise on playback -- so 1.0 LU there is our own display convention,
    /// not something they specify.
    double toleranceLu = 1.0;
};

[[nodiscard]] ComplianceTarget targetFor(LoudnessPlatform platform) noexcept;

/// Every target, in a stable order suitable for a preset list.
[[nodiscard]] const ComplianceTarget* allTargets() noexcept;

[[nodiscard]] std::size_t targetCount() noexcept;

[[nodiscard]] std::string_view toString(LoudnessPlatform platform) noexcept;

/// The verdict for one measurement against one target.
struct ComplianceResult {
    ComplianceTarget target;
    double measuredLufs = 0.0;
    double measuredTruePeakDbtp = 0.0;
    /// Measured minus target. Negative means quieter than asked for.
    double loudnessDeviationLu = 0.0;
    /// Ceiling minus measured. Negative means over the ceiling.
    double truePeakMarginDb = 0.0;
    bool loudnessOnTarget = false;
    bool truePeakWithinCeiling = false;

    [[nodiscard]] bool passed() const noexcept {
        return loudnessOnTarget && truePeakWithinCeiling;
    }

    /// Gain that puts the loudness exactly on target. May push the peaks over
    /// the ceiling -- that is what conformGainDb is for.
    double gainToTargetDb = 0.0;
    /// Gain that moves toward the target without breaching the ceiling. Equal
    /// to gainToTargetDb whenever there is headroom for it; short of it when
    /// the material would have to be limited rather than merely turned up.
    double conformGainDb = 0.0;
};

/// Check a measurement against a target. Pure arithmetic; no failure mode.
[[nodiscard]] ComplianceResult check(const ComplianceTarget& target, double integratedLufs,
                                     double truePeakDbtp) noexcept;

[[nodiscard]] ComplianceResult check(LoudnessPlatform platform, double integratedLufs,
                                     double truePeakDbtp) noexcept;

} // namespace sa::analysis
