#pragma once

#include <sa/analysis/Decibels.h>
#include <sa/core/AudioBuffer.h>
#include <sa/core/Result.h>
#include <sa/core/Types.h>

namespace sa::analysis {

/// What a stereo pair is doing with its two channels.
///
/// Four numbers, and between them they answer the question anyone mastering
/// asks about a mix: is it going to survive being played in mono. It will be,
/// somewhere -- a phone speaker, a club sum, a supermarket ceiling -- and a mix
/// with the bass slightly out of phase loses its bass entirely when it happens.
/// The fault is inaudible in stereo, which is why it needs a meter rather than
/// ears.
struct StereoField {
    /// Pearson correlation between the two channels, -1 to +1.
    ///
    /// The classic phase-correlation meter. +1 is the two channels identical,
    /// which is mono. 0 is uncorrelated, which is what a wide, natural stereo
    /// recording reads. Negative is the warning: the channels are working
    /// against each other, and at -1 they cancel completely in mono.
    double correlation = 1.0;

    /// Side energy over mid energy, in decibels.
    ///
    /// How wide it is, as a ratio rather than as a feeling. A mono programme is
    /// at the floor. Around -10 dB is a conventional mix; above 0 dB there is
    /// more difference between the channels than there is agreement, which is
    /// unusual outside deliberate effects and worth looking at.
    double widthDb = kDecibelFloor;

    /// Right energy over left energy, in decibels. Zero is centred.
    double balanceDb = 0.0;

    /// How much level the programme loses when summed to mono, in decibels and
    /// never positive.
    ///
    /// The number the other three are really about, and the actionable one.
    /// Zero means mono costs nothing. Two identical channels give 0 dB;
    /// uncorrelated channels of equal power give -3 dB, which is arithmetic
    /// rather than damage; anything much below that is cancellation, and a
    /// reading near the floor means the mix disappears.
    double monoLossDb = 0.0;

    /// False when the material was not a stereo pair, or was silent, in which
    /// case none of the above means anything.
    bool valid = false;

    SampleCount frames = 0;
};

/// Streaming accumulator for the stereo measures.
///
/// process() is allocation-free and audio-thread safe, and feeding the same
/// audio in one call or a hundred gives identical results: everything here is
/// a sum over samples, so block boundaries cannot be seen in the answer.
class StereoFieldMeter {
public:
    /// Only a stereo pair has a stereo field. Anything else is refused here
    /// rather than measured into a number that would look meaningful.
    [[nodiscard]] static Result<StereoFieldMeter> create(int channelCount);

    /// One-shot measurement of a whole buffer.
    [[nodiscard]] static Result<StereoField> measure(ConstAudioBufferView audio);

    /// Feed one block. A block whose channel count differs from the configured
    /// one is ignored, as in the other meters.
    void process(ConstAudioBufferView block) noexcept;

    void reset() noexcept;

    [[nodiscard]] StereoField field() const noexcept;

    [[nodiscard]] SampleCount framesProcessed() const noexcept { return frames_; }

private:
    StereoFieldMeter() = default;

    // Sums rather than running means: the four measures are all ratios of these
    // five quantities, so accumulating them is exact regardless of block size
    // and costs five doubles for a programme of any length.
    double sumLeftSquared_ = 0.0;
    double sumRightSquared_ = 0.0;
    double sumProduct_ = 0.0;
    double sumMidSquared_ = 0.0;
    double sumSideSquared_ = 0.0;
    SampleCount frames_ = 0;
};

} // namespace sa::analysis
