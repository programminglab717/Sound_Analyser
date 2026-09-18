#pragma once

#include <sa/core/AudioBuffer.h>
#include <sa/core/Result.h>
#include <sa/io/AudioFileInfo.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <ostream>

namespace sa::io {

/// Streaming RIFF/WAVE writer.
///
/// Writes the header with placeholder sizes, streams audio as it arrives, then
/// seeks back and patches the sizes in finish(). That keeps memory flat when
/// exporting a two-hour file, at the cost of requiring a seekable sink.
///
/// The caller owns the stream, which lets tests write to an ostringstream and
/// exercise the exact bytes without touching the filesystem.
/// Output settings.
///
/// At namespace scope rather than nested in WavWriter: a nested type's default
/// member initialisers are not usable in a default argument of the enclosing
/// class, so `Options options = {}` would not compile.
struct WavOptions {
    SampleFormat format = SampleFormat::PcmInt24;
    AudioFileMetadata metadata;
};

class WavWriter {
public:
    /// `stream` must stay open and seekable until finish() returns.
    [[nodiscard]] static Result<WavWriter> create(std::ostream& stream, SampleRate sampleRate,
                                                  ChannelLayout layout, WavOptions options = {});

    /// Append frames. May be called repeatedly.
    [[nodiscard]] Status write(ConstAudioBufferView frames);

    /// Patch the header sizes. Must be called, or the file is unreadable.
    [[nodiscard]] Status finish();

    [[nodiscard]] SampleCount framesWritten() const noexcept { return framesWritten_; }

    /// Convenience for short files: write a whole buffer to a path.
    [[nodiscard]] static Status writeFile(const std::filesystem::path& path,
                                          ConstAudioBufferView frames, SampleRate sampleRate,
                                          ChannelLayout layout, WavOptions options = {});

private:
    WavWriter(std::ostream& stream, SampleRate sampleRate, ChannelLayout layout,
              WavOptions options);

    std::ostream* stream_ = nullptr;
    SampleRate sampleRate_;
    ChannelLayout layout_;
    WavOptions options_;
    SampleCount framesWritten_ = 0;
    std::streampos riffSizePosition_ = 0;
    std::streampos dataSizePosition_ = 0;
    std::uint32_t bytesPerFrame_ = 0;
    bool finished_ = false;
};

} // namespace sa::io
