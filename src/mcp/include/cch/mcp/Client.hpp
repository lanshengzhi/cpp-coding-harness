#pragma once

// Transport-independent MCP client contract: one connected server speaks a
// JSON-RPC `request`/`notify` pair plus the identity used for diagnostics and
// tool naming. Both transports implement it, so a caller sees the same surface
// regardless of wire.

#include <cch/support/AsyncResult.hpp>
#include <cch/support/JsonValue.hpp>

#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

namespace cch::mcp {

/// One `tools/call` result. `value` is the ordinary transport result; the
/// optional text is the exact JavaScript `JSON.stringify(value, null, 2)` of
/// its `structuredContent` member when the transport preserved the wire member
/// order. The text never crosses as raw wire JSON.
struct ToolCallResult {
    support::JsonValue value;
    std::optional<std::string> structured_content_json;
};

struct RequestOptions {
    std::stop_token stop_token{};
    std::move_only_function<void(const support::JsonValue& params)> on_progress{};
};

class Client {
public:
    virtual ~Client() = default;

    /// pi `McpRequestOptions.onProgress`: one `notifications/progress` the
    /// server published for this request. Stored per request, so move-only.
    using ProgressCallback = std::move_only_function<void(const support::JsonValue& params)>;

    /// pi `McpRequestOptions` slice: the caller's cancellation and an optional
    /// progress observer. Supplying `on_progress` makes the client advertise
    /// `_meta.progressToken` on the request and deliver matching
    /// `notifications/progress` messages, re-arming the request deadline per
    /// pi `armTimeout`.
    using RequestOptions = cch::mcp::RequestOptions;

    /// One JSON-RPC request. Completes with the response `result`, the
    /// server's JSON-RPC error, or a transport error. `options.stop_token` is
    /// the caller's cancellation source: a transport that supports
    /// cancellation must resolve the request through it.
    [[nodiscard]] virtual support::AsyncResult<support::JsonValue> request(std::string method,
            std::optional<support::JsonValue> params = std::nullopt,
            RequestOptions options = {}) = 0;

    /// One `tools/call`, carrying the transport's order-preserving
    /// `structuredContent` text when available. The default implementation
    /// delegates to `request` and leaves the text absent, so existing clients
    /// remain valid; the stdio and HTTP transports override it.
    [[nodiscard]] virtual support::AsyncResult<ToolCallResult> call_tool(
            std::string name, support::JsonValue arguments, RequestOptions options = {});

    /// One JSON-RPC notification (no `id`, no response).
    virtual void notify(std::string method, std::optional<support::JsonValue> params = std::nullopt) = 0;

    /// The configured server name (the `mcp__<server>__` namespace).
    [[nodiscard]] virtual const std::string& server_name() const noexcept = 0;

    /// pi `initialize` result `instructions`, when the server sent them.
    [[nodiscard]] virtual const std::optional<std::string>& server_instructions() const noexcept = 0;

    /// One server-to-client notification, delivered on the client's serialized
    /// execution domain even while no request is pending. Close the client to
    /// end this observation and release its transport.
    using NotificationSink = std::move_only_function<void(std::string_view, const support::JsonValue&)>;
    virtual void set_notification_listener(NotificationSink listener) = 0;

    /// The capabilities advertised by the validated initialize response.
    [[nodiscard]] virtual const support::JsonValue& server_capabilities() const noexcept = 0;

    /// Stop the transport and release its resources. Idempotent; a client is
    /// not reusable after close.
    virtual void close() noexcept = 0;
};

} // namespace cch::mcp
