#pragma once

#include "coding_agent/extensions/ExtensionTool.hpp"
#include "coding_agent/extensions/ExtensionToolSource.hpp"
#include "coding_agent/mcp/McpHttpServerConfig.hpp"
#include "coding_agent/mcp/McpRequestAuthSource.hpp"
#include "coding_agent/mcp/McpServerConnection.hpp"
#include "coding_agent/mcp/McpStdioServerConfig.hpp"

#include <cch/support/Error.hpp>
#include <cch/support/JsonValue.hpp>

#include <boost/asio/awaitable.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace cch::coding_agent::mcp {

/// One tool a connected MCP server offers, as `tools/list` reported it.
struct McpToolDescriptor {
    /// The server-side tool name (`tools/call` `params.name`).
    std::string server_tool_name;
    /// The Agent-visible tool name, `mcp__<server>__<tool>` sanitized to
    /// `[A-Za-z0-9_]` (pi `createMcpToolName`).
    std::string full_name;
    std::string description;
    /// The tool's JSON Schema input schema, passed through unchanged.
    support::JsonValue parameters;
};

/// The Agent-visible name of one server tool (pi `createMcpToolName`): a
/// `mcp__<server>__<tool>` identifier with everything outside `[A-Za-z0-9_]`
/// replaced by `_`, so the name is also a valid identifier.
[[nodiscard]] std::string mcp_tool_name(std::string_view server, std::string_view tool);

/// Extension Tool Source backed by one MCP server (spec #865). The one async
/// boundary is the transport factory — `connect_stdio` (ticket #869) or
/// `connect_http` (ticket #873) — which connects the server, runs the MCP
/// `initialize` handshake, and lists the server's tools. The resulting source
/// is synchronous, so the #867 Extension Tool Source seam is unchanged;
/// `load_tools` converts the already-listed descriptors into Agent-visible
/// tools whose execution is `tools/call` on the long-lived connection. The
/// tool surface is identical regardless of transport.
class McpExtensionToolSource final : public extensions::ExtensionToolSource {
public:
    /// Launch `config` over stdio, handshake, and list the tools. A launch
    /// failure, a failed handshake, or an invalid `tools/list` result is
    /// returned as an explicit error — Session Assembly never silently drops a
    /// configured server.
    [[nodiscard]] static boost::asio::awaitable<support::Expected<std::unique_ptr<McpExtensionToolSource>>>
    connect_stdio(McpStdioServerConfig config);

    /// Connect `config` over the streamable HTTP transport, handshake, and
    /// list the tools. The URL is TLS-only (ADR 0054): a non-`https://` URL is
    /// rejected at registration, and a network, status, or handshake failure
    /// is returned as an explicit error. When `request_auth` is present the
    /// request-time headers it resolves (an OAuth access token, spec #865
    /// ticket #875) are attached to every POST; a missing credential or a
    /// failed refresh fails the connection explicitly.
    [[nodiscard]] static boost::asio::awaitable<support::Expected<std::unique_ptr<McpExtensionToolSource>>>
    connect_http(McpHttpServerConfig config, std::shared_ptr<McpRequestAuthSource> request_auth = nullptr);

    McpExtensionToolSource(const McpExtensionToolSource&) = delete;
    McpExtensionToolSource& operator=(const McpExtensionToolSource&) = delete;

    [[nodiscard]] support::Expected<std::vector<extensions::ExtensionTool>> load_tools() override;

    [[nodiscard]] const std::string& server_name() const noexcept { return server_name_; }
    /// The tools `tools/list` reported, in server order.
    [[nodiscard]] const std::vector<McpToolDescriptor>& tools() const noexcept { return tools_; }

private:
    McpExtensionToolSource(std::shared_ptr<McpServerConnection> connection, std::vector<McpToolDescriptor> tools);

    std::shared_ptr<McpServerConnection> connection_;
    std::string server_name_;
    std::vector<McpToolDescriptor> tools_;
};

} // namespace cch::coding_agent::mcp
