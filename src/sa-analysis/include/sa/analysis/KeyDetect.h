#pragma once

#include <sa/core/AudioBuffer.h>
#include <sa/core/Result.h>
#include <sa/core/Types.h>

#include <array>
#include <string_view>

/// Working out what key a piece of music is in.
///
/// Two steps, and the first is the one that carries the weight. A chromagram
/// folds the spectrum onto the twelve pitch classes, throwing away which octave
/// a note was in and keeping only which note it was; averaged over a piece,
/// that gives a profile of how much of each of the twelve was played. The
/// second step compares that profile against what each of the twenty-four keys
/// would be expected to produce and takes the closest.
///
/// What this is not: the published probe-tone profiles from the music
/// psychology literature. Those are experimental data and would very likely do
/// better on real music, but transcribing a table of constants from memory is
/// how a tool ends up quietly wrong, and this project does not do it. The
/// profiles here are written from the theory instead -- the tonic triad
/// weighted above the rest of the scale, the scale above the notes outside it
/// -- and they are stated in full in the source so that anyone comparing this
/// against a tool they trust can see exactly what it is comparing against.
///
/// Known limits, none of them hidden:
///
/// - It assumes twelve-tone equal temperament at A = 440 Hz. A recording a
///   quarter-tone flat smears every pitch class into its neighbour and the
///   answer degrades; `tuningOffsetCents` reports what it found so a caller can
///   see when that has happened.
/// - It reports one key for the whole passage. Music that modulates has more
///   than one, and this will return whichever dominates.
/// - Relative major and minor share all seven notes, so telling them apart
///   rests on which of them is emphasised rather than which notes occur. That
///   is the confusion to expect, and `runnerUp` will usually name it.
namespace sa::analysis {

enum class Mode { Major, Minor };

struct KeySettings {
    int fftSize = 4096;
    SampleCount hop = 2048;

    /// The range folded into the chromagram. Below the bottom of this a bass
    /// note's own partials dominate its fundamental, and above the top there is
    /// little but the harmonics of notes already counted.
    double lowHz = 65.0;
    double highHz = 5000.0;
};

struct KeyEstimate {
    /// Below this contrast there is no key in the material to find, and the
    /// best-fitting profile is fitting noise. Naming one anyway -- even beside
    /// a warning -- leaves a key on screen for someone to read off in a hurry.
    static constexpr double kKeylessContrast = 0.15;

    /// Below this strength the answer should not be named either. Strength is
    /// fit scaled by contrast, so keyless material cannot exceed its own
    /// contrast whatever it scores on fit; refusing a little above the floor
    /// keeps the refusal off a single decimal place.
    static constexpr double kRefuseBelowStrength = 0.20;

    /// Above this, the key is worth stating plainly. Between this and
    /// kRefuseBelowStrength it should be named but qualified.
    static constexpr double kFirmStrength = 0.50;

    /// Beyond this much detuning the chroma was smeared before any profile saw
    /// it, so the answer is worth less however well it fits.
    static constexpr double kFarFromConcertPitchCents = 25.0;

    /// Whether there is a key here worth naming at all. Every caller that
    /// prints a key name is expected to ask this first: these thresholds used
    /// to be written out again in each of them, and the window and the command
    /// line had already drifted into disagreeing about the same recording.
    [[nodiscard]] constexpr bool worthNaming() const noexcept {
        return contrast >= kKeylessContrast && strength >= kRefuseBelowStrength;
    }

    /// Pitch class of the tonic: 0 is C, 1 is C sharp, and so on.
    int tonic = 0;
    Mode mode = Mode::Major;

    /// How well the winning profile matched the *shape* of the chroma, from
    /// -1 to 1. A correlation and no more than that: it is invariant to how
    /// pronounced the chroma is, so a piece using all twelve notes evenly can
    /// still score 0.65 here on whichever way its noise happens to lean. Do
    /// not threshold on this; it is reported because it is what `margin` is a
    /// difference of.
    double fit = 0.0;

    /// How much structure the chroma has: its standard deviation over its
    /// mean. Around 1 for tonal music and under 0.15 for material with no key
    /// in it. Dimensionless, so it does not move with the recording level.
    double contrast = 0.0;

    /// The figure to threshold on: `fit` scaled by `contrast`, the latter
    /// clamped at 1. A chroma whose variation equals its mean is as shaped as
    /// a key profile itself is, and being more lopsided than that does not
    /// make the key any more certain -- so past that point the fit is all
    /// that remains. Below it, a good fit to a nearly flat chroma is what it
    /// sounds like, and is discounted accordingly.
    ///
    /// This is not a probability. It is a number that is high when the music
    /// has a key and low when it does not.
    double strength = 0.0;

    /// The second-best key, and how far behind it was. A small margin against
    /// the relative minor means something different from a small margin
    /// against an unrelated key, so both are reported rather than one number.
    int runnerUpTonic = 0;
    Mode runnerUpMode = Mode::Major;
    double margin = 0.0;

    /// Mean deviation of the energy from equal-tempered centres, in cents,
    /// over the range -50 to 50. Far from zero means the recording is not at
    /// A = 440 and the answer above is worth less.
    double tuningOffsetCents = 0.0;

    /// The twelve pitch-class weights the answer was drawn from, normalised to
    /// sum to one. Exposed because a caller looking at a doubtful answer wants
    /// to see what it was looking at.
    std::array<double, 12> chroma{};

    bool valid = false;
};

/// The name of a pitch class, sharps rather than flats throughout.
[[nodiscard]] std::string_view pitchClassName(int pitchClass) noexcept;

/// "F# minor", or an empty view if the estimate is not valid.
[[nodiscard]] std::string keyName(const KeyEstimate& estimate);

/// The averaged chromagram on its own, normalised to sum to one.
///
/// Separately available because it is what a caller wants when the key is not
/// the question: which notes a passage uses is a fair thing to ask without
/// asking what key they add up to.
[[nodiscard]] Result<std::array<double, 12>> chromagram(ConstAudioBufferView audio, SampleRate rate,
                                                        const KeySettings& settings = {},
                                                        int channel = 0);

[[nodiscard]] Result<KeyEstimate> detectKey(ConstAudioBufferView audio, SampleRate rate,
                                            const KeySettings& settings = {}, int channel = 0);

} // namespace sa::analysis
