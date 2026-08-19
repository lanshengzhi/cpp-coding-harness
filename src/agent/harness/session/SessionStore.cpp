#include <cch/agent/harness/session/SessionStore.hpp>

#include <cch/agent/harness/session/JsonlSessionStore.hpp>
#include "InMemorySessionStore.hpp"

#include <mutex>
#include <utility>
#include <variant>

namespace cch::harness::session {

struct SessionStore::Impl {
    std::variant<JsonlSessionStore, InMemorySessionStore> store;
    // Serializes appends between Runtime worker threads (Session Event
    // Commitment channel) and the Session loop (session-assembly appends).
    std::mutex append_mutex;
};

SessionStore::SessionStore(JsonlSessionStore jsonl)
    : impl_(std::make_unique<Impl>(std::variant<JsonlSessionStore, InMemorySessionStore>{
          std::move(jsonl)})) {}

SessionStore::SessionStore(InMemoryTag)
    : impl_(std::make_unique<Impl>(std::variant<JsonlSessionStore, InMemorySessionStore>{
          InMemorySessionStore{}})) {}

SessionStore SessionStore::in_memory() {
    return SessionStore{InMemoryTag{}};
}

SessionStore::SessionStore(SessionStore&&) noexcept = default;
SessionStore& SessionStore::operator=(SessionStore&&) noexcept = default;
SessionStore::~SessionStore() = default;

support::ExpectedVoid SessionStore::dispatch_append(
    std::move_only_function<support::ExpectedVoid(JsonlSessionStore&)> fn) {
    // The in-memory alternative is a successful no-op exactly like the former
    // skipped dynamic_cast call sites.
    std::lock_guard lock(impl_->append_mutex);
    if (auto* jsonl = std::get_if<JsonlSessionStore>(&impl_->store)) {
        return fn(*jsonl);
    }
    return {};
}

support::ExpectedVoid SessionStore::append(const ai::MessageVariant& message) {
    return dispatch_append([&](JsonlSessionStore& jsonl) {
        return jsonl.append(message);
    });
}

support::ExpectedVoid SessionStore::append_model_change(
    std::optional<std::string> parent_id,
    std::string provider,
    std::string model_id) {
    return dispatch_append([&](JsonlSessionStore& jsonl) {
        return jsonl.append_model_change(
            std::move(parent_id), std::move(provider), std::move(model_id));
    });
}

support::ExpectedVoid SessionStore::append_thinking_level_change(
    std::optional<std::string> parent_id,
    std::string thinking_level) {
    return dispatch_append([&](JsonlSessionStore& jsonl) {
        return jsonl.append_thinking_level_change(
            std::move(parent_id), std::move(thinking_level));
    });
}

support::ExpectedVoid SessionStore::append_label_change(
    std::optional<std::string> parent_id,
    std::string target_id,
    std::optional<std::string> label) {
    return dispatch_append([&](JsonlSessionStore& jsonl) {
        return jsonl.append_label_change(
            std::move(parent_id), std::move(target_id), std::move(label));
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
    return dispatch_append([&](JsonlSessionStore& jsonl) {
        return jsonl.append_compaction(
            std::move(parent_id),
            std::move(summary),
            std::move(first_kept_entry_id),
            tokens_before,
            std::move(details),
            from_hook,
            std::move(retained_tail),
            std::move(usage));
    });
}

support::ExpectedVoid SessionStore::append_session_info(
    std::optional<std::string> parent_id,
    std::string name) {
    return dispatch_append([&](JsonlSessionStore& jsonl) {
        return jsonl.append_session_info(std::move(parent_id), std::move(name));
    });
}

support::ExpectedVoid SessionStore::append_leaf(
    std::optional<std::string> parent_id,
    std::optional<std::string> target_id) {
    return dispatch_append([&](JsonlSessionStore& jsonl) {
        return jsonl.append_leaf(std::move(parent_id), std::move(target_id));
    });
}

std::optional<std::filesystem::path> SessionStore::path() const {
    // The path is fixed at construction, so this read does not need the
    // append lock.
    if (const auto* jsonl = std::get_if<JsonlSessionStore>(&impl_->store)) {
        return jsonl->path();
    }
    return std::nullopt;
}

} // namespace cch::harness::session
