#pragma once

#include <sa/core/AudioBuffer.h>
#include <sa/core/Result.h>
#include <sa/core/Types.h>

#include <vector>

/// How fast the music goes, and where its beats fall.
///
/// Those are two questions, and the second is the one that makes the first
/// useful. A period on its own says that a beat happens every half second
/// without saying which instants those are, and nothing that has to line up
/// with the audio -- a click track, a loop point, a tempo-synced delay -- can
/// be driven from it. So the period is found first, from the whole record, and
/// a phase is fitted afterwards at that period.
///
/// Both are read off one curve, the onset strength envelope, which is exposed
/// here for the same reason the Schroeder curve is exposed in RoomAcoustics.h:
/// every number below is a peak or a correlation taken from it, and an answer
/// that looks wrong is usually obvious from the curve.
///
/// What this does not claim. One period and one phase are fitted to the whole
/// of what it is given, so a piece that changes tempo, or a performance with
/// rubato in it, is answered with an average and a poor confidence rather than
/// with a curve. There is no metre and no downbeat -- bar lines are not found,
/// only beats -- and no attempt at the octave question beyond the stated
/// preference at trackTempo(). Nothing here has been run against the MIREX
/// beat-tracking sets. What is claimed is that on material whose beat times
/// are known by construction, it recovers them to within the hop it analysed
/// at, and that material with no rhythm is reported as having none.
namespace sa::analysis {

struct TempoSettings {
    /// The range of tempi considered, in beats per minute.
    ///
    /// Not only a filter. Half and double a tempo predict onsets at the same
    /// instants, so something has to choose between them, and the range is
    /// half of that choice -- trackTempo() describes the other half.
    double minBpm = 60.0;
    double maxBpm = 200.0;

    /// 1024 samples is about 21 ms at 48 kHz: long enough to resolve the
    /// pitched material whose change the flux is measuring, short enough that
    /// a drum hit occupies a few frames rather than one.
    int fftSize = 1024;

    /// The hop is the time resolution of everything reported here. A beat
    /// cannot be placed more finely than the spacing of the frames it was
    /// found in, which is about 5.3 ms at 48 kHz.
    SampleCount hop = 256;
};

struct BeatGrid {
    /// The tempo, in beats per minute. Left at zero when nothing was found,
    /// rather than filled in with the best of a bad set of candidates.
    double bpm = 0.0;

    /// How periodic the onset envelope was at that period: its autocorrelation
    /// there over its autocorrelation at zero. One would mean every beat's
    /// onsets are a copy of every other's; zero means the period explains
    /// nothing about the record.
    ///
    /// Zero when the record was turned down before any correlation was taken,
    /// which is what happens to material with no events in it: that is not a
    /// measurement of zero, it is the absence of one. Where a correlation was
    /// taken and came out too weak to call a tempo, the figure is reported as
    /// measured, because then it is the evidence for the refusal.
    double confidence = 0.0;

    /// The first beat, in seconds from the start of the buffer. The same value
    /// as beatSeconds.front(), named separately because it is the number a
    /// caller setting a grid offset actually reaches for.
    double firstBeatSeconds = 0.0;

    /// Every beat in the buffer, in seconds, evenly spaced by construction.
    /// The grid is fitted to the onsets rather than snapped to them, so a beat
    /// may land where nothing was played -- that is what makes it a grid and
    /// not a list of onsets.
    std::vector<double> beatSeconds;

    /// False when there is no tempo to report: an onset envelope with no
    /// events in it, a buffer too short to hold four beats at any tempo in the
    /// range, or a correlation too weak to mean anything. The other fields are
    /// then left at zero rather than holding a number that would be read as a
    /// measurement.
    bool valid = false;
};

/// The onset strength envelope: how much the spectrum grew, frame by frame.
///
/// Spectral flux -- the frame-to-frame increase in magnitude, half-wave
/// rectified and summed across bins. Rectified because an onset is energy
/// arriving: a note ending is a fall of the same size and is not a beat.
///
/// Magnitudes are compressed before they are differenced, and that is the
/// whole reason this reads anything at all in a quiet passage. On a linear
/// scale the flux of a verse is a rounding error beside the flux of a chorus,
/// and one tempo has to be found across both. The compression is
/// log(1 + 1000 * magnitude), so a quiet onset is a comparable event to a loud
/// one rather than a hundredth of it.
///
/// Bins more than 60 dB below the loudest bin of their own frame are first
/// held at that level. They are mostly the analysis window's own leakage from
/// something louder elsewhere in the spectrum, and on a compressed scale the
/// relative wobble of something inaudible would otherwise count for as much as
/// a snare drum.
///
/// What this does not do is make the envelope of a steady sound zero. A 21 ms
/// window cannot resolve a low tone, so a held 60 Hz note has a magnitude
/// spectrum that genuinely pulses at 60 Hz, and the flux of it is small but
/// not nothing. trackTempo() is where that is dealt with.
///
/// Index i is one frame and onsetFrameSeconds() says when it was. Entry 0 is
/// zero because there is no frame before it for it to have grown from.
///
/// Exposed separately because it is the curve everything else here is read
/// from, and because it is useful without any of the tempo machinery: it is
/// what an onset detector or a transient-aware editor wants.
[[nodiscard]] Result<std::vector<float>> onsetEnvelope(ConstAudioBufferView audio, SampleRate rate,
                                                       const TempoSettings& settings = {},
                                                       int channel = 0);

/// When an onset envelope frame happened, in seconds. Fractional indices are
/// meaningful, because a beat rarely falls on a frame.
///
/// Deliberately not the centre of the analysis window, which is the usual
/// convention and is the wrong one for a flux. Two corrections to the centre,
/// in order of size:
///
/// A compressed flux fires when a transient *enters* the window, not when it
/// reaches the middle of it -- by the time the transient is at the centre the
/// magnitude has already risen and the increase has been counted. The leading
/// edge of the window is half a window ahead of its centre, so half a window
/// goes back on. Then the flux at frame i is a difference between frames i and
/// i-1, which belongs midway between them, so half a hop comes off. Together:
/// i * hop + fftSize - hop / 2.
///
/// The bound that follows is the point of it. The first frame whose window
/// reaches a transient has its leading edge somewhere in the hop following
/// that transient, so this places the transient within half a hop either side
/// of where it really was -- where the window-centre convention would place it
/// most of a window early.
[[nodiscard]] double onsetFrameSeconds(double frameIndex, SampleRate rate,
                                       const TempoSettings& settings = {});

/// Find the tempo, then lay a beat grid on it.
///
/// The period comes from the autocorrelation of the onset envelope: a record
/// whose onsets repeat every half second correlates with itself at half a
/// second. Only lags inside the requested tempo range are considered, and only
/// lags the record is long enough to hold four times over -- a correlation at
/// a lag approaching the length of the record is computed from a handful of
/// terms and will find a period in anything.
///
/// The octave question. 60, 120 and 240 beats per minute predict onsets at the
/// same instants, and nothing in the signal separates them, so a preference
/// has to. Each lag's correlation is multiplied by a Gaussian in log-tempo
/// centred on the geometric middle of the requested range --
/// sqrt(minBpm * maxBpm), which is 110 BPM for the default 60 to 200 -- with a
/// standard deviation of 0.9 of an octave.
///
/// Of 60, 120 and 240, over a range wide enough to admit all three, this
/// prefers 120. A click every 0.25 s is reported as 120 BPM, with half the
/// clicks falling between beats, not as 240. That is a preference and not a
/// measurement, and it is chosen because tapping along to most music lands
/// between 80 and 160, so the middle of the range is where the answer usually
/// is.
///
/// The phase is then whichever alignment of a pulse train at that period
/// collects the most onset strength -- and after that, period and phase are
/// fitted together by least squares to the onsets the coarse grid landed on.
/// The correlation settles the period to a fraction of a frame, which is not
/// close enough: across twenty beats a fraction of a frame per beat is a grid
/// that is right in the middle of the record and wrong at both ends.
///
/// Before any of that, the envelope has to look like events. A record whose
/// onset strength is spread evenly over its whole length -- a held note, a
/// steady noise -- is answered with valid = false and no tempo, however
/// periodic it may be, and it can be very periodic indeed: an unresolved low
/// tone pulses at its own frequency and correlates with itself perfectly.
/// Rhythm is a few loud moments among many quiet ones, and that is what is
/// actually tested for.
[[nodiscard]] Result<BeatGrid> trackTempo(ConstAudioBufferView audio, SampleRate rate,
                                          const TempoSettings& settings = {}, int channel = 0);

} // namespace sa::analysis
