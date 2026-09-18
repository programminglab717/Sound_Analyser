#pragma once

#include <sa/core/AudioBuffer.h>
#include <sa/core/Result.h>
#include <sa/io/AudioFileInfo.h>

namespace sa::io {

/// Read-only random access to decoded audio.
///
/// The interface every decoder implements, so analysis code works against a
/// file without knowing or caring which container it came from. Adding FLAC or
/// MP3 later means implementing this, with no change to callers.
///
/// Implementations stream: a read touches only the bytes it needs, and a source
/// larger than memory is expected rather than exceptional.
class AudioSource {
public:
    virtual ~AudioSource() = default;

    [[nodiscard]] virtual const AudioFileInfo& info() const noexcept = 0;

    /// Read up to `destination.frames()` frames starting at `startFrame`,
    /// returning how many were actually read. A short read means end of source.
    [[nodiscard]] virtual Result<SampleCount> read(SampleIndex startFrame,
                                                   AudioBufferView destination) const = 0;
};

} // namespace sa::io
