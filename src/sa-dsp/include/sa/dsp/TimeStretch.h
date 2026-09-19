#pragma once

#include <sa/core/AudioBuffer.h>
#include <sa/core/Result.h>
#include <sa/core/Types.h>
#include <sa/dsp/Resampler.h>
#include <sa/dsp/Window.h>

namespace sa::dsp {

/// Changing how long something lasts without changing what it sounds like, and
/// changing what it sounds like without changing how long it lasts.
///
/// Both are the same machine. A phase vocoder analyses the signal into
/// overlapping short spectra, walks through those spectra at a different rate
/// than they were taken at, and adds them back up; stretching falls out
/// directly, and pitch shifting is a stretch followed by a resample that undoes
/// the length change and takes the pitch with it.
///
/// Neither function needs the sample rate. A stretch is a ratio and a shift is
/// a ratio of frequencies, so the same call does the same thing to 44.1 kHz and
/// to 192 kHz, and there is no rate to get wrong.
///
/// **What a phase vocoder is bad at, stated up front.** It assumes each bin
/// holds one steady sinusoid. Where that is true -- held notes, speech vowels,
/// room tone -- it is transparent well past 2x. Where it is not, it smears:
/// a drum hit stretched to twice its length is a drum hit with a tail on it,
/// because the transient's energy is spread across a window that is now played
/// out over twice as long. Transient preservation is a separate piece of work
/// and is not here yet. For the corrective work this product is for -- fitting
/// a take to a slot, pulling a flat note up, matching two recordings -- the
/// ratios in use are close to 1 and it is not the limiting factor.
namespace stretch {

/// Ratio limits. Past these a phase vocoder is not producing a slower version
/// of the material, it is producing a different piece of material, and the
/// honest thing is to refuse rather than to return something unusable.
inline constexpr double kMinimumFactor = 0.1;
inline constexpr double kMaximumFactor = 10.0;

/// Shift limits, in semitones. Three octaves each way -- which is already past
/// the point where anything sounds like what it was, and is what the stretch
/// limits above allow, since a shift is a stretch underneath.
inline constexpr double kMinimumSemitones = -36.0;
inline constexpr double kMaximumSemitones = 36.0;

} // namespace stretch

struct StretchSettings {
    /// Output length divided by input length. 2.0 takes twice as long, 0.5
    /// half as long.
    double factor = 1.0;

    /// 2048 with a hop of 256 -- an eighth of the window -- is the setting this
    /// was tuned at. The window is a compromise: long enough to resolve
    /// partials that are close together, short enough not to average over a
    /// note change. The overlap is deliberately more generous than the 4x that
    /// reconstruction alone requires, because a phase vocoder's errors are
    /// per-frame and averaging more frames over each output sample buries them.
    ///
    /// **The window sets a floor on how close two partials can be.** A Hann
    /// window's main lobe is four bins wide, so partials nearer than that are
    /// not two peaks -- they are one lump, and the machinery below tracks it as
    /// one thing with one frequency. At 2048 and 48 kHz that limit is about
    /// 94 Hz, which is wider than a semitone anywhere below roughly 1.6 kHz.
    /// Measured on a close A major triad -- 220, 277 and 330 Hz, two and a bit
    /// bins apart -- the three notes came back at 0.250, 0.182 and 0.216 where
    /// all three went in at 0.250, with 7.9% of the power astray. At 8192 the
    /// same triad is resolved and comes back at 0.250, 0.250 and 0.250 with
    /// nothing astray.
    ///
    /// So: a voice, a line, a mix with space in it, 2048. Dense low harmony,
    /// raise it, and accept that the transients will smear further for it.
    /// That trade is what an FFT size is, and there is no setting that escapes
    /// it.
    int fftSize = 2048;
    int hopSize = 256;
    WindowType window = WindowType::Hann;

    /// Lock every bin of a partial to the partial's own peak.
    ///
    /// This is not a refinement, and the default is not a preference. Without
    /// it a 440 Hz tone at 0.25 stretched to twice its length comes out at
    /// 0.129 -- 5.7 dB of the signal simply gone -- and with it, at 0.250.
    ///
    /// The reason is worth writing down, because "phasiness" is usually where
    /// the explanation stops. A sinusoid occupies three or four bins, and in
    /// the source those bins hold a fixed phase relationship: for a tone on a
    /// bin centre the neighbours sit exactly pi away from it. While a note is
    /// steady, every one of its bins measures the same advance, so letting
    /// each run on its own measurement preserves that relationship. But a note
    /// has to start, and during an onset the bins genuinely do evolve
    /// differently -- so each one comes out of the onset with a different
    /// constant phase offset, which nothing afterwards corrects. Measured on
    /// a tone sitting on bin 18, the three bins left the onset skewed by
    /// +0.64, +0.06 and -0.52 radians and held those offsets to the end of the
    /// file. Bins that should have cancelled into a clean sinusoid instead
    /// cancelled part of it away.
    ///
    /// Locking re-imposes the source's relationship every frame, so an onset
    /// cannot leave a permanent skew. It costs a peak search per frame.
    bool lockPhases = true;
};

/// Stretch or compress `audio` in time, leaving pitch alone.
///
/// The result is `round(audio.frames() * factor)` frames long: exactly, so a
/// caller fitting material to a slot gets the length it asked for rather than
/// the length that fell out of a frame count.
///
/// **It can come out louder than it went in.** Rebuilding a waveform from
/// magnitudes and reconstructed phases does not reproduce the original crest:
/// partials that cancelled at one instant in the source may not cancel at the
/// corresponding instant in the output. Measured on material normalised to
/// -0.09 dBFS, the worst overshoot across tones, noise and a mixture, at
/// factors from 0.5 to 4, was +0.11 dB -- small, but enough to clip a file that
/// was already mastered close to the ceiling. The gain is deliberately left
/// alone rather than trimmed, because a transform that quietly changes level is
/// worse than one that needs its output looked at; measure it and limit if it
/// matters.
[[nodiscard]] Result<AudioBuffer> timeStretch(const AudioBuffer& audio,
                                              const StretchSettings& settings = {});

struct PitchSettings {
    /// Positive shifts up. Twelve is an octave; fractional values are fine and
    /// are how a tuning correction is actually spelled -- 100 cents to the
    /// semitone, so -0.23 is 23 cents flat.
    double semitones = 0.0;

    StretchSettings stretch;

    /// The resample that turns the stretch into a shift. Best is the default
    /// because a pitch shift is an edit a user commits to, not a preview.
    ResamplerQuality quality = ResamplerQuality::Best;
};

/// Shift `audio` in pitch, leaving its length alone.
///
/// The result is exactly as long as the input. Everything in it moves by the
/// same ratio, formants included, so a large upward shift on a voice sounds
/// small rather than merely high; formant-preserving shift is a separate piece
/// of work and is not here.
[[nodiscard]] Result<AudioBuffer> pitchShift(const AudioBuffer& audio,
                                             const PitchSettings& settings = {});

/// The frequency ratio `semitones` describes: 2^(semitones/12).
[[nodiscard]] double pitchRatio(double semitones) noexcept;

} // namespace sa::dsp
