#pragma once

#include <sa/engine/Document.h>
#include <sa/io/AudioSource.h>

#include <mutex>

namespace sa::engine {

/// Reads a document's rendered timeline through the `AudioSource` interface.
///
/// Every analysis path in the project -- the peak pyramid, the spectrogram, the
/// loudness meter, the writers -- already takes an `AudioSource`, because they
/// were written against a file. After the first edit the file is no longer what
/// the user is looking at. This adapter closes that gap without any of them
/// learning what a document is: the document becomes just another source.
///
/// The rendering scratch is reused across reads and guarded by a mutex, so a
/// background analysis pass and a redraw can share one adapter. That mutex is
/// why this must not be read from the audio thread; playback renders from the
/// document directly, with its own context.
class DocumentSource final : public io::AudioSource {
public:
    explicit DocumentSource(const Document& document);

    [[nodiscard]] const io::AudioFileInfo& info() const noexcept override { return info_; }

    [[nodiscard]] Result<SampleCount> read(SampleIndex startFrame,
                                           AudioBufferView destination) const override;

private:
    const Document* document_;
    io::AudioFileInfo info_;
    mutable std::mutex mutex_;
    mutable RenderContext context_;
};

} // namespace sa::engine
