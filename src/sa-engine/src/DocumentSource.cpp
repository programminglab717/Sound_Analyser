#include <sa/engine/DocumentSource.h>

#include <algorithm>

namespace sa::engine {

DocumentSource::DocumentSource(const Document& document) : document_(&document) {
    info_.sampleRate = document.sampleRate();
    info_.layout = document.layout();
    info_.frameCount = document.duration();
    // A document is float internally and has no file encoding of its own. An
    // export chooses its own format; reporting Float32 here says "no
    // quantisation has been applied yet", which is true.
    info_.format = io::SampleFormat::Float32;
}

Result<SampleCount> DocumentSource::read(SampleIndex startFrame,
                                         AudioBufferView destination) const {
    if (startFrame < 0) {
        return Error{ErrorCode::InvalidArgument, "start frame is negative"};
    }
    if (destination.channelCount() != info_.layout.count()) {
        return Error{ErrorCode::InvalidArgument,
                     "destination channel count does not match the document"};
    }

    const SampleCount total = document_->duration();
    if (startFrame >= total) {
        return SampleCount{0};
    }

    const SampleCount available = std::min<SampleCount>(destination.frames(), total - startFrame);

    const std::lock_guard<std::mutex> lock{mutex_};
    // Render only the frames that exist. Document::render treats a gap as
    // silence rather than an error, so rendering past the end would happily
    // return zeros and make every caller think the document is longer than it
    // is; the short read is the honest answer and the interface's contract.
    AudioBufferView window = destination.subRange(0, available);
    if (auto status = document_->render(startFrame, window, context_); !status) {
        return status.error();
    }
    return available;
}

} // namespace sa::engine
