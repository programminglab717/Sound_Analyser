#pragma once

#include <sa/core/AudioBuffer.h>
#include <sa/io/AudioSource.h>

#include <algorithm>
#include <utility>

namespace sa::engine {

/// An `AudioSource` over audio already in memory.
///
/// The clipboard needs this, and so does anything generated rather than
/// decoded: a tone, a noise print, a repaired region rendered once and pasted
/// back. It owns its buffer, because a clipboard that borrows outlives whatever
/// it borrowed from sooner or later.
class BufferSource final : public io::AudioSource {
public:
    BufferSource(AudioBuffer audio, SampleRate rate) : audio_(std::move(audio)) {
        info_.sampleRate = rate;
        info_.layout = audio_.layout();
        info_.frameCount = audio_.frames();
        info_.format = io::SampleFormat::Float32;
    }

    [[nodiscard]] const io::AudioFileInfo& info() const noexcept override { return info_; }

    [[nodiscard]] Result<SampleCount> read(SampleIndex startFrame,
                                           AudioBufferView destination) const override {
        if (startFrame < 0) {
            return Error{ErrorCode::OutOfRange, "startFrame is negative"};
        }
        if (destination.channelCount() != info_.layout.count()) {
            return Error{ErrorCode::InvalidArgument,
                         "destination channel count does not match the buffer"};
        }
        if (startFrame >= info_.frameCount || destination.isEmpty()) {
            return SampleCount{0};
        }
        const SampleCount count =
            std::min<SampleCount>(destination.frames(), info_.frameCount - startFrame);
        for (int channel = 0; channel < destination.channelCount(); ++channel) {
            const float* in = audio_.channel(channel) + startFrame;
            std::copy_n(in, count, destination.channel(channel));
        }
        return count;
    }

private:
    AudioBuffer audio_;
    io::AudioFileInfo info_;
};

} // namespace sa::engine
