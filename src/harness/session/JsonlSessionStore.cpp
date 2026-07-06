#include "../../../include/cch/harness/session/JsonlSessionStore.hpp"
#include "../../../include/cch/harness/session/SessionTree.hpp"
#include "EntrySerializer.hpp"
#include "SessionLeaf.hpp"
#include "SessionJournal.hpp"

#include <filesystem>
#include <string>
#include <utility>

namespace cch::harness::session {

struct JsonlSessionStore::Impl {
    std::filesystem::path path;
    SessionMetadata metadata;
    SessionJournal journal;
    std::size_t next_entry_id{1};
    std::optional<std::string> active_append_parent_id;
    bool persist_leaf_after_message_append{false};
};

JsonlSessionStore::~JsonlSessionStore() = default;
JsonlSessionStore::JsonlSessionStore(JsonlSessionStore&&) = default;
JsonlSessionStore& JsonlSessionStore::operator=(JsonlSessionStore&&) = default;

const std::filesystem::path& JsonlSessionStore::path() const { return impl_->path; }
const SessionMetadata& JsonlSessionStore::metadata() const { return impl_->metadata; }

util::Expected<JsonlSessionStore> JsonlSessionStore::create_new(
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

util::Expected<JsonlSessionStore> JsonlSessionStore::open_existing(const std::filesystem::path& path) {
    auto loaded = load(path);
    if (!loaded) {
        return std::unexpected(loaded.error());
    }

    auto journal = SessionJournal::open_existing(path);
    if (!journal) {
        return std::unexpected(journal.error());
    }

    JsonlSessionStore store;
    store.impl_ = std::make_unique<Impl>();
    store.impl_->path = path;
    store.impl_->metadata = loaded->metadata;
    store.impl_->journal = std::move(*journal);
    store.impl_->next_entry_id = loaded->entries.size();
    auto active_leaf = select_active_leaf_target(loaded->entries);
    if (active_leaf.saw_leaf_marker) {
        store.impl_->active_append_parent_id = std::move(active_leaf.target_id);
        store.impl_->persist_leaf_after_message_append = true;
    }
    return store;
}

util::Expected<LoadedSession> JsonlSessionStore::load(const std::filesystem::path& path) {
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

util::Expected<SessionTree> JsonlSessionStore::open_as_tree(const std::filesystem::path& path) {
    auto loaded = load(path);
    if (!loaded.has_value()) {
        return std::unexpected(loaded.error());
    }
    return SessionTree(std::move(*loaded));
}

util::ExpectedVoid JsonlSessionStore::append(const ai::MessageVariant& message) {
    EntrySerializer serializer;
    auto parent_id = impl_->persist_leaf_after_message_append
        ? impl_->active_append_parent_id
        : std::nullopt;
    auto entry = serializer.serialize_message_entry(message, std::move(parent_id));
    if (!entry) {
        return std::unexpected(entry.error());
    }

    auto result = impl_->journal.append_line(entry->line);
    if (!result) {
        return result;
    }
    ++impl_->next_entry_id;

    if (impl_->persist_leaf_after_message_append) {
        auto leaf_line = serializer.serialize_leaf(std::nullopt, entry->entry_id);
        if (!leaf_line) {
            return std::unexpected(leaf_line.error());
        }

        auto leaf_result = impl_->journal.append_line(*leaf_line);
        if (!leaf_result) {
            return leaf_result;
        }
        ++impl_->next_entry_id;
    }

    if (impl_->persist_leaf_after_message_append) {
        impl_->active_append_parent_id = std::move(entry->entry_id);
    }
    return {};
}

util::ExpectedVoid JsonlSessionStore::append_model_change(
    std::optional<std::string> parent_id,
    std::string provider,
    std::string model_id) {
    EntrySerializer serializer;
    auto line = serializer.serialize_model_change(std::move(parent_id), std::move(provider), std::move(model_id));
    if (!line) {
        return std::unexpected(line.error());
    }

    auto result = impl_->journal.append_line(*line);
    if (result) {
        ++impl_->next_entry_id;
    }
    return result;
}

util::ExpectedVoid JsonlSessionStore::append_thinking_level_change(
    std::optional<std::string> parent_id,
    std::string thinking_level) {
    EntrySerializer serializer;
    auto line = serializer.serialize_thinking_level_change(std::move(parent_id), std::move(thinking_level));
    if (!line) {
        return std::unexpected(line.error());
    }

    auto result = impl_->journal.append_line(*line);
    if (result) {
        ++impl_->next_entry_id;
    }
    return result;
}

util::ExpectedVoid JsonlSessionStore::append_active_tools_change(
    std::optional<std::string> parent_id,
    std::vector<std::string> tools) {
    EntrySerializer serializer;
    auto line = serializer.serialize_active_tools_change(std::move(parent_id), std::move(tools));
    if (!line) {
        return std::unexpected(line.error());
    }

    auto result = impl_->journal.append_line(*line);
    if (result) {
        ++impl_->next_entry_id;
    }
    return result;
}

util::ExpectedVoid JsonlSessionStore::append_custom_entry(
    std::optional<std::string> parent_id,
    std::string custom_type,
    util::JsonValue data) {
    EntrySerializer serializer;
    auto line = serializer.serialize_custom_entry(std::move(parent_id), std::move(custom_type), std::move(data));
    if (!line) {
        return std::unexpected(line.error());
    }

    auto result = impl_->journal.append_line(*line);
    if (result) {
        ++impl_->next_entry_id;
    }
    return result;
}

util::ExpectedVoid JsonlSessionStore::append_custom_message_entry(
    std::optional<std::string> parent_id,
    std::string custom_type,
    std::string content,
    bool display,
    std::optional<util::JsonValue> details) {
    EntrySerializer serializer;
    auto line = serializer.serialize_custom_message_entry(
        std::move(parent_id), std::move(custom_type), std::move(content), display, std::move(details));
    if (!line) {
        return std::unexpected(line.error());
    }

    auto result = impl_->journal.append_line(*line);
    if (result) {
        ++impl_->next_entry_id;
    }
    return result;
}

util::ExpectedVoid JsonlSessionStore::append_label_change(
    std::optional<std::string> parent_id,
    std::string target_id,
    std::optional<std::string> label) {
    EntrySerializer serializer;
    auto line = serializer.serialize_label_change(std::move(parent_id), std::move(target_id), std::move(label));
    if (!line) {
        return std::unexpected(line.error());
    }

    auto result = impl_->journal.append_line(*line);
    if (result) {
        ++impl_->next_entry_id;
    }
    return result;
}

util::ExpectedVoid JsonlSessionStore::append_compaction(
    std::optional<std::string> parent_id,
    std::string summary,
    std::string first_kept_entry_id,
    std::size_t tokens_before,
    std::optional<util::JsonValue> details,
    std::optional<bool> from_hook) {
    EntrySerializer serializer;
    auto line = serializer.serialize_compaction(
        std::move(parent_id),
        std::move(summary),
        std::move(first_kept_entry_id),
        tokens_before,
        std::move(details),
        from_hook);
    if (!line) {
        return std::unexpected(line.error());
    }

    auto result = impl_->journal.append_line(*line);
    if (result) {
        ++impl_->next_entry_id;
    }
    return result;
}

util::ExpectedVoid JsonlSessionStore::append_branch_summary(
    std::optional<std::string> parent_id,
    std::string from_id,
    std::string summary,
    std::optional<util::JsonValue> details,
    std::optional<bool> from_hook) {
    EntrySerializer serializer;
    auto line = serializer.serialize_branch_summary(
        std::move(parent_id), std::move(from_id), std::move(summary), std::move(details), from_hook);
    if (!line) {
        return std::unexpected(line.error());
    }

    auto result = impl_->journal.append_line(*line);
    if (result) {
        ++impl_->next_entry_id;
    }
    return result;
}

util::ExpectedVoid JsonlSessionStore::append_session_info(
    std::optional<std::string> parent_id,
    std::string name) {
    EntrySerializer serializer;
    auto line = serializer.serialize_session_info(std::move(parent_id), std::move(name));
    if (!line) {
        return std::unexpected(line.error());
    }

    auto result = impl_->journal.append_line(*line);
    if (result) {
        ++impl_->next_entry_id;
    }
    return result;
}

util::ExpectedVoid JsonlSessionStore::append_leaf(
    std::optional<std::string> parent_id,
    std::string target_id) {
    EntrySerializer serializer;
    auto line = serializer.serialize_leaf(std::move(parent_id), std::move(target_id));
    if (!line) {
        return std::unexpected(line.error());
    }

    auto result = impl_->journal.append_line(*line);
    if (result) {
        ++impl_->next_entry_id;
    }
    return result;
}

} // namespace cch::harness::session
