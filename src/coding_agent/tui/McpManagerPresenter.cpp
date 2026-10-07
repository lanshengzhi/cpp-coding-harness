// The `/mcp` manager presenter (spec #882, ticket #884): the manager-side
// producer `McpManagerView`'s seam expects. pi source at `7c10bd43` (v1.0.4):
// `packages/coding-agent/src/extensions/mcp/index.ts` (`serversMenu`,
// `serverMenu`, `showTools`, `chooseExposure`, `runAction`, `manage`).

#include "coding_agent/tui/McpManagerPresenter.hpp"

#include "support/AsyncResultBridge.hpp"

#include <boost/asio/awaitable.hpp>

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cch::coding_agent::tui {

McpServerViewState mcp_view_state(runtime::McpServerState state) {
    switch (state) {
    case runtime::McpServerState::Connecting:
        return McpServerViewState::Connecting;
    case runtime::McpServerState::Connected:
        return McpServerViewState::Connected;
    case runtime::McpServerState::Disconnected:
        return McpServerViewState::Disconnected;
    case runtime::McpServerState::NeedsAuth:
        return McpServerViewState::NeedsAuth;
    case runtime::McpServerState::Failed:
        return McpServerViewState::Failed;
    case runtime::McpServerState::Closed:
        return McpServerViewState::Closed;
    }
    return McpServerViewState::Connecting;
}

McpServerView mcp_server_view(const runtime::McpServerSnapshot& snapshot) {
    McpServerView view;
    view.entry = snapshot.entry;
    view.scope = snapshot.scope;
    view.message = snapshot.message;
    if (snapshot.connection) {
        McpConnectionView connection;
        connection.state = mcp_view_state(snapshot.connection->state);
        for (const auto& tool : snapshot.connection->tools) {
            connection.tools.push_back(McpToolView{.name = tool.name, .description = tool.description});
        }
        connection.resource_count = snapshot.connection->resource_count;
        connection.error = snapshot.connection->error;
        connection.oauth = snapshot.connection->oauth;
        view.connection = std::move(connection);
    }
    return view;
}

McpManagerPresenter::McpManagerPresenter(runtime::McpSessionManager& manager) : manager_(&manager) {}

McpManagerPresenter::~McpManagerPresenter() = default;

std::vector<McpServerView> McpManagerPresenter::servers() const {
    std::vector<McpServerView> views;
    const auto snapshots = manager_->servers();
    views.reserve(snapshots.size());
    for (const auto& snapshot : snapshots) {
        views.push_back(mcp_server_view(snapshot));
    }
    return views;
}

McpMenu McpManagerPresenter::servers_menu() const {
    return mcp_servers_menu(servers(), manager_->config_errors(), manager_->overridden(), manager_->agent_dir());
}

McpMenu McpManagerPresenter::server_menu(std::string_view name) const {
    for (const auto& view : servers()) {
        if (view.entry.name == name) {
            return mcp_server_menu(view, manager_->project_config());
        }
    }
    McpMenu missing;
    missing.title = std::string{name};
    missing.empty = "This server is no longer configured.";
    missing.cancel_label = "back";
    return missing;
}

McpMenu McpManagerPresenter::exposure_menu(std::string_view name) const {
    for (const auto& view : servers()) {
        if (view.entry.name == name) {
            return mcp_exposure_menu(view);
        }
    }
    McpMenu missing;
    missing.title = std::string{name};
    missing.empty = "This server is no longer configured.";
    return missing;
}

McpMenu McpManagerPresenter::tools_menu(std::string_view name) const {
    for (const auto& view : servers()) {
        if (view.entry.name == name) {
            return mcp_tools_menu(view);
        }
    }
    McpMenu missing;
    missing.title = std::string{name};
    missing.empty = "This server is no longer configured.";
    return missing;
}

void McpManagerPresenter::subscribe(std::function<void()> listener) {
    manager_->set_change_listener(std::move(listener));
}

support::AsyncResult<std::optional<std::string>> McpManagerPresenter::set_exposure(
        std::string_view server, mcp::McpExposure exposure) {
    return manager_->set_exposure(server, exposure);
}

support::AsyncResult<McpActionOutcome> McpManagerPresenter::run_action(
        std::string_view server, McpServerAction action, runtime::McpSignInPrompt sign_in_prompt) {
    return support::detail::make_async_result(
            [this, name = std::string{server}, action, prompt = std::move(sign_in_prompt)]() mutable
                    -> boost::asio::awaitable<support::Expected<McpActionOutcome>> {
                McpActionOutcome outcome;
                switch (action) {
                case McpServerAction::Tools:
                    outcome.next_menu = tools_menu(name);
                    break;
                case McpServerAction::Exposure:
                    outcome.next_menu = exposure_menu(name);
                    break;
                case McpServerAction::SignIn: {
                    auto result =
                            co_await support::detail::await_async_result(manager_->sign_in(name, std::move(prompt)));
                    if (!result) {
                        co_return std::unexpected(std::move(result.error()));
                    }
                    outcome.message = std::move(*result);
                    break;
                }
                case McpServerAction::Reconnect: {
                    auto result = co_await support::detail::await_async_result(manager_->reconnect(name));
                    if (!result) {
                        co_return std::unexpected(std::move(result.error()));
                    }
                    outcome.message = std::move(*result);
                    break;
                }
                case McpServerAction::SignOut: {
                    auto removed = co_await support::detail::await_async_result(manager_->sign_out(name));
                    if (!removed) {
                        co_return std::unexpected(std::move(removed.error()));
                    }
                    break;
                }
                case McpServerAction::Enable:
                case McpServerAction::Disable:
                case McpServerAction::EnableInProject:
                case McpServerAction::DisableInProject: {
                    const bool enable = action == McpServerAction::Enable || action == McpServerAction::EnableInProject;
                    const bool in_project =
                            action == McpServerAction::EnableInProject || action == McpServerAction::DisableInProject;
                    auto result = co_await support::detail::await_async_result(
                            manager_->set_enabled(name, enable, in_project));
                    if (!result) {
                        co_return std::unexpected(std::move(result.error()));
                    }
                    outcome.message = std::move(*result);
                    break;
                }
                }
                co_return outcome;
            });
}

} // namespace cch::coding_agent::tui
