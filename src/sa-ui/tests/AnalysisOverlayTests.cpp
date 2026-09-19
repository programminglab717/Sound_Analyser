#include <sa/ui/AnalysisOverlay.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

using namespace sa;
using namespace sa::ui;
using Catch::Approx;

namespace {

constexpr SampleRate kRate{48000.0};

/// A plot a thousand columns wide showing one second at 48 kHz, so that a
/// column is 48 samples exactly.
constexpr TimePlot kTime{kGutterWidth, 1000, 0, 48000};

/// An axis 401 rows tall over the tracker's default range.
///
/// 401 rows means 400 intervals, so a fraction of the axis is a round number
/// of rows: the bottom is row 400, the top row 0, and the geometric middle row
/// 200 exactly.
constexpr PitchAxis kAxis{50.0, 1000.0, 0, 401};

/// The spectrum panel geometry EqCurveTests uses, for the same reason: 1080
/// intervals over the 108 dB axis is ten rows to the decibel.
constexpr SpectrumPlot kSpectrum{0, 0, 1000, 1081, 24000.0};

/// Column of a frequency on the spectrum panel's log axis, derived here from
/// the axis definition rather than borrowed from the code under test.
[[nodiscard]] int axisColumn(double hz) {
    const double fraction =
        std::log(hz / kLogAxisMinimumHz) / std::log(24000.0 / kLogAxisMinimumHz);
    return static_cast<int>(std::lround(fraction * kSpectrum.width));
}

/// One contour point, spelt out so the tests below read as the data they are.
[[nodiscard]] analysis::PitchPoint point(double seconds, double hz, bool voiced) {
    analysis::PitchPoint made;
    made.timeSeconds = seconds;
    made.hz = hz;
    made.voiced = voiced;
    made.confidence = voiced ? 0.9 : 0.1;
    return made;
}

[[nodiscard]] analysis::Band band(double centreHz, double lowHz, double highHz, double levelDb) {
    analysis::Band made;
    made.centreHz = centreHz;
    made.lowHz = lowHz;
    made.highHz = highHz;
    made.levelDb = levelDb;
    return made;
}

} // namespace

TEST_CASE("A pitch axis puts its own bounds on its first and last rows") {
    REQUIRE(kAxis.yAtHz(50.0) == 400);
    REQUIRE(kAxis.yAtHz(1000.0) == 0);
}

TEST_CASE("A pitch axis is logarithmic, so the geometric middle is the middle row") {
    // sqrt(50 * 1000) = 223.607 Hz is half of log(1000/50) above 50 Hz, and
    // half of 400 intervals is row 200.
    REQUIRE(kAxis.yAtHz(std::sqrt(50.0 * 1000.0)) == 200);

    // 100 Hz is log(2)/log(20) = 0.23138 of the way up, so 0.76862 of 400
    // intervals down from the top: row 307.4, which rounds to 307.
    REQUIRE(kAxis.yAtHz(100.0) == 307);

    // An octave is an octave wherever it falls, which is the whole reason for
    // a log axis: 100 to 200 Hz and 400 to 800 Hz must span the same rows.
    REQUIRE(kAxis.yAtHz(100.0) - kAxis.yAtHz(200.0) == kAxis.yAtHz(400.0) - kAxis.yAtHz(800.0));
}

TEST_CASE("A pitch axis inverts a row to the frequency drawn there") {
    REQUIRE(kAxis.hzAtY(200) == Approx(std::sqrt(50.0 * 1000.0)));
    REQUIRE(kAxis.hzAtY(400) == Approx(50.0));
    REQUIRE(kAxis.hzAtY(0) == Approx(1000.0));
}

TEST_CASE("A pitch axis does not pin a frequency outside it to its edge") {
    // A line ruled along the top of a panel is a reading that was never taken,
    // so a frequency off the axis comes back off the axis and pitchRuns breaks
    // the contour there.
    REQUIRE_FALSE(kAxis.holds(2000.0));
    REQUIRE(kAxis.yAtHz(2000.0) < kAxis.top);
    REQUIRE_FALSE(kAxis.holds(25.0));
    REQUIRE(kAxis.yAtHz(25.0) > kAxis.top + kAxis.height - 1);
}

TEST_CASE("A contour breaks across an unvoiced frame") {
    const std::vector<analysis::PitchPoint> contour{
        point(0.00, 220.0, true), point(0.01, 220.0, true), point(0.02, 0.0, false),
        point(0.03, 440.0, true), point(0.04, 440.0, true),
    };
    const auto runs = pitchRuns(contour, 0, kRate, kTime, kAxis);

    REQUIRE(runs.size() == 2);
    REQUIRE(runs[0].size() == 2);
    REQUIRE(runs[1].size() == 2);

    // The break is what stops the drawing ruling a straight line from 220 Hz
    // to 440 Hz across a rest, which would read as a glissando nobody played.
    REQUIRE(runs[0].back().y == kAxis.yAtHz(220.0));
    REQUIRE(runs[1].front().y == kAxis.yAtHz(440.0));

    // 0.03 s at 48 kHz is sample 1440, which is 1440/48000 of a thousand
    // columns -- 30 -- past the gutter.
    REQUIRE(runs[1].front().x == kGutterWidth + 30);
}

TEST_CASE("A contour breaks at a frame the axis cannot hold") {
    const std::vector<analysis::PitchPoint> contour{
        point(0.00, 220.0, true),
        point(0.01, 2000.0, true), // Voiced, and off the top of this axis.
        point(0.02, 220.0, true),
    };
    const auto runs = pitchRuns(contour, 0, kRate, kTime, kAxis);
    REQUIRE(runs.size() == 2);
    REQUIRE(runs[0].size() == 1);
    REQUIRE(runs[1].size() == 1);
}

TEST_CASE("A single voiced frame between two rests is still a run") {
    const std::vector<analysis::PitchPoint> contour{
        point(0.00, 0.0, false),
        point(0.01, 330.0, true),
        point(0.02, 0.0, false),
    };
    const auto runs = pitchRuns(contour, 0, kRate, kTime, kAxis);
    REQUIRE(runs.size() == 1);
    REQUIRE(runs[0].size() == 1);
    REQUIRE(runs[0].front().y == kAxis.yAtHz(330.0));
}

TEST_CASE("A contour is placed against the document, not against what was analysed") {
    // The contour's own times start at zero whatever part of the file was
    // read, so the offset has to go on somewhere. 24,000 samples in plus 0.01 s
    // is sample 24,480, which is 510 columns past the gutter.
    const std::vector<analysis::PitchPoint> contour{point(0.01, 220.0, true)};
    const auto runs = pitchRuns(contour, 24000, kRate, kTime, kAxis);
    REQUIRE(runs.size() == 1);
    REQUIRE(runs[0].front().x == kGutterWidth + 510);
}

TEST_CASE("A contour keeps the points that fall outside the plot") {
    // Kept so that the painter clips them, rather than dropped so that the
    // visible line starts a hop late.
    const std::vector<analysis::PitchPoint> contour{point(0.0, 220.0, true),
                                                    point(2.0, 220.0, true)};
    const auto runs = pitchRuns(contour, 0, kRate, kTime, kAxis);
    REQUIRE(runs.size() == 1);
    REQUIRE(runs[0].size() == 2);
    REQUIRE_FALSE(kTime.holds(runs[0].back().x));
}

TEST_CASE("A beat grid lands where the beats are") {
    // Two seconds on screen at 48 kHz over a thousand columns: a beat every
    // half second is a beat every 250 columns.
    constexpr TimePlot twoSeconds{kGutterWidth, 1000, 0, 96000};
    const std::vector<double> beats{0.0, 0.5, 1.0, 1.5};
    const auto columns = beatColumns(beats, 0, kRate, twoSeconds);

    REQUIRE(columns.size() == 4);
    REQUIRE(columns[0] == kGutterWidth);
    REQUIRE(columns[1] == kGutterWidth + 250);
    REQUIRE(columns[2] == kGutterWidth + 500);
    REQUIRE(columns[3] == kGutterWidth + 750);
}

TEST_CASE("A beat grid drops the beats that are off screen") {
    constexpr TimePlot twoSeconds{kGutterWidth, 1000, 0, 96000};
    const std::vector<double> beats{-0.5, 0.5, 2.5};
    const auto columns = beatColumns(beats, 0, kRate, twoSeconds);
    REQUIRE(columns.size() == 1);
    REQUIRE(columns.front() == kGutterWidth + 250);
}

TEST_CASE("A beat grid is placed against the document, not against what was analysed") {
    // A grid fitted to a selection starting a second in has its first beat at
    // zero seconds, and that beat belongs a second into the file.
    const std::vector<double> beats{0.0};
    const auto columns = beatColumns(beats, 48000, kRate, TimePlot{kGutterWidth, 1000, 0, 96000});
    REQUIRE(columns.size() == 1);
    REQUIRE(columns.front() == kGutterWidth + 500);
}

TEST_CASE("A band bar spans the edges that were integrated") {
    const auto bars = bandBars({band(1000.0, 891.0, 1122.0, -36.0)}, kSpectrum);
    REQUIRE(bars.size() == 1);
    REQUIRE(bars[0].left == axisColumn(891.0));
    // One column short of the next band's first, so neighbours touch without
    // drawing the boundary twice.
    REQUIRE(bars[0].right == axisColumn(1122.0) - 1);
    // -36 dB is a third of the way down a 108 dB axis, and a third of 1080
    // intervals is row 360.
    REQUIRE(bars[0].top == 360);
}

TEST_CASE("Band bars do not overlap and leave no gaps") {
    // Three contiguous third-octaves: each one's top edge is the next one's
    // bottom, which is how the layout comes out of bandLayout().
    const std::vector<analysis::Band> bands{band(500.0, 445.0, 561.0, -40.0),
                                            band(630.0, 561.0, 707.0, -42.0),
                                            band(800.0, 707.0, 891.0, -44.0)};
    const auto bars = bandBars(bands, kSpectrum);
    REQUIRE(bars.size() == 3);
    for (std::size_t i = 1; i < bars.size(); ++i) {
        REQUIRE(bars[i].left == bars[i - 1].right + 1);
    }
}

TEST_CASE("A band below the bottom of the frequency axis starts at the left edge") {
    // The 20 Hz third-octave runs from 17.8 Hz, which is below the 20 Hz the
    // panel's log axis starts at. Clamping is right here: the band really is
    // off the left of the axis, and there is nowhere further left to draw it.
    const auto bars = bandBars({band(20.0, 17.8, 22.4, -60.0)}, kSpectrum);
    REQUIRE(bars.size() == 1);
    REQUIRE(bars[0].left == kSpectrum.left);
    REQUIRE(bars[0].right == axisColumn(22.4) - 1);
}

TEST_CASE("A band with nothing in it still has a bar, on the bottom row") {
    // Returned rather than filtered out, so that a caller counting bars gets
    // the layout and not a version of it with the quiet bands missing.
    const auto bars = bandBars({band(20.0, 17.8, 22.4, analysis::kDecibelFloor)}, kSpectrum);
    REQUIRE(bars.size() == 1);
    REQUIRE(bars[0].top == kSpectrum.top + kSpectrum.height - 1);
}

TEST_CASE("A pitch axis is labelled at the 1-2-5 places inside its own range") {
    const auto ticks = pitchTicks(kAxis);
    REQUIRE(ticks.size() == 5); // 50, 100, 200, 500, 1000.
    REQUIRE(ticks.front().value == Approx(50.0));
    REQUIRE(ticks.back().value == Approx(1000.0));
    // Measured from the bottom of the axis, as everywhere else in
    // ViewGeometry.h: the tracker's lowest pitch is 0 and its highest is 1.
    REQUIRE(ticks.front().fraction == Approx(0.0));
    REQUIRE(ticks.back().fraction == Approx(1.0));
    // log(500/50) / log(1000/50) = log(10) / log(20).
    REQUIRE(ticks[3].value == Approx(500.0));
    REQUIRE(ticks[3].fraction == Approx(std::log(10.0) / std::log(20.0)));
    REQUIRE(ticks[3].label == "500");
}
