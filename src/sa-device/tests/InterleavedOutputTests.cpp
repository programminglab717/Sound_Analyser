#include <sa/core/AudioBuffer.h>
#include <sa/core/ChannelLayout.h>
#include <sa/core/RealtimeGuard.h>
#include <sa/core/Types.h>
#include <sa/device/AudioDevice.h>
#include <sa/device/InterleavedOutput.h>

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

using namespace sa;
using namespace sa::device;

namespace {

/// A planar buffer whose channel `c`, frame `i` holds `c * 100 + i`, so that a
/// misrouted channel or a transposed index is obvious in the failure message
/// rather than being a plausible-looking number.
AudioBuffer marked(int channels, SampleCount frames) {
    AudioBuffer buffer{ChannelLayout::discrete(channels), frames};
    for (int channel = 0; channel < channels; ++channel) {
        float* samples = buffer.channel(channel);
        for (SampleCount i = 0; i < frames; ++i) {
            samples[i] = static_cast<float>(channel) * 100.0f + static_cast<float>(i);
        }
    }
    return buffer;
}

AudioBuffer filled(int channels, SampleCount frames, float value) {
    AudioBuffer buffer{ChannelLayout::discrete(channels), frames};
    for (int channel = 0; channel < channels; ++channel) {
        float* samples = buffer.channel(channel);
        for (SampleCount i = 0; i < frames; ++i) {
            samples[i] = value;
        }
    }
    return buffer;
}

} // namespace

TEST_CASE("Interleaving lays channels out frame by frame", "[device][interleave]") {
    const AudioBuffer source = marked(2, 3);
    std::array<float, 6> destination{};

    interleave(source.constView(), destination.data(), InterleavedFormat::Float32, 2);

    CHECK(destination == std::array<float, 6>{0.0f, 100.0f, 1.0f, 101.0f, 2.0f, 102.0f});
}

TEST_CASE("Channels the source does not have are silenced, not left alone",
          "[device][interleave]") {
    // The driver's buffer is whatever the last block left there. A stereo mix
    // on a 7.1 endpoint that only wrote the front pair would play the previous
    // block out of the surrounds, over and over.
    const AudioBuffer source = marked(1, 2);
    std::array<float, 8> destination{};
    destination.fill(-7.0f);

    interleave(source.constView(), destination.data(), InterleavedFormat::Float32, 4);

    CHECK(destination[0] == 0.0f);
    CHECK(destination[1] == 0.0f);
    CHECK(destination[2] == 0.0f);
    CHECK(destination[3] == 0.0f);
    CHECK(destination[4] == 1.0f);
    CHECK(destination[5] == 0.0f);
}

TEST_CASE("Channels the destination cannot take are dropped", "[device][interleave]") {
    const AudioBuffer source = marked(4, 2);
    std::array<float, 4> destination{};

    interleave(source.constView(), destination.data(), InterleavedFormat::Float32, 2);

    CHECK(destination == std::array<float, 4>{0.0f, 100.0f, 1.0f, 101.0f});
}

TEST_CASE("Integer conversion scales to full code and clamps", "[device][interleave]") {
    AudioBuffer source{ChannelLayout::discrete(1), 6};
    float* samples = source.channel(0);
    samples[0] = 0.0f;
    samples[1] = 1.0f;
    samples[2] = -1.0f;
    samples[3] = 4.0f; // a processor that overshot
    samples[4] = -4.0f;
    samples[5] = std::numeric_limits<float>::quiet_NaN();

    std::array<std::int16_t, 6> asInt16{};
    interleave(source.constView(), asInt16.data(), InterleavedFormat::Int16, 1);

    CHECK(asInt16[0] == 0);
    CHECK(asInt16[1] == 32767);
    CHECK(asInt16[2] == -32767);
    CHECK(asInt16[3] == 32767);
    CHECK(asInt16[4] == -32767);

    // NaN must not reach the conversion: it compares false against both clamp
    // bounds, and on x86 a float-to-int conversion of NaN is INT_MIN -- a
    // full-scale click rather than the one bad sample it started as.
    CHECK(asInt16[5] == 0);

    std::array<std::int32_t, 6> asInt32{};
    interleave(source.constView(), asInt32.data(), InterleavedFormat::Int32, 1);

    CHECK(asInt32[0] == 0);
    CHECK(asInt32[1] == 2147483647);
    CHECK(asInt32[2] == -2147483647);
    CHECK(asInt32[5] == 0);
}

TEST_CASE("Sample sizes are what the drivers expect", "[device][interleave]") {
    CHECK(bytesPerSample(InterleavedFormat::Float32) == 4);
    CHECK(bytesPerSample(InterleavedFormat::Int16) == 2);
    CHECK(bytesPerSample(InterleavedFormat::Int32) == 4);

    CHECK(interleavedByteCount(64, 2, InterleavedFormat::Float32) == 512);
    CHECK(interleavedByteCount(64, 2, InterleavedFormat::Int16) == 256);
    CHECK(interleavedByteCount(0, 2, InterleavedFormat::Float32) == 0);
    CHECK(interleavedByteCount(64, 0, InterleavedFormat::Float32) == 0);
}

TEST_CASE("Interleaving refuses to write through nothing", "[device][interleave]") {
    const AudioBuffer source = marked(2, 4);
    interleave(source.constView(), nullptr, InterleavedFormat::Float32, 2);

    std::array<float, 4> destination{};
    destination.fill(3.0f);
    interleave(source.constView(), destination.data(), InterleavedFormat::Float32, 0);
    CHECK(destination[0] == 3.0f);

    const AudioBuffer empty;
    interleave(empty.constView(), destination.data(), InterleavedFormat::Float32, 2);
    CHECK(destination[0] == 3.0f);
}

TEST_CASE("The blocker hands the callback exactly the block it was prepared with",
          "[device][interleave]") {
    OutputBlocker blocker;
    blocker.prepare(2, 4);
    CHECK(blocker.blockFrames() == 4);
    CHECK(blocker.channels() == 2);

    int calls = 0;
    SampleCount seenFrames = -1;
    int seenChannels = -1;
    SampleCount seenInputFrames = -1;
    const AudioCallback callback = [&](ConstAudioBufferView input, AudioBufferView output) {
        ++calls;
        seenFrames = output.frames();
        seenChannels = output.channelCount();
        seenInputFrames = input.frames();
        for (int channel = 0; channel < output.channelCount(); ++channel) {
            float* samples = output.channel(channel);
            for (SampleCount i = 0; i < output.frames(); ++i) {
                samples[i] = static_cast<float>(calls);
            }
        }
    };

    std::array<float, 8> destination{};
    blocker.render(callback, destination.data(), 4, InterleavedFormat::Float32, 2);

    CHECK(calls == 1);
    CHECK(seenFrames == 4);
    CHECK(seenChannels == 2);

    // A direction the device does not have still reports the block length.
    CHECK(seenInputFrames == 4);
    CHECK(blocker.blocksRendered() == 1);
    CHECK(destination[0] == 1.0f);
    CHECK(destination[7] == 1.0f);
}

TEST_CASE("A driver chunk that is not a whole number of blocks still runs continuously",
          "[device][interleave]") {
    // This is the case the whole class exists for: shared-mode WASAPI hands
    // over whatever happens to be free, which is almost never the block size
    // the caller negotiated.
    OutputBlocker blocker;
    blocker.prepare(1, 4);

    int calls = 0;
    const AudioCallback callback = [&](ConstAudioBufferView, AudioBufferView output) {
        ++calls;
        float* samples = output.channel(0);
        for (SampleCount i = 0; i < output.frames(); ++i) {
            // A continuous ramp across blocks, so a dropped or repeated frame
            // shows up as a discontinuity rather than as plausible audio.
            samples[i] = static_cast<float>((calls - 1) * 4) + static_cast<float>(i);
        }
    };

    std::vector<float> destination(16, -1.0f);

    blocker.render(callback, destination.data(), 5, InterleavedFormat::Float32, 1);
    CHECK(calls == 2);

    blocker.render(callback, destination.data() + 5, 5, InterleavedFormat::Float32, 1);
    CHECK(calls == 3);

    blocker.render(callback, destination.data() + 10, 6, InterleavedFormat::Float32, 1);
    CHECK(calls == 4);

    for (std::size_t i = 0; i < destination.size(); ++i) {
        CHECK(destination[i] == static_cast<float>(i));
    }
    CHECK(blocker.blocksRendered() == 4);
}

TEST_CASE("Resetting drops the half-consumed block", "[device][interleave]") {
    // A stream that has just been rebuilt must not resume in the middle of
    // audio rendered for an endpoint that has since gone away.
    OutputBlocker blocker;
    blocker.prepare(1, 4);

    int calls = 0;
    const AudioCallback callback = [&](ConstAudioBufferView, AudioBufferView output) {
        ++calls;
        float* samples = output.channel(0);
        for (SampleCount i = 0; i < output.frames(); ++i) {
            samples[i] = static_cast<float>(calls);
        }
    };

    std::array<float, 4> destination{};
    blocker.render(callback, destination.data(), 2, InterleavedFormat::Float32, 1);
    CHECK(calls == 1);

    blocker.reset();
    blocker.render(callback, destination.data(), 2, InterleavedFormat::Float32, 1);

    CHECK(calls == 2);
    CHECK(destination[0] == 2.0f);
    CHECK(destination[1] == 2.0f);
}

TEST_CASE("A blocker with no callback writes silence rather than the last block",
          "[device][interleave]") {
    OutputBlocker blocker;
    blocker.prepare(2, 4);

    std::array<float, 8> destination{};
    destination.fill(0.9f);
    blocker.render(AudioCallback{}, destination.data(), 4, InterleavedFormat::Float32, 2);

    for (const float sample : destination) {
        CHECK(sample == 0.0f);
    }

    // And so does one that was never prepared, which is the state a device is
    // in between construction and start().
    OutputBlocker unprepared;
    destination.fill(0.9f);
    unprepared.render(AudioCallback{}, destination.data(), 4, InterleavedFormat::Float32, 2);
    CHECK(destination[0] == 0.0f);
}

TEST_CASE("Rendering a block allocates nothing", "[device][interleave][rt]") {
    if (!rt::checksEnabled()) {
        SUCCEED("SA_RT_SAFETY_CHECKS is off in this build");
        return;
    }

    OutputBlocker blocker;
    blocker.prepare(2, 64);

    const AudioBuffer tone = filled(2, 64, 0.5f);
    const AudioCallback callback = [&tone](ConstAudioBufferView, AudioBufferView output) {
        for (int channel = 0; channel < output.channelCount(); ++channel) {
            const float* source = tone.channel(channel);
            float* samples = output.channel(channel);
            for (SampleCount i = 0; i < output.frames(); ++i) {
                samples[i] = source[i];
            }
        }
    };

    std::vector<float> destination(64 * 2 * 3);
    std::vector<std::int16_t> narrow(64 * 2 * 3);

    // Guarded exactly as the render loops guard it, so this measures the same
    // path they do -- including the std::function call and the interleave.
    const rt::ScopedAudioThread guard;
    const rt::AllocationScope scope;

    blocker.render(callback, destination.data(), 100, InterleavedFormat::Float32, 2);
    blocker.render(callback, destination.data(), 92, InterleavedFormat::Float32, 2);
    blocker.render(callback, narrow.data(), 100, InterleavedFormat::Int16, 2);
    blocker.render(callback, destination.data(), 64, InterleavedFormat::Int32, 2);

    CHECK(scope.count() == 0);
}
