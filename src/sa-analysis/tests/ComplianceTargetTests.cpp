#include <sa/analysis/ComplianceTarget.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <set>

using namespace sa;
using namespace sa::analysis;
using Catch::Approx;

TEST_CASE("The shipped targets carry the published numbers", "[analysis][compliance]") {
    CHECK(targetFor(LoudnessPlatform::Spotify).integratedLufs == -14.0);
    CHECK(targetFor(LoudnessPlatform::AppleMusic).integratedLufs == -16.0);
    CHECK(targetFor(LoudnessPlatform::YouTube).integratedLufs == -14.0);
    CHECK(targetFor(LoudnessPlatform::AmazonMusic).integratedLufs == -14.0);
    CHECK(targetFor(LoudnessPlatform::Tidal).integratedLufs == -14.0);
    CHECK(targetFor(LoudnessPlatform::Podcast).integratedLufs == -16.0);
    CHECK(targetFor(LoudnessPlatform::EbuR128).integratedLufs == -23.0);
    CHECK(targetFor(LoudnessPlatform::AtscA85).integratedLufs == -24.0);

    // -1 dBTP is the usual ceiling; the two that ask for more room ask for -2.
    CHECK(targetFor(LoudnessPlatform::Spotify).truePeakCeilingDbtp == -1.0);
    CHECK(targetFor(LoudnessPlatform::AmazonMusic).truePeakCeilingDbtp == -2.0);
    CHECK(targetFor(LoudnessPlatform::AtscA85).truePeakCeilingDbtp == -2.0);

    // Tolerances the standards actually state, as opposed to our display default.
    CHECK(targetFor(LoudnessPlatform::EbuR128).toleranceLu == 0.5);
    CHECK(targetFor(LoudnessPlatform::AtscA85).toleranceLu == 2.0);
}

TEST_CASE("The target table is complete and unambiguous", "[analysis][compliance]") {
    REQUIRE(targetCount() == 8);

    std::set<int> seen;
    for (std::size_t i = 0; i < targetCount(); ++i) {
        const ComplianceTarget& target = allTargets()[i];
        INFO("target " << i << " " << target.name);
        CHECK_FALSE(target.name.empty());
        CHECK(target.integratedLufs < 0.0);
        CHECK(target.truePeakCeilingDbtp <= 0.0);
        CHECK(target.toleranceLu > 0.0);
        // Every enumerator appears exactly once, and targetFor agrees with the
        // table -- otherwise a preset list and a check could disagree.
        CHECK(seen.insert(static_cast<int>(target.platform)).second);
        CHECK(targetFor(target.platform).integratedLufs == target.integratedLufs);
        CHECK(toString(target.platform) == target.name);
    }
}

TEST_CASE("A master on target and under the ceiling passes", "[analysis][compliance]") {
    const auto result = check(LoudnessPlatform::Spotify, -14.0, -1.5);
    CHECK(result.passed());
    CHECK(result.loudnessOnTarget);
    CHECK(result.truePeakWithinCeiling);
    CHECK(result.loudnessDeviationLu == Approx(0.0).margin(1e-12));
    CHECK(result.truePeakMarginDb == Approx(0.5));
    CHECK(result.gainToTargetDb == Approx(0.0).margin(1e-12));
}

TEST_CASE("Peaks over the ceiling fail even at the right loudness", "[analysis][compliance]") {
    // The common real case: a master limited to 0.0 dBFS that measures dead on
    // target and still clips the moment anything reconstructs it.
    const auto result = check(LoudnessPlatform::AppleMusic, -16.0, 0.4);
    CHECK_FALSE(result.passed());
    CHECK(result.loudnessOnTarget);
    CHECK_FALSE(result.truePeakWithinCeiling);
    CHECK(result.truePeakMarginDb == Approx(-1.4));
}

TEST_CASE("Loudness outside tolerance fails", "[analysis][compliance]") {
    // EBU R128 allows +/-0.5 LU, so -22.4 is out and -22.6 is in.
    CHECK_FALSE(check(LoudnessPlatform::EbuR128, -22.4, -3.0).passed());
    CHECK(check(LoudnessPlatform::EbuR128, -22.6, -3.0).passed());
    CHECK(check(LoudnessPlatform::EbuR128, -23.5, -3.0).passed());
    CHECK_FALSE(check(LoudnessPlatform::EbuR128, -23.6, -3.0).passed());

    // ATSC A/85 is far more forgiving, and the same measurement passes there.
    CHECK(check(LoudnessPlatform::AtscA85, -22.4, -3.0).passed());
}

TEST_CASE("The deviation sign says which way the master is wrong", "[analysis][compliance]") {
    const auto quiet = check(LoudnessPlatform::YouTube, -20.0, -6.0);
    CHECK(quiet.loudnessDeviationLu == Approx(-6.0));
    CHECK(quiet.gainToTargetDb == Approx(6.0));

    const auto loud = check(LoudnessPlatform::YouTube, -9.0, -6.0);
    CHECK(loud.loudnessDeviationLu == Approx(5.0));
    CHECK(loud.gainToTargetDb == Approx(-5.0));
}

TEST_CASE("The conform gain never pushes peaks through the ceiling", "[analysis][compliance]") {
    // 6 dB too quiet but only 2 dB of true-peak headroom. Applying the gain the
    // loudness target asks for would clip; conformGainDb stops short and says
    // so, which is the difference between a one-click conform and a one-click
    // ruined master.
    const auto limited = check(LoudnessPlatform::Spotify, -20.0, -3.0);
    CHECK(limited.gainToTargetDb == Approx(6.0));
    CHECK(limited.truePeakMarginDb == Approx(2.0));
    CHECK(limited.conformGainDb == Approx(2.0));

    // With headroom to spare, the two agree.
    const auto roomy = check(LoudnessPlatform::Spotify, -20.0, -12.0);
    CHECK(roomy.conformGainDb == Approx(6.0));

    // Turning down is always available, however hot the peaks are.
    const auto hot = check(LoudnessPlatform::Spotify, -8.0, 0.5);
    CHECK(hot.gainToTargetDb == Approx(-6.0));
    CHECK(hot.conformGainDb == Approx(-6.0));
}

TEST_CASE("An explicit target checks the same as a platform", "[analysis][compliance]") {
    ComplianceTarget custom;
    custom.name = "in-house";
    custom.integratedLufs = -18.0;
    custom.truePeakCeilingDbtp = -0.5;
    custom.toleranceLu = 0.25;

    CHECK(check(custom, -18.2, -0.6).passed());
    CHECK_FALSE(check(custom, -18.3, -0.6).passed());
    CHECK_FALSE(check(custom, -18.0, -0.4).passed());
    CHECK(check(custom, -18.0, -0.6).target.name == "in-house");
}
