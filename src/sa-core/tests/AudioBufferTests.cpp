#include <sa/core/AudioBuffer.h>

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <utility>

using namespace sa;

namespace {

bool isAligned(const void* pointer, std::size_t alignment) {
    return reinterpret_cast<std::uintptr_t>(pointer) % alignment == 0;
}

void fillRamp(AudioBuffer& buffer) {
    for (int channel = 0; channel < buffer.channelCount(); ++channel) {
        float* samples = buffer.channel(channel);
        for (SampleCount i = 0; i < buffer.frames(); ++i) {
            samples[i] = static_cast<float>(channel * 1000) + static_cast<float>(i);
        }
    }
}

} // namespace

TEST_CASE("A default buffer is empty", "[core][buffer]") {
    const AudioBuffer buffer;
    CHECK(buffer.isEmpty());
    CHECK(buffer.channelCount() == 0);
    CHECK(buffer.frames() == 0);
}

TEST_CASE("Construction allocates the requested shape", "[core][buffer]") {
    const AudioBuffer buffer{ChannelLayout::stereo(), 1024};
    CHECK(buffer.channelCount() == 2);
    CHECK(buffer.frames() == 1024);
    CHECK_FALSE(buffer.isEmpty());
    CHECK(buffer.layout() == ChannelLayout::stereo());
}

TEST_CASE("Every channel is SIMD-aligned", "[core][buffer]") {
    // Odd frame counts are the interesting case: a single contiguous block
    // would leave later channels misaligned.
    const AudioBuffer buffer{ChannelLayout::fiveOne(), 1023};
    for (int channel = 0; channel < buffer.channelCount(); ++channel) {
        INFO("channel " << channel);
        CHECK(isAligned(buffer.channel(channel), kSampleAlignment));
    }
}

TEST_CASE("New storage is zeroed", "[core][buffer]") {
    const AudioBuffer buffer{ChannelLayout::stereo(), 512};
    for (int channel = 0; channel < buffer.channelCount(); ++channel) {
        for (SampleCount i = 0; i < buffer.frames(); ++i) {
            REQUIRE(buffer.channel(channel)[i] == 0.0f);
        }
    }
}

TEST_CASE("clear zeroes existing content", "[core][buffer]") {
    AudioBuffer buffer{ChannelLayout::stereo(), 256};
    fillRamp(buffer);
    REQUIRE(buffer.channel(0)[10] == 10.0f);

    buffer.clear();
    for (int channel = 0; channel < buffer.channelCount(); ++channel) {
        for (SampleCount i = 0; i < buffer.frames(); ++i) {
            REQUIRE(buffer.channel(channel)[i] == 0.0f);
        }
    }
}

TEST_CASE("Degenerate shapes produce an empty buffer, not a crash", "[core][buffer]") {
    const AudioBuffer noChannels{ChannelLayout::discrete(0), 1024};
    CHECK(noChannels.isEmpty());

    const AudioBuffer noFrames{ChannelLayout::stereo(), 0};
    CHECK(noFrames.isEmpty());

    const AudioBuffer negative{ChannelLayout::stereo(), -100};
    CHECK(negative.isEmpty());
}

TEST_CASE("Buffers move without copying storage", "[core][buffer]") {
    AudioBuffer source{ChannelLayout::stereo(), 128};
    fillRamp(source);
    const float* originalStorage = source.channel(0);

    AudioBuffer moved{std::move(source)};
    CHECK(moved.channel(0) == originalStorage);
    CHECK(moved.frames() == 128);
    CHECK(moved.channel(1)[5] == 1005.0f);

    // NOLINTNEXTLINE(bugprone-use-after-move) -- asserting the moved-from state
    CHECK(source.isEmpty());
}

TEST_CASE("Move assignment releases the previous allocation", "[core][buffer]") {
    AudioBuffer target{ChannelLayout::fiveOne(), 64};
    AudioBuffer source{ChannelLayout::mono(), 32};
    fillRamp(source);

    target = std::move(source);
    CHECK(target.channelCount() == 1);
    CHECK(target.frames() == 32);
    CHECK(target.channel(0)[3] == 3.0f);
}

TEST_CASE("resize replaces the shape and zeroes the new storage", "[core][buffer]") {
    AudioBuffer buffer{ChannelLayout::stereo(), 100};
    fillRamp(buffer);

    buffer.resize(ChannelLayout::fiveOne(), 200);
    CHECK(buffer.channelCount() == 6);
    CHECK(buffer.frames() == 200);
    for (int channel = 0; channel < buffer.channelCount(); ++channel) {
        REQUIRE(buffer.channel(channel)[0] == 0.0f);
    }
}

TEST_CASE("A view aliases the buffer rather than copying it", "[core][view]") {
    AudioBuffer buffer{ChannelLayout::stereo(), 64};
    fillRamp(buffer);

    const AudioBufferView view = buffer.view();
    CHECK(view.channelCount() == 2);
    CHECK(view.frames() == 64);
    CHECK(view.channel(0) == buffer.channel(0));

    view.channel(1)[7] = -1.0f;
    CHECK(buffer.channel(1)[7] == -1.0f);
}

TEST_CASE("subRange offsets without copying", "[core][view]") {
    AudioBuffer buffer{ChannelLayout::stereo(), 100};
    fillRamp(buffer);

    const AudioBufferView middle = buffer.view().subRange(10, 20);
    CHECK(middle.frames() == 20);
    CHECK(middle.channel(0) == buffer.channel(0) + 10);
    CHECK(middle.channel(0)[0] == 10.0f);
    CHECK(middle.channel(1)[0] == 1010.0f);
}

TEST_CASE("Nested subRanges compose", "[core][view]") {
    AudioBuffer buffer{ChannelLayout::mono(), 100};
    fillRamp(buffer);

    const auto nested = buffer.view().subRange(10, 50).subRange(5, 10);
    CHECK(nested.frames() == 10);
    CHECK(nested.channel(0)[0] == 15.0f);
}

TEST_CASE("subRange clamps instead of running off the end", "[core][view]") {
    AudioBuffer buffer{ChannelLayout::mono(), 100};

    CHECK(buffer.view().subRange(90, 50).frames() == 10);
    CHECK(buffer.view().subRange(200, 10).frames() == 0);
    CHECK(buffer.view().subRange(-10, 10).frames() == 10);
    CHECK(buffer.view().subRange(10, -5).frames() == 0);
}

TEST_CASE("A mutable view converts to a const view", "[core][view]") {
    AudioBuffer buffer{ChannelLayout::stereo(), 32};
    fillRamp(buffer);

    const ConstAudioBufferView readOnly = buffer.view();
    CHECK(readOnly.frames() == 32);
    CHECK(readOnly.channel(0)[4] == 4.0f);

    const ConstAudioBufferView fromConstBuffer = std::as_const(buffer).view();
    CHECK(fromConstBuffer.channel(1)[2] == 1002.0f);
}
