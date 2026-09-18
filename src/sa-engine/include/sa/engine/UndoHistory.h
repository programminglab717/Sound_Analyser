#pragma once

#include <sa/engine/Document.h>

#include <cstddef>
#include <string>
#include <vector>

namespace sa::engine {

/// A restorable snapshot of everything an edit can change.
struct DocumentState {
    Timeline timeline;
    std::vector<Marker> markers;
    std::vector<SourceEntry> sources;
    std::uint64_t nextId = 1;
};

/// Undo and redo over document structure.
///
/// Snapshot-based, not inverse-command-based. That is affordable precisely
/// because the document is non-destructive (ADR 0003): a snapshot copies clip
/// metadata and shared source handles, never audio, so a timeline of ten
/// thousand clips snapshots in kilobytes. Inverse commands would be smaller
/// still, but every one is a chance to write an `undo` that does not exactly
/// reverse its `do`, and that class of bug silently destroys a user's work.
///
/// The history is linear with a cursor. Committing after an undo discards the
/// redo branch, which is what every editor does and what users expect.
class UndoHistory {
public:
    static constexpr std::size_t kDefaultDepth = 128;

    explicit UndoHistory(const Document& document, std::size_t maxDepth = kDefaultDepth);

    /// Record the document's state after an edit.
    void commit(const Document& document, std::string label);

    /// Record, but replace the previous entry if it carries the same label.
    ///
    /// For continuous gestures -- dragging a fade handle, scrubbing a gain --
    /// where one entry per mouse move would bury every earlier edit under
    /// hundreds of steps.
    void commitCoalescing(const Document& document, std::string label);

    [[nodiscard]] bool canUndo() const noexcept { return cursor_ > 0; }

    [[nodiscard]] bool canRedo() const noexcept { return cursor_ + 1 < states_.size(); }

    bool undo(Document& document);
    bool redo(Document& document);

    /// Label of the edit that undo would reverse, or empty.
    [[nodiscard]] std::string_view undoLabel() const noexcept;
    /// Label of the edit that redo would reapply, or empty.
    [[nodiscard]] std::string_view redoLabel() const noexcept;

    /// Every entry's label, oldest first. Backs a visual history timeline.
    [[nodiscard]] const std::vector<std::string>& labels() const noexcept { return labels_; }

    /// Index of the current state within labels().
    [[nodiscard]] std::size_t position() const noexcept { return cursor_; }

    [[nodiscard]] std::size_t depth() const noexcept { return states_.size(); }

    /// Jump straight to an entry, for clicking a point in the history view.
    bool restore(std::size_t index, Document& document);

private:
    static DocumentState capture(const Document& document);
    static void apply(const DocumentState& state, Document& document);
    void trimToDepth();

    std::vector<DocumentState> states_;
    std::vector<std::string> labels_;
    std::size_t cursor_ = 0;
    std::size_t maxDepth_ = kDefaultDepth;
};

} // namespace sa::engine
