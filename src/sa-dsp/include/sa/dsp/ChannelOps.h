#pragma once

#include <sa/core/AudioBuffer.h>
#include <sa/core/Result.h>
#include <sa/core/Types.h>

#include <algorithm>
#include <utility>

/// The four edits that are pure arithmetic on the samples.
///
/// Reverse, invert, swap and sum-to-mono have no settings, no filter, no
/// analysis and no approximation: each one is exactly defined by what it does
/// to sample i, and a test can therefore demand bit-identical output rather
/// than a tolerance. That is why they live together and why they live here
/// rather than in the window -- the window and the headless driver both need
/// them, and an edit that only one of them can reach is in the wrong layer.
namespace sa::dsp {

enum class ChannelOp {
    /// Play it backwards. Sample i becomes sample n-1-i, per channel.
    Reverse,
    /// Negate every sample. Inaudible alone, which is the point: it matters
    /// when the result is summed with something else.
    InvertPolarity,
    /// Exchange left and right. Stereo only.
    SwapChannels,
    /// Put the average of the two channels on both of them. Stereo only.
    ///
    /// Both channels keep carrying the sum rather than the file becoming a
    /// one-channel file. Changing the layout is a different operation with
    /// different consequences -- every clip, every marker, every export
    /// setting -- and this is the one people mean by "make it mono".
    SumToMono,
};

/// Whether `operation` can be applied to a buffer with this many channels.
///
/// Separate from applying it so a caller can say why it refused before it has
/// read any audio: the window greys nothing out, it explains.
[[nodiscard]] constexpr bool channelOpNeedsStereo(ChannelOp operation) noexcept {
    return operation == ChannelOp::SwapChannels || operation == ChannelOp::SumToMono;
}

/// Apply `operation` to `audio` in place.
///
/// Fails only when the operation needs a stereo pair and did not get one. An
/// empty buffer is a no-op rather than an error: an edit over an empty
/// selection has nothing to do, which is not the same as being wrong.
[[nodiscard]] inline Status applyChannelOp(AudioBufferView audio, ChannelOp operation) noexcept {
    if (channelOpNeedsStereo(operation) && audio.channelCount() != 2) {
        return Error{ErrorCode::InvalidArgument, "that needs a stereo pair"};
    }

    // Before any channel() call, and not merely as an optimisation. A buffer
    // with no frames still reports its layout's channel count but holds no
    // channel pointers at all, so asking it for channel 0 reads off the end of
    // an empty vector. The layout check above is fine on such a buffer -- the
    // count survives -- which is why it comes first: an empty mono selection
    // is still refused a swap rather than silently accepted.
    if (audio.isEmpty()) {
        return {};
    }

    switch (operation) {
    case ChannelOp::Reverse:
        for (int channel = 0; channel < audio.channelCount(); ++channel) {
            float* samples = audio.channel(channel);
            std::reverse(samples, samples + audio.frames());
        }
        return {};
    case ChannelOp::InvertPolarity:
        for (int channel = 0; channel < audio.channelCount(); ++channel) {
            float* samples = audio.channel(channel);
            for (SampleCount i = 0; i < audio.frames(); ++i) {
                samples[i] = -samples[i];
            }
        }
        return {};
    case ChannelOp::SwapChannels:
        for (SampleCount i = 0; i < audio.frames(); ++i) {
            std::swap(audio.channel(0)[i], audio.channel(1)[i]);
        }
        return {};
    case ChannelOp::SumToMono:
        for (SampleCount i = 0; i < audio.frames(); ++i) {
            // Halved, not just added: two correlated channels summed at full
            // scale clip, and the operation people mean by "mono" does not
            // make the file louder.
            const float summed = 0.5f * (audio.channel(0)[i] + audio.channel(1)[i]);
            audio.channel(0)[i] = summed;
            audio.channel(1)[i] = summed;
        }
        return {};
    }

    return Error{ErrorCode::InvalidArgument, "unknown channel operation"};
}

} // namespace sa::dsp
