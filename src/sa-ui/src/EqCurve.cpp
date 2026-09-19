#include <sa/ui/EqCurve.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

namespace sa::ui {

namespace {

/// A ParametricEq at `rate`, or at 48 kHz when that rate is not one the
/// filters will accept.
///
/// The curve exists before any file is open and has to have somewhere to keep
/// bands, so an unusable rate is a reason to go on drawing at a sane default
/// rather than to leave the panel with no EQ in it at all. 48 kHz is valid by
/// construction, so the fallback cannot fail in its turn.
[[nodiscard]] dsp::ParametricEq makeEq(SampleRate rate) {
    Result<dsp::ParametricEq> made = dsp::ParametricEq::create(rate);
    if (made) {
        return std::move(made).value();
    }
    return dsp::ParametricEq::create(kSampleRate48000).value();
}

/// The highest frequency a band may be placed at, for a sample rate.
///
/// Short of Nyquist, where the cookbook peaking section degenerates into a wire
/// and a handle would sit on the edge of the plot doing nothing visible. Never
/// below the bottom of the axis either: SampleRate accepts anything positive,
/// and a rate under about 42 Hz would otherwise leave the clamp below with its
/// bounds the wrong way round. Such a rate then gets a band it cannot realise
/// and a refusal, which is the honest answer.
[[nodiscard]] double ceilingFor(SampleRate rate) {
    return std::max(rate.hz() * 0.5 * EqCurve::kFrequencyCeilingFraction, kLogAxisMinimumHz);
}

/// A band's parameters as they will be stored: inside the ranges the plot can
/// show and the filters can realise.
[[nodiscard]] dsp::EqBand clampedBand(double frequencyHz, double gainDb, double q,
                                      double ceilingHz) {
    dsp::EqBand band;
    band.filter.type = dsp::FilterType::Peaking;
    band.filter.frequency = std::clamp(frequencyHz, kLogAxisMinimumHz, ceilingHz);
    band.filter.gainDb = std::clamp(gainDb, -EqCurve::kGainLimitDb, EqCurve::kGainLimitDb);
    band.filter.q = std::clamp(q, EqCurve::kMinimumQ, EqCurve::kMaximumQ);
    band.enabled = true;
    return band;
}

[[nodiscard]] std::string formatBandFrequency(double hz) {
    char buffer[32];
    if (hz < 1000.0) {
        std::snprintf(buffer, sizeof buffer, "%.0f Hz", hz);
    } else {
        std::snprintf(buffer, sizeof buffer, "%.2f kHz", hz / 1000.0);
    }
    return buffer;
}

} // namespace

EqCurve::EqCurve(SampleRate rate) : eq_(makeEq(rate)) {}

double EqCurve::frequencyCeiling() const noexcept {
    return ceilingFor(eq_.sampleRate());
}

bool EqCurve::setSampleRate(SampleRate rate) {
    if (!rate.isValid()) {
        return false;
    }
    // The bands are in hertz and survive the change, but one that now sits
    // above Nyquist would be rejected by the designer. Pulling those down
    // first is not a liberty: the alternative is setSampleRate failing and the
    // curve staying designed for a rate the document no longer has.
    const double ceiling = ceilingFor(rate);
    for (int index = 0; index < eq_.bandCount(); ++index) {
        const dsp::EqBand* existing = eq_.band(index);
        if (existing == nullptr || existing->filter.frequency <= ceiling) {
            continue;
        }
        dsp::EqBand moved = *existing;
        moved.filter.frequency = ceiling;
        if (!eq_.setBand(index, moved)) {
            return false;
        }
    }
    return static_cast<bool>(eq_.setSampleRate(rate));
}

std::vector<dsp::EqBand> EqCurve::bands() const {
    std::vector<dsp::EqBand> list;
    list.reserve(static_cast<std::size_t>(eq_.bandCount()));
    for (int index = 0; index < eq_.bandCount(); ++index) {
        list.push_back(*eq_.band(index));
    }
    return list;
}

int EqCurve::addBand(double frequencyHz, double gainDb, double q) {
    const Result<int> added = eq_.addBand(clampedBand(frequencyHz, gainDb, q, frequencyCeiling()));
    return added ? added.value() : -1;
}

bool EqCurve::setBand(int index, double frequencyHz, double gainDb, double q) {
    return static_cast<bool>(
        eq_.setBand(index, clampedBand(frequencyHz, gainDb, q, frequencyCeiling())));
}

bool EqCurve::removeBand(int index) {
    return static_cast<bool>(eq_.removeBand(index));
}

bool EqCurve::isFlat() const noexcept {
    for (int index = 0; index < eq_.bandCount(); ++index) {
        const dsp::EqBand* existing = eq_.band(index);
        if (existing != nullptr && existing->enabled && existing->filter.gainDb != 0.0) {
            return false;
        }
    }
    return true;
}

double EqCurve::responseDbAt(double frequency) const noexcept {
    return eq_.magnitudeDbAt(frequency);
}

double EqCurve::bandResponseDbAt(int index, double frequency) const noexcept {
    const dsp::BiquadCoefficients* section = eq_.cascade().sectionCoefficients(index);
    if (section == nullptr) {
        return 0.0;
    }
    return section->magnitudeDb(eq_.sampleRate(), frequency);
}

std::vector<double> EqCurve::responseAcross(const SpectrumPlot& plot) const {
    std::vector<double> curve(static_cast<std::size_t>(std::max(1, plot.width)));
    for (std::size_t column = 0; column < curve.size(); ++column) {
        curve[column] = responseDbAt(plot.frequencyAtX(plot.left + static_cast<int>(column)));
    }
    return curve;
}

int EqCurve::handleX(const SpectrumPlot& plot, int index) const noexcept {
    const dsp::EqBand* existing = eq_.band(index);
    return existing == nullptr ? plot.left : plot.xAtFrequency(existing->filter.frequency);
}

int EqCurve::handleY(const SpectrumPlot& plot, int index) const noexcept {
    const dsp::EqBand* existing = eq_.band(index);
    return existing == nullptr ? plot.yAtGain(0.0) : plot.yAtGain(existing->filter.gainDb);
}

int EqCurve::handleAt(const SpectrumPlot& plot, int x, int y) const noexcept {
    int nearest = -1;
    int nearestDistance = kHandleRadius * kHandleRadius;
    for (int index = 0; index < eq_.bandCount(); ++index) {
        const int dx = handleX(plot, index) - x;
        const int dy = handleY(plot, index) - y;
        const int distance = dx * dx + dy * dy;
        // Not strictly nearer, so a later band on the same pixel takes it.
        if (distance <= nearestDistance) {
            nearest = index;
            nearestDistance = distance;
        }
    }
    return nearest;
}

bool EqCurve::moveHandleTo(const SpectrumPlot& plot, int index, int x, int y) {
    const dsp::EqBand* existing = eq_.band(index);
    if (existing == nullptr) {
        return false;
    }
    return setBand(index, plot.frequencyAtX(x), plot.gainAtY(y), existing->filter.q);
}

bool EqCurve::adjustQByNotches(int index, double notches) {
    const dsp::EqBand* existing = eq_.band(index);
    if (existing == nullptr) {
        return false;
    }
    return setBand(index, existing->filter.frequency, existing->filter.gainDb,
                   existing->filter.q * std::pow(kQPerWheelNotch, notches));
}

bool EqCurve::setQFromDrag(int index, double startQ, int dy) {
    const dsp::EqBand* existing = eq_.band(index);
    if (existing == nullptr) {
        return false;
    }
    // Upwards is a negative dy and has to narrow the band, to agree with the
    // wheel; hence the minus sign rather than an inverted constant.
    const double doublings = -static_cast<double>(dy) / kQPixelsPerDoubling;
    return setBand(index, existing->filter.frequency, existing->filter.gainDb,
                   startQ * std::pow(2.0, doublings));
}

std::string EqCurve::describe(int index) const {
    const dsp::EqBand* existing = eq_.band(index);
    if (existing == nullptr) {
        return {};
    }
    char buffer[96];
    std::snprintf(buffer, sizeof buffer, "%s   %+.1f dB   Q %.2f",
                  formatBandFrequency(existing->filter.frequency).c_str(), existing->filter.gainDb,
                  existing->filter.q);
    return buffer;
}

std::string EqCurve::summarise() const {
    if (eq_.bandCount() == 0) {
        return "no bands";
    }
    if (eq_.bandCount() == 1) {
        const dsp::EqBand* only = eq_.band(0);
        char buffer[64];
        std::snprintf(buffer, sizeof buffer, "%+.1f dB at %s", only->filter.gainDb,
                      formatBandFrequency(only->filter.frequency).c_str());
        return buffer;
    }
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%d bands", eq_.bandCount());
    return buffer;
}

} // namespace sa::ui
