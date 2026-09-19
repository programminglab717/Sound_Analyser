#include <sa/ui/ChunkedPitch.h>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace sa::ui {

SampleCount pitchHopFor(SampleCount frames, SampleCount base) {
    constexpr SampleCount kComfortableFrames = 1 << 21; // About 44 s at 48 kHz.
    SampleCount hop = base;
    while (hop < base * 8 && frames > kComfortableFrames * (hop / base)) {
        hop *= 2;
    }
    return hop;
}

Result<std::vector<analysis::PitchPoint>>
trackPitchInChunks(ConstAudioBufferView audio, SampleRate rate,
                   const analysis::PitchSettings& settings, const CancellationToken& cancellation) {
    // Settings this cannot divide by are handed on whole, so that the refusal
    // comes from the tracker with its own account of what was wrong rather
    // than from arithmetic here.
    if (!(settings.hop > 0) || !(settings.minHz > 0.0) || !rate.isValid()) {
        return analysis::trackPitch(audio, rate, settings);
    }

    // A second of audio: about a tenth of a second of work at the settings
    // this is used with, which is short enough not to be felt and long enough
    // that rebuilding the tracker's transform tables per chunk is noise.
    const SampleCount chunk = std::max<SampleCount>(
        settings.hop, static_cast<SampleCount>(rate.hz()) / settings.hop * settings.hop);
    // What a frame reads past its own start, plus a hop so that the last frame
    // of a chunk is complete. Erring long costs a few frames that are then
    // discarded; erring short would lose them.
    const SampleCount span = settings.window +
                             static_cast<SampleCount>(std::ceil(rate.hz() / settings.minHz)) +
                             settings.hop;

    std::vector<analysis::PitchPoint> contour;
    for (SampleCount base = 0; base < audio.frames(); base += chunk) {
        if (cancellation.isCancelled()) {
            // What it had, which nothing will look at: a cancelled run is a
            // superseded one, and its generation check drops the result.
            return contour;
        }
        const bool last = base + chunk >= audio.frames();
        const SampleCount take =
            last ? audio.frames() - base : std::min(chunk + span, audio.frames() - base);
        auto part = analysis::trackPitch(audio.subRange(base, take), rate, settings);
        if (!part) {
            // Only the first chunk's refusal is the caller's answer. A later
            // one means the tail was shorter than a frame, which is not a
            // failure of anything.
            if (base == 0) {
                return part.error();
            }
            break;
        }
        // The chunk's own times start at zero; they are times in the passage.
        const double offset = static_cast<double>(base) / rate.hz();
        for (analysis::PitchPoint& point : part.value()) {
            point.timeSeconds += offset;
        }
        // Frames past the chunk's own end belong to the next chunk, which will
        // produce them itself.
        const std::size_t keep =
            last ? part.value().size()
                 : std::min(part.value().size(), static_cast<std::size_t>(chunk / settings.hop));
        contour.insert(contour.end(), part.value().begin(),
                       part.value().begin() + static_cast<std::ptrdiff_t>(keep));
    }
    return contour;
}

} // namespace sa::ui
