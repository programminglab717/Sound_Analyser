#pragma once

#include <sa/core/Types.h>

#include <cstdint>
#include <string>

namespace sa::engine {

/// Strongly-typed identifiers.
///
/// Distinct types rather than bare integers because a clip index and a source
/// index are both `int` and mixing them silently edits the wrong thing.
enum class SourceId : std::uint64_t { Invalid = 0 };
enum class ClipId : std::uint64_t { Invalid = 0 };

/// Shape of a fade curve.
enum class FadeShape {
    Linear,
    /// Constant power. The correct default for a crossfade: two linear fades
    /// summed dip by 3 dB in the middle, which is audible as a hole.
    EqualPower,
    Logarithmic,
    Exponential,
    SCurve,
};

/// Gain multiplier at `position` samples into a fade of `length` samples.
/// Returns 1.0 outside the fade, so callers need no special cases.
[[nodiscard]] float fadeGain(FadeShape shape, SampleIndex position, SampleCount length,
                             bool fadingIn) noexcept;

struct Fade {
    SampleCount length = 0;
    FadeShape shape = FadeShape::EqualPower;
};

/// A region of a source placed on the timeline.
///
/// A clip owns no audio. It is a reference plus the decisions applied to it,
/// which is what makes an edit cheap and reversible: trimming a clip rewrites
/// two integers rather than moving samples.
struct Clip {
    ClipId id = ClipId::Invalid;
    SourceId source = SourceId::Invalid;

    /// Offset into the source where this clip's audio begins.
    SampleIndex sourceStart = 0;

    /// Length in samples. Never extends past the end of the source.
    SampleCount length = 0;

    /// Where the clip starts on the timeline.
    SampleIndex timelineStart = 0;

    /// Linear gain applied to the whole clip.
    float gain = 1.0f;

    Fade fadeIn;
    Fade fadeOut;

    std::string name;

    [[nodiscard]] SampleIndex timelineEnd() const noexcept { return timelineStart + length; }

    [[nodiscard]] bool overlaps(SampleIndex start, SampleIndex end) const noexcept {
        return timelineStart < end && start < timelineEnd();
    }

    /// Combined gain at `offset` samples into the clip, including both fades.
    [[nodiscard]] float gainAt(SampleIndex offset) const noexcept;
};

/// A named point or region on the timeline.
struct Marker {
    SampleIndex position = 0;
    SampleCount length = 0; ///< 0 for a point marker
    std::string label;
};

} // namespace sa::engine
