#include <sa/engine/UndoHistory.h>

#include <utility>

namespace sa::engine {

UndoHistory::UndoHistory(const Document& document, std::size_t maxDepth)
    : maxDepth_(maxDepth < 2 ? 2 : maxDepth) {
    states_.push_back(capture(document));
    labels_.emplace_back("Open");
    cursor_ = 0;
}

DocumentState UndoHistory::capture(const Document& document) {
    DocumentState state;
    state.timeline = document.timeline_;
    state.markers = document.markers_;
    state.sources = document.sources_;
    state.nextId = document.nextId_;
    return state;
}

void UndoHistory::apply(const DocumentState& state, Document& document) {
    document.timeline_ = state.timeline;
    document.markers_ = state.markers;
    document.sources_ = state.sources;
    // Restoring the id counter matters: reissuing an id that a stale reference
    // still names would silently retarget that reference at a different clip.
    document.nextId_ = state.nextId;
}

void UndoHistory::commit(const Document& document, std::string label) {
    // Anything ahead of the cursor is a redo branch the user has now abandoned.
    states_.resize(cursor_ + 1);
    labels_.resize(cursor_ + 1);

    states_.push_back(capture(document));
    labels_.push_back(std::move(label));
    cursor_ = states_.size() - 1;
    trimToDepth();
}

void UndoHistory::commitCoalescing(const Document& document, std::string label) {
    // Only coalesce onto the newest entry: merging into the middle of a history
    // would rewrite a step the user has already navigated past.
    const bool canMerge = cursor_ > 0 && cursor_ + 1 == states_.size() && labels_[cursor_] == label;
    if (canMerge) {
        states_[cursor_] = capture(document);
        return;
    }
    commit(document, std::move(label));
}

void UndoHistory::trimToDepth() {
    while (states_.size() > maxDepth_) {
        states_.erase(states_.begin());
        labels_.erase(labels_.begin());
        if (cursor_ > 0) {
            --cursor_;
        }
    }
}

bool UndoHistory::undo(Document& document) {
    if (!canUndo()) {
        return false;
    }
    --cursor_;
    apply(states_[cursor_], document);
    return true;
}

bool UndoHistory::redo(Document& document) {
    if (!canRedo()) {
        return false;
    }
    ++cursor_;
    apply(states_[cursor_], document);
    return true;
}

bool UndoHistory::restore(std::size_t index, Document& document) {
    if (index >= states_.size()) {
        return false;
    }
    cursor_ = index;
    apply(states_[cursor_], document);
    return true;
}

std::string_view UndoHistory::undoLabel() const noexcept {
    if (!canUndo()) {
        return {};
    }
    return labels_[cursor_];
}

std::string_view UndoHistory::redoLabel() const noexcept {
    if (!canRedo()) {
        return {};
    }
    return labels_[cursor_ + 1];
}

} // namespace sa::engine
