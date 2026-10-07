#pragma once

// The LIVE in-session MCP server manager (spec #882, ticket #884): pi's
// session-level manager (`packages/coding-agent/src/extensions/mcp/index.ts`
// `createMcpExtension` + `runtime.ts` `McpServerConnection`) at `7c10bd43`
// (v1.0.4), placed next to SessionFactory's MCP assembly.
//
// The manager owns one live connection per enabled server, reports pi's
// `ServerState` vocabulary, and drives the `/mcp` actions (sign-in, sign-out,
// reconnect, enable/disable, exposure) into explicit outcomes the panel can
// re-present. Re-registration, withdrawal-as-hidden, and the resource tools'
// widest non-hidden exposure follow pi's `registerTools` / `hideTools` /
// `syncResourceTools` semantics.
//
// The transports, the session tool surface, and the OAuth flow are injected
// seams, so the manager is testable with scripted fakes and holds no terminal,
// process, or network machinery of its own.

#include "coding_agent/mcp/McpConfigFile.hpp"
#include "coding_agent/mcp/McpConfigWrite.hpp"
#include "coding_agent/mcp/McpExposure.hpp"
#include "coding_agent/mcp/McpResourceTools.hpp"

#include <cch/support/AsyncResult.hpp>
#include <cch/support/Error.hpp>
#include <cch/support/JsonValue.hpp>

#include <boost/asio/awaitable.hpp>

#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace cch::coding_agent::runtime {

/// pi `ServerState` (`extensions/mcp/runtime.ts`): one connection's lifecycle.
/// `Disconnected` is a dropped connection that reconnects lazily; `Closed` is
/// the shutdown state.
enum class McpServerState {
    Connecting,
    Connected,
    Disconnected,
    NeedsAuth,
    Failed,
    Closed,
};

/// pi's `ServerState` spelling (`connecting`, `needs-auth`, ...).
[[nodiscard]] std::string_view mcp_server_state_name(McpServerState state);

/// pi `Tool` as the manager reads it: the server-side name, the description
/// the tools list renders, and the input/output schemas the surface declares.
struct McpLiveTool {
    std::string name;
    std::string description;
    /// pi `Tool.inputSchema` (a JSON Schema object).
    support::JsonValue input_schema{};
    /// pi `Tool.outputSchema`, when the server declared one.
    std::optional<support::JsonValue> output_schema{std::nullopt};
};

/// One live connection (pi `McpServerConnection`). The manager owns one per
/// enabled server. A production adapter wraps the landed stdio/streamable-http
/// clients; tests inject a scripted one.
class McpLiveConnection {
public:
    virtual ~McpLiveConnection() = default;

    [[nodiscard]] virtual const std::string& server_name() const noexcept = 0;
    [[nodiscard]] virtual McpServerState state() const noexcept = 0;
    /// The tools the server currently offers, in server order.
    [[nodiscard]] virtual const std::vector<McpLiveTool>& tools() const noexcept = 0;
    /// pi `connection.hasResources`: the server advertised the resources
    /// capability at connect time.
    [[nodiscard]] virtual bool has_resources() const noexcept = 0;
    /// pi `connection.resources.length`.
    [[nodiscard]] virtual std::size_t resource_count() const noexcept = 0;
    /// pi `connection.error`; absent when there is none.
    [[nodiscard]] virtual const std::optional<std::string>& error() const noexcept = 0;
    /// pi `connection.stderrTail`: the last `2000` chars of a failed stdio
    /// server's stderr, surfaced with the failure.
    [[nodiscard]] virtual const std::optional<std::string>& stderr_tail() const noexcept = 0;
    /// pi `connection.oauthUrl !== undefined`: the server can sign in.
    [[nodiscard]] virtual bool uses_oauth() const noexcept = 0;
    /// pi `connection.oauthUrl`.
    [[nodiscard]] virtual const std::string& oauth_url() const noexcept = 0;
    /// pi `initialize` result `instructions`, when the server sent them: the
    /// `mcp_servers` prompt section's summary fallback.
    [[nodiscard]] virtual const std::optional<std::string>& instructions() const noexcept = 0;

    /// pi `client.callTool`: one `tools/call` for `tool` on this connection.
    [[nodiscard]] virtual support::AsyncResult<support::JsonValue> call_tool(
            std::string_view tool, support::JsonValue arguments, std::stop_token stop_token) = 0;
    /// pi `client.listResourcesPage`: one `resources/list` page.
    [[nodiscard]] virtual support::AsyncResult<support::JsonValue> resources_page(
            std::optional<std::string> cursor, std::stop_token stop_token) = 0;
    /// pi `client.listResourceTemplatesPage`: one `resources/templates/list` page.
    [[nodiscard]] virtual support::AsyncResult<support::JsonValue> resource_templates_page(
            std::optional<std::string> cursor, std::stop_token stop_token) = 0;
    /// pi `client.readResource`: one `resources/read` for `uri`.
    [[nodiscard]] virtual support::AsyncResult<support::JsonValue> read_resource(
            std::string uri, std::stop_token stop_token) = 0;

    /// pi `connection.reconnect()`: stdio restarts the whole process; HTTP
    /// reconnects and refreshes `tools/list`.
    [[nodiscard]] virtual support::AsyncResult<void> reconnect() = 0;
    /// pi `connection.signOut()`: drop the live client and mark needs-auth.
    [[nodiscard]] virtual support::AsyncResult<void> sign_out() = 0;
    virtual void close() noexcept = 0;

    /// pi runtime `notifications/tools/list_changed` -> re-list: the connection
    /// owns the re-list and calls this when its `tools()` changed.
    using ToolsChangedListener = std::function<void()>;
    /// pi runtime `notifications/resources/list_changed` -> re-list; the
    /// resource lane left `McpResourceChangeListener` as the consumer shape.
    using ResourcesChangedListener = std::function<void()>;
    virtual void set_tools_changed_listener(ToolsChangedListener listener) = 0;
    virtual void set_resources_changed_listener(ResourcesChangedListener listener) = 0;
};

/// pi runtime's per-connection notification dispatch: route a server-to-client
/// notification to the connected server's re-list listeners. The production
/// HTTP adapter installs `dispatch` on `McpHttpClient::set_notification_listener`
/// (and its resource changes arrive through the resource lane's
/// `McpResourceChangeListener`, which this also satisfies), so
/// `notifications/tools/list_changed` and `notifications/resources/list_changed`
/// reach the manager through one seam.
class McpLiveNotificationRouter final : public mcp::McpResourceChangeListener {
public:
    McpLiveNotificationRouter(
            McpLiveConnection::ToolsChangedListener on_tools, McpLiveConnection::ResourcesChangedListener on_resources);

    /// pi runtime's `notifications/message` switch: a `tools/list_changed`
    /// re-lists and notifies `on_tools`; a `resources/list_changed` re-lists
    /// and notifies `on_resources`. Every other method is ignored.
    void dispatch(std::string_view method, const support::JsonValue& params);

    /// pi `McpResourceChangeListener` consumer shape: the resource lane's
    /// re-list hook routes to the same resources listener.
    void on_resources_changed() override;

private:
    McpLiveConnection::ToolsChangedListener on_tools_;
    McpLiveConnection::ResourcesChangedListener on_resources_;
};

/// The connection factory the manager drives (pi `createConnection`). The
/// production implementation reads the entry's transport and connects it; tests
/// inject a scripted one.
class McpConnectionFactory {
public:
    virtual ~McpConnectionFactory() = default;
    /// Connect one enabled entry and list its tools. Completes with the live
    /// connection whose `state()` is `Connected`, `NeedsAuth`, or `Failed`; a
    /// launch/handshake/`tools/list` failure is a `Failed` connection carrying
    /// the error and, for stdio, the stderr tail.
    [[nodiscard]] virtual support::AsyncResult<std::shared_ptr<McpLiveConnection>> connect(
            const mcp::McpConfigEntry& entry) = 0;
};

/// pi `client.callTool`: one `tools/call` execution on a server's live
/// connection, bound by the manager at registration. The surface turns it into
/// the tool's execute operation; it stays bound when a withdrawn tool
/// re-registers as hidden, exactly like pi's definition map.
using McpToolCall = std::function<support::AsyncResult<support::JsonValue>(
        support::JsonValue arguments, std::stop_token stop_token)>;

/// One tool the manager registers on the session tool surface (pi
/// `ToolDefinition` narrowed to the exposure decision, plus the schemas and
/// the call execution the declaration needs).
struct McpRegisteredTool {
    std::string server;
    std::string server_tool_name;
    std::string name;
    std::string description;
    mcp::McpExposure exposure{mcp::McpExposure::Codemode};
    /// pi `Tool.inputSchema`: the JSON Schema the server advertised.
    support::JsonValue input_schema;
    /// pi `Tool.outputSchema`, when the server declared one.
    std::optional<support::JsonValue> output_schema;
    /// pi `client.callTool`: the runnable `tools/call` execution.
    McpToolCall call;
};

/// One tool already on the surface, for pi's `pi.getAllTools()`.
struct McpSurfaceTool {
    std::string name;
    mcp::McpExposure exposure{mcp::McpExposure::Codemode};
};

/// The session tool surface the manager re-registers tools on (pi
/// `pi.registerTool` / `pi.setActiveTools` / `pi.getAllTools`). A production
/// adapter writes into the session's live registry; tests record calls.
class McpToolSurface {
public:
    virtual ~McpToolSurface() = default;
    /// Register or replace `tool.name` at `tool.exposure`. A withdrawn tool is
    /// re-registered with `exposure == hidden` (pi semantics: tools cannot be
    /// unregistered). A `direct` tool joins the declared set on registration
    /// (pi `_isActivatedOnRegistration`), which is the surface's responsibility
    /// so the manager never needs to echo it back with `set_active_tools`.
    virtual void register_tool(McpRegisteredTool tool) = 0;
    /// Register the three Codex-compatible resource tools at `exposure`,
    /// reaching `servers` (pi `createMcpResourceToolDefinitions`).
    virtual void register_resource_tools(
            mcp::McpExposure exposure, std::vector<std::shared_ptr<mcp::McpResourceServer>> servers) = 0;
    /// Replace the active (declared) tool set (pi `pi.setActiveTools`).
    virtual void set_active_tools(std::vector<std::string> names) = 0;
    [[nodiscard]] virtual std::vector<std::string> active_tools() const = 0;
    /// Every tool currently on the surface with its exposure (pi
    /// `pi.getAllTools`).
    [[nodiscard]] virtual std::vector<McpSurfaceTool> all_tools() const = 0;
};

/// pi's sign-in presentation: the authorization URL to show/open, and the
/// pasted redirect URL when the browser runs elsewhere.
struct McpSignInPrompt {
    std::function<void(std::string_view authorization_url)> show_authorization_url;
    std::function<boost::asio::awaitable<std::optional<std::string>>()> prompt_for_redirect_url;
};

/// The OAuth sign-in seam (pi `signInMcpServer` + `McpOAuthCredentialStore`).
/// The production implementation runs `McpOAuthFlow` against `McpAuthStore`;
/// tests inject a scripted one.
class McpSignInDriver {
public:
    virtual ~McpSignInDriver() = default;
    /// Run the flow. Completes with the failure message, or `std::nullopt` on
    /// success (pi `signIn`'s `string | undefined`).
    [[nodiscard]] virtual support::AsyncResult<std::optional<std::string>> sign_in(
            const mcp::McpConfigEntry& entry, const McpSignInPrompt& prompt) = 0;
    /// Remove the server's stored credentials (pi `credentials.remove`).
    /// Returns whether any credential was stored.
    [[nodiscard]] virtual support::AsyncResult<bool> sign_out(const mcp::McpConfigEntry& entry) = 0;
};

/// One snapshot of a connection as the panel reads it (pi `connection`).
struct McpConnectionSnapshot {
    McpServerState state{McpServerState::Connecting};
    std::vector<McpLiveTool> tools;
    bool has_resources{false};
    std::size_t resource_count{0};
    std::optional<std::string> error;
    std::optional<std::string> stderr_tail;
    bool oauth{false};
    /// pi `connection`'s `instructions`: the initialize result's instructions.
    std::optional<std::string> instructions;
};

/// One configured server plus its live connection and last action message (pi
/// `McpServer`).
struct McpServerSnapshot {
    mcp::McpConfigEntry entry;
    std::optional<std::string> scope;
    std::optional<McpConnectionSnapshot> connection;
    std::optional<std::string> message;
};

/// The manager's injected seams. `connections`, `tools`, and `in_prompt` are
/// required; a null `auth` disables the sign-in actions explicitly.
struct McpManagerDependencies {
    std::shared_ptr<McpConnectionFactory> connections;
    std::shared_ptr<McpToolSurface> tools;
    std::shared_ptr<McpSignInDriver> auth;
    /// pi `ctx.ui.notify(..., "warning")` for the discovery-reachability
    /// warning (`ensureDiscoveryActive`). Optional: the warning is always
    /// recorded on the manager (`warnings()`), so a host without a UI seam
    /// still surfaces it through the panel or the session.
    std::function<void(std::string_view message)> notify_warning;
};

/// The session-level MCP server manager (pi `createMcpExtension`'s `servers`
/// list and its `/mcp` manager half).
class McpSessionManager {
public:
    /// Build a manager over an already-loaded `mcp.json` (pi `session_start`).
    /// `overridden` are the registered-server override notices (pi
    /// `overridden()`); `agent_dir` names the global `mcp.json` for the empty
    /// message. The project's trust decision is already applied by
    /// `load_mcp_config`: a trusted project contributes its `mcp.json` entries
    /// and `projectConfig`.
    McpSessionManager(mcp::McpConfigLoad config,
            std::filesystem::path agent_dir,
            std::vector<std::string> overridden,
            McpManagerDependencies dependencies);

    McpSessionManager(McpSessionManager&&) = delete;
    McpSessionManager& operator=(McpSessionManager&&) = delete;
    ~McpSessionManager();
    McpSessionManager(const McpSessionManager&) = delete;
    McpSessionManager& operator=(const McpSessionManager&) = delete;

    /// Bind the session's live tool surface (pi's `pi` ExtensionAPI). The
    /// session constructs the manager before the Agent exists, so the surface
    /// arrives here; it must be attached before `start()`.
    void attach_tool_surface(std::shared_ptr<McpToolSurface> surface);

    /// Connect every enabled server (pi `session_start`'s background connect).
    /// Each connection's state reflects the outcome; a failed server does not
    /// veto the others.
    [[nodiscard]] support::AsyncResult<void> start();

    /// pi `session_end`: close every live connection. Idempotent.
    void close() noexcept;

    /// The servers as the panel reads them, in configuration order.
    [[nodiscard]] std::vector<McpServerSnapshot> servers() const;
    [[nodiscard]] const std::vector<std::string>& config_errors() const noexcept { return config_errors_; }
    [[nodiscard]] const std::vector<std::string>& overridden() const noexcept { return overridden_; }
    /// pi `warnedUnreachable`'s texts: the verbatim discovery-reachability
    /// warning, recorded at most once for the manager's lifetime (pi latches
    /// per session).
    [[nodiscard]] const std::vector<std::string>& warnings() const noexcept { return warnings_; }
    [[nodiscard]] const std::filesystem::path& agent_dir() const noexcept { return agent_dir_; }
    /// pi `projectConfig !== undefined`: a trusted project's `mcp.json` exists,
    /// so a global server can be enabled or disabled for the project alone.
    [[nodiscard]] bool project_config() const noexcept { return project_config_.has_value(); }

    /// Subscribe to manager changes (pi `subscribe`): a connection state change,
    /// a re-registration, or an action completion. Fired with the new servers.
    void set_change_listener(std::function<void()> listener);

    // ── Actions (pi `runAction`) ────────────────────────────────────────────
    // Each completes with the failure message to show in `server.message`, or
    // `std::nullopt` on success.

    /// pi `signInWithUi`: run the flow, then reconnect. A server that does not
    /// use OAuth reports pi's exact message.
    [[nodiscard]] support::AsyncResult<std::optional<std::string>> sign_in(
            std::string_view server, McpSignInPrompt prompt);
    /// pi `signOut`: remove credentials and sign the connection out. Completes
    /// with whether any credential was removed.
    [[nodiscard]] support::AsyncResult<bool> sign_out(std::string_view server);
    /// pi `reconnect`: an unknown/disabled server reports pi's message; a
    /// failed reconnect reports its error.
    [[nodiscard]] support::AsyncResult<std::optional<std::string>> reconnect(std::string_view server);
    /// pi `setEnabled`: write the `enabled` half and connect or disconnect.
    /// `in_project` adds a project override for a global server.
    [[nodiscard]] support::AsyncResult<std::optional<std::string>> set_enabled(
            std::string_view server, bool enabled, bool in_project = false);
    /// pi `setExposure`: write the `exposure` half and re-register the server's
    /// tools and the resource tools.
    [[nodiscard]] support::AsyncResult<std::optional<std::string>> set_exposure(
            std::string_view server, mcp::McpExposure exposure);

    /// pi runtime `registerTools`: (re-)register every tool the connection
    /// offers at its configured exposure, re-register tools the server dropped
    /// as hidden, and sync the resource tools.
    void register_tools(std::string_view server);
    /// pi `hideTools`: make a disabled server's tools unreachable.
    void hide_tools(std::string_view server);
    /// pi `syncResourceTools`: register the resource tools at the widest
    /// non-hidden exposure of the resource-bearing enabled servers, else
    /// hidden.
    void sync_resource_tools();

private:
    /// pi `ensureDiscoveryActive`: activate the discovery tool the configured
    /// exposures ask for — `codemode` for `codemode` exposure unless
    /// `autoEnableCodemode` is false, `tool_search` for `deferred` — and warn
    /// once, with pi's verbatim text, when neither is active. Computed from
    /// the config, so the tool is active before the servers connect.
    void ensure_discovery_active();
    struct ManagedServer {
        mcp::McpConfigEntry entry;
        std::optional<std::string> scope;
        std::shared_ptr<McpLiveConnection> connection;
        std::optional<std::string> message;
    };

    [[nodiscard]] ManagedServer* find(std::string_view server);
    [[nodiscard]] const ManagedServer* find(std::string_view server) const;
    [[nodiscard]] support::AsyncResult<void> start_connection(std::string_view server);
    /// pi `saveConfig`: persist `enabled`/`exposure` through the write half and
    /// update the in-memory entry. Returns pi's failure message on a write
    /// failure.
    [[nodiscard]] std::optional<std::string> save_config(
            ManagedServer& server, mcp::McpServerConfigPatch patch, bool in_project);
    void emit_change();
    [[nodiscard]] std::vector<std::string> resource_bearing_servers() const;

    mcp::McpConfigLoad config_;
    std::filesystem::path agent_dir_;
    std::optional<std::filesystem::path> project_config_;
    std::vector<std::string> config_errors_;
    std::vector<std::string> overridden_;
    std::vector<ManagedServer> servers_;
    McpManagerDependencies dependencies_;
    /// pi `serverTools`: the tool names currently offered by each server.
    std::map<std::string, std::set<std::string>> server_tools_;
    /// pi `definitions`: the last registration under each name, so a withdrawn
    /// tool re-registers as hidden.
    std::map<std::string, McpRegisteredTool> definitions_;
    /// pi `resourceToolsExposure`: the exposure the resource tools were last
    /// registered with; unset until a server has resources.
    std::optional<mcp::McpExposure> resource_tools_exposure_;
    /// pi `warnedUnreachable`: latch so the reachability warning fires once.
    bool warned_unreachable_{false};
    /// The recorded warning texts (see `warnings()`).
    std::vector<std::string> warnings_;
    std::function<void()> change_listener_;
};

} // namespace cch::coding_agent::runtime
