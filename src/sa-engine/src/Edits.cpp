#include <sa/engine/BufferSource.h>
#include <sa/engine/Edits.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace sa::engine {

namespace {

Status noSuchClip() {
    return Error{ErrorCode::NotFound, "no such clip"};
}

} // namespace

Result<std::pair<ClipId, ClipId>> splitClip(Document& document, ClipId clip, SampleIndex position) {
    Clip* original = document.timeline().findMutable(clip);
    if (original == nullptr) {
        return Error{ErrorCode::NotFound, "no such clip"};
    }
    if (position <= original->timelineStart || position >= original->timelineEnd()) {
        return Error{ErrorCode::OutOfRange, "split position is not inside the clip"};
    }

    const SampleCount firstLength = position - original->timelineStart;

    Clip second = *original;
    second.id = static_cast<ClipId>(document.nextId());
    document.setNextId(document.nextId() + 1);
    second.timelineStart = position;
    second.sourceStart = original->sourceStart + firstLength;
    second.length = original->length - firstLength;
    // The split point is a cut, not a fade: the first clip keeps its fade-in and
    // the second keeps its fade-out. Carrying both fades to both halves would
    // introduce two audible dips where the user asked for none.
    second.fadeIn = Fade{};

    original->length = firstLength;
    original->fadeOut = Fade{};

    const ClipId firstId = original->id;
    const ClipId secondId = second.id;

    // `original` dangles once insert() reallocates, so both ids are read out
    // before the call rather than after it.
    document.timeline().insert(std::move(second));
    return std::pair<ClipId, ClipId>{firstId, secondId};
}

Status trimClip(Document& document, ClipId clip, SampleIndex newTimelineStart,
                SampleCount newLength) {
    Clip* target = document.timeline().findMutable(clip);
    if (target == nullptr) {
        return noSuchClip();
    }
    if (newLength <= 0) {
        return Error{ErrorCode::InvalidArgument, "trimmed length must be positive"};
    }
    if (newTimelineStart < 0) {
        return Error{ErrorCode::OutOfRange, "trimmed start is negative"};
    }

    // Moving the start moves the window into the source by the same amount, so
    // the audio under the clip stays put instead of sliding.
    const SampleIndex delta = newTimelineStart - target->timelineStart;
    const SampleIndex newSourceStart = target->sourceStart + delta;
    if (newSourceStart < 0) {
        return Error{ErrorCode::OutOfRange, "trim would start before the source begins"};
    }

    const SourceEntry* entry = document.source(target->source);
    if (entry != nullptr && newSourceStart + newLength > entry->audio->info().frameCount) {
        return Error{ErrorCode::OutOfRange, "trim would extend past the end of the source"};
    }

    target->timelineStart = newTimelineStart;
    target->sourceStart = newSourceStart;
    target->length = newLength;

    // Fades cannot outlast the clip they belong to.
    target->fadeIn.length = std::min(target->fadeIn.length, newLength);
    target->fadeOut.length = std::min(target->fadeOut.length, newLength);

    document.timeline().reorder();
    return Status{};
}

Status moveClip(Document& document, ClipId clip, SampleIndex newTimelineStart) {
    Clip* target = document.timeline().findMutable(clip);
    if (target == nullptr) {
        return noSuchClip();
    }
    if (newTimelineStart < 0) {
        return Error{ErrorCode::OutOfRange, "position is negative"};
    }
    target->timelineStart = newTimelineStart;
    document.timeline().reorder();
    return Status{};
}

Status setClipGain(Document& document, ClipId clip, float gain) {
    Clip* target = document.timeline().findMutable(clip);
    if (target == nullptr) {
        return noSuchClip();
    }
    if (!(gain >= 0.0f) || gain > 1000.0f) {
        return Error{ErrorCode::InvalidArgument, "gain must be finite and in [0, 1000]"};
    }
    target->gain = gain;
    return Status{};
}

Status setClipFades(Document& document, ClipId clip, Fade fadeIn, Fade fadeOut) {
    Clip* target = document.timeline().findMutable(clip);
    if (target == nullptr) {
        return noSuchClip();
    }
    if (fadeIn.length < 0 || fadeOut.length < 0) {
        return Error{ErrorCode::InvalidArgument, "fade lengths must not be negative"};
    }
    if (fadeIn.length + fadeOut.length > target->length) {
        return Error{ErrorCode::InvalidArgument, "fades together exceed the clip length"};
    }
    target->fadeIn = fadeIn;
    target->fadeOut = fadeOut;
    return Status{};
}

Status deleteRange(Document& document, SampleIndex start, SampleIndex end, bool ripple) {
    if (end <= start) {
        return Error{ErrorCode::InvalidArgument, "range is empty"};
    }

    Timeline& timeline = document.timeline();
    const SampleCount removed = end - start;

    // Collect ids first: the edits below insert and erase, which invalidates any
    // iterator or pointer held across them.
    std::vector<ClipId> affected;
    for (const Clip& clip : timeline.clips()) {
        if (clip.overlaps(start, end) || (ripple && clip.timelineStart >= end)) {
            affected.push_back(clip.id);
        }
    }

    std::vector<Clip> additions;
    std::vector<ClipId> removals;

    for (ClipId id : affected) {
        Clip* clip = timeline.findMutable(id);
        if (clip == nullptr) {
            continue;
        }

        if (ripple && clip->timelineStart >= end) {
            clip->timelineStart -= removed;
            continue;
        }

        const SampleIndex clipStart = clip->timelineStart;
        const SampleIndex clipEnd = clip->timelineEnd();

        if (clipStart >= start && clipEnd <= end) {
            removals.push_back(id); // fully inside the range
            continue;
        }

        if (clipStart < start && clipEnd > end) {
            // The range is strictly inside the clip, so it becomes two pieces.
            Clip tail = *clip;
            tail.id = static_cast<ClipId>(document.nextId());
            document.setNextId(document.nextId() + 1);
            tail.sourceStart = clip->sourceStart + (end - clipStart);
            tail.length = clipEnd - end;
            tail.timelineStart = ripple ? start : end;
            tail.fadeIn = Fade{};

            clip->length = start - clipStart;
            clip->fadeOut = Fade{};
            additions.push_back(std::move(tail));
            continue;
        }

        if (clipStart < start) {
            clip->length = start - clipStart; // trim the tail off
            clip->fadeOut.length = std::min(clip->fadeOut.length, clip->length);
            continue;
        }

        // Starts inside the range and continues past it: trim the head off.
        const SampleCount cut = end - clipStart;
        clip->sourceStart += cut;
        clip->length -= cut;
        clip->timelineStart = ripple ? start : end;
        clip->fadeIn.length = std::min(clip->fadeIn.length, clip->length);
    }

    for (ClipId id : removals) {
        timeline.remove(id);
    }
    for (Clip& addition : additions) {
        timeline.insert(std::move(addition));
    }
    timeline.reorder();

    if (ripple) {
        for (Marker& marker : document.markers()) {
            if (marker.position >= end) {
                marker.position -= removed;
            } else if (marker.position > start) {
                marker.position = start;
            }
        }
    }
    return Status{};
}

Status insertSilence(Document& document, SampleIndex position, SampleCount length) {
    if (length <= 0) {
        return Error{ErrorCode::InvalidArgument, "length must be positive"};
    }
    if (position < 0) {
        return Error{ErrorCode::OutOfRange, "position is negative"};
    }

    Timeline& timeline = document.timeline();

    // Split anything spanning the insertion point first, so the silence lands
    // inside it rather than pushing the whole clip aside.
    std::vector<ClipId> spanning;
    for (const Clip& clip : timeline.clips()) {
        if (clip.timelineStart < position && clip.timelineEnd() > position) {
            spanning.push_back(clip.id);
        }
    }
    for (ClipId id : spanning) {
        auto split = splitClip(document, id, position);
        if (!split) {
            return split.error();
        }
    }

    timeline.shiftClipsFrom(position, length);

    for (Marker& marker : document.markers()) {
        if (marker.position >= position) {
            marker.position += length;
        }
    }
    return Status{};
}

Status crossfade(Document& document, ClipId first, ClipId second, SampleCount length,
                 FadeShape shape) {
    if (length <= 0) {
        return Error{ErrorCode::InvalidArgument, "crossfade length must be positive"};
    }

    Clip* a = document.timeline().findMutable(first);
    Clip* b = document.timeline().findMutable(second);
    if (a == nullptr || b == nullptr) {
        return noSuchClip();
    }
    if (a->timelineStart > b->timelineStart) {
        std::swap(a, b);
    }
    if (length > a->length || length > b->length) {
        return Error{ErrorCode::InvalidArgument, "crossfade is longer than one of the clips"};
    }

    // Pull the later clip back so the two genuinely overlap; a crossfade over a
    // gap is silence, and over a butt join is a click.
    const SampleIndex overlapStart = a->timelineEnd() - length;
    if (overlapStart < a->timelineStart) {
        return Error{ErrorCode::InvalidArgument, "clips are too short to overlap"};
    }

    const SampleIndex shift = b->timelineStart - overlapStart;
    b->timelineStart -= shift;
    b->sourceStart += shift;
    b->length -= shift;
    if (b->length <= 0) {
        return Error{ErrorCode::InvalidArgument, "crossfade would consume the second clip"};
    }

    // Matching shapes on both sides, so the pair is a true mirror and the
    // chosen curve's constant-sum property actually holds.
    a->fadeOut = Fade{length, shape};
    b->fadeIn = Fade{length, shape};

    document.timeline().reorder();
    return Status{};
}

Status splitAt(Document& document, SampleIndex position) {
    if (position < 0) {
        return Error{ErrorCode::OutOfRange, "position is negative"};
    }

    // Find the clip strictly containing the position. splitClip rejects a
    // position on an edge, which is exactly the case that needs no split.
    ClipId target = ClipId::Invalid;
    for (const Clip& clip : document.timeline().clips()) {
        if (clip.timelineStart < position && position < clip.timelineEnd()) {
            target = clip.id;
            break;
        }
    }
    if (target == ClipId::Invalid) {
        return Status{};
    }
    if (!splitClip(document, target, position)) {
        return Error{ErrorCode::InvalidArgument, "could not split at the range boundary"};
    }
    return Status{};
}

Status applyRangeGain(Document& document, SampleIndex start, SampleIndex end, float factor) {
    if (end <= start) {
        return Error{ErrorCode::InvalidArgument, "range is empty"};
    }
    if (!std::isfinite(factor) || factor < 0.0f) {
        return Error{ErrorCode::InvalidArgument, "gain factor must be finite and non-negative"};
    }

    if (auto status = splitAt(document, start); !status) {
        return status;
    }
    if (auto status = splitAt(document, end); !status) {
        return status;
    }

    // Collect first: setClipGain goes through the timeline, and holding a
    // pointer across it is how the earlier edits in this file got their
    // invalidation comments.
    std::vector<ClipId> inside;
    for (const Clip& clip : document.timeline().clips()) {
        if (clip.timelineStart >= start && clip.timelineEnd() <= end) {
            inside.push_back(clip.id);
        }
    }

    for (ClipId id : inside) {
        const Clip* clip = document.timeline().find(id);
        if (clip == nullptr) {
            continue;
        }
        if (auto status = setClipGain(document, id, clip->gain * factor); !status) {
            return status;
        }
    }
    return Status{};
}

Status flattenRange(Document& document, SampleIndex start, SampleIndex end) {
    if (end <= start) {
        return Error{ErrorCode::InvalidArgument, "range is empty"};
    }

    // Render what the range currently sounds like, gains and fades included,
    // then put that back as a single clip. This is the one place the model goes
    // destructive, and it does so deliberately: some operations cannot be
    // expressed as a decision attached to a clip, and pretending otherwise
    // produces an approximation nobody can reason about (ADR 0003).
    AudioBuffer rendered{document.layout(), end - start};
    if (auto status = document.render(start, rendered.view()); !status) {
        return status;
    }

    // Without ripple: the range keeps its place on the timeline, which is the
    // whole point of replacing it in situ.
    if (auto status = deleteRange(document, start, end, false); !status) {
        return status;
    }

    auto source = document.addSource(
        std::make_shared<BufferSource>(std::move(rendered), document.sampleRate()), "flattened");
    if (!source) {
        return source.error();
    }
    auto clip = document.appendSource(source.value(), start);
    if (!clip) {
        return clip.error();
    }
    return Status{};
}

Status applyRangeFade(Document& document, SampleIndex start, SampleIndex end, bool fadingIn,
                      FadeShape shape) {
    if (end <= start) {
        return Error{ErrorCode::InvalidArgument, "range is empty"};
    }

    if (auto status = splitAt(document, start); !status) {
        return status;
    }
    if (auto status = splitAt(document, end); !status) {
        return status;
    }

    const auto clipsInside = [&] {
        std::vector<ClipId> found;
        for (const Clip& clip : document.timeline().clips()) {
            if (clip.timelineStart >= start && clip.timelineEnd() <= end) {
                found.push_back(clip.id);
            }
        }
        return found;
    };

    std::vector<ClipId> inside = clipsInside();
    if (inside.empty()) {
        return Error{ErrorCode::NotFound, "nothing to fade in that range"};
    }

    // A fade runs from a clip's own edge, so a range spanning several clips
    // cannot carry one curve: each clip would restart it, which is audible as a
    // series of dips. Flatten to one clip first rather than approximate. After
    // a few edits any selection spans clips, so refusing instead would make
    // this unusable in practice.
    if (inside.size() > 1) {
        if (auto status = flattenRange(document, start, end); !status) {
            return status;
        }
        inside = clipsInside();
        if (inside.size() != 1) {
            return Error{ErrorCode::Unknown, "flattening did not leave a single clip"};
        }
    }

    const Clip* clip = document.timeline().find(inside.front());
    if (clip == nullptr) {
        return Error{ErrorCode::NotFound, "the clip vanished mid-edit"};
    }

    Fade fadeIn = clip->fadeIn;
    Fade fadeOut = clip->fadeOut;
    Fade& affected = fadingIn ? fadeIn : fadeOut;
    affected.shape = shape;
    affected.length = clip->length;

    return setClipFades(document, inside.front(), fadeIn, fadeOut);
}

Result<ClipId> duplicateClip(Document& document, ClipId clip, SampleIndex newTimelineStart) {
    const Clip* original = document.timeline().find(clip);
    if (original == nullptr) {
        return Error{ErrorCode::NotFound, "no such clip"};
    }
    if (newTimelineStart < 0) {
        return Error{ErrorCode::OutOfRange, "position is negative"};
    }

    Clip copy = *original;
    copy.id = static_cast<ClipId>(document.nextId());
    document.setNextId(document.nextId() + 1);
    copy.timelineStart = newTimelineStart;

    const ClipId id = copy.id;
    document.timeline().insert(std::move(copy));
    return id;
}

} // namespace sa::engine
