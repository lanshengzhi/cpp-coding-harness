#include <cch/agent/harness/session/SessionStore.hpp>

#include "agent/harness/session/EntrySerializer.hpp"
#include "agent/harness/session/JsonlSessionStore.hpp"

#include <mutex>
#include <utility>

namespace cch::harness::session {

struct SessionStore::Impl {
    explicit Impl(SessionTree live_tree) : tree(std::move(live_tree)) {}

    /// The persistence alternative, engaged only for JSONL sessions; an
    /// in-memory session has no journal and no durable append position.
    std::optional<JsonlSessionStore> persistence;
    SessionTree tree;
    // Serializes appends and tree queries between Runtime worker threads
    // (Session Event Commitment channel) and the Session loop (session-
    // assembly appends, topology queries, context reconstruction).
    std::mutex mutex;

    /// Mirror one in-memory append into the live tree.
    void record(SessionEntry entry) { tree.append_entry(std::move(entry)); }

    /// Mirror a persisting append's accepted entries into the live tree.
    [[nodiscard]] support::ExpectedVoid record_persisted(support::Expected<std::vector<SessionEntry>> outcome) {
        if (!outcome) {
            return std::unexpected(outcome.error());
        }
        for (auto& entry : *outcome) {
            record(std::move(entry));
        }
        return {};
    }

    /// The live tree's current leaf as an explicit append parent (nullopt at
    /// the root position) — pi `appendMessage` hangs the message under it.
    [[nodiscard]] std::optional<std::string> leaf_parent() const {
        const auto& leaf = tree.leaf_id();
        return leaf.empty() ? std::nullopt : std::optional<std::string>{leaf};
    }
};

SessionStore::SessionStore(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

support::Expected<SessionStore> SessionStore::create_new(
    const std::filesystem::path& path,
    SessionMetadata metadata) {
    auto jsonl = JsonlSessionStore::create_new(path, metadata);
    if (!jsonl) {
        return std::unexpected(jsonl.error());
    }
    LoadedSession empty;
    empty.metadata = std::move(metadata);
    auto impl = std::make_unique<Impl>(SessionTree(std::move(empty)));
    impl->persistence = std::move(*jsonl);
    return SessionStore(std::move(impl));
}

support::Expected<SessionStore> SessionStore::open_existing(const std::filesystem::path& path) {
    // Parse the persisted entries exactly once: the live tree is built from
    // the same load that opens the journal and resolves the append-parent
    // and leaf-marker state.
    auto loaded = JsonlSessionStore::load(path);
    if (!loaded) {
        return std::unexpected(loaded.error());
    }
    auto jsonl = JsonlSessionStore::open_loaded(path, *loaded);
    if (!jsonl) {
        return std::unexpected(jsonl.error());
    }
    auto impl = std::make_unique<Impl>(SessionTree(std::move(*loaded)));
    impl->persistence = std::move(*jsonl);
    return SessionStore(std::move(impl));
}

SessionStore SessionStore::in_memory(SessionMetadata metadata) {
    LoadedSession empty;
    empty.metadata = std::move(metadata);
    return SessionStore(std::make_unique<Impl>(SessionTree(std::move(empty))));
}

support::Expected<LoadedSession> SessionStore::load(const std::filesystem::path& path) {
    return JsonlSessionStore::load(path);
}

support::ExpectedVoid SessionStore::create_from_entries(
        const std::filesystem::path& path, SessionMetadata metadata, std::vector<SessionEntry> entries) {
    auto jsonl = JsonlSessionStore::create_from_entries(path, std::move(metadata), std::move(entries));
    if (!jsonl) {
        return std::unexpected(jsonl.error());
    }
    return {};
}

SessionStore::SessionStore(SessionStore&&) noexcept = default;
SessionStore& SessionStore::operator=(SessionStore&&) noexcept = default;
SessionStore::~SessionStore() = default;

support::ExpectedVoid SessionStore::append(const ai::MessageVariant& message) {
    std::lock_guard lock(impl_->mutex);
    if (auto& jsonl = impl_->persistence) {
        // The message is durable before its leaf marker, so a marker failure
        // must not leave the live tree behind the file.
        auto outcome = jsonl->append(message);
        for (auto& entry : outcome.entries) {
            impl_->record(std::move(entry));
        }
        if (!outcome.status) {
            return std::unexpected(outcome.status.error());
        }
        return {};
    }
    impl_->record(EntrySerializer::new_message_entry(message, impl_->leaf_parent()));
    return {};
}

support::ExpectedVoid SessionStore::append_model_change(
    std::optional<std::string> parent_id,
    std::string provider,
    std::string model_id) {
    std::lock_guard lock(impl_->mutex);
    if (auto& jsonl = impl_->persistence) {
        return impl_->record_persisted(
                jsonl->append_model_change(std::move(parent_id), std::move(provider), std::move(model_id)));
    }
    impl_->record(
            EntrySerializer::new_model_change_entry(std::move(parent_id), std::move(provider), std::move(model_id)));
    return {};
}

support::ExpectedVoid SessionStore::append_thinking_level_change(
    std::optional<std::string> parent_id,
    std::string thinking_level) {
    std::lock_guard lock(impl_->mutex);
    if (auto& jsonl = impl_->persistence) {
        return impl_->record_persisted(
                jsonl->append_thinking_level_change(std::move(parent_id), std::move(thinking_level)));
    }
    impl_->record(EntrySerializer::new_thinking_level_change_entry(std::move(parent_id), std::move(thinking_level)));
    return {};
}

support::ExpectedVoid SessionStore::append_label_change(
    std::optional<std::string> parent_id,
    std::string target_id,
    std::optional<std::string> label) {
    std::lock_guard lock(impl_->mutex);
    if (auto& jsonl = impl_->persistence) {
        return impl_->record_persisted(
                jsonl->append_label_change(std::move(parent_id), std::move(target_id), std::move(label)));
    }
    impl_->record(
            EntrySerializer::new_label_change_entry(std::move(parent_id), std::move(target_id), std::move(label)));
    return {};
}

support::ExpectedVoid SessionStore::append_compaction(
    std::optional<std::string> parent_id,
    CompactionEntryValue value) {
    std::lock_guard lock(impl_->mutex);
    if (auto& jsonl = impl_->persistence) {
        return impl_->record_persisted(jsonl->append_compaction(std::move(parent_id), std::move(value)));
    }
    impl_->record(EntrySerializer::new_compaction_entry(std::move(parent_id), std::move(value)));
    return {};
}

support::ExpectedVoid SessionStore::append_branch_summary(std::optional<std::string> parent_id,
        std::string from_id,
        std::string summary,
        std::optional<support::JsonValue> details,
        std::optional<bool> from_hook,
        std::optional<ai::Usage> usage) {
    std::lock_guard lock(impl_->mutex);
    if (auto& jsonl = impl_->persistence) {
        return impl_->record_persisted(jsonl->append_branch_summary(std::move(parent_id),
                std::move(from_id),
                std::move(summary),
                std::move(details),
                from_hook,
                std::move(usage)));
    }
    impl_->record(EntrySerializer::new_branch_summary_entry(std::move(parent_id),
            std::move(from_id),
            std::move(summary),
            std::move(details),
            from_hook,
            std::move(usage)));
    return {};
}

support::ExpectedVoid SessionStore::append_session_info(
    std::optional<std::string> parent_id,
    std::string name) {
    std::lock_guard lock(impl_->mutex);
    if (auto& jsonl = impl_->persistence) {
        return impl_->record_persisted(jsonl->append_session_info(std::move(parent_id), std::move(name)));
    }
    impl_->record(EntrySerializer::new_session_info_entry(std::move(parent_id), std::move(name)));
    return {};
}

support::ExpectedVoid SessionStore::append_leaf(
    std::optional<std::string> parent_id,
    std::optional<std::string> target_id) {
    std::lock_guard lock(impl_->mutex);
    if (auto& jsonl = impl_->persistence) {
        return impl_->record_persisted(jsonl->append_leaf(std::move(parent_id), std::move(target_id)));
    }
    impl_->record(EntrySerializer::new_leaf_entry(std::move(parent_id), std::move(target_id)));
    return {};
}

// --- Live tree queries (snapshots taken under the append lock) ---

std::vector<SessionTreeNode> SessionStore::tree() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->tree.get_tree();
}

SessionTreeSnapshot SessionStore::tree_snapshot() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->tree.topology();
}

SessionContext SessionStore::build_context() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->tree.buildSessionContext();
}

support::ExpectedVoid SessionStore::branch(std::string_view target_id) {
    std::lock_guard lock(impl_->mutex);
    return impl_->tree.branch(target_id);
}

void SessionStore::reset_leaf() {
    std::lock_guard lock(impl_->mutex);
    impl_->tree.reset_leaf();
}

std::string SessionStore::leaf_id() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->tree.leaf_id();
}

std::optional<SessionEntry> SessionStore::get_entry(std::string_view entry_id) const {
    std::lock_guard lock(impl_->mutex);
    if (const auto* entry = impl_->tree.getEntry(entry_id)) {
        return *entry;
    }
    return std::nullopt;
}

std::optional<std::string> SessionStore::effective_parent_id(std::string_view entry_id) const {
    std::lock_guard lock(impl_->mutex);
    return impl_->tree.effective_parent_id(entry_id);
}

std::vector<SessionEntry> SessionStore::get_branch(std::string_view from_id) const {
    std::lock_guard lock(impl_->mutex);
    std::vector<SessionEntry> branch;
    for (const auto* entry : impl_->tree.getBranch(from_id)) {
        branch.push_back(*entry);
    }
    return branch;
}

std::vector<SessionEntry> SessionStore::entries() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->tree.entries();
}

std::optional<std::string> SessionStore::get_label(std::string_view entry_id) const {
    std::lock_guard lock(impl_->mutex);
    return impl_->tree.get_label(entry_id);
}

std::optional<std::string> SessionStore::get_session_name() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->tree.get_session_name();
}

std::optional<std::filesystem::path> SessionStore::path() const {
    // The path is fixed at construction, so this read does not need the
    // append lock.
    if (impl_->persistence) {
        return impl_->persistence->path();
    }
    return std::nullopt;
}

const SessionMetadata& SessionStore::metadata() const {
    // The metadata is fixed at construction (the live tree owns the header
    // copy), so this read does not need the append lock.
    return impl_->tree.metadata();
}

} // namespace cch::harness::session
