#include <cch/agent/harness/session/SessionStore.hpp>

#include "agent/harness/session/InMemorySessionStore.hpp"
#include "agent/harness/session/JsonlSessionStore.hpp"

#include <mutex>
#include <utility>

namespace cch::harness::session {

struct SessionStore::Impl {
    Impl(JsonlSessionStore jsonl, SessionTree live_tree)
        : store(std::move(jsonl)), tree(std::move(live_tree)) {}
    Impl(InMemorySessionStore memory, SessionTree live_tree)
        : store(std::move(memory)), tree(std::move(live_tree)) {}

    StorageVariant store;
    SessionTree tree;
    // Serializes appends and tree queries between Runtime worker threads
    // (Session Event Commitment channel) and the Session loop (session-
    // assembly appends, topology queries, context reconstruction).
    std::mutex mutex;
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
    SessionTree tree(std::move(empty));
    return SessionStore(std::make_unique<Impl>(std::move(*jsonl), std::move(tree)));
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
    SessionTree tree(std::move(*loaded));
    return SessionStore(std::make_unique<Impl>(std::move(*jsonl), std::move(tree)));
}

SessionStore SessionStore::in_memory(SessionMetadata metadata) {
    LoadedSession empty;
    empty.metadata = std::move(metadata);
    SessionTree tree(std::move(empty));
    return SessionStore(std::make_unique<Impl>(InMemorySessionStore{}, std::move(tree)));
}

support::Expected<LoadedSession> SessionStore::load(const std::filesystem::path& path) {
    return JsonlSessionStore::load(path);
}

SessionStore::SessionStore(SessionStore&&) noexcept = default;
SessionStore& SessionStore::operator=(SessionStore&&) noexcept = default;
SessionStore::~SessionStore() = default;

support::ExpectedVoid SessionStore::dispatch_append(
    std::move_only_function<support::Expected<std::vector<SessionEntry>>(StorageVariant&)> fn) {
    std::lock_guard lock(impl_->mutex);
    auto entries = fn(impl_->store);
    if (!entries) {
        return std::unexpected(entries.error());
    }
    for (auto& entry : *entries) {
        impl_->tree.append_entry(std::move(entry));
    }
    return {};
}

support::ExpectedVoid SessionStore::append(const ai::MessageVariant& message) {
    std::lock_guard lock(impl_->mutex);
    if (auto* jsonl = std::get_if<JsonlSessionStore>(&impl_->store)) {
        auto outcome = jsonl->append(message);
        // Mirror every durably written entry even when the outcome is an
        // error (the message persists before its leaf marker, so a marker
        // failure must not leave the live tree behind the file).
        for (auto& entry : outcome.entries) {
            impl_->tree.append_entry(std::move(entry));
        }
        if (!outcome.status) {
            return std::unexpected(outcome.status.error());
        }
        return {};
    }
    // In-memory: pi `appendMessage` hangs the message under the current
    // leaf (null at the root position).
    const auto& leaf = impl_->tree.leaf_id();
    auto entries = std::get<InMemorySessionStore>(impl_->store).append(
        message,
        leaf.empty() ? std::nullopt : std::optional<std::string>{leaf});
    if (!entries) {
        return std::unexpected(entries.error());
    }
    for (auto& entry : *entries) {
        impl_->tree.append_entry(std::move(entry));
    }
    return {};
}

support::ExpectedVoid SessionStore::append_model_change(
    std::optional<std::string> parent_id,
    std::string provider,
    std::string model_id) {
    return dispatch_append([&](StorageVariant& store) {
        return std::visit(
            [&](auto& active) {
                return active.append_model_change(
                    std::move(parent_id), std::move(provider), std::move(model_id));
            },
            store);
    });
}

support::ExpectedVoid SessionStore::append_thinking_level_change(
    std::optional<std::string> parent_id,
    std::string thinking_level) {
    return dispatch_append([&](StorageVariant& store) {
        return std::visit(
            [&](auto& active) {
                return active.append_thinking_level_change(
                    std::move(parent_id), std::move(thinking_level));
            },
            store);
    });
}

support::ExpectedVoid SessionStore::append_label_change(
    std::optional<std::string> parent_id,
    std::string target_id,
    std::optional<std::string> label) {
    return dispatch_append([&](StorageVariant& store) {
        return std::visit(
            [&](auto& active) {
                return active.append_label_change(
                    std::move(parent_id), std::move(target_id), std::move(label));
            },
            store);
    });
}

support::ExpectedVoid SessionStore::append_compaction(
    std::optional<std::string> parent_id,
    std::string summary,
    std::string first_kept_entry_id,
    std::size_t tokens_before,
    std::optional<support::JsonValue> details,
    std::optional<bool> from_hook,
    std::vector<ai::MessageVariant> retained_tail,
    std::optional<ai::Usage> usage) {
    return dispatch_append([&](StorageVariant& store) {
        return std::visit(
            [&](auto& active) {
                return active.append_compaction(
                    std::move(parent_id),
                    std::move(summary),
                    std::move(first_kept_entry_id),
                    tokens_before,
                    std::move(details),
                    from_hook,
                    std::move(retained_tail),
                    std::move(usage));
            },
            store);
    });
}

support::ExpectedVoid SessionStore::append_branch_summary(
    std::optional<std::string> parent_id,
    std::string from_id,
    std::string summary,
    std::optional<support::JsonValue> details,
    std::optional<bool> from_hook) {
    return dispatch_append([&](StorageVariant& store) {
        return std::visit(
            [&](auto& active) {
                return active.append_branch_summary(
                    std::move(parent_id),
                    std::move(from_id),
                    std::move(summary),
                    std::move(details),
                    from_hook);
            },
            store);
    });
}

support::ExpectedVoid SessionStore::append_session_info(
    std::optional<std::string> parent_id,
    std::string name) {
    return dispatch_append([&](StorageVariant& store) {
        return std::visit(
            [&](auto& active) {
                return active.append_session_info(std::move(parent_id), std::move(name));
            },
            store);
    });
}

support::ExpectedVoid SessionStore::append_leaf(
    std::optional<std::string> parent_id,
    std::optional<std::string> target_id) {
    return dispatch_append([&](StorageVariant& store) {
        return std::visit(
            [&](auto& active) {
                return active.append_leaf(std::move(parent_id), std::move(target_id));
            },
            store);
    });
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
    if (const auto* jsonl = std::get_if<JsonlSessionStore>(&impl_->store)) {
        return jsonl->path();
    }
    return std::nullopt;
}

const SessionMetadata& SessionStore::metadata() const {
    // The metadata is fixed at construction (the live tree owns the header
    // copy), so this read does not need the append lock.
    return impl_->tree.metadata();
}

} // namespace cch::harness::session
