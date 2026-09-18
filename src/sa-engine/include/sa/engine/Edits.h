#pragma once

#include <sa/core/Result.h>
#include <sa/engine/Document.h>

#include <utility>

namespace sa::engine {

/// Editing verbs.
///
/// Free functions rather than Document members so the document stays a data
/// model and editing policy lives apart from it. Each returns a Result and
/// leaves the document untouched on failure, so a caller can commit to the
/// undo history only when an edit actually happened.
///
/// None of these move audio. Splitting a clip writes two clip records; deleting
/// a range rewrites positions. That is the whole point of the model.

/// Split a clip at an absolute timeline position, producing two adjacent clips.
/// Fails if the position is not strictly inside the clip.
[[nodiscard]] Result<std::pair<ClipId, ClipId>> splitClip(Document& document, ClipId clip,
                                                          SampleIndex position);

/// Resize a clip by moving its start and/or end on the timeline, adjusting the
/// source offset so the audio under the clip does not slide.
[[nodiscard]] Status trimClip(Document& document, ClipId clip, SampleIndex newTimelineStart,
                              SampleCount newLength);

/// Move a clip without changing which audio it references.
[[nodiscard]] Status moveClip(Document& document, ClipId clip, SampleIndex newTimelineStart);

[[nodiscard]] Status setClipGain(Document& document, ClipId clip, float gain);

[[nodiscard]] Status setClipFades(Document& document, ClipId clip, Fade fadeIn, Fade fadeOut);

/// Delete everything in [start, end).
///
/// With `ripple`, later clips shift earlier to close the gap -- the behaviour a
/// dialogue editor wants. Without it, the range is silenced in place and timing
/// is preserved, which is what a music editor wants. Clips straddling a
/// boundary are trimmed; clips fully inside are removed.
[[nodiscard]] Status deleteRange(Document& document, SampleIndex start, SampleIndex end,
                                 bool ripple);

/// Insert `length` samples of silence at `position`, shifting later clips.
/// A clip spanning the position is split so the silence lands inside it.
[[nodiscard]] Status insertSilence(Document& document, SampleIndex position, SampleCount length);

/// Overlap two adjacent clips by `length` and apply matching fades.
///
/// **The shape choice is not cosmetic, and the right answer depends on the
/// material.** Equal-power holds constant loudness when the two clips are
/// uncorrelated -- different takes, different room tone, a scene change -- which
/// is the common case in dialogue editing, so it is the default. When the two
/// clips are the *same* continuous audio, as in a repair splice, their
/// amplitudes add coherently and equal-power produces a +3 dB bump in the
/// middle; linear is correct there and holds amplitude constant.
///
/// Getting this backwards is audible either as a dip or as a bump, which is why
/// it is a caller's decision rather than a fixed policy.
[[nodiscard]] Status crossfade(Document& document, ClipId first, ClipId second, SampleCount length,
                               FadeShape shape = FadeShape::EqualPower);

/// Duplicate a clip at a new position, referencing the same source audio.
[[nodiscard]] Result<ClipId> duplicateClip(Document& document, ClipId clip,
                                           SampleIndex newTimelineStart);

} // namespace sa::engine
