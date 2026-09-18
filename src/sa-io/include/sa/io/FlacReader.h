#pragma once

#include <sa/core/AudioBuffer.h>
#include <sa/core/Result.h>
#include <sa/io/AudioFileInfo.h>
#include <sa/io/AudioSource.h>
#include <sa/io/ByteSource.h>

#include <filesystem>
#include <memory>
#include <span>

namespace sa::io {

/// Streaming reader for native FLAC.
///
/// Unlike the WAV and AIFF parsers this one is not ours. Those are hand-written
/// because the metadata that matters in post-production is exactly what the
/// convenient decoders drop; no such argument applies here. A FLAC frame is
/// Rice-coded residuals over an LPC predictor, and a hand-rolled entropy
/// decoder would be a far worse hostile-input surface than one the whole
/// industry has been fuzzing for a decade. So this wraps dr_flac, and the value
/// it adds is the part that is ours to get right: bounds-checked byte access,
/// errors returned rather than signalled by a null pointer, and reads that
/// stream.
///
/// Ogg-encapsulated FLAC is deliberately not built (DR_FLAC_NO_OGG). The
/// sniffer in AudioFile.cpp only ever routes native `fLaC` streams here, so the
/// Ogg path would be unreachable code carrying an attack surface.
///
/// Reading is random-access and streamed: opening parses the metadata blocks
/// only, and read() decodes just the frames it needs. As with WavReader **the
/// file stays open for the reader's lifetime**, so on Windows an open reader
/// blocks deletion and renaming; anything that needs to release a file must
/// destroy its reader.
///
/// One reader must not be read from two threads at once: a read moves the
/// decoder's position. Neither must a WavReader, for the same reason.
class FlacReader final : public AudioSource {
public:
    [[nodiscard]] static Result<FlacReader> open(const std::filesystem::path& path);

    /// Decode from memory. The bytes must outlive the reader.
    [[nodiscard]] static Result<FlacReader> fromMemory(std::span<const std::byte> bytes);

    [[nodiscard]] const AudioFileInfo& info() const noexcept override { return info_; }

    /// Read up to `destination.frames()` frames starting at `startFrame`,
    /// converting to float32 in [-1, 1].
    ///
    /// Returns the number of frames actually read, which is short at end of
    /// file and also where a file turns out to be corrupt part way through --
    /// half a recording is more use than none. Channels beyond the
    /// destination's count are skipped; channels the file does not have are
    /// left untouched.
    [[nodiscard]] Result<SampleCount> read(SampleIndex startFrame,
                                           AudioBufferView destination) const override;

    /// Decode the whole file into a new buffer. Convenient for short files and
    /// tests; prefer read() for anything long.
    [[nodiscard]] Result<AudioBuffer> readAll() const;

private:
    FlacReader() = default;

    /// Owns the dr_flac handle and the cursor its callbacks read through.
    /// Opaque here so that dr_flac.h stays private to this module.
    struct Decoder;

    [[nodiscard]] static Result<FlacReader> decode(std::shared_ptr<const ByteSource> source);

    std::shared_ptr<Decoder> decoder_;
    AudioFileInfo info_;
};

} // namespace sa::io
