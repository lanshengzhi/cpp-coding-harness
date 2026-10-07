#pragma once

// The `/mcp` interactive flow (spec #882, ticket #884): pi's
// `extensions/mcp/index.ts` command handler (`registerCommand("mcp")`,
// `pickServer`, `loginCommand`) and `manage` loop (`serversMenu` ->
// `serverMenu` -> `runAction`, re-presenting on manager change), plus
// `signInWithUi`'s redirect-URL screen, hosted on the landed
// `McpManagerView` + `McpManagerPresenter`. The engine dispatches the parsed
// `/mcp [action [server]]` invocation here; presentation reaches the terminal
// only through ModalPresenter, and the live manager arrives through
// McpFlowHostHooks, so a headless host drives the flow against a recording
// presenter and a scripted manager.
//
// Repository-private `cch_coding_agent` implementation header: not part of an
// Owner Interface, not installed, never exported.

#include "coding_agent/runtime/McpSessionManager.hpp"
#include "coding_agent/tui/McpManagerPresenter.hpp"
#include "coding_agent/tui/McpManagerView.hpp"
#include "coding_agent/tui/ModalPresenter.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace cch::coding_agent::tui {

class LiveTheme;
class SharedKeybindings;

/// The host seams the `/mcp` flow needs. The flow owns the manage loop, the
/// subcommand flows, and the completion snapshot; the host supplies the live
/// manager, executor marshalling, the detached-flow spawner, and the two
/// environment actions (browser open for the authorization URL, clipboard for
/// its copy row).
struct McpFlowHostHooks {
    /// Marshal one input-thread action onto the host executor. The action is
    /// dropped once the host stops running.
    std::move_only_function<void(std::move_only_function<void()>)> post_on_executor{nullptr};
    /// Spawn one detached flow coroutine on the host executor (the manager
    /// actions and the sign-in race); a failure is reported under the label.
    std::move_only_function<void(
            std::move_only_function<boost::asio::awaitable<void>()>,
            std::string failure_label)>
            spawn_flow{nullptr};
    /// Resolve the live MCP manager at execution time (the session owns it).
    /// Null when the current session has none.
    std::move_only_function<runtime::McpSessionManager*()> mcp_manager{nullptr};
    /// Resolve the live component palette when the panel opens.
    std::move_only_function<const LiveTheme&()> live_theme{nullptr};
    /// Whether the interactive host can present (running with a view).
    std::move_only_function<bool()> is_live{nullptr};
    /// Whether another modal (an overlay) is currently open.
    std::move_only_function<bool()> overlay_active{nullptr};
    /// pi `openUrl`: open the authorization URL in the browser.
    std::move_only_function<void(std::string)> open_browser{nullptr};
    /// pi `app.message.copy`: copy the authorization URL.
    std::move_only_function<void(std::string)> copy_text{nullptr};
};

/// The `/mcp` flow controller. Executor-confined like the sibling flow
/// controllers; the view's sinks fire on the input thread and are marshaled
/// through `post_on_executor`.
class McpManagerFlow final : public std::enable_shared_from_this<McpManagerFlow> {
public:
    McpManagerFlow(boost::asio::any_io_executor executor,
            ModalPresenter& presenter,
            std::weak_ptr<void> host_lifetime,
            McpFlowHostHooks hooks,
            std::shared_ptr<SharedKeybindings> keybindings);
    McpManagerFlow(McpManagerFlow&&) = delete;
    McpManagerFlow& operator=(McpManagerFlow&&) = delete;
    ~McpManagerFlow() = default;
    McpManagerFlow(const McpManagerFlow&) = delete;
    McpManagerFlow& operator=(const McpManagerFlow&) = delete;

    /// pi's `/mcp` handler with no argument: `showMcpManager` + `manage` —
    /// the servers menu loop. When the host cannot present the panel, pi's
    /// non-TUI branch prints `formatStatus()` instead.
    void open_manager();
    /// pi's non-TUI `/mcp` branch: the formatStatus lines through the status
    /// channel.
    void print_status_report();

    /// The `/mcp <argument>` dispatch entry: `login|logout|reconnect
    /// [server]`, pi's usage warning otherwise.
    void handle_command(std::string argument);
    /// pi `loginCommand` + `pickServer` over the OAuth-eligible servers.
    void run_login(std::string name);
    /// pi's logout branch with the two verbatim outcomes.
    void run_logout(std::string name);
    /// pi's reconnect branch with the verbatim success line.
    void run_reconnect(std::string name);

    /// Rebuild the `/mcp` argument-completion snapshot, (re)subscribe to the
    /// manager's changes, install the warning sink, and drain the warnings
    /// the manager latched before the sink existed. The host calls this at
    /// session bind and replacement (the manager's single listener slot makes
    /// one subscriber own both the completion refresh and the panel's
    /// re-presentation).
    void refresh_completion();
    [[nodiscard]] std::shared_ptr<const McpCompletionSnapshot> completion_snapshot() const {
        return completion_;
    }

private:
    /// Which menu/screen the panel currently shows.
    enum class Screen {
        servers,
        server,
        exposure,
        tools,
        sign_in,
        status,
    };

    struct Panel {
        std::shared_ptr<McpManagerView> view;
        Screen screen{Screen::servers};
        std::string server{};
    };

    void post(std::move_only_function<void()> action);
    runtime::McpSessionManager* manager();
    bool live();

    void show_status(std::string text);
    void show_error(std::string text);
    void show_warning(std::string text);

    void ensure_panel();
    void close_panel();
    void present_servers();
    void present_server(std::string_view name);
    void present_current_menu();
    void on_menu_confirm(std::string value);
    void on_menu_cancel();
    void on_manager_change();

    void run_server_action(std::string name, McpServerAction action);
    void run_sign_in(std::string name, bool close_after);

    /// pi `pickServer`: resolve one server by the eligibility set; every
    /// failure path reports pi's message and returns nullopt.
    struct PickOptions {
        std::function<bool(const McpServerView&)> eligible{nullptr};
        std::function<bool(const McpServerView&)> preferred{nullptr};
        std::string none_message{};
    };
    [[nodiscard]] std::optional<std::string> pick_server(const std::string& name, const PickOptions& options);

    /// The status lines for a candidate set (pi `formatStatus`), one status
    /// message per line.
    void show_status_lines(const std::vector<McpServerView>& servers);

    boost::asio::any_io_executor executor_;
    ModalPresenter* presenter_;
    std::weak_ptr<void> host_lifetime_;
    McpFlowHostHooks hooks_;
    std::shared_ptr<SharedKeybindings> keybindings_;
    std::optional<Panel> panel_;
    std::shared_ptr<const McpCompletionSnapshot> completion_;
    runtime::McpSessionManager* subscribed_manager_{nullptr};
    std::size_t drained_warnings_{0};
};

} // namespace cch::coding_agent::tui
