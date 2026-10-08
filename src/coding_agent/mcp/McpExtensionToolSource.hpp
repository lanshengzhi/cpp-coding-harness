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

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
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
    /// The tool's JSON Schema output schema (`tools/list` `outputSchema`), when
    /// the server declares one. The Agent-visible tool declares
    /// `create_mcp_result_schema` around it.
    std::optional<support::JsonValue> output_schema;
};

/// pi `createMcpResultSchema`: the output schema every MCP tool declares — the
/// `CallToolResult` a codemode script receives, with the tool's own output
/// schema nested as `structuredContent`.
[[nodiscard]] support::JsonValue create_mcp_result_schema(
        const std::optional<support::JsonValue>& structured_content_schema);

/// pi `isTool` + `listAll`: list one connected server's tools, following
/// `nextCursor` to exhaustion. An invalid page is an explicit error. Shared by
/// the assembly-time Extension Tool Source and the live session manager's
/// production connection.
[[nodiscard]] boost::asio::awaitable<support::Expected<std::vector<McpToolDescriptor>>> list_mcp_server_tools(
        McpServerConnection& connection);

/// pi `extensions/mcp/tools.ts`' result mapping: convert one `tools/call`
/// result (`{content, structuredContent?, isError?}`) into the extension-side
/// outcome — text blocks to model text, image blocks to model images, any
/// other block to its compact JSON text, so no server content is dropped
/// silently. Shared by the assembly-time conversion and the live tool surface.
[[nodiscard]] support::Expected<extensions::ExtensionToolResult> convert_mcp_tools_call_result(
        const std::string& server, const support::JsonValue& result);

/// The Agent-visible name of one server tool (pi `createMcpToolName`): a
/// `mcp__<server>__<tool>` identifier with everything outside `[A-Za-z0-9_]`
/// replaced by `_`, so the name is also a valid identifier. The plain form
/// stands only while it fits pi's `MAX_TOOL_NAME_LENGTH` (64) bound.
[[nodiscard]] std::string mcp_tool_name(std::string_view server, std::string_view tool);

/// pi `createMcpToolName(server, tool, isTaken)`: when the sanitized name is
/// over the 64-char bound or `is_taken` reports another tool owning it, the
/// name becomes the first 55 characters plus `_<8-hex-sha256>` over the RAW
/// `${server}\0${tool}` pair (not the sanitized name), so colliding tools
/// hash independently of which one kept the plain name.
[[nodiscard]] std::string mcp_tool_name(
        std::string_view server, std::string_view tool, std::move_only_function<bool(const std::string&)> is_taken);

/// pi `index.ts`'s order-independent assignment for one server's whole tool
/// list: `plain` counts the sanitized names, and every tool whose plain name
/// is duplicated (or already owned) takes the hash suffix — so which tool
/// would have kept the plain name never depends on the tools/list order.
/// Returns the assigned names, one per input, in input order.
[[nodiscard]] std::vector<std::string> assign_mcp_tool_names(
        std::string_view server, const std::vector<std::string>& raw_tool_names);

/// The model-visible text of one `notifications/progress` (pi `tools.ts`
/// `onProgress`): the server's `message` when it carries one, else
/// `Progress <n>[/<total>]`, the total omitted when absent.
[[nodiscard]] std::string progress_update_text(const support::JsonValue& params);

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

    /// Construction passkey (§7.7): `std::make_unique` cannot reach a private
    /// constructor, so construction goes through the public constructor below,
    /// whose key only a member of this class can name. `connect_stdio` and
    /// `connect_http` are the sole callers.
    struct ConstructionKey {
    private:
        ConstructionKey() = default;
        friend class McpExtensionToolSource;
    };
    McpExtensionToolSource(
            ConstructionKey, std::shared_ptr<McpServerConnection> connection, std::vector<McpToolDescriptor> tools);

    McpExtensionToolSource(const McpExtensionToolSource&) = delete;
    McpExtensionToolSource& operator=(const McpExtensionToolSource&) = delete;

    [[nodiscard]] support::Expected<std::vector<extensions::ExtensionTool>> load_tools() override;

    [[nodiscard]] const std::string& server_name() const noexcept { return server_name_; }
    /// The tools `tools/list` reported, in server order.
    [[nodiscard]] const std::vector<McpToolDescriptor>& tools() const noexcept { return tools_; }

private:
    std::shared_ptr<McpServerConnection> connection_;
    std::string server_name_;
    std::vector<McpToolDescriptor> tools_;
};

} // namespace cch::coding_agent::mcp
