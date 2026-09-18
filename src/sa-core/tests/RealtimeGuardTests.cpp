#include <sa/core/AudioBuffer.h>
#include <sa/core/RealtimeGuard.h>

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <thread>
#include <vector>

using namespace sa;

TEST_CASE("A thread is not the audio thread by default", "[core][rt]") {
    CHECK_FALSE(rt::isAudioThread());
}

TEST_CASE("ScopedAudioThread marks and restores", "[core][rt]") {
    REQUIRE_FALSE(rt::isAudioThread());
    {
        const rt::ScopedAudioThread guard;
        CHECK(rt::isAudioThread());
        {
            const rt::ScopedAudioThread nested;
            CHECK(rt::isAudioThread());
        }
        CHECK(rt::isAudioThread());
    }
    CHECK_FALSE(rt::isAudioThread());
}

TEST_CASE("The audio-thread mark is per-thread", "[core][rt]") {
    const rt::ScopedAudioThread guard;
    REQUIRE(rt::isAudioThread());

    bool otherThreadSawMark = true;
    std::thread worker{[&] { otherThreadSawMark = rt::isAudioThread(); }};
    worker.join();

    CHECK_FALSE(otherThreadSawMark);
}

TEST_CASE("Allocating on the audio thread is detected", "[core][rt]") {
    if (!rt::checksEnabled()) {
        SUCCEED("SA_RT_SAFETY_CHECKS is off in this build; detection not compiled in");
        return;
    }

    const rt::ScopedAudioThread guard;
    const rt::AllocationScope scope;

    // Deliberately illegal on the audio thread -- this is the thing the guard
    // exists to catch.
    std::vector<float> offender(1024);
    offender[0] = 1.0f;

    CHECK(scope.count() > 0);
}

TEST_CASE("Buffer processing on the audio thread allocates nothing", "[core][rt]") {
    if (!rt::checksEnabled()) {
        SUCCEED("SA_RT_SAFETY_CHECKS is off in this build");
        return;
    }

    // Allocate up front, off the audio thread, as real code must.
    AudioBuffer buffer{ChannelLayout::stereo(), 512};

    const rt::ScopedAudioThread guard;
    const rt::AllocationScope scope;

    const AudioBufferView view = buffer.view();
    const AudioBufferView block = view.subRange(64, 128);
    for (int channel = 0; channel < block.channelCount(); ++channel) {
        float* samples = block.channel(channel);
        for (SampleCount i = 0; i < block.frames(); ++i) {
            samples[i] *= 0.5f;
        }
    }
    buffer.clear();

    CHECK(scope.count() == 0);
}

TEST_CASE("Allocation off the audio thread is not counted", "[core][rt]") {
    const rt::AllocationScope scope;
    std::vector<float> ordinary(4096);
    ordinary[0] = 1.0f;
    CHECK(scope.count() == 0);
}

TEST_CASE("Aligned allocation and release round-trip", "[core][rt]") {
    // Exercises the instrumented aligned operator new/delete pair: a mismatch
    // between them corrupts the heap rather than failing visibly.
    for (SampleCount frames : {1, 7, 63, 64, 65, 4096}) {
        AudioBuffer buffer{ChannelLayout::fiveOne(), frames};
        REQUIRE(buffer.frames() == frames);
        buffer.clear();
    }
    SUCCEED("no heap corruption across aligned allocate/free cycles");
}
