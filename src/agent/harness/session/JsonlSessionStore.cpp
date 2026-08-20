#include "JsonlSessionStore.hpp"

#include "agent/harness/session/EntrySerializer.hpp"
#include "agent/harness/session/SessionJournal.hpp"
#include "agent/harness/session/SessionLeaf.hpp"

#include <filesystem>
#include <string>
#include <utility>

namespace cch::harness::session {

namespace {

/// Persist one serialized entry and hand back the mirror entry the
/// serializer built alongside the line: the owning SessionStore's live tree
/// receives exactly what the wire carries (redacted content, generated id
/// and timestamp) without re-reading the file or round-tripping the line
/// back through the stricter reader.
[[nodiscard]] support::Expected<std::vector<SessionEntry>> append_mirrored_line(
    SessionJournal& journal,
    support::Expected<EntrySerializer::SerializationResult> serialized) {
    if (!serialized) {
        return std::unexpected(serialized.error());
    }
    if (auto appended = journal.append_line(serialized->line); !appended) {
        return std::unexpected(appended.error());
    }
    std::vector<SessionEntry> entries;
    entries.push_back(std::move(serialized->entry));
    return entries;
}

} // namespace

struct JsonlSessionStore::Impl {
    std::filesystem::path path;
    SessionMetadata metadata;
    SessionJournal journal;
    std::optional<std::string> active_append_parent_id;
    bool persist_leaf_after_message_append{false};
};

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

    auto journal = SessionJournal::create_new(path, *header_json);
    if (!journal) {
        return std::unexpected(journal.error());
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

    if (auto result = impl_->journal.append_line(serialized->line); !result) {
        return AppendResult{
            .entries = {},
            .status = std::unexpected(result.error()),
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
        if (auto leaf_result = impl_->journal.append_line(leaf->line); !leaf_result) {
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
    return append_mirrored_line(impl_->journal, serializer.serialize_model_change(
        std::move(parent_id), std::move(provider), std::move(model_id)));
}

support::Expected<std::vector<SessionEntry>> JsonlSessionStore::append_thinking_level_change(
    std::optional<std::string> parent_id,
    std::string thinking_level) {
    EntrySerializer serializer;
    return append_mirrored_line(impl_->journal, serializer.serialize_thinking_level_change(
        std::move(parent_id), std::move(thinking_level)));
}

support::Expected<std::vector<SessionEntry>> JsonlSessionStore::append_active_tools_change(
    std::optional<std::string> parent_id,
    std::vector<std::string> tools) {
    EntrySerializer serializer;
    return append_mirrored_line(impl_->journal, serializer.serialize_active_tools_change(
        std::move(parent_id), std::move(tools)));
}

support::Expected<std::vector<SessionEntry>> JsonlSessionStore::append_custom_entry(
    std::optional<std::string> parent_id,
    std::string custom_type,
    support::JsonValue data) {
    EntrySerializer serializer;
    return append_mirrored_line(impl_->journal, serializer.serialize_custom_entry(
        std::move(parent_id), std::move(custom_type), std::move(data)));
}

support::Expected<std::vector<SessionEntry>> JsonlSessionStore::append_custom_message_entry(
    std::optional<std::string> parent_id,
    std::string custom_type,
    CustomMessageEntryContent content,
    bool display,
    std::optional<support::JsonValue> details) {
    EntrySerializer serializer;
    return append_mirrored_line(impl_->journal, serializer.serialize_custom_message_entry(
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
    return append_mirrored_line(impl_->journal, serializer.serialize_label_change(
        std::move(parent_id), std::move(target_id), std::move(label)));
}

support::Expected<std::vector<SessionEntry>> JsonlSessionStore::append_compaction(
    std::optional<std::string> parent_id,
    CompactionEntryValue value) {
    EntrySerializer serializer;
    return append_mirrored_line(impl_->journal, serializer.serialize_compaction(
        std::move(parent_id), std::move(value)));
}

support::Expected<std::vector<SessionEntry>> JsonlSessionStore::append_branch_summary(
    std::optional<std::string> parent_id,
    std::string from_id,
    std::string summary,
    std::optional<support::JsonValue> details,
    std::optional<bool> from_hook) {
    EntrySerializer serializer;
    return append_mirrored_line(impl_->journal, serializer.serialize_branch_summary(
        std::move(parent_id),
        std::move(from_id),
        std::move(summary),
        std::move(details),
        from_hook));
}

support::Expected<std::vector<SessionEntry>> JsonlSessionStore::append_session_info(
    std::optional<std::string> parent_id,
    std::string name) {
    EntrySerializer serializer;
    return append_mirrored_line(impl_->journal, serializer.serialize_session_info(
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
    auto entries = append_mirrored_line(impl_->journal, serializer.serialize_leaf(
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
