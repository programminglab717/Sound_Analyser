#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

/// One value handed from a control thread to the audio thread, without a lock.
///
/// docs/03-architecture.md §3 says the audio thread reads parameter updates
/// that were built elsewhere and never builds anything itself. This is the
/// mechanism for a parameter too big to be an atomic: a set of filter
/// coefficients, a gain envelope, anything the writer assembles as a whole and
/// the reader must see as a whole.
///
/// It is a *latest value* slot, not a queue. A drag over an EQ band emits
/// hundreds of updates a second and only the newest one is worth hearing, so a
/// publish that the reader has not collected is overwritten rather than
/// buffered. That is also what makes the writer wait-free: it never has to wait
/// for space, because there is no space to run out of.
///
///
/// Why three slots
/// ---------------
/// Two is not enough. With two, the writer's next write lands on the buffer the
/// reader may be reading from, so either the writer waits or the reader tears.
/// With three, the invariant is that the writer's slot, the reader's slot and
/// the slot named by `state_` are always a permutation of {0, 1, 2} -- each
/// exchange swaps one party's slot with the published one atomically, which
/// preserves the permutation -- so no slot is ever written and read at the same
/// time. Both sides are then a single `exchange` plus a copy, with no retry
/// loop and no unbounded work.
///
/// A seqlock would be smaller and is the other obvious answer, but its reader
/// reads the payload while the writer may be writing it and retries on a
/// version mismatch. That is a data race under the C++ memory model however
/// well it behaves in practice, and ThreadSanitizer reports it as one. A slot
/// nobody else can touch is race-free by construction, which is a property that
/// can be tested rather than argued.
///
///
/// What is not claimed
/// -------------------
/// Exactly one writer and exactly one *other* reader, as with
/// device::SpscRingBuffer -- a second of either corrupts the slot ownership,
/// and there is deliberately no guard, because the guard would be a cost the
/// audio thread pays on every block for a design-time property.
///
/// Nothing here says *when* the reader sees a publish. It sees it the next time
/// it asks, which for an audio callback means within one block. Nothing here
/// smooths anything either: delivering a changed value is this class's whole
/// job, and making the change inaudible belongs to whatever receives it.
namespace sa::engine {

template <typename T>
class ParameterSlot {
public:
    static_assert(std::is_trivially_copyable_v<T>,
                  "a value crossing to the audio thread must copy without running code");
    static_assert(std::is_nothrow_default_constructible_v<T>,
                  "the slots are constructed before either thread exists and must not throw");

    /// Starts with every slot holding a default-constructed value and nothing
    /// fresh, so a reader that runs before the first publish takes the default
    /// rather than waiting for one.
    ParameterSlot() = default;

    /// Neither copyable nor movable: both threads hold a bare pointer to this
    /// object, so relocating it under a live audio callback is a use-after-free.
    ParameterSlot(const ParameterSlot&) = delete;
    ParameterSlot& operator=(const ParameterSlot&) = delete;
    ParameterSlot(ParameterSlot&&) = delete;
    ParameterSlot& operator=(ParameterSlot&&) = delete;

    ~ParameterSlot() = default;

    /// Writer side. Makes `value` the one the reader will collect next.
    ///
    /// Wait-free: one copy into a slot no one else can see, then one exchange.
    /// A value the reader never collected is simply replaced.
    void publish(const T& value) noexcept {
        slots_[static_cast<std::size_t>(writeSlot_)] = value;
        // Release, so the copy above happens-before the reader's acquire below;
        // acquire, so the slot we take back -- which the reader has just
        // finished reading -- is ours to overwrite.
        const std::uint32_t previous =
            state_.exchange(writeSlot_ | kFreshBit, std::memory_order_acq_rel);
        writeSlot_ = previous & kSlotMask;
    }

    /// Reader side. Copies the newest unread value into `destination` and
    /// returns true, or returns false and leaves `destination` alone.
    ///
    /// Wait-free, allocation-free, and safe from the audio thread. The fast
    /// path when nothing has changed is a single relaxed load.
    [[nodiscard]] bool fetch(T& destination) noexcept {
        if ((state_.load(std::memory_order_relaxed) & kFreshBit) == 0) {
            return false;
        }
        const std::uint32_t previous = state_.exchange(readSlot_, std::memory_order_acq_rel);
        readSlot_ = previous & kSlotMask;
        destination = slots_[static_cast<std::size_t>(readSlot_)];
        return true;
    }

    /// True when a publish is waiting to be collected. A hint on either thread
    /// -- by the time it is acted on the answer may have changed -- and useful
    /// only for diagnostics and tests.
    [[nodiscard]] bool hasFreshValue() const noexcept {
        return (state_.load(std::memory_order_relaxed) & kFreshBit) != 0;
    }

private:
    /// The published slot index lives in the low two bits of `state_` and the
    /// "not yet collected" flag in the third, so one atomic word carries both
    /// and one exchange moves both.
    static constexpr std::uint32_t kSlotMask = 0x3u;
    static constexpr std::uint32_t kFreshBit = 0x4u;
    static constexpr std::uint32_t kInitialPublished = 2u;

    // The lock-free promise is only worth making if the word really is
    // lock-free; a standard library that fell back to a mutex here would put a
    // lock in the audio callback while every comment above still claimed
    // otherwise.
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
                  "ParameterSlot needs a lock-free 32-bit atomic to be audio-thread safe");

    std::array<T, 3> slots_{};

    /// Writer-owned. No other thread reads or writes it.
    std::uint32_t writeSlot_ = 0;

    /// Reader-owned. No other thread reads or writes it.
    std::uint32_t readSlot_ = 1;

    std::atomic<std::uint32_t> state_{kInitialPublished};
};

} // namespace sa::engine
