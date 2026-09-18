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

/// Streaming reader for AIFF and AIFF-C.
///
/// The second container, and the one that proves the AudioSource abstraction
/// earns its keep: AIFF is big-endian where WAV is little-endian, stores its
/// sample rate as an 80-bit extended float, and puts the frame count in the
/// header rather than implying it from the data chunk length. None of that
/// reaches callers.
///
/// AIFF-C compression types are handled only where they are really just PCM:
/// `NONE` (big-endian), `sowt` (little-endian, what macOS writes), and the
/// `fl32`/`FL32`/`fl64`/`FL64` float encodings. Genuinely compressed variants
/// are rejected rather than decoded as noise.
///
/// As with the WAV parser, every size in the file is attacker-controlled and
/// none is trusted.
class AiffReader final : public AudioSource {
public:
    [[nodiscard]] static Result<AiffReader> open(const std::filesystem::path& path);

    /// Parse from memory. The bytes must outlive the reader.
    [[nodiscard]] static Result<AiffReader> fromMemory(std::span<const std::byte> bytes);

    [[nodiscard]] const AudioFileInfo& info() const noexcept override { return info_; }

    [[nodiscard]] const AudioFileMetadata& metadata() const noexcept { return metadata_; }

    [[nodiscard]] Result<SampleCount> read(SampleIndex startFrame,
                                           AudioBufferView destination) const override;

    [[nodiscard]] Result<AudioBuffer> readAll() const;

private:
    AiffReader() = default;

    [[nodiscard]] static Result<AiffReader> parse(std::shared_ptr<const ByteSource> source);

    std::shared_ptr<const ByteSource> source_;
    AudioFileInfo info_;
    AudioFileMetadata metadata_;
    std::uint64_t dataOffset_ = 0;
    std::uint32_t bytesPerFrame_ = 0;
    /// AIFF is big-endian by default, but `sowt` -- the type macOS actually
    /// writes -- is byte-swapped PCM in an otherwise big-endian container.
    bool littleEndianSamples_ = false;
};

} // namespace sa::io
