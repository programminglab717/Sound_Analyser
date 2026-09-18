#pragma once

#include <sa/core/AudioBuffer.h>
#include <sa/core/Result.h>
#include <sa/core/Types.h>
#include <sa/engine/Timeline.h>
#include <sa/io/AudioSource.h>

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace sa::engine {

/// A source registered with a document.
struct SourceEntry {
    SourceId id = SourceId::Invalid;
    std::shared_ptr<const io::AudioSource> audio;
    std::string name;

    /// Where the audio came from, so a session can be saved and reopened.
    /// Empty for sources with no file behind them -- generated tone, recorded
    /// material not yet written to disk, a test fixture.
    std::filesystem::path path;
};

/// Reusable scratch for rendering.
///
/// Rendering reads clip audio into a temporary buffer before mixing. Passing
/// the scratch in lets a render loop allocate once instead of once per block,
/// which matters when scrubbing redraws continuously.
class RenderContext {
public:
    void prepare(ChannelLayout layout, SampleCount frames);
    [[nodiscard]] AudioBufferView scratch(SampleCount frames) noexcept;

private:
    AudioBuffer buffer_;
    std::vector<std::size_t> overlapping_;
    friend class Document;
};

/// The edited document: sources, a timeline of clips, and markers.
///
/// Non-destructive by construction (ADR 0003). An edit appends to or rewrites
/// the clip list; it never touches audio. Undo is therefore a cheap structural
/// operation and a saved session reopens with every decision still adjustable.
///
/// Destructive editing still exists for users who expect it, as `flatten`,
/// implemented on top of this rather than instead of it.
class Document {
public:
    Document() = default;
    explicit Document(SampleRate sampleRate, ChannelLayout layout);

    [[nodiscard]] SampleRate sampleRate() const noexcept { return sampleRate_; }

    [[nodiscard]] const ChannelLayout& layout() const noexcept { return layout_; }

    [[nodiscard]] const Timeline& timeline() const noexcept { return timeline_; }

    [[nodiscard]] Timeline& timeline() noexcept { return timeline_; }

    [[nodiscard]] const std::vector<Marker>& markers() const noexcept { return markers_; }

    [[nodiscard]] std::vector<Marker>& markers() noexcept { return markers_; }

    /// Register a source. The document holds a reference; the audio is shared,
    /// never copied, so the same file backing ten clips costs one decode.
    [[nodiscard]] Result<SourceId> addSource(std::shared_ptr<const io::AudioSource> audio,
                                             std::string name = {},
                                             std::filesystem::path path = {});

    [[nodiscard]] const SourceEntry* source(SourceId id) const noexcept;

    [[nodiscard]] std::size_t sourceCount() const noexcept { return sources_.size(); }

    /// Every registered source, in registration order. Needed to serialise a
    /// session, which must record each source's path and shape.
    [[nodiscard]] const std::vector<SourceEntry>& sources() const noexcept { return sources_; }

    /// Place a whole source on the timeline at `position`.
    [[nodiscard]] Result<ClipId> appendSource(SourceId source, SampleIndex position);

    /// Total length of the document in samples.
    [[nodiscard]] SampleCount duration() const noexcept { return timeline_.duration(); }

    /// Render [start, start + destination.frames()) by mixing every overlapping
    /// clip with its gain and fades applied.
    ///
    /// Regions with no clip render as silence rather than as an error, so a view
    /// scrolled past the end still draws.
    [[nodiscard]] Status render(SampleIndex start, AudioBufferView destination,
                                RenderContext& context) const;

    /// Convenience overload that allocates its own scratch. Prefer the
    /// context-taking form in a loop.
    [[nodiscard]] Status render(SampleIndex start, AudioBufferView destination) const;

    /// Next id the document will hand out. Exposed so a restored snapshot does
    /// not reissue an id that a stale reference still names.
    [[nodiscard]] std::uint64_t nextId() const noexcept { return nextId_; }

    void setNextId(std::uint64_t value) noexcept { nextId_ = value; }

private:
    SampleRate sampleRate_{48000.0};
    ChannelLayout layout_ = ChannelLayout::stereo();
    Timeline timeline_;
    std::vector<SourceEntry> sources_;
    std::vector<Marker> markers_;
    std::uint64_t nextId_ = 1;

    friend class UndoHistory;
};

} // namespace sa::engine
