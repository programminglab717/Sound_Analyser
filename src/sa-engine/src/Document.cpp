#include <sa/engine/Document.h>

#include <algorithm>

namespace sa::engine {

void RenderContext::prepare(ChannelLayout layout, SampleCount frames) {
    if (buffer_.channelCount() != layout.count() || buffer_.frames() < frames) {
        buffer_.resize(layout, frames);
    }
}

AudioBufferView RenderContext::scratch(SampleCount frames) noexcept {
    return buffer_.view().subRange(0, frames);
}

Document::Document(SampleRate sampleRate, ChannelLayout layout)
    : sampleRate_(sampleRate), layout_(layout) {}

Result<SourceId> Document::addSource(std::shared_ptr<const io::AudioSource> audio, std::string name,
                                     std::filesystem::path path) {
    if (audio == nullptr) {
        return Error{ErrorCode::InvalidArgument, "source audio is null"};
    }
    SourceEntry entry;
    entry.id = static_cast<SourceId>(nextId_++);
    entry.audio = std::move(audio);
    entry.name = std::move(name);
    entry.path = std::move(path);
    const SourceId id = entry.id;
    sources_.push_back(std::move(entry));
    return id;
}

const SourceEntry* Document::source(SourceId id) const noexcept {
    const auto it = std::find_if(sources_.begin(), sources_.end(),
                                 [id](const SourceEntry& entry) { return entry.id == id; });
    return it == sources_.end() ? nullptr : &*it;
}

bool Document::setSourcePath(SourceId id, std::filesystem::path path) {
    for (SourceEntry& entry : sources_) {
        if (entry.id == id) {
            entry.path = std::move(path);
            return true;
        }
    }
    return false;
}

Result<ClipId> Document::appendSource(SourceId source, SampleIndex position) {
    const SourceEntry* entry = this->source(source);
    if (entry == nullptr) {
        return Error{ErrorCode::NotFound, "no such source"};
    }
    if (position < 0) {
        return Error{ErrorCode::OutOfRange, "position is negative"};
    }

    Clip clip;
    clip.id = static_cast<ClipId>(nextId_++);
    clip.source = source;
    clip.sourceStart = 0;
    clip.length = entry->audio->info().frameCount;
    clip.timelineStart = position;
    clip.name = entry->name;

    timeline_.insert(clip);
    return clip.id;
}

Status Document::render(SampleIndex start, AudioBufferView destination,
                        RenderContext& context) const {
    if (destination.isEmpty()) {
        return Status{};
    }

    // Silence first: a range with no clips is a legitimate, common state, so it
    // renders as silence rather than as an error.
    for (int channel = 0; channel < destination.channelCount(); ++channel) {
        std::fill_n(destination.channel(channel), destination.frames(), 0.0f);
    }

    const SampleIndex end = start + destination.frames();
    timeline_.collectOverlapping(start, end, context.overlapping_);
    if (context.overlapping_.empty()) {
        return Status{};
    }

    context.prepare(layout_, destination.frames());
    const std::vector<Clip>& clips = timeline_.clips();

    for (std::size_t index : context.overlapping_) {
        const Clip& clip = clips[index];
        const SourceEntry* entry = source(clip.source);
        if (entry == nullptr) {
            continue; // a clip whose source went away renders as silence
        }

        // Intersect the clip with the requested range, in timeline coordinates.
        const SampleIndex regionStart = std::max(start, clip.timelineStart);
        const SampleIndex regionEnd = std::min(end, clip.timelineEnd());
        if (regionEnd <= regionStart) {
            continue;
        }
        const SampleCount regionFrames = regionEnd - regionStart;

        const SampleIndex offsetInClip = regionStart - clip.timelineStart;
        const SampleIndex sourcePosition = clip.sourceStart + offsetInClip;

        AudioBufferView scratch = context.scratch(regionFrames);
        for (int channel = 0; channel < scratch.channelCount(); ++channel) {
            std::fill_n(scratch.channel(channel), regionFrames, 0.0f);
        }

        auto read = entry->audio->read(sourcePosition, scratch);
        if (!read) {
            return read.error();
        }
        const SampleCount got = read.value();
        if (got <= 0) {
            continue;
        }

        const SampleIndex destinationOffset = regionStart - start;
        const int channels = std::min(destination.channelCount(), scratch.channelCount());

        for (int channel = 0; channel < channels; ++channel) {
            const float* in = scratch.channel(channel);
            float* out = destination.channel(channel) + destinationOffset;
            for (SampleCount i = 0; i < got; ++i) {
                // Mixing is addition: overlapping clips sum, which is what makes
                // a crossfade work without a special case.
                out[i] += in[i] * clip.gainAt(offsetInClip + i);
            }
        }
    }

    return Status{};
}

Status Document::render(SampleIndex start, AudioBufferView destination) const {
    RenderContext context;
    return render(start, destination, context);
}

} // namespace sa::engine
