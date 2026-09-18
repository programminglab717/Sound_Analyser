#include <sa/analysis/ComplianceTarget.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace sa::analysis {

namespace {

/// Targets as the platforms publish them. Ceilings are -1 dBTP except where a
/// platform asks for more room ahead of its own lossy encode.
constexpr std::array<ComplianceTarget, 8> kTargets{{
    {LoudnessPlatform::Spotify, "Spotify", -14.0, -1.0, 1.0},
    {LoudnessPlatform::AppleMusic, "Apple Music", -16.0, -1.0, 1.0},
    {LoudnessPlatform::YouTube, "YouTube", -14.0, -1.0, 1.0},
    {LoudnessPlatform::AmazonMusic, "Amazon Music", -14.0, -2.0, 1.0},
    {LoudnessPlatform::Tidal, "Tidal", -14.0, -1.0, 1.0},
    {LoudnessPlatform::Podcast, "Podcast", -16.0, -1.0, 1.0},
    {LoudnessPlatform::EbuR128, "EBU R128", -23.0, -1.0, 0.5},
    {LoudnessPlatform::AtscA85, "ATSC A/85", -24.0, -2.0, 2.0},
}};

} // namespace

std::string_view toString(LoudnessPlatform platform) noexcept {
    return targetFor(platform).name;
}

ComplianceTarget targetFor(LoudnessPlatform platform) noexcept {
    for (const ComplianceTarget& target : kTargets) {
        if (target.platform == platform) {
            return target;
        }
    }
    return ComplianceTarget{};
}

const ComplianceTarget* allTargets() noexcept {
    return kTargets.data();
}

std::size_t targetCount() noexcept {
    return kTargets.size();
}

ComplianceResult check(const ComplianceTarget& target, double integratedLufs,
                       double truePeakDbtp) noexcept {
    ComplianceResult result;
    result.target = target;
    result.measuredLufs = integratedLufs;
    result.measuredTruePeakDbtp = truePeakDbtp;
    result.loudnessDeviationLu = integratedLufs - target.integratedLufs;
    result.truePeakMarginDb = target.truePeakCeilingDbtp - truePeakDbtp;
    result.loudnessOnTarget = std::abs(result.loudnessDeviationLu) <= target.toleranceLu;
    result.truePeakWithinCeiling = truePeakDbtp <= target.truePeakCeilingDbtp;
    result.gainToTargetDb = -result.loudnessDeviationLu;
    // Turning down is always safe; turning up is capped by the ceiling, because
    // a conform that clips is not a conform.
    result.conformGainDb = std::min(result.gainToTargetDb, result.truePeakMarginDb);
    return result;
}

ComplianceResult check(LoudnessPlatform platform, double integratedLufs,
                       double truePeakDbtp) noexcept {
    return check(targetFor(platform), integratedLufs, truePeakDbtp);
}

} // namespace sa::analysis
