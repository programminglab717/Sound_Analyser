#include <sa/core/RealtimeGuard.h>
#include <sa/engine/ParameterSlot.h>

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <thread>

using namespace sa;
using namespace sa::engine;

namespace {

/// A payload whose fields must all agree.
///
/// Sixteen words is wide enough that a torn read -- some fields from one
/// publish, the rest from the next -- would have to be fantastically unlucky to
/// go unnoticed, and a torn read is the one failure this class exists to make
/// impossible.
struct Stamped {
    std::array<std::uint64_t, 16> fields{};

    [[nodiscard]] static Stamped of(std::uint64_t value) noexcept {
        Stamped stamped;
        stamped.fields.fill(value);
        return stamped;
    }

    [[nodiscard]] bool isWhole() const noexcept {
        for (const std::uint64_t field : fields) {
            if (field != fields[0]) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] std::uint64_t value() const noexcept { return fields[0]; }
};

} // namespace

TEST_CASE("A slot with nothing published has nothing to collect", "[engine][preview][slot]") {
    ParameterSlot<Stamped> slot;
    CHECK_FALSE(slot.hasFreshValue());

    Stamped collected = Stamped::of(99);
    CHECK_FALSE(slot.fetch(collected));
    CHECK(collected.value() == 99); // untouched
}

TEST_CASE("A published value is collected exactly once", "[engine][preview][slot]") {
    ParameterSlot<Stamped> slot;
    slot.publish(Stamped::of(7));
    CHECK(slot.hasFreshValue());

    Stamped collected;
    REQUIRE(slot.fetch(collected));
    CHECK(collected.value() == 7);
    CHECK(collected.isWhole());

    CHECK_FALSE(slot.hasFreshValue());
    CHECK_FALSE(slot.fetch(collected));
    CHECK(collected.value() == 7);
}

TEST_CASE("Only the newest publish is collected", "[engine][preview][slot]") {
    // A drag emits hundreds of updates a second and only the last one is worth
    // hearing, so an uncollected publish is replaced rather than queued.
    ParameterSlot<Stamped> slot;
    for (std::uint64_t i = 1; i <= 500; ++i) {
        slot.publish(Stamped::of(i));
    }

    Stamped collected;
    REQUIRE(slot.fetch(collected));
    CHECK(collected.value() == 500);
    CHECK_FALSE(slot.fetch(collected));
}

TEST_CASE("Collecting a value allocates nothing", "[engine][preview][slot][rt]") {
    if (!rt::checksEnabled()) {
        SUCCEED("SA_RT_SAFETY_CHECKS is off in this build; detection not compiled in");
        return;
    }

    ParameterSlot<Stamped> slot;
    slot.publish(Stamped::of(1));
    Stamped collected;

    std::size_t allocations = 0;
    {
        const rt::ScopedAudioThread guard;
        const rt::AllocationScope scope;
        for (int i = 0; i < 64; ++i) {
            // Both the "nothing new" fast path and the collecting path.
            (void)slot.fetch(collected);
        }
        allocations = scope.count();
    }

    CHECK(allocations == 0);
    CHECK(collected.value() == 1);
}

TEST_CASE("A writer and a reader can run at once", "[engine][preview][slot][threads]") {
    // The point of the test is what ThreadSanitizer says about it: the reader
    // touches a slot the writer cannot be touching, so there is no race to
    // find. The value checks below are the second line of defence -- a torn
    // read would show up as a payload whose fields disagree.
    ParameterSlot<Stamped> slot;
    std::atomic<bool> running{true};
    std::atomic<std::uint64_t> published{0};

    std::thread writer{[&] {
        std::uint64_t value = 0;
        while (running.load(std::memory_order_relaxed)) {
            slot.publish(Stamped::of(++value));
        }
        published.store(value, std::memory_order_relaxed);
    }};

    std::uint64_t collections = 0;
    std::uint64_t torn = 0;
    std::uint64_t backwards = 0;
    std::uint64_t last = 0;

    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
    while (std::chrono::steady_clock::now() < until) {
        Stamped collected;
        if (!slot.fetch(collected)) {
            continue;
        }
        ++collections;
        if (!collected.isWhole()) {
            ++torn;
        }
        if (collected.value() < last) {
            ++backwards;
        }
        last = collected.value();
    }

    running.store(false, std::memory_order_relaxed);
    writer.join();

    CHECK(torn == 0);
    // Latest-value semantics: a collected value is never older than the one
    // before it, however many publishes went past uncollected.
    CHECK(backwards == 0);
    CHECK(collections > 0);
    CHECK(published.load(std::memory_order_relaxed) > 0);
}
