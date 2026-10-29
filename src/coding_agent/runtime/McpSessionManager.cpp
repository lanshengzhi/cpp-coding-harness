// The LIVE in-session MCP server manager (spec #882, ticket #884). The
// transport-independent manager logic of pi's `extensions/mcp/index.ts`
// (`createMcpExtension`) and `runtime.ts` (`McpServerConnection`) at
// `7c10bd43` (v1.0.4): the per-server `ServerState` vocabulary, the `/mcp`
// actions, tool re-registration with withdrawal-as-hidden, and the widest
// non-hidden exposure for the resource tools. Transports, the session tool
// surface, and the OAuth flow are injected seams (see the header).

#include "coding_agent/runtime/McpSessionManager.hpp"

#include "coding_agent/extensions/codemode/CodemodeTool.hpp"
#include "coding_agent/extensions/tool_search/ToolSearch.hpp"
#include "coding_agent/mcp/McpConfigWrite.hpp"
#include "coding_agent/mcp/McpExtensionToolSource.hpp"
#include "coding_agent/mcp/McpNamespace.hpp"
#include "coding_agent/mcp/McpServersSection.hpp"

#include "support/AsyncResultBridge.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/this_coro.hpp>

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
        } else if (server.start_pending) {
            // pi `startConnection` publishes a `connecting` connection while
            // the background connect is in flight (#1104): the management
            // panel and the creation status read the pending server as
            // connecting instead of connection-less.
            McpConnectionSnapshot connecting;
            connecting.state = McpServerState::Connecting;
            snapshot.connection = std::move(connecting);
        }
        snapshots.push_back(std::move(snapshot));
    }
    return snapshots;
}

void McpSessionManager::attach_tool_surface(std::shared_ptr<McpToolSurface> surface) {
    dependencies_.tools = std::move(surface);
}

std::vector<extensions::ToolSearchCandidate> McpSessionManager::search_tools(
        std::string_view query, std::size_t limit) {
    if (!dependencies_.tools) return {};
    const auto active = dependencies_.tools->active_tools();
    std::vector<extensions::ToolSearchCandidate> candidates;
    for (const auto& item : dependencies_.tools->all_tools()) {
        candidates.push_back({.name = item.name,
                .description = item.description,
                .namespace_name = item.namespace_name,
                .exposure = item.exposure,
                .active = std::ranges::find(active, item.name) != active.end()});
    }
    return extensions::rank_tool_search_candidates(std::move(candidates), query, limit);
}

support::ExpectedVoid McpSessionManager::activate_tools(std::vector<std::string> names) {
    if (!dependencies_.tools || names.empty()) return {};
    return dependencies_.tools->activate_tools(std::move(names));
}

void McpSessionManager::restore_loaded_tools(std::vector<std::string> names) {
    if (!dependencies_.tools || names.empty()) return;
    dependencies_.tools->restore_loaded_tools(std::move(names));
}

void McpSessionManager::set_notify_warning_sink(std::function<void(std::string_view message)> sink) {
    dependencies_.notify_warning = std::move(sink);
}

void McpSessionManager::set_change_listener(std::function<void()> listener) { change_listener_ = std::move(listener); }

void McpSessionManager::close() noexcept {
    // pi `session_end`: the shutdown path does not await the background
    // connects (`backgroundActions` outlive the session); every live
    // connection closes now and an in-flight connect drops its late
    // connection when it lands (`launch_start`'s `isCurrent` drop).
    closed_ = true;
    tokens_at_sign_in_.clear();
    for (auto& server : servers_) {
        server.generation += 1;
        if (server.connection) {
            server.connection->close();
        }
    }
}

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
        // pi's `session_start` launches every enabled server's
        // `createConnection` without awaiting and tracks the settle in
        // `backgroundActions`: session creation must not block on a slow
        // handshake (audit-1104, frozen pi v1.3.0), and a failed server is a
        // failed connection — never a veto on the session. The launches queue
        // on the caller's serialized executor; the settles land whenever the
        // transports answer.
        auto executor = co_await boost::asio::this_coro::executor;
        for (auto& server : servers_) {
            if (!is_enabled(server.entry)) {
                continue;
            }
            launch_start(executor, server.entry.name);
        }
        co_return support::ExpectedVoid{};
    });
}

support::AsyncResult<void> McpSessionManager::wait_for_pending_connects() {
    return support::detail::make_async_result([this]() -> boost::asio::awaitable<support::ExpectedVoid> {
        // pi `waitForPendingConnections`: every launch decrements the shared
        // counter on the same serialized executor this waiter polls, so one
        // yielded pass per change settles the wait without a signal channel.
        auto executor = co_await boost::asio::this_coro::executor;
        while (pending_starts_ > 0 && !closed_) {
            co_await boost::asio::post(executor, boost::asio::use_awaitable);
        }
        co_return support::ExpectedVoid{};
    });
}

support::AsyncResult<bool> McpSessionManager::wait_for_direct_servers(std::chrono::milliseconds bound) {
    return support::detail::make_async_result([this, bound]() -> boost::asio::awaitable<support::Expected<bool>> {
        // pi `waitForDirectServers` (`before_agent_start`): the first prompt
        // only (`waitedForStartup` latches) waits, bounded by `startupWaitMs`,
        // for the enabled servers with `direct` exposure (pi `hasDirectTools`
        // reads the configured exposures, so the wait is known before any
        // connection answers). On expiry the still-connecting servers keep
        // connecting and the model is told so.
        if (waited_for_startup_) {
            co_return support::Expected<bool>{true};
        }
        waited_for_startup_ = true;
        auto executor = co_await boost::asio::this_coro::executor;
        const auto deadline = std::chrono::steady_clock::now() + bound;
        for (;;) {
            if (closed_) {
                co_return support::Expected<bool>{false};
            }
            bool any_pending = false;
            for (auto& server : servers_) {
                if (!is_enabled(server.entry) || !server.start_pending) {
                    continue;
                }
                const auto exposures = mcp::mcp_configured_exposures(server.entry);
                if (std::ranges::find(exposures, mcp::McpExposure::Direct) != exposures.end()) {
                    any_pending = true;
                }
            }
            if (!any_pending) {
                co_return support::Expected<bool>{true};
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                if (dependencies_.notify_info) {
                    dependencies_.notify_info(
                            "MCP servers are still connecting; their tools become available once connected.");
                }
                co_return support::Expected<bool>{false};
            }
            co_await boost::asio::post(executor, boost::asio::use_awaitable);
        }
    });
}

support::AsyncResult<void> McpSessionManager::on_turn_start() {
    return support::detail::make_async_result([this]() -> boost::asio::awaitable<support::ExpectedVoid> {
        // pi `turn_start` first awaits `waitForPendingConnections` so the
        // pickup sees settled `needs-auth` states. Pike's prompt path runs the
        // bounded `wait_for_direct_servers` just before this hook (pi's
        // `before_agent_start`), which subsumes the unbounded wait: direct
        // servers settle within the bound, and a codemode-only straggler is
        // awaited by whatever reads it (status, subcommands, tool calls). The
        // pickup itself is the observable part: reconnect every server whose
        // stored tokens changed since its `needs-auth` snapshot.
        co_await reconnect_signed_in();
        co_return support::ExpectedVoid{};
    });
}

void McpSessionManager::launch_start(const boost::asio::any_io_executor& executor, std::string_view server) {
    auto* record = find(server);
    if (record == nullptr || closed_ || record->start_pending || record->connection) {
        return;
    }
    record->start_pending = true;
    record->generation += 1;
    const auto generation = record->generation;
    ++pending_starts_;
    // The panel's first read after `start()` should show the server
    // connecting (pi renders the tracked connection's `connecting` state).
    emit_change();
    std::weak_ptr<McpSessionManager> weak = weak_from_this();
    boost::asio::co_spawn(
            executor,
            [weak, name = std::string{server}, generation]() mutable -> boost::asio::awaitable<void> {
                auto self = weak.lock();
                if (!self) {
                    co_return;
                }
                auto outcome = co_await support::detail::await_async_result(self->start_connection(name, generation));
                auto* record = self->find(name);
                self->pending_starts_ -= 1;
                if (record != nullptr) {
                    record->start_pending = false;
                }
                if (self->closed_ || (record != nullptr && record->generation != generation)) {
                    // pi `startConnection`'s `isCurrent` drop: a server
                    // disabled (or a manager closed) while the connect flew
                    // loses the late connection instead of resurrecting it.
                    if (record != nullptr && record->connection) {
                        auto stale = std::move(record->connection);
                        record->connection.reset();
                        stale->close();
                        self->hide_tools(name);
                    }
                    co_return;
                }
                if (!outcome && record != nullptr) {
                    // A factory-level error (unlike a Failed connection) has
                    // no connection to carry it: pi surfaces it as the
                    // server's panel message, never as a session veto.
                    record->message = outcome.error().message;
                }
                if (record != nullptr) {
                    co_await self->sync_sign_in_snapshot(*record);
                }
                co_return;
            },
            boost::asio::detached);
}

boost::asio::awaitable<void> McpSessionManager::sync_sign_in_snapshot(ManagedServer& server) {
    const std::string name = server.entry.name;
    if (closed_ || !server.connection || !server.connection->uses_oauth() ||
            server.connection->state() != McpServerState::NeedsAuth) {
        tokens_at_sign_in_.erase(name);
        co_return;
    }
    if (tokens_at_sign_in_.contains(name) || !dependencies_.auth) {
        co_return;
    }
    auto stored = co_await support::detail::await_async_result(dependencies_.auth->stored_tokens(server.entry));
    tokens_at_sign_in_[name] = stored.has_value() ? std::optional<std::string>{std::move(*stored)}
                                                  : std::optional<std::string>{std::nullopt};
}

boost::asio::awaitable<void> McpSessionManager::reconnect_signed_in() {
    // pi `reconnectSignedIn`: for every connection that was `needs-auth` with
    // a stored-token snapshot, compare the snapshot with the live store; on a
    // change forget the snapshot and reconnect (`Promise.allSettled` — a
    // failed reconnect stays on the server's message and never rejects the
    // turn).
    const auto snapshots = tokens_at_sign_in_;
    for (const auto& [name, snapshot] : snapshots) {
        auto* record = find(name);
        if (record == nullptr || !dependencies_.auth || !record->connection || !record->connection->uses_oauth() ||
                record->connection->state() != McpServerState::NeedsAuth) {
            tokens_at_sign_in_.erase(name);
            continue;
        }
        auto current = co_await support::detail::await_async_result(dependencies_.auth->stored_tokens(record->entry));
        if (!current) {
            // The store cannot be read right now: keep the snapshot and try
            // again on the next turn rather than reconnecting blind.
            continue;
        }
        if (*current == snapshot) {
            continue;
        }
        tokens_at_sign_in_.erase(name);
        // The reconnect records its own outcome (message and change) on the
        // server; pi's `Promise.allSettled` means a failed reconnect never
        // rejects the turn.
        (void)co_await support::detail::await_async_result(reconnect(name));
    }
}

support::AsyncResult<void> McpSessionManager::start_connection(
        std::string_view server, std::uint64_t expected_generation) {
    return support::detail::make_async_result(
            [this, name = std::string{server}, expected_generation]() -> boost::asio::awaitable<support::ExpectedVoid> {
                auto* record = find(name);
                if (record == nullptr) {
                    co_return support::ExpectedVoid{};
                }
                auto connection =
                        co_await support::detail::await_async_result(dependencies_.connections->connect(record->entry));
                if (!connection) {
                    co_return std::unexpected(std::move(connection.error()));
                }
                // pi `startConnection`'s `isCurrent` drop, checked before the
                // connection touches the session: a connect that lands after
                // the session closed (or the server was disabled) releases the
                // late connection instead of registering tools on the
                // already-released Agent (#1104).
                if (closed_ || (expected_generation != 0 && record->generation != expected_generation)) {
                    (*connection)->close();
                    co_return support::ExpectedVoid{};
                }
                record->connection = std::move(*connection);
                // pi runtime `notifications/tools/list_changed` -> re-list -> `onTools`;
                // `notifications/resources/list_changed` -> re-list -> `onChange`.
                // The listeners outlive this call: a tool call may hold the
                // connection after the session released the manager, so they
                // bind the manager weakly and drop the notification instead of
                // touching a destroyed surface.
                std::weak_ptr<McpSessionManager> weak_self = weak_from_this();
                record->connection->set_tools_changed_listener([weak_self, name]() {
                    auto self = weak_self.lock();
                    if (!self || self->closed_) {
                        return;
                    }
                    self->register_tools(name);
                    self->emit_change();
                });
                record->connection->set_resources_changed_listener([weak_self]() {
                    auto self = weak_self.lock();
                    if (!self || self->closed_) {
                        return;
                    }
                    self->emit_change();
                });
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
    // pi `session_end` releases the session surface with the session: a
    // `notifications/tools/list_changed` (or any re-list) that lands after
    // Close must never touch the released Agent (#1104).
    if (closed_) {
        return;
    }
    auto* record = find(server);
    if (record == nullptr || !record->connection) {
        return;
    }
    const std::string name = record->entry.name;
    const McpServerConfigBase& base = config_base(record->entry);
    const std::set<std::string> previous = server_tools_[name];
    const auto& live_tools = record->connection->tools();
    // pi `index.ts` assigns Agent-visible names order-independently across the
    // whole listing: colliding sanitized names all take the hash suffix, so a
    // re-registration (or a tools/list update) never reshuffles which tool
    // owns which name.
    std::vector<std::string> raw_names;
    raw_names.reserve(live_tools.size());
    for (const auto& tool : live_tools) {
        raw_names.push_back(tool.name);
    }
    const std::vector<std::string> assigned = mcp::assign_mcp_tool_names(name, raw_names);
    std::set<std::string> current;
    for (std::size_t index = 0; index < live_tools.size(); ++index) {
        const auto& tool = live_tools[index];
        McpRegisteredTool registration;
        registration.server = name;
        registration.server_tool_name = tool.name;
        registration.name = assigned[index];
        registration.description = tool.description;
        registration.exposure = mcp::get_mcp_tool_exposure(base.tool_exposure, base.exposure, tool.name);
        registration.input_schema = tool.input_schema;
        registration.output_schema = tool.output_schema;
        // pi `client.callTool`: the runnable execution stays bound to the
        // live connection, so a re-registration (including withdrawal as
        // hidden) keeps the same caller.
        std::shared_ptr<McpLiveConnection> connection = record->connection;
        const std::string server_tool_name = tool.name;
        registration.call = [connection, server_tool_name](support::JsonValue arguments,
                                    std::stop_token stop_token,
                                    cch::mcp::Client::ProgressCallback on_progress) {
            return connection->call_tool(server_tool_name, std::move(arguments), stop_token, std::move(on_progress));
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
    if (closed_) {
        return;
    }
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
                // pi `onConnectionChange`: a connected server drops its
                // `needs-auth` snapshot, so a later external token change can
                // no longer retrigger a pickup reconnect.
                co_await sync_sign_in_snapshot(*record);
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
                // pi `onConnectionChange`: the freshly `needs-auth` connection
                // snapshots the store as it stands after the removal, so a
                // later external sign-in reads as a change at `turn_start`.
                co_await sync_sign_in_snapshot(*record);
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
                // pi `onConnectionChange`: the reconnect's outcome settles
                // the snapshot question (connected erases it; a fresh
                // `needs-auth` re-snapshots below).
                co_await sync_sign_in_snapshot(*record);
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
                    // Invalidate any in-flight connect so its late completion
                    // drops instead of resurrecting the disabled server (pi
                    // `startConnection`'s `isCurrent` drop).
                    record->generation += 1;
                    record->start_pending = false;
                    hide_tools(name);
                    ensure_discovery_active();
                    record->message.reset();
                    emit_change();
                    if (connection) {
                        connection->close();
                    }
                    co_return support::Expected<std::optional<std::string>>{std::nullopt};
                }
                if (record->start_pending) {
                    // `start()`'s background connect already owns this server;
                    // enabling a connecting server is a no-op success (pi's
                    // enable runs `reconnect`, whose pending `initialize` the
                    // client itself supersedes).
                    record->message.reset();
                    emit_change();
                    co_return support::Expected<std::optional<std::string>>{std::nullopt};
                }
                auto started = co_await support::detail::await_async_result(start_connection(name));
                if (!started) {
                    record->message = started.error().message;
                    emit_change();
                    co_return support::Expected<std::optional<std::string>>{record->message};
                }
                co_await sync_sign_in_snapshot(*record);
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
