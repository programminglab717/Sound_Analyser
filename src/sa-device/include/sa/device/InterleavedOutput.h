#pragma once

#include <sa/core/AudioBuffer.h>
#include <sa/core/Types.h>
#include <sa/device/AudioDevice.h>

#include <atomic>
#include <cstddef>

namespace sa::device {

/// The interleaved sample formats a playback driver will accept.
///
/// Only the three that matter are here. Float32 is what every modern shared
/// mixer runs at and what the rest of this codebase speaks, so it is the
/// pass-through case; the two integer formats exist because an endpoint is
/// occasionally configured for one and refusing to play would be a worse answer
/// than converting.
enum class InterleavedFormat {
    Float32,
    Int16,
    Int32,
};

[[nodiscard]] constexpr int bytesPerSample(InterleavedFormat format) noexcept {
    switch (format) {
    case InterleavedFormat::Int16:
        return 2;
    case InterleavedFormat::Float32:
    case InterleavedFormat::Int32:
        return 4;
    }
    return 4;
}

/// Bytes occupied by `frames` interleaved frames of `channels`.
[[nodiscard]] constexpr std::size_t interleavedByteCount(SampleCount frames, int channels,
                                                         InterleavedFormat format) noexcept {
    if (frames <= 0 || channels <= 0) {
        return 0;
    }
    return static_cast<std::size_t>(frames) * static_cast<std::size_t>(channels) *
           static_cast<std::size_t>(bytesPerSample(format));
}

/// Writes `source` into `destination` as interleaved frames of
/// `destinationChannels`, converting to `format`.
///
/// Real-time safe: no allocation, no locks, no branches on anything that could
/// be. The frame count comes from the view, so pass a subRange to write part of
/// a block.
///
/// Channel counts need not match. Destination channels the source does not have
/// are filled with silence rather than left alone -- a stereo mix on a 7.1
/// endpoint is routine, and whatever the driver left in those slots is the
/// previous block, which would come out of the surrounds as a stutter. Source
/// channels the destination has no room for are dropped, because the
/// alternative is refusing to play at all.
void interleave(ConstAudioBufferView source, void* destination, InterleavedFormat format,
                int destinationChannels) noexcept;

/// Turns the chunk size a driver asks for into the fixed block size the
/// AudioCallback contract promises.
///
/// A shared-mode WASAPI endpoint hands over however much space happens to be
/// free when its event fires, and an ALSA period is whatever the hardware would
/// agree to rather than what was asked for. Neither is the block length the
/// caller negotiated. Absorbing that here, once, is much better than letting it
/// reach the callback: a processor sized to its block length is the normal case,
/// and a short block would be a bug in every one of them rather than in one
/// place here.
///
/// Not copyable or movable: the render thread holds a pointer to one of these
/// and moving it mid-stream would leave that pointer dangling.
class OutputBlocker {
public:
    OutputBlocker() = default;

    OutputBlocker(const OutputBlocker&) = delete;
    OutputBlocker& operator=(const OutputBlocker&) = delete;
    OutputBlocker(OutputBlocker&&) = delete;
    OutputBlocker& operator=(OutputBlocker&&) = delete;

    ~OutputBlocker() = default;

    /// Sizes the planar block the callback fills. Allocates, so this belongs on
    /// the thread that opens the device and never in the render loop.
    void prepare(int channels, int blockFrames);

    /// Renders `frames` interleaved frames into `destination`, calling
    /// `callback` as many times as that takes.
    ///
    /// This is the real-time path. It allocates nothing, takes no lock and
    /// marks the thread so that a callback which breaks that rule is caught.
    void render(const AudioCallback& callback, void* destination, SampleCount frames,
                InterleavedFormat format, int destinationChannels) noexcept;

    /// Discards the partly-consumed block. Call when a stream is rebuilt, so
    /// playback does not resume in the middle of audio rendered for an endpoint
    /// that has since gone away.
    void reset() noexcept { consumed_ = blockFrames_; }

    [[nodiscard]] int blockFrames() const noexcept { return blockFrames_; }

    [[nodiscard]] int channels() const noexcept { return channels_; }

    /// Blocks handed to the callback since prepare(), which is to say since the
    /// stream last started. The render loops have no other way to say "audio is
    /// really flowing", and a test has no other way to wait for it.
    [[nodiscard]] std::size_t blocksRendered() const noexcept;

private:
    /// Always zero channels but the full block length, so an output-only
    /// callback can still size its work from the input view -- the behaviour
    /// AudioCallback documents for a direction the device does not have.
    AudioBuffer input_;
    AudioBuffer output_;

    int channels_ = 0;
    int blockFrames_ = 0;

    /// Frames of the current block already delivered. Starts level with
    /// blockFrames_, which is what makes the first render() call the callback
    /// rather than emit an unwritten block.
    SampleCount consumed_ = 0;

    /// Written by the render thread and read by anyone, so it cannot be a plain
    /// counter even though only one thread ever increments it.
    std::atomic<std::size_t> blocksRendered_{0};
};

} // namespace sa::device
