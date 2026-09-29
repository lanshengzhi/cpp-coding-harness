#pragma once

#include <cch/support/Error.hpp>
#include <cch/support/JsonValue.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cch::mcp::jsonrpc {

/// One framed JSON-RPC 2.0 message.
///
/// `cch::support::JsonValue` has no integer alternative, so a request id is
/// carried as the `double` the JSON number decodes to. The client stack only
/// ever mints integral ids and only accepts an integral id back, so no
/// precision claim is made: an id above 2^53 is a protocol violation and is
/// rejected rather than compared approximately.
struct WireMessage {
    /// Absent on a notification; present on a request and on a response.
    std::optional<double> id{};
    std::string method{};
    support::JsonValue params{};
    bool is_error{false};
    int error_code{0};
    std::string error_message{};
    support::JsonValue error_data{};
    support::JsonValue result{};
    bool has_result{false};

    /// A server-initiated message this build does not act on (ADR 0064: v1
    /// ignores notifications, including `tools/list_changed`).
    [[nodiscard]] bool is_notification() const noexcept { return !id.has_value() && !method.empty(); }
};

[[nodiscard]] cch::support::JsonValue encode_request(
        double id, std::string_view method, cch::support::JsonValue params);

/// Encode a client notification: a method with no `id`, and therefore no
/// response. The client stack writes exactly one — `notifications/cancelled` —
/// so the Upstream is told to stop work the closed response stream abandoned.
[[nodiscard]] cch::support::JsonValue encode_notification(
        std::string_view method, cch::support::JsonValue params);

[[nodiscard]] cch::support::JsonValue encode_result(double id, cch::support::JsonValue result);

[[nodiscard]] cch::support::JsonValue encode_error(
        double id, int code, std::string message, cch::support::JsonValue data);

/// Decode one JSON-RPC message. A message that is not an object, that is not
/// `2.0`, or that carries neither a well-formed response envelope nor a
/// method is a protocol violation and fails the decode.
[[nodiscard]] cch::support::Expected<WireMessage> decode_message(const cch::support::JsonValue& value);

/// Split one transport body into the JSON-RPC messages it carries, in order.
/// A Streamable HTTP response may place a server notification ahead of the
/// response it belongs to, so framing must be positional rather than
/// whole-body. Whitespace and JSON-text between messages is ignored; an
/// unbalanced or truncated frame is a protocol violation.
[[nodiscard]] cch::support::Expected<std::vector<cch::support::JsonValue>> split_messages(std::string_view body);

} // namespace cch::mcp::jsonrpc
