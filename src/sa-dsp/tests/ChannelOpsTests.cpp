#include <sa/dsp/ChannelOps.h>

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <numbers>
#include <vector>

using namespace sa;
using namespace sa::dsp;

namespace {

/// Distinct material per channel, and distinct sample to sample.
///
/// Both matter. A buffer whose channels hold the same thing makes swapping
/// invisible and mono a no-op; a buffer that is symmetric in time makes
/// reversing invisible. Either would let a broken implementation pass.
[[nodiscard]] AudioBuffer distinct(SampleCount frames, int channels = 2) {
    AudioBuffer audio{ChannelLayout::discrete(channels), frames};
    for (int channel = 0; channel < channels; ++channel) {
        for (SampleCount i = 0; i < frames; ++i) {
            const double t = static_cast<double>(i) / 48000.0;
            const double hz = 300.0 + 700.0 * channel;
            audio.channel(channel)[i] =
                static_cast<float>(0.4 * std::sin(2.0 * std::numbers::pi * hz * t + channel) +
                                   0.0001 * static_cast<double>(i));
        }
    }
    return audio;
}

[[nodiscard]] std::vector<std::vector<float>> copyOf(const AudioBuffer& audio) {
    std::vector<std::vector<float>> copy;
    for (int channel = 0; channel < audio.channelCount(); ++channel) {
        copy.emplace_back(audio.channel(channel), audio.channel(channel) + audio.frames());
    }
    return copy;
}

} // namespace

TEST_CASE("Reverse reads the buffer backwards, exactly") {
    AudioBuffer audio = distinct(1000);
    const auto before = copyOf(audio);

    REQUIRE(applyChannelOp(audio.view(), ChannelOp::Reverse));

    for (int channel = 0; channel < audio.channelCount(); ++channel) {
        for (SampleCount i = 0; i < audio.frames(); ++i) {
            // Bit-identical, not approximately: moving a float is not
            // arithmetic, so anything but equality is a bug.
            REQUIRE(audio.channel(channel)[i] ==
                    before[static_cast<std::size_t>(channel)]
                          [static_cast<std::size_t>(audio.frames() - 1 - i)]);
        }
    }
}

TEST_CASE("Reverse twice is the identity") {
    AudioBuffer audio = distinct(999); // Odd, so the middle sample has to stay put.
    const auto before = copyOf(audio);

    REQUIRE(applyChannelOp(audio.view(), ChannelOp::Reverse));
    REQUIRE(applyChannelOp(audio.view(), ChannelOp::Reverse));

    for (int channel = 0; channel < audio.channelCount(); ++channel) {
        for (SampleCount i = 0; i < audio.frames(); ++i) {
            REQUIRE(audio.channel(channel)[i] ==
                    before[static_cast<std::size_t>(channel)][static_cast<std::size_t>(i)]);
        }
    }
}

TEST_CASE("Inverting polarity negates every sample and is its own inverse") {
    AudioBuffer audio = distinct(512);
    const auto before = copyOf(audio);

    REQUIRE(applyChannelOp(audio.view(), ChannelOp::InvertPolarity));
    for (int channel = 0; channel < audio.channelCount(); ++channel) {
        for (SampleCount i = 0; i < audio.frames(); ++i) {
            REQUIRE(audio.channel(channel)[i] ==
                    -before[static_cast<std::size_t>(channel)][static_cast<std::size_t>(i)]);
        }
    }

    REQUIRE(applyChannelOp(audio.view(), ChannelOp::InvertPolarity));
    for (int channel = 0; channel < audio.channelCount(); ++channel) {
        for (SampleCount i = 0; i < audio.frames(); ++i) {
            REQUIRE(audio.channel(channel)[i] ==
                    before[static_cast<std::size_t>(channel)][static_cast<std::size_t>(i)]);
        }
    }
}

TEST_CASE("Inverting and summing with the original cancels to silence") {
    // The property that makes polarity inversion worth having at all.
    AudioBuffer audio = distinct(512);
    const auto before = copyOf(audio);

    REQUIRE(applyChannelOp(audio.view(), ChannelOp::InvertPolarity));

    for (int channel = 0; channel < audio.channelCount(); ++channel) {
        for (SampleCount i = 0; i < audio.frames(); ++i) {
            REQUIRE(audio.channel(channel)[i] +
                        before[static_cast<std::size_t>(channel)][static_cast<std::size_t>(i)] ==
                    0.0f);
        }
    }
}

TEST_CASE("Swapping exchanges the two channels and is its own inverse") {
    AudioBuffer audio = distinct(512);
    const auto before = copyOf(audio);

    REQUIRE(applyChannelOp(audio.view(), ChannelOp::SwapChannels));
    for (SampleCount i = 0; i < audio.frames(); ++i) {
        REQUIRE(audio.channel(0)[i] == before[1][static_cast<std::size_t>(i)]);
        REQUIRE(audio.channel(1)[i] == before[0][static_cast<std::size_t>(i)]);
    }

    REQUIRE(applyChannelOp(audio.view(), ChannelOp::SwapChannels));
    for (SampleCount i = 0; i < audio.frames(); ++i) {
        REQUIRE(audio.channel(0)[i] == before[0][static_cast<std::size_t>(i)]);
    }
}

TEST_CASE("Summing to mono puts the average on both channels") {
    AudioBuffer audio = distinct(512);
    const auto before = copyOf(audio);

    REQUIRE(applyChannelOp(audio.view(), ChannelOp::SumToMono));

    for (SampleCount i = 0; i < audio.frames(); ++i) {
        const float wanted = 0.5f * (before[0][static_cast<std::size_t>(i)] +
                                     before[1][static_cast<std::size_t>(i)]);
        REQUIRE(audio.channel(0)[i] == wanted);
        REQUIRE(audio.channel(1)[i] == wanted);
    }
}

TEST_CASE("Summing to mono is idempotent") {
    AudioBuffer audio = distinct(512);

    REQUIRE(applyChannelOp(audio.view(), ChannelOp::SumToMono));
    const auto once = copyOf(audio);
    REQUIRE(applyChannelOp(audio.view(), ChannelOp::SumToMono));

    for (int channel = 0; channel < audio.channelCount(); ++channel) {
        for (SampleCount i = 0; i < audio.frames(); ++i) {
            REQUIRE(audio.channel(channel)[i] ==
                    once[static_cast<std::size_t>(channel)][static_cast<std::size_t>(i)]);
        }
    }
}

TEST_CASE("Summing an out-of-phase pair to mono cancels it") {
    // The classic way a stereo mix loses its bass. Worth a test, because it is
    // the behaviour a user is checking for when they reach for this.
    AudioBuffer audio{ChannelLayout::stereo(), 256};
    for (SampleCount i = 0; i < audio.frames(); ++i) {
        const float value = static_cast<float>(
            0.5 * std::sin(2.0 * std::numbers::pi * 440.0 * static_cast<double>(i) / 48000.0));
        audio.channel(0)[i] = value;
        audio.channel(1)[i] = -value;
    }

    REQUIRE(applyChannelOp(audio.view(), ChannelOp::SumToMono));

    for (SampleCount i = 0; i < audio.frames(); ++i) {
        REQUIRE(audio.channel(0)[i] == 0.0f);
        REQUIRE(audio.channel(1)[i] == 0.0f);
    }
}

TEST_CASE("The stereo-only operations refuse anything that is not a pair") {
    for (const int channels : {1, 3, 6}) {
        AudioBuffer audio = distinct(64, channels);
        REQUIRE_FALSE(applyChannelOp(audio.view(), ChannelOp::SwapChannels));
        REQUIRE_FALSE(applyChannelOp(audio.view(), ChannelOp::SumToMono));

        // And the ones that do not care about the layout still work.
        REQUIRE(applyChannelOp(audio.view(), ChannelOp::Reverse));
        REQUIRE(applyChannelOp(audio.view(), ChannelOp::InvertPolarity));
    }
}

TEST_CASE("An empty selection is a no-op, not a failure") {
    AudioBuffer audio{ChannelLayout::stereo(), 0};
    REQUIRE(applyChannelOp(audio.view(), ChannelOp::Reverse));
    REQUIRE(applyChannelOp(audio.view(), ChannelOp::InvertPolarity));
    REQUIRE(applyChannelOp(audio.view(), ChannelOp::SwapChannels));
    REQUIRE(applyChannelOp(audio.view(), ChannelOp::SumToMono));
}

TEST_CASE("Every operation leaves the buffer the length it found it") {
    for (const ChannelOp operation : {ChannelOp::Reverse, ChannelOp::InvertPolarity,
                                      ChannelOp::SwapChannels, ChannelOp::SumToMono}) {
        AudioBuffer audio = distinct(777);
        REQUIRE(applyChannelOp(audio.view(), operation));
        REQUIRE(audio.frames() == 777);
        REQUIRE(audio.channelCount() == 2);
    }
}
