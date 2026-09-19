#pragma once

#include <sa/core/AudioBuffer.h>
#include <sa/core/Result.h>
#include <sa/io/AudioFileInfo.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <ostream>

namespace sa::io {

/// Streaming AIFF writer, the counterpart to AiffReader.
///
/// Same shape as WavWriter, deliberately: the header goes out with placeholder
/// sizes, audio streams through as it arrives, and finish() seeks back and
/// patches them. A two-hour export costs one block of memory, and the caller
/// owns the stream so a test can write to an ostringstream and read the exact
/// bytes back.
///
/// What comes out is plain uncompressed big-endian AIFF -- FORM/AIFF with a
/// COMM and an SSND chunk -- because that is the form every tool that claims to
/// read AIFF actually reads. Three deliberate omissions:
///
///  * **No AIFF-C.** `sowt` (little-endian PCM) exists to save a byte swap on a
///    little-endian machine, which is not a reason to write a file half the
///    world's AIFF readers treat as a compressed type they do not know.
///  * **No float.** AIFF's float encodings live in AIFF-C, and writing them
///    would mean picking a byte order for `fl32` -- a choice on which real
///    files in the wild disagree. A float export has a container that carries
///    it without ambiguity, which is WAV; AIFF here is integer PCM.
///  * **No 8-bit.** AiffReader rejects 8-bit AIFF, so writing it would produce
///    a file this program could not open.
///
/// Both chunks that can end on an odd byte -- SSND's audio, and any text
/// chunk -- get the IFF pad byte, and the pad is counted in the FORM size but
/// not in the chunk's own size, which is what the format asks for and what
/// AiffReader's scan expects.
struct AiffOptions {
    /// PcmInt16, PcmInt24 and PcmInt32 only. See the class comment.
    SampleFormat format = SampleFormat::PcmInt24;
    AudioFileMetadata metadata;
};

class AiffWriter {
public:
    /// `stream` must stay open and seekable until finish() returns.
    [[nodiscard]] static Result<AiffWriter> create(std::ostream& stream, SampleRate sampleRate,
                                                   ChannelLayout layout, AiffOptions options = {});

    /// Append frames. May be called repeatedly.
    [[nodiscard]] Status write(ConstAudioBufferView frames);

    /// Patch the chunk sizes and the COMM frame count. Must be called, or the
    /// file claims no frames at all.
    [[nodiscard]] Status finish();

    [[nodiscard]] SampleCount framesWritten() const noexcept { return framesWritten_; }

    /// Convenience for short files: write a whole buffer to a path.
    [[nodiscard]] static Status writeFile(const std::filesystem::path& path,
                                          ConstAudioBufferView frames, SampleRate sampleRate,
                                          ChannelLayout layout, AiffOptions options = {});

private:
    AiffWriter(std::ostream& stream, SampleRate sampleRate, ChannelLayout layout,
               AiffOptions options);

    std::ostream* stream_ = nullptr;
    SampleRate sampleRate_;
    ChannelLayout layout_;
    AiffOptions options_;
    SampleCount framesWritten_ = 0;
    std::streampos formSizePosition_ = 0;
    std::streampos frameCountPosition_ = 0;
    std::streampos soundSizePosition_ = 0;
    std::uint32_t bytesPerFrame_ = 0;
    bool finished_ = false;
};

} // namespace sa::io
