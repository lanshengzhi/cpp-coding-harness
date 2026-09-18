#pragma once

#include "CodexShared.hpp"

#include "ai/Timestamps.hpp"
#include "ai/providers/WebSocketTransport.hpp"
#include "support/Json.hpp"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <cstdint>

namespace cch::ai::api {

struct CodexContinuation {
    support::JsonValue last_request_body;
    std::string last_response_id;
    support::JsonValue::array_t last_response_items;
};

struct CodexSocketEntry {
    std::string session_id;
    std::string account_id;
    std::shared_ptr<providers::WebSocket> socket;
    bool busy{false};
    std::int64_t created_at_ms{0};
    std::int64_t released_at_ms{0};
    std::optional<CodexContinuation> continuation;
};

[[nodiscard]] inline std::int64_t now_epoch_ms() { return current_timestamp_ms(); }

/// pi websocketSessionCache: session-id → account-id → one connection, with
/// pi's 5-minute idle close and 55-minute hard age. Expiry is checked lazily
/// at acquisition (the socket is closed when the next request notices), which
/// keeps the observable connection/reuse behavior identical without timers

class CodexWebSocketCache {
public:
    explicit CodexWebSocketCache(providers::CodexWebSocketCacheConfig config) : config_(config) {}

    ~CodexWebSocketCache() { close_all(); }

    CodexWebSocketCache(const CodexWebSocketCache&) = delete;
    CodexWebSocketCache& operator=(const CodexWebSocketCache&) = delete;

    struct Acquisition {
        std::shared_ptr<providers::WebSocket> socket;
        std::shared_ptr<CodexSocketEntry> entry;
        bool reused{false};
    };

    /// Returns a reusable cached connection (marked busy) without connecting,
    /// or nullptr when no session/account entry is reusable. The caller then
    /// connects and registers the fresh socket.
    std::shared_ptr<CodexSocketEntry> try_reuse(
            std::optional<std::string_view> session_id, std::string_view account_id) {
        if (!session_id) {
            return nullptr;
        }
        const auto session = sessions_.find(std::string{*session_id});
        if (session == sessions_.end()) {
            return nullptr;
        }
        auto& account_entries = session->second;
        const auto found = account_entries.find(std::string{account_id});
        if (found == account_entries.end()) {
            return nullptr;
        }
        auto& entry = found->second;
        if (entry->busy) {
            return nullptr;
        }
        const auto now = now_epoch_ms();
        const bool idle_expired =
                entry->released_at_ms != 0 && now - entry->released_at_ms >= config_.idle_close.count();
        const bool aged = now - entry->created_at_ms >= config_.max_age.count();
        if (idle_expired || aged) {
            close_and_remove(*session_id, account_id, entry);
            return nullptr;
        }
        entry->busy = true;
        return entry;
    }

    /// Registers a freshly connected socket under the session/account keys.
    /// Without a session id the socket is one-shot: never cached and closed by
    /// the caller. A busy cached entry yields an uncached one-shot socket.
    Acquisition register_or_reuse(std::optional<std::string_view> session_id,
            std::string_view account_id,
            std::shared_ptr<providers::WebSocket> socket) {
        if (!session_id) {
            return Acquisition{std::move(socket), nullptr, false};
        }
        auto& account_entries = sessions_[std::string{*session_id}];
        const auto found = account_entries.find(std::string{account_id});
        if (found != account_entries.end() && found->second->busy) {
            // Busy: pi opens a fresh uncached connection instead of waiting.
            return Acquisition{std::move(socket), nullptr, false};
        }
        auto entry = std::make_shared<CodexSocketEntry>(CodexSocketEntry{
                .session_id = std::string{*session_id},
                .account_id = std::string{account_id},
                .socket = socket,
                .busy = true,
                .created_at_ms = now_epoch_ms(),
                .released_at_ms = 0,
                .continuation = std::nullopt,
        });
        account_entries.emplace(std::string{account_id}, entry);
        return Acquisition{std::move(socket), std::move(entry), false};
    }

    void release(const std::shared_ptr<CodexSocketEntry>& entry, bool keep) {
        if (!entry) {
            return;
        }
        if (!keep) {
            close_and_remove(entry->session_id, entry->account_id, entry);
            return;
        }
        entry->busy = false;
        entry->released_at_ms = now_epoch_ms();
    }

    void close_all() {
        for (auto& [_, account_entries] : sessions_) {
            for (auto& [__, entry] : account_entries) {
                entry->socket->close();
            }
        }
        sessions_.clear();
    }

private:
    void close_and_remove(
            std::string_view session_id, std::string_view account_id, const std::shared_ptr<CodexSocketEntry>& entry) {
        entry->socket->close();
        const auto session = sessions_.find(std::string{session_id});
        if (session == sessions_.end()) {
            return;
        }
        auto& account_entries = session->second;
        const auto found = account_entries.find(std::string{account_id});
        if (found != account_entries.end() && found->second == entry) {
            account_entries.erase(found);
        }
        if (account_entries.empty()) {
            sessions_.erase(session);
        }
    }

    std::map<std::string, std::map<std::string, std::shared_ptr<CodexSocketEntry>, std::less<>>, std::less<>> sessions_;
    providers::CodexWebSocketCacheConfig config_;
};

[[nodiscard]] inline support::JsonValue body_without_input_and_previous(const support::JsonValue& body) {
    auto object = *body.get_if<JsonObject>();
    object.erase("input");
    object.erase("previous_response_id");
    return support::JsonValue{std::move(object)};
}

[[nodiscard]] inline bool request_bodies_match_except_input(
        const support::JsonValue& left, const support::JsonValue& right) {
    const auto left_bytes = support::write_json(body_without_input_and_previous(left));
    const auto right_bytes = support::write_json(body_without_input_and_previous(right));
    return left_bytes && right_bytes && *left_bytes == *right_bytes;
}

/// Delta between the previous cached request body and the new one (pi's
/// continuation reuse): the new input items plus the ids to anchor against.
[[nodiscard]] std::optional<support::JsonValue::array_t> cached_input_delta(
        const support::JsonValue& body, const CodexContinuation& continuation);

} // namespace cch::ai::api
