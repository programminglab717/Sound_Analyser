#pragma once

#include <sa/core/Types.h>

#include <atomic>
#include <cstddef>
#include <cstring>
#include <memory>
#include <new>
#include <span>

namespace sa::device {

/// Lock-free single-producer / single-consumer ring buffer of float samples.
///
/// This is how audio crosses between the audio callback and any other thread
/// (docs/03-architecture.md §3). Both sides are wait-free and allocation-free,
/// so a stalled consumer costs samples but can never stall the callback --
/// which is the whole point: a blocked audio thread is a glitch, a dropped
/// block is a statistic.
///
/// Exactly one thread may call write() and exactly one *other* thread may call
/// read(). A second producer or a second consumer corrupts the indices. There
/// is deliberately no guard against that, because the guard would cost the
/// audio thread something on every call; ownership is a design-time property
/// here, not a run-time one.
///
/// Storage is allocated once, by the constructor. A failed allocation leaves
/// the buffer at zero capacity instead of throwing -- this module reports
/// failure, it does not throw -- so check capacity() when the size is large
/// enough for that to be a real possibility.
///
///
/// Memory ordering
/// ---------------
/// Each side owns exactly one index: the producer is the only writer of
/// writeIndex_, the consumer the only writer of readIndex_. So each side loads
/// *its own* index relaxed -- nobody else can have changed it -- and pays for
/// ordering only on the index it does not own:
///
///   producer  load readIndex_  acquire  pairs with the consumer's release
///                                       store, so the consumer has finished
///                                       reading the slots we are about to
///                                       overwrite before we overwrite them.
///             store writeIndex_ release  publishes the samples just written;
///                                       they happen-before the consumer's
///                                       acquire load observes the new index.
///   consumer  the mirror image.
///
/// Relaxed everywhere is the tempting shortcut and is simply a data race: the
/// index would carry no ordering, so a consumer could read a slot the producer
/// had not finished writing, and the result would be intermittent noise that
/// reproduces on one CPU and not another. seq_cst everywhere is correct but
/// buys a single total order across unrelated atomics that nothing here needs,
/// at the cost of a full barrier on every store -- on x86 an `mfence`-class
/// instruction in the audio callback, per block, for nothing.
///
///
/// Full versus empty
/// -----------------
/// The classic bug: with indices in [0, capacity) the full and empty states are
/// both "writeIndex == readIndex" and cannot be told apart. Here the indices
/// run over [0, 2 * capacity) instead, and the slot an index names is
/// `index % capacity`. The doubled range is one extra bit recording which lap
/// each side is on, which separates the two states cleanly:
///
///   empty  writeIndex == readIndex
///   full   writeIndex - readIndex == capacity   (modulo 2 * capacity)
///
/// The usual alternative -- leave one slot permanently unused -- costs a sample
/// of capacity and makes `capacity` a lie at the call site. The doubled range
/// costs one compare-and-subtract per advance instead, and keeps the indices
/// small enough that they can never overflow.
class SpscRingBuffer {
public:
    /// Ceiling on capacity, in samples (1 GiB of float). Keeps the doubled
    /// index range far from overflow and turns an absurd request into a visible
    /// clamp rather than a wild allocation.
    static constexpr std::size_t kMaxCapacity = std::size_t{1} << 28;

    SpscRingBuffer() = default;

    /// Allocates `capacity` samples, clamped to kMaxCapacity. A zero capacity
    /// is legal and yields a buffer that is permanently both empty and full;
    /// every write and read reports zero. That keeps "no device yet" from
    /// needing a null check on the audio thread.
    explicit SpscRingBuffer(std::size_t capacity) {
        const std::size_t wanted = capacity < kMaxCapacity ? capacity : kMaxCapacity;
        if (wanted == 0) {
            return;
        }
        storage_.reset(new (std::nothrow) float[wanted]());
        if (storage_ != nullptr) {
            capacity_ = wanted;
        }
    }

    /// Neither copyable nor movable: both sides hold a bare pointer to this
    /// object, so relocating it while either is running is a use-after-free
    /// waiting to happen. Hold one by value or behind unique_ptr.
    SpscRingBuffer(const SpscRingBuffer&) = delete;
    SpscRingBuffer& operator=(const SpscRingBuffer&) = delete;
    SpscRingBuffer(SpscRingBuffer&&) = delete;
    SpscRingBuffer& operator=(SpscRingBuffer&&) = delete;

    ~SpscRingBuffer() = default;

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

    /// Samples waiting to be read.
    ///
    /// Only the consumer may size a read from this. The consumer's own index is
    /// exact and the producer's can only be stale in the direction of *fewer*
    /// samples, so it never over-reports here -- whereas on the producer thread
    /// it can, because the consumer may have drained more than the producer has
    /// observed. Treat it as a hint anywhere but the consumer.
    [[nodiscard]] std::size_t availableToRead() const noexcept {
        const std::size_t readPos = readIndex_.load(std::memory_order_relaxed);
        const std::size_t writePos = writeIndex_.load(std::memory_order_acquire);
        return fillLevel(writePos, readPos);
    }

    /// Free space, in samples. The mirror image: safe to size a write from on
    /// the producer thread, a hint anywhere else.
    [[nodiscard]] std::size_t availableToWrite() const noexcept {
        const std::size_t writePos = writeIndex_.load(std::memory_order_relaxed);
        const std::size_t readPos = readIndex_.load(std::memory_order_acquire);
        return capacity_ - fillLevel(writePos, readPos);
    }

    /// True when the consumer has nothing to take. Carries the same caveat as
    /// availableToRead(): trustworthy on the consumer thread, a hint elsewhere.
    [[nodiscard]] bool isEmpty() const noexcept { return availableToRead() == 0; }

    /// True when the producer cannot place anything. Trustworthy on the
    /// producer thread, a hint elsewhere.
    [[nodiscard]] bool isFull() const noexcept { return availableToWrite() == 0; }

    /// Copies up to `count` samples in, returning how many were taken. A short
    /// write means the buffer filled: the producer decides whether that is a
    /// dropped block or a reason to retry, and the audio thread never waits.
    [[nodiscard]] std::size_t write(const float* source, std::size_t count) noexcept {
        if (source == nullptr || count == 0 || capacity_ == 0) {
            return 0;
        }

        const std::size_t writePos = writeIndex_.load(std::memory_order_relaxed);
        const std::size_t readPos = readIndex_.load(std::memory_order_acquire);
        const std::size_t free = capacity_ - fillLevel(writePos, readPos);
        const std::size_t taken = count < free ? count : free;
        if (taken == 0) {
            return 0;
        }

        const std::size_t offset = slotOf(writePos);
        const std::size_t firstRun = taken < capacity_ - offset ? taken : capacity_ - offset;
        std::memcpy(storage_.get() + offset, source, firstRun * sizeof(float));
        if (taken > firstRun) {
            std::memcpy(storage_.get(), source + firstRun, (taken - firstRun) * sizeof(float));
        }

        writeIndex_.store(advance(writePos, taken), std::memory_order_release);
        return taken;
    }

    [[nodiscard]] std::size_t write(std::span<const float> source) noexcept {
        return write(source.data(), source.size());
    }

    /// Copies up to `count` samples out, returning how many were delivered. A
    /// short read means the buffer ran dry -- for a playback consumer that is
    /// an underrun, and the caller should pad with silence rather than wait.
    [[nodiscard]] std::size_t read(float* destination, std::size_t count) noexcept {
        if (destination == nullptr || count == 0 || capacity_ == 0) {
            return 0;
        }

        const std::size_t readPos = readIndex_.load(std::memory_order_relaxed);
        const std::size_t writePos = writeIndex_.load(std::memory_order_acquire);
        const std::size_t used = fillLevel(writePos, readPos);
        const std::size_t taken = count < used ? count : used;
        if (taken == 0) {
            return 0;
        }

        const std::size_t offset = slotOf(readPos);
        const std::size_t firstRun = taken < capacity_ - offset ? taken : capacity_ - offset;
        std::memcpy(destination, storage_.get() + offset, firstRun * sizeof(float));
        if (taken > firstRun) {
            std::memcpy(destination + firstRun, storage_.get(), (taken - firstRun) * sizeof(float));
        }

        readIndex_.store(advance(readPos, taken), std::memory_order_release);
        return taken;
    }

    [[nodiscard]] std::size_t read(std::span<float> destination) noexcept {
        return read(destination.data(), destination.size());
    }

    /// Drops up to `count` unread samples, returning how many were dropped.
    /// Consumer-side. Recovering from an overrun by skipping stale audio is
    /// cheaper and less glitchy than draining it through a scratch buffer.
    [[nodiscard]] std::size_t discard(std::size_t count) noexcept {
        if (count == 0 || capacity_ == 0) {
            return 0;
        }

        const std::size_t readPos = readIndex_.load(std::memory_order_relaxed);
        const std::size_t writePos = writeIndex_.load(std::memory_order_acquire);
        const std::size_t used = fillLevel(writePos, readPos);
        const std::size_t taken = count < used ? count : used;
        if (taken == 0) {
            return 0;
        }

        readIndex_.store(advance(readPos, taken), std::memory_order_release);
        return taken;
    }

    /// Returns the buffer to empty. Not safe against a concurrent producer or
    /// consumer -- call it only between runs, while both sides are stopped.
    void reset() noexcept {
        writeIndex_.store(0, std::memory_order_relaxed);
        readIndex_.store(0, std::memory_order_relaxed);
    }

private:
    /// Producer and consumer spin on different indices from different cores.
    /// Sharing a cache line between them turns every hand-off into a coherence
    /// round trip, which is exactly the cost this class exists to avoid.
    static constexpr std::size_t kCacheLineSize = 64;

    [[nodiscard]] std::size_t indexLimit() const noexcept { return capacity_ * 2; }

    [[nodiscard]] std::size_t slotOf(std::size_t index) const noexcept {
        return index >= capacity_ ? index - capacity_ : index;
    }

    /// `count` never exceeds capacity, so one conditional subtraction is enough
    /// to fold the sum back into [0, 2 * capacity).
    [[nodiscard]] std::size_t advance(std::size_t index, std::size_t count) const noexcept {
        const std::size_t next = index + count;
        return next >= indexLimit() ? next - indexLimit() : next;
    }

    [[nodiscard]] std::size_t fillLevel(std::size_t writePos, std::size_t readPos) const noexcept {
        return writePos >= readPos ? writePos - readPos : writePos + indexLimit() - readPos;
    }

    // The wait-free promise is only worth making if the indices really are
    // lock-free; a libstdc++ that fell back to a mutex here would put a lock in
    // the audio callback while every comment above still claimed otherwise.
    static_assert(std::atomic<std::size_t>::is_always_lock_free,
                  "SpscRingBuffer needs lock-free size_t atomics to be audio-thread safe");

    std::unique_ptr<float[]> storage_;
    std::size_t capacity_ = 0;
    alignas(kCacheLineSize) std::atomic<std::size_t> writeIndex_{0};
    alignas(kCacheLineSize) std::atomic<std::size_t> readIndex_{0};
};

} // namespace sa::device
