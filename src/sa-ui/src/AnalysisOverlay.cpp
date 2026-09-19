#include <sa/ui/AnalysisOverlay.h>

#include <algorithm>
#include <cmath>

namespace sa::ui {

int PitchAxis::yAtHz(double hz) const noexcept {
    if (height <= 1 || !(lowHz > 0.0) || !(highHz > lowHz)) {
        return top;
    }
    if (!(hz > 0.0)) {
        // An unvoiced frame's zero, if one reaches here. Reported as a row
        // below the axis so that a caller testing whether the point is on the
        // axis gets the same answer whichever way it asks.
        return top + height;
    }
    const double fraction = std::log(hz / lowHz) / std::log(highHz / lowHz);
    // Bounded before it is rounded. A frequency far off the axis only has to
    // come back as off the axis, and an unbounded fraction would hand lround a
    // double no int can hold.
    const double bounded = std::clamp(fraction, -1.0, 2.0);
    return top + static_cast<int>(std::lround((1.0 - bounded) * (height - 1)));
}

double PitchAxis::hzAtY(int y) const noexcept {
    // One row tall is a degenerate axis rather than an error -- it happens
    // while a splitter is being dragged shut -- and every frequency in it is
    // the top of the axis.
    if (height <= 1 || !(lowHz > 0.0) || !(highHz > lowHz)) {
        return highHz;
    }
    const double fraction =
        1.0 - std::clamp(static_cast<double>(y - top) / static_cast<double>(height - 1), 0.0, 1.0);
    return lowHz * std::pow(highHz / lowHz, fraction);
}

std::vector<AxisTick> pitchTicks(const PitchAxis& axis) {
    std::vector<AxisTick> ticks;
    if (!(axis.lowHz > 0.0) || !(axis.highHz > axis.lowHz)) {
        return ticks;
    }
    static constexpr double kMantissas[] = {1.0, 2.0, 5.0};
    const double span = std::log(axis.highHz / axis.lowHz);
    for (int decade = 1; decade <= 5; ++decade) {
        const double base = std::pow(10.0, decade); // 10, 100, 1k, 10k, 100k
        for (const double mantissa : kMantissas) {
            const double hz = base * mantissa;
            if (hz < axis.lowHz || hz > axis.highHz) {
                continue;
            }
            AxisTick tick;
            tick.value = hz;
            tick.fraction = std::log(hz / axis.lowHz) / span;
            tick.label = formatFrequency(hz);
            ticks.push_back(std::move(tick));
        }
    }
    return ticks;
}

std::vector<std::vector<ContourPoint>> pitchRuns(const std::vector<analysis::PitchPoint>& contour,
                                                 SampleIndex startSample, SampleRate rate,
                                                 const TimePlot& time, const PitchAxis& axis) {
    std::vector<std::vector<ContourPoint>> runs;
    if (contour.empty() || !rate.isValid() || time.width <= 0) {
        return runs;
    }

    std::vector<ContourPoint> run;
    const auto endRun = [&] {
        // A run of one point is kept. It is a frame that was voiced with
        // silence either side, and dropping it would hide the shortest notes
        // there are -- the panel draws it as a dot.
        if (!run.empty()) {
            runs.push_back(std::move(run));
            run.clear();
        }
    };

    for (const analysis::PitchPoint& point : contour) {
        if (!point.voiced || !axis.holds(point.hz)) {
            endRun();
            continue;
        }
        // The contour's times are measured from the start of what was
        // analysed, and the axis is drawn against the document, so the offset
        // goes on here rather than being left for each caller to remember.
        const int x = time.xAtSample(startSample + secondsToSamples(point.timeSeconds, rate));
        run.push_back(ContourPoint{x, axis.yAtHz(point.hz)});
    }
    endRun();
    return runs;
}

std::vector<int> beatColumns(const std::vector<double>& beatSeconds, SampleIndex startSample,
                             SampleRate rate, const TimePlot& time) {
    std::vector<int> columns;
    if (beatSeconds.empty() || !rate.isValid() || time.width <= 0) {
        return columns;
    }
    columns.reserve(beatSeconds.size());
    for (const double seconds : beatSeconds) {
        const int x = time.xAtSample(startSample + secondsToSamples(seconds, rate));
        if (time.holds(x)) {
            columns.push_back(x);
        }
    }
    return columns;
}

std::vector<BandBar> bandBars(const std::vector<analysis::Band>& bands, const SpectrumPlot& plot) {
    std::vector<BandBar> bars;
    if (bands.empty() || plot.width <= 0 || plot.height <= 0) {
        return bars;
    }
    const int lastColumn = plot.left + plot.width - 1;
    bars.reserve(bands.size());
    for (const analysis::Band& band : bands) {
        BandBar bar;
        bar.left = std::clamp(plot.xAtFrequency(band.lowHz), plot.left, lastColumn);
        // One column short of the next band's first, so neighbouring bars
        // touch without overlapping -- an overlap would draw the boundary
        // twice and make every band look a column wider than it is.
        bar.right = std::clamp(plot.xAtFrequency(band.highHz) - 1, bar.left, lastColumn);
        bar.top = plot.yAtLevel(band.levelDb);
        bars.push_back(bar);
    }
    return bars;
}

} // namespace sa::ui
