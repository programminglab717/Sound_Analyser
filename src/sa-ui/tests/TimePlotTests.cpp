#include <sa/ui/ViewGeometry.h>

#include <catch2/catch_test_macros.hpp>

using namespace sa;
using namespace sa::ui;

namespace {

/// A plot a thousand columns wide showing exactly one second at 48 kHz.
///
/// Both numbers are chosen so the assertions below are arithmetic rather than
/// tolerance: a thousand columns over 48,000 samples is 48 samples to the
/// column exactly, and a second on screen makes a fraction of the view a round
/// number of milliseconds.
constexpr TimePlot kPlot{kGutterWidth, 1000, 0, 48000};
constexpr SampleRate kRate{48000.0};

} // namespace

TEST_CASE("A time plot puts the start of the view on the first column after the gutter") {
    REQUIRE(kPlot.xAtSample(0) == kGutterWidth);
    REQUIRE(kPlot.holds(kGutterWidth));
}

TEST_CASE("A time plot maps a sample to a column by its fraction of the view") {
    // Half of 48,000 samples is 24,000, half of a thousand columns is 500, and
    // the plot starts at the gutter.
    REQUIRE(kPlot.xAtSample(24000) == kGutterWidth + 500);
    REQUIRE(kPlot.xAtSample(12000) == kGutterWidth + 250);

    // The end of the view is one past the last column it has, which is what
    // makes holds() the right question to ask about a drawn point.
    REQUIRE(kPlot.xAtSample(48000) == kGutterWidth + 1000);
    REQUIRE_FALSE(kPlot.holds(kGutterWidth + 1000));
    REQUIRE(kPlot.holds(kGutterWidth + 999));
}

TEST_CASE("A time plot does not clamp samples outside the view") {
    // Deliberately outside the plot rather than pinned to its edge: a contour
    // that started at the left edge whenever it began off-screen would move by
    // up to a whole hop every time the view scrolled.
    REQUIRE(kPlot.xAtSample(-48000) == kGutterWidth - 1000);
    REQUIRE(kPlot.xAtSample(96000) == kGutterWidth + 2000);
    REQUIRE_FALSE(kPlot.holds(kGutterWidth - 1000));
}

TEST_CASE("A time plot maps a scrolled view from its own start") {
    // Half a second in, showing half a second: 36,000 is halfway across.
    constexpr TimePlot scrolled{kGutterWidth, 1000, 24000, 24000};
    REQUIRE(scrolled.xAtSample(24000) == kGutterWidth);
    REQUIRE(scrolled.xAtSample(36000) == kGutterWidth + 500);
    REQUIRE(scrolled.xAtSample(0) == kGutterWidth - 1000);
}

TEST_CASE("A time plot maps seconds through the same rule as samples") {
    // 0.5 s at 48 kHz is sample 24,000, which is column 500 of the plot above.
    REQUIRE(kPlot.xAtSeconds(0.5, kRate) == kPlot.xAtSample(24000));
    REQUIRE(kPlot.xAtSeconds(0.5, kRate) == kGutterWidth + 500);

    // A beat time lands on the nearest sample first and the nearest column
    // after that. 0.25 s is sample 12,000; a microsecond either side of it is
    // the same sample and so the same column.
    REQUIRE(kPlot.xAtSeconds(0.25 + 1e-6, kRate) == kPlot.xAtSample(12000));
    REQUIRE(kPlot.xAtSeconds(0.25 - 1e-6, kRate) == kPlot.xAtSample(12000));
}

TEST_CASE("A time plot inverts a column to the sample it covers") {
    REQUIRE(kPlot.sampleAtX(kGutterWidth) == 0);
    REQUIRE(kPlot.sampleAtX(kGutterWidth + 500) == 24000);

    // Clamped, unlike the forward mapping: a pointer outside the plot is
    // asking about the nearest sample it can see, whereas a contour point
    // outside the plot is genuinely elsewhere.
    REQUIRE(kPlot.sampleAtX(kGutterWidth - 100) == 0);
    REQUIRE(kPlot.sampleAtX(kGutterWidth + 5000) == 48000);
}

TEST_CASE("A degenerate time plot answers with its own left edge") {
    // Both happen while a splitter is being dragged shut, and neither is an
    // error: there is one place for everything to be drawn and it is the edge.
    constexpr TimePlot noWidth{kGutterWidth, 0, 0, 48000};
    constexpr TimePlot noView{kGutterWidth, 1000, 0, 0};
    REQUIRE(noWidth.xAtSample(24000) == kGutterWidth);
    REQUIRE(noView.xAtSample(24000) == kGutterWidth);
    REQUIRE(noWidth.sampleAtX(500) == 0);
}

TEST_CASE("A time plot with an invalid rate cannot place an instant") {
    REQUIRE(kPlot.xAtSeconds(0.5, SampleRate{0.0}) == kGutterWidth);
}
