#include <sa/io/PeakPyramid.h>
#include <sa/ui/WaveformView.h>

#include <QColor>
#include <QImage>
#include <QPixmap>
#include <QRgb>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <map>
#include <memory>
#include <numbers>
#include <vector>

using namespace sa;
using namespace sa::ui;

/// What the waveform draws that no windowless class can be asked about.
///
/// Everything about *where* an overlay goes is arithmetic and lives in
/// AnalysisOverlay, where AnalysisOverlayTests calls it. What is left is the
/// pen: a beat grid fitted to an onset envelope that barely repeats is drawn
/// dashed instead of solid, and the end of a bounded analysis is drawn as a
/// dotted line across the plot. Both are claims about a picture, so they are
/// checked by taking one.
namespace {

constexpr SampleRate kRate{48000.0};
constexpr double kTwoPi = 2.0 * std::numbers::pi;

/// Eight seconds, and a view 800 columns wide over all of it, so a beat a
/// second apart is a hundred columns from its neighbours and no two beats can
/// share a column.
constexpr SampleCount kFrames = 8 * 48000;
constexpr int kWidth = 800;
constexpr int kHeight = 240;

/// WaveformView's own colours, restated here rather than shared with it.
///
/// The same arrangement tools/ui_analysis_test.py works under, and for the same
/// reason: a test that read the constant out of the file it is checking would
/// agree with any value that file held, including the waveform's own blue.
constexpr QRgb kBeatLine = qRgb(0xc7, 0x7d, 0xff);
constexpr QRgb kBoundary = qRgb(0x6d, 0x73, 0x82);

[[nodiscard]] AudioBuffer tone(SampleCount frames) {
    AudioBuffer buffer{ChannelLayout::mono(), frames};
    for (SampleCount i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / kRate.hz();
        buffer.channel(0)[i] = static_cast<float>(0.5 * std::sin(kTwoPi * 220.0 * t));
    }
    return buffer;
}

/// A view with audio behind it, sized and ready to be grabbed.
[[nodiscard]] std::unique_ptr<WaveformView> viewOverTone() {
    auto view = std::make_unique<WaveformView>();
    view->resize(kWidth, kHeight);
    const AudioBuffer audio = tone(kFrames);
    auto built = io::PeakPyramid::build(audio.constView());
    REQUIRE(built);
    view->setPyramid(std::make_shared<io::PeakPyramid>(std::move(built).value()), kRate);
    return view;
}

/// How many rows of each column carry `colour`, for the columns that have any.
[[nodiscard]] std::map<int, int> columnsOf(const QImage& shot, QRgb colour) {
    std::map<int, int> found;
    for (int y = 0; y < shot.height(); ++y) {
        for (int x = 0; x < shot.width(); ++x) {
            if ((shot.pixel(x, y) & 0x00ffffffU) == (colour & 0x00ffffffU)) {
                ++found[x];
            }
        }
    }
    return found;
}

/// The columns where a colour runs down a useful part of the plot.
///
/// The threshold is what keeps the "analysis ends" caption out of the reading:
/// it is drawn with the same pen as the line beside it, so a letter can put a
/// dozen rows of that exact colour into a column, and a dozen rows is not a
/// line across a plot two hundred and forty rows tall.
[[nodiscard]] std::vector<int> linesOf(const QImage& shot, QRgb colour) {
    std::vector<int> columns;
    for (const auto& [x, count] : columnsOf(shot, colour)) {
        if (count > shot.height() / 5) {
            columns.push_back(x);
        }
    }
    return columns;
}

/// The column a sample falls in, recomputed here from the view's geometry.
[[nodiscard]] int columnOf(SampleIndex sample) {
    return kGutterWidth +
           static_cast<int>(std::lround(static_cast<double>(sample) / static_cast<double>(kFrames) *
                                        (kWidth - kGutterWidth)));
}

/// One beat a second, which is 60 BPM and eight beats in the file.
[[nodiscard]] std::vector<double> everySecond() {
    std::vector<double> beats;
    for (int i = 0; i < 8; ++i) {
        beats.push_back(static_cast<double>(i) + 0.25);
    }
    return beats;
}

} // namespace

TEST_CASE("A tempo the tracker is sure of is drawn as a solid grid", "[ui][waveform][overlay]") {
    auto view = viewOverTone();
    view->setBeatGrid(everySecond(), 0, false);
    const QImage shot = view->grab().toImage();

    const std::vector<int> drawn = linesOf(shot, kBeatLine);
    REQUIRE(drawn.size() == everySecond().size());

    // Solid means solid: every row of the plot, which is the whole height of
    // the widget because the waveform's gutter is beside the plot and not
    // above it.
    const std::map<int, int> counts = columnsOf(shot, kBeatLine);
    for (const int x : drawn) {
        INFO("column " << x << " carries " << counts.at(x) << " of " << shot.height() << " rows");
        REQUIRE(counts.at(x) == shot.height());
    }

    // And in the right columns, worked out here from the geometry rather than
    // read back off the view.
    for (std::size_t i = 0; i < everySecond().size(); ++i) {
        const auto sample = static_cast<SampleIndex>(everySecond()[i] * kRate.hz());
        REQUIRE(drawn[i] == columnOf(sample));
    }
}

TEST_CASE("A tempo the tracker doubts is drawn as a dashed grid", "[ui][waveform][overlay]") {
    // The case no probe has ever produced: a tempo that is valid and whose
    // confidence is under the threshold at which the panel starts hedging. The
    // threshold is driven directly here, because what is being checked is the
    // pen and not the tracker.
    auto view = viewOverTone();
    view->setBeatGrid(everySecond(), 0, true);
    const QImage shot = view->grab().toImage();

    const std::vector<int> drawn = linesOf(shot, kBeatLine);
    REQUIRE(drawn.size() == everySecond().size());

    const std::map<int, int> counts = columnsOf(shot, kBeatLine);
    for (const int x : drawn) {
        INFO("column " << x << " carries " << counts.at(x) << " of " << shot.height() << " rows");
        // A dash leaves gaps, which is the whole of the difference: the line is
        // still recognisably a line, and it is not a solid one.
        REQUIRE(counts.at(x) < shot.height());
        REQUIRE(counts.at(x) > shot.height() / 3);
    }

    // The same beats in the same columns as the solid grid, so the dashing is
    // the only thing that changed.
    auto solid = viewOverTone();
    solid->setBeatGrid(everySecond(), 0, false);
    const std::map<int, int> firm = columnsOf(solid->grab().toImage(), kBeatLine);
    REQUIRE(linesOf(solid->grab().toImage(), kBeatLine) == drawn);
    for (const int x : drawn) {
        REQUIRE(counts.at(x) < firm.at(x));
    }
}

TEST_CASE("A bounded analysis is drawn with the line where it stopped", "[ui][waveform][overlay]") {
    // The line the panel's "read the first 2:00 of 5:13" is the caption for.
    // Confirmed by hand once, on a file too long for any driver probe; here the
    // end is simply stated, which is all the view is given anyway.
    const SampleIndex end = kFrames / 2;
    auto view = viewOverTone();
    view->setBeatGrid(everySecond(), 0, false);
    view->setAnalysedEnd(end);
    const QImage shot = view->grab().toImage();

    const std::vector<int> drawn = linesOf(shot, kBoundary);
    REQUIRE(drawn.size() == 1);
    REQUIRE(drawn.front() == columnOf(end));

    const std::map<int, int> counts = columnsOf(shot, kBoundary);
    INFO("the boundary column carries " << counts.at(drawn.front()) << " of " << shot.height()
                                        << " rows");
    // Dotted, not solid and not dashed: a third of the rows, against the two
    // thirds a dash leaves and the whole height a solid line does.
    REQUIRE(counts.at(drawn.front()) < shot.height() / 2);
    REQUIRE(counts.at(drawn.front()) > shot.height() / 5);
}

TEST_CASE("An analysis that covered everything draws no boundary", "[ui][waveform][overlay]") {
    // Which is what makes the line above mean something. A view that drew it
    // always would pass every assertion in the case before this one.
    auto view = viewOverTone();
    view->setBeatGrid(everySecond(), 0, false);
    view->setAnalysedEnd(-1);
    REQUIRE(linesOf(view->grab().toImage(), kBoundary).empty());
}

TEST_CASE("A boundary with nothing drawn beside it is not drawn", "[ui][waveform][overlay]") {
    // Nothing bounded is on screen, so a line saying where the bound fell would
    // be a caption on an empty picture.
    auto view = viewOverTone();
    view->setAnalysedEnd(kFrames / 2);
    REQUIRE(linesOf(view->grab().toImage(), kBoundary).empty());
}

TEST_CASE("Clearing the overlays takes the grid and the boundary with them",
          "[ui][waveform][overlay]") {
    auto view = viewOverTone();
    view->setBeatGrid(everySecond(), 0, true);
    view->setAnalysedEnd(kFrames / 2);
    REQUIRE_FALSE(linesOf(view->grab().toImage(), kBeatLine).empty());

    view->clearOverlays();
    const QImage shot = view->grab().toImage();
    REQUIRE(linesOf(shot, kBeatLine).empty());
    REQUIRE(linesOf(shot, kBoundary).empty());
}
