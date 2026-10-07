// pi `extensions/mcp/ui.ts` + the `/mcp` manager half of `index.ts` (spec
// #882, ticket #884): the verbatim state descriptions, the attention-ranked
// server list, the per-server actions and their save-location text, the
// exposure chooser, the tools list, the non-TUI status lines, the `/mcp`
// subcommand completion, and the framed view (border, accent-bold title, dim
// footer) with its sign-in screen.

#include "coding_agent/tui/KeybindingsManager.hpp"
#include "coding_agent/tui/McpManagerView.hpp"
#include "coding_agent/tui/Theme.hpp"

#include <cch/tui/Keybindings.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <filesystem>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace cch;

namespace {

[[nodiscard]] std::shared_ptr<const tui::KeybindingRegistry> test_keybindings() {
    tui::KeybindingResolutionRequest request;
    request.definitions = tui::builtin_tui_keybinding_definitions();
    const std::array<std::string_view, 1> copy_action{"app.message.copy"};
    auto application = coding_agent::tui::app_keybinding_definitions(copy_action);
    REQUIRE(application);
    request.definitions.insert(request.definitions.end(),
            std::make_move_iterator(application->begin()),
            std::make_move_iterator(application->end()));
    auto resolved = tui::resolve_keybindings(std::move(request));
    REQUIRE(resolved);
    return resolved->registry;
}

[[nodiscard]] coding_agent::tui::LiveTheme test_theme() {
    return coding_agent::tui::LiveTheme(
            coding_agent::tui::builtin_dark_theme(), tui::TerminalColorCapability::TrueColor);
}

/// Remove SGR (`ESC [ … final`) and OSC hyperlink (`ESC ] … BEL`) sequences
/// so assertions target the visible text.
[[nodiscard]] std::string strip_ansi(std::string_view text) {
    std::string stripped;
    stripped.reserve(text.size());
    for (std::size_t index = 0; index < text.size();) {
        if (text[index] == '\x1b' && index + 1 < text.size() && text[index + 1] == '[') {
            index += 2;
            while (index < text.size() && !(text[index] >= '@' && text[index] <= '~'))
                ++index;
            if (index < text.size()) ++index;
            continue;
        }
        if (text[index] == '\x1b' && index + 1 < text.size() && text[index + 1] == ']') {
            index += 2;
            while (index < text.size() && text[index] != '\a')
                ++index;
            if (index < text.size()) ++index;
            continue;
        }
        stripped.push_back(text[index]);
        ++index;
    }
    return stripped;
}

[[nodiscard]] std::string rstrip(std::string text) {
    const auto last = text.find_last_not_of(' ');
    if (last == std::string::npos) return {};
    return text.substr(0, last + 1);
}

[[nodiscard]] std::string rule_line(std::size_t width) {
    std::string rule;
    for (std::size_t index = 0; index < width; ++index)
        rule += "─";
    return rule;
}

[[nodiscard]] std::vector<std::string> screen_of(cch::tui::Component& component, std::size_t width = 80) {
    auto rendered = component.render(width);
    REQUIRE(rendered);
    std::vector<std::string> lines;
    lines.reserve(rendered->lines.size());
    for (const auto& line : rendered->lines)
        lines.push_back(rstrip(strip_ansi(line)));
    return lines;
}

[[nodiscard]] bool screen_contains(const std::vector<std::string>& screen, std::string_view needle) {
    for (const auto& line : screen) {
        if (line.find(needle) != std::string::npos) return true;
    }
    return false;
}

[[nodiscard]] coding_agent::mcp::McpConfigEntry stdio_entry(
        std::string name, bool enabled, coding_agent::mcp::McpExposure exposure) {
    coding_agent::mcp::McpStdioServerConfig config;
    config.name = name;
    config.command = "npx";
    config.args = {"-y", "server"};
    config.exposure = exposure;
    coding_agent::mcp::McpConfigEntry entry;
    entry.name = std::move(name);
    entry.enabled = enabled;
    entry.source = "/home/u/.pi/agent/mcp.json";
    entry.config = std::move(config);
    return entry;
}

[[nodiscard]] coding_agent::mcp::McpConfigEntry http_entry(std::string name, bool oauth) {
    coding_agent::mcp::McpHttpServerConfig config;
    config.name = name;
    config.url = "https://mcp.example.com/mcp";
    if (oauth) config.oauth = coding_agent::mcp::McpOAuthConfig{};
    coding_agent::mcp::McpConfigEntry entry;
    entry.name = std::move(name);
    entry.source = "/home/u/.pi/agent/mcp.json";
    entry.config = std::move(config);
    return entry;
}

[[nodiscard]] coding_agent::tui::McpServerView server(coding_agent::mcp::McpConfigEntry entry,
        std::optional<std::string> scope,
        std::optional<coding_agent::tui::McpConnectionView> connection,
        std::optional<std::string> override_path = std::nullopt,
        std::optional<std::string> message = std::nullopt) {
    coding_agent::tui::McpServerView view;
    view.entry = std::move(entry);
    view.entry.override =
            override_path ? std::optional<std::filesystem::path>{std::filesystem::path{*override_path}} : std::nullopt;
    view.scope = std::move(scope);
    view.connection = std::move(connection);
    view.message = std::move(message);
    return view;
}

[[nodiscard]] coding_agent::tui::McpConnectionView connection(coding_agent::tui::McpServerViewState state,
        std::vector<std::string> tools = {},
        std::size_t resources = 0,
        std::optional<std::string> error = std::nullopt,
        bool oauth = false) {
    coding_agent::tui::McpConnectionView view;
    view.state = state;
    for (auto& tool : tools)
        view.tools.push_back({.name = std::move(tool), .description = {}});
    view.resource_count = resources;
    view.error = std::move(error);
    view.oauth = oauth;
    return view;
}

[[nodiscard]] std::string menu_value(const coding_agent::tui::McpMenu& menu, std::size_t index) {
    REQUIRE(index < menu.items.size());
    return menu.items[index].value;
}

[[nodiscard]] std::string menu_description(const coding_agent::tui::McpMenu& menu, std::size_t index) {
    REQUIRE(index < menu.items.size());
    REQUIRE(menu.items[index].description);
    return *menu.items[index].description;
}

void press(cch::tui::InputHandler& handler, cch::tui::KeyEvent key) {
    static_cast<void>(handler.handle_input(cch::tui::InputEventVariant{std::move(key)}));
}

void type(cch::tui::InputHandler& handler, std::string_view text) {
    for (const char ch : text) {
        press(handler, cch::tui::KeyEvent{.key = std::string(1, ch)});
    }
}

} // namespace

TEST_CASE("McpManagerView describeState renders pi's verbatim state descriptions",
        "[coding_agent][tui][mcp][issue884][spec]") {
    using coding_agent::tui::mcp_describe_state;
    using coding_agent::tui::McpServerViewState;

    CHECK(mcp_describe_state(
                  server(stdio_entry("a", false, coding_agent::mcp::McpExposure::Codemode), "global", std::nullopt)) ==
            "disabled");
    CHECK(mcp_describe_state(
                  server(stdio_entry("a", true, coding_agent::mcp::McpExposure::Codemode), "global", std::nullopt)) ==
            "starting");
    CHECK(mcp_describe_state(server(stdio_entry("a", true, coding_agent::mcp::McpExposure::Codemode),
                  "global",
                  connection(McpServerViewState::NeedsAuth))) == "needs sign-in");
    CHECK(mcp_describe_state(server(stdio_entry("a", true, coding_agent::mcp::McpExposure::Codemode),
                  "global",
                  connection(McpServerViewState::Failed, {}, 0, std::string{"boom\nsecond line"}))) == "failed: boom");
    CHECK(mcp_describe_state(server(stdio_entry("a", true, coding_agent::mcp::McpExposure::Codemode),
                                     "global",
                                     connection(McpServerViewState::Failed, {}, 0, std::string{"boom\nsecond line"})),
                  /* with_error */ false) == "failed");
    CHECK(mcp_describe_state(server(stdio_entry("a", true, coding_agent::mcp::McpExposure::Codemode),
                  "global",
                  connection(McpServerViewState::Failed))) == "failed: unknown error");
    CHECK(mcp_describe_state(server(stdio_entry("a", true, coding_agent::mcp::McpExposure::Codemode),
                  "global",
                  connection(McpServerViewState::Connected, {"one"}))) == "connected · 1 tool");
    CHECK(mcp_describe_state(server(stdio_entry("a", true, coding_agent::mcp::McpExposure::Codemode),
                  "global",
                  connection(McpServerViewState::Connected, {"one", "two"}, 1))) == "connected · 2 tools · 1 resource");
    CHECK(mcp_describe_state(server(stdio_entry("a", true, coding_agent::mcp::McpExposure::Codemode),
                  "global",
                  connection(McpServerViewState::Connecting))) == "connecting…");
    CHECK(mcp_describe_state(server(stdio_entry("a", true, coding_agent::mcp::McpExposure::Codemode),
                  "global",
                  connection(McpServerViewState::Disconnected))) == "disconnected");
}

TEST_CASE("McpManagerView sorts the server list by attention rank", "[coding_agent][tui][mcp][issue884][spec]") {
    using coding_agent::tui::McpServerViewState;
    std::vector<coding_agent::tui::McpServerView> servers;
    servers.push_back(
            server(stdio_entry("disabled", false, coding_agent::mcp::McpExposure::Codemode), "global", std::nullopt));
    servers.push_back(server(stdio_entry("needs", true, coding_agent::mcp::McpExposure::Codemode),
            "global",
            connection(McpServerViewState::NeedsAuth)));
    servers.push_back(server(stdio_entry("failed", true, coding_agent::mcp::McpExposure::Codemode),
            "global",
            connection(McpServerViewState::Failed)));
    servers.push_back(server(stdio_entry("dropped", true, coding_agent::mcp::McpExposure::Codemode),
            "global",
            connection(McpServerViewState::Disconnected)));
    servers.push_back(
            server(stdio_entry("starting", true, coding_agent::mcp::McpExposure::Codemode), "global", std::nullopt));
    servers.push_back(server(stdio_entry("connected", true, coding_agent::mcp::McpExposure::Codemode),
            "global",
            connection(McpServerViewState::Connected)));

    const auto menu = coding_agent::tui::mcp_servers_menu(servers, {}, {}, "/home/u/.pi/agent");
    REQUIRE(menu.items.size() == 6);
    CHECK(menu.items[0].value == "needs");
    CHECK(menu.items[1].value == "failed");
    CHECK(menu.items[2].value == "dropped");
    CHECK(menu.items[3].value == "starting");
    CHECK(menu.items[4].value == "connected");
    CHECK(menu.items[5].value == "disabled");
}

TEST_CASE("McpManagerView server row renders state, exposure and source", "[coding_agent][tui][mcp][issue884][spec]") {
    using coding_agent::tui::McpServerViewState;
    std::vector<coding_agent::tui::McpServerView> servers;
    servers.push_back(server(stdio_entry("plain", true, coding_agent::mcp::McpExposure::Deferred),
            "global",
            connection(McpServerViewState::Connected, {"one"})));
    servers.push_back(server(stdio_entry("local", true, coding_agent::mcp::McpExposure::Direct),
            "project",
            connection(McpServerViewState::Connected)));
    servers.push_back(server(stdio_entry("over", true, coding_agent::mcp::McpExposure::Codemode),
            "global",
            connection(McpServerViewState::Connected),
            "/repo/.pi/mcp.json"));
    const auto menu = coding_agent::tui::mcp_servers_menu(servers, {}, {}, "/home/u/.pi/agent");

    CHECK(menu.title == "MCP servers");
    CHECK(menu.confirm_label == "manage");
    CHECK(menu.cancel_label == "close");
    REQUIRE(menu.items.size() == 3);
    // Connected servers share the same attention rank, so the list is name-ordered.
    CHECK(menu.items[0].label == "local");
    CHECK(menu_description(menu, 0) == "connected · 0 tools · direct · project");
    CHECK(menu.items[1].label == "over");
    CHECK(menu_description(menu, 1) == "connected · 0 tools · codemode · global, project override");
    CHECK(menu.items[2].label == "plain");
    CHECK(menu_description(menu, 2) == "connected · 1 tool · deferred · global");
}

TEST_CASE(
        "McpManagerView empty server list names the configuration files", "[coding_agent][tui][mcp][issue884][spec]") {
    const auto menu = coding_agent::tui::mcp_servers_menu({}, {}, {}, "/home/u/.pi/agent");
    REQUIRE(menu.empty);
    CHECK(*menu.empty == "No MCP servers configured. Add them to /home/u/.pi/agent/mcp.json or .pi/mcp.json.");

    const auto with_notices = coding_agent::tui::mcp_servers_menu({}, {"bad entry"}, {"other"}, "/home/u/.pi/agent");
    REQUIRE(with_notices.error);
    CHECK(*with_notices.error == "config: bad entry\noverridden: other");
}

TEST_CASE("McpManagerView server menu lists pi's actions with the save-location text",
        "[coding_agent][tui][mcp][issue884][spec]") {
    using coding_agent::tui::McpServerViewState;
    const auto view = server(http_entry("remote", true),
            "global",
            connection(McpServerViewState::Connected, {"one"}, 0, std::nullopt, /* oauth */ true));
    const auto menu = coding_agent::tui::mcp_server_menu(view, /* project_config */ true);

    CHECK(menu.title == "MCP server remote");
    REQUIRE(menu.details);
    // pi's `describeState(server, false)` suppresses only the failure text, so
    // a connected server still reports its tool count here.
    CHECK(*menu.details ==
            "https://mcp.example.com/mcp\nglobal: /home/u/.pi/agent/mcp.json\nState: connected · 1 tool");
    REQUIRE(menu.items.size() == 6);
    CHECK(menu_value(menu, 0) == "tools");
    CHECK(menu.items[0].label == "Tools");
    CHECK(menu_description(menu, 0) == "1 offered");
    CHECK(menu_value(menu, 1) == "reconnect");
    CHECK(menu_value(menu, 2) == "signout");
    CHECK(menu_description(menu, 2) == "deletes the stored credentials");
    CHECK(menu_value(menu, 3) == "exposure");
    CHECK(menu_description(menu, 3) == "codemode");
    CHECK(menu_value(menu, 4) == "disable");
    CHECK(menu_description(menu, 4) == "saved to the global mcp.json");
    CHECK(menu_value(menu, 5) == "disable-project");
    CHECK(menu_description(menu, 5) == "saved to the project mcp.json");
    REQUIRE(menu.selected);
    CHECK(*menu.selected == "tools");
    CHECK(menu.confirm_label == "select");
    CHECK(menu.cancel_label == "back");
}

TEST_CASE("McpManagerView disabled server offers only Enable and its project variant",
        "[coding_agent][tui][mcp][issue884][spec]") {
    const auto view =
            server(stdio_entry("off", false, coding_agent::mcp::McpExposure::Codemode), "global", std::nullopt);
    const auto menu = coding_agent::tui::mcp_server_menu(view, /* project_config */ true);
    REQUIRE(menu.items.size() == 2);
    CHECK(menu_value(menu, 0) == "enable");
    CHECK(menu_description(menu, 0) == "saved to the global mcp.json");
    CHECK(menu_value(menu, 1) == "enable-project");
    CHECK(*menu.items[1].description == "saved to the project mcp.json");

    const auto without_project = coding_agent::tui::mcp_server_menu(view, /* project_config */ false);
    REQUIRE(without_project.items.size() == 1);
    CHECK(menu_value(without_project, 0) == "enable");
}

TEST_CASE("McpManagerView needs-auth server offers Sign in, Reconnect and Exposure",
        "[coding_agent][tui][mcp][issue884][spec]") {
    using coding_agent::tui::McpServerViewState;
    const auto view = server(http_entry("remote", true),
            "global",
            connection(McpServerViewState::NeedsAuth, {}, 0, std::string{"401 Unauthorized"}, true));
    const auto menu = coding_agent::tui::mcp_server_menu(view, /* project_config */ false);

    REQUIRE(menu.items.size() == 4);
    CHECK(menu_value(menu, 0) == "signin");
    CHECK(menu.items[0].label == "Sign in");
    CHECK(menu_description(menu, 0) == "opens the browser");
    CHECK(menu_value(menu, 1) == "reconnect");
    CHECK(menu_value(menu, 2) == "exposure");
    CHECK(menu_value(menu, 3) == "disable");
    REQUIRE(menu.error);
    CHECK(*menu.error == "401 Unauthorized");
}

TEST_CASE("McpManagerView session-registered server reports its save location",
        "[coding_agent][tui][mcp][issue884][spec]") {
    using coding_agent::tui::McpServerViewState;
    const auto view = server(stdio_entry("ext", true, coding_agent::mcp::McpExposure::Codemode),
            "extension",
            connection(McpServerViewState::Connected));
    const auto menu = coding_agent::tui::mcp_server_menu(view, /* project_config */ false);
    CHECK(menu_value(menu, 0) == "tools");
    CHECK(menu_description(menu, menu.items.size() - 1) == "for this session");

    const auto exposure = coding_agent::tui::mcp_exposure_menu(view);
    REQUIRE(exposure.details);
    CHECK(*exposure.details == "Applies to this session; the server is registered by /home/u/.pi/agent/mcp.json.");
}

TEST_CASE("McpManagerView exposure chooser marks the current exposure and lists pi's descriptions",
        "[coding_agent][tui][mcp][issue884][spec]") {
    using coding_agent::tui::McpServerViewState;
    const auto view = server(stdio_entry("srv", true, coding_agent::mcp::McpExposure::Deferred),
            "global",
            connection(McpServerViewState::Connected));
    const auto menu = coding_agent::tui::mcp_exposure_menu(view);

    CHECK(menu.title == "Exposure of srv");
    REQUIRE(menu.details);
    CHECK(*menu.details == "Saved to /home/u/.pi/agent/mcp.json.");
    REQUIRE(menu.items.size() == 3);
    CHECK(menu.items[0].label == "  codemode");
    CHECK(menu.items[1].label == "✓ deferred");
    CHECK(menu.items[2].label == "  direct");
    CHECK(*menu.items[0].description == "called from codemode scripts, which find them with searchTools()");
    CHECK(*menu.items[1].description ==
            "not declared until tool_search loads them, then called directly; no codemode needed");
    CHECK(*menu.items[2].description == "declared to the model like built-in tools");
    REQUIRE(menu.selected);
    CHECK(*menu.selected == "deferred");
    CHECK(menu.confirm_label == "save");
    CHECK(menu.cancel_label == "back");
}

TEST_CASE("McpManagerView tools list renders the server's tools and its empty message",
        "[coding_agent][tui][mcp][issue884][spec]") {
    using coding_agent::tui::McpServerViewState;
    coding_agent::tui::McpConnectionView conn;
    conn.state = McpServerViewState::Connected;
    conn.tools.push_back({.name = "echo", .description = "Echo text\nwith detail"});
    conn.tools.push_back({.name = "sum", .description = "Add numbers"});
    const auto view =
            server(stdio_entry("srv", true, coding_agent::mcp::McpExposure::Codemode), "global", std::move(conn));
    const auto menu = coding_agent::tui::mcp_tools_menu(view);

    CHECK(menu.title == "Tools of srv");
    REQUIRE(menu.details);
    CHECK(*menu.details == "Exposure codemode: called from codemode scripts, which find them with searchTools()");
    REQUIRE(menu.items.size() == 2);
    CHECK(menu.items[0].label == "echo");
    CHECK(*menu.items[0].description == "Echo text");
    CHECK(menu.empty);
    CHECK(*menu.empty == "The server offers no tools.");
    CHECK(menu.confirm_label == "back");
    CHECK(menu.cancel_label == "back");

    const auto hidden = coding_agent::tui::mcp_tools_menu(
            server(stdio_entry("h", true, coding_agent::mcp::McpExposure::Hidden), "global", std::nullopt));
    REQUIRE(hidden.details);
    CHECK(*hidden.details == "Exposure hidden: unreachable");
}

TEST_CASE("McpManagerView tools list marks a tool whose toolExposure overrides the server's",
        "[coding_agent][tui][mcp][issue884][spec]") {
    using coding_agent::tui::McpServerViewState;
    auto entry = stdio_entry("srv", true, coding_agent::mcp::McpExposure::Codemode);
    std::get<coding_agent::mcp::McpStdioServerConfig>(entry.config).tool_exposure = {
            {"sum", coding_agent::mcp::McpExposure::Deferred}};
    coding_agent::tui::McpConnectionView conn;
    conn.state = McpServerViewState::Connected;
    conn.tools.push_back({.name = "echo", .description = "Echo text"});
    conn.tools.push_back({.name = "sum", .description = "Add numbers"});

    const auto menu = coding_agent::tui::mcp_tools_menu(server(std::move(entry), "global", std::move(conn)));
    REQUIRE(menu.details);
    CHECK(*menu.details == "Exposure codemode: called from codemode scripts, which find them with searchTools()\n"
                           "Some tools override it with toolExposure.");
    REQUIRE(menu.items.size() == 2);
    CHECK(*menu.items[0].description == "Echo text");
    CHECK(*menu.items[1].description == "[deferred] Add numbers");
}

TEST_CASE("McpManagerView plain status prints pi's formatStatus lines", "[coding_agent][tui][mcp][issue884][spec]") {
    using coding_agent::tui::McpServerViewState;
    std::vector<coding_agent::tui::McpServerView> servers;
    servers.push_back(server(stdio_entry("connected", true, coding_agent::mcp::McpExposure::Codemode),
            "global",
            connection(McpServerViewState::Connected, {"a", "b"})));
    servers.push_back(server(http_entry("auth", true),
            "global",
            connection(McpServerViewState::NeedsAuth, {}, 0, std::string{"401"}, true)));
    servers.push_back(
            server(stdio_entry("off", false, coding_agent::mcp::McpExposure::Deferred), "global", std::nullopt));
    servers.push_back(server(stdio_entry("dropped", true, coding_agent::mcp::McpExposure::Direct),
            "global",
            connection(McpServerViewState::Disconnected)));
    servers.push_back(server(stdio_entry("bad", true, coding_agent::mcp::McpExposure::Codemode),
            "global",
            connection(McpServerViewState::Failed, {}, 0, std::string{"line one\nline two"})));

    const auto status =
            coding_agent::tui::mcp_format_status(servers, {"config problem"}, {"registered x"}, "/home/u/.pi/agent");
    CHECK(status == "connected: connected, 2 tools (codemode)\n"
                    "auth: needs sign-in, run /mcp login auth (codemode)\n"
                    "off: disabled (deferred)\n"
                    "dropped: disconnected, reconnects on next call (direct)\n"
                    "bad: failed (codemode)\n    line one\n    line two\n"
                    "config error: config problem\n"
                    "overridden: registered x");

    CHECK(coding_agent::tui::mcp_format_status({}, {}, {}, "/home/u/.pi/agent") ==
            "No MCP servers configured. Add them to /home/u/.pi/agent/mcp.json or .pi/mcp.json.");
}

TEST_CASE("McpManagerView command completions offer actions then eligible server names",
        "[coding_agent][tui][mcp][issue884][spec]") {
    using coding_agent::tui::McpServerViewState;
    std::vector<coding_agent::tui::McpServerView> servers;
    servers.push_back(server(http_entry("remote", true),
            "global",
            connection(McpServerViewState::Connected, {}, 0, std::nullopt, true)));
    servers.push_back(server(stdio_entry("local", true, coding_agent::mcp::McpExposure::Codemode),
            "global",
            connection(McpServerViewState::Disconnected)));

    const auto actions = coding_agent::tui::mcp_command_completions("", servers);
    REQUIRE(actions.size() == 3);
    CHECK(actions[0].value == "login ");
    CHECK(actions[0].label == "login");
    CHECK(actions[1].value == "logout ");
    CHECK(actions[2].value == "reconnect ");

    const auto filtered = coding_agent::tui::mcp_command_completions("lo", servers);
    REQUIRE(filtered.size() == 2);
    CHECK(filtered[0].value == "login ");
    CHECK(filtered[1].value == "logout ");

    const auto login = coding_agent::tui::mcp_command_completions("login ", servers);
    REQUIRE(login.size() == 1);
    CHECK(login[0].value == "login remote");
    CHECK(login[0].label == "remote");
    REQUIRE(login[0].description);
    CHECK(*login[0].description == "connected · 0 tools");

    const auto reconnect = coding_agent::tui::mcp_command_completions("reconnect ", servers);
    REQUIRE(reconnect.size() == 2);

    const auto partial = coding_agent::tui::mcp_command_completions("login re", servers);
    REQUIRE(partial.size() == 1);
    CHECK(partial[0].value == "login remote");

    const auto unknown = coding_agent::tui::mcp_command_completions("nope x", servers);
    CHECK(unknown.empty());
    const auto overflow = coding_agent::tui::mcp_command_completions("login a b", servers);
    CHECK(overflow.empty());
}

TEST_CASE("McpManagerView usage matches pi's /mcp usage line", "[coding_agent][tui][mcp][issue884][spec]") {
    CHECK(coding_agent::tui::mcp_usage() ==
            "Usage: /mcp, /mcp login [server], /mcp logout [server], /mcp reconnect [server]");
}

TEST_CASE("McpManagerView server action vocabulary round-trips and rejects non-actions",
        "[coding_agent][tui][mcp][issue884][spec]") {
    using coding_agent::tui::mcp_server_action_value;
    using coding_agent::tui::McpServerAction;
    using coding_agent::tui::parse_mcp_server_action;

    CHECK(mcp_server_action_value(McpServerAction::SignIn) == "signin");
    CHECK(mcp_server_action_value(McpServerAction::Reconnect) == "reconnect");
    CHECK(mcp_server_action_value(McpServerAction::DisableInProject) == "disable-project");
    for (const auto action : {McpServerAction::SignIn,
                 McpServerAction::Tools,
                 McpServerAction::Reconnect,
                 McpServerAction::SignOut,
                 McpServerAction::Exposure,
                 McpServerAction::Disable,
                 McpServerAction::Enable,
                 McpServerAction::DisableInProject,
                 McpServerAction::EnableInProject}) {
        CHECK(parse_mcp_server_action(mcp_server_action_value(action)) == action);
    }
    CHECK(parse_mcp_server_action("my-server") == std::nullopt);
    CHECK(parse_mcp_server_action("") == std::nullopt);
}

TEST_CASE("McpManagerView frames the servers menu with the accent title and dim footer",
        "[coding_agent][tui][mcp][issue884][spec]") {
    auto theme = test_theme();
    std::optional<std::string> confirmed;
    bool cancelled = false;
    coding_agent::tui::McpManagerView view(
            theme,
            test_keybindings(),
            [&confirmed](std::string value) { confirmed = std::move(value); },
            [&cancelled]() { cancelled = true; });

    std::vector<coding_agent::tui::McpServerView> servers;
    servers.push_back(server(http_entry("remote", true),
            "global",
            connection(coding_agent::tui::McpServerViewState::Connected, {"one"}, 0, std::nullopt, true)));
    servers.push_back(server(stdio_entry("local", true, coding_agent::mcp::McpExposure::Direct),
            "global",
            connection(coding_agent::tui::McpServerViewState::Disconnected)));
    view.show_menu(coding_agent::tui::mcp_servers_menu(servers, {}, {}, "/home/u/.pi/agent"));

    const auto screen = screen_of(view);
    REQUIRE(screen.size() >= 8);
    CHECK(screen.front() == rule_line(80));
    CHECK(screen[1] == " MCP servers");
    CHECK(screen_contains(screen, "remote"));
    CHECK(screen_contains(screen, "connected · 1 tool · codemode · global"));
    CHECK(screen_contains(screen, "enter manage • escape/ctrl+c close"));
    CHECK(screen.back() == screen.front());

    // The title is accent-bold: the raw line carries the bold SGR and the
    // accent color.
    const auto raw_title = view.render(80)->lines[1];
    CHECK(raw_title.find("\x1b[1m") != std::string::npos);

    // `local` (disconnected) outranks `remote` (connected), so it is selected first.
    press(view, cch::tui::KeyEvent{.key = "enter"});
    REQUIRE(confirmed);
    CHECK(*confirmed == "local");

    press(view, cch::tui::KeyEvent{.key = "escape"});
    CHECK(cancelled);
}

TEST_CASE("McpManagerView empty menu shows the empty message and only the cancel hint",
        "[coding_agent][tui][mcp][issue884][spec]") {
    auto theme = test_theme();
    bool cancelled = false;
    coding_agent::tui::McpManagerView view(
            theme, test_keybindings(), [](std::string) {}, [&cancelled]() { cancelled = true; });
    view.show_menu(coding_agent::tui::mcp_servers_menu({}, {}, {}, "/home/u/.pi/agent"));

    const auto screen = screen_of(view);
    // The full message wraps at 80 columns; the menu's `empty` field carries it
    // verbatim (covered by the servers-menu case above).
    CHECK(screen_contains(screen, "No MCP servers configured."));
    CHECK(screen_contains(screen, "/home/u/.pi/agent/mcp.json or"));
    CHECK(screen_contains(screen, ".pi/mcp.json."));
    CHECK(screen_contains(screen, "escape/ctrl+c close"));
    CHECK(!screen_contains(screen, "manage"));

    press(view, cch::tui::KeyEvent{.key = "escape"});
    CHECK(cancelled);
}

TEST_CASE("McpManagerView window caps at twelve visible rows", "[coding_agent][tui][mcp][issue884][spec]") {
    auto theme = test_theme();
    coding_agent::tui::McpManagerView view(theme, test_keybindings(), [](std::string) {}, [] {});
    std::vector<coding_agent::tui::McpServerView> servers;
    for (std::size_t index = 0; index < 20; ++index) {
        servers.push_back(server(
                stdio_entry(std::string{"srv"} + std::to_string(index), true, coding_agent::mcp::McpExposure::Codemode),
                "global",
                connection(coding_agent::tui::McpServerViewState::Connected)));
    }
    view.show_menu(coding_agent::tui::mcp_servers_menu(servers, {}, {}, "/home/u/.pi/agent"));

    const auto screen = screen_of(view);
    std::size_t visible_rows = 0;
    for (const auto& line : screen) {
        if (line.find("srv") != std::string::npos) ++visible_rows;
    }
    CHECK(visible_rows == 12);
    CHECK(screen_contains(screen, "(1/20)"));
}

TEST_CASE("McpManagerView status screen shows the title and message without a footer",
        "[coding_agent][tui][mcp][issue884][spec]") {
    auto theme = test_theme();
    coding_agent::tui::McpManagerView view(theme, test_keybindings(), [](std::string) {}, [] {});
    view.show_status("MCP server remote", "Reconnecting…");

    CHECK(view.title() == "MCP server remote");
    const auto screen = screen_of(view);
    CHECK(screen_contains(screen, "MCP server remote"));
    CHECK(screen_contains(screen, "Reconnecting…"));
    CHECK(!screen_contains(screen, "enter"));
    CHECK(!screen_contains(screen, "escape"));
}

TEST_CASE("McpManagerView sign-in screen shows the URL, the paste prompt and submit/cancel hints",
        "[coding_agent][tui][mcp][issue884][spec]") {
    auto theme = test_theme();
    coding_agent::tui::McpManagerView view(theme, test_keybindings(), [](std::string) {}, [] {});
    std::optional<std::string> submitted;
    bool cancelled = false;
    std::optional<std::string> copied;
    // The copy sink is a construction-time seam; a second view exercises it.
    coding_agent::tui::McpManagerView copying(
            theme, test_keybindings(), [](std::string) {}, [] {}, [&copied](std::string url) { copied = url; });

    view.show_redirect_url(
            "Sign in to remote",
            "https://auth.example.com/authorize?code=abc",
            [&submitted](std::string value) { submitted = std::move(value); },
            [&cancelled]() { cancelled = true; });

    CHECK(view.title() == "Sign in to remote");
    CHECK(view.authorization_url() == "https://auth.example.com/authorize?code=abc");
    const auto screen = screen_of(view);
    CHECK(screen_contains(screen, "Approve access in your browser. If it did not open, visit:"));
    CHECK(screen_contains(screen, "https://auth.example.com/authorize?code=abc"));
    CHECK(screen_contains(screen, "If the browser runs on another machine, paste the URL it was redirected to:"));
    CHECK(screen_contains(screen, "enter submit • escape/ctrl+c cancel"));

    // Empty submit is ignored (pi `if (value)`).
    press(view, cch::tui::KeyEvent{.key = "enter"});
    CHECK(!submitted);

    type(view, "  http://127.0.0.1:1234/callback?code=xyz  ");
    press(view, cch::tui::KeyEvent{.key = "enter"});
    REQUIRE(submitted);
    CHECK(*submitted == "http://127.0.0.1:1234/callback?code=xyz");

    press(view, cch::tui::KeyEvent{.key = "escape"});
    CHECK(cancelled);

    copying.show_redirect_url(
            "Sign in to remote", "https://auth.example.com/authorize?code=abc", [](std::string) {}, [] {});
    press(copying, cch::tui::KeyEvent{.key = "x", .ctrl = true});
    REQUIRE(copied);
    CHECK(*copied == "https://auth.example.com/authorize?code=abc");
    CHECK(screen_contains(screen_of(copying), "Copied URL to clipboard"));
}
