#pragma once

// The `/mcp` manager presenter (spec #882, ticket #884): the manager-side
// producer `McpManagerView`'s documented seam expects. It maps the live
// `runtime::McpSessionManager` state onto the panel's `McpServerView`, builds
// each screen through the panel's pure menu builders, and executes a confirmed
// `McpServerAction` against the live manager so the host can re-present the
// panel on completion or on a manager change.
//
// Repository-private `frontend_tui` implementation header: not part of an
// Owner Interface, not installed, never exported.

#include "coding_agent/runtime/McpSessionManager.hpp"
#include "coding_agent/tui/McpManagerView.hpp"

#include <cch/support/AsyncResult.hpp>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cch::coding_agent::tui {

/// Map one manager snapshot onto the panel's server view (pi `McpServer`).
[[nodiscard]] McpServerView mcp_server_view(const runtime::McpServerSnapshot& snapshot);

/// Map pi `ServerState` onto the panel's view state (the panel's own
/// `McpServerViewState` spelling).
[[nodiscard]] McpServerViewState mcp_view_state(runtime::McpServerState state);

/// What executing one confirmed menu row produces for the host: a screen to
/// present (the `Tools`/`Exposure` sub-menus) and/or the failure message that
/// becomes `server.message`.
struct McpActionOutcome {
    std::optional<McpMenu> next_menu{std::nullopt};
    std::optional<std::string> message{std::nullopt};
};

/// The manager-driven `/mcp` producer the panel's seam expects.
class McpManagerPresenter {
public:
    explicit McpManagerPresenter(runtime::McpSessionManager& manager);

    McpManagerPresenter(McpManagerPresenter&&) = delete;
    McpManagerPresenter& operator=(McpManagerPresenter&&) = delete;
    ~McpManagerPresenter();
    McpManagerPresenter(const McpManagerPresenter&) = delete;
    McpManagerPresenter& operator=(const McpManagerPresenter&) = delete;

    /// The live server list as the panel reads it (pi `servers`).
    [[nodiscard]] std::vector<McpServerView> servers() const;

    /// pi `serversMenu()`: the manager's server list with the config notices.
    [[nodiscard]] McpMenu servers_menu() const;
    /// pi `serverMenu(name)`.
    [[nodiscard]] McpMenu server_menu(std::string_view name) const;
    /// pi `chooseExposure`'s menu for `name`.
    [[nodiscard]] McpMenu exposure_menu(std::string_view name) const;
    /// pi `showTools`'s menu for `name`.
    [[nodiscard]] McpMenu tools_menu(std::string_view name) const;

    /// pi `runAction`: run one confirmed row against the live manager.
    /// `Tools` and `Exposure` return the sub-screen for the host to present;
    /// the side-effecting actions return the failure message (`server.message`)
    /// or `std::nullopt`. `sign_in_prompt` drives the OAuth screen for
    /// `SignIn` only.
    [[nodiscard]] support::AsyncResult<McpActionOutcome> run_action(
            std::string_view server, McpServerAction action, runtime::McpSignInPrompt sign_in_prompt = {});

    /// pi `chooseExposure`'s save step: write the chosen exposure and
    /// re-register. Completes with the failure message or `std::nullopt`.
    [[nodiscard]] support::AsyncResult<std::optional<std::string>> set_exposure(
            std::string_view server, mcp::McpExposure exposure);

    /// pi `ui.menu(…, subscribe)`: register the manager-change listener the
    /// host re-presents the panel on. One listener at a time.
    void subscribe(std::function<void()> listener);

    [[nodiscard]] runtime::McpSessionManager& manager() noexcept { return *manager_; }
    [[nodiscard]] const runtime::McpSessionManager& manager() const noexcept { return *manager_; }

private:
    runtime::McpSessionManager* manager_;
};

} // namespace cch::coding_agent::tui
