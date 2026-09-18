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

/// Streaming reader for RIFF/WAVE and RF64 files.
///
/// Written in-house rather than taken from a library because the metadata that
/// matters in post-production -- `bext` timecode, cue markers, LIST/INFO -- is
/// exactly what the convenient single-header decoders drop, and because an
/// audio file parser is a hostile-input surface we would rather own and fuzz
/// than inherit.
///
/// Reading is random-access and streamed: opening a file parses only its chunk
/// headers, and read() touches just the bytes it needs.
///
/// **Every size in a WAV file is attacker-controlled.** This parser trusts none
/// of them: chunk extents are validated against the real source length before
/// use, and a zero-length chunk cannot stall the scan.
class WavReader final : public AudioSource {
public:
    [[nodiscard]] static Result<WavReader> open(const std::filesystem::path& path);

    /// Parse from memory. The bytes must outlive the reader.
    [[nodiscard]] static Result<WavReader> fromMemory(std::span<const std::byte> bytes);

    [[nodiscard]] const AudioFileInfo& info() const noexcept override { return info_; }

    [[nodiscard]] const AudioFileMetadata& metadata() const noexcept { return metadata_; }

    /// Read up to `destination.frames()` frames starting at `startFrame`,
    /// converting to float32 in [-1, 1].
    ///
    /// Returns the number of frames actually read, which is short at end of
    /// file. Channels beyond the destination's count are skipped; channels the
    /// file does not have are left untouched.
    [[nodiscard]] Result<SampleCount> read(SampleIndex startFrame,
                                           AudioBufferView destination) const override;

    /// Read the whole file into a new buffer. Convenient for short files and
    /// tests; prefer read() for anything long.
    [[nodiscard]] Result<AudioBuffer> readAll() const;

private:
    WavReader() = default;

    [[nodiscard]] static Result<WavReader> parse(std::shared_ptr<const ByteSource> source);

    std::shared_ptr<const ByteSource> source_;
    AudioFileInfo info_;
    AudioFileMetadata metadata_;
    std::uint64_t dataOffset_ = 0;
    std::uint32_t bytesPerFrame_ = 0;
};

} // namespace sa::io
