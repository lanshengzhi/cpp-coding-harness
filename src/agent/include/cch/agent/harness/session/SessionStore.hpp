#pragma once

#include <cch/agent/harness/session/SessionEntry.hpp>
#include <cch/ai/Message.hpp>
#include <cch/support/Error.hpp>
#include <cch/support/JsonValue.hpp>

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cch::harness::session {

class JsonlSessionStore;

/// The Runtime-facing Session Store: a closed concrete facade over the two
/// supported persistence alternatives — a JSONL session file and an
/// in-memory no-persistence store (ADR 0040). The seam is closed to
/// extension and exposes no generic serialization machinery; encoding and
/// journal I/O stay private behind the typed append operations.
///
/// Mutating appends are internally synchronized so the serialized Session
/// Event Commitment channel can execute them on Runtime worker threads while
/// rare session-assembly appends (model change, compaction, …) run on the
/// Session's loop. Entry order across the two paths is the caller's
/// responsibility (the commitment channel drains before a mid-run typed
/// append).
class SessionStore final {
public:
    /// Wrap one opened JSONL store (see JsonlSessionStore::create_new /
    /// open_existing).
    explicit SessionStore(JsonlSessionStore jsonl);
    /// The in-memory alternative: every append is a successful no-op and
    /// path() is empty (in-memory sessions are not resumable, #409).
    [[nodiscard]] static SessionStore in_memory();

    SessionStore(SessionStore&&) noexcept;
    SessionStore& operator=(SessionStore&&) noexcept;
    ~SessionStore();
    SessionStore(const SessionStore&) = delete;
    SessionStore& operator=(const SessionStore&) = delete;

    /// Append one completed Session Entry message (pi `appendEntry`).
    [[nodiscard]] support::ExpectedVoid append(const ai::MessageVariant& message);

    // --- v3 tree entry appends; successful no-ops for in-memory sessions ---

    [[nodiscard]] support::ExpectedVoid append_model_change(
        std::optional<std::string> parent_id,
        std::string provider,
        std::string model_id);
    [[nodiscard]] support::ExpectedVoid append_thinking_level_change(
        std::optional<std::string> parent_id,
        std::string thinking_level);
    [[nodiscard]] support::ExpectedVoid append_label_change(
        std::optional<std::string> parent_id,
        std::string target_id,
        std::optional<std::string> label);
    [[nodiscard]] support::ExpectedVoid append_compaction(
        std::optional<std::string> parent_id,
        std::string summary,
        std::string first_kept_entry_id,
        std::size_t tokens_before,
        std::optional<support::JsonValue> details,
        std::optional<bool> from_hook,
        std::vector<ai::MessageVariant> retained_tail = {},
        std::optional<ai::Usage> usage = std::nullopt);
    [[nodiscard]] support::ExpectedVoid append_session_info(
        std::optional<std::string> parent_id,
        std::string name);
    /// Write a Leaf entry to persist the current active leaf position
    /// (`target_id` nullopt writes the root leaf, pi `setLeafId(null)`).
    [[nodiscard]] support::ExpectedVoid append_leaf(
        std::optional<std::string> parent_id,
        std::optional<std::string> target_id);

    /// The JSONL session file path; empty for in-memory sessions.
    [[nodiscard]] std::optional<std::filesystem::path> path() const;

private:
    struct Impl;
    struct InMemoryTag {};

    explicit SessionStore(InMemoryTag);

    /// Run `fn` under the append lock against the JSONL alternative; the
    /// in-memory alternative is a successful no-op.
    [[nodiscard]] support::ExpectedVoid dispatch_append(
        std::move_only_function<support::ExpectedVoid(JsonlSessionStore&)> fn);

    std::unique_ptr<Impl> impl_;
};

} // namespace cch::harness::session
