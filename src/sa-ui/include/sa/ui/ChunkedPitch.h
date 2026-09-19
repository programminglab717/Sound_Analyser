#pragma once

#include <sa/analysis/PitchTrack.h>
#include <sa/core/AudioBuffer.h>
#include <sa/core/Cancellation.h>
#include <sa/core/Result.h>
#include <sa/core/Types.h>

#include <vector>

/// Tracking a pitch contour in pieces, so that a superseded run can be dropped.
///
/// Split out of AnalysisPanel for the reason AnalysisOverlay.h gives: this is
/// arithmetic that can be wrong, no window is needed to be wrong in it, and the
/// claim it makes -- that cutting the audio up changes nothing -- is one that
/// has to be checked against the whole-buffer tracker rather than argued.
namespace sa::ui {

/// A hop that gives a contour a few points per pixel of a wide window without
/// tracking a two-minute passage at the default 5.3 ms.
///
/// The default hop is right for a phrase and wasteful for a passage: at 48 kHz
/// it is 22,500 frames a minute, every one of them a transform, and a display
/// two thousand columns wide cannot show more than a fraction of them. So the
/// hop grows with the length, in powers of two, and stops at eight times the
/// default -- past which the contour starts to miss notes rather than merely
/// draw them with fewer points.
///
/// The hop is reported alongside the contour, because it is the time resolution
/// of everything drawn from it.
[[nodiscard]] SampleCount pitchHopFor(SampleCount frames, SampleCount base);

/// The contour, tracked a second of audio at a time so it can be given up on.
///
/// By far the most expensive thing the panel does -- on a two-minute passage it
/// is an order of magnitude more work than everything else put together -- and
/// the one place where a superseded run would otherwise hold the window still
/// for seconds. trackPitch() takes a buffer and runs to the end of it, so the
/// only way to make it interruptible is to hand it less at a time.
///
/// This produces the contour one call would have produced. The argument is
/// worth spelling out, and then worth holding to account. trackPitch places a
/// frame at every multiple of the hop for which window + longestLag samples
/// remain, and each frame's reading depends on nothing outside that span. So a
/// chunk starting at a multiple of the hop and carrying that span past its own
/// end gives its frames the identical samples they would have had, and the
/// frames kept from each chunk tile the whole exactly once. The alternative --
/// cutting the buffer up and keeping whatever came back -- drops the frames
/// straddling every seam, and a contour with a hole every second is a contour
/// that says "unvoiced" where it means "not looked at".
///
/// The holding to account is ChunkedPitchTests, and it does not come back with
/// quite the round answer. It tracks the same passage both ways at eight
/// lengths -- several chunks, a whole number of them, a sample past one, a tail
/// either side of a frame, one frame exactly, and less -- and hz, confidence
/// and voiced come back bit for bit every time, because the samples
/// a frame reads are the same bytes in both cases. The time does not, and the
/// reason is arithmetic and not audio: one call divides once, and this divides
/// the frame's offset and the chunk's and adds them. That lands within one unit
/// in the last place -- under 10^-15 s, a ten-thousand-millionth of a sample,
/// and never enough to move the sample a time converts to. The test holds the
/// readings and the times to those two different standards rather than blurring
/// them into one tolerance, which is the only way either of them says anything.
///
/// A cancelled run returns whatever it had reached, which is not a contour: the
/// caller's generation check is what drops it.
[[nodiscard]] Result<std::vector<analysis::PitchPoint>>
trackPitchInChunks(ConstAudioBufferView audio, SampleRate rate,
                   const analysis::PitchSettings& settings, const CancellationToken& cancellation);

} // namespace sa::ui
