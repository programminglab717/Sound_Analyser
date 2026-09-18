#include <sa/core/RealtimeGuard.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <new>

namespace sa::rt {

namespace {

/// Whether the calling thread is currently inside an audio callback.
/// thread_local rather than a thread-id comparison so that the check is a
/// single load -- it sits on the allocation path in instrumented builds.
thread_local bool tlsIsAudioThread = false;

std::atomic<std::size_t>& allocationCounter() noexcept {
    static std::atomic<std::size_t> counter{0};
    return counter;
}

} // namespace

bool isAudioThread() noexcept {
    return tlsIsAudioThread;
}

void setAudioThread(bool value) noexcept {
    tlsIsAudioThread = value;
}

std::size_t audioThreadAllocationCount() noexcept {
    return allocationCounter().load(std::memory_order_relaxed);
}

#if defined(SA_RT_SAFETY_CHECKS)

namespace detail {

void noteAllocation() noexcept {
    if (tlsIsAudioThread) {
        allocationCounter().fetch_add(1, std::memory_order_relaxed);
    }
}

/// Portable aligned allocation.
///
/// Implemented by over-allocating and stashing the original pointer just below
/// the aligned address, rather than branching on _aligned_malloc vs
/// std::aligned_alloc. One code path, and every block frees with plain free().
void* alignedAllocate(std::size_t size, std::size_t alignment) noexcept {
    if (alignment < alignof(void*)) {
        alignment = alignof(void*);
    }
    const std::size_t total = size + alignment + sizeof(void*);
    void* raw = std::malloc(total);
    if (raw == nullptr) {
        return nullptr;
    }

    auto base = reinterpret_cast<std::uintptr_t>(raw) + sizeof(void*);
    const auto aligned = (base + alignment - 1) & ~(static_cast<std::uintptr_t>(alignment) - 1);
    auto* result = reinterpret_cast<void*>(aligned);
    static_cast<void**>(result)[-1] = raw;
    return result;
}

void alignedRelease(void* pointer) noexcept {
    if (pointer != nullptr) {
        std::free(static_cast<void**>(pointer)[-1]);
    }
}

} // namespace detail

#endif // SA_RT_SAFETY_CHECKS

} // namespace sa::rt

#if defined(SA_RT_SAFETY_CHECKS)

// Global allocation operators, instrumented.
//
// Every allocation made while a thread is marked as the audio thread bumps a
// counter that tests assert against. Without an automated check the "no
// allocation on the audio thread" rule decays within weeks
// (docs/03-architecture.md §3).
//
// These are defined in the same translation unit as the sa::rt entry points so
// that the linker always pulls the object file in from the static library.

void* operator new(std::size_t size) {
    sa::rt::detail::noteAllocation();
    void* memory = std::malloc(size != 0 ? size : 1);
    if (memory == nullptr) {
        throw std::bad_alloc{};
    }
    return memory;
}

void* operator new[](std::size_t size) {
    return ::operator new(size);
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    sa::rt::detail::noteAllocation();
    return std::malloc(size != 0 ? size : 1);
}

void* operator new[](std::size_t size, const std::nothrow_t& tag) noexcept {
    return ::operator new(size, tag);
}

void operator delete(void* memory) noexcept {
    std::free(memory);
}

void operator delete[](void* memory) noexcept {
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept {
    std::free(memory);
}

void operator delete[](void* memory, std::size_t) noexcept {
    std::free(memory);
}

void operator delete(void* memory, const std::nothrow_t&) noexcept {
    std::free(memory);
}

void operator delete[](void* memory, const std::nothrow_t&) noexcept {
    std::free(memory);
}

void* operator new(std::size_t size, std::align_val_t alignment) {
    sa::rt::detail::noteAllocation();
    void* memory =
        sa::rt::detail::alignedAllocate(size != 0 ? size : 1, static_cast<std::size_t>(alignment));
    if (memory == nullptr) {
        throw std::bad_alloc{};
    }
    return memory;
}

void* operator new[](std::size_t size, std::align_val_t alignment) {
    return ::operator new(size, alignment);
}

void operator delete(void* memory, std::align_val_t) noexcept {
    sa::rt::detail::alignedRelease(memory);
}

void operator delete[](void* memory, std::align_val_t) noexcept {
    sa::rt::detail::alignedRelease(memory);
}

void operator delete(void* memory, std::size_t, std::align_val_t) noexcept {
    sa::rt::detail::alignedRelease(memory);
}

void operator delete[](void* memory, std::size_t, std::align_val_t) noexcept {
    sa::rt::detail::alignedRelease(memory);
}

#endif // SA_RT_SAFETY_CHECKS
