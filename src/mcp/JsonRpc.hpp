#pragma once

#include <cch/support/Error.hpp>
#include <cch/support/JsonValue.hpp>

#include <optional>
#include <span>
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

/// One member whose value is written to the wire as already-serialized JSON
/// text rather than through `cch::support::JsonValue`.
///
/// It exists for exactly one value: the opaque `requestState` an Upstream sent
/// with an `input_required` result. Parsing that value into `JsonValue` and
/// re-serializing it would normalize it — object members would be reordered,
/// numbers would be reformatted, escapes rewritten — so a server whose
/// continuation token is compared byte-for-byte would see a different token on
/// the retry. Carrying its source text through unchanged is the whole of
/// "echo the `requestState` verbatim" (spec #833 story 28).
struct RawJsonMember {
    std::string key{};
    std::string json_text{};
};

/// The framed body of one request, with `raw_params` members appended to the
/// `params` object as raw text. With no raw members this is byte-identical to
/// `write_json(encode_request(...))`; with them, the raw members follow every
/// `params` member the value tree contributed, in the order supplied.
[[nodiscard]] cch::support::Expected<std::string> encode_request_body(double id,
        std::string_view method,
        const cch::support::JsonValue& params,
        std::span<const RawJsonMember> raw_params = {});

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

/// The raw JSON text of each message one transport body carries, in order.
/// The views alias `body`, so a caller that keeps one past the body's lifetime
/// must copy it. This is the path by which an opaque member's source text is
/// recovered from the bytes the Upstream actually sent.
[[nodiscard]] cch::support::Expected<std::vector<std::string_view>> split_message_slices(std::string_view body);

/// The raw JSON text of `key`'s value inside the JSON object `object_text`, or
/// `std::nullopt` when `object_text` is not an object or carries no such
/// member. The view aliases `object_text`.
///
/// A member that is absent, that is named by a non-string key, or whose value
/// is not followed by a delimiter before the object's end yields `std::nullopt`
/// rather than a partial span: a member's text is only the member's text when
/// the whole value was framed.
[[nodiscard]] std::optional<std::string_view> object_member_source(std::string_view object_text, std::string_view key);

} // namespace cch::mcp::jsonrpc
