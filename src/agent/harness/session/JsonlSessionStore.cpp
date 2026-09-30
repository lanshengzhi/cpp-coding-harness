#include "JsonlSessionStore.hpp"

#include "agent/harness/session/EntrySerializer.hpp"
#include "agent/harness/session/SessionJournal.hpp"
#include "agent/harness/session/SessionLeaf.hpp"

#include <algorithm>
#include <filesystem>
#include <string>
#include <utility>
#include <variant>

namespace cch::harness::session {

struct JsonlSessionStore::Impl {
    std::filesystem::path path;
    SessionMetadata metadata;
    SessionJournal journal;
    std::optional<std::string> active_append_parent_id;
    bool persist_leaf_after_message_append{false};

    /// Deferred first flush (ADR 0064): while the journal is deferred — a
    /// new session before its first user or assistant message, gated exactly
    /// like pi v0.99.1 `SessionManager._persist`'s `_hasConversation` —
    /// accepted lines buffer here instead of hitting the disk:
    /// `pending_header` is the serialized header line (no terminator),
    /// `pending_lines` the entry lines (each terminated, in append order).
    std::string pending_header;
    std::vector<std::string> pending_lines;

    /// Persist one serialized entry line: a flushed journal writes through,
    /// a deferred one buffers.
    [[nodiscard]] support::ExpectedVoid buffer_line(std::string line) {
        if (!journal.deferred()) {
            return journal.append_line(line);
        }
        pending_lines.push_back(std::move(line));
        return {};
    }

    /// The flush-triggering append (the first user or assistant message):
    /// buffer the line,
    /// then write the file with the header and every buffered line in order.
    /// A failed flush rejects the triggering line like a write-through
    /// failure (the caller mirrors nothing) while the earlier buffered lines
    /// stay pending for the next flush-triggering append's retry.
    [[nodiscard]] support::ExpectedVoid flush_with_trigger_line(std::string line) {
        pending_lines.push_back(std::move(line));
        if (auto flushed = journal.flush_new(pending_header, pending_lines); !flushed) {
            pending_lines.pop_back();
            return std::unexpected(flushed.error());
        }
        pending_lines.clear();
        pending_header.clear();
        return {};
    }
};

namespace {

/// Persist one serialized entry and hand back the mirror entry the
/// serializer built alongside the line: the owning SessionStore's live tree
/// receives exactly what the wire carries (redacted content, generated id
/// and timestamp) without re-reading the file or round-tripping the line
/// back through the stricter reader.
[[nodiscard]] support::Expected<std::vector<SessionEntry>> append_mirrored_line(
    JsonlSessionStore::Impl& impl,
    support::Expected<EntrySerializer::SerializationResult> serialized) {
    if (!serialized) {
        return std::unexpected(serialized.error());
    }
    if (auto appended = impl.buffer_line(std::move(serialized->line)); !appended) {
        return std::unexpected(appended.error());
    }
    std::vector<SessionEntry> entries;
    entries.push_back(std::move(serialized->entry));
    return entries;
}

} // namespace

JsonlSessionStore::~JsonlSessionStore() = default;
JsonlSessionStore::JsonlSessionStore(JsonlSessionStore&&) = default;
JsonlSessionStore& JsonlSessionStore::operator=(JsonlSessionStore&&) = default;

std::optional<std::filesystem::path> JsonlSessionStore::path() const { return impl_->path; }
const SessionMetadata& JsonlSessionStore::metadata() const { return impl_->metadata; }

support::Expected<JsonlSessionStore> JsonlSessionStore::create_new(
    const std::filesystem::path& path, SessionMetadata metadata) {
    EntrySerializer serializer;
    auto header_json = serializer.serialize_header(metadata);
    if (!header_json) {
        return std::unexpected(header_json.error());
    }

    // Deferred first flush: the path is validated and reserved here, but the
    // file is created only when the first user or assistant message append
    // flushes the header and every buffered entry (ADR 0064).
    auto journal = SessionJournal::create_deferred(path);
    if (!journal) {
        return std::unexpected(journal.error());
    }

    JsonlSessionStore store;
    store.impl_ = std::make_unique<Impl>();
    store.impl_->path = path;
    store.impl_->metadata = std::move(metadata);
    store.impl_->journal = std::move(*journal);
    store.impl_->pending_header = std::move(*header_json);
    return store;
}

support::Expected<JsonlSessionStore> JsonlSessionStore::create_from_entries(
        const std::filesystem::path& path, SessionMetadata metadata, std::vector<SessionEntry> entries) {
    // The header comes from `metadata` alone, so a nested Header entry is a
    // caller error. Reject it before the journal exists, so a rejected fork
    // leaves no half-written session file at the target path.
    if (std::any_of(entries.begin(), entries.end(), [](const SessionEntry& entry) {
            return entry.kind == SessionEntryKind::Header;
        })) {
        return std::unexpected(support::make_error(
                support::ErrorCode::Session, "a new session file's header is written from metadata alone"));
    }

    EntrySerializer serializer;
    auto header_json = serializer.serialize_header(metadata);
    if (!header_json) {
        return std::unexpected(header_json.error());
    }

    // Both arms are normalized to an unterminated wire line: a known kind
    // loses the terminator `serialize_entry` adds, and a foreign kind keeps
    // the `raw_line` the reader handed back without one.
    std::vector<std::string> lines;
    lines.reserve(entries.size());
    for (const auto& entry : entries) {
        if (entry.kind == SessionEntryKind::Unknown) {
            lines.push_back(entry.raw_line);
            continue;
        }
        auto line = serializer.serialize_entry(entry);
        if (!line) {
            return std::unexpected(line.error());
        }
        lines.push_back(line->ends_with('\n') ? line->substr(0, line->size() - 1) : std::move(*line));
    }

    auto journal = SessionJournal::create_new(path, *header_json);
    if (!journal) {
        return std::unexpected(journal.error());
    }
    for (auto& line : lines) {
        line += '\n';
        if (auto appended = journal->append_line(line); !appended) {
            return std::unexpected(appended.error());
        }
    }

    JsonlSessionStore store;
    store.impl_ = std::make_unique<Impl>();
    store.impl_->path = path;
    store.impl_->metadata = std::move(metadata);
    store.impl_->journal = std::move(*journal);
    return store;
}

support::Expected<JsonlSessionStore> JsonlSessionStore::open_existing(const std::filesystem::path& path) {
    auto loaded = load(path);
    if (!loaded) {
        return std::unexpected(loaded.error());
    }
    return open_loaded(path, *loaded);
}

support::Expected<JsonlSessionStore> JsonlSessionStore::open_loaded(
    const std::filesystem::path& path,
    const LoadedSession& loaded) {
    auto journal = SessionJournal::open_existing(path);
    if (!journal) {
        return std::unexpected(journal.error());
    }

    JsonlSessionStore store;
    store.impl_ = std::make_unique<Impl>();
    store.impl_->path = path;
    store.impl_->metadata = loaded.metadata;
    store.impl_->journal = std::move(*journal);
    auto active_leaf = select_active_leaf_target(loaded.entries);
    if (active_leaf.saw_leaf_marker) {
        store.impl_->active_append_parent_id = active_leaf.target_id;
        store.impl_->persist_leaf_after_message_append = true;
    }
    return store;
}

support::Expected<LoadedSession> JsonlSessionStore::load(const std::filesystem::path& path) {
    auto journal = SessionJournal::open_existing(path);
    if (!journal) {
        return std::unexpected(journal.error());
    }

    auto lines = journal->read_lines();
    if (!lines) {
        return std::unexpected(lines.error());
    }

    EntrySerializer serializer;
    return serializer.parse_lines(*lines);
}

AppendResult JsonlSessionStore::append(const ai::MessageVariant& message) {
    EntrySerializer serializer;
    auto parent_id = impl_->persist_leaf_after_message_append
        ? impl_->active_append_parent_id
        : std::nullopt;
    auto serialized = serializer.serialize_message_entry(message, std::move(parent_id));
    if (!serialized) {
        return AppendResult{
            .entries = {},
            .status = std::unexpected(serialized.error()),
        };
    }

    // pi's delayed first flush verbatim (v0.99.1 `_hasConversation`): the
    // first user or assistant message append creates the file, so a provider
    // failure never costs the completed user history (ADR 0064, pi #10000).
    const auto triggers_flush = std::holds_alternative<ai::UserMessage>(message)
        || std::holds_alternative<ai::AssistantMessage>(message);
    auto persisted = impl_->journal.deferred() && triggers_flush
        ? impl_->flush_with_trigger_line(std::move(serialized->line))
        : impl_->buffer_line(std::move(serialized->line));
    if (!persisted) {
        return AppendResult{
            .entries = {},
            .status = std::unexpected(persisted.error()),
        };
    }

    AppendResult outcome;
    const auto message_entry_id = serialized->entry.entry_id;
    outcome.entries.push_back(std::move(serialized->entry));

    if (impl_->persist_leaf_after_message_append) {
        // The message itself is already durable. Advance the in-process append
        // parent before writing the leaf marker so a marker failure does not
        // poison or roll back later appends.
        impl_->active_append_parent_id = message_entry_id;

        auto leaf = serializer.serialize_leaf(std::nullopt, message_entry_id);
        if (!leaf) {
            outcome.status = std::unexpected(leaf.error());
            return outcome;
        }
        if (auto leaf_result = impl_->buffer_line(std::move(leaf->line)); !leaf_result) {
            outcome.status = std::unexpected(leaf_result.error());
            return outcome;
        }
        outcome.entries.push_back(std::move(leaf->entry));
    }
    return outcome;
}

support::Expected<std::vector<SessionEntry>> JsonlSessionStore::append_model_change(
    std::optional<std::string> parent_id,
    std::string provider,
    std::string model_id) {
    EntrySerializer serializer;
    return append_mirrored_line(*impl_, serializer.serialize_model_change(
        std::move(parent_id), std::move(provider), std::move(model_id)));
}

support::Expected<std::vector<SessionEntry>> JsonlSessionStore::append_thinking_level_change(
    std::optional<std::string> parent_id,
    std::string thinking_level) {
    EntrySerializer serializer;
    return append_mirrored_line(*impl_, serializer.serialize_thinking_level_change(
        std::move(parent_id), std::move(thinking_level)));
}

support::Expected<std::vector<SessionEntry>> JsonlSessionStore::append_custom_entry(
    std::optional<std::string> parent_id,
    std::string custom_type,
    support::JsonValue data) {
    EntrySerializer serializer;
    return append_mirrored_line(*impl_, serializer.serialize_custom_entry(
        std::move(parent_id), std::move(custom_type), std::move(data)));
}

support::Expected<std::vector<SessionEntry>> JsonlSessionStore::append_custom_message_entry(
    std::optional<std::string> parent_id,
    std::string custom_type,
    CustomMessageEntryContent content,
    bool display,
    std::optional<support::JsonValue> details) {
    EntrySerializer serializer;
    return append_mirrored_line(*impl_, serializer.serialize_custom_message_entry(
        std::move(parent_id),
        std::move(custom_type),
        std::move(content),
        display,
        std::move(details)));
}

support::Expected<std::vector<SessionEntry>> JsonlSessionStore::append_label_change(
    std::optional<std::string> parent_id,
    std::string target_id,
    std::optional<std::string> label) {
    EntrySerializer serializer;
    return append_mirrored_line(*impl_, serializer.serialize_label_change(
        std::move(parent_id), std::move(target_id), std::move(label)));
}

support::Expected<std::vector<SessionEntry>> JsonlSessionStore::append_compaction(
    std::optional<std::string> parent_id,
    CompactionEntryValue value) {
    EntrySerializer serializer;
    return append_mirrored_line(*impl_, serializer.serialize_compaction(
        std::move(parent_id), std::move(value)));
}

support::Expected<std::vector<SessionEntry>> JsonlSessionStore::append_branch_summary(
        std::optional<std::string> parent_id,
        std::string from_id,
        std::string summary,
        std::optional<support::JsonValue> details,
        std::optional<bool> from_hook,
        std::optional<ai::Usage> usage) {
    EntrySerializer serializer;
    return append_mirrored_line(*impl_,
            serializer.serialize_branch_summary(std::move(parent_id),
                    std::move(from_id),
                    std::move(summary),
                    std::move(details),
                    from_hook,
                    std::move(usage)));
}

support::Expected<std::vector<SessionEntry>> JsonlSessionStore::append_session_info(
    std::optional<std::string> parent_id,
    std::string name) {
    EntrySerializer serializer;
    return append_mirrored_line(*impl_, serializer.serialize_session_info(
        std::move(parent_id), std::move(name)));
}

support::Expected<std::vector<SessionEntry>> JsonlSessionStore::append_leaf(
    std::optional<std::string> parent_id,
    std::optional<std::string> target_id) {
    // The marker target is also the new in-process append parent: copy it
    // before the serializer consumes the optional (a moved-from optional
    // holds a moved-from string, never nullopt).
    const auto marker_target = target_id;
    EntrySerializer serializer;
    auto entries = append_mirrored_line(*impl_, serializer.serialize_leaf(
        std::move(parent_id), std::move(target_id)));
    if (!entries) {
        return entries;
    }
    // A durable leaf marker IS the new active position: the in-process
    // append parent follows it (pi: the next append becomes a child of the
    // new leaf, or a root at the null position), and marker discipline
    // engages exactly like a resumed marker-bearing file.
    impl_->active_append_parent_id = marker_target;
    impl_->persist_leaf_after_message_append = true;
    return entries;
}

} // namespace cch::harness::session
