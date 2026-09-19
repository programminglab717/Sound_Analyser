#pragma once

#include <sa/core/AudioBuffer.h>
#include <sa/core/Result.h>
#include <sa/io/AudioFileInfo.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <ostream>

namespace sa::io {

/// Output settings for FlacWriter.
///
/// At namespace scope for the same reason WavOptions is: a nested type's
/// default member initialisers cannot be used in a default argument of the
/// enclosing class.
struct FlacOptions {
    /// PcmInt16 or PcmInt24. FLAC stores integers, so a float document has to
    /// be quantised on the way in, and the caller is the one that knows whether
    /// that quantisation wants dither -- so the choice of depth is made here
    /// and the dither is applied before the samples arrive.
    SampleFormat format = SampleFormat::PcmInt24;
    AudioFileMetadata metadata;
    /// Samples per FLAC frame. 4096 is the usual choice and is within the FLAC
    /// subset at every sample rate. Exposed mainly so tests can force many
    /// frames out of a short buffer.
    SampleCount blockFrames = 4096;
};

/// Streaming FLAC encoder.
///
/// **Written here rather than taken from libFLAC**, and the reasoning is worth
/// stating because it is the opposite of the reasoning in FlacReader.
///
/// Decoding is a hostile-input problem: a file arrives from outside, and a
/// hand-rolled entropy decoder is a far worse attack surface than one the
/// industry has fuzzed for a decade. So FlacReader wraps dr_flac. Encoding has
/// no such surface. The input is a float buffer this program produced, the
/// output is bytes we write, and there is nothing an attacker controls except
/// sample values. What is left is a format to get exactly right, which is the
/// same problem the hand-written WAV and AIFF parsers solve, and a
/// correctness check that is stronger than any argument: a FLAC is lossless, so
/// the samples that come back through an independent decoder must be identical
/// to the ones that went in. The tests assert that bit for bit.
///
/// Against that, the cost of the alternative: libFLAC is BSD-3-Clause and would
/// pass the licence gate, but it is a multi-file C library with its own build
/// system, and vendoring it would be the largest third-party surface in the
/// tree by an order of magnitude -- next to two single headers that decode
/// three formats between them.
///
/// What this encoder does **not** do, stated plainly because it shows up as
/// file size rather than as a failure: there is no LPC stage. Subframes are
/// chosen from constant, verbatim and the four fixed polynomial predictors,
/// with wasted-bit detection, stereo decorrelation and a partitioned-Rice
/// search over the residual. That reaches most of what a general-purpose
/// encoder reaches -- the fixed predictors are where the bulk of the
/// redundancy in PCM audio goes -- and gives up the last several percent that
/// a per-block LPC fit would find. The output is a conformant FLAC stream
/// either way; it is simply a slightly larger one.
///
/// Like WavWriter this needs a seekable sink: STREAMINFO carries the stream
/// length, the frame-size extremes and the MD5 of the audio, none of which is
/// known until the last frame has gone out, so finish() seeks back and patches
/// them.
class FlacWriter {
public:
    /// `stream` must stay open and seekable until finish() returns.
    [[nodiscard]] static Result<FlacWriter> create(std::ostream& stream, SampleRate sampleRate,
                                                   ChannelLayout layout, FlacOptions options = {});

    FlacWriter(FlacWriter&&) noexcept;
    FlacWriter& operator=(FlacWriter&&) noexcept;
    FlacWriter(const FlacWriter&) = delete;
    FlacWriter& operator=(const FlacWriter&) = delete;
    ~FlacWriter();

    /// Append frames. May be called repeatedly; frames are held until a whole
    /// FLAC block has arrived.
    [[nodiscard]] Status write(ConstAudioBufferView frames);

    /// Flush the part-filled block, then patch STREAMINFO. Must be called, or
    /// the file is missing its tail and claims a length of zero.
    [[nodiscard]] Status finish();

    [[nodiscard]] SampleCount framesWritten() const noexcept;

    /// Convenience for short files: write a whole buffer to a path.
    [[nodiscard]] static Status writeFile(const std::filesystem::path& path,
                                          ConstAudioBufferView frames, SampleRate sampleRate,
                                          ChannelLayout layout, FlacOptions options = {});

private:
    struct Encoder;

    explicit FlacWriter(std::unique_ptr<Encoder> encoder) noexcept;

    std::unique_ptr<Encoder> encoder_;
};

} // namespace sa::io
