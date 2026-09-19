#include <sa/analysis/RoomAcoustics.h>

#include <algorithm>
#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <numbers>
#include <random>

using namespace sa;
using namespace sa::analysis;
using Catch::Approx;

namespace {

constexpr SampleRate kRate{48000.0};

/// An exponentially decaying noise burst: the textbook idealisation of a room,
/// and the one case where the right answer can be written down.
///
/// The envelope is exp(-t/tau), so the energy goes as exp(-2t/tau) and the
/// decay in decibels is -10*log10(e) * 2t/tau = -8.6859 t/tau. Sixty decibels
/// therefore takes 60*tau/8.6859 = 6.9078*tau seconds, which is the T60 every
/// measure below should recover.
[[nodiscard]] double tauForT60(double t60Seconds) {
    return t60Seconds * std::log10(std::numbers::e) * 2.0 * 10.0 / 60.0;
}

[[nodiscard]] AudioBuffer decayingNoise(double t60Seconds, double lengthSeconds, unsigned seed = 17,
                                        double leadingSilence = 0.0, double noiseFloor = 0.0) {
    const auto frames = static_cast<SampleCount>(lengthSeconds * kRate.hz());
    const auto lead = static_cast<SampleCount>(leadingSilence * kRate.hz());
    const double tau = tauForT60(t60Seconds);

    std::mt19937 engine{seed};
    std::normal_distribution<double> noise{0.0, 1.0};
    AudioBuffer buffer{ChannelLayout::mono(), frames + lead};
    for (SampleCount i = 0; i < lead; ++i) {
        buffer.channel(0)[i] = static_cast<float>(noiseFloor * noise(engine));
    }
    for (SampleCount i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / kRate.hz();
        const double envelope = std::exp(-t / tau);
        buffer.channel(0)[i + lead] =
            static_cast<float>(0.9 * envelope * noise(engine) + noiseFloor * noise(engine));
    }
    return buffer;
}

} // namespace

TEST_CASE("Every reverberation measure recovers a decay whose rate is known", "[analysis][room]") {
    // The whole claim. A room with a 1.2 second T60 has to come back as 1.2.
    for (const double t60 : {0.4, 1.2, 2.5}) {
        const AudioBuffer ir = decayingNoise(t60, t60 * 2.0);
        const auto measured = measureRoomAcoustics(ir.view(), kRate);
        REQUIRE(measured);
        REQUIRE(measured.value().valid);

        REQUIRE(measured.value().hasT20);
        REQUIRE(measured.value().t20Seconds == Approx(t60).epsilon(0.05));
        REQUIRE(measured.value().hasT30);
        REQUIRE(measured.value().t30Seconds == Approx(t60).epsilon(0.05));
        // A purely exponential decay has no early reflections, so EDT agrees
        // with the tail. A real room is where they come apart.
        REQUIRE(measured.value().hasEarlyDecay);
        REQUIRE(measured.value().earlyDecaySeconds == Approx(t60).epsilon(0.10));
    }
}

TEST_CASE("Clarity matches what an exponential decay implies", "[analysis][room]") {
    // C80 is the energy before 80 ms over the energy after it. For an envelope
    // exp(-t/tau) the energy integrates to (1 - exp(-2*0.08/tau)) against
    // exp(-2*0.08/tau), so the ratio is exp(0.16/tau) - 1 and the answer can be
    // written down rather than remembered.
    const double t60 = 1.0;
    const double tau = tauForT60(t60);
    const AudioBuffer ir = decayingNoise(t60, 4.0);

    const auto measured = measureRoomAcoustics(ir.view(), kRate);
    REQUIRE(measured);

    const double expected80 = 10.0 * std::log10(std::exp(0.16 / tau) - 1.0);
    const double expected50 = 10.0 * std::log10(std::exp(0.10 / tau) - 1.0);
    REQUIRE(measured.value().clarity80Db == Approx(expected80).margin(0.6));
    REQUIRE(measured.value().clarity50Db == Approx(expected50).margin(0.6));

    // D50 and C50 are the same statement twice; they have to agree.
    const double fromClarity = std::pow(10.0, measured.value().clarity50Db / 10.0) /
                               (1.0 + std::pow(10.0, measured.value().clarity50Db / 10.0));
    REQUIRE(measured.value().definition50 == Approx(fromClarity).margin(0.01));
}

TEST_CASE("A livelier room reads as less clear", "[analysis][room]") {
    // The direction of the measure, independent of its absolute calibration.
    const auto clarityOf = [](double t60) {
        const AudioBuffer ir = decayingNoise(t60, t60 * 3.0);
        const auto measured = measureRoomAcoustics(ir.view(), kRate);
        REQUIRE(measured);
        return measured.value().clarity80Db;
    };
    REQUIRE(clarityOf(2.5) < clarityOf(1.0));
    REQUIRE(clarityOf(1.0) < clarityOf(0.3));
}

TEST_CASE("Centre time grows with reverberation", "[analysis][room]") {
    const auto centreOf = [](double t60) {
        const AudioBuffer ir = decayingNoise(t60, t60 * 3.0);
        const auto measured = measureRoomAcoustics(ir.view(), kRate);
        REQUIRE(measured);
        return measured.value().centreTimeSeconds;
    };
    REQUIRE(centreOf(0.3) < centreOf(1.0));
    REQUIRE(centreOf(1.0) < centreOf(2.5));
    // For an exponential decay the centre of gravity is tau/2.
    REQUIRE(centreOf(1.0) == Approx(tauForT60(1.0) / 2.0).epsilon(0.15));
}

TEST_CASE("Silence before the impulse is discarded rather than counted", "[analysis][room]") {
    // Half a second of nothing in front of the direct sound would otherwise be
    // half a second of "early energy", and every measure would depend on how
    // the file happened to be trimmed.
    const AudioBuffer tight = decayingNoise(1.0, 3.0, 17, 0.0);
    const AudioBuffer padded = decayingNoise(1.0, 3.0, 17, 0.5);

    const auto a = measureRoomAcoustics(tight.view(), kRate);
    const auto b = measureRoomAcoustics(padded.view(), kRate);
    REQUIRE(a);
    REQUIRE(b);

    REQUIRE(b.value().directSound > 20000); // About half a second in.
    REQUIRE(b.value().t20Seconds == Approx(a.value().t20Seconds).epsilon(0.02));
    REQUIRE(b.value().clarity80Db == Approx(a.value().clarity80Db).margin(0.2));
    REQUIRE(b.value().centreTimeSeconds == Approx(a.value().centreTimeSeconds).margin(0.01));
}

TEST_CASE("A noise floor does not inflate the reverberation time", "[analysis][room]") {
    // The failure the truncation exists to prevent: integrating a noisy tail
    // backwards bends the end of the decay curve flat, and a slope fitted
    // through that reads as a longer reverberation than the room has.
    const AudioBuffer clean = decayingNoise(1.0, 4.0, 17, 0.0, 0.0);
    const AudioBuffer noisy = decayingNoise(1.0, 4.0, 17, 0.0, 0.0015);

    const auto a = measureRoomAcoustics(clean.view(), kRate);
    const auto b = measureRoomAcoustics(noisy.view(), kRate);
    REQUIRE(a);
    REQUIRE(b);
    REQUIRE(b.value().hasT20);
    REQUIRE(b.value().t20Seconds == Approx(1.0).epsilon(0.10));
    REQUIRE(b.value().t20Seconds == Approx(a.value().t20Seconds).epsilon(0.10));
}

TEST_CASE("A measurement without the range for T30 says so rather than guessing",
          "[analysis][room]") {
    // A record holding about forty decibels of decay supports T20 and does not
    // support T30. The honest answer to the second is "not measurable", not an
    // extrapolation through the end of the recording.
    //
    // This test is the one that found the missing tail compensation. It was
    // written against a 25 dB record and failed, because the uncompensated
    // curve dives to minus infinity at the end of any finite record and so
    // always reaches -35 dB somewhere. The code was wrong, not the assertion:
    // that same dive was returning T20 = 0.547 for a decay of 1.000 s.
    const double t60 = 1.0;
    const double tau = tauForT60(t60);
    const double seconds = 40.0 * tau / 8.6859;

    const AudioBuffer ir = decayingNoise(t60, seconds, 17);
    const auto measured = measureRoomAcoustics(ir.view(), kRate);
    REQUIRE(measured);
    REQUIRE(measured.value().valid);

    REQUIRE(measured.value().hasT20);
    REQUIRE(measured.value().t20Seconds == Approx(t60).epsilon(0.05));
    REQUIRE_FALSE(measured.value().hasT30);
    REQUIRE(measured.value().t30Seconds == 0.0);
}

TEST_CASE("A record barely longer than the decay measures nothing but EDT", "[analysis][room]") {
    // Twenty-five decibels supports neither reverberation time -- both need
    // the curve to reach -25 dB after starting at -5 -- and saying so is the
    // point. EDT needs only ten decibels and is still available.
    const double t60 = 1.0;
    const double seconds = 25.0 * tauForT60(t60) / 8.6859;

    const AudioBuffer ir = decayingNoise(t60, seconds, 17);
    const auto measured = measureRoomAcoustics(ir.view(), kRate);
    REQUIRE(measured);
    REQUIRE(measured.value().valid);
    REQUIRE_FALSE(measured.value().hasT20);
    REQUIRE_FALSE(measured.value().hasT30);
    REQUIRE(measured.value().hasEarlyDecay);
    REQUIRE(measured.value().earlyDecaySeconds == Approx(t60).epsilon(0.12));
}

TEST_CASE("The tail compensation is what makes a short record honest", "[analysis][room]") {
    // Directly against the numbers the uncompensated version produced, so a
    // regression cannot pass quietly. A record holding forty decibels of a
    // 1.000 s decay returned 0.928 before the compensation and 1.011 after;
    // one holding sixty returned 0.997 and 1.004. The second is within noise
    // of correct either way, which is exactly why the first is the test.
    const double t60 = 1.0;
    const double tau = tauForT60(t60);

    const AudioBuffer shortish = decayingNoise(t60, 40.0 * tau / 8.6859, 17);
    const auto measured = measureRoomAcoustics(shortish.view(), kRate);
    REQUIRE(measured);
    REQUIRE(measured.value().hasT20);
    // The uncompensated answer was 0.928, which is outside this margin; the
    // compensated one is 1.011, which is inside it.
    REQUIRE(measured.value().t20Seconds == Approx(1.0).margin(0.04));
}

TEST_CASE("The Schroeder curve starts at zero and never rises", "[analysis][room]") {
    // The two properties that make it usable at all: it is relative to the
    // total energy, and it is monotonic, which is exactly what integrating
    // backwards buys over reading the envelope directly.
    const AudioBuffer ir = decayingNoise(1.0, 3.0);
    const auto curve = schroederCurveDb(ir.view());
    REQUIRE(curve);
    REQUIRE(curve.value().size() > 1000);
    REQUIRE(curve.value().front() == Approx(0.0).margin(1e-6));
    for (std::size_t i = 1; i < curve.value().size(); ++i) {
        REQUIRE(curve.value()[i] <= curve.value()[i - 1] + 1e-5f);
    }
}

TEST_CASE("Nonsense input is refused or reported as unmeasurable", "[analysis][room]") {
    const AudioBuffer ir = decayingNoise(1.0, 1.0);
    REQUIRE_FALSE(measureRoomAcoustics(ir.view(), kRate, 3));
    REQUIRE_FALSE(measureRoomAcoustics(ir.view(), SampleRate{0.0}));
    REQUIRE_FALSE(schroederCurveDb(ir.view(), -1));

    const AudioBuffer empty{ChannelLayout::mono(), 0};
    const auto nothing = measureRoomAcoustics(empty.view(), kRate);
    REQUIRE(nothing);
    REQUIRE_FALSE(nothing.value().valid);

    const AudioBuffer silent{ChannelLayout::mono(), 48000};
    const auto quiet = measureRoomAcoustics(silent.view(), kRate);
    REQUIRE(quiet);
    REQUIRE_FALSE(quiet.value().valid);
}

TEST_CASE("Each channel of a stereo impulse response is measured separately", "[analysis][room]") {
    // A stereo IR is two rooms as heard from two places, not one averaged room.
    AudioBuffer pair{ChannelLayout::stereo(), 48000 * 3};
    const AudioBuffer fast = decayingNoise(0.4, 3.0, 5);
    const AudioBuffer slow = decayingNoise(2.0, 3.0, 9);
    for (SampleCount i = 0; i < pair.frames(); ++i) {
        pair.channel(0)[i] = fast.channel(0)[i];
        pair.channel(1)[i] = slow.channel(0)[i];
    }

    const auto left = measureRoomAcoustics(pair.view(), kRate, 0);
    const auto right = measureRoomAcoustics(pair.view(), kRate, 1);
    REQUIRE(left);
    REQUIRE(right);
    REQUIRE(left.value().t20Seconds == Approx(0.4).epsilon(0.08));
    REQUIRE(right.value().t20Seconds == Approx(2.0).epsilon(0.08));
}

namespace {

/// A decay whose rate depends on frequency: slow low down, fast up top, which
/// is what a room with soft furnishings does and what a single T30 cannot say.
[[nodiscard]] AudioBuffer twoRateDecay(double lowT60, double highT60, double seconds,
                                       double splitHz = 1000.0, unsigned seed = 23) {
    const auto frames = static_cast<SampleCount>(seconds * kRate.hz());
    const double lowTau = tauForT60(lowT60);
    const double highTau = tauForT60(highT60);

    std::mt19937 engine{seed};
    std::normal_distribution<double> noise{0.0, 1.0};

    // Two noise sources with their own envelopes, separated by a cascade of six
    // one-poles rather than one.
    //
    // One pole each way was the first attempt and it was the test material
    // that was wrong, not the code. A single pole at 1 kHz leaves the slow
    // low-frequency component only 12 dB down at 4 kHz, and because it decays
    // four times more slowly it overtakes the fast component there within half
    // a second -- so the 4 kHz band was correctly measuring leakage, and read
    // 1.39 s where the high component's own rate is 0.5. Six poles is 72 dB of
    // separation two octaves out, which the fast decay stays ahead of for
    // longer than the fit needs.
    constexpr int kStages = 6;
    AudioBuffer buffer{ChannelLayout::mono(), frames};
    const double w = 2.0 * std::numbers::pi * splitHz / kRate.hz();
    const double alpha = std::sin(w) / (1.0 + std::cos(w));
    std::array<double, kStages> lowState{};
    std::array<double, kStages> highState{};

    for (SampleCount i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / kRate.hz();

        double lowValue = noise(engine);
        for (int stage = 0; stage < kStages; ++stage) {
            lowState[static_cast<std::size_t>(stage)] +=
                alpha * (lowValue - lowState[static_cast<std::size_t>(stage)]);
            lowValue = lowState[static_cast<std::size_t>(stage)];
        }

        double highValue = noise(engine);
        for (int stage = 0; stage < kStages; ++stage) {
            const double before = highValue;
            highState[static_cast<std::size_t>(stage)] +=
                alpha * (before - highState[static_cast<std::size_t>(stage)]);
            highValue = before - highState[static_cast<std::size_t>(stage)];
        }

        // The low path loses a great deal of level to six low-passes; scaled so
        // both components arrive at a comparable amplitude.
        buffer.channel(0)[i] = static_cast<float>(
            0.6 * (12.0 * lowValue * std::exp(-t / lowTau) + highValue * std::exp(-t / highTau)));
    }
    return buffer;
}

} // namespace

TEST_CASE("Octave bands each report their own reverberation time", "[analysis][room][bands]") {
    // The reason banded measurement exists: one number cannot say that a room
    // is lively at the bottom and dead at the top, and that difference is what
    // decides which material fixes it.
    const AudioBuffer ir = twoRateDecay(2.0, 0.5, 6.0);
    const auto banded = measureRoomAcousticsByBand(ir.view(), kRate);
    REQUIRE(banded);
    REQUIRE(banded.value().size() >= 8);

    const auto at = [&](double centre) {
        const auto found = std::find_if(
            banded.value().begin(), banded.value().end(),
            [centre](const BandedRoomAcoustics& b) { return std::abs(b.centreHz - centre) < 0.6; });
        REQUIRE(found != banded.value().end());
        return found->measures;
    };

    const RoomAcoustics low = at(250.0);
    const RoomAcoustics high = at(4000.0);
    REQUIRE(low.valid);
    REQUIRE(high.valid);
    REQUIRE(low.hasT20);
    REQUIRE(high.hasT20);
    // Not the exact rates -- the filters have skirts and the test material is
    // crude -- but the ordering and a clear separation, which is the claim.
    REQUIRE(low.t20Seconds > high.t20Seconds * 1.5);
}

TEST_CASE("The band next to a change in decay rate is not dragged by its neighbour",
          "[analysis][room][bands]") {
    // The hard case, and the one the band filter's skirt decides. 250 Hz and
    // 4 kHz are two octaves clear of the crossover and easy; the band sitting
    // directly above it is where a neighbour's energy leaks in, and a slow
    // decay leaking into a fast band overtakes the fast one within a second
    // and drags the answer towards its own rate.
    //
    // This is why the per-band filter is a sixth-order Butterworth from the
    // shared bank rather than the two cookbook sections it used to be. On this
    // material the 2 kHz band's true figure is 0.500 s, and it reads:
    //
    //                    EDT      T20      T30
    //   two sections    0.518    0.643    1.182
    //   six poles       0.499    0.507    (see below)
    //
    // The old filter's T20 was 29% high; the new one is 1.5% high. Both
    // figures were measured on exactly this material, and the assertion below
    // sits between them, so returning to a gentler skirt fails here rather
    // than passing quietly.
    //
    // T30 is not asserted. It fits down to -35 dB, which on a band this close
    // to a fourfold change in decay rate is far enough in for what leaked past
    // the skirt to have overtaken what belongs here -- the slow neighbour is
    // still going when the fast band has gone. A steeper filter pushes that
    // crossing later without removing it, and claiming a T30 here would be
    // claiming the filter did something it cannot.
    const AudioBuffer ir = twoRateDecay(2.0, 0.5, 6.0);
    const auto banded = measureRoomAcousticsByBand(ir.view(), kRate);
    REQUIRE(banded);

    const auto found = std::find_if(
        banded.value().begin(), banded.value().end(),
        [](const BandedRoomAcoustics& b) { return std::abs(b.centreHz - 2000.0) < 5.0; });
    REQUIRE(found != banded.value().end());
    const RoomAcoustics band = found->measures;
    REQUIRE(band.valid);
    REQUIRE(band.hasT20);

    CAPTURE(band.earlyDecaySeconds, band.t20Seconds, band.t30Seconds);
    // EDT reads the first 10 dB, before anything that leaked in has had time
    // to overtake, so it is the figure that should be nearly exact.
    REQUIRE(band.earlyDecaySeconds == Approx(0.5).epsilon(0.12));
    // T20 is allowed the leakage that remains, and is bounded well below the
    // 0.627 the old filter gave -- so a return to a gentler skirt fails here.
    REQUIRE(band.t20Seconds < 0.60);
    REQUIRE(band.t20Seconds > 0.40);
}

TEST_CASE("A uniform decay reads the same in every band", "[analysis][room][bands]") {
    // The converse, and the one that would catch a filter Q that varies with
    // the band in a way it should not: white noise decaying at one rate has to
    // come back as one rate everywhere.
    const AudioBuffer ir = decayingNoise(1.0, 4.0);
    const auto banded = measureRoomAcousticsByBand(ir.view(), kRate);
    REQUIRE(banded);

    int checked = 0;
    for (const BandedRoomAcoustics& band : banded.value()) {
        // The lowest bands have too few cycles in the record to measure, and
        // say so rather than being wrong; skip those rather than assert on
        // them.
        if (!band.measures.valid || !band.measures.hasT20) {
            continue;
        }
        REQUIRE(band.measures.t20Seconds == Approx(1.0).epsilon(0.20));
        ++checked;
    }
    REQUIRE(checked >= 5);
}

TEST_CASE("Banded measurement covers the standard centres and stops at Nyquist",
          "[analysis][room][bands]") {
    const AudioBuffer ir = decayingNoise(1.0, 2.0);
    const auto banded = measureRoomAcousticsByBand(ir.view(), kRate);
    REQUIRE(banded);
    REQUIRE(banded.value().front().centreHz == Approx(31.5));
    for (const BandedRoomAcoustics& band : banded.value()) {
        REQUIRE(band.centreHz * std::exp2(0.5) <= kRate.hz() * 0.5);
    }

    // Third octaves give more bands over the same range.
    const auto thirds = measureRoomAcousticsByBand(ir.view(), kRate, 0, BandWidth::ThirdOctave);
    REQUIRE(thirds);
    REQUIRE(thirds.value().size() > banded.value().size() * 2);
}

TEST_CASE("Banded measurement refuses what the single-band one refuses",
          "[analysis][room][bands]") {
    const AudioBuffer ir = decayingNoise(1.0, 1.0);
    REQUIRE_FALSE(measureRoomAcousticsByBand(ir.view(), kRate, 4));
    REQUIRE_FALSE(measureRoomAcousticsByBand(ir.view(), SampleRate{0.0}));

    const AudioBuffer empty{ChannelLayout::mono(), 0};
    const auto nothing = measureRoomAcousticsByBand(empty.view(), kRate);
    REQUIRE(nothing);
    REQUIRE_FALSE(nothing.value().empty());
    for (const BandedRoomAcoustics& band : nothing.value()) {
        REQUIRE_FALSE(band.measures.valid);
    }
}
