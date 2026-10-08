#pragma once

// The production adapters for the live in-session MCP manager (spec #882,
// ticket #884): the bridge between the manager's injected seams
// (`McpConnectionFactory` / `McpToolSurface` / `McpSignInDriver`,
// `McpSessionManager.hpp`) and the landed machinery — the stdio/streamable-
// http clients, the OAuth sign-in flow + `mcp-auth.json` store, and the
// Agent's live tool-surface seam (commit 4d8312de8). SessionFactory
// constructs these; tests inject scripted seams instead and never touch this
// file's types.

#include "coding_agent/runtime/McpSessionManager.hpp"

#include <cch/agent/Agent.hpp>
#include <cch/support/AsyncResult.hpp>
#include <cch/support/JsonValue.hpp>

#include <boost/asio/awaitable.hpp>

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace cch::coding_agent::mcp {
class McpAuthStore;
class McpServerConnection;
} // namespace cch::coding_agent::mcp

namespace cch::coding_agent::runtime {

/// One freshly connected transport plus what the connection keeps that the
/// transport-independent `McpServerConnection` seam does not carry: the
/// `initialize` result's `instructions` (only the concrete clients retain
/// them).
struct ConnectedMcpTransport {
    std::shared_ptr<mcp::McpServerConnection> client;
    std::optional<std::string> instructions;
};

/// The production `McpLiveConnection` over one landed transport client: pi
/// runtime's `McpServerConnection` (state vocabulary, the listed tools, the
/// connected-time resource snapshot, the OAuth challenge state). The
/// transport, the tool listing, and the resource snapshot arrive from the
/// factory at connect; `reconnect()` re-runs the factory's connect source (pi
/// `connection.reconnect()`: stdio restarts the process, HTTP reconnects and
/// refreshes `tools/list`).
class ProductionMcpConnection final : public McpLiveConnection {
public:
    /// The factory's connect operation: one fresh transport with handshake.
    using ConnectSource = std::function<boost::asio::awaitable<support::Expected<ConnectedMcpTransport>>()>;

    /// `connected` carries the fresh transport, when the connect succeeded;
    /// the connection starts in `Connecting` and the factory runs `refresh()`
    /// (pi runtime `getClient()`: `tools/list` + the resource snapshot) before
    /// handing it to the manager.
    ProductionMcpConnection(std::string server_name,
            ConnectSource connect_source,
            ConnectedMcpTransport connected,
            bool oauth_eligible,
            std::string oauth_url);

    /// Record a connect-time failure: an OAuth challenge becomes `NeedsAuth`
    /// (the panel offers the sign-in action), anything else `Failed` carrying
    /// the error. The connection stays live enough to retry via `reconnect()`.
    void note_connect_error(support::Error error) noexcept;

    [[nodiscard]] const std::string& server_name() const noexcept override { return server_name_; }
    [[nodiscard]] McpServerState state() const noexcept override { return state_; }
    [[nodiscard]] const std::vector<McpLiveTool>& tools() const noexcept override { return tools_; }
    [[nodiscard]] bool has_resources() const noexcept override { return has_resources_; }
    [[nodiscard]] std::size_t resource_count() const noexcept override { return resource_count_; }
    [[nodiscard]] const std::optional<std::string>& error() const noexcept override { return error_; }
    [[nodiscard]] const std::optional<std::string>& stderr_tail() const noexcept override { return stderr_tail_; }
    [[nodiscard]] bool uses_oauth() const noexcept override { return oauth_eligible_; }
    [[nodiscard]] const std::string& oauth_url() const noexcept override { return oauth_url_; }
    [[nodiscard]] const std::optional<std::string>& instructions() const noexcept override { return instructions_; }

    [[nodiscard]] support::AsyncResult<support::JsonValue> call_tool(
            std::string_view tool, support::JsonValue arguments, std::stop_token stop_token) override;
    [[nodiscard]] support::AsyncResult<support::JsonValue> resources_page(
            std::optional<std::string> cursor, std::stop_token stop_token) override;
    [[nodiscard]] support::AsyncResult<support::JsonValue> resource_templates_page(
            std::optional<std::string> cursor, std::stop_token stop_token) override;
    [[nodiscard]] support::AsyncResult<support::JsonValue> read_resource(
            std::string uri, std::stop_token stop_token) override;

    /// pi `connection.reconnect()`: fresh transport through the connect
    /// source, then `tools/list` and the resource snapshot again. Also the
    /// factory's post-connect step, so both paths share one implementation.
    [[nodiscard]] support::AsyncResult<void> refresh();

    [[nodiscard]] support::AsyncResult<void> reconnect() override;
    [[nodiscard]] support::AsyncResult<void> sign_out() override;
    void close() noexcept override;

    void set_tools_changed_listener(ToolsChangedListener listener) override;
    void set_resources_changed_listener(ResourcesChangedListener listener) override;

private:
    [[nodiscard]] support::AsyncResult<support::JsonValue> request(
            std::string method, std::optional<support::JsonValue> params, std::stop_token stop_token);

    std::string server_name_;
    ConnectSource connect_source_;
    std::shared_ptr<mcp::McpServerConnection> client_;
    McpServerState state_{McpServerState::Connected};
    std::vector<McpLiveTool> tools_;
    bool has_resources_{false};
    std::size_t resource_count_{0};
    std::optional<std::string> error_;
    std::optional<std::string> stderr_tail_;
    bool oauth_eligible_{false};
    std::string oauth_url_;
    std::optional<std::string> instructions_;
    ToolsChangedListener tools_changed_;
    ResourcesChangedListener resources_changed_;
};

/// The production connection factory over the landed stdio/streamable-http
/// clients (pi `createConnection` + `createDefaultTransport`). The URL gate is
/// TLS-only (ADR 0054); an OAuth-eligible HTTP entry resolves its request-time
/// token from `<agentDir>/mcp-auth.json` (migrating a legacy `auth.json`
/// `mcp__<server>` record on first need, pi shape). The result is never a
/// fatal error: the connection carries `Connected`, `NeedsAuth` (an OAuth
/// challenge), or `Failed` (the error), so one dead server never vetoes the
/// session.
class ProductionMcpConnectionFactory final : public McpConnectionFactory {
public:
    explicit ProductionMcpConnectionFactory(std::filesystem::path agent_dir);

    [[nodiscard]] support::AsyncResult<std::shared_ptr<McpLiveConnection>> connect(
            const mcp::McpConfigEntry& entry) override;

private:
    [[nodiscard]] std::shared_ptr<mcp::McpAuthStore> auth_store() const;

    std::filesystem::path agent_dir_;
    /// Resolved lazily so a session that never touches OAuth pays nothing.
    mutable std::shared_ptr<mcp::McpAuthStore> auth_store_;
};

/// The production sign-in driver (pi `signInMcpServer` + `credentials.remove`
/// over `mcp-auth.json`): the interactive flow runs through the loopback
/// callback with the pasted-redirect-URL fallback; sign-out removes the
/// server's stored credential.
class McpOAuthFlowSignInDriver final : public McpSignInDriver {
public:
    explicit McpOAuthFlowSignInDriver(std::filesystem::path agent_dir);

    [[nodiscard]] support::AsyncResult<std::optional<std::string>> sign_in(
            const mcp::McpConfigEntry& entry, const McpSignInPrompt& prompt) override;
    [[nodiscard]] support::AsyncResult<bool> sign_out(const mcp::McpConfigEntry& entry) override;

private:
    std::filesystem::path agent_dir_;
    mutable std::shared_ptr<mcp::McpAuthStore> auth_store_;
};

/// The session tool surface over the live Agent (pi `pi.registerTool` /
/// `pi.setActiveTools` / `pi.getAllTools`): manager registrations become
/// Agent tools with the same conversion as assembly-time extension tools, a
/// `direct` tool joins the declared set on registration (pi
/// `_isActivatedOnRegistration`), a re-registration at a non-direct exposure
/// leaves the declared set, and the three resource tools register through the
/// same path. Exposure bookkeeping mirrors pi's `definitions` map so the
/// manager's withdrawal-as-hidden and exposure changes see the last
/// registration.
class AgentMcpToolSurface final : public McpToolSurface {
public:
    /// `registered_names` seeds the pi `getAllTools()` view with the tools
    /// the assembly registered before the manager existed (built-ins,
    /// codemode, programmatic servers): the Agent has no registered-tool
    /// listing, and the discovery-activation check must see codemode.
    explicit AgentMcpToolSurface(agent::Agent& agent, std::vector<std::string> registered_names);

    void register_tool(McpRegisteredTool tool) override;
    void register_resource_tools(
            mcp::McpExposure exposure, std::vector<std::shared_ptr<mcp::McpResourceServer>> servers) override;
    void set_active_tools(std::vector<std::string> names) override;
    [[nodiscard]] std::vector<std::string> active_tools() const override;
    [[nodiscard]] std::vector<McpSurfaceTool> all_tools() const override;

private:
    /// pi `_isActivatedOnRegistration`: a direct tool enters the declared set;
    /// a non-direct re-registration of a declared tool leaves it.
    void apply_exposure(const std::string& name, mcp::McpExposure exposure);

    agent::Agent& agent_;
    /// Every tool name known on the surface: the assembly-time registrations
    /// plus everything registered through this surface.
    std::set<std::string> known_names_;
    /// pi `definitions`: the last exposure under each registered name.
    std::map<std::string, mcp::McpExposure, std::less<>> exposures_;
};

} // namespace cch::coding_agent::runtime
