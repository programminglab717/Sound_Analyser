#include <sa/ui/ViewGeometry.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <utility>

namespace sa::ui {

namespace {

/// Time divisions an engineer already thinks in. A general "nice number" rule
/// would offer 2.5 s, which nobody reads a transport in.
constexpr std::array<double, 22> kTimeSteps = {
    0.001, 0.002, 0.005, 0.01, 0.02, 0.05,  0.1,   0.2,   0.5,   1.0,    2.0,
    5.0,   10.0,  15.0,  30.0, 60.0, 120.0, 300.0, 600.0, 900.0, 1800.0, 3600.0};

/// Roughly this far apart, in pixels, before a label is too dense to read.
constexpr int kMinimumLabelSpacing = 78;
constexpr int kMinimumFrequencySpacing = 34;

} // namespace

double frequencyAtFraction(FrequencyScale scale, double fraction, double nyquistHz) noexcept {
    if (nyquistHz <= 0.0) {
        return 0.0;
    }
    if (scale == FrequencyScale::Linear) {
        return nyquistHz * fraction;
    }
    const double low = std::min(kLogAxisMinimumHz, nyquistHz * 0.5);
    return low * std::pow(nyquistHz / low, fraction);
}

double fractionAtFrequency(FrequencyScale scale, double hz, double nyquistHz) noexcept {
    if (nyquistHz <= 0.0) {
        return 0.0;
    }
    if (scale == FrequencyScale::Linear) {
        return hz / nyquistHz;
    }
    const double low = std::min(kLogAxisMinimumHz, nyquistHz * 0.5);
    if (hz <= low) {
        return 0.0;
    }
    return std::log(hz / low) / std::log(nyquistHz / low);
}

double SpectrumPlot::frequencyAtX(int x) const noexcept {
    const double fraction =
        width > 1 ? static_cast<double>(x - left) / static_cast<double>(width) : 0.0;
    return frequencyAtFraction(FrequencyScale::Logarithmic, std::clamp(fraction, 0.0, 1.0),
                               nyquistHz);
}

int SpectrumPlot::xAtFrequency(double hz) const noexcept {
    const double fraction = fractionAtFrequency(FrequencyScale::Logarithmic, hz, nyquistHz);
    return left + static_cast<int>(std::lround(fraction * width));
}

int SpectrumPlot::yAtLevel(double decibels) const noexcept {
    const double fraction =
        std::clamp((kSpectrumTopDb - decibels) / (kSpectrumTopDb - kSpectrumBottomDb), 0.0, 1.0);
    return top + static_cast<int>(std::lround(fraction * (height - 1)));
}

double SpectrumPlot::levelAtY(int y) const noexcept {
    // One row tall is a degenerate plot rather than an error -- it happens
    // while a splitter is being dragged shut -- and every level in it is the
    // top of the axis.
    if (height <= 1) {
        return kSpectrumTopDb;
    }
    const double fraction = static_cast<double>(y - top) / static_cast<double>(height - 1);
    return kSpectrumTopDb - std::clamp(fraction, 0.0, 1.0) * (kSpectrumTopDb - kSpectrumBottomDb);
}

std::string formatTime(double seconds, double spanSeconds) {
    if (seconds < 0.0) {
        seconds = 0.0;
    }
    char buffer[32];

    if (spanSeconds < 2.0) {
        // Under two seconds on screen, minutes are noise and milliseconds are
        // the whole point.
        std::snprintf(buffer, sizeof buffer, "%.3f", seconds);
        return buffer;
    }
    const auto totalSeconds = static_cast<long long>(seconds);
    const long long minutes = totalSeconds / 60;
    const long long remainder = totalSeconds % 60;

    if (spanSeconds < 20.0) {
        const double fractional = seconds - static_cast<double>(totalSeconds);
        std::snprintf(buffer, sizeof buffer, "%lld:%02lld.%01d", minutes, remainder,
                      static_cast<int>(fractional * 10.0 + 0.5) % 10);
        return buffer;
    }
    if (minutes >= 60) {
        std::snprintf(buffer, sizeof buffer, "%lld:%02lld:%02lld", minutes / 60, minutes % 60,
                      remainder);
        return buffer;
    }
    std::snprintf(buffer, sizeof buffer, "%lld:%02lld", minutes, remainder);
    return buffer;
}

std::string formatFrequency(double hz) {
    char buffer[24];
    if (hz < 1000.0) {
        std::snprintf(buffer, sizeof buffer, "%g", std::round(hz * 10.0) / 10.0);
        return buffer;
    }
    const double kilohertz = hz / 1000.0;
    if (kilohertz < 10.0 && std::abs(kilohertz - std::round(kilohertz)) > 0.05) {
        std::snprintf(buffer, sizeof buffer, "%.1fk", kilohertz);
    } else {
        std::snprintf(buffer, sizeof buffer, "%gk", std::round(kilohertz));
    }
    return buffer;
}

std::vector<AxisTick> timeTicks(double startSeconds, double spanSeconds, int pixels) {
    std::vector<AxisTick> ticks;
    if (spanSeconds <= 0.0 || pixels <= 0) {
        return ticks;
    }

    const double maximumTicks = std::max(1.0, static_cast<double>(pixels) / kMinimumLabelSpacing);
    const double wanted = spanSeconds / maximumTicks;

    double step = kTimeSteps.back();
    for (const double candidate : kTimeSteps) {
        if (candidate >= wanted) {
            step = candidate;
            break;
        }
    }

    // A tick every step from the first multiple at or after the view start.
    const double first = std::ceil(startSeconds / step) * step;
    // Guard against a pathological span producing thousands of ticks.
    const int limit = pixels;
    for (int i = 0; i < limit; ++i) {
        const double value = first + step * i;
        if (value > startSeconds + spanSeconds) {
            break;
        }
        AxisTick tick;
        tick.value = value;
        tick.fraction = (value - startSeconds) / spanSeconds;
        tick.label = formatTime(value, spanSeconds);
        ticks.push_back(std::move(tick));
    }
    return ticks;
}

std::vector<AxisTick> frequencyTicks(FrequencyScale scale, double nyquistHz, int pixels) {
    std::vector<AxisTick> ticks;
    if (nyquistHz <= 0.0 || pixels <= 0) {
        return ticks;
    }

    if (scale == FrequencyScale::Logarithmic) {
        // 1-2-5 per decade: the positions on every EQ anyone has ever used.
        static constexpr std::array<double, 3> kMantissas = {1.0, 2.0, 5.0};
        int lastLabelPixel = -kMinimumFrequencySpacing;

        for (int decade = 1; decade <= 5; ++decade) {
            const double base = std::pow(10.0, decade); // 10, 100, 1k, 10k, 100k
            for (const double mantissa : kMantissas) {
                const double hz = base * mantissa;
                if (hz < kLogAxisMinimumHz || hz > nyquistHz) {
                    continue;
                }
                AxisTick tick;
                tick.value = hz;
                tick.fraction = fractionAtFrequency(scale, hz, nyquistHz);
                const int pixel = static_cast<int>(tick.fraction * pixels);
                tick.major = pixel - lastLabelPixel >= kMinimumFrequencySpacing;
                if (tick.major) {
                    tick.label = formatFrequency(hz);
                    lastLabelPixel = pixel;
                }
                ticks.push_back(std::move(tick));
            }
        }
        return ticks;
    }

    const double maximumTicks =
        std::max(1.0, static_cast<double>(pixels) / kMinimumFrequencySpacing);
    const double wanted = nyquistHz / maximumTicks;
    const double magnitude = std::pow(10.0, std::floor(std::log10(wanted)));
    double step = magnitude;
    for (const double multiple : {1.0, 2.0, 5.0, 10.0}) {
        if (magnitude * multiple >= wanted) {
            step = magnitude * multiple;
            break;
        }
    }

    for (double hz = step; hz <= nyquistHz; hz += step) {
        AxisTick tick;
        tick.value = hz;
        tick.fraction = fractionAtFrequency(scale, hz, nyquistHz);
        tick.label = formatFrequency(hz);
        ticks.push_back(std::move(tick));
    }
    return ticks;
}

} // namespace sa::ui
