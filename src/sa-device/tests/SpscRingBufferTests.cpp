#include <sa/core/RealtimeGuard.h>
#include <sa/device/SpscRingBuffer.h>

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <random>
#include <span>
#include <vector>

using namespace sa;
using sa::device::SpscRingBuffer;

namespace {

/// A ramp of exactly representable values: every float here is an integer below
/// 2^24, so every comparison in these tests can be exact. An epsilon comparison
/// would hide precisely the bug we are hunting -- a sample landing one slot out.
std::vector<float> ramp(std::size_t count, std::size_t first = 0) {
    std::vector<float> values(count);
    for (std::size_t i = 0; i < count; ++i) {
        values[i] = static_cast<float>(first + i);
    }
    return values;
}

/// Writes everything, retrying around partial writes. Fails the test rather
/// than looping forever if the buffer cannot take it all.
void writeAll(SpscRingBuffer& ring, const std::vector<float>& values) {
    std::size_t offset = 0;
    while (offset < values.size()) {
        const std::size_t taken = ring.write(values.data() + offset, values.size() - offset);
        REQUIRE(taken > 0);
        offset += taken;
    }
}

std::vector<float> readUpTo(SpscRingBuffer& ring, std::size_t count) {
    std::vector<float> out(count, -1.0f);
    const std::size_t taken = ring.read(out.data(), out.size());
    out.resize(taken);
    return out;
}

} // namespace

TEST_CASE("A new buffer is empty and holds its whole capacity", "[device][ring]") {
    SpscRingBuffer ring{16};

    CHECK(ring.capacity() == 16);
    CHECK(ring.isEmpty());
    CHECK_FALSE(ring.isFull());
    CHECK(ring.availableToRead() == 0);

    // Not 15: the doubled index range means no slot is sacrificed to tell full
    // from empty, so the capacity asked for is the capacity delivered.
    CHECK(ring.availableToWrite() == 16);
}

TEST_CASE("Samples come back in the order they went in", "[device][ring]") {
    SpscRingBuffer ring{64};
    const std::vector<float> sent = ramp(40);

    CHECK(ring.write(sent.data(), sent.size()) == 40);
    CHECK(ring.availableToRead() == 40);
    CHECK(ring.availableToWrite() == 24);

    const std::vector<float> received = readUpTo(ring, 40);
    CHECK(received == sent);
    CHECK(ring.isEmpty());
}

TEST_CASE("The span overloads agree with the pointer overloads", "[device][ring]") {
    SpscRingBuffer ring{8};
    const std::array<float, 4> sent{1.0f, 2.0f, 3.0f, 4.0f};
    std::array<float, 4> received{};

    CHECK(ring.write(std::span<const float>{sent}) == 4);
    CHECK(ring.read(std::span<float>{received}) == 4);
    CHECK(received == sent);
}

TEST_CASE("Writing more than fits is a partial write", "[device][ring]") {
    SpscRingBuffer ring{8};
    const std::vector<float> sent = ramp(20);

    CHECK(ring.write(sent.data(), sent.size()) == 8);
    CHECK(ring.isFull());

    // The rest is the producer's problem: the buffer refuses rather than
    // blocking, because blocking here would be a blocked audio thread.
    CHECK(ring.write(sent.data() + 8, 12) == 0);

    const std::vector<float> received = readUpTo(ring, 8);
    CHECK(received == ramp(8));
}

TEST_CASE("Reading more than is present is a partial read", "[device][ring]") {
    SpscRingBuffer ring{8};
    const std::vector<float> sent = ramp(3);
    writeAll(ring, sent);

    std::array<float, 8> destination{};
    destination.fill(-1.0f);
    CHECK(ring.read(destination.data(), destination.size()) == 3);

    CHECK(destination[0] == 0.0f);
    CHECK(destination[1] == 1.0f);
    CHECK(destination[2] == 2.0f);
    CHECK(destination[3] == -1.0f); // untouched beyond what was available
    CHECK(ring.isEmpty());
}

TEST_CASE("Reading an empty buffer delivers nothing", "[device][ring]") {
    SpscRingBuffer ring{8};
    std::array<float, 4> destination{};

    CHECK(ring.read(destination.data(), destination.size()) == 0);
    CHECK(ring.discard(4) == 0);
    CHECK(ring.isEmpty());
    CHECK(ring.availableToWrite() == 8);
}

TEST_CASE("Exactly full and exactly empty stay distinguishable", "[device][ring]") {
    // The classic ring-buffer bug: with indices in [0, capacity) both states
    // read as "write == read". Each lap below ends with the two indices
    // numerically equal modulo the capacity, so a buffer that conflated them
    // would report the wrong one here.
    SpscRingBuffer ring{4};
    const std::vector<float> block = ramp(4);

    for (int lap = 0; lap < 5; ++lap) {
        CHECK(ring.write(block.data(), block.size()) == 4);
        CHECK(ring.isFull());
        CHECK_FALSE(ring.isEmpty());
        CHECK(ring.availableToRead() == 4);
        CHECK(ring.availableToWrite() == 0);

        CHECK(readUpTo(ring, 4) == block);
        CHECK(ring.isEmpty());
        CHECK_FALSE(ring.isFull());
        CHECK(ring.availableToRead() == 0);
        CHECK(ring.availableToWrite() == 4);
    }
}

TEST_CASE("A block that straddles the end of the storage is reassembled", "[device][ring]") {
    SpscRingBuffer ring{8};

    // Park the write position near the end so the next block has to wrap.
    writeAll(ring, ramp(6));
    CHECK(readUpTo(ring, 6).size() == 6);

    const std::vector<float> straddling = ramp(6, 100);
    writeAll(ring, straddling);
    CHECK(ring.availableToRead() == 6);
    CHECK(readUpTo(ring, 6) == straddling);
}

TEST_CASE("Odd-sized traffic survives many laps of the buffer", "[device][ring]") {
    // Every write and read lands at a different offset, so the wrap arithmetic
    // is exercised at every alignment rather than only at the tidy ones.
    constexpr std::size_t kCapacity = 13;
    constexpr std::size_t kTotal = 5000;

    SpscRingBuffer ring{kCapacity};
    std::mt19937 rng{20260918};
    std::uniform_int_distribution<std::size_t> chunk{1, 7};

    std::size_t produced = 0;
    std::size_t consumed = 0;
    std::vector<float> staging(8);
    std::vector<float> received(8);

    while (consumed < kTotal) {
        if (produced < kTotal) {
            const std::size_t want = std::min(chunk(rng), kTotal - produced);
            for (std::size_t i = 0; i < want; ++i) {
                staging[i] = static_cast<float>(produced + i);
            }
            produced += ring.write(staging.data(), want);
        }

        const std::size_t taken = ring.read(received.data(), chunk(rng));
        for (std::size_t i = 0; i < taken; ++i) {
            REQUIRE(received[i] == static_cast<float>(consumed));
            ++consumed;
        }
    }

    CHECK(produced == kTotal);
    CHECK(ring.isEmpty());
}

TEST_CASE("Discarding drops the oldest samples and clamps", "[device][ring]") {
    SpscRingBuffer ring{8};
    writeAll(ring, ramp(6));

    CHECK(ring.discard(2) == 2);
    CHECK(ring.availableToRead() == 4);
    CHECK(readUpTo(ring, 1).front() == 2.0f);

    CHECK(ring.discard(100) == 3);
    CHECK(ring.isEmpty());
}

TEST_CASE("Reset returns the buffer to empty", "[device][ring]") {
    SpscRingBuffer ring{8};
    writeAll(ring, ramp(5));

    ring.reset();

    CHECK(ring.isEmpty());
    CHECK(ring.availableToWrite() == 8);
    CHECK(readUpTo(ring, 8).empty());
}

TEST_CASE("A zero-capacity buffer is inert rather than dangerous", "[device][ring]") {
    // "No device open yet" should not need a null check on the audio thread.
    SpscRingBuffer ring{0};
    const std::array<float, 4> sent{1.0f, 2.0f, 3.0f, 4.0f};
    std::array<float, 4> received{};

    CHECK(ring.capacity() == 0);
    CHECK(ring.isEmpty());
    CHECK(ring.isFull());
    CHECK(ring.write(sent.data(), sent.size()) == 0);
    CHECK(ring.read(received.data(), received.size()) == 0);
    CHECK(ring.discard(4) == 0);
}

TEST_CASE("Zero-length and null transfers are no-ops", "[device][ring]") {
    SpscRingBuffer ring{8};
    const std::array<float, 4> sent{1.0f, 2.0f, 3.0f, 4.0f};
    std::array<float, 4> received{};

    CHECK(ring.write(sent.data(), 0) == 0);
    CHECK(ring.write(nullptr, 4) == 0);
    CHECK(ring.isEmpty());

    writeAll(ring, ramp(4));
    CHECK(ring.read(received.data(), 0) == 0);
    CHECK(ring.read(nullptr, 4) == 0);
    CHECK(ring.availableToRead() == 4);
}

TEST_CASE("A default-constructed buffer behaves like a zero-capacity one", "[device][ring]") {
    SpscRingBuffer ring;
    const std::array<float, 2> sent{1.0f, 2.0f};

    CHECK(ring.capacity() == 0);
    CHECK(ring.write(sent.data(), sent.size()) == 0);
}

TEST_CASE("Write and read allocate nothing on the audio thread", "[device][ring][rt]") {
    if (!rt::checksEnabled()) {
        SUCCEED("SA_RT_SAFETY_CHECKS is off in this build; detection not compiled in");
        return;
    }

    // Everything the transfer touches is allocated up front, exactly as the
    // device layer requires of its callers.
    SpscRingBuffer ring{1024};
    std::array<float, 256> block{};

    std::size_t allocations = 0;
    std::size_t written = 0;
    std::size_t readBack = 0;
    std::size_t discarded = 0;
    {
        const rt::ScopedAudioThread guard;
        const rt::AllocationScope scope;

        written = ring.write(block.data(), block.size());
        readBack = ring.read(block.data(), block.size());
        written += ring.write(block.data(), block.size());
        discarded = ring.discard(block.size());

        // Read before leaving the scope: the assertion machinery below
        // allocates, and would be counted if it ran first.
        allocations = scope.count();
    }

    CHECK(allocations == 0);
    CHECK(written == 512);
    CHECK(readBack == 256);
    CHECK(discarded == 256);
}
