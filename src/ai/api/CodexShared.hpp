#pragma once

#include "ai/Headers.hpp"

#include <cch/ai/Message.hpp>
#include <cch/ai/Model.hpp>
#include "ai/providers/Provider.hpp"
#include <cch/support/Error.hpp>
#include <cch/support/JsonValue.hpp>

#include <chrono>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace cch::ai::api {

using JsonObject = support::JsonValue::object_t;
using JsonArray = support::JsonValue::array_t;

constexpr std::string_view kDefaultCodexBaseUrl = "https://chatgpt.com/backend-api";
constexpr std::string_view kSseBeta = "responses=experimental";
constexpr std::string_view kPreviousResponseNotFound = "previous_response_not_found";
constexpr std::string_view kWebSocketConnectionLimitReached = "websocket_connection_limit_reached";
constexpr std::chrono::milliseconds kDefaultWebSocketConnectTimeout{15000};

enum class CodexFailureKind { Transport, Api, Protocol, Cancelled };

struct CodexFailure {
    CodexFailureKind kind{CodexFailureKind::Transport};
    std::string code{};
    std::string message{};
    support::Error error{};
    std::optional<InferenceFailure> inference_failure{std::nullopt};
};

/// Codex request shaping: backend URL resolution, the pi-parity header set
/// (Bearer + chatgpt-account-id + originator), and the JWT account-id
/// extraction wrapper that freezes auth extraction failures to one stream
/// error.

[[nodiscard]] std::string resolve_codex_url(std::string_view base_url);
[[nodiscard]] std::string resolve_codex_websocket_url(std::string_view base_url);
[[nodiscard]] support::Expected<std::string> extract_account_id(std::string_view token);
[[nodiscard]] ProviderHeaders codex_headers(
        const ProviderStreamOptions& options, std::string_view account_id, bool websocket);

} // namespace cch::ai::api
