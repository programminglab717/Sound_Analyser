#pragma once

#include <sa/engine/Clip.h>

#include <vector>

namespace sa::engine {

/// An ordered set of clips.
///
/// Clips are kept sorted by timeline position so that rendering a range is a
/// binary search plus a short forward scan rather than a walk over everything.
/// Overlap is allowed and mixes -- that is what a crossfade is.
class Timeline {
public:
    [[nodiscard]] const std::vector<Clip>& clips() const noexcept { return clips_; }

    [[nodiscard]] std::size_t clipCount() const noexcept { return clips_.size(); }

    [[nodiscard]] bool isEmpty() const noexcept { return clips_.empty(); }

    /// Insert, keeping the ordering invariant. Returns the clip's id.
    ClipId insert(Clip clip);

    /// Remove by id. Returns false if no such clip.
    bool remove(ClipId id);

    [[nodiscard]] const Clip* find(ClipId id) const noexcept;

    /// Mutable access. Re-sorts if the change moved the clip, so callers cannot
    /// break the ordering invariant by editing through the pointer.
    [[nodiscard]] Clip* findMutable(ClipId id) noexcept;

    /// Call after mutating a clip's timelineStart through findMutable.
    void reorder();

    /// Move every clip starting at or after `position` by `delta` samples.
    /// Ripple edits and silence insertion both need this, and doing it through
    /// the clip list directly would mean handing out mutable bulk access and
    /// trusting callers to re-sort.
    void shiftClipsFrom(SampleIndex position, SampleIndex delta);

    /// Indices of clips overlapping [start, end), in timeline order.
    void collectOverlapping(SampleIndex start, SampleIndex end,
                            std::vector<std::size_t>& out) const;

    /// One past the last sample any clip occupies.
    [[nodiscard]] SampleCount duration() const noexcept;

    void clear() noexcept { clips_.clear(); }

private:
    std::vector<Clip> clips_;
};

} // namespace sa::engine
