#pragma once

#include <sa/core/AudioBuffer.h>
#include <sa/core/Result.h>
#include <sa/core/Types.h>

namespace sa::dsp {

/// Taking mains hum out without taking the music with it.
///
/// Hum is the most avoidable fault in the catalogue and the most common: a
/// ground loop, an unbalanced run beside a power cable, a cheap supply. What
/// arrives is the mains frequency and a long series of its harmonics, steady
/// for the whole recording, sitting exactly where a bass line lives.
///
/// **Why this subtracts rather than notches.** The usual tool is a comb of
/// narrow notches at 50, 100, 150 Hz and so on. A notch removes everything at
/// its frequency -- the hum, and equally the note the bassist was playing. That
/// is a real cost on music and the reason de-humming has a reputation for
/// thinning a recording.
///
/// What is actually distinctive about hum is not its frequency but its
/// steadiness: it is the same sinusoid, at the same amplitude and phase, for
/// minutes at a time. So each harmonic's complex amplitude is measured over a
/// window and exactly that sinusoid is subtracted, leaving whatever else was at
/// that frequency untouched. A bass note at 50 Hz is not steady across a
/// window -- it starts, it decays, it has vibrato -- so only its steady part is
/// removed, which is a fraction of it, rather than all of it.
///
/// **What it does not fix.** Buzz from a dimmer or a switching supply is not a
/// harmonic series at a stable frequency; it is broadband and it moves. That is
/// what the noise profile and the denoiser are for.
struct DehumSettings {
    /// The mains frequency, in Hz. Zero asks it to find one, which it looks for
    /// near 50 and near 60 -- the two that exist -- and reports what it found.
    /// A recording made where the mains is 50 Hz can still be at 49.93, and a
    /// tenth of a Hertz matters by the fortieth harmonic, so the search is fine
    /// rather than a choice between two numbers.
    double frequency = 0.0;

    /// How many partials to take out, counting the fundamental. Zero means as
    /// many as fit below `highestHarmonicHz`.
    int harmonics = 0;

    /// Hum above this is inaudible against almost any programme, and estimating
    /// it costs as much as estimating the fundamental.
    double highestHarmonicHz = 4000.0;

    /// The window each partial's amplitude and phase are measured over for the
    /// subtraction itself, in samples. 32768 is about 680 ms at 48 kHz.
    ///
    /// Two things protect the music from being subtracted along with the hum,
    /// and this is the first. A window of N samples cannot separate two
    /// frequencies closer than roughly 2.5 * rate / N, so at 8192 the estimate
    /// of the fourth harmonic of 50 Hz also picks up anything within about
    /// 12 Hz of 200 -- which on real music is a note. Measured before this was
    /// raised: with a bass line whose partials sat 4 Hz from hum harmonics,
    /// de-humming left the recording seven times further from the original
    /// than the hum had. At 32768 the reach is under 4 Hz.
    ///
    /// The second is that each block's estimate is averaged with its
    /// neighbours' before anything is subtracted. Hum has the same complex
    /// amplitude in every block, because the phase is measured against
    /// absolute time, so it survives that averaging unchanged; a note at the
    /// same frequency does not, and averages away. Resolution narrows the
    /// problem and coherence finishes it.
    ///
    /// Mains hum is stable enough that the long window costs nothing: it
    /// drifts by hundredths of a Hertz over minutes, not over two-thirds of a
    /// second.
    int blockSize = 32768;

    /// How much of each measured partial to remove, 0 to 1. Below 1 leaves some
    /// of the hum, which is occasionally what a user wants when the removal
    /// takes something with it.
    double amount = 1.0;
};

/// How many partials the search looks at when deciding what the hum frequency
/// is, and the range it looks in. Not settings: they describe the electricity
/// supply rather than a preference.
inline constexpr int kHumSearchHarmonics = 8;
/// A grid is held within a few hundredths of a Hertz of nominal in normal
/// operation and within half a Hertz under stress, so a Hertz either way covers
/// every supply and leaves margin for a generator. Widening it further would
/// only cost search time.
inline constexpr double kHumSearchSpread = 1.0;

/// How much a partial's level is allowed to move across the recording and still
/// count as hum, as a coefficient of variation.
///
/// This is the criterion that actually separates hum from music, and it is the
/// one the first version lacked. Prominence -- standing above the spectrum
/// either side -- says a partial is there; it does not say what kind of thing
/// it is, and a sustained note stands out exactly as well as hum does. What is
/// different about hum is that it does not change: the same amplitude for
/// minutes, while a note starts, decays, and is played with vibrato.
///
/// A perfectly steady synthetic tone is therefore indistinguishable from hum by
/// any test, including this one, because it genuinely is the same thing. That
/// is a statement about the test signal rather than a limitation to work
/// around.
inline constexpr double kSteadinessLimit = 0.35;

struct DehumReport {
    /// True when a harmonic series stood out clearly enough to act on. False
    /// means nothing was changed.
    bool found = false;

    /// The fundamental used, in Hz -- either the one asked for or the one
    /// found.
    double frequency = 0.0;

    /// Partials removed.
    int harmonics = 0;

    /// How much energy came out, in dB relative to the input. A large figure on
    /// a recording that did not sound hummy is a sign the detection has locked
    /// onto programme material rather than hum.
    double removedDb = 0.0;

    /// How far the harmonic series stood above the spectrum around it, averaged
    /// over the partials the search looked at. This is part of what `found` is
    /// decided on, and it is reported so that a borderline case can be seen
    /// rather than guessed at.
    double prominence = 0.0;
};

/// Remove mains hum from `audio`, in place.
[[nodiscard]] Result<DehumReport> dehum(AudioBufferView audio, SampleRate rate,
                                        const DehumSettings& settings = {});

} // namespace sa::dsp
