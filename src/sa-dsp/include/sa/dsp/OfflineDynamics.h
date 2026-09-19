#pragma once

#include <sa/core/AudioBuffer.h>
#include <sa/core/Result.h>
#include <sa/core/Types.h>
#include <sa/dsp/Dynamics.h>
#include <sa/dsp/StereoLink.h>

/// Applying a compressor or a gate to a buffer, rather than to a stream.
///
/// The processors themselves are real-time: they take samples one at a time and
/// keep an envelope. Running one over a finished selection is a different job
/// with two problems the streaming case does not have, and both are handled
/// here so that the window and the headless driver get the same answer.
///
/// The first is where the envelope starts. A compressor begun at the first
/// sample of a selection has not heard anything yet, so it applies no gain at
/// all for the length of its attack, and the passage begins with a burst of the
/// uncompressed signal. It is given a run-up over the audio before the
/// selection, which is thrown away.
///
/// The second is the two ends. Whatever gain the processor settled on at the
/// selection's edge meets the untouched audio beside it, and the step between
/// them is audible as a click on anything but silence. The edges are blended.
namespace sa::dsp {

/// What to run, and how.
struct OfflineDynamicsSettings {
    /// Link the stereo pair, so the two channels are always given the same
    /// gain and the image cannot move. Ignored for anything but two channels.
    bool linkStereo = true;
    StereoLinkSettings link;
};

/// Compress `audio` in place.
///
/// `runUp` is the number of frames at the start of `audio` that exist only to
/// settle the envelope: they are processed and then left as they were, so the
/// caller reads back `audio` and takes everything after `runUp`. Zero is
/// allowed and means the processor starts cold.
///
/// `blend` frames at each end of the processed region are crossfaded between
/// the processed and the original signal.
[[nodiscard]] Status compressOffline(AudioBufferView audio, SampleRate rate,
                                     const CompressorSettings& compressor, SampleCount runUp,
                                     SampleCount blend,
                                     const OfflineDynamicsSettings& settings = {});

/// Gate `audio` in place, with the same run-up and blend rules.
[[nodiscard]] Status gateOffline(AudioBufferView audio, SampleRate rate, const GateSettings& gate,
                                 SampleCount runUp, SampleCount blend,
                                 const OfflineDynamicsSettings& settings = {});

/// How much audio before a selection is enough to settle an envelope.
///
/// Ten times the longer of attack and release, floored at a fifth of a second.
/// An exponential smoother is within a thousandth of its target after about
/// seven time constants; ten is cheap insurance, and the floor covers settings
/// so fast that ten of them is no run-up at all.
[[nodiscard]] SampleCount dynamicsRunUp(SampleRate rate, double attackSeconds,
                                        double releaseSeconds) noexcept;

} // namespace sa::dsp
