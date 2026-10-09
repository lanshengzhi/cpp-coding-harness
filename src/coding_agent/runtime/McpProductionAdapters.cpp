// The production adapters for the live in-session MCP manager (see the
// header). Each adapter is the thin bridge the manager's injected seams
// describe: the connection/factory over the landed stdio/streamable-http
// clients, the sign-in driver over `sign_in_mcp_server` + `mcp-auth.json`,
// and the tool surface over the Agent's live seam.

#include "coding_agent/runtime/McpProductionAdapters.hpp"

#include <cch/ai/BoostBeastStreamTransport.hpp>
#include "coding_agent/extensions/ExtensionToolRegistry.hpp"
#include "coding_agent/extensions/codemode/CodemodeTool.hpp"
#include "coding_agent/mcp/McpAuthStore.hpp"
#include "coding_agent/mcp/McpConfigFile.hpp"
#include "coding_agent/mcp/McpExtensionToolSource.hpp"
#include "coding_agent/mcp/McpHttpClient.hpp"
#include "coding_agent/mcp/McpHttpServerConfig.hpp"
#include "coding_agent/mcp/McpOAuthProvider.hpp"
#include "coding_agent/mcp/McpOAuthSignIn.hpp"
#include "coding_agent/mcp/McpOAuthTokenResolver.hpp"
#include "coding_agent/mcp/McpResourceTools.hpp"
#include "coding_agent/mcp/McpStdioClient.hpp"

#include "support/AsyncResultBridge.hpp"

#include <boost/asio/awaitable.hpp>

#include <algorithm>
#include <cctype>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace cch::coding_agent::runtime {
namespace {

/// pi `usesOAuth` (extensions/mcp/runtime.ts): an HTTP server with no `auth`
/// provider and no explicit `Authorization` header can sign in; stdio servers
/// cannot.
[[nodiscard]] bool oauth_eligible(const mcp::McpHttpServerConfig& config) {
    if (config.auth_provider.has_value()) {
        return false;
    }
    for (const auto& [key, _] : config.headers) {
        if (key.size() == 13) {
            std::string lowered = key;
            for (char& character : lowered) {
                character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
            }
            if (lowered == "authorization") {
                return false;
            }
        }
    }
    return true;
}

/// pi runtime's connected-time tool shape: the descriptors `tools/list`
/// reported become the manager's live tools.
[[nodiscard]] std::vector<McpLiveTool> live_tools(const std::vector<mcp::McpToolDescriptor>& descriptors) {
    std::vector<McpLiveTool> tools;
    tools.reserve(descriptors.size());
    for (const auto& descriptor : descriptors) {
        McpLiveTool tool;
        tool.name = descriptor.server_tool_name;
        tool.description = descriptor.description;
        tool.input_schema = descriptor.parameters;
        tool.output_schema = descriptor.output_schema;
        tools.push_back(std::move(tool));
    }
    return tools;
}

} // namespace

// ── ProductionMcpConnection ─────────────────────────────────────────────────

ProductionMcpConnection::ProductionMcpConnection(std::string server_name,
        ConnectSource connect_source,
        ConnectedMcpTransport connected,
        bool oauth_eligible_value,
        std::string oauth_url)
    : server_name_(std::move(server_name)), connect_source_(std::move(connect_source)),
      client_(std::move(connected.client)), state_(client_ ? McpServerState::Connecting : McpServerState::Failed),
      oauth_eligible_(oauth_eligible_value), oauth_url_(std::move(oauth_url)),
      instructions_(std::move(connected.instructions)) {}

void ProductionMcpConnection::note_connect_error(support::Error error) noexcept {
    state_ = error.code == support::ErrorCode::OAuth ? McpServerState::NeedsAuth : McpServerState::Failed;
    error_ = std::move(error.message);
}

support::AsyncResult<support::JsonValue> ProductionMcpConnection::request(std::string method,
        std::optional<support::JsonValue> params,
        mcp::McpServerConnection::RequestOptions options) {
    return support::detail::make_async_result(
            [this, method = std::move(method), params = std::move(params), stop_token = options.stop_token]() mutable
                    -> boost::asio::awaitable<support::Expected<support::JsonValue>> {
                if (!client_) {
                    co_return std::unexpected(support::make_error(support::ErrorCode::Validation,
                            "MCP server '" + server_name_ + "' has no live connection"));
                }
                auto res = co_await support::detail::await_async_result(client_->request(
                        method, params, mcp::McpServerConnection::RequestOptions{.stop_token = stop_token}));
                // pi `withClient`: McpSessionExpiredError on 404 retried once on a fresh session
                if (!res && res.error().message == "MCP session expired") {
                    auto reconnected = co_await support::detail::await_async_result(reconnect());
                    if (reconnected && client_) {
                        res = co_await support::detail::await_async_result(client_->request(std::move(method),
                                std::move(params),
                                mcp::McpServerConnection::RequestOptions{.stop_token = stop_token}));
                    }
                }
                co_return res;
            });
}

support::AsyncResult<support::JsonValue> ProductionMcpConnection::call_tool(std::string_view tool,
        support::JsonValue arguments,
        std::stop_token stop_token,
        mcp::McpServerConnection::ProgressCallback on_progress) {
    return support::detail::make_async_result(
            [this,
                    tool = std::string{tool},
                    arguments = std::move(arguments),
                    stop_token,
                    on_progress = std::move(
                            on_progress)]() mutable -> boost::asio::awaitable<support::Expected<support::JsonValue>> {
                support::JsonValue params{support::JsonValue::object_t{
                        {"name", std::move(tool)},
                        {"arguments", std::move(arguments)},
                }};
                auto result = co_await support::detail::await_async_result(request("tools/call",
                        std::move(params),
                        mcp::McpServerConnection::RequestOptions{
                                .stop_token = stop_token, .on_progress = std::move(on_progress)}));
                if (!result) {
                    // pi runtime: an OAuth challenge moves the connection to
                    // needs-auth so the panel offers the sign-in action.
                    if (result.error().code == support::ErrorCode::OAuth) {
                        state_ = McpServerState::NeedsAuth;
                        error_ = result.error().message;
                    }
                    co_return std::unexpected(std::move(result.error()));
                }
                co_return std::move(*result);
            });
}

support::AsyncResult<support::JsonValue> ProductionMcpConnection::resources_page(
        std::optional<std::string> cursor, std::stop_token stop_token) {
    std::optional<support::JsonValue> params;
    if (cursor.has_value()) {
        params = support::JsonValue{support::JsonValue::object_t{{"cursor", std::move(*cursor)}}};
    }
    return request(
            "resources/list", std::move(params), mcp::McpServerConnection::RequestOptions{.stop_token = stop_token});
}

support::AsyncResult<support::JsonValue> ProductionMcpConnection::resource_templates_page(
        std::optional<std::string> cursor, std::stop_token stop_token) {
    std::optional<support::JsonValue> params;
    if (cursor.has_value()) {
        params = support::JsonValue{support::JsonValue::object_t{{"cursor", std::move(*cursor)}}};
    }
    return request("resources/templates/list",
            std::move(params),
            mcp::McpServerConnection::RequestOptions{.stop_token = stop_token});
}

support::AsyncResult<support::JsonValue> ProductionMcpConnection::read_resource(
        std::string uri, std::stop_token stop_token) {
    return request("resources/read",
            support::JsonValue{support::JsonValue::object_t{{"uri", std::move(uri)}}},
            mcp::McpServerConnection::RequestOptions{.stop_token = stop_token});
}

support::AsyncResult<void> ProductionMcpConnection::refresh() {
    return support::detail::make_async_result([this]() -> boost::asio::awaitable<support::ExpectedVoid> {
        if (!connect_source_) {
            co_return std::unexpected(support::make_error(
                    support::ErrorCode::Validation, "MCP server '" + server_name_ + "' cannot reconnect"));
        }
        auto connected = co_await connect_source_();
        if (!connected) {
            // An OAuth challenge (for example stale credentials after an
            // external sign-out) asks for a sign-in, not a failure.
            state_ = connected.error().code == support::ErrorCode::OAuth ? McpServerState::NeedsAuth
                                                                         : McpServerState::Failed;
            error_ = connected.error().message;
            co_return std::unexpected(std::move(connected.error()));
        }
        auto tools = co_await mcp::list_mcp_server_tools(*connected->client);
        if (!tools) {
            state_ = McpServerState::Failed;
            error_ = tools.error().message;
            co_return std::unexpected(std::move(tools.error()));
        }
        // pi runtime `fetchResources`: the connected-time snapshot, with MCP
        // App resources removed. A list failure leaves that side empty, so a
        // server whose lists fail still connects.
        mcp::McpConnectionResourceServer resource_server{connected->client};
        auto snapshot = co_await support::detail::await_async_result(
                mcp::fetch_mcp_resource_snapshot(resource_server, std::stop_token{}));
        if (!snapshot) {
            state_ = McpServerState::Failed;
            error_ = snapshot.error().message;
            co_return std::unexpected(std::move(snapshot.error()));
        }
        client_ = std::move(connected->client);
        instructions_ = std::move(connected->instructions);
        tools_ = live_tools(*tools);
        has_resources_ = snapshot->has_resources;
        resource_count_ = snapshot->resources.size();
        error_.reset();
        state_ = McpServerState::Connected;
        co_return support::ExpectedVoid{};
    });
}

support::AsyncResult<void> ProductionMcpConnection::reconnect() {
    return support::detail::make_async_result([this]() -> boost::asio::awaitable<support::ExpectedVoid> {
        state_ = McpServerState::Connecting;
        error_.reset();
        auto refreshed = co_await support::detail::await_async_result(refresh());
        if (!refreshed) {
            co_return std::unexpected(std::move(refreshed.error()));
        }
        if (tools_changed_) {
            tools_changed_();
        }
        co_return support::ExpectedVoid{};
    });
}

support::AsyncResult<void> ProductionMcpConnection::sign_out() {
    // pi `connection.signOut()`: drop the live client and mark needs-auth.
    // The credential removal itself is the sign-in driver's job.
    client_.reset();
    state_ = McpServerState::NeedsAuth;
    return support::AsyncResult<void>{support::ExpectedVoid{}};
}

void ProductionMcpConnection::close() noexcept {
    state_ = McpServerState::Closed;
    if (client_) {
        // stdio teardown runs in the client's destructor; the HTTP client has
        // an explicit close (GET stream abort + session DELETE).
        if (auto* http = dynamic_cast<mcp::McpHttpClient*>(client_.get()); http != nullptr) {
            http->close();
        }
        client_.reset();
    }
}

void ProductionMcpConnection::set_tools_changed_listener(ToolsChangedListener listener) {
    tools_changed_ = std::move(listener);
}

void ProductionMcpConnection::set_resources_changed_listener(ResourcesChangedListener listener) {
    resources_changed_ = std::move(listener);
}

// ── ProductionMcpConnectionFactory ──────────────────────────────────────────

ProductionMcpConnectionFactory::ProductionMcpConnectionFactory(std::filesystem::path agent_dir)
    : agent_dir_(std::move(agent_dir)) {}

std::shared_ptr<mcp::McpAuthStore> ProductionMcpConnectionFactory::auth_store() const {
    if (!auth_store_) {
        auth_store_ = std::make_shared<mcp::McpAuthStore>(mcp::McpAuthStore::default_path(agent_dir_));
    }
    return auth_store_;
}

support::AsyncResult<std::shared_ptr<McpLiveConnection>> ProductionMcpConnectionFactory::connect(
        const mcp::McpConfigEntry& entry) {
    return support::detail::make_async_result(
            [this, entry]() mutable -> boost::asio::awaitable<support::Expected<std::shared_ptr<McpLiveConnection>>> {
                const std::string name = entry.name;
                // The connect source the connection re-runs on reconnect: one fresh
                // transport plus handshake. For an OAuth-eligible HTTP server the
                // request-time token resolves from mcp-auth.json (pi shape), with the
                // legacy auth.json record migrated on first need — best-effort, never
                // vetoing the connect.
                ProductionMcpConnection::ConnectSource source;
                bool eligible = false;
                std::string oauth_url;
                if (const auto* http = std::get_if<mcp::McpHttpServerConfig>(&entry.config)) {
                    eligible = http->resolved_oauth.has_value() ||
                               (http->oauth.has_value() && !http->auth_provider.has_value() && oauth_eligible(*http));
                    oauth_url = http->url;
                    std::shared_ptr<mcp::McpRequestAuthSource> request_auth;
                    if (eligible) {
                        const std::shared_ptr<mcp::McpAuthStore> store = auth_store();
                        if (http->resolved_oauth) {
                            request_auth = std::make_shared<mcp::McpOAuthTokenResolver>(store,
                                    http->name,
                                    http->url,
                                    std::make_shared<mcp::McpOAuthProvider>(*http->resolved_oauth));
                        } else {
                            (void)store->migrate_from_auth_json(agent_dir_ / "auth.json", http->name, http->url);
                            request_auth = std::make_shared<mcp::McpOAuthFlowTokenResolver>(
                                    store, http->name, http->url, *http->oauth);
                        }
                    }
                    const mcp::McpHttpServerConfig config = *http;
                    source = [config, request_auth]()
                            -> boost::asio::awaitable<support::Expected<ConnectedMcpTransport>> {
                        auto client = co_await mcp::McpHttpClient::connect(
                                config, std::make_shared<ai::providers::BoostBeastStreamTransport>(), request_auth);
                        if (!client) {
                            co_return std::unexpected(std::move(client.error()));
                        }
                        ConnectedMcpTransport connected;
                        connected.client = *client;
                        connected.instructions = (*client)->server_instructions();
                        co_return connected;
                    };
                } else {
                    const mcp::McpStdioServerConfig config = std::get<mcp::McpStdioServerConfig>(entry.config);
                    source = [config]() -> boost::asio::awaitable<support::Expected<ConnectedMcpTransport>> {
                        auto client = co_await mcp::McpStdioClient::connect(config);
                        if (!client) {
                            co_return std::unexpected(std::move(client.error()));
                        }
                        ConnectedMcpTransport connected;
                        connected.client = *client;
                        connected.instructions = (*client)->server_instructions();
                        co_return connected;
                    };
                }
                auto connected = co_await source();
                auto connection = std::make_shared<ProductionMcpConnection>(name,
                        source,
                        connected ? std::move(*connected) : ConnectedMcpTransport{},
                        eligible,
                        std::move(oauth_url));
                if (!connected) {
                    // pi: an OAuth challenge at connect is needs-auth (the panel
                    // offers the sign-in action); anything else is failed. Either way
                    // the connection is live enough to retry via reconnect().
                    connection->note_connect_error(std::move(connected.error()));
                    co_return std::shared_ptr<McpLiveConnection>{std::move(connection)};
                }
                // pi runtime `getClient()` after connect: list the tools and take the
                // connected-time resource snapshot before the connection reports
                // connected.
                static_cast<void>(co_await support::detail::await_async_result(connection->refresh()));
                co_return std::shared_ptr<McpLiveConnection>{std::move(connection)};
            });
}

// ── McpOAuthFlowSignInDriver ────────────────────────────────────────────────

McpOAuthFlowSignInDriver::McpOAuthFlowSignInDriver(std::filesystem::path agent_dir)
    : agent_dir_(std::move(agent_dir)) {}

support::AsyncResult<std::optional<std::string>> McpOAuthFlowSignInDriver::sign_in(
        const mcp::McpConfigEntry& entry, const McpSignInPrompt& prompt) {
    return support::detail::make_async_result(
            [this, entry, prompt]() mutable -> boost::asio::awaitable<support::Expected<std::optional<std::string>>> {
                const auto* http = std::get_if<mcp::McpHttpServerConfig>(&entry.config);
                if (http == nullptr || !http->oauth.has_value() || http->auth_provider.has_value() ||
                        !oauth_eligible(*http)) {
                    co_return std::optional<std::string>{
                            std::format("MCP server \"{}\" does not use OAuth.", entry.name)};
                }
                if (!auth_store_) {
                    auth_store_ = std::make_shared<mcp::McpAuthStore>(mcp::McpAuthStore::default_path(agent_dir_));
                }
                mcp::McpOAuthSignInRequest request;
                request.store = auth_store_;
                request.server_name = entry.name;
                request.server_url = http->url;
                request.oauth = *http->oauth;
                mcp::McpOAuthSignInPrompt adapted;
                adapted.show_authorization_url = [show = prompt.show_authorization_url](const std::string& url) {
                    if (show) {
                        show(url);
                    }
                };
                adapted.prompt_for_redirect_url =
                        [prompt_for_redirect = prompt.prompt_for_redirect_url](
                                std::stop_token) -> support::AsyncResult<std::optional<std::string>> {
                    if (!prompt_for_redirect) {
                        return support::AsyncResult<std::optional<std::string>>{std::nullopt};
                    }
                    return support::detail::make_async_result(
                            [prompt_for_redirect]() mutable
                                    -> boost::asio::awaitable<support::Expected<std::optional<std::string>>> {
                                co_return co_await prompt_for_redirect();
                            });
                };
                request.prompt = std::move(adapted);
                auto outcome = co_await support::detail::await_async_result(sign_in_mcp_server(std::move(request)));
                if (!outcome) {
                    // pi `signIn` (extensions/mcp/index.ts): the cancelled
                    // error is the info-level "Sign-in cancelled."; every
                    // other failure stays an error, which the manager renders
                    // with pi's "Sign-in failed: " prefix.
                    if (outcome.error().code == support::ErrorCode::Cancelled) {
                        co_return std::optional<std::string>{"Sign-in cancelled."};
                    }
                    co_return std::unexpected(std::move(outcome.error()));
                }
                co_return std::optional<std::string>{};
            });
}

support::AsyncResult<bool> McpOAuthFlowSignInDriver::sign_out(const mcp::McpConfigEntry& entry) {
    return support::detail::make_async_result(
            [this, entry]() mutable -> boost::asio::awaitable<support::Expected<bool>> {
                const auto* http = std::get_if<mcp::McpHttpServerConfig>(&entry.config);
                if (http == nullptr) {
                    co_return false;
                }
                if (!auth_store_) {
                    auth_store_ = std::make_shared<mcp::McpAuthStore>(mcp::McpAuthStore::default_path(agent_dir_));
                }
                auto removed = auth_store_->remove(entry.name, http->url);
                if (!removed) {
                    co_return std::unexpected(std::move(removed.error()));
                }
                co_return support::Expected<bool>{*removed};
            });
}

// ── AgentMcpToolSurface ─────────────────────────────────────────────────────

AgentMcpToolSurface::AgentMcpToolSurface(agent::Agent& agent, std::vector<std::string> registered_names)
    : agent_(agent) {
    known_names_.insert(
            std::make_move_iterator(registered_names.begin()), std::make_move_iterator(registered_names.end()));
}

void AgentMcpToolSurface::register_tool(McpRegisteredTool tool) {
    const std::string name = tool.name;
    const mcp::McpExposure exposure = tool.exposure;
    const std::string server = tool.server;
    extensions::ExtensionTool extension;
    extension.definition.name = name;
    extension.definition.description = std::move(tool.description);
    extension.definition.parameters = std::move(tool.input_schema);
    extension.definition.output_schema = mcp::create_mcp_result_schema(tool.output_schema);
    // pi MCP tools carry no `executionMode`: parallel-capable, the transport
    // serializes frames internally.
    extension.concurrency = agent::ToolConcurrency::ParallelSafe;
    auto call = std::move(tool.call);
    // pi `createMcpToolDefinition`'s execute observes the call's progress and
    // streams pi's "Progress <n>[/<total>]" updates through the run's update
    // sink (the same conversion the assembly-time source uses).
    extension.context_execute =
            [call = std::move(call), server](support::JsonValue arguments,
                    extensions::ExtensionToolContext context,
                    std::stop_token stop_token) mutable -> support::AsyncResult<extensions::ExtensionToolResult> {
        return support::detail::make_async_result(
                [call = std::move(call),
                        server,
                        arguments = std::move(arguments),
                        update_sink = std::move(context.update_sink),
                        stop_token]() mutable
                        -> boost::asio::awaitable<support::Expected<extensions::ExtensionToolResult>> {
                    if (!call) {
                        co_return std::unexpected(support::make_error(support::ErrorCode::Validation,
                                "MCP tool from server '" + server + "' is not callable"));
                    }
                    mcp::McpServerConnection::ProgressCallback on_progress;
                    if (update_sink) {
                        on_progress = [update_sink = std::move(update_sink)](
                                              const support::JsonValue& progress) mutable {
                            (void)update_sink(agent::AsyncToolExecutionResult{
                                    .content = std::vector<ai::Content>{ai::text_content(
                                            mcp::progress_update_text(progress))},
                            });
                        };
                    }
                    auto result = co_await support::detail::await_async_result(
                            call(std::move(arguments), stop_token, std::move(on_progress)));
                    if (!result) {
                        co_return std::unexpected(std::move(result.error()));
                    }
                    co_return mcp::convert_mcp_tools_call_result(server, *result);
                });
    };
    static_cast<void>(agent_.register_tool(extensions::convert_extension_tool(std::move(extension))));
    known_names_.insert(name);
    apply_exposure(name, exposure);
    exposures_[name] = exposure;
    refresh_codemode_description();
}

void AgentMcpToolSurface::register_resource_tools(
        mcp::McpExposure exposure, std::vector<std::shared_ptr<mcp::McpResourceServer>> servers) {
    for (auto& tool : mcp::create_mcp_resource_tools(std::move(servers))) {
        const std::string name = tool.definition.name;
        static_cast<void>(agent_.register_tool(extensions::convert_extension_tool(std::move(tool))));
        known_names_.insert(name);
        apply_exposure(name, exposure);
        exposures_[name] = exposure;
    }
    refresh_codemode_description();
}

void AgentMcpToolSurface::set_active_tools(std::vector<std::string> names) {
    static_cast<void>(agent_.set_active_tools(std::move(names)));
}

std::vector<std::string> AgentMcpToolSurface::active_tools() const { return agent_.active_tools(); }

std::vector<McpSurfaceTool> AgentMcpToolSurface::all_tools() const {
    std::vector<McpSurfaceTool> tools;
    tools.reserve(known_names_.size());
    for (const auto& name : known_names_) {
        const auto exposure = exposures_.find(name);
        tools.push_back(McpSurfaceTool{
                name,
                exposure != exposures_.end() ? exposure->second : mcp::McpExposure::Hidden,
        });
    }
    return tools;
}

void AgentMcpToolSurface::refresh_codemode_description() {
    std::vector<ai::Tool> callable;
    for (auto& definition : agent_.tool_definitions()) {
        if (definition.name == extensions::kCodemodeToolName) continue;
        const auto exposure = exposures_.find(definition.name);
        if (exposure != exposures_.end() && exposure->second == mcp::McpExposure::Hidden) continue;
        callable.push_back(std::move(definition));
    }
    static_cast<void>(
            agent_.update_tool_description(extensions::kCodemodeToolName, extensions::codemode_description(callable)));
}

void AgentMcpToolSurface::apply_exposure(const std::string& name, mcp::McpExposure exposure) {
    std::vector<std::string> active = agent_.active_tools();
    const auto found = std::ranges::find(active, name);
    if (exposure == mcp::McpExposure::Direct) {
        if (found == active.end()) {
            active.push_back(name);
            static_cast<void>(agent_.set_active_tools(std::move(active)));
        }
        return;
    }
    if (found != active.end()) {
        active.erase(found);
        static_cast<void>(agent_.set_active_tools(std::move(active)));
    }
}

} // namespace cch::coding_agent::runtime
