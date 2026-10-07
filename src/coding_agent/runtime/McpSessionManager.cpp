// The LIVE in-session MCP server manager (spec #882, ticket #884). The
// transport-independent manager logic of pi's `extensions/mcp/index.ts`
// (`createMcpExtension`) and `runtime.ts` (`McpServerConnection`) at
// `7c10bd43` (v1.0.4): the per-server `ServerState` vocabulary, the `/mcp`
// actions, tool re-registration with withdrawal-as-hidden, and the widest
// non-hidden exposure for the resource tools. Transports, the session tool
// surface, and the OAuth flow are injected seams (see the header).

#include "coding_agent/runtime/McpSessionManager.hpp"

#include "coding_agent/extensions/codemode/CodemodeTool.hpp"
#include "coding_agent/mcp/McpConfigWrite.hpp"
#include "coding_agent/mcp/McpExtensionToolSource.hpp"
#include "coding_agent/mcp/McpNamespace.hpp"
#include "coding_agent/mcp/McpServersSection.hpp"

#include "support/AsyncResultBridge.hpp"

#include <boost/asio/awaitable.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace cch::coding_agent::runtime {
namespace {

using mcp::McpConfigEntry;
using mcp::McpExposure;
using mcp::McpServerConfigBase;

/// pi `errorMessage(error)` for a failed transport operation.
[[nodiscard]] std::string error_message(const support::Error& error) { return error.message; }

/// The transport-independent half of an entry's descriptor (pi
/// `entry.config`).
[[nodiscard]] McpServerConfigBase& config_base(McpConfigEntry& entry) {
    return std::visit([](auto& config) -> McpServerConfigBase& { return config; }, entry.config);
}

[[nodiscard]] const McpServerConfigBase& config_base(const McpConfigEntry& entry) {
    return std::visit([](const auto& config) -> const McpServerConfigBase& { return config; }, entry.config);
}

/// pi `exposureOf(entry)`: the configured exposure, defaulting to codemode.
[[nodiscard]] McpExposure exposure_of(const McpConfigEntry& entry) {
    return config_base(entry).exposure.value_or(McpExposure::Codemode);
}

/// pi `isEnabled(server)`.
[[nodiscard]] bool is_enabled(const McpConfigEntry& entry) { return entry.enabled; }

/// pi `configPatch`: the exposure value as the write half's patch.
[[nodiscard]] mcp::McpServerConfigPatch patch_for_optional(
        std::optional<bool> enabled, std::optional<McpExposure> exposure) {
    mcp::McpServerConfigPatch patch;
    patch.enabled = enabled;
    patch.exposure = exposure;
    return patch;
}

/// The widest non-hidden exposure among `exposures` (pi `syncResourceTools`):
/// `direct` > `codemode` > `deferred`, else hidden.
[[nodiscard]] McpExposure widest_exposure(const std::set<McpExposure>& exposures) {
    constexpr std::array<McpExposure, 3> kOrder{McpExposure::Direct, McpExposure::Codemode, McpExposure::Deferred};
    for (const auto exposure : kOrder) {
        if (exposures.contains(exposure)) {
            return exposure;
        }
    }
    return McpExposure::Hidden;
}

[[nodiscard]] constexpr std::array<std::string_view, 3> resource_tool_names() {
    return {mcp::kListMcpResourcesTool, mcp::kListMcpResourceTemplatesTool, mcp::kReadMcpResourceTool};
}

/// One live connection presented as a resource server (pi
/// `McpResourceServer` over `McpServerConnection`): the resource tools' lane
/// to the connection's resource request methods.
class LiveResourceServer final : public mcp::McpResourceServer {
public:
    explicit LiveResourceServer(std::shared_ptr<McpLiveConnection> connection) : connection_(std::move(connection)) {}

    [[nodiscard]] const std::string& name() const noexcept override { return connection_->server_name(); }
    [[nodiscard]] support::AsyncResult<support::JsonValue> resources_page(
            std::optional<std::string> cursor, std::stop_token stop_token) override {
        return connection_->resources_page(std::move(cursor), stop_token);
    }
    [[nodiscard]] support::AsyncResult<support::JsonValue> resource_templates_page(
            std::optional<std::string> cursor, std::stop_token stop_token) override {
        return connection_->resource_templates_page(std::move(cursor), stop_token);
    }
    [[nodiscard]] support::AsyncResult<support::JsonValue> read_resource(
            std::string uri, std::stop_token stop_token) override {
        return connection_->read_resource(std::move(uri), stop_token);
    }

private:
    std::shared_ptr<McpLiveConnection> connection_;
};

} // namespace

std::string_view mcp_server_state_name(McpServerState state) {
    switch (state) {
    case McpServerState::Connecting:
        return "connecting";
    case McpServerState::Connected:
        return "connected";
    case McpServerState::Disconnected:
        return "disconnected";
    case McpServerState::NeedsAuth:
        return "needs-auth";
    case McpServerState::Failed:
        return "failed";
    case McpServerState::Closed:
        return "closed";
    }
    return "connecting";
}

McpLiveNotificationRouter::McpLiveNotificationRouter(
        McpLiveConnection::ToolsChangedListener on_tools, McpLiveConnection::ResourcesChangedListener on_resources)
    : on_tools_(std::move(on_tools)), on_resources_(std::move(on_resources)) {}

void McpLiveNotificationRouter::dispatch(std::string_view method, const support::JsonValue&) {
    if (method == "notifications/tools/list_changed") {
        if (on_tools_) {
            on_tools_();
        }
        return;
    }
    if (method == "notifications/resources/list_changed") {
        on_resources_changed();
    }
}

void McpLiveNotificationRouter::on_resources_changed() {
    if (on_resources_) {
        on_resources_();
    }
}

McpSessionManager::McpSessionManager(mcp::McpConfigLoad config,
        std::filesystem::path agent_dir,
        std::vector<std::string> overridden,
        McpManagerDependencies dependencies)
    : config_(std::move(config)), agent_dir_(std::move(agent_dir)), project_config_(config_.project_config),
      config_errors_(config_.errors), overridden_(std::move(overridden)), dependencies_(std::move(dependencies)) {
    servers_.reserve(config_.servers.size());
    for (auto& entry : config_.servers) {
        ManagedServer server;
        // pi `entry.scope`: an entry defined in, or overridden by, the trusted
        // project's mcp.json keeps its defining file's scope; a global server
        // overridden by the project keeps scope `global` and an `override`.
        server.scope = project_config_.has_value() && entry.source == *project_config_ ? "project" : "global";
        server.entry = std::move(entry);
        servers_.push_back(std::move(server));
    }
}

McpSessionManager::~McpSessionManager() = default;

McpSessionManager::ManagedServer* McpSessionManager::find(std::string_view server) {
    for (auto& candidate : servers_) {
        if (candidate.entry.name == server) {
            return &candidate;
        }
    }
    return nullptr;
}

const McpSessionManager::ManagedServer* McpSessionManager::find(std::string_view server) const {
    for (const auto& candidate : servers_) {
        if (candidate.entry.name == server) {
            return &candidate;
        }
    }
    return nullptr;
}

std::vector<McpServerSnapshot> McpSessionManager::servers() const {
    std::vector<McpServerSnapshot> snapshots;
    snapshots.reserve(servers_.size());
    for (const auto& server : servers_) {
        McpServerSnapshot snapshot;
        snapshot.entry = server.entry;
        snapshot.scope = server.scope;
        snapshot.message = server.message;
        if (server.connection) {
            McpConnectionSnapshot connection;
            connection.state = server.connection->state();
            connection.tools = server.connection->tools();
            connection.has_resources = server.connection->has_resources();
            connection.resource_count = server.connection->resource_count();
            connection.error = server.connection->error();
            connection.stderr_tail = server.connection->stderr_tail();
            connection.oauth = server.connection->uses_oauth();
            connection.instructions = server.connection->instructions();
            snapshot.connection = std::move(connection);
        }
        snapshots.push_back(std::move(snapshot));
    }
    return snapshots;
}

void McpSessionManager::set_change_listener(std::function<void()> listener) { change_listener_ = std::move(listener); }

void McpSessionManager::emit_change() {
    if (change_listener_) {
        change_listener_();
    }
}

support::AsyncResult<void> McpSessionManager::start() {
    return support::detail::make_async_result([this]() -> boost::asio::awaitable<support::ExpectedVoid> {
        // pi `session_start`: the discovery tool is activated from the config
        // before any connection, so a script or the first prompt never sees
        // it inactive.
        ensure_discovery_active();
        for (auto& server : servers_) {
            if (!is_enabled(server.entry)) {
                continue;
            }
            auto started = co_await support::detail::await_async_result(start_connection(server.entry.name));
            if (!started) {
                co_return std::unexpected(std::move(started.error()));
            }
        }
        co_return support::ExpectedVoid{};
    });
}

support::AsyncResult<void> McpSessionManager::start_connection(std::string_view server) {
    return support::detail::make_async_result(
            [this, name = std::string{server}]() -> boost::asio::awaitable<support::ExpectedVoid> {
                auto* record = find(name);
                if (record == nullptr) {
                    co_return support::ExpectedVoid{};
                }
                auto connection =
                        co_await support::detail::await_async_result(dependencies_.connections->connect(record->entry));
                if (!connection) {
                    co_return std::unexpected(std::move(connection.error()));
                }
                record->connection = std::move(*connection);
                // pi runtime `notifications/tools/list_changed` -> re-list -> `onTools`;
                // `notifications/resources/list_changed` -> re-list -> `onChange`.
                record->connection->set_tools_changed_listener([this, name]() {
                    register_tools(name);
                    emit_change();
                });
                record->connection->set_resources_changed_listener([this]() { emit_change(); });
                if (record->connection->state() == McpServerState::Connected) {
                    register_tools(name);
                }
                emit_change();
                co_return support::ExpectedVoid{};
            });
}

std::vector<std::string> McpSessionManager::resource_bearing_servers() const {
    std::vector<std::string> names;
    for (const auto& server : servers_) {
        if (server.connection && is_enabled(server.entry) && server.connection->has_resources() &&
                exposure_of(server.entry) != McpExposure::Hidden) {
            names.push_back(server.entry.name);
        }
    }
    return names;
}

void McpSessionManager::register_tools(std::string_view server) {
    auto* record = find(server);
    if (record == nullptr || !record->connection) {
        return;
    }
    const std::string name = record->entry.name;
    const McpServerConfigBase& base = config_base(record->entry);
    const std::set<std::string> previous = server_tools_[name];
    std::set<std::string> current;
    for (const auto& tool : record->connection->tools()) {
        McpRegisteredTool registration;
        registration.server = name;
        registration.server_tool_name = tool.name;
        registration.name = mcp::mcp_tool_name(name, tool.name);
        registration.description = tool.description;
        registration.exposure = mcp::get_mcp_tool_exposure(base.tool_exposure, base.exposure, tool.name);
        registration.input_schema = tool.input_schema;
        registration.output_schema = tool.output_schema;
        // pi `client.callTool`: the runnable execution stays bound to the
        // live connection, so a re-registration (including withdrawal as
        // hidden) keeps the same caller.
        std::shared_ptr<McpLiveConnection> connection = record->connection;
        const std::string server_tool_name = tool.name;
        registration.call = [connection, server_tool_name](support::JsonValue arguments, std::stop_token stop_token) {
            return connection->call_tool(server_tool_name, std::move(arguments), stop_token);
        };
        definitions_[registration.name] = registration;
        dependencies_.tools->register_tool(registration);
        current.insert(registration.name);
    }
    server_tools_[name] = std::move(current);
    // pi: tools cannot be unregistered, so a tool the server dropped is
    // re-registered as hidden; it returns with its configured exposure above
    // when the server offers it again.
    for (const auto& dropped : previous) {
        if (!server_tools_[name].contains(dropped)) {
            const auto definition = definitions_.find(dropped);
            if (definition != definitions_.end()) {
                auto hidden = definition->second;
                hidden.exposure = McpExposure::Hidden;
                dependencies_.tools->register_tool(std::move(hidden));
            }
        }
    }
    sync_resource_tools();
}

void McpSessionManager::hide_tools(std::string_view server) {
    auto* record = find(server);
    if (record == nullptr) {
        return;
    }
    const std::string name = record->entry.name;
    for (const auto& tool_name : server_tools_[name]) {
        const auto definition = definitions_.find(tool_name);
        if (definition != definitions_.end()) {
            auto hidden = definition->second;
            hidden.exposure = McpExposure::Hidden;
            dependencies_.tools->register_tool(std::move(hidden));
        }
    }
    server_tools_[name].clear();
    sync_resource_tools();
}

void McpSessionManager::sync_resource_tools() {
    std::set<McpExposure> exposures;
    for (const auto& server : servers_) {
        if (server.connection && is_enabled(server.entry) && server.connection->has_resources() &&
                exposure_of(server.entry) != McpExposure::Hidden) {
            exposures.insert(exposure_of(server.entry));
        }
    }
    const McpExposure next = widest_exposure(exposures);
    if (resource_tools_exposure_ && *resource_tools_exposure_ == next) {
        return;
    }
    if (!resource_tools_exposure_ && next == McpExposure::Hidden) {
        return;
    }
    const bool was_direct = resource_tools_exposure_ && *resource_tools_exposure_ == McpExposure::Direct;
    resource_tools_exposure_ = next;
    std::vector<std::shared_ptr<mcp::McpResourceServer>> servers;
    for (const auto& name : resource_bearing_servers()) {
        if (const ManagedServer* record = find(name); record != nullptr && record->connection) {
            servers.push_back(std::make_shared<LiveResourceServer>(record->connection));
        }
    }
    dependencies_.tools->register_resource_tools(next, std::move(servers));
    if (was_direct) {
        // pi: tools no longer exposed directly leave the declared set.
        std::set<std::string> names;
        for (const auto tool : resource_tool_names()) {
            names.insert(std::string{tool});
        }
        std::vector<std::string> active;
        for (const auto& name : dependencies_.tools->active_tools()) {
            if (!names.contains(name)) {
                active.push_back(name);
            }
        }
        dependencies_.tools->set_active_tools(std::move(active));
    }
}

void McpSessionManager::ensure_discovery_active() {
    // pi `configuredExposures` over every enabled server (the section
    // renderer's helper): the exposures are known from the config before the
    // servers connect.
    std::set<McpExposure> exposures;
    for (const auto& server : servers_) {
        if (!is_enabled(server.entry)) {
            continue;
        }
        for (const auto exposure : mcp::mcp_configured_exposures(server.entry)) {
            exposures.insert(exposure);
        }
    }
    const bool needs_codemode = exposures.contains(McpExposure::Codemode);
    const bool needs_tool_search = exposures.contains(McpExposure::Deferred);
    if (!needs_codemode && !needs_tool_search) {
        return;
    }
    // pi `isCodemodeTool`/`isToolSearchTool`: whether the discovery tools are
    // registered on this session's surface. Pike has no tool_search tool
    // (ADR 0066): the deferred branch activates nothing and the warning
    // names it only as pi's text does.
    constexpr std::string_view kToolSearchToolName = "tool_search";
    bool has_codemode = false;
    bool has_tool_search = false;
    for (const auto& tool : dependencies_.tools->all_tools()) {
        has_codemode = has_codemode || tool.name == extensions::kCodemodeToolName;
        has_tool_search = has_tool_search || tool.name == kToolSearchToolName;
    }
    const std::vector<std::string> active = dependencies_.tools->active_tools();
    const std::string codemode_name{extensions::kCodemodeToolName};
    std::vector<std::string> activate;
    if (needs_codemode && has_codemode && config_.auto_enable_codemode &&
            std::ranges::find(active, codemode_name) == active.end()) {
        activate.push_back(codemode_name);
    }
    if (needs_tool_search && has_tool_search &&
            std::ranges::find(active, std::string{kToolSearchToolName}) == active.end()) {
        activate.push_back(std::string{kToolSearchToolName});
    }
    if (!activate.empty()) {
        std::vector<std::string> next = active;
        next.insert(next.end(), activate.begin(), activate.end());
        dependencies_.tools->set_active_tools(std::move(next));
    }
    const auto reachable = [&] {
        std::vector<std::string> names = active;
        names.insert(names.end(), activate.begin(), activate.end());
        return names;
    }();
    // Either discovery tool reaches every undeclared tool (pi's doc comment),
    // so a warning is needed only when neither is active.
    if (has_codemode && std::ranges::find(reachable, codemode_name) != reachable.end()) {
        return;
    }
    if (has_tool_search && std::ranges::find(reachable, std::string{kToolSearchToolName}) != reachable.end()) {
        return;
    }
    if (warned_unreachable_) {
        return;
    }
    warned_unreachable_ = true;
    const std::string reason =
            needs_codemode && has_codemode && !config_.auto_enable_codemode ? " (autoEnableCodemode is false)" : "";
    std::string warning = "MCP tools are only reachable from the codemode or tool_search tool, but neither is "
                          "active" +
                          reason + "; they cannot be called.";
    warnings_.push_back(warning);
    if (dependencies_.notify_warning) {
        dependencies_.notify_warning(warning);
    }
}

std::optional<std::string> McpSessionManager::save_config(
        ManagedServer& server, mcp::McpServerConfigPatch patch, bool in_project) {
    std::optional<std::filesystem::path> override_path = in_project ? project_config_ : server.entry.override;
    const std::filesystem::path path = override_path ? *override_path : server.entry.source;
    // pi `saveConfig`: an extension-registered server is session-only; this
    // manager's servers always come from mcp.json.
    if (auto updated = mcp::update_mcp_server_config(path, server.entry.name, patch, override_path.has_value());
            !updated) {
        return "Could not update " + path.string() + ": " + updated.error().message;
    }
    if (in_project) {
        // The override is written into the project mcp.json; the defining
        // file's scope is unchanged.
        server.entry.override = override_path;
    }
    if (patch.enabled) {
        server.entry.enabled = *patch.enabled;
    }
    if (patch.exposure) {
        config_base(server.entry).exposure = patch.exposure;
    }
    return std::nullopt;
}

support::AsyncResult<std::optional<std::string>> McpSessionManager::sign_in(
        std::string_view server, McpSignInPrompt prompt) {
    return support::detail::make_async_result(
            [this, name = std::string{server}, prompt = std::move(prompt)]() mutable
                    -> boost::asio::awaitable<support::Expected<std::optional<std::string>>> {
                auto* record = find(name);
                if (record == nullptr) {
                    co_return std::optional<std::string>{std::format("MCP server \"{}\" does not use OAuth.", name)};
                }
                if (!record->connection || !record->connection->uses_oauth() || !dependencies_.auth) {
                    record->message = std::format("MCP server \"{}\" does not use OAuth.", name);
                    emit_change();
                    co_return std::optional<std::string>{record->message};
                }
                auto signed_in = co_await support::detail::await_async_result(
                        dependencies_.auth->sign_in(record->entry, prompt));
                if (!signed_in) {
                    record->message = "Sign-in failed: " + signed_in.error().message;
                    emit_change();
                    co_return std::optional<std::string>{record->message};
                }
                if (signed_in->has_value()) {
                    // pi returns `Sign-in cancelled.` or the flow's message.
                    record->message = **signed_in;
                    emit_change();
                    co_return std::optional<std::string>{record->message};
                }
                // The challenge that asked for this sign-in (for example for
                // more scope) is answered; reconnect with fresh credentials.
                auto reconnected = co_await support::detail::await_async_result(record->connection->reconnect());
                if (!reconnected) {
                    record->message = "Signed in, but " + reconnected.error().message;
                    emit_change();
                    co_return std::optional<std::string>{record->message};
                }
                if (record->connection->state() == McpServerState::Connected) {
                    register_tools(name);
                }
                // pi `runAction`: every action re-checks the discovery
                // activation (a sign-in can make a codemode server reachable).
                ensure_discovery_active();
                record->message.reset();
                emit_change();
                co_return std::optional<std::string>{};
            });
}

support::AsyncResult<bool> McpSessionManager::sign_out(std::string_view server) {
    return support::detail::make_async_result(
            [this, name = std::string{server}]() -> boost::asio::awaitable<support::Expected<bool>> {
                auto* record = find(name);
                if (record == nullptr || !record->connection || !record->connection->uses_oauth() ||
                        !dependencies_.auth) {
                    co_return false;
                }
                auto removed =
                        co_await support::detail::await_async_result(dependencies_.auth->sign_out(record->entry));
                if (!removed) {
                    co_return support::Expected<bool>{false};
                }
                auto signed_out = co_await support::detail::await_async_result(record->connection->sign_out());
                (void)signed_out;
                emit_change();
                co_return support::Expected<bool>{*removed};
            });
}

support::AsyncResult<std::optional<std::string>> McpSessionManager::reconnect(std::string_view server) {
    return support::detail::make_async_result(
            [this, name = std::string{server}]()
                    -> boost::asio::awaitable<support::Expected<std::optional<std::string>>> {
                auto* record = find(name);
                if (record == nullptr || !record->connection) {
                    co_return std::optional<std::string>{std::format("MCP server \"{}\" is disabled.", name)};
                }
                auto reconnected = co_await support::detail::await_async_result(record->connection->reconnect());
                if (!reconnected) {
                    const std::string message = error_message(reconnected.error());
                    record->message = message;
                    emit_change();
                    co_return std::optional<std::string>{message};
                }
                if (record->connection->state() == McpServerState::Connected) {
                    register_tools(name);
                }
                ensure_discovery_active();
                record->message.reset();
                emit_change();
                co_return std::optional<std::string>{};
            });
}

support::AsyncResult<std::optional<std::string>> McpSessionManager::set_enabled(
        std::string_view server, bool enabled, bool in_project) {
    return support::detail::make_async_result(
            [this, name = std::string{server}, enabled, in_project]()
                    -> boost::asio::awaitable<support::Expected<std::optional<std::string>>> {
                auto* record = find(name);
                if (record == nullptr) {
                    co_return support::Expected<std::optional<std::string>>{std::nullopt};
                }
                if (auto failed = save_config(*record, patch_for_optional(enabled, std::nullopt), in_project);
                        failed.has_value()) {
                    record->message = *failed;
                    emit_change();
                    co_return support::Expected<std::optional<std::string>>{*failed};
                }
                if (!enabled) {
                    auto connection = std::move(record->connection);
                    record->connection.reset();
                    hide_tools(name);
                    ensure_discovery_active();
                    record->message.reset();
                    emit_change();
                    if (connection) {
                        connection->close();
                    }
                    co_return support::Expected<std::optional<std::string>>{std::nullopt};
                }
                auto started = co_await support::detail::await_async_result(start_connection(name));
                if (!started) {
                    record->message = started.error().message;
                    emit_change();
                    co_return support::Expected<std::optional<std::string>>{record->message};
                }
                ensure_discovery_active();
                record->message.reset();
                emit_change();
                co_return support::Expected<std::optional<std::string>>{std::nullopt};
            });
}

support::AsyncResult<std::optional<std::string>> McpSessionManager::set_exposure(
        std::string_view server, McpExposure exposure) {
    return support::detail::make_async_result(
            [this,
                    name = std::string{server},
                    exposure]() -> boost::asio::awaitable<support::Expected<std::optional<std::string>>> {
                auto* record = find(name);
                if (record == nullptr) {
                    co_return support::Expected<std::optional<std::string>>{std::nullopt};
                }
                if (auto failed = save_config(*record, patch_for_optional(std::nullopt, exposure), false);
                        failed.has_value()) {
                    record->message = *failed;
                    emit_change();
                    co_return support::Expected<std::optional<std::string>>{*failed};
                }
                if (record->connection && record->connection->state() == McpServerState::Connected) {
                    register_tools(name);
                }
                sync_resource_tools();
                // pi `setExposure`: tools no longer exposed directly leave the
                // declared set; direct tools are activated on registration.
                std::set<std::string> indirect;
                for (const auto& tool : dependencies_.tools->all_tools()) {
                    if (tool.exposure != McpExposure::Direct) {
                        indirect.insert(tool.name);
                    }
                }
                const std::set<std::string> owned = server_tools_[name];
                std::vector<std::string> active;
                for (const auto& active_name : dependencies_.tools->active_tools()) {
                    if (!owned.contains(active_name) || !indirect.contains(active_name)) {
                        active.push_back(active_name);
                    }
                }
                dependencies_.tools->set_active_tools(std::move(active));
                ensure_discovery_active();
                record->message.reset();
                emit_change();
                co_return support::Expected<std::optional<std::string>>{std::nullopt};
            });
}

} // namespace cch::coding_agent::runtime
