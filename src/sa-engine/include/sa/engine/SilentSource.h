#pragma once

#include <sa/io/AudioSource.h>

namespace sa::engine {

/// Stands in for audio a session referenced but could not open.
///
/// It reports the shape the session recorded -- frame count, rate, channels --
/// and reads as silence. That is what lets a session with a moved file still
/// open with its arrangement intact: clip lengths, positions and fades are all
/// preserved, the timeline is the right duration, and the only thing missing is
/// the sound. Refusing to load at all would destroy a project because one file
/// moved.
class SilentSource final : public io::AudioSource {
public:
    SilentSource(SampleRate rate, ChannelLayout layout, SampleCount frames) {
        info_.sampleRate = rate;
        info_.layout = layout;
        info_.frameCount = frames;
        info_.format = io::SampleFormat::Float32;
    }

    [[nodiscard]] const io::AudioFileInfo& info() const noexcept override { return info_; }

    [[nodiscard]] Result<SampleCount> read(SampleIndex startFrame,
                                           AudioBufferView destination) const override {
        if (startFrame < 0) {
            return Error{ErrorCode::OutOfRange, "startFrame is negative"};
        }
        if (startFrame >= info_.frameCount || destination.isEmpty()) {
            return SampleCount{0};
        }
        const SampleCount count = std::min(destination.frames(), info_.frameCount - startFrame);
        for (int channel = 0; channel < destination.channelCount(); ++channel) {
            std::fill_n(destination.channel(channel), count, 0.0f);
        }
        return count;
    }

private:
    io::AudioFileInfo info_;
};

} // namespace sa::engine
