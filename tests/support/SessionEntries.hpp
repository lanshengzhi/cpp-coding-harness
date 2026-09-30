#pragma once

#include <cch/agent/harness/session/SessionEntry.hpp>

#include <vector>

namespace cch::tests {

/// The last entry of `kind` in append order — the shape resume restores from
/// a leaf path (a new-session initial entry precedes any real change).
[[nodiscard]] inline const harness::session::SessionEntry* find_last_entry_of_kind(
    const std::vector<harness::session::SessionEntry>& entries,
    harness::session::SessionEntryKind kind) {
    const harness::session::SessionEntry* found = nullptr;
    for (const auto& entry : entries) {
        if (entry.kind == kind) {
            found = &entry;
        }
    }
    return found;
}

[[nodiscard]] inline const harness::session::SessionEntry* find_last_model_change(
    const std::vector<harness::session::SessionEntry>& entries) {
    return find_last_entry_of_kind(entries, harness::session::SessionEntryKind::ModelChange);
}

[[nodiscard]] inline const harness::session::SessionEntry* find_last_thinking_level_change(
    const std::vector<harness::session::SessionEntry>& entries) {
    return find_last_entry_of_kind(entries, harness::session::SessionEntryKind::ThinkingLevelChange);
}

} // namespace cch::tests
