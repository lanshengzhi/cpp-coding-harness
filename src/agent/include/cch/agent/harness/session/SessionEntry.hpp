#pragma once

#include <cch/ai/Message.hpp>
#include <cch/support/JsonValue.hpp>

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace cch::harness::session {

struct SessionMetadata {
    std::string session_id;
    std::string created_at;
    std::filesystem::path workspace;
    std::string provider;
    std::string model;
    /// pi `parentSession?: string` — the absolute source session path for
    /// forked sessions (absent on ordinary sessions).
    std::optional<std::filesystem::path> parent_session{std::nullopt};
};

enum class SessionEntryKind {
    Header,
    Message,
    ModelChange,
    ThinkingLevelChange,
    Custom,
    CustomMessage,
    Label,
    Compaction,
    BranchSummary,
    SessionInfo,
    Unknown,
};

struct ModelChangeValue {
    std::string provider;
    std::string model_id;
};

struct ThinkingLevelChangeValue {
    std::string thinking_level;
};

struct CustomEntryValue {
    std::string custom_type;
    /// pi `data?: T` — absent (omitted on the wire) vs explicit null are both
    /// representable: nullopt = absent, engaged = the JSON value (including an
    /// explicit JSON null).
    std::optional<support::JsonValue> data;
};

using CustomMessageEntryContentBlock = std::variant<ai::TextContent, ai::ImageContent>;
using CustomMessageEntryContent =
    std::variant<std::string, std::vector<CustomMessageEntryContentBlock>>;

struct CustomMessageEntryValue {
    std::string custom_type;
    CustomMessageEntryContent content;
    bool display{true};
    std::optional<support::JsonValue> details;
};

struct LabelEntryValue {
    std::string target_id;
    std::optional<std::string> label;
};

struct CompactionEntryValue {
    std::string summary;
    /// pi `firstKeptEntryId?: string` — nullopt is omitted on the wire.
    std::optional<std::string> first_kept_entry_id;
    std::size_t tokens_before{0};
    /// pi `retainedTail?: AgentMessage[]` — recent messages kept on the entry
    /// itself; context rebuild projects retained system updates, the summary,
    /// then the remaining tail.
    std::optional<std::vector<ai::MessageVariant>> retained_tail{};
    std::optional<support::JsonValue> details{};
    std::optional<ai::Usage> usage{};
    std::optional<bool> from_hook{};
};

struct BranchSummaryEntryValue {
    std::string from_id;
    std::string summary;
    std::optional<support::JsonValue> details;
    std::optional<ai::Usage> usage;
    std::optional<bool> from_hook;
};

struct SessionInfoEntryValue {
    /// pi `name?: string` — nullopt is omitted on the wire.
    std::optional<std::string> name;
};

using SessionEntryValue = std::variant<std::monostate,
        ModelChangeValue,
        ThinkingLevelChangeValue,
        CustomEntryValue,
        CustomMessageEntryValue,
        LabelEntryValue,
        CompactionEntryValue,
        BranchSummaryEntryValue,
        SessionInfoEntryValue>;

/// The three shapes a session line's `parentId` can take. pi's field is
/// `parentId: string | null` (`session-manager.ts:60`) and pi never infers a
/// parent on read: an explicit `null` is a root, and an id that names no
/// entry is an orphan root. The absent field is the third shape — a legacy
/// flat file whose chain comes from the entry order — which pi meets only as
/// an un-migrated v1 file it rewrites on open (`migrateV1ToV2`). C++ writes a
/// value or an explicit `null`, so `Absent` survives only for hand-written
/// and legacy files.
enum class ParentIdKind {
    /// The line carries no `parentId` field: the legacy flat-file shape,
    /// whose parent comes from the entry order.
    Absent,
    /// The line carries an explicit `parentId: null`: a root.
    Root,
    /// The line carries a parent entry id.
    Entry,
};

/// A session entry's `parentId` exactly as the wire spells it.
struct ParentId {
    ParentIdKind kind{ParentIdKind::Absent};
    /// The parent entry id; read only when `kind == ParentIdKind::Entry`.
    std::string entry_id;

    friend bool operator==(const ParentId&, const ParentId&) = default;
};

/// The parent entry this `parentId` names, or nullopt when it names none: an
/// absent field, an explicit root, or a value that is not a usable entry name
/// (pi `getTree` treats all three as a root).
[[nodiscard]] inline std::optional<std::string> parent_entry_id(const ParentId& parent) {
    if (parent.kind != ParentIdKind::Entry || parent.entry_id.empty()) {
        return std::nullopt;
    }
    return parent.entry_id;
}

/// The wire shape an append writes for an optional parent: nullopt is an
/// explicit `null` root, which is exactly what pi writes when its leaf
/// pointer is null (pi `appendMessage` writes `parentId: this.leafId`).
[[nodiscard]] inline ParentId parent_id_from_optional(const std::optional<std::string>& parent_id) {
    if (parent_id.has_value()) {
        return ParentId{.kind = ParentIdKind::Entry, .entry_id = *parent_id};
    }
    return ParentId{.kind = ParentIdKind::Root, .entry_id = {}};
}

/// The wire shape for a parent entry id.
[[nodiscard]] inline ParentId parent_entry(std::string entry_id) {
    return ParentId{.kind = ParentIdKind::Entry, .entry_id = std::move(entry_id)};
}

struct SessionEntry {
    SessionEntryKind kind{SessionEntryKind::Unknown};
    std::string entry_id;
    ParentId parent_id;
    ai::TimestampMs timestamp{};
    std::optional<ai::MessageVariant> message;
    SessionEntryValue value;
    support::JsonValue payload;
    std::string raw_line;
};

struct LoadedSession {
    SessionMetadata metadata;
    std::vector<ai::MessageVariant> messages;
    std::vector<SessionEntry> entries;
    std::vector<std::string> unknown_lines;
};

} // namespace cch::harness::session
