#include <sa/analysis/LoudnessContour.h>
#include <sa/analysis/LoudnessMeter.h>
#include <sa/analysis/TruePeakMeter.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>
#include <optional>

using namespace sa;
using namespace sa::analysis;
using Catch::Approx;

namespace {

/// Analytic constants every assertion below is derived from, written out so a
/// failure says which piece of arithmetic the contour has broken.
///
/// kSineCrestDb is 20 * log10(sqrt(2)) = 10 * log10(2): the ratio of a sine's
/// peak to its RMS. It is also, exactly, the amount by which BS.1770 reads a
/// mono sine below its own peak level, which is why it turns up twice here --
/// once as a crest factor and once as a mono sine's PLR.
constexpr double kSineCrestDb = 3.010299956639812;

SampleCount framesFor(double seconds, SampleRate rate) {
    return secondsToSamples(seconds, rate);
}

void addSine(AudioBuffer& buffer, int channel, double frequency, double amplitude, SampleRate rate,
             SampleCount start, SampleCount count) {
    float* samples = buffer.channel(channel);
    for (SampleCount i = 0; i < count; ++i) {
        const SampleCount index = start + i;
        if (index < 0 || index >= buffer.frames()) {
            continue;
        }
        // Phase runs from the absolute sample index, not from `start`, so a
        // level change part-way through a buffer is a change of amplitude only
        // -- the waveform stays continuous and no click is introduced that the
        // meter would have to be asked to ignore.
        const double phase =
            2.0 * std::numbers::pi * frequency * static_cast<double>(index) / rate.hz();
        samples[index] += static_cast<float>(amplitude * std::sin(phase));
    }
}

void fillSine(AudioBuffer& buffer, int channel, double frequency, double amplitude,
              SampleRate rate) {
    addSine(buffer, channel, frequency, amplitude, rate, 0, buffer.frames());
}

/// A square wave of exactly `frequency`, built from the sign of the sine so the
/// two fixtures share a phase and a period.
void fillSquare(AudioBuffer& buffer, int channel, double frequency, double amplitude,
                SampleRate rate) {
    float* samples = buffer.channel(channel);
    for (SampleCount i = 0; i < buffer.frames(); ++i) {
        const double phase =
            2.0 * std::numbers::pi * frequency * static_cast<double>(i) / rate.hz();
        samples[i] = static_cast<float>(std::sin(phase) < 0.0 ? -amplitude : amplitude);
    }
}

LoudnessContour contourOrFail(const AudioBuffer& buffer, SampleRate rate,
                              double intervalSeconds = kDefaultContourIntervalSeconds) {
    auto result =
        measureLoudnessContour(buffer.constView(), rate, buffer.layout(), intervalSeconds);
    REQUIRE(result.hasValue());
    return std::move(result).value();
}

LoudnessMeasurement measureOrFail(const AudioBuffer& buffer, SampleRate rate) {
    auto result = LoudnessMeter::measure(buffer.constView(), rate, buffer.layout());
    REQUIRE(result.hasValue());
    return std::move(result).value();
}

/// Index of the point whose timestamp is `seconds`, on a contour sampled every
/// kDefaultContourIntervalSeconds. Integer arithmetic, because comparing
/// accumulated times to a literal is the one way this could go wrong silently.
std::size_t pointIndexAt(double seconds) {
    return static_cast<std::size_t>(std::lround(seconds / kDefaultContourIntervalSeconds));
}

/// Time of the first point at or after `from` whose short-term (or momentary)
/// reading has settled within `toleranceDb` of `target`, or -1.
double settleTime(const LoudnessContour& contour, bool momentary, double target, double toleranceDb,
                  double from) {
    for (const LoudnessPoint& point : contour.points) {
        if (point.timeSeconds < from) {
            continue;
        }
        const std::optional<double>& reading =
            momentary ? point.momentaryLufs : point.shortTermLufs;
        if (reading && std::abs(*reading - target) <= toleranceDb) {
            return point.timeSeconds;
        }
    }
    return -1.0;
}

} // namespace

// --- Agreement with the meter it is built on --------------------------------

TEST_CASE("A steady tone gives a flat contour at its own level", "[analysis][contour]") {
    // EBU Tech 3341 case 1 again, but as a series: a 1 kHz stereo sine at
    // -23 dBFS reads -23 LUFS, so every point of the contour that has a reading
    // at all must read -23 and none may wander. Rebuilt from the description,
    // not from the official WAV set, which is not in the repository -- passing
    // this is not conformance, it is one point of the curve being right.
    const SampleRate rate = kSampleRate48000;
    const double amplitude = std::pow(10.0, -23.0 / 20.0);

    AudioBuffer buffer{ChannelLayout::stereo(), framesFor(10.0, rate)};
    fillSine(buffer, 0, 1000.0, amplitude, rate);
    fillSine(buffer, 1, 1000.0, amplitude, rate);

    const LoudnessContour contour = contourOrFail(buffer, rate);

    int momentaryPoints = 0;
    int shortTermPoints = 0;
    for (const LoudnessPoint& point : contour.points) {
        if (point.momentaryLufs) {
            CHECK(*point.momentaryLufs == Approx(-23.0).margin(0.1));
            ++momentaryPoints;
        }
        if (point.shortTermLufs) {
            CHECK(*point.shortTermLufs == Approx(-23.0).margin(0.1));
            ++shortTermPoints;
        }
    }

    // 10 s at 0.1 s is 101 points, numbered 0..100. Momentary appears from
    // point 4 (400 ms), short-term from point 30 (3 s).
    CHECK(contour.points.size() == 101);
    CHECK(momentaryPoints == 97);
    CHECK(shortTermPoints == 71);
}

TEST_CASE("The contour's integrated figure is the meter's, bit for bit", "[analysis][contour]") {
    // The test that decides whether any of the rest can be trusted. The contour
    // does not integrate anything itself -- it drives a LoudnessMeter and
    // reports what that meter holds -- so this is an equality, not a tolerance.
    // If it ever became approximate, there would be two implementations of
    // BS.1770 in the tree and no way to say which was right.
    const SampleRate rate = kSampleRate48000;

    AudioBuffer buffer{ChannelLayout::stereo(), framesFor(12.34, rate)};
    fillSine(buffer, 0, 997.0, 0.25, rate);
    fillSine(buffer, 1, 1310.0, 0.18, rate);
    // A 10 dB drop part-way through, so the gating has something to do and the
    // two paths have to agree about which blocks survive it.
    addSine(buffer, 0, 997.0, 0.25 * (1.0 / std::sqrt(10.0) - 1.0), rate, framesFor(6.0, rate),
            buffer.frames() - framesFor(6.0, rate));

    const LoudnessMeasurement measurement = measureOrFail(buffer, rate);
    const LoudnessContour contour = contourOrFail(buffer, rate);

    // The buffer is deliberately 12.34 s -- not a whole number of sub-blocks --
    // so the trailing partial block is on the path being compared.
    CHECK(contour.integratedLufs == measurement.integratedLufs);
    CHECK(contour.loudnessRangeLu == measurement.loudnessRangeLu);
    CHECK(contour.gatedBlockCount == measurement.gatedBlockCount);
    CHECK(contour.integratedLufs > kDecibelFloor);

    // At the default 0.1 s the contour lands on every sub-block boundary, so it
    // sees every short-term value the meter ever held -- the loudest of them is
    // the meter's own Max S, exactly.
    REQUIRE(contour.loudestShortTermLufs.has_value());
    CHECK(*contour.loudestShortTermLufs == measurement.maximumShortTermLufs);
}

TEST_CASE("A contour reading is the meter's reading at that instant", "[analysis][contour]") {
    // Stronger than agreeing on the summary: the value at 400 ms is the value a
    // LoudnessMeter holds after exactly 400 ms of the same audio, and likewise
    // at 3 s. Equality again, for the same reason.
    const SampleRate rate = kSampleRate48000;
    const auto layout = ChannelLayout::mono();

    AudioBuffer buffer{layout, framesFor(6.0, rate)};
    fillSine(buffer, 0, 1000.0, 0.3, rate);

    const LoudnessContour contour = contourOrFail(buffer, rate);

    auto meterResult = LoudnessMeter::create(rate, layout);
    REQUIRE(meterResult.hasValue());
    LoudnessMeter& meter = meterResult.value();

    meter.process(buffer.constView().subRange(0, framesFor(0.4, rate)));
    REQUIRE(contour.points[pointIndexAt(0.4)].momentaryLufs.has_value());
    CHECK(*contour.points[pointIndexAt(0.4)].momentaryLufs == meter.momentaryLufs());

    meter.process(buffer.constView().subRange(framesFor(0.4, rate), framesFor(2.6, rate)));
    REQUIRE(contour.points[pointIndexAt(3.0)].shortTermLufs.has_value());
    CHECK(*contour.points[pointIndexAt(3.0)].shortTermLufs == meter.shortTermLufs());
    CHECK(*contour.points[pointIndexAt(3.0)].momentaryLufs == meter.momentaryLufs());
}

TEST_CASE("The programme true peak is the true peak meter's own", "[analysis][contour]") {
    // The contour reads the true-peak meter in 100 ms segments so that each
    // window has its own peak. resetPeaks() keeps the interpolator's history
    // across those seams, so the maximum of the segments has to come out as the
    // maximum of one uninterrupted run. Bit-identical, or the seam is lossy.
    const SampleRate rate = kSampleRate48000;

    // 5.05 s is fifty whole sub-blocks and half of one more, and the loudest
    // material is in that trailing half -- so this also says the partial
    // sub-block reaches the peak meter rather than being dropped for producing
    // no loudness reading.
    AudioBuffer buffer{ChannelLayout::stereo(), framesFor(5.05, rate)};
    fillSine(buffer, 0, 997.0, 0.5, rate);
    // Near Nyquist, where the interpolator is doing the most work and a reset
    // delay line would show up most clearly.
    fillSine(buffer, 1, 19000.0, 0.7, rate);
    addSine(buffer, 0, 997.0, 0.4, rate, framesFor(5.0, rate), framesFor(0.05, rate));

    auto reference = TruePeakMeter::measureDbtp(buffer.constView());
    REQUIRE(reference.hasValue());

    const LoudnessContour contour = contourOrFail(buffer, rate);
    CHECK(contour.truePeakDbtp == reference.value());
    // The tail is where the 0.9 amplitude lives, so a dropped tail would read
    // 0.7 and this would be about 2 dB low.
    CHECK(contour.truePeakDbtp > amplitudeToDecibels(0.88));
}

// --- Undefined readings -----------------------------------------------------

TEST_CASE("Points before a full window report nothing at all", "[analysis][contour]") {
    // A short-term reading taken over half a second is not a short-term
    // reading. The empty optional is what makes that impossible to report by
    // accident -- kDecibelFloor could not carry it, because once a window does
    // exist the floor is a measurement meaning "that window was silent".
    const SampleRate rate = kSampleRate48000;

    AudioBuffer buffer{ChannelLayout::mono(), framesFor(5.0, rate)};
    fillSine(buffer, 0, 1000.0, 0.3, rate);

    const LoudnessContour contour = contourOrFail(buffer, rate);

    SECTION("the momentary boundary is 400 ms, not 399") {
        // Point 3 sits at 300 ms with 300 ms of audio behind it; point 4 sits at
        // 400 ms with exactly one block behind it.
        CHECK_FALSE(contour.points[pointIndexAt(0.3)].momentaryLufs.has_value());
        CHECK(contour.points[pointIndexAt(0.4)].momentaryLufs.has_value());
        for (std::size_t i = 0; i < pointIndexAt(0.4); ++i) {
            CHECK_FALSE(contour.points[i].momentaryLufs.has_value());
            CHECK_FALSE(contour.points[i].crestDb.has_value());
        }
    }

    SECTION("the short-term boundary is 3 s, not 2.9") {
        CHECK_FALSE(contour.points[pointIndexAt(2.9)].shortTermLufs.has_value());
        CHECK(contour.points[pointIndexAt(3.0)].shortTermLufs.has_value());
        for (std::size_t i = 0; i < pointIndexAt(3.0); ++i) {
            CHECK_FALSE(contour.points[i].shortTermLufs.has_value());
            // Everything that needs a short-term window goes with it.
            CHECK_FALSE(contour.points[i].truePeakDbtp.has_value());
            CHECK_FALSE(contour.points[i].psrDb.has_value());
        }
    }

    SECTION("an off-grid interval lands either side of the same boundary") {
        // 130 ms: no point falls on 400 ms, so this checks the boundary is a
        // property of how much audio is behind a point rather than of the
        // sampling grid. Point 3 is at 390 ms (no block yet), point 4 at 520 ms.
        const LoudnessContour offGrid = contourOrFail(buffer, rate, 0.13);
        CHECK(offGrid.points[3].timeSeconds == Approx(0.39).margin(1e-9));
        CHECK_FALSE(offGrid.points[3].momentaryLufs.has_value());
        CHECK(offGrid.points[4].timeSeconds == Approx(0.52).margin(1e-9));
        CHECK(offGrid.points[4].momentaryLufs.has_value());
    }
}

TEST_CASE("A silent window is measured, not withheld", "[analysis][contour]") {
    // The other half of the distinction. Once 400 ms exist the reading is the
    // floor and the optional is engaged: the window was measured and found to
    // contain nothing. Collapsing this into "unavailable" would lose the fact
    // that the programme has silence in it.
    const SampleRate rate = kSampleRate48000;
    const AudioBuffer buffer{ChannelLayout::stereo(), framesFor(5.0, rate)};

    const LoudnessContour contour = contourOrFail(buffer, rate);
    const LoudnessPoint& late = contour.points[pointIndexAt(4.0)];

    REQUIRE(late.momentaryLufs.has_value());
    CHECK(*late.momentaryLufs == kDecibelFloor);
    REQUIRE(late.shortTermLufs.has_value());
    CHECK(*late.shortTermLufs == kDecibelFloor);
    REQUIRE(late.truePeakDbtp.has_value());
    CHECK(*late.truePeakDbtp == kDecibelFloor);
    // PSR is the one that must stay empty: floor minus floor is 0 dB, which
    // would read as the most brickwalled master ever made.
    CHECK_FALSE(late.psrDb.has_value());

    // Nothing cleared the absolute gate, so there is no quietest passage to
    // name -- silence is not a quiet passage.
    CHECK_FALSE(contour.quietestShortTermLufs.has_value());
    CHECK_FALSE(contour.loudestShortTermLufs.has_value());
    CHECK(contour.integratedLufs == kDecibelFloor);
    CHECK_FALSE(contour.plrDb.has_value());
}

// --- Timing -----------------------------------------------------------------

TEST_CASE("Momentary reacts to a step 2.6 s before short-term does", "[analysis][contour]") {
    // A 20 dB step up at exactly 10 s. The momentary window is 400 ms long, so
    // it is entirely past the step at 10.4 s; the short-term window is 3 s
    // long, so it is entirely past at 13.0 s. The difference is the difference
    // between the two windows, 2.6 s, and that is the whole distinction between
    // the two readings.
    const SampleRate rate = kSampleRate48000;
    const auto layout = ChannelLayout::mono();
    const SampleCount stepFrame = framesFor(10.0, rate);
    const double quiet = 0.02;
    const double loud = quiet * 10.0;

    AudioBuffer buffer{layout, framesFor(20.0, rate)};
    addSine(buffer, 0, 1000.0, quiet, rate, 0, stepFrame);
    addSine(buffer, 0, 1000.0, loud, rate, stepFrame, buffer.frames() - stepFrame);

    const LoudnessContour contour = contourOrFail(buffer, rate);

    // A mono sine at amplitude A reads 20*log10(A) - 3.01 LUFS: the K-weighting
    // gain at 1 kHz and the -0.691 offset cancel, leaving the sine's own crest.
    const double loudLufs = 20.0 * std::log10(loud) - kSineCrestDb;
    const double quietLufs = 20.0 * std::log10(quiet) - kSineCrestDb;
    CHECK(*contour.points[pointIndexAt(9.0)].momentaryLufs == Approx(quietLufs).margin(0.1));
    CHECK(*contour.points[pointIndexAt(15.0)].momentaryLufs == Approx(loudLufs).margin(0.1));

    // The timing assertions below run against the levels the contour itself
    // settles to rather than against the analytic ones, so that they measure
    // when a window fills and not how well the meter is calibrated -- the two
    // lines above are what tests the calibration.
    const double settled = *contour.points[pointIndexAt(15.0)].momentaryLufs;
    const double before = *contour.points[pointIndexAt(9.0)].momentaryLufs;

    const double momentarySettled = settleTime(contour, true, settled, 0.05, 10.0);
    const double shortTermSettled = settleTime(contour, false, settled, 0.05, 10.0);
    INFO("momentary settled at " << momentarySettled << " s, short-term at " << shortTermSettled);
    CHECK(momentarySettled == Approx(10.4).margin(0.05));
    CHECK(shortTermSettled == Approx(13.0).margin(0.05));
    CHECK(shortTermSettled - momentarySettled == Approx(2.6).margin(0.05));

    // And the size of the lag, not just its timing. At 10.4 s the 3 s window
    // holds 0.4 s at the loud level and 2.6 s at the quiet one, so its mean
    // power is (0.4*100 + 2.6*1)/3 = 14.2 times the quiet level against the
    // loud level's 100 -- 10*log10(14.2/100) = -8.48 dB below where momentary
    // already sits.
    const double lagDb = *contour.points[pointIndexAt(10.4)].shortTermLufs - settled;
    CHECK(lagDb == Approx(10.0 * std::log10(14.2 / 100.0)).margin(0.05));

    // Both readings move at the first window that touches the step, so what
    // separates them is settling time and nothing else. At 10.1 s momentary has
    // 0.1 s of the 0.4 s window past the step -- (0.1*100 + 0.3)/0.4 = 25.75
    // times the quiet power -- while short-term has 0.1 s of 3 s, which is
    // (0.1*100 + 2.9)/3 = 4.3 times.
    CHECK(*contour.points[pointIndexAt(10.1)].momentaryLufs - before ==
          Approx(10.0 * std::log10(25.75)).margin(0.02));
    CHECK(*contour.points[pointIndexAt(10.1)].shortTermLufs - before ==
          Approx(10.0 * std::log10(4.3)).margin(0.02));
}

// --- Dynamics arithmetic ----------------------------------------------------

TEST_CASE("PLR and PSR are the differences they are defined to be", "[analysis][contour]") {
    const SampleRate rate = kSampleRate48000;
    const double amplitude = std::pow(10.0, -23.0 / 20.0);

    SECTION("a mono sine's PLR is the sine's own crest factor") {
        // True peak is the amplitude, 20*log10(A) dBTP. A mono sine measures
        // 20*log10(A) - 3.01 LUFS. PLR is the difference: 3.01 dB, and it is
        // the same 3.01 dB as the crest factor because for one sine the two
        // questions are the same question.
        AudioBuffer buffer{ChannelLayout::mono(), framesFor(8.0, rate)};
        fillSine(buffer, 0, 1000.0, amplitude, rate);

        const LoudnessContour contour = contourOrFail(buffer, rate);
        CHECK(contour.truePeakDbtp == Approx(-23.0).margin(0.05));
        CHECK(contour.integratedLufs == Approx(-23.0 - kSineCrestDb).margin(0.05));
        REQUIRE(contour.plrDb.has_value());
        CHECK(*contour.plrDb == Approx(kSineCrestDb).margin(0.05));

        // PSR is the same subtraction against the short-term value, which for a
        // steady tone equals the integrated one, so every point reads 3.01 too.
        for (const LoudnessPoint& point : contour.points) {
            if (point.psrDb) {
                CHECK(*point.psrDb == Approx(kSineCrestDb).margin(0.05));
            }
        }
    }

    SECTION("stereo summation eats exactly that 3.01 dB") {
        // The same tone in two channels is twice the power and so 3.01 LU
        // louder, while the peak has not moved. PLR therefore falls to zero --
        // which is why a stereo -23 dBFS sine is the calibration signal it is.
        AudioBuffer buffer{ChannelLayout::stereo(), framesFor(8.0, rate)};
        fillSine(buffer, 0, 1000.0, amplitude, rate);
        fillSine(buffer, 1, 1000.0, amplitude, rate);

        const LoudnessContour contour = contourOrFail(buffer, rate);
        REQUIRE(contour.plrDb.has_value());
        CHECK(*contour.plrDb == Approx(0.0).margin(0.05));
    }

    SECTION("PSR is truePeakDbtp minus shortTermLufs at every point") {
        // Arithmetic identity on material whose two operands differ point by
        // point, so the check is on the subtraction rather than on a constant.
        AudioBuffer buffer{ChannelLayout::stereo(), framesFor(12.0, rate)};
        fillSine(buffer, 0, 1000.0, 0.4, rate);
        fillSine(buffer, 1, 1000.0, 0.4, rate);
        addSine(buffer, 0, 1000.0, -0.36, rate, framesFor(6.0, rate), framesFor(6.0, rate));
        addSine(buffer, 1, 1000.0, -0.36, rate, framesFor(6.0, rate), framesFor(6.0, rate));

        const LoudnessContour contour = contourOrFail(buffer, rate);
        int checked = 0;
        for (const LoudnessPoint& point : contour.points) {
            if (!point.psrDb) {
                continue;
            }
            CHECK(*point.psrDb == *point.truePeakDbtp - *point.shortTermLufs);
            ++checked;
        }
        CHECK(checked > 80);

        // A window that still holds the loud passage's peak while its loudness
        // has already fallen reads a much higher PSR, and that is the behaviour
        // which makes the series worth plotting at all.
        //
        // At 8.9 s the 3 s window runs from 5.9 s, so it keeps the last 0.1 s
        // of the loud passage -- and with it that passage's peak -- over 2.9 s
        // of the quiet one. Mean power is (0.1*0.4^2 + 2.9*0.04^2)/3 against the
        // loud passage's 0.4^2, a ratio of 0.043, so the short-term value has
        // dropped 13.67 dB while the peak has not moved at all.
        const double steady = *contour.points[pointIndexAt(5.5)].psrDb;
        const double straddling = *contour.points[pointIndexAt(8.9)].psrDb;
        INFO("PSR " << steady << " dB steady, " << straddling << " dB across the drop");
        CHECK(straddling - steady == Approx(10.0 * std::log10(0.16 / 0.00688)).margin(0.02));

        // By 9.5 s the window has slid clear of the loud passage entirely, peak
        // and loudness have fallen together, and PSR is back where it started.
        CHECK(*contour.points[pointIndexAt(9.5)].psrDb == Approx(steady).margin(0.01));
    }
}

TEST_CASE("Crest factor is 3.01 dB for a sine and 0 dB for a square", "[analysis][contour]") {
    // Both derived, not measured. A sine's peak is sqrt(2) times its RMS, so
    // 20*log10(sqrt(2)) = 3.0103 dB. A square wave is at its peak for every
    // sample, so its RMS *is* its peak and the ratio is exactly 1, or 0 dB.
    //
    // At 1 kHz and 48 kHz both fixtures fit 400 whole cycles into the 400 ms
    // window, so neither figure is approximate for want of a partial cycle --
    // which is also why crest is taken from the sample peak rather than the
    // true peak: a square wave's reconstruction overshoots between samples, so
    // a true-peak crest would not be 0 dB and would not be what anyone means.
    const SampleRate rate = kSampleRate48000;

    SECTION("sine") {
        AudioBuffer buffer{ChannelLayout::mono(), framesFor(4.0, rate)};
        fillSine(buffer, 0, 1000.0, 0.5, rate);

        const LoudnessContour contour = contourOrFail(buffer, rate);
        int checked = 0;
        for (const LoudnessPoint& point : contour.points) {
            if (point.crestDb) {
                CHECK(*point.crestDb == Approx(kSineCrestDb).margin(0.001));
                ++checked;
            }
        }
        CHECK(checked == 37);
    }

    SECTION("square") {
        AudioBuffer buffer{ChannelLayout::mono(), framesFor(4.0, rate)};
        fillSquare(buffer, 0, 1000.0, 0.5, rate);

        const LoudnessContour contour = contourOrFail(buffer, rate);
        for (const LoudnessPoint& point : contour.points) {
            if (point.crestDb) {
                CHECK(*point.crestDb == Approx(0.0).margin(1e-9));
            }
        }
    }

    SECTION("a square wave's true peak overshoots, which is why crest is not") {
        // Stated as a test rather than as a comment, because it is the whole
        // reason the two figures use different peaks.
        AudioBuffer buffer{ChannelLayout::mono(), framesFor(4.0, rate)};
        fillSquare(buffer, 0, 1000.0, 0.5, rate);

        const LoudnessContour contour = contourOrFail(buffer, rate);
        CHECK(contour.truePeakDbtp > amplitudeToDecibels(0.5));
    }
}

// --- Summary statistics -----------------------------------------------------

TEST_CASE("The quietest and loudest short-term values bracket the passages",
          "[analysis][contour]") {
    // Twenty seconds at one level, twenty exactly 10 dB below. A mono sine at
    // amplitude A reads 20*log10(A) - 3.01 LUFS, so the two passages sit at
    // -16.99 and -26.99 LUFS and the extremes of the contour are those two
    // numbers -- the windows straddling the change fall between them and so
    // cannot become either extreme.
    const SampleRate rate = kSampleRate48000;
    const SampleCount half = framesFor(20.0, rate);
    const double loud = 0.2;
    const double quiet = loud / std::sqrt(10.0);

    AudioBuffer buffer{ChannelLayout::mono(), half * 2};
    addSine(buffer, 0, 1000.0, loud, rate, 0, half);
    addSine(buffer, 0, 1000.0, quiet, rate, half, half);

    const LoudnessContour contour = contourOrFail(buffer, rate);

    const double loudLufs = 20.0 * std::log10(loud) - kSineCrestDb;   // -16.9897
    const double quietLufs = 20.0 * std::log10(quiet) - kSineCrestDb; // -26.9897
    REQUIRE(contour.loudestShortTermLufs.has_value());
    REQUIRE(contour.quietestShortTermLufs.has_value());
    CHECK(*contour.loudestShortTermLufs == Approx(loudLufs).margin(0.05));
    CHECK(*contour.quietestShortTermLufs == Approx(quietLufs).margin(0.05));
    CHECK(*contour.loudestShortTermLufs - *contour.quietestShortTermLufs ==
          Approx(10.0).margin(0.05));

    // Roughly half the programme sits at each level, so the 10th percentile is
    // in the quiet half and the 95th in the loud one, and their spread is the
    // step. This is not LRA -- no relative gate, and every point counts rather
    // than one a second -- but on material this simple the two agree.
    REQUIRE(contour.shortTermPercentile10Lufs.has_value());
    REQUIRE(contour.shortTermPercentile95Lufs.has_value());
    CHECK(*contour.shortTermPercentile10Lufs == Approx(quietLufs).margin(0.2));
    CHECK(*contour.shortTermPercentile95Lufs == Approx(loudLufs).margin(0.2));
    CHECK(contour.loudnessRangeLu == Approx(10.0).margin(0.5));
}

TEST_CASE("A steady tone has no spread between its percentiles", "[analysis][contour]") {
    const SampleRate rate = kSampleRate48000;

    AudioBuffer buffer{ChannelLayout::mono(), framesFor(20.0, rate)};
    fillSine(buffer, 0, 1000.0, 0.2, rate);

    const LoudnessContour contour = contourOrFail(buffer, rate);
    REQUIRE(contour.shortTermPercentile95Lufs.has_value());
    CHECK(*contour.shortTermPercentile95Lufs - *contour.shortTermPercentile10Lufs < 0.05);
    CHECK(*contour.loudestShortTermLufs - *contour.quietestShortTermLufs < 0.05);
}

// --- Sampling grid ----------------------------------------------------------

TEST_CASE("The timestamp grid is exact and does not drift", "[analysis][contour]") {
    const SampleRate rate = kSampleRate48000;

    AudioBuffer buffer{ChannelLayout::mono(), framesFor(30.0, rate)};
    fillSine(buffer, 0, 1000.0, 0.2, rate);

    const LoudnessContour contour = contourOrFail(buffer, rate, 0.25);
    CHECK(contour.intervalSeconds == Approx(0.25).margin(1e-12));
    REQUIRE(contour.points.size() == 121); // 30 s / 0.25 s, plus the point at 0
    for (std::size_t i = 0; i < contour.points.size(); ++i) {
        CHECK(contour.points[i].timeSeconds == Approx(0.25 * static_cast<double>(i)).margin(1e-12));
    }
    CHECK(contour.points.back().timeSeconds == Approx(30.0).margin(1e-12));
}

TEST_CASE("A finer interval resamples the same staircase", "[analysis][contour]") {
    // BS.1770's blocks advance every 100 ms, so there is nothing defined
    // between them. Asking for 25 ms gives four points per step with the same
    // value, not an interpolation through values the standard does not define.
    const SampleRate rate = kSampleRate48000;

    AudioBuffer buffer{ChannelLayout::mono(), framesFor(6.0, rate)};
    addSine(buffer, 0, 1000.0, 0.1, rate, 0, framesFor(3.0, rate));
    addSine(buffer, 0, 1000.0, 0.5, rate, framesFor(3.0, rate), framesFor(3.0, rate));

    const LoudnessContour fine = contourOrFail(buffer, rate, 0.025);
    const LoudnessContour coarse = contourOrFail(buffer, rate, 0.1);

    // Every coarse point has an exactly equal partner in the fine contour, four
    // points apart, and the three between it and the next repeat its value.
    for (std::size_t i = 4; i + 1 < coarse.points.size(); ++i) {
        const LoudnessPoint& coarsePoint = coarse.points[i];
        const LoudnessPoint& finePoint = fine.points[i * 4];
        REQUIRE(finePoint.momentaryLufs.has_value());
        CHECK(*finePoint.momentaryLufs == *coarsePoint.momentaryLufs);
        CHECK(*fine.points[i * 4 + 1].momentaryLufs == *coarsePoint.momentaryLufs);
        CHECK(*fine.points[i * 4 + 3].momentaryLufs == *coarsePoint.momentaryLufs);
    }
}

TEST_CASE("The layout-free overload infers mono and stereo", "[analysis][contour]") {
    const SampleRate rate = kSampleRate48000;
    const double amplitude = std::pow(10.0, -23.0 / 20.0);

    AudioBuffer stereo{ChannelLayout::stereo(), framesFor(5.0, rate)};
    fillSine(stereo, 0, 1000.0, amplitude, rate);
    fillSine(stereo, 1, 1000.0, amplitude, rate);

    auto inferred = measureLoudnessContour(stereo.constView(), rate);
    REQUIRE(inferred.hasValue());
    CHECK(inferred.value().integratedLufs ==
          contourOrFail(stereo, rate, kDefaultContourIntervalSeconds).integratedLufs);
    CHECK(inferred.value().integratedLufs == Approx(-23.0).margin(0.1));

    // Six channels become discrete, not a guessed 5.1: every channel at full
    // weight, so none is dropped as an LFE or lifted as a surround. Six
    // identical channels are six times the power of one, 7.78 LU.
    AudioBuffer six{ChannelLayout::discrete(6), framesFor(5.0, rate)};
    for (int channel = 0; channel < 6; ++channel) {
        fillSine(six, channel, 1000.0, amplitude, rate);
    }
    auto discrete = measureLoudnessContour(six.constView(), rate);
    REQUIRE(discrete.hasValue());
    CHECK(discrete.value().integratedLufs - (-23.0 - kSineCrestDb) ==
          Approx(10.0 * std::log10(6.0)).margin(0.05));
}

// --- Refusals ---------------------------------------------------------------

TEST_CASE("Degenerate input is refused rather than guessed at", "[analysis][contour]") {
    const SampleRate rate = kSampleRate48000;
    const auto mono = ChannelLayout::mono();

    AudioBuffer buffer{mono, framesFor(5.0, rate)};
    fillSine(buffer, 0, 1000.0, 0.3, rate);

    SECTION("a zero or invalid sample rate") {
        CHECK_FALSE(measureLoudnessContour(buffer.constView(), SampleRate{0.0}, mono).hasValue());
        CHECK_FALSE(measureLoudnessContour(buffer.constView(), SampleRate{-1.0}, mono).hasValue());
        // Below kMinimumSampleRateHz the K-weighting derivation is undefined.
        CHECK_FALSE(measureLoudnessContour(buffer.constView(), SampleRate{100.0}, mono).hasValue());
    }

    SECTION("empty audio") {
        const ConstAudioBufferView empty;
        CHECK_FALSE(measureLoudnessContour(empty, rate, mono).hasValue());
        CHECK_FALSE(measureLoudnessContour(empty, rate).hasValue());

        AudioBuffer noFrames{mono, 0};
        CHECK_FALSE(measureLoudnessContour(noFrames.constView(), rate, mono).hasValue());
    }

    SECTION("a non-positive interval") {
        CHECK_FALSE(measureLoudnessContour(buffer.constView(), rate, mono, 0.0).hasValue());
        CHECK_FALSE(measureLoudnessContour(buffer.constView(), rate, mono, -0.1).hasValue());
        CHECK_FALSE(measureLoudnessContour(buffer.constView(), rate, mono,
                                           std::numeric_limits<double>::quiet_NaN())
                        .hasValue());
    }

    SECTION("an interval shorter than one sample") {
        // 1 us at 48 kHz rounds to nought frames, and a grid that never
        // advances is an infinite contour rather than a fine one.
        CHECK_FALSE(measureLoudnessContour(buffer.constView(), rate, mono, 1e-6).hasValue());
    }

    SECTION("audio shorter than one 400 ms block") {
        // One frame short of a block, then exactly a block: the boundary
        // itself, since 400 ms is precisely where a momentary value begins.
        AudioBuffer justUnder{mono, framesFor(0.4, rate) - 1};
        fillSine(justUnder, 0, 1000.0, 0.3, rate);
        CHECK_FALSE(measureLoudnessContour(justUnder.constView(), rate, mono).hasValue());

        AudioBuffer exactly{mono, framesFor(0.4, rate)};
        fillSine(exactly, 0, 1000.0, 0.3, rate);
        auto shortest = measureLoudnessContour(exactly.constView(), rate, mono);
        REQUIRE(shortest.hasValue());
        // Five points, 0 to 400 ms, and only the last one has anything in it.
        REQUIRE(shortest.value().points.size() == 5);
        CHECK(shortest.value().points[4].momentaryLufs.has_value());
        CHECK_FALSE(shortest.value().points[4].shortTermLufs.has_value());
        CHECK_FALSE(shortest.value().points[3].momentaryLufs.has_value());
    }

    SECTION("a layout that does not match the buffer") {
        CHECK_FALSE(
            measureLoudnessContour(buffer.constView(), rate, ChannelLayout::stereo()).hasValue());
    }

    SECTION("a layout with nothing measurable in it") {
        const auto lfeOnly = ChannelLayout::discrete(0).withSpeakers({Speaker::Lfe});
        CHECK_FALSE(measureLoudnessContour(buffer.constView(), rate, lfeOnly).hasValue());
    }

    SECTION("an oversampling factor the true-peak meter will not accept") {
        CHECK_FALSE(measureLoudnessContour(buffer.constView(), rate, mono,
                                           kDefaultContourIntervalSeconds, 2)
                        .hasValue());
    }
}
