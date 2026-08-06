#pragma once

#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

namespace cch::tui::detail {

/// Generic undo stack with clone-on-push semantics (pi `undo-stack.ts` at
/// baseline 83114817).
///
/// Stores copies of state snapshots; popped snapshots are returned directly
/// (no re-cloning) since they are already detached. Package-private,
/// matching pi's `index.ts` re-export boundary; the single-line Input
/// composes it (the Editor keeps its document-model undo machinery for
/// multiline paste-marker editing).
template <typename Snapshot>
class UndoStack {
public:
    /// Push a copy of the given state onto the stack.
    void push(const Snapshot& state) {
        stack_.push_back(state);
    }

    /// Pop and return the most recent snapshot, or nullopt when empty.
    [[nodiscard]] std::optional<Snapshot> pop() {
        if (stack_.empty()) return std::nullopt;
        auto snapshot = std::move(stack_.back());
        stack_.pop_back();
        return snapshot;
    }

    /// Remove all snapshots.
    void clear() {
        stack_.clear();
    }

    [[nodiscard]] std::size_t length() const {
        return stack_.size();
    }

private:
    std::vector<Snapshot> stack_;
};

} // namespace cch::tui::detail
