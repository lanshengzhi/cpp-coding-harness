#pragma once

#include <cch/support/AsyncResult.hpp>
#include <cch/support/JsonValue.hpp>

#include <optional>
#include <stop_token>
#include <string>

namespace cch::coding_agent::mcp {

/// One connected MCP server, transport-independent (spec #865, tickets #869
/// stdio / #873 streamable-http): a JSON-RPC `request`/`notify` pair plus the
/// server identity used for diagnostics and tool naming. Both transports
/// implement it, so the Extension Tool Source conversion and the Agent-visible
/// `mcp__<server>__<tool>` surface are identical regardless of transport.
class McpServerConnection {
public:
    virtual ~McpServerConnection() = default;

    /// One JSON-RPC request. Completes with the response `result`, the
    /// server's JSON-RPC error, or a transport error. `stop_token` is the
    /// caller's cancellation source (ADR 0020): a transport that supports
    /// cancellation must resolve the request through it; a transport that is
    /// not yet cancellation-aware must still receive the token so the
    /// capability is explicit at this seam.
    [[nodiscard]] virtual support::AsyncResult<support::JsonValue> request(std::string method,
            std::optional<support::JsonValue> params = std::nullopt,
            std::stop_token stop_token = {}) = 0;

    /// One JSON-RPC notification (no `id`, no response).
    virtual void notify(std::string method, std::optional<support::JsonValue> params = std::nullopt) = 0;

    /// The configured server name (the `mcp__<server>__` namespace).
    [[nodiscard]] virtual const std::string& server_name() const noexcept = 0;
};

} // namespace cch::coding_agent::mcp
