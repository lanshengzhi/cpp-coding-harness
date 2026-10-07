#pragma once

#include "coding_agent/extensions/ExtensionTool.hpp"
#include "coding_agent/mcp/McpNamespace.hpp"
#include "coding_agent/mcp/McpServerConnection.hpp"

#include <cch/support/AsyncResult.hpp>
#include <cch/support/JsonValue.hpp>

#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace cch::coding_agent::mcp {

/// pi `isMcpAppResource`: an MCP App user interface, which only hosts that
/// render it can use. Either a `ui://` URI/URI template or a `profile=mcp-app`
/// mime type.
[[nodiscard]] bool is_mcp_app_resource(const support::JsonValue& item);

/// One connected server the resource tools reach (pi `McpResourceServer`). The
/// request results are the raw `resources/list`, `resources/templates/list`,
/// and `resources/read` payloads, so a caller can supply one directly and the
/// tools own the normalization (missing `name`, `ui://` filtering, `_meta`).
class McpResourceServer {
public:
    virtual ~McpResourceServer() = default;

    [[nodiscard]] virtual const std::string& name() const noexcept = 0;

    /// One `resources/list` page (`{resources, nextCursor?}`).
    [[nodiscard]] virtual support::AsyncResult<support::JsonValue> resources_page(
            std::optional<std::string> cursor, std::stop_token stop_token) = 0;

    /// One `resources/templates/list` page (`{resourceTemplates, nextCursor?}`).
    [[nodiscard]] virtual support::AsyncResult<support::JsonValue> resource_templates_page(
            std::optional<std::string> cursor, std::stop_token stop_token) = 0;

    /// `resources/read` for `uri` (`{contents:[…]}`).
    [[nodiscard]] virtual support::AsyncResult<support::JsonValue> read_resource(
            std::string uri, std::stop_token stop_token) = 0;
};

/// pi `McpServerConnection`'s resource half: the resource request methods of one
/// transport-independent connection. A server that does not implement
/// `resources/templates/list` reports `-32601`; that page reads as an empty
/// template list, matching pi's `withoutTemplates`.
class McpConnectionResourceServer final : public McpResourceServer {
public:
    explicit McpConnectionResourceServer(std::shared_ptr<McpServerConnection> connection);

    [[nodiscard]] const std::string& name() const noexcept override;

    [[nodiscard]] support::AsyncResult<support::JsonValue> resources_page(
            std::optional<std::string> cursor, std::stop_token stop_token) override;
    [[nodiscard]] support::AsyncResult<support::JsonValue> resource_templates_page(
            std::optional<std::string> cursor, std::stop_token stop_token) override;
    [[nodiscard]] support::AsyncResult<support::JsonValue> read_resource(
            std::string uri, std::stop_token stop_token) override;

private:
    std::shared_ptr<McpServerConnection> connection_;
};

/// The connected-time resource snapshot of one server (pi runtime
/// `fetchResources`): every page of its resources and templates, with MCP App
/// resources removed. A list that fails leaves that side empty, so a server
/// whose lists fail still connects and the tools re-list on demand.
struct McpResourceSnapshot {
    bool has_resources{false};
    std::vector<support::JsonValue> resources;
    std::vector<support::JsonValue> resource_templates;
};

[[nodiscard]] support::AsyncResult<McpResourceSnapshot> fetch_mcp_resource_snapshot(
        McpResourceServer& server, std::stop_token stop_token);

/// The dispatch hook a transport calls when the connected server sends
/// `notifications/resources/list_changed` (pi runtime `refreshResources`). The
/// long-lived HTTP channel owns delivery; this seam keeps the re-list one call.
class McpResourceChangeListener {
public:
    virtual ~McpResourceChangeListener() = default;
    virtual void on_resources_changed() = 0;
};

/// The three resource tools (pi `createMcpResourceToolDefinitions`). `servers`
/// is the resource-bearing server set the tools reach at call time.
[[nodiscard]] std::vector<extensions::ExtensionTool> create_mcp_resource_tools(
        std::vector<std::shared_ptr<McpResourceServer>> servers);

} // namespace cch::coding_agent::mcp
