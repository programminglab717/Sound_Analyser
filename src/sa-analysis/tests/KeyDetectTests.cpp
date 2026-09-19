#include <sa/analysis/KeyDetect.h>

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <numbers>
#include <random>
#include <vector>

using namespace sa;
using namespace sa::analysis;
using Catch::Approx;

namespace {

constexpr SampleRate kRate{44100.0};

/// Equal temperament at A = 440, which is what the detector assumes. MIDI note
/// 69 is that A, and every semitone is a factor of 2^(1/12).
[[nodiscard]] double noteHz(int midi, double centsOffset = 0.0) {
    return 440.0 * std::pow(2.0, (static_cast<double>(midi) - 69.0 + centsOffset / 100.0) / 12.0);
}

/// One note, with harmonics, faded at both ends so the joins between notes are
/// not themselves broadband clicks.
void addNote(std::vector<float>& into, SampleCount at, SampleCount length, double hz,
             double amplitude) {
    const auto fade = std::min<SampleCount>(length / 8, static_cast<SampleCount>(kRate.hz() / 100));
    for (SampleCount i = 0; i < length; ++i) {
        const auto index = at + i;
        if (index >= static_cast<SampleCount>(into.size())) {
            break;
        }
        const double t = static_cast<double>(i) / kRate.hz();
        // Six harmonics at 1/h, which is a sawtooth's envelope and close enough
        // to a bowed or blown note to be a fair thing to fold.
        double value = 0.0;
        for (int harmonic = 1; harmonic <= 6; ++harmonic) {
            const double partial = hz * harmonic;
            if (partial > kRate.hz() * 0.45) {
                break;
            }
            value += std::sin(2.0 * std::numbers::pi * partial * t) / harmonic;
        }
        double envelope = 1.0;
        if (i < fade) {
            envelope = static_cast<double>(i) / static_cast<double>(fade);
        } else if (i > length - fade) {
            envelope = static_cast<double>(length - i) / static_cast<double>(fade);
        }
        into[static_cast<std::size_t>(index)] += static_cast<float>(value * amplitude * envelope);
    }
}

/// A chord progression, each chord given as semitone offsets from a root.
struct Chord {
    int root = 0;
    std::vector<int> intervals;
    int beats = 1;
};

[[nodiscard]] AudioBuffer play(const std::vector<Chord>& chords, int transpose = 0,
                               double centsOffset = 0.0, int repeats = 4) {
    const auto beat = static_cast<SampleCount>(kRate.hz() * 0.5);
    SampleCount total = 0;
    for (const auto& chord : chords) {
        total += beat * chord.beats;
    }
    total *= repeats;

    std::vector<float> samples(static_cast<std::size_t>(total), 0.0f);
    SampleCount cursor = 0;
    for (int repeat = 0; repeat < repeats; ++repeat) {
        for (const auto& chord : chords) {
            const auto length = beat * chord.beats;
            for (const int interval : chord.intervals) {
                addNote(samples, cursor, length,
                        noteHz(60 + transpose + chord.root + interval, centsOffset),
                        0.12 / static_cast<double>(chord.intervals.size()));
            }
            cursor += length;
        }
    }

    AudioBuffer out{ChannelLayout::mono(), total};
    std::copy(samples.begin(), samples.end(), out.channel(0));
    return out;
}

/// I - IV - V - I, with the tonic chord held twice as long as the others. This
/// is the shape the major profile is built to recognise: the tonic triad
/// present more than anything else, the rest of the scale present, nothing
/// outside it.
[[nodiscard]] std::vector<Chord> majorProgression() {
    return {{0, {0, 4, 7, 12}, 2}, {5, {0, 4, 7}, 1}, {7, {0, 4, 7}, 1}, {0, {0, 4, 7, 12}, 2}};
}

/// i - iv - V - i in the minor. The V is major -- that is the harmonic minor's
/// leading note, and it is what distinguishes a minor key from the major a
/// minor third above it, which otherwise uses exactly the same seven notes.
[[nodiscard]] std::vector<Chord> minorProgression() {
    return {{0, {0, 3, 7, 12}, 2}, {5, {0, 3, 7}, 1}, {7, {0, 4, 7}, 1}, {0, {0, 3, 7, 12}, 2}};
}

} // namespace

TEST_CASE("A major progression is heard as major, on the right tonic", "[analysis][key]") {
    // Transposed to C, which is MIDI 60 and pitch class 0.
    const AudioBuffer audio = play(majorProgression());
    const auto estimate = detectKey(audio.view(), kRate);
    REQUIRE(estimate);
    REQUIRE(estimate.value().valid);
    REQUIRE(estimate.value().tonic == 0);
    REQUIRE(estimate.value().mode == Mode::Major);
    REQUIRE(keyName(estimate.value()) == "C major");
    // A clear answer, not a coin toss between two: the chroma is strongly
    // shaped and it fits the profile well.
    REQUIRE(estimate.value().contrast > 0.8);
    REQUIRE(estimate.value().fit > 0.9);
    REQUIRE(estimate.value().strength > 0.7);
}

TEST_CASE("A minor progression is heard as minor, not as its relative major", "[analysis][key]") {
    // Rooted on A, MIDI 69, pitch class 9. A minor and C major share all seven
    // natural notes, so this is the confusion the detector exists to avoid,
    // and getting it right rests on the tonic triad and the leading note
    // rather than on which notes appear at all.
    const AudioBuffer audio = play(minorProgression(), 9);
    const auto estimate = detectKey(audio.view(), kRate);
    REQUIRE(estimate);
    REQUIRE(estimate.value().tonic == 9);
    REQUIRE(estimate.value().mode == Mode::Minor);
    REQUIRE(keyName(estimate.value()) == "A minor");
}

TEST_CASE("Transposing the music transposes the answer", "[analysis][key]") {
    // The strongest thing that can be checked without trusting the profile:
    // whatever the detector thinks C major sounds like, moving the music up by
    // n semitones has to move the answer by n as well. Any error in the
    // rotation, the fold or the pitch-class arithmetic breaks this for some n
    // even if it happens to be right for zero.
    for (int semitones = 0; semitones < 12; ++semitones) {
        const AudioBuffer audio = play(majorProgression(), semitones);
        const auto estimate = detectKey(audio.view(), kRate);
        REQUIRE(estimate);
        INFO("transposed by " << semitones);
        REQUIRE(estimate.value().tonic == semitones % 12);
        REQUIRE(estimate.value().mode == Mode::Major);
    }
}

TEST_CASE("And transposing a minor progression transposes it too", "[analysis][key]") {
    for (int semitones = 0; semitones < 12; ++semitones) {
        const AudioBuffer audio = play(minorProgression(), semitones);
        const auto estimate = detectKey(audio.view(), kRate);
        REQUIRE(estimate);
        INFO("transposed by " << semitones);
        REQUIRE(estimate.value().tonic == semitones % 12);
        REQUIRE(estimate.value().mode == Mode::Minor);
    }
}

TEST_CASE("A single note puts its energy on one pitch class", "[analysis][key]") {
    // The chromagram on its own, checked against the one thing about it that
    // is not a matter of degree: a sustained A4 is pitch class 9, and its
    // harmonics are A, E, A, C#, E -- pitch classes 9, 4, 9, 1, 4 -- so those
    // three should hold nearly all of it and the other nine nearly none.
    std::vector<float> samples(static_cast<std::size_t>(kRate.hz() * 2.0), 0.0f);
    addNote(samples, 0, static_cast<SampleCount>(samples.size()), noteHz(69), 0.3);
    AudioBuffer audio{ChannelLayout::mono(), static_cast<SampleCount>(samples.size())};
    std::copy(samples.begin(), samples.end(), audio.channel(0));

    const auto chroma = chromagram(audio.view(), kRate);
    REQUIRE(chroma);
    const auto& weights = chroma.value();

    double sum = 0.0;
    for (const double value : weights) {
        sum += value;
    }
    REQUIRE(sum == Approx(1.0).epsilon(0.001));

    REQUIRE(weights[9] > 0.4);
    const double harmonics = weights[9] + weights[4] + weights[1];
    REQUIRE(harmonics > 0.9);
    // The tonic's own class is the largest of them: the fundamental and the
    // second and fourth harmonics all land there, and nothing else does.
    REQUIRE(std::max_element(weights.begin(), weights.end()) - weights.begin() == 9);
}

TEST_CASE("Tuning that is not A = 440 is reported rather than hidden", "[analysis][key]") {
    // Thirty cents sharp. The key should survive it -- thirty cents is under a
    // third of a semitone, so nothing crosses into its neighbour -- but the
    // offset has to be reported, because at fifty it would and the caller
    // needs to know which side of that they are on.
    const AudioBuffer audio = play(majorProgression(), 0, 30.0);
    const auto estimate = detectKey(audio.view(), kRate);
    REQUIRE(estimate);
    REQUIRE(estimate.value().tonic == 0);
    // Wide, because a note's harmonics are not all at the same offset in cents
    // once the window smears them, and because the measure is an energy-
    // weighted circular mean over every bin in range rather than a reading off
    // the fundamental.
    REQUIRE(estimate.value().tuningOffsetCents == Approx(30.0).margin(12.0));

    // In tune reads as in tune, which is the half of it that is easy to get
    // wrong by leaving a bias in.
    const AudioBuffer reference = play(majorProgression());
    const auto inTune = detectKey(reference.view(), kRate);
    REQUIRE(inTune);
    REQUIRE(inTune.value().tuningOffsetCents == Approx(0.0).margin(12.0));
}

TEST_CASE("Music with no key in it does not produce a confident one", "[analysis][key]") {
    // All twelve notes, equally. There is no right answer, and the honest
    // behaviour is a low strength and a small margin rather than a refusal:
    // the caller asked what key it is closest to, and something always is.
    std::vector<Chord> chromatic;
    for (int semitone = 0; semitone < 12; ++semitone) {
        chromatic.push_back({semitone, {0}, 1});
    }
    const AudioBuffer audio = play(chromatic, 0, 0.0, 2);
    const auto estimate = detectKey(audio.view(), kRate);
    REQUIRE(estimate);
    REQUIRE(estimate.value().valid);

    // The chroma is nearly flat, and that is the fact the answer has to
    // reflect. Measured: a standard deviation under a tenth of the mean,
    // against roughly one times the mean for the tonal case below.
    REQUIRE(estimate.value().contrast < 0.15);
    REQUIRE(estimate.value().strength < 0.15);

    // The bare correlation, by contrast, is *not* low -- it reads around 0.65
    // here, because correlating centred profiles asks only which way the
    // chroma leans and not how far. That is exactly why `strength` is not the
    // correlation, and this assertion pins the reason down rather than
    // leaving it as a remark in a header.
    REQUIRE(estimate.value().fit > 0.4);

    // And against the major progression, which is the comparison that gives
    // the number meaning: whatever "low" is, it has to be far below that.
    const AudioBuffer tonal = play(majorProgression());
    const auto confident = detectKey(tonal.view(), kRate);
    REQUIRE(confident);
    REQUIRE(confident.value().contrast > 6.0 * estimate.value().contrast);
    REQUIRE(estimate.value().strength < confident.value().strength - 0.5);
}

TEST_CASE("The runner-up is reported, and is the key you would expect", "[analysis][key]") {
    // C major's nearest neighbours are the keys sharing most of its notes:
    // A minor, which shares all seven, and G major or F major, which share
    // six. Whichever wins second place should be one of those rather than
    // something a tritone away.
    const AudioBuffer audio = play(majorProgression());
    const auto estimate = detectKey(audio.view(), kRate);
    REQUIRE(estimate);
    const int runnerUp = estimate.value().runnerUpTonic;
    INFO("runner-up was " << pitchClassName(runnerUp)
                          << (estimate.value().runnerUpMode == Mode::Major ? " major" : " minor"));
    // Related, by one of the three ways a key can be: the parallel minor
    // (same tonic and fifth, which are the two degrees this profile weights
    // most), the relative minor (the same seven notes), or a neighbour on the
    // circle of fifths at 5 or 7 semitones (six notes in common).
    const bool related = runnerUp == 0 || runnerUp == 9 || runnerUp == 5 || runnerUp == 7;
    REQUIRE(related);
    // And emphatically not the tritone, which shares the least with C of any
    // of the twelve and is where a rotation error would put it.
    REQUIRE(runnerUp != 6);
    REQUIRE(estimate.value().margin > 0.0);
}

TEST_CASE("Pitch classes are named the way a musician writes them", "[analysis][key]") {
    REQUIRE(pitchClassName(0) == "C");
    REQUIRE(pitchClassName(9) == "A");
    REQUIRE(pitchClassName(11) == "B");
    REQUIRE(pitchClassName(-1).empty());
    REQUIRE(pitchClassName(12).empty());

    KeyEstimate estimate;
    estimate.valid = false;
    REQUIRE(keyName(estimate).empty());
    estimate.valid = true;
    estimate.tonic = 6;
    estimate.mode = Mode::Minor;
    REQUIRE(keyName(estimate) == "F# minor");
}

TEST_CASE("What cannot be analysed is refused", "[analysis][key]") {
    const AudioBuffer audio = play(majorProgression());

    REQUIRE_FALSE(detectKey(audio.view(), kRate, {}, 1));
    REQUIRE_FALSE(detectKey(audio.view(), kRate, {}, -1));
    REQUIRE_FALSE(detectKey(audio.view(), SampleRate{0.0}));

    KeySettings settings;
    settings.fftSize = 3000; // Not a power of two.
    REQUIRE_FALSE(detectKey(audio.view(), kRate, settings));

    settings = {};
    settings.hop = 0;
    REQUIRE_FALSE(detectKey(audio.view(), kRate, settings));

    settings = {};
    settings.lowHz = 5000.0;
    settings.highHz = 65.0;
    REQUIRE_FALSE(detectKey(audio.view(), kRate, settings));

    // Shorter than one window.
    const AudioBuffer brief{ChannelLayout::mono(), 1000};
    REQUIRE_FALSE(detectKey(brief.view(), kRate));

    // Silence is long enough to analyse and has nothing in it, which is a
    // different failure and gets a different message.
    const AudioBuffer silence{ChannelLayout::mono(), 100000};
    REQUIRE_FALSE(detectKey(silence.view(), kRate));
    REQUIRE_FALSE(chromagram(silence.view(), kRate));
}
