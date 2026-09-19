#include <sa/ui/AnalysisReadout.h>

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>

using namespace sa;
using namespace sa::ui;

namespace {

/// Whether `text` contains `fragment`. The assertions below check that a
/// caveat says the thing that matters, not that it is worded exactly one way.
[[nodiscard]] bool mentions(const std::string& text, const std::string& fragment) {
    return text.find(fragment) != std::string::npos;
}

/// A key estimate whose fields say what each test is about and nothing else.
[[nodiscard]] analysis::KeyEstimate keyOf(double strength, double contrast, double cents = 0.0) {
    analysis::KeyEstimate estimate;
    estimate.valid = true;
    estimate.tonic = 6; // F#
    estimate.mode = analysis::Mode::Minor;
    estimate.strength = strength;
    estimate.contrast = contrast;
    estimate.fit = contrast > 0.0 ? strength / contrast : strength;
    estimate.tuningOffsetCents = cents;
    estimate.runnerUpTonic = 9; // A, the relative major of F# minor.
    estimate.runnerUpMode = analysis::Mode::Major;
    estimate.margin = 0.04;
    return estimate;
}

[[nodiscard]] analysis::BeatGrid tempoOf(double bpm, double confidence) {
    analysis::BeatGrid grid;
    grid.valid = true;
    grid.bpm = bpm;
    grid.confidence = confidence;
    grid.firstBeatSeconds = 0.021;
    grid.beatSeconds = {0.021, 0.521, 1.021};
    return grid;
}

[[nodiscard]] analysis::PitchPoint voiced(double hz) {
    analysis::PitchPoint point;
    point.hz = hz;
    point.voiced = true;
    point.confidence = 0.9;
    return point;
}

[[nodiscard]] analysis::PitchPoint unvoiced() {
    analysis::PitchPoint point;
    point.confidence = 0.2;
    return point;
}

} // namespace

TEST_CASE("A key on a near-flat chroma is refused rather than named") {
    // KeyDetect.h's own figure: under 0.15 of contrast is material with no key
    // in it. Naming the best-fitting of twenty-four keys anyway -- even with a
    // warning -- still puts a key on screen for someone to read off in a hurry.
    const Reading reading = keyReading(keyOf(0.9, 0.10));
    REQUIRE(reading.certainty == Certainty::None);
    REQUIRE(reading.value == "no key");
    REQUIRE(mentions(reading.caveat, "near-evenly"));
}

TEST_CASE("A key with too little strength behind it is refused rather than named") {
    const Reading reading = keyReading(keyOf(0.18, 0.9));
    REQUIRE(reading.certainty == Certainty::None);
    REQUIRE(reading.value == "no key");
    REQUIRE(mentions(reading.caveat, "0.18"));
}

TEST_CASE("A key with a weak fit is named with the reason to doubt it") {
    const Reading reading = keyReading(keyOf(0.35, 0.9));
    REQUIRE(reading.certainty == Certainty::Doubtful);
    REQUIRE(reading.value == "F# minor");
    REQUIRE(mentions(reading.caveat, "weak fit"));
    REQUIRE(mentions(reading.caveat, "0.35"));
}

TEST_CASE("A key with a strong fit at concert pitch is shown plainly") {
    const Reading reading = keyReading(keyOf(0.8, 0.95));
    REQUIRE(reading.certainty == Certainty::Firm);
    REQUIRE(reading.value == "F# minor");
    REQUIRE(reading.caveat.empty());
}

TEST_CASE("A key from a recording far from A = 440 is doubtful however well it fits") {
    // The detuning is a fact about the recording rather than about the fit:
    // every pitch class is smeared into its neighbour before the profiles see
    // any of it, so a confident answer on a smeared chroma is still doubtful.
    const Reading reading = keyReading(keyOf(0.85, 0.95, -40.0));
    REQUIRE(reading.certainty == Certainty::Doubtful);
    REQUIRE(reading.value == "F# minor");
    REQUIRE(mentions(reading.caveat, "-40 cents"));
    REQUIRE(mentions(reading.caveat, "A = 440"));
}

TEST_CASE("A detuning inside the threshold is reported without a caveat") {
    const Reading reading = keyReading(keyOf(0.85, 0.95, 20.0));
    REQUIRE(reading.certainty == Certainty::Firm);
    REQUIRE(reading.caveat.empty());
    REQUIRE(tuningReading(keyOf(0.85, 0.95, 20.0)) == "+20 cents");
}

TEST_CASE("Both reasons to doubt a key are shown together") {
    const Reading reading = keyReading(keyOf(0.3, 0.9, 45.0));
    REQUIRE(reading.certainty == Certainty::Doubtful);
    REQUIRE(mentions(reading.caveat, "weak fit"));
    REQUIRE(mentions(reading.caveat, "cents"));
}

TEST_CASE("A key analysis that could not run says so rather than reporting no key") {
    // "The audio is silent" and "there is no key in this" are different
    // statements and a panel that conflated them would be making one of them
    // up.
    const Reading reading = keyReading(analysis::KeyEstimate{}, "the audio is silent");
    REQUIRE(reading.certainty == Certainty::None);
    REQUIRE(reading.value == "--");
    REQUIRE(reading.caveat == "the audio is silent");
}

TEST_CASE("The runner-up names the expected confusion and how close it was") {
    // Relative major and minor share all seven notes, so this is the mistake
    // to expect and the margin is how nearly it was made.
    REQUIRE(runnerUpReading(keyOf(0.8, 0.9)) == "A major, 0.04 behind");
}

TEST_CASE("A tempo that was refused is reported as an answer, not as a gap") {
    const Reading reading = tempoReading(analysis::BeatGrid{});
    REQUIRE(reading.certainty == Certainty::None);
    REQUIRE(reading.value == "no tempo");
    REQUIRE(mentions(reading.caveat, "rhythmic"));
}

TEST_CASE("A weakly periodic tempo is shown with the reason to check it") {
    const Reading reading = tempoReading(tempoOf(119.8, 0.22));
    REQUIRE(reading.certainty == Certainty::Doubtful);
    REQUIRE(reading.value == "119.8 BPM");
    REQUIRE(mentions(reading.caveat, "0.22"));
    // The caveat sends the reader to the grid over the waveform, which is the
    // only thing that can settle it.
    REQUIRE(mentions(reading.caveat, "beat grid"));
}

TEST_CASE("A strongly periodic tempo is shown plainly") {
    const Reading reading = tempoReading(tempoOf(120.0, 0.86));
    REQUIRE(reading.certainty == Certainty::Firm);
    REQUIRE(reading.value == "120.0 BPM");
    REQUIRE(reading.caveat.empty());
}

TEST_CASE("A contour nobody asked for is not an empty contour") {
    const Reading reading = pitchReading({}, false);
    REQUIRE(reading.certainty == Certainty::None);
    REQUIRE(mentions(reading.caveat, "no contour"));
}

TEST_CASE("A contour with nothing periodic in it says so") {
    const Reading reading = pitchReading({unvoiced(), unvoiced(), unvoiced()}, true);
    REQUIRE(reading.certainty == Certainty::None);
    REQUIRE(reading.value == "none");
    REQUIRE(mentions(reading.caveat, "nothing periodic"));
}

TEST_CASE("A contour reports the median of its voiced frames in hertz") {
    // Sorted, the five voiced readings are 220, 440, 441, 442, 880, and the
    // middle one is 441. Deliberately hertz rather than a note name:
    // PitchTrack.h finds a period and says it does not map one to a note.
    const Reading reading = pitchReading(
        {voiced(440.0), voiced(442.0), voiced(441.0), voiced(880.0), voiced(220.0)}, true);
    REQUIRE(reading.certainty == Certainty::Firm);
    REQUIRE(reading.value == "441.0 Hz");
}

TEST_CASE("A contour that is mostly gaps says what the median was taken over") {
    // Two voiced frames out of twenty is 10%, which is under the quarter at
    // which three frames in four have no pitch in them.
    std::vector<analysis::PitchPoint> contour(18, unvoiced());
    contour.push_back(voiced(300.0));
    contour.push_back(voiced(302.0));
    const Reading reading = pitchReading(contour, true);
    REQUIRE(reading.certainty == Certainty::Doubtful);
    REQUIRE(reading.value == "302.0 Hz");
    REQUIRE(mentions(reading.caveat, "10%"));
}

TEST_CASE("A room figure the decay could not support is a dash and never a number") {
    REQUIRE(roomSeconds(false, 1.234) == "--");
    REQUIRE(roomSeconds(true, 0.41234) == "0.412 s");
    // Zero is the value the field is left at when its flag is false, and it
    // would read as a very dead room.
    REQUIRE(roomSeconds(false, 0.0) == "--");
}

TEST_CASE("Something that is not an impulse response is refused rather than measured") {
    const Reading reading = roomReading(analysis::RoomAcoustics{}, true);
    REQUIRE(reading.certainty == Certainty::None);
    REQUIRE(reading.value == "--");
    REQUIRE(mentions(reading.caveat, "not an impulse response"));
}

TEST_CASE("A decay with room for T20 and not T30 says which") {
    analysis::RoomAcoustics room;
    room.valid = true;
    room.hasEarlyDecay = true;
    room.hasT20 = true;
    room.hasT30 = false;
    room.usableRangeDb = 27.0;
    const Reading reading = roomReading(room, true);
    REQUIRE(reading.certainty == Certainty::Doubtful);
    REQUIRE(reading.value == "27.0 dB");
    REQUIRE(mentions(reading.caveat, "supports T20 but not T30"));
}

TEST_CASE("A decay with room for neither reverberation time says so once") {
    analysis::RoomAcoustics room;
    room.valid = true;
    room.hasEarlyDecay = true;
    room.usableRangeDb = 14.0;
    const Reading reading = roomReading(room, true);
    REQUIRE(reading.certainty == Certainty::Doubtful);
    REQUIRE(mentions(reading.caveat, "neither T20 nor T30"));
}

TEST_CASE("A decay with room for everything is shown plainly") {
    analysis::RoomAcoustics room;
    room.valid = true;
    room.hasEarlyDecay = true;
    room.hasT20 = true;
    room.hasT30 = true;
    room.usableRangeDb = 48.0;
    const Reading reading = roomReading(room, true);
    REQUIRE(reading.certainty == Certainty::Firm);
    REQUIRE(reading.caveat.empty());
}

TEST_CASE("A bounded read admits to what it did not look at") {
    MusicalAnalysis result;
    result.rate = SampleRate{48000.0};
    result.analysedFrames = 120 * 48000;
    result.requestedFrames = 313 * 48000; // 5:13.
    REQUIRE(coverageNote(result) == "first 2:00 of 5:13");
}

TEST_CASE("A read that covered everything has nothing to admit to") {
    MusicalAnalysis result;
    result.rate = SampleRate{48000.0};
    result.analysedFrames = 30 * 48000;
    result.requestedFrames = 30 * 48000;
    REQUIRE(coverageNote(result).empty());
}
