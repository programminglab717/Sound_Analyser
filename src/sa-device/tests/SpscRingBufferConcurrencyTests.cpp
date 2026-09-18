#include <sa/device/SpscRingBuffer.h>

#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

using sa::device::SpscRingBuffer;

namespace {

/// How long either side may make no progress before we call it a lost sample
/// rather than wait forever. A dropped or duplicated value would otherwise hang
/// the suite instead of failing it, which on CI is the difference between a
/// diagnosis and a timeout.
constexpr auto kStallTimeout = std::chrono::seconds{30};

/// Result of one side of a transfer. Catch2's assertion macros are not
/// thread-safe, so each thread records what it saw and the main thread does the
/// asserting after the join.
struct TransferOutcome {
    std::size_t count = 0;
    bool stalled = false;
    bool outOfOrder = false;

    /// Index of the first value that was not the one expected. Distinguishes a
    /// lost sample from a duplicated or torn one when something does go wrong.
    std::size_t firstBadIndex = 0;
    float expectedValue = 0.0f;
    float actualValue = 0.0f;
};

/// Counter ramp as exactly representable floats.
///
/// Integers below 2^24 are exact in float, so the comparison can be equality
/// and any bit that moved anywhere is visible; an epsilon comparison would hide
/// exactly the off-by-one-slot bug being hunted. Taking the counter modulo 2^23
/// keeps that exactness however long the run is, and still pins position: the
/// consumer derives the expected value from its own count, so a lost or
/// duplicated sample shifts the phase and fails on the very next value.
[[nodiscard]] float rampValue(std::size_t index) noexcept {
    return static_cast<float>(index & 0x7FFFFFU);
}

/// Deterministic chunk sizes in [1, limit]. A fixed size would exercise one
/// alignment against the buffer end; this walks every alignment, which is where
/// wrap bugs live. xorshift rather than <random> so the loop stays tight enough
/// to interleave the two threads properly.
class ChunkSizes {
public:
    explicit ChunkSizes(std::uint32_t seed) noexcept : state_(seed) {}

    [[nodiscard]] std::size_t next(std::size_t limit) noexcept {
        state_ ^= state_ << 13U;
        state_ ^= state_ >> 17U;
        state_ ^= state_ << 5U;
        return 1U + static_cast<std::size_t>(state_) % limit;
    }

private:
    std::uint32_t state_;
};

TransferOutcome produce(SpscRingBuffer& ring, std::size_t total, std::size_t maxChunk,
                        std::uint32_t seed) {
    TransferOutcome outcome;
    ChunkSizes chunks{seed};
    std::vector<float> staging(maxChunk);
    auto lastProgress = std::chrono::steady_clock::now();

    while (outcome.count < total) {
        const std::size_t want = std::min(chunks.next(maxChunk), total - outcome.count);
        for (std::size_t i = 0; i < want; ++i) {
            staging[i] = rampValue(outcome.count + i);
        }

        // A partial write must leave the remainder to be retried, never dropped
        // and never written twice -- that is the invariant the consumer checks.
        std::size_t offset = 0;
        while (offset < want) {
            const std::size_t taken = ring.write(staging.data() + offset, want - offset);
            if (taken == 0) {
                if (std::chrono::steady_clock::now() - lastProgress > kStallTimeout) {
                    outcome.stalled = true;
                    return outcome;
                }
                std::this_thread::yield();
                continue;
            }
            offset += taken;
            lastProgress = std::chrono::steady_clock::now();
        }

        outcome.count += want;
    }

    return outcome;
}

TransferOutcome consume(SpscRingBuffer& ring, std::size_t total, std::size_t maxChunk,
                        std::uint32_t seed) {
    TransferOutcome outcome;
    ChunkSizes chunks{seed};
    std::vector<float> received(maxChunk);
    auto lastProgress = std::chrono::steady_clock::now();

    while (outcome.count < total) {
        const std::size_t taken = ring.read(received.data(), chunks.next(maxChunk));
        if (taken == 0) {
            if (std::chrono::steady_clock::now() - lastProgress > kStallTimeout) {
                outcome.stalled = true;
                return outcome;
            }
            std::this_thread::yield();
            continue;
        }
        lastProgress = std::chrono::steady_clock::now();

        for (std::size_t i = 0; i < taken; ++i) {
            const float expected = rampValue(outcome.count);
            if (received[i] != expected) {
                outcome.outOfOrder = true;
                outcome.firstBadIndex = outcome.count;
                outcome.expectedValue = expected;
                outcome.actualValue = received[i];
                return outcome;
            }
            ++outcome.count;
        }
    }

    return outcome;
}

void runTransfer(std::size_t capacity, std::size_t total, std::size_t maxChunk) {
    SpscRingBuffer ring{capacity};

    TransferOutcome producer;
    TransferOutcome consumer;

    std::thread producerThread{[&] { producer = produce(ring, total, maxChunk, 0x9E3779B9U); }};
    std::thread consumerThread{[&] { consumer = consume(ring, total, maxChunk, 0x85EBCA6BU); }};
    producerThread.join();
    consumerThread.join();

    if (consumer.outOfOrder) {
        // Reported only on failure, so the happy path stays quiet: the index
        // says whether a sample was lost, duplicated, or read before it was
        // written.
        INFO("first mismatch at " << consumer.firstBadIndex << ": expected "
                                  << consumer.expectedValue << ", got " << consumer.actualValue);
        FAIL("the ramp did not arrive intact");
    }

    CHECK_FALSE(producer.stalled);
    CHECK_FALSE(consumer.stalled);

    CHECK(producer.count == total);
    CHECK(consumer.count == total);
    CHECK(ring.isEmpty());
}

} // namespace

TEST_CASE("A ramp crosses a small buffer intact under contention", "[device][ring][concurrency]") {
    // A small buffer against a long sequence is the whole point: both sides
    // spend most of their time at the full and empty boundaries, which is where
    // the memory ordering and the full-versus-empty distinction actually get
    // exercised. Eight million samples is around 350 000 write calls against as
    // many reads -- long enough that a mis-ordered store has to show up rather
    // than hide behind a run too short to interleave.
    runTransfer(/*capacity=*/64, /*total=*/1U << 23U, /*maxChunk=*/48);
}

TEST_CASE("A single-slot buffer still delivers every sample in order",
          "[device][ring][concurrency]") {
    // Degenerate capacity, and the harshest interleaving available: every write
    // fills the buffer and every read empties it, so the two threads hand off
    // on each individual sample instead of on each block.
    runTransfer(/*capacity=*/1, /*total=*/200000, /*maxChunk=*/4);
}

TEST_CASE("A buffer larger than the traffic never blocks the producer",
          "[device][ring][concurrency]") {
    // The mirror case: the producer always wins the race, so the consumer is
    // the one constantly finding the buffer empty.
    runTransfer(/*capacity=*/4096, /*total=*/1000000, /*maxChunk=*/128);
}

TEST_CASE("The consumer drains everything after the producer has finished",
          "[device][ring][concurrency]") {
    // Shutdown ordering: a consumer that stops as soon as it sees an empty
    // buffer loses the tail. Here the producer is joined first, so anything
    // still in flight has to come out afterwards.
    constexpr std::size_t kTotal = 4096;
    SpscRingBuffer ring{kTotal};

    TransferOutcome producer;
    std::thread producerThread{[&] { producer = produce(ring, kTotal, 64, 0xC2B2AE35U); }};
    producerThread.join();

    const TransferOutcome consumer = consume(ring, kTotal, 64, 0x27D4EB2FU);

    CHECK(producer.count == kTotal);
    CHECK(consumer.count == kTotal);
    CHECK_FALSE(consumer.outOfOrder);
    CHECK(ring.isEmpty());
}
