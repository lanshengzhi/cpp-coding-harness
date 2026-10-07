#pragma once

// The `/mcp` manager view (spec #882, ticket #884): pi's
// `extensions/mcp/ui.ts` (`McpMenu`, `McpUi`, `McpManagerView`, `frame`) and
// the manager half of `extensions/mcp/index.ts` (`describeState`,
// `attentionRank`, `serversMenu`, `serverMenu`, `showTools`,
// `chooseExposure`, `EXPOSURE_DESCRIPTIONS`, `formatStatus`) at `7c10bd43`
// (v1.0.4). The view renders one menu, one status screen, or the sign-in
// screen; the host owns the navigation and side effects, like the sibling
// Native TUI selectors.
//
// Repository-private `cch_coding_agent` implementation header: not part of an
// Owner Interface, not installed, never exported.

#include "coding_agent/mcp/McpConfigFile.hpp"
#include "coding_agent/mcp/McpExposure.hpp"
#include "coding_agent/tui/Theme.hpp"

#include <cch/tui/Component.hpp>
#include <cch/tui/Input.hpp>
#include <cch/tui/Keybindings.hpp>
#include <cch/tui/SelectList.hpp>
#include <cch/support/Error.hpp>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cch::coding_agent::tui {

/// pi `ServerState` (`extensions/mcp/runtime.ts`): one MCP server connection's
/// lifecycle state as the manager reports it. `Disconnected` is a dropped
/// connection that reconnects lazily; `Closed` is the shutdown state.
enum class McpServerViewState {
    Connecting,
    Connected,
    Disconnected,
    NeedsAuth,
    Failed,
    Closed,
};

/// pi `Tool` as the manager reads it: the server-side name and the
/// description the tools list renders.
struct McpToolView {
    std::string name{};
    std::string description{};
};

/// pi `McpServerConnection` as the manager reads it: the state, the tools the
/// server offers, its resources, the last error, and whether it uses OAuth
/// (`connection.oauthUrl !== undefined`).
struct McpConnectionView {
    McpServerViewState state{McpServerViewState::Connecting};
    /// The server's tools, in server order.
    std::vector<McpToolView> tools{};
    /// pi `connection.resources.length`.
    std::size_t resource_count{0};
    /// pi `connection.error`; absent when the connection has no error.
    std::optional<std::string> error{std::nullopt};
    /// pi `connection.oauthUrl !== undefined`: the server can sign in.
    bool oauth{false};
};

/// pi `McpServer` (`extensions/mcp/index.ts`): a configured entry plus its
/// optional live connection and the last `/mcp` action's failure message.
struct McpServerView {
    mcp::McpConfigEntry entry{};
    /// pi `entry.scope`: `global`, `project`, `extension`, or empty for an
    /// unnamed source.
    std::optional<std::string> scope{std::nullopt};
    /// pi `connection`; absent for a disabled server or one still starting.
    std::optional<McpConnectionView> connection{std::nullopt};
    /// pi `server.message`: the last action that failed.
    std::optional<std::string> message{std::nullopt};
};

/// pi `SelectItem` for one manager row.
struct McpMenuItem {
    std::string value{};
    std::string label{};
    std::optional<std::string> description{std::nullopt};
};

/// pi `McpMenu`: one manager screen's title, detail lines, items, and the key
/// hints for confirming and cancelling.
struct McpMenu {
    std::string title{};
    /// Shown below the title (muted).
    std::optional<std::string> details{std::nullopt};
    /// Shown below the details in the error color.
    std::optional<std::string> error{std::nullopt};
    std::vector<McpMenuItem> items{};
    /// Shown when there are no items.
    std::optional<std::string> empty{std::nullopt};
    /// Value of the item selected when the menu opens.
    std::optional<std::string> selected{std::nullopt};
    /// What the confirm key does, for the key hint.
    std::string confirm_label{};
    /// What the cancel key does, for the key hint.
    std::string cancel_label{};
};

/// pi `MAX_VISIBLE_ITEMS` (`ui.ts`): the row window the servers list scrolls
/// inside.
inline constexpr std::size_t kMcpMaxVisibleItems = 12;

/// pi `MCP_USAGE` (`index.ts`).
[[nodiscard]] std::string_view mcp_usage();

/// pi `EXPOSURE_DESCRIPTIONS` (`index.ts`): the one-line description of each
/// selectable exposure. `hidden` is unreachable and has no entry.
[[nodiscard]] std::string_view mcp_exposure_description(mcp::McpExposure exposure);

/// pi `describeTransport(entry)`: the streamable-HTTP URL, or the stdio
/// command line.
[[nodiscard]] std::string mcp_describe_transport(const mcp::McpConfigEntry& entry);

/// pi `exposureOf(entry)`: the entry's configured exposure, defaulting to
/// `codemode`.
[[nodiscard]] mcp::McpExposure mcp_entry_exposure(const mcp::McpConfigEntry& entry);

/// pi `isEnabled(server)`: `entry.config.enabled !== false`.
[[nodiscard]] bool mcp_server_enabled(const McpServerView& server);

/// pi `ServerState` spelling (`disconnected`, `needs-auth`, ...).
[[nodiscard]] std::string_view mcp_server_view_state_name(McpServerViewState state);

/// pi `describeState(server, withError)`: the verbatim short state.
/// `withError` appends the failure's first line.
[[nodiscard]] std::string mcp_describe_state(const McpServerView& server, bool with_error = true);

/// pi `attentionRank(server)`: servers that need the user first.
[[nodiscard]] int mcp_attention_rank(const McpServerView& server);

/// pi `serversMenu()`: the server list, sorted by attention rank then name.
/// `config_errors` and `overridden` become the menu error (`notices()`);
/// `agent_dir` names the global `mcp.json` in the empty message.
[[nodiscard]] McpMenu mcp_servers_menu(const std::vector<McpServerView>& servers,
        const std::vector<std::string>& config_errors,
        const std::vector<std::string>& overridden,
        const std::filesystem::path& agent_dir);

/// pi `serverMenu(name)`: the per-server actions. `project_config` is pi's
/// `projectConfig !== undefined` (a trusted project's `mcp.json` exists, so a
/// global server can be enabled or disabled for the project alone).
[[nodiscard]] McpMenu mcp_server_menu(const McpServerView& server, bool project_config);

/// pi `chooseExposure`'s menu: codemode/deferred/direct with the current one
/// marked `✓ `.
[[nodiscard]] McpMenu mcp_exposure_menu(const McpServerView& server);

/// pi `showTools`'s menu: the connected server's tools.
[[nodiscard]] McpMenu mcp_tools_menu(const McpServerView& server);

/// pi `formatStatus()`: the non-TUI `/mcp` status lines. `agent_dir` names
/// the global `mcp.json` when nothing is configured.
[[nodiscard]] std::string mcp_format_status(const std::vector<McpServerView>& servers,
        const std::vector<std::string>& config_errors,
        const std::vector<std::string>& overridden,
        const std::filesystem::path& agent_dir);

/// pi `getArgumentCompletions`' item shape.
struct McpCommandCompletion {
    std::string value{};
    std::string label{};
    std::optional<std::string> description{std::nullopt};
};

/// pi's `/mcp` argument completion: action names for an incomplete first
/// token, then server names for `login`/`logout`/`reconnect`. Empty when the
/// argument has more than two tokens or the action is unknown.
[[nodiscard]] std::vector<McpCommandCompletion> mcp_command_completions(
        std::string_view prefix, const std::vector<McpServerView>& servers);

/// pi `serverMenu`'s action values: the operation the manager runs for the
/// selected server row. The `/mcp` wiring lane implements each one against
/// the live server manager and re-presents the panel when it finishes; the
/// panel itself only reports the value.
enum class McpServerAction {
    SignIn,
    Tools,
    Reconnect,
    SignOut,
    Exposure,
    Disable,
    Enable,
    DisableInProject,
    EnableInProject,
};

/// The pi `SelectItem.value` spelling of one server action (for example
/// `reconnect`, `disable-project`); the value `McpManagerView`'s confirm sink
/// reports.
[[nodiscard]] std::string_view mcp_server_action_value(McpServerAction action);

/// The action a server-menu row's value names, or `std::nullopt` when the
/// value is not a server action.
[[nodiscard]] std::optional<McpServerAction> parse_mcp_server_action(std::string_view value);

/// Fired when a menu row is confirmed, with the row's value.
using McpMenuConfirmSink = std::move_only_function<void(std::string)>;
/// Fired when a menu is cancelled (pi `onCancel`).
using McpMenuCancelSink = std::move_only_function<void()>;
/// Fired when the sign-in screen submits a pasted redirect URL.
using McpRedirectSubmitSink = std::move_only_function<void(std::string)>;
/// Fired when the sign-in screen is cancelled, or when the flow aborts it
/// because the browser already reached the callback.
using McpRedirectCancelSink = std::move_only_function<void()>;
/// pi `AuthUrlComponent`'s `app.message.copy` effect: copy the authorization
/// URL. Defaults to the platform clipboard writer.
using McpCopySink = std::move_only_function<void(std::string)>;

/// The `/mcp` manager view (pi `McpManagerView`): the framed menu screen, the
/// status screen, and the sign-in screen.
///
/// The frame is pi's `frame()`: an accent rule, the accent-bold title, the
/// body, and the dim footer, closed by an accent rule. The body is the
/// details and error lines, a spacer, then the `cch::tui::SelectList` of at
/// most `kMcpMaxVisibleItems` rows (or the empty message), and the key hints.
///
/// The manager seam this view is designed against is pi's `manage` loop
/// (`extensions/mcp/index.ts`): the wiring lane owns the live server list,
/// builds each screen with `mcp_servers_menu`/`mcp_server_menu`/
/// `mcp_exposure_menu`/`mcp_tools_menu` over live `McpServerView`s, presents
/// it here, executes the confirmed `McpServerAction` (or the selected server
/// name) against the live manager, and re-presents on completion or on a
/// manager change. The view holds no server state itself beyond the screen it
/// is showing.
///
/// Threading: constructed and driven on the TUI thread; the sinks fire on the
/// input thread like every sibling selector.
class McpManagerView final : public cch::tui::Component, public cch::tui::InputHandler, public cch::tui::Focusable {
public:
    McpManagerView(const LiveTheme& theme,
            std::shared_ptr<const cch::tui::KeybindingRegistry> keybindings,
            McpMenuConfirmSink on_confirm,
            McpMenuCancelSink on_cancel,
            McpCopySink on_copy = {});

    McpManagerView(McpManagerView&&) = delete;
    McpManagerView& operator=(McpManagerView&&) = delete;
    ~McpManagerView() override;
    McpManagerView(const McpManagerView&) = delete;
    McpManagerView& operator=(const McpManagerView&) = delete;

    /// pi `menu()`: show `menu` and resolve to the confirmed item's value or
    /// the cancel sink.
    void show_menu(McpMenu menu);
    /// pi `status()`: show a message while an operation runs.
    void show_status(std::string title, std::string message);
    /// pi `redirectUrl()`: show the authorization URL and wait for a pasted
    /// redirect URL. The submit sink receives the trimmed non-empty value.
    void show_redirect_url(std::string title,
            std::string authorization_url,
            McpRedirectSubmitSink on_submit,
            McpRedirectCancelSink on_redirect_cancel);

    /// pi `setContent`: the screen title currently shown (tests and hosts).
    [[nodiscard]] const std::string& title() const noexcept;
    /// The authorization URL the sign-in screen displays.
    [[nodiscard]] std::string_view authorization_url() const noexcept;
    /// The paste input of the sign-in screen (pi `RedirectInput` seam).
    [[nodiscard]] cch::tui::Input& redirect_input() noexcept;

    [[nodiscard]] support::Expected<cch::tui::RenderResult> render(std::size_t width) override;
    void invalidate() override;
    cch::tui::InputAdmissionOutcome handle_input(const cch::tui::InputEventVariant& input) override;
    void set_focused(bool focused) override;
    [[nodiscard]] bool focused() const override;
    [[nodiscard]] std::optional<cch::tui::CursorPosition> cursor_location() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cch::coding_agent::tui
