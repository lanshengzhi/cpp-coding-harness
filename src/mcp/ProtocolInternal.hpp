#pragma once

// Private JSON-RPC framing shared by the stdio and streamable-http transports.
// Not an Owner Interface: it needs the private JSON serializer and is compiled
// into cch_mcp only.

#include <cch/mcp/Protocol.hpp>

#include "support/Json.hpp"

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <string_view>

namespace cch::mcp::detail {

/// The shared transport error of both MCP clients: the summary and detail name
/// the server and the message, and an optional cause is appended to the detail.
[[nodiscard]] inline support::Error transport_error(std::string server, std::string message, std::string cause = {}) {
    std::string summary = "MCP server '" + std::move(server) + "' " + std::move(message);
    std::string detail = summary;
    if (!cause.empty()) {
        detail += ": " + std::move(cause);
    }
    return support::make_error(support::ErrorCode::Process, std::move(summary), std::move(detail));
}

/// One compact JSON-RPC request body (no framing delimiter). The caller adds
/// its transport's delimiter (`\n` for stdio, none for an HTTP body).
[[nodiscard]] inline std::string build_request_body(
        int id, std::string_view method, const std::optional<support::JsonValue>& params) {
    support::JsonValue request{support::JsonValue::object_t{
            {"jsonrpc", "2.0"},
            {"id", static_cast<double>(id)},
            {"method", std::string{method}},
    }};
    if (params) {
        request.get_object().emplace("params", *params);
    }
    auto serialized = support::write_json(request);
    return serialized ? std::move(*serialized) : std::string{"{}"};
}

/// pi `requestInternal`: a request that observes progress carries
/// `_meta.progressToken` in its params — the request id doubles as the token —
/// merging any `_meta` the caller already set, exactly like pi.
[[nodiscard]] inline support::JsonValue with_progress_token(std::optional<support::JsonValue> params, int id) {
    support::JsonValue::object_t meta{{"progressToken", static_cast<double>(id)}};
    support::JsonValue::object_t merged;
    if (params) {
        if (const auto* object = params->get_if<support::JsonValue::object_t>()) {
            merged = *object;
            if (const auto existing = object->find("_meta");
                    existing != object->end() && existing->second.get_if<support::JsonValue::object_t>() != nullptr) {
                meta = existing->second.get_object();
                meta["progressToken"] = static_cast<double>(id);
            }
        }
    }
    merged["_meta"] = support::JsonValue{std::move(meta)};
    return support::JsonValue{std::move(merged)};
}

/// The numeric progress token of one incoming `notifications/progress`, or
/// nullopt for anything else. pi `handleProgress` accepts only a notification
/// whose params carry a JSON-RPC-id token and a numeric `progress`; Pike's
/// tokens are the numeric request ids, so a string token never matches.
[[nodiscard]] inline std::optional<double> progress_token_of(const support::JsonValue& message) {
    const auto* object = message.get_if<support::JsonValue::object_t>();
    if (object == nullptr) {
        return std::nullopt;
    }
    const auto method = object->find("method");
    if (method == object->end() || !method->second.holds<std::string>() ||
            method->second.get_string() != "notifications/progress") {
        return std::nullopt;
    }
    const auto params = object->find("params");
    if (params == object->end()) {
        return std::nullopt;
    }
    const auto* params_object = params->second.get_if<support::JsonValue::object_t>();
    if (params_object == nullptr) {
        return std::nullopt;
    }
    const auto token = params_object->find("progressToken");
    const auto progress = params_object->find("progress");
    if (token == params_object->end() || !token->second.holds<double>() || progress == params_object->end() ||
            !progress->second.holds<double>()) {
        return std::nullopt;
    }
    return token->second.get_number();
}

/// One compact JSON-RPC notification body (no `id`, no response).
[[nodiscard]] inline std::string build_notification_body(
        std::string_view method, const std::optional<support::JsonValue>& params) {
    support::JsonValue notification{support::JsonValue::object_t{
            {"jsonrpc", "2.0"},
            {"method", std::string{method}},
    }};
    if (params) {
        notification.get_object().emplace("params", *params);
    }
    auto serialized = support::write_json(notification);
    return serialized ? std::move(*serialized) : std::string{"{}"};
}

[[nodiscard]] inline std::string json_or_empty(const support::JsonValue& value) {
    auto serialized = support::write_json(value);
    return serialized ? std::move(*serialized) : std::string{"<unserializable>"};
}

/// Map a JSON-RPC `error` member onto the shared error channel: the message
/// carries the server's `message`, the detail its `code` and message.
[[nodiscard]] inline support::Error json_rpc_error(const support::JsonValue& error) {
    std::string message = "returned a JSON-RPC error";
    std::string detail = json_or_empty(error);
    if (const auto* object = error.get_if<support::JsonValue::object_t>()) {
        if (const auto it = object->find("message"); it != object->end() && it->second.holds<std::string>()) {
            message = it->second.get_string();
        }
        if (const auto it = object->find("code"); it != object->end() && it->second.holds<double>()) {
            detail = "code " + std::to_string(static_cast<long long>(it->second.get_number()));
            if (const auto message_it = object->find("message");
                    message_it != object->end() && message_it->second.holds<std::string>()) {
                detail += ": " + message_it->second.get_string();
            }
        }
    }
    return support::make_error(support::ErrorCode::Process, std::move(message), std::move(detail));
}

/// The error a pending request fails with when its caller's stop token is
/// requested (ADR 0020). The detail is what the Agent's tool-failure text
/// reads, so it names the server and the cancellation. Both transports share
/// it so a cancellation reads the same regardless of wire.
[[nodiscard]] inline support::Error cancelled_error(const std::string& server) {
    return support::make_error(support::ErrorCode::Cancelled,
            "MCP server '" + server + "' request cancelled",
            "MCP server '" + server + "' request cancelled before the server responded");
}

/// The error a pending request fails with when no response arrives within the
/// configured request timeout (pi `McpTimeoutError`), so a hung server fails
/// the call explicitly instead of blocking the session.
[[nodiscard]] inline support::Error timeout_error(const std::string& server, std::chrono::milliseconds timeout) {
    return support::make_error(support::ErrorCode::Timeout,
            "MCP server '" + server + "' request timed out",
            "MCP server '" + server + "' did not respond within " + std::to_string(timeout.count()) + " ms");
}

/// Interpret one JSON-RPC message as the response to `id`. Returns the
/// response `result`; a JSON-RPC `error` becomes the shared error channel.
/// A message that is not a `2.0` object, carries no numeric `id`, or answers a
/// different request is not a response to `id` (`std::nullopt`), so the caller
/// keeps waiting for its own response.
[[nodiscard]] inline support::Expected<std::optional<support::JsonValue>> response_for_id(
        const support::JsonValue& message, int id) {
    const auto* object = message.get_if<support::JsonValue::object_t>();
    if (object == nullptr) {
        return std::optional<support::JsonValue>{};
    }
    const auto version = object->find("jsonrpc");
    if (version == object->end() || !version->second.holds<std::string>() || version->second.get_string() != "2.0") {
        return std::optional<support::JsonValue>{};
    }
    const auto response_id = object->find("id");
    if (response_id == object->end() || !response_id->second.holds<double>() ||
            static_cast<int>(response_id->second.get_number()) != id) {
        return std::optional<support::JsonValue>{};
    }
    if (const auto error = object->find("error"); error != object->end()) {
        return std::unexpected(json_rpc_error(error->second));
    }
    const auto result = object->find("result");
    if (result == object->end()) {
        return std::unexpected(
                support::make_error(support::ErrorCode::Process, "MCP response has neither result nor error"));
    }
    return std::optional<support::JsonValue>{result->second};
}

} // namespace cch::mcp::detail
