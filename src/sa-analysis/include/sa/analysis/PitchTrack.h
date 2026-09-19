#pragma once

#include <sa/core/AudioBuffer.h>
#include <sa/core/Result.h>
#include <sa/core/Types.h>

#include <vector>

/// What note is sounding, and when.
///
/// This is YIN (de Cheveigné & Kawahara 2002), which finds the period of a
/// signal by asking how badly it matches a delayed copy of itself and taking
/// the shortest delay that matches well. Four steps, each of which exists to
/// stop one specific wrong answer:
///
/// The squared difference function is the mismatch itself, summed over a
/// window. It is zero at the period, and the naive thing to do is take its
/// smallest value.
///
/// The cumulative mean normalisation divides each lag by the average of every
/// lag up to it. Without it the function is smallest at a lag of zero, where
/// every signal matches itself perfectly, and any search has to be fenced off
/// from short lags by hand. With it the curve starts at one and stays near one
/// while the signal is still correlated with itself for trivial reasons, so
/// the fence is no longer needed -- which is what lets the search below start
/// at a lag of one sample and so notice a signal pitched above the range asked
/// for, rather than quietly answering with a sub-harmonic of it.
///
/// The absolute threshold with the first-dip-below rule is the step that earns
/// the algorithm its reputation. Every multiple of a period is also a period:
/// a signal that repeats every 5 ms repeats every 10 ms too, and the mismatch
/// at 10 ms is just as close to zero. A detector that takes the best match
/// anywhere therefore reports an octave too low the moment the second dip
/// scores a shade better than the first, which on real material is often.
/// Taking the first dip under a fixed threshold settles it by construction,
/// and the dip is then followed down to its bottom, because the lag that first
/// crosses the threshold is on the shoulder and a shoulder is a fraction of a
/// period out.
///
/// Parabolic interpolation recovers the fraction of a sample that the integer
/// lag threw away. At 48 kHz a period of 880 Hz is 54.5 samples, and rounding
/// that to 55 reads as 872.7 Hz -- 0.8% flat, a seventh of a semitone, audible
/// on anything sustained. Three values already computed fix it.
///
/// What is not claimed. It is monophonic: given two notes at once it reports
/// one of them and which one is not defined. It finds a period, not a note --
/// there is no mapping from hertz to a name here, no decision about where one
/// note ends and the next begins, and no vibrato or portamento model. It is
/// not claimed to agree sample for sample with any published implementation of
/// YIN; what is claimed is that on signals whose period is known by
/// construction, it returns that period.
namespace sa::analysis {

struct PitchSettings {
    /// The lowest and highest pitch to accept, in hertz.
    ///
    /// These bound which lags may be reported, not which are searched. A frame
    /// whose shortest period is outside them is reported as having no pitch,
    /// which is the honest answer: the alternative is to report the first
    /// multiple of that period that does fall inside, and a multiple of a
    /// period is an octave error with a range check wrapped round it.
    ///
    /// 50 Hz to 1000 Hz covers a bass voice up to the top of a soprano's range
    /// and the fundamentals of most instruments. Widening the range is not
    /// free: the lower bound sets how many samples every frame has to read.
    double minHz = 50.0;
    double maxHz = 1000.0;

    /// YIN's absolute threshold: how close a match has to be before it counts.
    ///
    /// It is a fraction of the average mismatch, so 0.15 means "at least
    /// 85% better than lags in general". The paper's value, and the trade is
    /// the obvious one -- lower is stricter, and turns borderline frames into
    /// unvoiced ones rather than into wrong notes.
    double threshold = 0.15;

    /// How many samples each estimate is taken over.
    ///
    /// 2048 is 43 ms at 48 kHz, a little over two periods of the lowest pitch
    /// the defaults look for, which is about the least that gives a period
    /// estimate rather than a coin toss. It also sets how fast the contour can
    /// move: everything inside one window is averaged, so a pitch that changes
    /// within it is reported as something in between.
    ///
    /// Note that a frame reads more than this. The difference function
    /// compares the window against a copy delayed by up to rate/minHz samples,
    /// so a frame spans window + rate/minHz -- 3008 samples at the defaults
    /// and 48 kHz -- and the input has to be at least that long.
    SampleCount window = 2048;

    /// How far the window moves between estimates. 256 samples is 5.3 ms at
    /// 48 kHz, fine enough that a contour drawn from it looks continuous.
    SampleCount hop = 256;
};

struct PitchPoint {
    /// The centre of the integration window.
    ///
    /// The estimate also draws on up to rate/minHz samples past the end of
    /// that window, so the reading trails a moving pitch a little, and by more
    /// at low pitches than at high ones.
    double timeSeconds = 0.0;

    /// The fundamental in hertz, or zero where nothing periodic was found.
    ///
    /// Zero rather than the best-scoring lag, so that a contour drawn from
    /// these breaks across an unvoiced frame instead of ruling a line through
    /// noise. A caller wanting the near miss can read the confidence.
    double hz = 0.0;

    /// One minus the aperiodicity at the lag the search settled on: one is a
    /// perfect match, zero is no better than lags in general.
    ///
    /// This says how periodic the frame is, not how much to trust `hz`. A
    /// voiced frame is above 1 - threshold by definition, so the interesting
    /// readings are the unvoiced ones -- and note that a frame pitched above
    /// maxHz reports high confidence and no pitch at the same time, because it
    /// is genuinely periodic, just not at a period in range.
    double confidence = 0.0;

    /// Whether a match under the threshold was found at a lag inside the
    /// range minHz and maxHz describe.
    bool voiced = false;
};

/// The pitch of a passage: the median of its voiced frames, or zero if it has
/// none.
///
/// A median rather than one frame's reading because a single frame of real
/// audio is a lottery -- a glottal closure, a bow change or a consonant either
/// lands inside it or does not, and the answer moves by an octave depending.
/// The median is the number a reader would take off the contour by eye, and it
/// survives the handful of frames that go wrong without being dragged by them
/// the way a mean would.
///
/// With an even number of voiced frames this is the upper of the two middle
/// readings rather than their average, so the result is always a frequency
/// that was actually measured.
///
/// Refuses anything shorter than one frame rather than analysing a partial
/// one. See PitchSettings::window for how long a frame is.
[[nodiscard]] Result<double> estimatePitch(const float* samples, SampleCount count, SampleRate rate,
                                           const PitchSettings& settings = {});

/// The whole contour: one point per hop, in order.
[[nodiscard]] Result<std::vector<PitchPoint>> trackPitch(ConstAudioBufferView audio,
                                                         SampleRate rate,
                                                         const PitchSettings& settings = {},
                                                         int channel = 0);

} // namespace sa::analysis
