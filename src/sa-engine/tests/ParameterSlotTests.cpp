#include <sa/core/RealtimeGuard.h>
#include <sa/engine/ParameterSlot.h>

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
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
    //
    // The writer's work is a fixed count rather than a wall-clock slice, and
    // nothing here asserts a rate. This machine shares four cores between four
    // agents, and a thread can lose most of a timeslice to something else
    // entirely; an assertion on how many hand-offs fit into 400 ms measures the
    // load on the box rather than anything about the code. What is asserted
    // instead is deterministic: the reader keeps going until the writer has
    // finished and the slot is drained, so the last value published is always
    // the last value collected, whatever the scheduler did in between.
    constexpr std::uint64_t kPublishes = 20000;

    ParameterSlot<Stamped> slot;
    std::atomic<bool> writing{true};

    std::thread writer{[&] {
        for (std::uint64_t value = 1; value <= kPublishes; ++value) {
            slot.publish(Stamped::of(value));
            if (value % 256 == 0) {
                // Yielded now and then so the reader gets turns on a machine
                // with one core to spare. Yielding on every publish instead
                // hands the scheduler a decision hundreds of thousands of
                // times and starves the writer under load.
                std::this_thread::yield();
            }
        }
        writing.store(false, std::memory_order_release);
    }};

    std::uint64_t collections = 0;
    std::uint64_t torn = 0;
    std::uint64_t backwards = 0;
    std::uint64_t last = 0;
    Stamped collected;

    while (writing.load(std::memory_order_acquire) || slot.hasFreshValue()) {
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

    writer.join();

    INFO("collected " << collections << " of " << kPublishes << " publishes");
    CHECK(torn == 0);
    // Latest-value semantics: a collected value is never older than the one
    // before it, however many publishes went past uncollected.
    CHECK(backwards == 0);
    CHECK(collections > 0);
    // And the newest publish always arrives. Nothing is left stranded in the
    // slot, which is the property a preview depends on -- the last thing the
    // user did is the thing they hear.
    CHECK(last == kPublishes);
}
