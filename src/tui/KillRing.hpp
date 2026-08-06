#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace cch::tui::detail {

/// Ring buffer for Emacs-style kill/yank operations (pi `kill-ring.ts` at
/// baseline 83114817).
///
/// Tracks killed text entries. Consecutive kills accumulate into a single
/// entry (prepending for backward deletions, appending for forward ones).
/// Supports yank (paste most recent) and yank-pop (cycle through older
/// entries). Package-private, matching pi's `index.ts` re-export boundary;
/// the single-line Input composes it (the Editor keeps its document-model
/// kill machinery for multiline paste-marker editing).
class KillRing {
public:
    /// Add text to the kill ring. Empty text is ignored, as in pi.
    ///
    /// @param text       The killed text to add
    /// @param prepend    When accumulating, prepend (backward) or append (forward)
    /// @param accumulate Merge with the most recent entry instead of creating a new one
    void push(std::string text, bool prepend, bool accumulate) {
        if (text.empty()) return;
        if (accumulate && !ring_.empty()) {
            auto last = std::move(ring_.back());
            ring_.pop_back();
            ring_.push_back(prepend ? text + last : last + text);
        } else {
            ring_.push_back(std::move(text));
        }
    }

    /// The most recent entry, or nullptr when the ring is empty.
    [[nodiscard]] const std::string* peek() const {
        return ring_.empty() ? nullptr : &ring_.back();
    }

    /// Move the last entry to the front (for yank-pop cycling).
    void rotate() {
        if (ring_.size() > 1) {
            auto last = std::move(ring_.back());
            ring_.pop_back();
            ring_.insert(ring_.begin(), std::move(last));
        }
    }

    [[nodiscard]] std::size_t length() const {
        return ring_.size();
    }

private:
    std::vector<std::string> ring_;
};

} // namespace cch::tui::detail
