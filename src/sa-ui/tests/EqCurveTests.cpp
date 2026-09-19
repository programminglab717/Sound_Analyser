#include <sa/ui/EqCurve.h>
#include <sa/ui/ViewGeometry.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <numbers>
#include <string>
#include <utility>
#include <vector>

using namespace sa;
using namespace sa::ui;
using Catch::Approx;

namespace {

constexpr double kRate = 48000.0;

/// A plot 1000 columns wide and 1081 rows tall, at 48 kHz.
///
/// Both numbers are chosen so that the assertions below are arithmetic rather
/// than tolerance: 1080 rows over the 108 dB axis is exactly ten rows to the
/// decibel, and a width of 1000 makes a fraction of the frequency axis a round
/// number of columns.
constexpr SpectrumPlot kPlot{0, 0, 1000, 1081, kRate * 0.5};

/// The bottom of the log frequency axis for kPlot: min(20 Hz, Nyquist/2).
constexpr double kAxisLow = kLogAxisMinimumHz;

/// Frequency at a fraction of the way across kPlot, from the axis definition
/// rather than from the code under test.
[[nodiscard]] double axisFrequency(double fraction) {
    return kAxisLow * std::pow(kPlot.nyquistHz / kAxisLow, fraction);
}

/// Magnitude of an RBJ peaking section in decibels, derived here rather than
/// borrowed from the DSP layer, so that agreement between the two is evidence.
///
/// With A = 10^(gain/40), w0 = 2 pi f0 / fs and alpha = sin(w0) / (2Q), the
/// cookbook section is
///
///     b = {1 + alpha A, -2 cos w0, 1 - alpha A}
///     a = {1 + alpha/A, -2 cos w0, 1 - alpha/A}
///
/// Writing z = e^(jw) and multiplying numerator and denominator through by z,
///
///     num = (1 + alpha A) z + (1 - alpha A) z^-1 - 2 cos w0
///         = 2(cos w - cos w0) + 2j alpha A sin w
///     den = 2(cos w - cos w0) + 2j (alpha / A) sin w
///
/// because z + z^-1 = 2 cos w and z - z^-1 = 2j sin w. So with
/// C = (cos w - cos w0)^2 and S = (alpha sin w)^2,
///
///     |H|^2 = (C + A^2 S) / (C + S / A^2).
[[nodiscard]] double peakingDb(double f0, double q, double gainDb, double frequency) {
    const double a = std::pow(10.0, gainDb / 40.0);
    const double w0 = 2.0 * std::numbers::pi * f0 / kRate;
    const double w = 2.0 * std::numbers::pi * frequency / kRate;
    const double alpha = std::sin(w0) / (2.0 * q);
    const double c = (std::cos(w) - std::cos(w0)) * (std::cos(w) - std::cos(w0));
    const double s = (alpha * std::sin(w)) * (alpha * std::sin(w));
    return 10.0 * std::log10((c + a * a * s) / (c + s / (a * a)));
}

/// The two frequencies at which a peaking band is exactly half its gain, in
/// decibels.
///
/// Setting |H|^2 = A^2 in the expression above gives C(1 - A^2) = S(1 - A^2),
/// so for any gain but unity the half-gain points are simply where
/// |cos w - cos w0| = alpha |sin w|. Substituting t = tan(w/2), using
/// (1 - cos w0) / (1 + cos w0) = tan^2(w0/2) and
/// sin(w0) / (1 + cos w0) = tan(w0/2), that collapses to
///
///     t^2 +- (T0 / Q) t - T0^2 = 0,   T0 = tan(w0 / 2)
///
/// whose positive roots are T0 (+-1/(2Q) + sqrt(1/(4Q^2) + 1)). The two are
/// geometrically symmetric in tan(w/2) rather than in hertz, which is the
/// bilinear transform showing through: their product is exactly T0^2.
[[nodiscard]] std::pair<double, double> halfGainFrequencies(double f0, double q) {
    const double t0 = std::tan(std::numbers::pi * f0 / kRate);
    const double root = std::sqrt(1.0 / (4.0 * q * q) + 1.0);
    const double upper = t0 * (root + 1.0 / (2.0 * q));
    const double lower = t0 * (root - 1.0 / (2.0 * q));
    return {kRate * std::atan(lower) / std::numbers::pi,
            kRate * std::atan(upper) / std::numbers::pi};
}

[[nodiscard]] EqCurve makeCurve() {
    return EqCurve{SampleRate{kRate}};
}

} // namespace

TEST_CASE("The frequency axis maps pixels both ways", "[ui][eq]") {
    // The axis runs from 20 Hz to Nyquist logarithmically, so the left edge is
    // 20 Hz, the right edge 24 kHz, and the middle their geometric mean:
    // 20 * sqrt(1200) = 692.820...
    CHECK(kPlot.frequencyAtX(0) == Approx(20.0));
    CHECK(kPlot.frequencyAtX(1000) == Approx(24000.0));
    CHECK(kPlot.frequencyAtX(500) == Approx(20.0 * std::sqrt(1200.0)));

    // A decade up from the bottom is log(10)/log(1200) of the way across:
    // 2.302585 / 7.090077 = 0.324761, so 325 columns of the thousand.
    CHECK(kPlot.xAtFrequency(200.0) == 325);
    CHECK(kPlot.xAtFrequency(20.0) == 0);
    CHECK(kPlot.xAtFrequency(24000.0) == 1000);

    // Below the axis clamps rather than running to minus infinity.
    CHECK(kPlot.xAtFrequency(1.0) == 0);
    CHECK(kPlot.frequencyAtX(-50) == Approx(20.0));

    // Round trip, to the pixel, at three places on the axis.
    for (const double hz : {50.0, 1000.0, 11000.0}) {
        const int column = kPlot.xAtFrequency(hz);
        CHECK(kPlot.frequencyAtX(column) == Approx(hz).epsilon(0.005));
    }
}

TEST_CASE("The level axis puts one decibel of EQ on one decibel of spectrum", "[ui][eq]") {
    // 1080 rows spanning 0 to -108 dBFS: ten rows to the decibel.
    CHECK(kPlot.yAtLevel(0.0) == 0);
    CHECK(kPlot.yAtLevel(-108.0) == 1080);
    CHECK(kPlot.yAtLevel(-54.0) == 540);
    CHECK(kPlot.levelAtY(270) == Approx(-27.0));

    // Gain is the same ruler, offset so that 0 dB sits on kEqZeroLevelDb. A
    // 6 dB boost is therefore sixty rows tall, exactly as tall as 6 dB of
    // spectrum, which is the entire point of drawing them together.
    CHECK(kPlot.yAtGain(0.0) == kPlot.yAtLevel(kEqZeroLevelDb));
    CHECK(kPlot.yAtGain(0.0) - kPlot.yAtGain(6.0) == 60);
    CHECK(kPlot.yAtGain(-6.0) - kPlot.yAtGain(0.0) == 60);
    CHECK(kPlot.gainAtY(kPlot.yAtGain(6.0)) == Approx(6.0));
    CHECK(kPlot.gainAtY(kPlot.yAtGain(-11.0)) == Approx(-11.0));
}

TEST_CASE("A curve with no bands is flat", "[ui][eq]") {
    const EqCurve curve = makeCurve();
    CHECK(curve.bandCount() == 0);
    CHECK(curve.isFlat());
    CHECK(curve.band(0) == nullptr);
    for (const double hz : {20.0, 200.0, 2000.0, 20000.0}) {
        CHECK(curve.responseDbAt(hz) == Approx(0.0).margin(1e-12));
    }

    const std::vector<double> across = curve.responseAcross(kPlot);
    REQUIRE(across.size() == 1000);
    CHECK(across.front() == Approx(0.0).margin(1e-12));
    CHECK(across.back() == Approx(0.0).margin(1e-12));
}

TEST_CASE("One bell is its gain at the centre and half of it at the edges", "[ui][eq]") {
    EqCurve curve = makeCurve();
    REQUIRE(curve.addBand(1000.0, 12.0, 2.0) == 0);
    CHECK_FALSE(curve.isFlat());

    // At the centre the cookbook peaking section is exactly A^2, where
    // A = 10^(gain/40) -- see peakingDb above -- so 20 log10(A^2) is the gain
    // asked for, to the last bit and not approximately.
    CHECK(curve.responseDbAt(1000.0) == Approx(12.0).margin(1e-9));

    // Either side, at the two frequencies where the response is half the gain
    // in decibels. These are derived from the transfer function rather than
    // measured off it, and they are not symmetric in hertz: 781.2 and 1279.6
    // about a 1000 Hz centre.
    const auto [lower, upper] = halfGainFrequencies(1000.0, 2.0);
    CHECK(lower == Approx(781.2).epsilon(0.001));
    CHECK(upper == Approx(1279.6).epsilon(0.001));
    CHECK(curve.responseDbAt(lower) == Approx(6.0).margin(1e-9));
    CHECK(curve.responseDbAt(upper) == Approx(6.0).margin(1e-9));

    // And everywhere else, against the same derivation.
    for (const double hz : {50.0, 250.0, 700.0, 1500.0, 4000.0, 16000.0}) {
        CHECK(curve.responseDbAt(hz) == Approx(peakingDb(1000.0, 2.0, 12.0, hz)).margin(1e-9));
    }

    // Two octaves down, a Q of 2 has all but let go. From the analogue
    // prototype at Omega = f/f0 = 0.25, with A = 10^(12/40) = 1.99526:
    //   |H|^2 = ((1-0.0625)^2 + A^2 (0.0625)/4) / ((1-0.0625)^2 + 0.0625/(4A^2))
    //         = (0.87891 + 0.06221) / (0.87891 + 0.00392) = 1.06603,
    // which is 0.2776 dB. The digital filter reads a shade under that, at
    // 0.2768, because the bilinear transform measures the distance from the
    // centre in tan(w/2) rather than in hertz: tan(pi 250/48000) /
    // tan(pi 1000/48000) is 0.24966, not 0.25, so 250 Hz is fractionally
    // further out than an octave count suggests.
    CHECK(curve.responseDbAt(250.0) == Approx(0.2768).epsilon(0.001));
}

TEST_CASE("A cut is the mirror of the boost that undoes it", "[ui][eq]") {
    EqCurve boost = makeCurve();
    EqCurve cut = makeCurve();
    REQUIRE(boost.addBand(3000.0, 8.0, 1.5) == 0);
    REQUIRE(cut.addBand(3000.0, -8.0, 1.5) == 0);

    // Not a tautology about the sign of a parameter: the cookbook cut is a
    // different filter, with A inverted through the numerator and denominator
    // rather than a boost with a minus in front of it.
    for (const double hz : {100.0, 1500.0, 3000.0, 6000.0, 18000.0}) {
        CHECK(boost.responseDbAt(hz) == Approx(-cut.responseDbAt(hz)).margin(1e-9));
    }
    CHECK(cut.responseDbAt(3000.0) == Approx(-8.0).margin(1e-9));
}

TEST_CASE("Overlapping bands sum in decibels the way the filters cascade", "[ui][eq]") {
    EqCurve curve = makeCurve();
    REQUIRE(curve.addBand(1000.0, 6.0, 1.0) == 0);
    REQUIRE(curve.addBand(1000.0, 6.0, 1.0) == 1);

    // Two sections in series multiply their magnitudes, so their decibels add.
    // Each is exactly +6 dB at its centre, so both together are exactly +12 --
    // the number an EQ that drew only the individual bells would hide.
    CHECK(curve.responseDbAt(1000.0) == Approx(12.0).margin(1e-9));

    EqCurve spread = makeCurve();
    REQUIRE(spread.addBand(1000.0, 6.0, 2.0) == 0);
    // The second band is centred on the first one's upper half-gain point, so
    // at that frequency the first contributes exactly 3 dB and the second
    // exactly 6: nine decibels, derived rather than measured.
    const double upper = halfGainFrequencies(1000.0, 2.0).second;
    REQUIRE(spread.addBand(upper, 6.0, 2.0) == 1);
    CHECK(spread.responseDbAt(upper) == Approx(9.0).margin(1e-9));

    // And the sum is the sum of the parts everywhere, not only where it was
    // contrived to be.
    for (const double hz : {60.0, 400.0, 1000.0, 2500.0, 12000.0}) {
        const double parts = spread.bandResponseDbAt(0, hz) + spread.bandResponseDbAt(1, hz);
        CHECK(spread.responseDbAt(hz) == Approx(parts).margin(1e-9));
    }

    // A boost and a matching cut on top of each other cancel to a wire.
    EqCurve cancelling = makeCurve();
    REQUIRE(cancelling.addBand(500.0, 9.0, 1.2) == 0);
    REQUIRE(cancelling.addBand(500.0, -9.0, 1.2) == 1);
    for (const double hz : {100.0, 500.0, 2000.0}) {
        CHECK(cancelling.responseDbAt(hz) == Approx(0.0).margin(1e-9));
    }
}

TEST_CASE("The drawn curve is the response at every column", "[ui][eq]") {
    EqCurve curve = makeCurve();
    REQUIRE(curve.addBand(800.0, -5.0, 3.0) == 0);
    REQUIRE(curve.addBand(6000.0, 7.0, 0.8) == 1);

    const std::vector<double> across = curve.responseAcross(kPlot);
    REQUIRE(across.size() == static_cast<std::size_t>(kPlot.width));
    for (int column = 0; column < kPlot.width; column += 97) {
        const double hz = axisFrequency(static_cast<double>(column) / kPlot.width);
        CHECK(across[static_cast<std::size_t>(column)] ==
              Approx(curve.responseDbAt(hz)).margin(1e-9));
    }
}

TEST_CASE("A handle is hit within its radius and missed outside it", "[ui][eq]") {
    EqCurve curve = makeCurve();
    REQUIRE(curve.addBand(1000.0, 6.0, 1.0) == 0);

    const int x = curve.handleX(kPlot, 0);
    const int y = curve.handleY(kPlot, 0);
    // 1000 Hz is log(50)/log(1200) = 0.551760 across, so column 552; +6 dB is
    // sixty rows above the 540 of the zero line.
    CHECK(x == 552);
    CHECK(y == 480);

    CHECK(curve.handleAt(kPlot, x, y) == 0);
    CHECK(curve.handleAt(kPlot, x + EqCurve::kHandleRadius, y) == 0);
    CHECK(curve.handleAt(kPlot, x, y - EqCurve::kHandleRadius) == 0);
    CHECK(curve.handleAt(kPlot, x + EqCurve::kHandleRadius + 1, y) == -1);
    // Diagonally, 8 and 8 is 11.3 away, which is outside a radius of 10 even
    // though neither component is.
    CHECK(curve.handleAt(kPlot, x + 8, y + 8) == -1);
    CHECK(curve.handleAt(kPlot, 10, 10) == -1);
}

TEST_CASE("The nearest handle takes the click, and the later one takes a tie", "[ui][eq]") {
    EqCurve curve = makeCurve();
    REQUIRE(curve.addBand(1000.0, 0.0, 1.0) == 0);
    REQUIRE(curve.addBand(1100.0, 0.0, 1.0) == 1);

    const int first = curve.handleX(kPlot, 0);
    const int second = curve.handleX(kPlot, 1);
    const int row = curve.handleY(kPlot, 0);
    REQUIRE(second > first);

    CHECK(curve.handleAt(kPlot, first, row) == 0);
    CHECK(curve.handleAt(kPlot, second, row) == 1);

    // A band dropped exactly on another has to be the one that comes back up,
    // or it is stranded under a handle the user cannot see past.
    EqCurve stacked = makeCurve();
    REQUIRE(stacked.addBand(2000.0, 3.0, 1.0) == 0);
    REQUIRE(stacked.addBand(2000.0, 3.0, 1.0) == 1);
    CHECK(stacked.handleAt(kPlot, stacked.handleX(kPlot, 0), stacked.handleY(kPlot, 0)) == 1);
}

TEST_CASE("A drag converts to hertz and decibels", "[ui][eq]") {
    EqCurve curve = makeCurve();
    REQUIRE(curve.addBand(1000.0, 0.0, 1.4) == 0);
    const int from = curve.handleX(kPlot, 0);
    REQUIRE(from == 552);

    // A hundred columns to the right is a tenth of the axis, and the axis is
    // 1200:1 over its width, so the frequency multiplies by 1200^0.1 = 2.0245.
    // Absolutely: column 652 of 1000 is 20 * 1200^0.652.
    REQUIRE(curve.moveHandleTo(kPlot, 0, from + 100, kPlot.yAtGain(0.0)));
    CHECK(curve.band(0)->filter.frequency == Approx(axisFrequency(0.652)).epsilon(1e-9));
    CHECK(curve.band(0)->filter.frequency == Approx(2035.36).epsilon(0.001));

    // Sixty rows up is six decibels on a ten-rows-to-the-decibel axis.
    REQUIRE(curve.moveHandleTo(kPlot, 0, from + 100, kPlot.yAtGain(0.0) - 60));
    CHECK(curve.band(0)->filter.gainDb == Approx(6.0).margin(1e-9));
    // And down the other way.
    REQUIRE(curve.moveHandleTo(kPlot, 0, from + 100, kPlot.yAtGain(0.0) + 125));
    CHECK(curve.band(0)->filter.gainDb == Approx(-12.5).margin(1e-9));

    // A drag is a drag: it does not quietly reshape the band it moves.
    CHECK(curve.band(0)->filter.q == Approx(1.4));
}

TEST_CASE("A drag off the plot parks a band at the edge", "[ui][eq]") {
    EqCurve curve = makeCurve();
    REQUIRE(curve.addBand(1000.0, 0.0, 1.0) == 0);

    REQUIRE(curve.moveHandleTo(kPlot, 0, -400, -400));
    CHECK(curve.band(0)->filter.frequency == Approx(kLogAxisMinimumHz));
    CHECK(curve.band(0)->filter.gainDb == Approx(EqCurve::kGainLimitDb));

    REQUIRE(curve.moveHandleTo(kPlot, 0, 4000, 4000));
    // The ceiling keeps a band clear of Nyquist, where the peaking section
    // degenerates into a wire and a handle would sit on the edge doing nothing.
    CHECK(curve.band(0)->filter.frequency ==
          Approx(kRate * 0.5 * EqCurve::kFrequencyCeilingFraction));
    CHECK(curve.band(0)->filter.gainDb == Approx(-EqCurve::kGainLimitDb));

    CHECK_FALSE(curve.moveHandleTo(kPlot, 3, 500, 500));
}

TEST_CASE("A rate no audio file would use is refused, not blundered through", "[ui][eq]") {
    // SampleRate accepts anything positive, so the ceiling on a band's
    // frequency can in principle fall below the bottom of the axis. It is
    // held at the axis instead, and a band that then sits on Nyquist is one
    // the designer will not build -- a refusal rather than a clamp with its
    // bounds crossed over.
    EqCurve curve{SampleRate{40.0}};
    CHECK(curve.addBand(1000.0, 6.0, 1.0) == -1);
    CHECK(curve.bandCount() == 0);
}

TEST_CASE("The wheel multiplies Q and stops at the limits", "[ui][eq]") {
    EqCurve curve = makeCurve();
    REQUIRE(curve.addBand(1000.0, 3.0, 2.0) == 0);

    // Forward narrows: three notches is 1.15^3 = 1.520875 times the Q.
    REQUIRE(curve.adjustQByNotches(0, 3.0));
    CHECK(curve.band(0)->filter.q == Approx(2.0 * std::pow(1.15, 3.0)));

    // Back again returns it, because the step is a ratio rather than an
    // amount: 1.15^3 * 1.15^-3 is one.
    REQUIRE(curve.adjustQByNotches(0, -3.0));
    CHECK(curve.band(0)->filter.q == Approx(2.0));

    // Neither the frequency nor the gain moves with it.
    CHECK(curve.band(0)->filter.frequency == Approx(1000.0));
    CHECK(curve.band(0)->filter.gainDb == Approx(3.0));

    REQUIRE(curve.adjustQByNotches(0, 400.0));
    CHECK(curve.band(0)->filter.q == Approx(EqCurve::kMaximumQ));
    REQUIRE(curve.adjustQByNotches(0, -400.0));
    CHECK(curve.band(0)->filter.q == Approx(EqCurve::kMinimumQ));

    CHECK_FALSE(curve.adjustQByNotches(7, 1.0));
}

TEST_CASE("A held drag sets Q from where it started", "[ui][eq]") {
    EqCurve curve = makeCurve();
    REQUIRE(curve.addBand(1000.0, 0.0, 2.0) == 0);

    // Sixty pixels up is one doubling, and up narrows, to agree with the wheel.
    REQUIRE(curve.setQFromDrag(0, 2.0, -static_cast<int>(EqCurve::kQPixelsPerDoubling)));
    CHECK(curve.band(0)->filter.q == Approx(4.0));
    REQUIRE(curve.setQFromDrag(0, 2.0, static_cast<int>(EqCurve::kQPixelsPerDoubling)));
    CHECK(curve.band(0)->filter.q == Approx(1.0));

    // Anchored, not accumulated: the same distance from the same start is the
    // same Q however many times the pointer has moved to get there.
    REQUIRE(curve.setQFromDrag(0, 2.0, -30));
    const double halfway = curve.band(0)->filter.q;
    REQUIRE(curve.setQFromDrag(0, 2.0, -120));
    REQUIRE(curve.setQFromDrag(0, 2.0, -30));
    CHECK(curve.band(0)->filter.q == Approx(halfway));
    CHECK(halfway == Approx(2.0 * std::sqrt(2.0)));
}

TEST_CASE("Bands are added, removed and renumbered", "[ui][eq]") {
    EqCurve curve = makeCurve();
    REQUIRE(curve.addBand(100.0, 2.0, 1.0) == 0);
    REQUIRE(curve.addBand(1000.0, 4.0, 1.0) == 1);
    REQUIRE(curve.addBand(10000.0, 6.0, 1.0) == 2);
    CHECK(curve.bandCount() == 3);
    CHECK(curve.bands().size() == 3);

    REQUIRE(curve.removeBand(1));
    CHECK(curve.bandCount() == 2);
    CHECK(curve.band(0)->filter.frequency == Approx(100.0));
    CHECK(curve.band(1)->filter.frequency == Approx(10000.0));

    CHECK_FALSE(curve.removeBand(-1));
    CHECK_FALSE(curve.removeBand(2));

    curve.clear();
    CHECK(curve.bandCount() == 0);
    CHECK(curve.isFlat());

    // The EQ is bounded because the cascade behind it is. Filling it is a
    // refusal, not a silent drop of the seventeenth band.
    for (int index = 0; index < dsp::ParametricEq::kMaxBands; ++index) {
        REQUIRE(curve.addBand(1000.0, 1.0, 1.0) == index);
    }
    CHECK(curve.addBand(1000.0, 1.0, 1.0) == -1);
}

TEST_CASE("Bands at 0 dB leave the curve flat", "[ui][eq]") {
    EqCurve curve = makeCurve();
    REQUIRE(curve.addBand(1000.0, 0.0, 1.0) == 0);
    REQUIRE(curve.addBand(4000.0, 0.0, 1.0) == 1);
    CHECK(curve.bandCount() == 2);
    // Two bands that would cost an undo step and change nothing.
    CHECK(curve.isFlat());
    CHECK(curve.responseDbAt(1000.0) == Approx(0.0).margin(1e-12));

    REQUIRE(curve.setBand(1, 4000.0, 0.5, 1.0));
    CHECK_FALSE(curve.isFlat());
}

TEST_CASE("A change of sample rate keeps the bands in hertz", "[ui][eq]") {
    EqCurve curve{SampleRate{48000.0}};
    REQUIRE(curve.addBand(1000.0, 6.0, 1.0) == 0);
    REQUIRE(curve.addBand(18000.0, -4.0, 2.0) == 1);

    REQUIRE(curve.setSampleRate(SampleRate{44100.0}));
    CHECK(curve.sampleRate() == SampleRate{44100.0});
    CHECK(curve.band(0)->filter.frequency == Approx(1000.0));
    // Band 0 alone is still exactly its gain at its centre, now designed for
    // the new rate. The summed curve reads a whisker under six, because the
    // 18 kHz band's skirt reaches all the way down here: -0.000465 dB at
    // 1 kHz, which is the sum doing its job rather than a design that missed.
    CHECK(curve.bandResponseDbAt(0, 1000.0) == Approx(6.0).margin(1e-9));
    CHECK(curve.responseDbAt(1000.0) < 6.0);
    CHECK(curve.responseDbAt(1000.0) == Approx(6.0).margin(1e-3));

    // A band that no longer fits comes down to the ceiling rather than being
    // dropped or taking the whole change down with it.
    REQUIRE(curve.setSampleRate(SampleRate{32000.0}));
    CHECK(curve.band(1)->filter.frequency == Approx(16000.0 * EqCurve::kFrequencyCeilingFraction));
    CHECK(curve.band(0)->filter.frequency == Approx(1000.0));

    CHECK_FALSE(curve.setSampleRate(SampleRate{0.0}));
    CHECK(curve.sampleRate() == SampleRate{32000.0});
}

TEST_CASE("A band says what it is in numbers", "[ui][eq]") {
    EqCurve curve = makeCurve();
    REQUIRE(curve.addBand(1000.0, 6.0, std::sqrt(2.0)) == 0);
    CHECK(curve.describe(0) == std::string{"1.00 kHz   +6.0 dB   Q 1.41"});
    CHECK(curve.summarise() == std::string{"+6.0 dB at 1.00 kHz"});

    REQUIRE(curve.setBand(0, 315.0, -3.25, 4.0));
    // A cut carries its sign, because "3.2 dB at 315" is not a setting anyone
    // can act on without knowing which way.
    CHECK(curve.describe(0) == std::string{"315 Hz   -3.2 dB   Q 4.00"});

    REQUIRE(curve.addBand(4000.0, 2.0, 1.0) == 1);
    CHECK(curve.summarise() == std::string{"2 bands"});

    CHECK(curve.describe(9).empty());
    curve.clear();
    CHECK(curve.summarise() == std::string{"no bands"});
}
