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
///
/// **It holds its own copy of the document, and that is the point.** The
/// readers above run on background threads -- a spectrogram build, the player's
/// render worker -- while the user goes on editing, and a document is not
/// thread safe. Pointing at the live one made every edit a race against every
/// reader, which is the kind of fault that appears once in a hundred runs on a
/// loaded machine and never in a test. A copy costs a clip list and a source
/// table; the audio itself is shared, because sources are immutable once added.
/// The consequence to know is that a source does not track later edits: the
/// caller makes a new one, which is what an edit does anyway.
class DocumentSource final : public io::AudioSource {
public:
    explicit DocumentSource(const Document& document);

    [[nodiscard]] const io::AudioFileInfo& info() const noexcept override { return info_; }

    [[nodiscard]] Result<SampleCount> read(SampleIndex startFrame,
                                           AudioBufferView destination) const override;

private:
    Document document_;
    io::AudioFileInfo info_;
    mutable std::mutex mutex_;
    mutable RenderContext context_;
};

} // namespace sa::engine
