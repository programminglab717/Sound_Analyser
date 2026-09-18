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

/// Streaming reader for MPEG-1, MPEG-2 and MPEG-2.5 audio, layers I to III.
///
/// Wraps dr_mp3, for the same reason FlacReader wraps dr_flac: a layer III
/// decoder is a Huffman-coded hybrid filter bank, and writing one ourselves
/// would buy nothing and cost us a large hostile-input surface.
///
/// **MP3 is lossy, so this is a decoder and not a parser.** What comes back is
/// not what went in, and AudioFileInfo::format reports Float32 because there is
/// no PCM width in the file to round-trip to. Anything that treats an opened
/// source as the user's master needs to know it is looking at a decode.
///
/// **Two costs are worth knowing about before opening a very long MP3.** An MP3
/// carries no index, so the frame count comes either from the Xing/Info header
/// -- free, and present on anything a normal encoder wrote -- or, failing that,
/// from decoding the stream through once at open, which is linear in the length
/// of the file though constant in memory. And a backwards seek costs a rescan
/// from the start of the stream, so sequential reading is dramatically cheaper
/// than jumping about. read() therefore never seeks when it is already
/// positioned where the caller asked for, which is the case every streaming
/// analysis pass hits.
///
/// As with WavReader the file stays open for the reader's lifetime, and one
/// reader must not be read from two threads at once.
class Mp3Reader final : public AudioSource {
public:
    [[nodiscard]] static Result<Mp3Reader> open(const std::filesystem::path& path);

    /// Decode from memory. The bytes must outlive the reader.
    [[nodiscard]] static Result<Mp3Reader> fromMemory(std::span<const std::byte> bytes);

    [[nodiscard]] const AudioFileInfo& info() const noexcept override { return info_; }

    /// Read up to `destination.frames()` frames starting at `startFrame`.
    ///
    /// Returns the number of frames actually read, which is short at end of
    /// file and where a file turns out to be corrupt part way through.
    /// Channels beyond the destination's count are skipped; channels the file
    /// does not have are left untouched.
    [[nodiscard]] Result<SampleCount> read(SampleIndex startFrame,
                                           AudioBufferView destination) const override;

    /// Decode the whole file into a new buffer. Convenient for short files and
    /// tests; prefer read() for anything long.
    [[nodiscard]] Result<AudioBuffer> readAll() const;

private:
    Mp3Reader() = default;

    /// Owns the dr_mp3 decoder and the cursor its callbacks read through.
    /// Opaque here so that dr_mp3.h stays private to this module.
    struct Decoder;

    [[nodiscard]] static Result<Mp3Reader> decode(std::shared_ptr<const ByteSource> source);

    std::shared_ptr<Decoder> decoder_;
    AudioFileInfo info_;
};

} // namespace sa::io
