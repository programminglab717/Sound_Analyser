#pragma once

#include <cstddef>

/// Real-time safety enforcement.
///
/// The audio thread must never allocate, lock, block or throw
/// (docs/03-architecture.md §3). That rule decays within weeks unless it is
/// checked automatically, so this header provides the check.
///
/// When SA_RT_SAFETY_CHECKS is enabled the global operator new/delete are
/// instrumented: every allocation made while a thread is marked as the audio
/// thread increments a counter, which tests assert against. Builds with the
/// option off pay nothing -- the counters compile to constants.
namespace sa::rt {

/// True if the calling thread is currently marked as the audio thread.
[[nodiscard]] bool isAudioThread() noexcept;

/// Mark or unmark the calling thread. Prefer ScopedAudioThread.
void setAudioThread(bool value) noexcept;

/// Number of heap allocations made on audio-marked threads since process start.
/// Always 0 when SA_RT_SAFETY_CHECKS is off.
[[nodiscard]] std::size_t audioThreadAllocationCount() noexcept;

/// True if the build can actually detect audio-thread allocations. Lets a test
/// skip rather than pass vacuously in a release build.
[[nodiscard]] constexpr bool checksEnabled() noexcept {
#if defined(SA_RT_SAFETY_CHECKS)
    return true;
#else
    return false;
#endif
}

/// Marks the calling thread as the audio thread for the enclosing scope.
/// Install one at the top of every audio callback.
class ScopedAudioThread {
public:
    ScopedAudioThread() noexcept : previous_(isAudioThread()) { setAudioThread(true); }

    ~ScopedAudioThread() noexcept { setAudioThread(previous_); }

    ScopedAudioThread(const ScopedAudioThread&) = delete;
    ScopedAudioThread& operator=(const ScopedAudioThread&) = delete;

private:
    bool previous_;
};

/// Asserts that the guarded scope performs no heap allocation. Use in tests to
/// pin real-time safety of a processor:
///
///     SA_EXPECT_NO_ALLOCATIONS({ processor.process(view); });
class AllocationScope {
public:
    AllocationScope() noexcept : start_(audioThreadAllocationCount()) {}

    [[nodiscard]] std::size_t count() const noexcept {
        return audioThreadAllocationCount() - start_;
    }

private:
    std::size_t start_;
};

} // namespace sa::rt
