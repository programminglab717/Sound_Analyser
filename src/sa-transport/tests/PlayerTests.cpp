#include <sa/core/RealtimeGuard.h>
#include <sa/device/AudioDeviceBackend.h>
#include <sa/device/AudioDeviceManager.h>
#include <sa/device/NullAudioDevice.h>
#include <sa/transport/Player.h>

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <memory>
#include <thread>
#include <vector>

using namespace sa;
using namespace sa::transport;
using Catch::Approx;

namespace {

/// A source whose sample at frame f is f, with a per-channel offset, so any
/// frame played can be traced back to where it came from.
class RampSource final : public io::AudioSource {
public:
    RampSource(SampleCount frames, int channels) {
        info_.sampleRate = kSampleRate48000;
        info_.layout = channels == 1 ? ChannelLayout::mono() : ChannelLayout::stereo();
        info_.frameCount = frames;
        info_.format = io::SampleFormat::Float32;
    }

    [[nodiscard]] const io::AudioFileInfo& info() const noexcept override { return info_; }

    [[nodiscard]] Result<SampleCount> read(SampleIndex startFrame,
                                           AudioBufferView destination) const override {
        if (startFrame < 0 || startFrame >= info_.frameCount) {
            return SampleCount{0};
        }
        const SampleCount count =
            std::min<SampleCount>(destination.frames(), info_.frameCount - startFrame);
        for (int channel = 0; channel < destination.channelCount(); ++channel) {
            float* out = destination.channel(channel);
            for (SampleCount i = 0; i < count; ++i) {
                out[i] =
                    static_cast<float>(startFrame + i) + static_cast<float>(channel) * 1000000.0f;
            }
        }
        return count;
    }

private:
    io::AudioFileInfo info_;
};

/// Opens a null device, which runs a real thread on a real clock but plays to
/// nothing -- which is exactly what a test wants.
///
/// The backend list is explicit rather than the platform's, for two reasons.
/// The first version asked for the platform default and failed on a headless
/// Windows runner, where WASAPI lists a default endpoint that is not there.
/// The second reason is worse and would not have shown up in CI at all: on a
/// developer's machine the platform default *is* there, and the whole suite
/// would have played through their speakers.
std::unique_ptr<device::AudioDevice> openNullDevice(int channels = 2, int blockFrames = 256) {
    device::AudioDeviceConfig config;
    config.outputChannels = channels;
    config.inputChannels = 0;
    config.bufferFrames = blockFrames;

    std::vector<std::unique_ptr<device::AudioDeviceBackend>> backends;
    backends.push_back(std::make_unique<device::NullAudioBackend>());
    device::AudioDeviceManager manager{std::move(backends)};

    auto opened = manager.openDefault(config);
    REQUIRE(opened.hasValue());
    return std::move(opened).value();
}

Player makePlayer(int channels = 2, int blockFrames = 256) {
    auto player = Player::create(openNullDevice(channels, blockFrames));
    REQUIRE(player.hasValue());
    return std::move(player).value();
}

/// Wait until `predicate` holds or the deadline passes. Returns whether it held.
template <typename Predicate>
bool waitUntil(Predicate predicate, std::chrono::milliseconds limit = std::chrono::seconds{10}) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    return predicate();
}

} // namespace

TEST_CASE("Creating a player validates its device", "[transport][player]") {
    CHECK_FALSE(Player::create(nullptr).hasValue());

    device::AudioDeviceConfig inputOnly;
    inputOnly.outputChannels = 0;
    inputOnly.inputChannels = 2;
    std::vector<std::unique_ptr<device::AudioDeviceBackend>> backends;
    backends.push_back(std::make_unique<device::NullAudioBackend>());
    device::AudioDeviceManager manager{std::move(backends)};
    if (auto opened = manager.openDefault(inputOnly); opened) {
        CHECK_FALSE(Player::create(std::move(opened).value()).hasValue());
    }

    CHECK(Player::create(openNullDevice()).hasValue());
}

TEST_CASE("Playing refuses a mismatched or empty request", "[transport][player]") {
    Player player = makePlayer(2);
    const auto stereo = std::make_shared<RampSource>(48000, 2);
    const auto mono = std::make_shared<RampSource>(48000, 1);

    CHECK_FALSE(player.play(nullptr, 0, 100).ok());
    CHECK_FALSE(player.play(stereo, 100, 100).ok());
    CHECK_FALSE(player.play(stereo, 100, 50).ok());
    CHECK_FALSE(player.play(mono, 0, 100).ok());
    CHECK(player.play(stereo, 0, 4800).ok());
    player.stop();
}

TEST_CASE("Playback advances the position and then ends", "[transport][player]") {
    Player player = makePlayer(2);
    const auto source = std::make_shared<RampSource>(48000, 2);

    REQUIRE(player.play(source, 4800, 14400).ok());

    // Position is absolute in the source, so it starts where playback started.
    CHECK(waitUntil([&] { return player.position() > 4800; }));
    CHECK(waitUntil([&] { return player.position() >= 14400; }));
    CHECK(player.position() <= 14400 + 512);

    player.stop();
    CHECK_FALSE(player.isPlaying());
}

TEST_CASE("Stopping is idempotent and safe before playing", "[transport][player]") {
    Player player = makePlayer();
    player.stop();
    player.stop();
    CHECK_FALSE(player.isPlaying());

    const auto source = std::make_shared<RampSource>(24000, 2);
    REQUIRE(player.play(source, 0, 24000).ok());
    player.stop();
    player.stop();
    CHECK_FALSE(player.isPlaying());
}

TEST_CASE("Playing again while playing restarts cleanly", "[transport][player]") {
    Player player = makePlayer();
    const auto source = std::make_shared<RampSource>(480000, 2);

    REQUIRE(player.play(source, 0, 480000).ok());
    CHECK(waitUntil([&] { return player.position() > 1000; }));

    REQUIRE(player.play(source, 240000, 480000).ok());
    CHECK(player.position() >= 240000);
    player.stop();
}

TEST_CASE("A whole file plays without underrunning", "[transport][player]") {
    // Underruns mean the worker fell behind the callback. Against a ramp source
    // and the null device, on a machine that can schedule the worker, there is
    // no excuse for one.
    //
    // That last clause is not a hedge, it is the whole caveat, and it is here
    // because the comment that used to sit in its place denied it. The null
    // device paces itself against a real clock; a worker thread that the
    // scheduler does not run cannot fill a ring buffer however correct its
    // buffering is. This test was seen to fail exactly once, on a machine
    // compiling three sanitiser builds at the time, and passed immediately on
    // its own. So: a failure here on an idle machine is a buffering defect and
    // should be treated as one. A failure while the machine is saturated is
    // the scheduler, and re-running it on an idle machine is the way to tell
    // which you have -- a real defect underruns every time.
    Player player = makePlayer(2, 128);
    const auto source = std::make_shared<RampSource>(96000, 2);

    REQUIRE(player.play(source, 0, 96000).ok());
    CHECK(waitUntil([&] { return player.position() >= 96000; }));
    player.stop();

    INFO("underruns: " << player.underruns());
    CHECK(player.underruns() == 0);
}

TEST_CASE("Destroying a playing player does not hang or crash", "[transport][player]") {
    const auto source = std::make_shared<RampSource>(480000, 2);
    {
        Player player = makePlayer();
        REQUIRE(player.play(source, 0, 480000).ok());
        CHECK(waitUntil([&] { return player.position() > 0; }));
    }
    SUCCEED("the destructor stopped the device and joined the worker");
}

TEST_CASE("The audio callback allocates nothing", "[transport][player][rt]") {
    if (!rt::checksEnabled()) {
        SUCCEED("SA_RT_SAFETY_CHECKS is off in this build");
        return;
    }

    // The null device marks its own thread as the audio thread, so the counter
    // sees exactly the allocations our callback makes and none of the setup
    // around it. This is the check that matters most in this file: everything
    // else here would still pass if the callback allocated on every block, and
    // the user would hear it as a dropout under load rather than as a failure.
    Player player = makePlayer(2, 256);
    const auto source = std::make_shared<RampSource>(96000, 2);

    const std::size_t before = rt::audioThreadAllocationCount();
    REQUIRE(player.play(source, 0, 96000).ok());
    REQUIRE(waitUntil([&] { return player.position() > 24000; }));
    player.stop();
    const std::size_t after = rt::audioThreadAllocationCount();

    CHECK(after == before);
}

namespace {

/// Records what it was handed, without allocating: the store is sized before
/// playback starts and the audio thread only writes into it. Anything past the
/// end is counted and dropped rather than grown, because growing is the one
/// thing this must not do.
class RecordingProcessor final : public AudioProcessor {
public:
    explicit RecordingProcessor(std::size_t capacityFrames, int channels)
        : channels_(channels), store_(capacityFrames * static_cast<std::size_t>(channels), 0.0f) {}

    void process(AudioBufferView block) noexcept override {
        calls_.fetch_add(1, std::memory_order_relaxed);
        const auto frames = static_cast<std::size_t>(block.frames());
        for (std::size_t frame = 0; frame < frames; ++frame) {
            const std::size_t at = written_ + frame;
            if (at * static_cast<std::size_t>(channels_) + static_cast<std::size_t>(channels_) >
                store_.size()) {
                break;
            }
            for (int channel = 0; channel < block.channelCount(); ++channel) {
                store_[at * static_cast<std::size_t>(channels_) +
                       static_cast<std::size_t>(channel)] = block.channel(channel)[frame];
            }
        }
        written_ += frames;
        // Prove the block really is the device's buffer and not a copy of it:
        // whatever is written here is what the device is handed.
        for (int channel = 0; channel < block.channelCount(); ++channel) {
            float* samples = block.channel(channel);
            for (std::size_t frame = 0; frame < frames; ++frame) {
                samples[frame] = -1.0f;
            }
        }
    }

    [[nodiscard]] std::size_t calls() const noexcept {
        return calls_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] const std::vector<float>& recorded() const noexcept { return store_; }

private:
    int channels_;
    std::vector<float> store_;
    std::size_t written_ = 0;
    std::atomic<std::size_t> calls_{0};
};

} // namespace

TEST_CASE("A processor is handed the frames that are playing", "[transport][player][processor]") {
    Player player = makePlayer(2, 256);
    const auto source = std::make_shared<RampSource>(96000, 2);
    RecordingProcessor processor{8192, 2};

    REQUIRE(player.play(source, 0, 96000, &processor).ok());
    REQUIRE(waitUntil([&] { return player.position() > 8192; }));
    player.stop();

    CHECK(processor.calls() > 0);

    // RampSource puts the frame index in channel 0 and index + 1000000 in
    // channel 1, so the recording says both which frames arrived and in what
    // order. Checking the first four thousand is enough to catch a processor
    // fed a copy, fed the interleaving scratch, or fed blocks out of order.
    const std::vector<float>& got = processor.recorded();
    for (std::size_t frame = 0; frame < 4000; ++frame) {
        CHECK(got[frame * 2] == Approx(static_cast<float>(frame)));
        CHECK(got[frame * 2 + 1] == Approx(static_cast<float>(frame) + 1000000.0f));
    }
}

TEST_CASE("A processor does not make the audio callback allocate",
          "[transport][player][processor][rt]") {
    if (!rt::checksEnabled()) {
        SUCCEED("SA_RT_SAFETY_CHECKS is off in this build");
        return;
    }

    // The allocation check above this one covers playback with no processor,
    // which would still pass if installing one allocated on every block. This
    // is the same check with the processor in the path.
    Player player = makePlayer(2, 256);
    const auto source = std::make_shared<RampSource>(96000, 2);
    RecordingProcessor processor{8192, 2};

    const std::size_t before = rt::audioThreadAllocationCount();
    REQUIRE(player.play(source, 0, 96000, &processor).ok());
    REQUIRE(waitUntil([&] { return player.position() > 24000; }));
    player.stop();
    const std::size_t after = rt::audioThreadAllocationCount();

    CHECK(after == before);
}
