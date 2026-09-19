#pragma once

#include <sa/analysis/KeyDetect.h>
#include <sa/analysis/OctaveBands.h>
#include <sa/analysis/PitchTrack.h>
#include <sa/analysis/RoomAcoustics.h>
#include <sa/analysis/TempoTrack.h>
#include <sa/core/Types.h>

#include <string>
#include <vector>

/// What the analysis panel is allowed to say, given what was actually measured.
///
/// The result of a run and the rules for printing it live in one header
/// deliberately. Every module under sa-analysis takes care to report what it
/// could not establish -- a tempo that is `valid == false`, a T30 the decay had
/// no range for, a key whose chroma is nearly flat -- and all of that care is
/// undone by a panel that prints the number next to the flag and leaves the
/// reader to notice. So the struct that carries a result and the functions that
/// turn it into text are the same piece of work, and the text is decided here,
/// away from any window, where it can be checked by calling it.
///
/// None of this decides colour, position or wording of anything the analysis
/// modules already say for themselves; it decides whether there is an answer to
/// show at all, and what has to be shown beside it when there is.
///
/// What it does not claim: the thresholds below are display decisions, not
/// measurements and not anything the analysis modules endorse. Each one is
/// justified where it is declared, and every one of them sits clear of the
/// refusal the module itself already makes.
namespace sa::ui {

/// Everything one run of the musical analysis found.
///
/// Flat, copyable and free of Qt, so that a worker thread can fill one, hand it
/// across to the main thread by value, and have nothing to tidy up if the
/// result turns out to have been superseded on the way.
struct MusicalAnalysis {
    SampleRate rate;

    /// Where in the document the analysis started, and how much of it was read.
    /// Both are in samples of the document, so a contour or a beat time can be
    /// put back on the timeline it came from.
    SampleIndex start = 0;
    SampleCount analysedFrames = 0;

    /// How long the range the user asked about actually is. Larger than
    /// `analysedFrames` when the read was bounded, which is the case the panel
    /// has to admit to rather than quietly answer a different question.
    SampleCount requestedFrames = 0;

    /// How much of the read the key was taken from. Less than the whole,
    /// because a key is settled long before a tempo is.
    SampleCount keyFrames = 0;

    analysis::KeyEstimate key;
    analysis::BeatGrid tempo;
    std::vector<analysis::Band> bands;

    /// The contour, and whether one was asked for at all. An empty contour
    /// that was requested means nothing periodic was found, which is an answer;
    /// an empty contour that was not requested is not.
    std::vector<analysis::PitchPoint> pitch;
    bool pitchRequested = false;

    /// The hop the contour was tracked at, in samples. Reported because a long
    /// selection is tracked more coarsely than a short one and the difference
    /// is the time resolution of everything drawn from it.
    SampleCount pitchHop = 0;

    analysis::RoomAcoustics room;
    bool roomRequested = false;

    /// Why a stage could not run at all, from the Result it returned. Distinct
    /// from a stage that ran and found nothing: "the audio is silent" is not
    /// the same statement as "there is no key in this".
    std::string keyError;
    std::string tempoError;
    std::string pitchError;
    std::string bandError;
    std::string roomError;

    [[nodiscard]] bool hasBeats() const noexcept {
        return tempo.valid && !tempo.beatSeconds.empty();
    }
};

/// How much weight a reading will bear, which is what decides how it is drawn.
enum class Certainty {
    /// There is an answer and nothing argues against it.
    Firm,
    /// There is an answer and a reason it may be wrong. Both are shown.
    Doubtful,
    /// There is no answer. `caveat` says why, and `value` is never a number.
    None,
};

/// One row of the panel: the answer, and what has to be read with it.
struct Reading {
    std::string value;
    std::string caveat;
    Certainty certainty = Certainty::None;

    [[nodiscard]] bool isDoubtful() const noexcept { return certainty != Certainty::Firm; }
};

/// Below this contrast the chroma is too flat for the key to mean anything.
///
/// KeyDetect.h's own figure: contrast runs "around 1 for tonal music and under
/// 0.15 for material with no key in it". Taking the module's number rather than
/// inventing one is the point -- it is the number the module was measured
/// against.
inline constexpr double kKeylessContrast = 0.15;

/// Below this strength no key is named at all.
///
/// Strength is the fit scaled by the contrast clamped at one, so material with
/// no key cannot exceed its own contrast whatever it scores on fit -- 0.15,
/// above. Refusing a little above that floor is what keeps the refusal from
/// resting on a single decimal place.
inline constexpr double kKeyRefusedBelow = 0.20;

/// At or above this, a key is shown plainly; between the two it is shown with
/// the reason to doubt it. Halfway between a chroma that fits nothing and the
/// 0.7 and upwards that well-behaved tonal material reaches.
inline constexpr double kKeyFirmAt = 0.50;

/// Beyond this much detuning the key is worth less, and says so.
///
/// A quarter of a semitone is 50 cents, at which point every pitch class is as
/// near its neighbour as its own centre and the chromagram is meaningless; half
/// of that is where the smearing is already doing real damage.
inline constexpr double kTuningFarCents = 25.0;

/// Below this the tempo is shown with a warning rather than plainly.
///
/// trackTempo() refuses outright below 0.15, so this is not a second refusal --
/// it is the band above the refusal, where the period explains something about
/// the record but not much, and where looking at the grid against the
/// transients is the only way to settle it.
inline constexpr double kTempoDoubtfulBelow = 0.35;

/// Below this fraction of voiced frames the contour is mostly gaps, and a
/// median taken over what is left is a median of very little. A quarter is the
/// point at which three frames in four had no pitch in them.
inline constexpr double kThinVoicing = 0.25;

/// The name of a pitch class, the key name, and everything else about the key
/// in one reading.
///
/// Refuses to name a key on a flat chroma or a weak fit. That refusal is the
/// whole reason this function exists: `detectKey` always returns its best
/// twenty-fourth, and a best twenty-fourth of nothing is still a key name.
[[nodiscard]] Reading keyReading(const analysis::KeyEstimate& estimate,
                                 const std::string& error = {});

/// The second-best key and how far behind it was, always shown when a key is:
/// relative major and minor share every note, so the runner-up is where the
/// expected mistake shows up.
[[nodiscard]] std::string runnerUpReading(const analysis::KeyEstimate& estimate);

/// The tuning offset, signed, in cents.
[[nodiscard]] std::string tuningReading(const analysis::KeyEstimate& estimate);

[[nodiscard]] Reading tempoReading(const analysis::BeatGrid& grid, const std::string& error = {});

/// The median of the voiced frames and how many there were.
///
/// Deliberately hertz and not a note name: PitchTrack.h finds a period and says
/// in as many words that it does not map one to a note, so a panel that printed
/// "A4" would be claiming something nothing measured.
[[nodiscard]] Reading pitchReading(const std::vector<analysis::PitchPoint>& contour, bool requested,
                                   const std::string& error = {});

/// Whether the record supports the room figures at all, and how far its decay
/// went before it ran into the noise.
[[nodiscard]] Reading roomReading(const analysis::RoomAcoustics& room, bool requested,
                                  const std::string& error = {});

/// A room figure in seconds, or "--" when its flag says the decay had no range
/// for it. Never a zero dressed as a measurement.
[[nodiscard]] std::string roomSeconds(bool measured, double seconds);

/// What was actually read, when that is less than what was asked for. Empty
/// when the whole range was analysed and there is nothing to admit to.
[[nodiscard]] std::string coverageNote(const MusicalAnalysis& result);

} // namespace sa::ui
