#include <sa/engine/Timeline.h>

#include <algorithm>

namespace sa::engine {

namespace {

bool earlier(const Clip& a, const Clip& b) noexcept {
    if (a.timelineStart != b.timelineStart) {
        return a.timelineStart < b.timelineStart;
    }
    // Stable tiebreak on id so ordering is deterministic for clips that start
    // together -- otherwise a render could mix them in a different order run to
    // run, which makes float results irreproducible.
    return a.id < b.id;
}

} // namespace

ClipId Timeline::insert(Clip clip) {
    const ClipId id = clip.id;
    const auto position = std::lower_bound(clips_.begin(), clips_.end(), clip, earlier);
    clips_.insert(position, std::move(clip));
    return id;
}

bool Timeline::remove(ClipId id) {
    const auto it = std::find_if(clips_.begin(), clips_.end(),
                                 [id](const Clip& clip) { return clip.id == id; });
    if (it == clips_.end()) {
        return false;
    }
    clips_.erase(it);
    return true;
}

const Clip* Timeline::find(ClipId id) const noexcept {
    const auto it = std::find_if(clips_.begin(), clips_.end(),
                                 [id](const Clip& clip) { return clip.id == id; });
    return it == clips_.end() ? nullptr : &*it;
}

Clip* Timeline::findMutable(ClipId id) noexcept {
    const auto it = std::find_if(clips_.begin(), clips_.end(),
                                 [id](const Clip& clip) { return clip.id == id; });
    return it == clips_.end() ? nullptr : &*it;
}

void Timeline::reorder() {
    std::stable_sort(clips_.begin(), clips_.end(), earlier);
}

void Timeline::shiftClipsFrom(SampleIndex position, SampleIndex delta) {
    if (delta == 0) {
        return;
    }
    for (Clip& clip : clips_) {
        if (clip.timelineStart >= position) {
            clip.timelineStart += delta;
        }
    }
    reorder();
}

void Timeline::collectOverlapping(SampleIndex start, SampleIndex end,
                                  std::vector<std::size_t>& out) const {
    out.clear();
    if (end <= start) {
        return;
    }

    // Clips are sorted by start, so a binary search finds the first clip that
    // could begin inside the range. A clip starting earlier can still overlap
    // if it is long, so the scan walks backwards from there until a clip ends
    // before the range begins -- bounded in practice because clips do not
    // usually nest deeply.
    Clip probe;
    probe.timelineStart = start;
    probe.id = ClipId::Invalid;
    const auto first = std::lower_bound(clips_.begin(), clips_.end(), probe, earlier);

    for (auto it = clips_.begin(); it != first; ++it) {
        if (it->timelineEnd() > start) {
            out.push_back(static_cast<std::size_t>(std::distance(clips_.begin(), it)));
        }
    }
    for (auto it = first; it != clips_.end() && it->timelineStart < end; ++it) {
        out.push_back(static_cast<std::size_t>(std::distance(clips_.begin(), it)));
    }
}

SampleCount Timeline::duration() const noexcept {
    SampleCount end = 0;
    for (const Clip& clip : clips_) {
        end = std::max(end, clip.timelineEnd());
    }
    return end;
}

} // namespace sa::engine
