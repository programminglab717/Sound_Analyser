#pragma once

#include <sa/io/AudioSource.h>

#include <cmath>
#include <memory>

namespace sa::engine::test {

/// An AudioSource whose sample at frame f is exactly `f` (plus a per-channel
/// offset), so a render can be checked against the arithmetic rather than
/// against another render. If audio ever slides by one sample, the value says
/// by how much.
class RampSource final : public io::AudioSource {
public:
    RampSource(int channels, SampleCount frames, float scale = 1.0f) : scale_(scale) {
        info_.sampleRate = kSampleRate48000;
        info_.layout = channels == 1 ? ChannelLayout::mono() : ChannelLayout::stereo();
        info_.frameCount = frames;
        info_.format = io::SampleFormat::Float32;
    }

    [[nodiscard]] const io::AudioFileInfo& info() const noexcept override { return info_; }

    [[nodiscard]] Result<SampleCount> read(SampleIndex startFrame,
                                           AudioBufferView destination) const override {
        if (startFrame < 0) {
            return Error{ErrorCode::OutOfRange, "negative start"};
        }
        if (startFrame >= info_.frameCount || destination.isEmpty()) {
            return SampleCount{0};
        }
        const SampleCount count = std::min(destination.frames(), info_.frameCount - startFrame);
        const int channels = std::min(destination.channelCount(), info_.channelCount());

        for (int channel = 0; channel < channels; ++channel) {
            float* out = destination.channel(channel);
            for (SampleCount i = 0; i < count; ++i) {
                out[i] =
                    (static_cast<float>(startFrame + i) + static_cast<float>(channel) * 10000.0f) *
                    scale_;
            }
        }
        return count;
    }

private:
    io::AudioFileInfo info_;
    float scale_ = 1.0f;
};

inline std::shared_ptr<const io::AudioSource> makeRamp(int channels, SampleCount frames,
                                                       float scale = 1.0f) {
    return std::make_shared<const RampSource>(channels, frames, scale);
}

/// A constant-valued source, for checking gain and fade arithmetic without the
/// ramp's changing amplitude getting in the way.
class ConstantSource final : public io::AudioSource {
public:
    ConstantSource(int channels, SampleCount frames, float value) : value_(value) {
        info_.sampleRate = kSampleRate48000;
        info_.layout = channels == 1 ? ChannelLayout::mono() : ChannelLayout::stereo();
        info_.frameCount = frames;
        info_.format = io::SampleFormat::Float32;
    }

    [[nodiscard]] const io::AudioFileInfo& info() const noexcept override { return info_; }

    [[nodiscard]] Result<SampleCount> read(SampleIndex startFrame,
                                           AudioBufferView destination) const override {
        if (startFrame < 0 || startFrame >= info_.frameCount || destination.isEmpty()) {
            return SampleCount{0};
        }
        const SampleCount count = std::min(destination.frames(), info_.frameCount - startFrame);
        for (int channel = 0; channel < destination.channelCount(); ++channel) {
            std::fill_n(destination.channel(channel), count, value_);
        }
        return count;
    }

private:
    io::AudioFileInfo info_;
    float value_ = 0.0f;
};

inline std::shared_ptr<const io::AudioSource> makeConstant(int channels, SampleCount frames,
                                                           float value) {
    return std::make_shared<const ConstantSource>(channels, frames, value);
}

} // namespace sa::engine::test
