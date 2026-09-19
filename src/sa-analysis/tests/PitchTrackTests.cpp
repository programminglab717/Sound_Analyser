#include <sa/analysis/PitchTrack.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <random>
#include <vector>

using namespace sa;
using namespace sa::analysis;
using Catch::Approx;

namespace {

constexpr SampleRate kRate{48000.0};
constexpr double kTwoPi = 2.0 * std::numbers::pi;

[[nodiscard]] AudioBuffer sine(double hz, double seconds, SampleRate rate = kRate) {
    const auto frames = static_cast<SampleCount>(seconds * rate.hz());
    AudioBuffer buffer{ChannelLayout::mono(), frames};
    for (SampleCount i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / rate.hz();
        buffer.channel(0)[i] = static_cast<float>(0.5 * std::sin(kTwoPi * hz * t));
    }
    return buffer;
}

/// A stack of harmonics of `hz`: entry k of `amplitudes` is harmonic k + 1.
///
/// Additive, rather than a sampled ramp or a comparator, so that the only
/// frequencies present are exact multiples of the fundamental. Sampling a
/// geometric sawtooth folds its upper harmonics back to frequencies that are
/// multiples of nothing, which would leave the period here approximate where
/// it can be exact.
[[nodiscard]] AudioBuffer harmonics(double hz, const std::vector<double>& amplitudes,
                                    double seconds, SampleRate rate = kRate) {
    const auto frames = static_cast<SampleCount>(seconds * rate.hz());
    AudioBuffer buffer{ChannelLayout::mono(), frames};

    double sum = 0.0;
    for (const double amplitude : amplitudes) {
        sum += std::abs(amplitude);
    }
    const double scale = sum > 0.0 ? 0.5 / sum : 0.0;

    for (SampleCount i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / rate.hz();
        double value = 0.0;
        for (std::size_t k = 0; k < amplitudes.size(); ++k) {
            value += amplitudes[k] * std::sin(kTwoPi * hz * static_cast<double>(k + 1) * t);
        }
        buffer.channel(0)[i] = static_cast<float>(scale * value);
    }
    return buffer;
}

/// Harmonic amplitudes of a band-limited sawtooth or square wave.
///
/// A sawtooth is every harmonic at 1/k with alternating sign; a square is the
/// odd ones only, also at 1/k. Forty harmonics: the fortieth is 32 dB down, so
/// what is left off is not what a period finder is looking at, and the series
/// stops below Nyquist by construction rather than aliasing over it.
[[nodiscard]] std::vector<double> harmonicSeries(double hz, bool oddOnly, SampleRate rate) {
    std::vector<double> amplitudes;
    for (int k = 1; k <= 40 && static_cast<double>(k) * hz < rate.hz() * 0.5; ++k) {
        const bool present = !oddOnly || k % 2 == 1;
        const double sign = (oddOnly || k % 2 == 1) ? 1.0 : -1.0;
        amplitudes.push_back(present ? sign / static_cast<double>(k) : 0.0);
    }
    return amplitudes;
}

[[nodiscard]] AudioBuffer sawtooth(double hz, double seconds, SampleRate rate = kRate) {
    return harmonics(hz, harmonicSeries(hz, false, rate), seconds, rate);
}

[[nodiscard]] AudioBuffer square(double hz, double seconds, SampleRate rate = kRate) {
    return harmonics(hz, harmonicSeries(hz, true, rate), seconds, rate);
}

/// A sine whose frequency rises linearly across the whole buffer.
///
/// Phase is the integral of frequency, so a glide at r = (toHz - fromHz) /
/// seconds hertz per second has phase 2*pi*(fromHz*t + r*t*t/2), and the
/// instantaneous frequency at t is fromHz + r*t -- which is the number the
/// contour has to give back at t.
[[nodiscard]] AudioBuffer glide(double fromHz, double toHz, double seconds,
                                SampleRate rate = kRate) {
    const auto frames = static_cast<SampleCount>(seconds * rate.hz());
    AudioBuffer buffer{ChannelLayout::mono(), frames};
    const double slope = (toHz - fromHz) / seconds;
    for (SampleCount i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / rate.hz();
        const double phase = kTwoPi * (fromHz * t + 0.5 * slope * t * t);
        buffer.channel(0)[i] = static_cast<float>(0.5 * std::sin(phase));
    }
    return buffer;
}

[[nodiscard]] AudioBuffer whiteNoise(double seconds, unsigned seed = 23, SampleRate rate = kRate) {
    const auto frames = static_cast<SampleCount>(seconds * rate.hz());
    AudioBuffer buffer{ChannelLayout::mono(), frames};
    std::mt19937 engine{seed};
    std::normal_distribution<double> gaussian{0.0, 0.2};
    for (SampleCount i = 0; i < frames; ++i) {
        buffer.channel(0)[i] = static_cast<float>(gaussian(engine));
    }
    return buffer;
}

} // namespace

TEST_CASE("A pure tone comes back at the frequency it was made at", "[analysis][pitch]") {
    // Half a per cent is a twelfth of a semitone: a semitone is 2^(1/12),
    // which is 5.95%, and a twelfth of that is 0.50%.
    //
    // 880 Hz is in the list because it is the one that makes the parabolic fit
    // necessary rather than tidy. 48000/880 = 54.545 samples, and the nearest
    // whole lag, 55, reads back as 48000/55 = 872.73 Hz -- 0.83% flat, which
    // fails this on its own. 80 Hz, at 600 samples exactly, would pass without
    // any fit at all, which is why both are here.
    for (const double hz : {80.0, 220.0, 440.0, 880.0}) {
        const AudioBuffer tone = sine(hz, 0.5);
        const auto measured = estimatePitch(tone.channel(0), tone.frames(), kRate);
        REQUIRE(measured);
        REQUIRE(measured.value() == Approx(hz).epsilon(0.005));
    }
}

TEST_CASE("The reading does not depend on the sample rate", "[analysis][pitch]") {
    // 440 Hz is 109.09 samples at 48 kHz, 100.23 at 44.1 kHz and 218.18 at
    // 96 kHz: three different lags, three different roundings, one tone.
    for (const double hz : {44100.0, 48000.0, 96000.0}) {
        const SampleRate rate{hz};
        const AudioBuffer tone = sine(440.0, 0.5, rate);
        const auto measured = estimatePitch(tone.channel(0), tone.frames(), rate);
        REQUIRE(measured);
        REQUIRE(measured.value() == Approx(440.0).epsilon(0.005));
    }
}

TEST_CASE("A harmonically rich tone gives its fundamental, not an octave either way",
          "[analysis][pitch]") {
    // The case the algorithm exists for. A sawtooth at 110 Hz has a period of
    // 48000/110 = 436.36 samples and energy at every multiple of 110 Hz up to
    // 4.4 kHz. 872.73 samples is just as exactly a period of it, and so is
    // 1309.09 -- so a detector that takes the best match anywhere reports 55 Hz
    // or 36.7 Hz on material like this, and one that follows the strongest
    // partial in the spectrum can be talked into 220.
    //
    // Every voiced frame is checked rather than their median, because an
    // octave error in a third of the frames would leave the median intact and
    // the contour ruined.
    const auto check = [](const AudioBuffer& tone, double hz) {
        const auto contour = trackPitch(tone.view(), kRate);
        REQUIRE(contour);
        REQUIRE(contour.value().size() == 83);
        for (const PitchPoint& point : contour.value()) {
            // A steady tone with no attack and no gaps: nothing here should be
            // ambiguous enough for a frame to decline.
            REQUIRE(point.voiced);
            REQUIRE(point.hz == Approx(hz).epsilon(0.005));
        }
    };

    for (const double hz : {110.0, 220.0, 330.0}) {
        check(sawtooth(hz, 0.5), hz);
        check(square(hz, 0.5), hz);
    }
}

TEST_CASE("A tone with nothing at its fundamental still reads as that fundamental",
          "[analysis][pitch]") {
    // Harmonics two to five at 1/k, and no energy whatever at 155 Hz. The
    // period is still 1/155 s, because the greatest common divisor of 2, 3, 4
    // and 5 is 1 -- and a period is what is being measured. Anything working
    // from the spectrum finds nothing at 155 Hz and says 310.
    const AudioBuffer missing =
        harmonics(155.0, {0.0, 1.0 / 2.0, 1.0 / 3.0, 1.0 / 4.0, 1.0 / 5.0}, 0.5);
    const auto absent = estimatePitch(missing.channel(0), missing.frames(), kRate);
    REQUIRE(absent);
    REQUIRE(absent.value() == Approx(155.0).epsilon(0.005));

    // The same point with the fundamental present but 20 dB under the second
    // harmonic, which is roughly what a voice down a telephone line looks like.
    const AudioBuffer weak = harmonics(155.0, {0.1, 1.0, 0.5, 0.3, 0.2}, 0.5);
    const auto quiet = estimatePitch(weak.channel(0), weak.frames(), kRate);
    REQUIRE(quiet);
    REQUIRE(quiet.value() == Approx(155.0).epsilon(0.005));
}

TEST_CASE("The contour follows a tone that glides", "[analysis][pitch]") {
    // 200 Hz to 400 Hz over two seconds is 100 Hz per second, about as fast as
    // a portamento gets.
    //
    // The tolerance comes from how far the tone moves inside one frame, not
    // from the detector. With minHz at 150 the longest lag is 320 samples, so a
    // frame spans 2048 + 320 = 2368 samples, 49.3 ms, over which the tone moves
    // 4.93 Hz. The timestamp sits at the middle of the integration window,
    // sample 1024, while the evidence is centred near sample (2048 + lag)/2,
    // which is about 1120 at these pitches: 96 samples, 2.0 ms, 0.20 Hz of
    // systematic lag. One per cent is 2 to 4 Hz here, ten times that bias and
    // still well inside the 4.93 Hz the frame itself averages over.
    PitchSettings settings;
    settings.minHz = 150.0;
    settings.maxHz = 500.0;

    const AudioBuffer swept = glide(200.0, 400.0, 2.0);
    const auto contour = trackPitch(swept.view(), kRate, settings);
    REQUIRE(contour);

    // 96000 samples, frames spanning 2368, hops of 256: the last frame starts
    // at 93440, which is 365 hops in, so 366 points.
    REQUIRE(contour.value().size() == 366);

    for (const PitchPoint& point : contour.value()) {
        REQUIRE(point.voiced);
        REQUIRE(point.hz == Approx(200.0 + 100.0 * point.timeSeconds).epsilon(0.01));
    }

    // Spelled out at four points along the way, because a rule applied to every
    // point can be right about a curve that is the wrong curve.
    const auto at = [&contour](double seconds) {
        for (const PitchPoint& point : contour.value()) {
            if (point.timeSeconds >= seconds) {
                return point;
            }
        }
        return contour.value().back();
    };
    REQUIRE(at(0.1).hz == Approx(210.0).epsilon(0.01));
    REQUIRE(at(0.5).hz == Approx(250.0).epsilon(0.01));
    REQUIRE(at(1.0).hz == Approx(300.0).epsilon(0.01));
    REQUIRE(at(1.5).hz == Approx(350.0).epsilon(0.01));
}

TEST_CASE("Silence has no pitch and does not invent one", "[analysis][pitch]") {
    const AudioBuffer quiet{ChannelLayout::mono(), 24000}; // Zeroed by construction.
    const auto contour = trackPitch(quiet.view(), kRate);
    REQUIRE(contour);
    REQUIRE(contour.value().size() == 83);

    for (const PitchPoint& point : contour.value()) {
        REQUIRE_FALSE(point.voiced);
        REQUIRE(point.hz == 0.0);
        // Every lag mismatches by nothing, and so does the average of every
        // lag. "Perfectly aperiodic" is the honest reading of a frame with
        // nothing in it, not the perfect match that dividing zero by zero
        // would otherwise suggest.
        REQUIRE(point.confidence == 0.0);
    }

    const auto single = estimatePitch(quiet.channel(0), quiet.frames(), kRate);
    REQUIRE(single);
    REQUIRE(single.value() == 0.0);
}

TEST_CASE("White noise is reported as unvoiced, at low confidence", "[analysis][pitch]") {
    // A voiced frame needs the normalised difference below 0.15, which is a
    // confidence above 0.85, and the arithmetic says how far noise is from
    // that. The mismatch at one lag is a sum of 2048 squared gaussian
    // differences, so it is chi-squared with 2048 degrees of freedom and has a
    // relative spread of sqrt(2/2048) = 3.1% about its mean -- which is very
    // nearly what the cumulative mean divides it by. The smallest of the 960
    // lags searched therefore lands about three spreads down, near 0.9, for a
    // confidence near 0.1. Getting to 0.85 would take twenty-seven spreads, so
    // half is a bound with no chance of a lucky seed behind it.
    const AudioBuffer noise = whiteNoise(0.5);
    const auto contour = trackPitch(noise.view(), kRate);
    REQUIRE(contour);
    REQUIRE(contour.value().size() == 83);

    for (const PitchPoint& point : contour.value()) {
        REQUIRE_FALSE(point.voiced);
        REQUIRE(point.hz == 0.0);
        REQUIRE(point.confidence < 0.5);
    }

    const auto single = estimatePitch(noise.channel(0), noise.frames(), kRate);
    REQUIRE(single);
    REQUIRE(single.value() == 0.0);
}

TEST_CASE("A tone outside the range asked for is not reported as one inside it",
          "[analysis][pitch]") {
    // Below the floor. 30 Hz at 48 kHz has a period of 1600 samples and the
    // defaults only look out to 960. No shorter lag is a period of it either:
    // 800 samples is half a period, where a sine is the exact negative of
    // itself, which is as far from a match as it ever gets. There is nothing
    // in range to find, and saying so is the answer.
    const AudioBuffer below = sine(30.0, 0.5);
    const auto low = trackPitch(below.view(), kRate);
    REQUIRE(low);
    REQUIRE(low.value().size() == 83);
    for (const PitchPoint& point : low.value()) {
        REQUIRE_FALSE(point.voiced);
        REQUIRE(point.hz == 0.0);
    }
    REQUIRE(estimatePitch(below.channel(0), below.frames(), kRate).value() == 0.0);

    // Above the ceiling, which is the harder half. 2000 Hz repeats every 24
    // samples, shorter than the 48 a 1000 Hz ceiling allows -- but 48, 72 and
    // 96 are all multiples of 24, and every one of them is a genuine period of
    // the signal. Reporting 1000 Hz would not even be a poor match; it would
    // be a perfect match to the wrong question. Searching from a lag of one
    // sample is what finds 24 first and lets it be refused.
    const AudioBuffer above = sine(2000.0, 0.5);
    const auto high = trackPitch(above.view(), kRate);
    REQUIRE(high);
    REQUIRE(high.value().size() == 83);
    for (const PitchPoint& point : high.value()) {
        REQUIRE_FALSE(point.voiced);
        REQUIRE(point.hz == 0.0);
        // High confidence and no pitch at once, and deliberately so: the frame
        // really is periodic. What it is not is periodic at a pitch that was
        // asked for.
        REQUIRE(point.confidence > 0.9);
    }

    // With a ceiling that does take it in, the same audio reads 2000 Hz -- so
    // what is being tested above is the range doing its job, not the detector
    // failing to find the tone.
    PitchSettings wider;
    wider.maxHz = 4000.0;
    const auto reached = estimatePitch(above.channel(0), above.frames(), kRate, wider);
    REQUIRE(reached);
    REQUIRE(reached.value() == Approx(2000.0).epsilon(0.005));
}

TEST_CASE("The contour is one point per hop, centred on its own window", "[analysis][pitch]") {
    // Half a second is 24000 samples. A frame spans window plus longest lag,
    // 2048 + 48000/50 = 3008, so the last one that fits starts at
    // 24000 - 3008 = 20992, and 20992/256 = 82 exactly: 83 points counting the
    // one at zero.
    const AudioBuffer tone = sine(440.0, 0.5);
    const auto contour = trackPitch(tone.view(), kRate);
    REQUIRE(contour);
    REQUIRE(contour.value().size() == 83);

    // Stamped at the middle of the integration window, so the first is at
    // 1024/48000 = 21.333 ms and they are 256/48000 = 5.333 ms apart.
    REQUIRE(contour.value().front().timeSeconds == Approx(1024.0 / 48000.0));
    REQUIRE(contour.value().back().timeSeconds == Approx(22016.0 / 48000.0));
    for (std::size_t i = 1; i < contour.value().size(); ++i) {
        REQUIRE(contour.value()[i].timeSeconds - contour.value()[i - 1].timeSeconds ==
                Approx(256.0 / 48000.0));
    }
}

TEST_CASE("Each channel is tracked on its own", "[analysis][pitch]") {
    // A stereo pair is two things happening, not one averaged thing.
    const AudioBuffer lowTone = sine(220.0, 0.5);
    const AudioBuffer highTone = sine(660.0, 0.5);
    AudioBuffer pair{ChannelLayout::stereo(), lowTone.frames()};
    for (SampleCount i = 0; i < pair.frames(); ++i) {
        pair.channel(0)[i] = lowTone.channel(0)[i];
        pair.channel(1)[i] = highTone.channel(0)[i];
    }

    const auto left = trackPitch(pair.constView(), kRate, {}, 0);
    const auto right = trackPitch(pair.constView(), kRate, {}, 1);
    REQUIRE(left);
    REQUIRE(right);
    REQUIRE(left.value().front().hz == Approx(220.0).epsilon(0.005));
    REQUIRE(right.value().front().hz == Approx(660.0).epsilon(0.005));
}

TEST_CASE("Confidence is the share of the frame that is the tone", "[analysis][pitch]") {
    // The figure is not a vague score. For a tone in white noise it has a value
    // that can be written down, and writing it down is also what says where the
    // threshold puts the voiced/unvoiced line.
    //
    // The mismatch at a lag of one period is 2*W*(noise power): the tone
    // matches itself exactly there, and noise matches nothing at any lag. The
    // average mismatch over the lags up to that one is
    // 2*W*(tone power + noise power), because the tone's contribution goes as
    // 1 - cos, and cos averages to zero over the whole period being averaged
    // across. So the normalised difference at the period is
    // noise/(tone + noise), and the confidence is tone/(tone + noise).
    //
    // Which means the threshold has a plain reading: voicing at 0.15 asks for
    // the tone to be at least 0.85/0.15 = 5.67 times the noise in power, i.e.
    // 7.5 dB. Both cases below are put either side of that deliberately.
    const AudioBuffer clean = sine(220.0, 0.5); // 0.5 peak, so 0.125 in power.

    const auto mix = [&clean](double noiseRms, unsigned seed) {
        const AudioBuffer noise = whiteNoise(0.5, seed);
        AudioBuffer mixed{ChannelLayout::mono(), clean.frames()};
        for (SampleCount i = 0; i < mixed.frames(); ++i) {
            // whiteNoise() is 0.2 RMS, so scaling gets any level wanted.
            mixed.channel(0)[i] =
                clean.channel(0)[i] + static_cast<float>(noiseRms / 0.2) * noise.channel(0)[i];
        }
        return mixed;
    };

    const auto meanConfidence = [](const AudioBuffer& audio) {
        const auto contour = trackPitch(audio.constView(), kRate);
        REQUIRE(contour);
        double total = 0.0;
        for (const PitchPoint& point : contour.value()) {
            total += point.confidence;
        }
        return total / static_cast<double>(contour.value().size());
    };

    // The margins are a spread, not a fudge: the mismatch at one lag is a sum
    // of 2048 squared gaussian terms and so carries a relative spread of
    // sqrt(2/2048) = 3.1%, which on these confidences is up to 0.03, and the
    // search takes the smallest of the few lags around the period, which pulls
    // it a little further down again.

    // Nothing but the tone: it matches itself to the transform's own rounding.
    REQUIRE(meanConfidence(clean) > 0.99);

    // Noise at 0.1 RMS is 0.01 in power, so confidence 0.125/0.135 = 0.926 and
    // a difference of 0.074 -- half the threshold, so still a pitch.
    const AudioBuffer lightly = mix(0.1, 31);
    REQUIRE(meanConfidence(lightly) == Approx(0.926).margin(0.05));
    const auto audible = estimatePitch(lightly.channel(0), lightly.frames(), kRate);
    REQUIRE(audible);
    // Held to 1% rather than the 0.5% a clean tone is held to, because noise
    // this loud moves the three values the parabola is fitted through.
    REQUIRE(audible.value() == Approx(220.0).epsilon(0.01));

    // Noise at 0.35 RMS is 0.1225 in power, so confidence 0.125/0.2475 = 0.505
    // and a difference of 0.495 -- more than three times the threshold, so no
    // pitch is reported at all even though the tone is plainly still in there.
    const AudioBuffer buried = mix(0.35, 31);
    REQUIRE(meanConfidence(buried) == Approx(0.505).margin(0.05));
    const auto lost = estimatePitch(buried.channel(0), buried.frames(), kRate);
    REQUIRE(lost);
    REQUIRE(lost.value() == 0.0);
}

TEST_CASE("Input that cannot be analysed is refused rather than guessed at", "[analysis][pitch]") {
    const AudioBuffer tone = sine(440.0, 0.5);

    // Nothing to analyse.
    REQUIRE_FALSE(estimatePitch(tone.channel(0), 0, kRate));
    REQUIRE(estimatePitch(tone.channel(0), 0, kRate).error().code() == ErrorCode::InvalidArgument);
    REQUIRE_FALSE(estimatePitch(nullptr, 8192, kRate));
    const AudioBuffer empty{ChannelLayout::mono(), 0};
    REQUIRE_FALSE(trackPitch(empty.view(), kRate));

    // A frame is the window plus the longest lag the lowest pitch implies:
    // 2048 + 48000/50 = 3008 samples. One sample short of that cannot be
    // analysed, and exactly that can.
    REQUIRE_FALSE(estimatePitch(tone.channel(0), 3007, kRate));
    REQUIRE(estimatePitch(tone.channel(0), 3007, kRate).error().code() == ErrorCode::OutOfRange);
    const auto exact = estimatePitch(tone.channel(0), 3008, kRate);
    REQUIRE(exact);
    REQUIRE(exact.value() == Approx(440.0).epsilon(0.005));

    // The same refusal from the other end: a window as long as the whole
    // buffer leaves nothing for the lag to reach into.
    PitchSettings wholeBuffer;
    wholeBuffer.window = tone.frames();
    REQUIRE_FALSE(estimatePitch(tone.channel(0), tone.frames(), kRate, wholeBuffer));

    // Channel index, either side.
    REQUIRE_FALSE(trackPitch(tone.view(), kRate, {}, 1));
    REQUIRE_FALSE(trackPitch(tone.view(), kRate, {}, -1));
    REQUIRE(trackPitch(tone.view(), kRate, {}, 1).error().code() == ErrorCode::OutOfRange);

    // No sample rate, so no way to turn a lag into a frequency.
    REQUIRE_FALSE(trackPitch(tone.view(), SampleRate{0.0}));
    REQUIRE_FALSE(estimatePitch(tone.channel(0), tone.frames(), SampleRate{0.0}));

    // Settings that do not describe a range to search.
    PitchSettings inverted;
    inverted.minHz = 900.0;
    inverted.maxHz = 100.0;
    REQUIRE_FALSE(trackPitch(tone.view(), kRate, inverted));

    // A ceiling past Nyquist asks for a period of under two samples, which a
    // sampled signal does not have.
    PitchSettings pastNyquist;
    pastNyquist.maxHz = 30000.0;
    REQUIRE_FALSE(trackPitch(tone.view(), kRate, pastNyquist));

    PitchSettings noThreshold;
    noThreshold.threshold = 0.0;
    REQUIRE_FALSE(trackPitch(tone.view(), kRate, noThreshold));

    PitchSettings noHop;
    noHop.hop = 0;
    REQUIRE_FALSE(trackPitch(tone.view(), kRate, noHop));
}
