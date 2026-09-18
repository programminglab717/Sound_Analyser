#pragma once

#include <atomic>
#include <functional>

namespace sa {

/// Cooperative cancellation for long-running work.
///
/// Every analysis job must be cancellable (docs/03-architecture.md §3): a user
/// who opens the wrong two-hour file should not have to wait for it. Workers
/// poll this at a coarse granularity -- often enough to feel responsive, rarely
/// enough that the atomic load does not show up in a profile.
class CancellationToken {
public:
    void cancel() noexcept { cancelled_.store(true, std::memory_order_relaxed); }

    [[nodiscard]] bool isCancelled() const noexcept {
        return cancelled_.load(std::memory_order_relaxed);
    }

    void reset() noexcept { cancelled_.store(false, std::memory_order_relaxed); }

private:
    std::atomic<bool> cancelled_{false};
};

/// Progress reporting and cancellation for a long-running job.
///
/// Both parts are optional, so callers that want neither pass a default-
/// constructed instance and pay one null check per poll.
struct JobMonitor {
    /// Called with completion in [0, 1]. May be empty. Called from the worker
    /// thread, so an implementation that touches the UI must marshal.
    std::function<void(double)> onProgress;

    /// Not owned; must outlive the job. May be null.
    const CancellationToken* cancellation = nullptr;

    [[nodiscard]] bool shouldCancel() const noexcept {
        return cancellation != nullptr && cancellation->isCancelled();
    }

    void report(double fraction) const {
        if (onProgress) {
            onProgress(fraction);
        }
    }
};

} // namespace sa
