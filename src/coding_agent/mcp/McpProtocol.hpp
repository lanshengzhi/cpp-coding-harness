#pragma once

#include <cch/support/Error.hpp>
#include <cch/support/JsonValue.hpp>

#include "support/Json.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

namespace cch::coding_agent::mcp {

/// The MCP protocol version Pike requests on a connection (pi v1.0.4's
/// default for the shipped transports).
inline constexpr std::string_view kMcpProtocolVersion = "2025-06-18";

/// Client identity sent in the `initialize` handshake.
inline constexpr std::string_view kMcpClientName = "pike";
inline constexpr std::string_view kMcpClientVersion = "0.1.0";

/// Transport-independent JSON-RPC framing and `initialize` validation
/// (spec #865): the stdio and streamable-http clients speak the same MCP
/// protocol, so their request/notification bodies, response error mapping, and
/// handshake checks live here once and only the framing (newline-delimited
/// lines vs. HTTP POST bodies) differs per transport.
namespace detail {

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
/// requested (ADR 0020, ticket #872). The detail is what the Agent's
/// tool-failure text reads, so it names the server and the cancellation. Both
/// transports share it so a cancellation reads the same regardless of wire.
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

/// Validate the `initialize` result like pi `validateInitializeResult`: the
/// protocol version, capabilities object, and serverInfo identity must be
/// present, so a server that cannot speak MCP fails explicitly at connect.
[[nodiscard]] inline support::ExpectedVoid validate_initialize_result(
        const std::string& server, const support::JsonValue& result) {
    const auto* object = result.get_if<support::JsonValue::object_t>();
    if (object == nullptr) {
        return std::unexpected(support::make_error(
                support::ErrorCode::Process, "MCP server '" + server + "' sent an invalid initialize result"));
    }
    const auto protocol_version = object->find("protocolVersion");
    if (protocol_version == object->end() || !protocol_version->second.holds<std::string>()) {
        return std::unexpected(support::make_error(support::ErrorCode::Process,
                "MCP server '" + server + "' initialize result is missing protocolVersion"));
    }
    const auto capabilities = object->find("capabilities");
    if (capabilities == object->end() || capabilities->second.get_if<support::JsonValue::object_t>() == nullptr) {
        return std::unexpected(support::make_error(
                support::ErrorCode::Process, "MCP server '" + server + "' initialize result is missing capabilities"));
    }
    const auto server_info = object->find("serverInfo");
    if (server_info == object->end() || server_info->second.get_if<support::JsonValue::object_t>() == nullptr) {
        return std::unexpected(support::make_error(
                support::ErrorCode::Process, "MCP server '" + server + "' initialize result is missing serverInfo"));
    }
    return {};
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

} // namespace detail

} // namespace cch::coding_agent::mcp
