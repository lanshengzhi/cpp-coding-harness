#include "McpManagerView.hpp"

#include "coding_agent/tui/ClipboardWrite.hpp"
#include "coding_agent/tui/DynamicBorder.hpp"
#include "coding_agent/tui/KeybindingHints.hpp"

#include <cch/tui/Container.hpp>
#include <cch/tui/Text.hpp>

#include <algorithm>
#include <cctype>
#include <format>
#include <string>
#include <utility>
#include <vector>

namespace cch::coding_agent::tui {
namespace {

/// pi `firstLine(text)`: the text up to its first newline.
[[nodiscard]] std::string first_line(std::string_view text) {
    const auto newline = text.find('\n');
    return std::string{text.substr(0, newline == std::string_view::npos ? text.size() : newline)};
}

/// pi `errorMessage(error)`/`connection.error ?? "unknown error"`.
[[nodiscard]] std::string describe_error(const std::optional<std::string>& error) {
    return first_line(error.value_or("unknown error"));
}

/// The `McpServerConfigBase` half of an entry's descriptor.
[[nodiscard]] const mcp::McpServerConfigBase& config_base(const mcp::McpConfigEntry& entry) {
    return std::visit([](const auto& config) -> const mcp::McpServerConfigBase& { return config; }, entry.config);
}

/// pi `notices()`: `config: <error>` then `overridden: <line>`.
[[nodiscard]] std::string mcp_notices(
        const std::vector<std::string>& config_errors, const std::vector<std::string>& overridden) {
    std::vector<std::string> lines;
    lines.reserve(config_errors.size() + overridden.size());
    for (const auto& error : config_errors)
        lines.push_back("config: " + error);
    for (const auto& line : overridden)
        lines.push_back("overridden: " + line);
    std::string joined;
    for (const auto& line : lines) {
        if (!joined.empty()) joined.push_back('\n');
        joined += line;
    }
    return joined;
}

/// pi's `empty` message for the server list and the plain status.
[[nodiscard]] std::string mcp_empty_message(const std::filesystem::path& agent_dir) {
    return std::format("No MCP servers configured. Add them to {} or .pi/mcp.json.", (agent_dir / "mcp.json").string());
}

/// pi `saved` in `serverMenu`: where a `Disable`/`Enable` change is written.
[[nodiscard]] std::string mcp_saved_location(const McpServerView& server) {
    if (server.scope && *server.scope == "extension") return "for this session";
    if (server.entry.override) return "saved to the project mcp.json";
    if (server.scope && !server.scope->empty()) return "saved to the " + *server.scope + " mcp.json";
    return "saved to mcp.json";
}

/// pi's `inProject` guard: a global server can be toggled for the trusted
/// project alone when the project has its own `mcp.json`.
[[nodiscard]] bool mcp_has_project_override_option(const McpServerView& server, bool project_config) {
    return server.scope && *server.scope == "global" && !server.entry.override && project_config;
}

/// pi's `entry.scope ?? entry.source` row/`details` location.
[[nodiscard]] std::string mcp_entry_location(const McpServerView& server) {
    if (server.scope && !server.scope->empty()) return *server.scope;
    return server.entry.source.string();
}

[[nodiscard]] std::vector<cch::tui::SelectItem> to_select_items(const std::vector<McpMenuItem>& items) {
    std::vector<cch::tui::SelectItem> select_items;
    select_items.reserve(items.size());
    for (const auto& item : items) {
        select_items.push_back(cch::tui::SelectItem{
                .value = item.value,
                .label = item.label,
                .description = item.description,
        });
    }
    return select_items;
}

/// pi's `prefix.trimStart().split(/\s+/)`: the pieces between maximal
/// whitespace runs, with a trailing separator leaving one empty final piece
/// (`"login "` -> `{"login", ""}`).
[[nodiscard]] std::vector<std::string> split_whitespace_runs(std::string_view text) {
    const auto is_space = [](char value) { return std::isspace(static_cast<unsigned char>(value)) != 0; };
    std::vector<std::string> parts;
    std::size_t index = 0;
    while (true) {
        const auto start = index;
        while (index < text.size() && !is_space(text[index]))
            ++index;
        parts.emplace_back(text.substr(start, index - start));
        if (index >= text.size()) break;
        while (index < text.size() && is_space(text[index]))
            ++index;
        if (index >= text.size()) {
            parts.emplace_back();
            break;
        }
    }
    return parts;
}

[[nodiscard]] std::string trim_start(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    return first == std::string_view::npos ? std::string{} : std::string{text.substr(first)};
}

} // namespace

std::string_view mcp_usage() {
    return "Usage: /mcp, /mcp login [server], /mcp logout [server], /mcp reconnect [server]";
}

std::string_view mcp_exposure_description(mcp::McpExposure exposure) {
    switch (exposure) {
    case mcp::McpExposure::Codemode:
        return "called from codemode scripts, which find them with searchTools()";
    case mcp::McpExposure::Deferred:
        return "not declared until tool_search loads them, then called directly; no codemode needed";
    case mcp::McpExposure::Direct:
        return "declared to the model like built-in tools";
    case mcp::McpExposure::Hidden:
        break;
    }
    return {};
}

std::string_view mcp_server_view_state_name(McpServerViewState state) {
    switch (state) {
    case McpServerViewState::Connecting:
        return "connecting";
    case McpServerViewState::Connected:
        return "connected";
    case McpServerViewState::Disconnected:
        return "disconnected";
    case McpServerViewState::NeedsAuth:
        return "needs-auth";
    case McpServerViewState::Failed:
        return "failed";
    case McpServerViewState::Closed:
        return "closed";
    }
    return "connecting";
}

std::string mcp_describe_transport(const mcp::McpConfigEntry& entry) {
    if (const auto* http = std::get_if<mcp::McpHttpServerConfig>(&entry.config)) {
        return http->url;
    }
    const auto& stdio = std::get<mcp::McpStdioServerConfig>(entry.config);
    std::string command = stdio.command;
    for (const auto& argument : stdio.args) {
        command.push_back(' ');
        command += argument;
    }
    return command;
}

std::string_view mcp_server_action_value(McpServerAction action) {
    switch (action) {
    case McpServerAction::SignIn:
        return "signin";
    case McpServerAction::Tools:
        return "tools";
    case McpServerAction::Reconnect:
        return "reconnect";
    case McpServerAction::SignOut:
        return "signout";
    case McpServerAction::Exposure:
        return "exposure";
    case McpServerAction::Disable:
        return "disable";
    case McpServerAction::Enable:
        return "enable";
    case McpServerAction::DisableInProject:
        return "disable-project";
    case McpServerAction::EnableInProject:
        return "enable-project";
    }
    return {};
}

std::optional<McpServerAction> parse_mcp_server_action(std::string_view value) {
    for (const auto action : {McpServerAction::SignIn,
                 McpServerAction::Tools,
                 McpServerAction::Reconnect,
                 McpServerAction::SignOut,
                 McpServerAction::Exposure,
                 McpServerAction::Disable,
                 McpServerAction::Enable,
                 McpServerAction::DisableInProject,
                 McpServerAction::EnableInProject}) {
        if (mcp_server_action_value(action) == value) return action;
    }
    return std::nullopt;
}

mcp::McpExposure mcp_entry_exposure(const mcp::McpConfigEntry& entry) {
    return config_base(entry).exposure.value_or(mcp::McpExposure::Codemode);
}

bool mcp_server_enabled(const McpServerView& server) { return server.entry.enabled; }

std::string mcp_describe_state(const McpServerView& server, bool with_error) {
    if (!mcp_server_enabled(server)) return "disabled";
    if (!server.connection) return "starting";
    const auto& connection = *server.connection;
    switch (connection.state) {
    case McpServerViewState::NeedsAuth:
        return "needs sign-in";
    case McpServerViewState::Failed:
        return with_error ? "failed: " + describe_error(connection.error) : "failed";
    case McpServerViewState::Connected: {
        const auto count = connection.resource_count;
        const auto resource_count =
                count > 0 ? std::format(" · {} resource{}", count, count == 1 ? "" : "s") : std::string{};
        const auto tool_count = connection.tools.size();
        return std::format("connected · {} tool{}{}", tool_count, tool_count == 1 ? "" : "s", resource_count);
    }
    case McpServerViewState::Connecting:
        return "connecting…";
    default:
        return std::string{mcp_server_view_state_name(connection.state)};
    }
}

int mcp_attention_rank(const McpServerView& server) {
    if (!mcp_server_enabled(server)) return 5;
    if (!server.connection) return 3;
    switch (server.connection->state) {
    case McpServerViewState::NeedsAuth:
        return 0;
    case McpServerViewState::Failed:
        return 1;
    case McpServerViewState::Disconnected:
        return 2;
    case McpServerViewState::Connected:
        return 4;
    default:
        return 3;
    }
}

McpMenu mcp_servers_menu(const std::vector<McpServerView>& servers,
        const std::vector<std::string>& config_errors,
        const std::vector<std::string>& overridden,
        const std::filesystem::path& agent_dir) {
    std::vector<const McpServerView*> sorted;
    sorted.reserve(servers.size());
    for (const auto& server : servers)
        sorted.push_back(&server);
    std::stable_sort(sorted.begin(), sorted.end(), [](const McpServerView* left, const McpServerView* right) {
        const auto left_rank = mcp_attention_rank(*left);
        const auto right_rank = mcp_attention_rank(*right);
        if (left_rank != right_rank) return left_rank < right_rank;
        return left->entry.name < right->entry.name;
    });

    McpMenu menu;
    menu.title = "MCP servers";
    const auto notices = mcp_notices(config_errors, overridden);
    if (!notices.empty()) menu.error = notices;
    for (const auto* server : sorted) {
        const auto location =
                server->entry.override ? std::string{"global, project override"} : mcp_entry_location(*server);
        menu.items.push_back(McpMenuItem{
                .value = server->entry.name,
                .label = server->entry.name,
                .description = std::format("{} · {} · {}",
                        mcp_describe_state(*server),
                        mcp::mcp_exposure_name(mcp_entry_exposure(server->entry)),
                        location),
        });
    }
    menu.empty = mcp_empty_message(agent_dir);
    menu.confirm_label = "manage";
    menu.cancel_label = "close";
    return menu;
}

McpMenu mcp_server_menu(const McpServerView& server, bool project_config) {
    McpMenu menu;
    const auto name = server.entry.name;
    menu.title = "MCP server " + name;

    std::vector<std::string> details;
    details.push_back(mcp_describe_transport(server.entry));
    details.push_back(std::format("{}: {}",
            server.scope && !server.scope->empty() ? *server.scope : std::string{"config"},
            server.entry.source.string()));
    if (server.entry.override) {
        details.push_back("project override: " + server.entry.override->string());
    }
    details.push_back("State: " + mcp_describe_state(server, false));
    std::string detail_text;
    for (const auto& line : details) {
        if (!detail_text.empty()) detail_text.push_back('\n');
        detail_text += line;
    }
    menu.details = std::move(detail_text);

    std::vector<std::string> error_lines;
    if (server.message) error_lines.push_back(*server.message);
    if (server.connection && server.connection->state != McpServerViewState::Connected && server.connection->error) {
        error_lines.push_back(*server.connection->error);
    }
    std::string error_text;
    for (const auto& line : error_lines) {
        if (!error_text.empty()) error_text.push_back('\n');
        error_text += line;
    }
    if (!error_text.empty()) menu.error = std::move(error_text);

    const auto saved = mcp_saved_location(server);
    const bool in_project = mcp_has_project_override_option(server, project_config);
    constexpr std::string_view kInProjectSaved = "saved to the project mcp.json";

    if (!mcp_server_enabled(server)) {
        menu.items.push_back(McpMenuItem{.value = std::string{mcp_server_action_value(McpServerAction::Enable)},
                .label = "Enable",
                .description = saved});
        if (in_project) {
            menu.items.push_back(
                    McpMenuItem{.value = std::string{mcp_server_action_value(McpServerAction::EnableInProject)},
                            .label = "Enable in this project",
                            .description = std::string{kInProjectSaved}});
        }
    } else {
        const auto state =
                server.connection ? std::optional<McpServerViewState>{server.connection->state} : std::nullopt;
        if (state == McpServerViewState::NeedsAuth) {
            menu.items.push_back(McpMenuItem{.value = std::string{mcp_server_action_value(McpServerAction::SignIn)},
                    .label = "Sign in",
                    .description = std::string{"opens the browser"}});
        }
        if (state == McpServerViewState::Connected) {
            menu.items.push_back(McpMenuItem{.value = std::string{mcp_server_action_value(McpServerAction::Tools)},
                    .label = "Tools",
                    .description = std::format("{} offered", server.connection->tools.size())});
        }
        if (state == McpServerViewState::Failed || state == McpServerViewState::Disconnected ||
                state == McpServerViewState::Connected || state == McpServerViewState::NeedsAuth) {
            menu.items.push_back(McpMenuItem{
                    .value = std::string{mcp_server_action_value(McpServerAction::Reconnect)}, .label = "Reconnect"});
        }
        if (state == McpServerViewState::Connected && server.connection->oauth) {
            menu.items.push_back(McpMenuItem{.value = std::string{mcp_server_action_value(McpServerAction::SignOut)},
                    .label = "Sign out",
                    .description = std::string{"deletes the stored credentials"}});
        }
        menu.items.push_back(McpMenuItem{.value = std::string{mcp_server_action_value(McpServerAction::Exposure)},
                .label = "Exposure",
                .description = std::string{mcp::mcp_exposure_name(mcp_entry_exposure(server.entry))}});
        menu.items.push_back(McpMenuItem{.value = std::string{mcp_server_action_value(McpServerAction::Disable)},
                .label = "Disable",
                .description = saved});
        if (in_project) {
            menu.items.push_back(
                    McpMenuItem{.value = std::string{mcp_server_action_value(McpServerAction::DisableInProject)},
                            .label = "Disable in this project",
                            .description = std::string{kInProjectSaved}});
        }
    }

    if (!menu.items.empty()) menu.selected = menu.items.front().value;
    menu.confirm_label = "select";
    menu.cancel_label = "back";
    return menu;
}

McpMenu mcp_exposure_menu(const McpServerView& server) {
    McpMenu menu;
    menu.title = "Exposure of " + server.entry.name;
    if (server.scope && *server.scope == "extension") {
        menu.details =
                std::format("Applies to this session; the server is registered by {}.", server.entry.source.string());
    } else {
        menu.details = "Saved to " +
                       (server.entry.override ? server.entry.override->string() : server.entry.source.string()) + ".";
    }
    const auto current = mcp_entry_exposure(server.entry);
    for (const auto exposure : {mcp::McpExposure::Codemode, mcp::McpExposure::Deferred, mcp::McpExposure::Direct}) {
        menu.items.push_back(McpMenuItem{
                .value = std::string{mcp::mcp_exposure_name(exposure)},
                .label = std::format("{}{}", exposure == current ? "✓ " : "  ", mcp::mcp_exposure_name(exposure)),
                .description = std::string{mcp_exposure_description(exposure)},
        });
    }
    menu.selected = std::string{mcp::mcp_exposure_name(current)};
    menu.confirm_label = "save";
    menu.cancel_label = "back";
    return menu;
}

McpMenu mcp_tools_menu(const McpServerView& server) {
    McpMenu menu;
    menu.title = "Tools of " + server.entry.name;
    const auto exposure = mcp_entry_exposure(server.entry);
    const auto descriptions = exposure == mcp::McpExposure::Hidden ? std::string{"unreachable"}
                                                                   : std::string{mcp_exposure_description(exposure)};
    auto details = std::format("Exposure {}: {}", mcp::mcp_exposure_name(exposure), descriptions);
    if (!config_base(server.entry).tool_exposure.empty()) {
        details += "\nSome tools override it with toolExposure.";
    }
    menu.details = std::move(details);

    const auto* connection = server.connection ? &*server.connection : nullptr;
    if (connection != nullptr) {
        for (const auto& tool : connection->tools) {
            const auto tool_exposure = mcp::get_mcp_tool_exposure(
                    config_base(server.entry).tool_exposure, config_base(server.entry).exposure, tool.name);
            const auto description = first_line(tool.description);
            std::string rendered;
            if (tool_exposure == exposure) {
                rendered = description;
            } else {
                rendered = std::format("[{}] {}", mcp::mcp_exposure_name(tool_exposure), description);
            }
            menu.items.push_back(McpMenuItem{
                    .value = tool.name,
                    .label = tool.name,
                    .description = std::move(rendered),
            });
        }
    }
    menu.empty = "The server offers no tools.";
    menu.confirm_label = "back";
    menu.cancel_label = "back";
    return menu;
}

std::string mcp_format_status(const std::vector<McpServerView>& servers,
        const std::vector<std::string>& config_errors,
        const std::vector<std::string>& overridden,
        const std::filesystem::path& agent_dir) {
    if (servers.empty() && config_errors.empty() && overridden.empty()) {
        return mcp_empty_message(agent_dir);
    }
    std::vector<std::string> lines;
    lines.reserve(servers.size() + config_errors.size() + overridden.size());
    for (const auto& server : servers) {
        const auto name = server.entry.name;
        const auto exposure = mcp::mcp_exposure_name(mcp_entry_exposure(server.entry));
        const auto* connection = server.connection ? &*server.connection : nullptr;
        if (connection != nullptr && connection->state == McpServerViewState::NeedsAuth) {
            lines.push_back(std::format("{}: needs sign-in, run /mcp login {} ({})", name, name, exposure));
            continue;
        }
        const auto tools = connection != nullptr && connection->state == McpServerViewState::Connected
                                   ? std::format(", {} tools", connection->tools.size())
                                   : std::string{};
        std::string state;
        if (!mcp_server_enabled(server)) {
            state = "disabled";
        } else if (connection != nullptr && connection->state == McpServerViewState::Disconnected) {
            state = "disconnected, reconnects on next call";
        } else if (connection != nullptr) {
            state = std::string{mcp_server_view_state_name(connection->state)};
        } else {
            state = "starting";
        }
        std::string error;
        if (connection != nullptr && connection->error && connection->state != McpServerViewState::Connected) {
            error = "\n    ";
            for (std::size_t index = 0; index < connection->error->size(); ++index) {
                error.push_back((*connection->error)[index]);
                if ((*connection->error)[index] == '\n') error += "    ";
            }
        }
        lines.push_back(std::format("{}: {}{} ({}){}", name, state, tools, exposure, error));
    }
    for (const auto& error : config_errors)
        lines.push_back("config error: " + error);
    for (const auto& line : overridden)
        lines.push_back("overridden: " + line);
    std::string joined;
    for (const auto& line : lines) {
        if (!joined.empty()) joined.push_back('\n');
        joined += line;
    }
    return joined;
}

std::vector<McpCommandCompletion> mcp_command_completions(
        std::string_view prefix, const std::vector<McpServerView>& servers) {
    const auto parts = split_whitespace_runs(trim_start(prefix));
    const std::string action = parts.empty() ? std::string{} : parts[0];
    const std::optional<std::string> server = parts.size() >= 2 ? std::optional<std::string>{parts[1]} : std::nullopt;
    const bool has_rest = parts.size() > 2;
    if (has_rest) return {};

    if (!server) {
        std::vector<McpCommandCompletion> completions;
        for (const auto& candidate : {"login", "logout", "reconnect"}) {
            const std::string spelling{candidate};
            if (spelling.rfind(action, 0) != 0) continue;
            completions.push_back(McpCommandCompletion{.value = spelling + " ", .label = spelling});
        }
        return completions;
    }

    if (action != "login" && action != "logout" && action != "reconnect") return {};

    std::vector<McpCommandCompletion> completions;
    for (const auto& candidate : servers) {
        const bool eligible = action == "reconnect" ? candidate.connection.has_value()
                                                    : (candidate.connection && candidate.connection->oauth);
        if (!eligible) continue;
        if (candidate.entry.name.rfind(*server, 0) != 0) continue;
        completions.push_back(McpCommandCompletion{
                .value = action + " " + candidate.entry.name,
                .label = candidate.entry.name,
                .description = mcp_describe_state(candidate),
        });
    }
    return completions;
}

namespace {

/// pi `frame()`: the accent rule, the accent-bold title, the body, and the
/// dim footer, closed by an accent rule.
[[nodiscard]] support::ExpectedVoid append_component(
        cch::tui::RenderResult& result, cch::tui::Component& component, std::size_t width) {
    auto rendered = component.render(width);
    if (!rendered) return std::unexpected(rendered.error());
    for (auto& line : rendered->lines)
        result.lines.push_back(std::move(line));
    return {};
}

} // namespace

struct McpManagerView::Impl {
    enum class Screen { Menu, Status, Redirect };

    const LiveTheme& theme; // must outlive this view.
    std::shared_ptr<const cch::tui::KeybindingRegistry> keybindings;
    McpMenuConfirmSink on_confirm;
    McpMenuCancelSink on_cancel;
    McpCopySink on_copy;

    Screen screen{Screen::Menu};
    std::string title;
    std::optional<cch::tui::SelectList> list;
    /// The selected item's value carried across a rebuild of the same menu
    /// (pi's per-`menu()` closure state).
    std::optional<std::string> selected;
    /// The title of the menu currently shown, so a rebuild of the same menu
    /// keeps `selected` and a different menu starts from its own default.
    std::optional<std::string> menu_title;
    std::optional<std::string> empty;
    std::string confirm_label;
    std::string cancel_label;
    std::string status_message;

    std::string authorization_url;
    std::string copy_hint;
    std::unique_ptr<cch::tui::Input> redirect_input;
    McpRedirectSubmitSink on_redirect_submit;
    McpRedirectCancelSink on_redirect_cancel;
    /// The input's row inside the rendered frame; `cursor_location` moves the
    /// Input's own row by it.
    std::size_t redirect_input_row{0};
    bool focused{false};

    Impl(const LiveTheme& theme,
            std::shared_ptr<const cch::tui::KeybindingRegistry> keybindings,
            McpMenuConfirmSink on_confirm,
            McpMenuCancelSink on_cancel,
            McpCopySink on_copy)
        : theme(theme), keybindings(keybindings ? std::move(keybindings) : cch::tui::default_tui_keybindings()),
          on_confirm(std::move(on_confirm)), on_cancel(std::move(on_cancel)), on_copy(std::move(on_copy)) {}

    [[nodiscard]] std::string hint(std::string_view action, std::string_view description) const {
        return key_hint(theme, *keybindings, action, description);
    }

    /// pi's `keyHint("app.message.copy", "to copy")` default hint text.
    [[nodiscard]] std::string default_copy_hint() const {
        const auto click_hint = "Ctrl+click to open";
        return theme.foreground(ThemeToken::Dim, click_hint) + theme.foreground(ThemeToken::Dim, " • ") +
               hint("app.message.copy", "to copy");
    }

    void copy_authorization_url() {
        const auto url = authorization_url;
        bool copied = false;
        if (on_copy) {
            on_copy(url);
            copied = true;
        } else {
            copied = write_clipboard_text(url);
        }
        copy_hint = copied ? theme.foreground(ThemeToken::Success, "Copied URL to clipboard")
                           : theme.foreground(ThemeToken::Error, "Could not copy the URL");
    }
};

McpManagerView::McpManagerView(const LiveTheme& theme,
        std::shared_ptr<const cch::tui::KeybindingRegistry> keybindings,
        McpMenuConfirmSink on_confirm,
        McpMenuCancelSink on_cancel,
        McpCopySink on_copy)
    : impl_(std::make_unique<Impl>(
              theme, std::move(keybindings), std::move(on_confirm), std::move(on_cancel), std::move(on_copy))) {}

McpManagerView::~McpManagerView() = default;

void McpManagerView::show_menu(McpMenu menu) {
    impl_->screen = Impl::Screen::Menu;
    const bool same_menu = impl_->menu_title && *impl_->menu_title == menu.title;
    if (!same_menu) impl_->selected = std::nullopt;
    impl_->menu_title = menu.title;
    impl_->title = menu.title;
    impl_->empty = menu.empty;
    impl_->confirm_label = menu.confirm_label;
    impl_->cancel_label = menu.cancel_label;
    impl_->status_message.clear();

    auto items = to_select_items(menu.items);
    if (items.empty()) {
        impl_->list = std::nullopt;
    } else {
        const auto wanted = impl_->selected ? impl_->selected : menu.selected;
        cch::tui::SelectListOptions options;
        options.max_visible = std::min(items.size(), kMcpMaxVisibleItems);
        options.theme = impl_->theme.select_list_theme();
        options.keybindings = impl_->keybindings;
        options.on_select = [this](const cch::tui::SelectItem& item) -> support::ExpectedVoid {
            impl_->selected = item.value;
            if (impl_->on_confirm) impl_->on_confirm(item.value);
            return {};
        };
        // pi `list.onSelectionChange`: the selection is carried into the next
        // rebuild of the same menu.
        options.on_selection_change = [this](const cch::tui::SelectItem& item) -> support::ExpectedVoid {
            impl_->selected = item.value;
            return {};
        };
        options.on_cancel = [this]() -> support::ExpectedVoid {
            if (impl_->on_cancel) impl_->on_cancel();
            return {};
        };
        impl_->list.emplace(std::move(items), std::move(options));
        impl_->list->set_focused(impl_->focused);
        if (wanted) {
            const auto& source = menu.items;
            for (std::size_t index = 0; index < source.size(); ++index) {
                if (source[index].value == *wanted) {
                    impl_->list->set_selected_index(index);
                    break;
                }
            }
        }
    }
}

void McpManagerView::show_status(std::string title, std::string message) {
    impl_->screen = Impl::Screen::Status;
    impl_->title = std::move(title);
    impl_->status_message = std::move(message);
    impl_->list = std::nullopt;
}

void McpManagerView::show_redirect_url(std::string title,
        std::string authorization_url,
        McpRedirectSubmitSink on_submit,
        McpRedirectCancelSink on_redirect_cancel) {
    impl_->screen = Impl::Screen::Redirect;
    impl_->title = std::move(title);
    impl_->authorization_url = std::move(authorization_url);
    impl_->on_redirect_submit = std::move(on_submit);
    impl_->on_redirect_cancel = std::move(on_redirect_cancel);
    impl_->copy_hint = impl_->default_copy_hint();
    impl_->redirect_input =
            std::make_unique<cch::tui::Input>(cch::tui::InputOptions{.keybindings = impl_->keybindings});
    impl_->redirect_input->set_focused(impl_->focused);
    impl_->list = std::nullopt;
}

const std::string& McpManagerView::title() const noexcept { return impl_->title; }

std::string_view McpManagerView::authorization_url() const noexcept { return impl_->authorization_url; }

cch::tui::Input& McpManagerView::redirect_input() noexcept { return *impl_->redirect_input; }

support::Expected<cch::tui::RenderResult> McpManagerView::render(std::size_t width) {
    cch::tui::RenderResult result;
    if (width == 0) {
        return std::unexpected(support::make_error(
                support::ErrorCode::Validation, "McpManagerView requires a positive visible width"));
    }

    // pi `frame()`: accent border, accent-bold title, body, optional dim
    // footer, accent border.
    DynamicBorder top_border(impl_->theme.foreground_hook(ThemeToken::Accent));
    if (auto appended = append_component(result, top_border, width); !appended) {
        return std::unexpected(appended.error());
    }

    cch::tui::Text title_text(impl_->theme.foreground(ThemeToken::Accent, "\x1b[1m" + impl_->title + "\x1b[22m"), 1, 0);
    if (auto appended = append_component(result, title_text, width); !appended) {
        return std::unexpected(appended.error());
    }

    switch (impl_->screen) {
    case Impl::Screen::Menu: {
        cch::tui::Spacer spacer(1);
        if (auto appended = append_component(result, spacer, width); !appended) {
            return std::unexpected(appended.error());
        }
        if (impl_->list) {
            if (auto appended = append_component(result, *impl_->list, width); !appended) {
                return std::unexpected(appended.error());
            }
        } else {
            cch::tui::Text empty_text(impl_->theme.foreground(ThemeToken::Muted,
                                              impl_->empty ? *impl_->empty : std::string{"Nothing to show."}),
                    1,
                    0);
            if (auto appended = append_component(result, empty_text, width); !appended) {
                return std::unexpected(appended.error());
            }
        }
        cch::tui::Spacer footer_spacer(1);
        if (auto appended = append_component(result, footer_spacer, width); !appended) {
            return std::unexpected(appended.error());
        }
        const auto footer = impl_->list ? impl_->hint("tui.select.confirm", impl_->confirm_label) + " • " +
                                                  impl_->hint("tui.select.cancel", impl_->cancel_label)
                                        : impl_->hint("tui.select.cancel", impl_->cancel_label);
        cch::tui::Text footer_text(impl_->theme.foreground(ThemeToken::Dim, footer), 1, 0);
        if (auto appended = append_component(result, footer_text, width); !appended) {
            return std::unexpected(appended.error());
        }
        break;
    }
    case Impl::Screen::Status: {
        cch::tui::Spacer spacer(1);
        if (auto appended = append_component(result, spacer, width); !appended) {
            return std::unexpected(appended.error());
        }
        cch::tui::Text message_text(impl_->theme.foreground(ThemeToken::Muted, impl_->status_message), 1, 0);
        if (auto appended = append_component(result, message_text, width); !appended) {
            return std::unexpected(appended.error());
        }
        break;
    }
    case Impl::Screen::Redirect: {
        cch::tui::Spacer spacer(1);
        if (auto appended = append_component(result, spacer, width); !appended) {
            return std::unexpected(appended.error());
        }
        cch::tui::Text instruction(impl_->theme.foreground(ThemeToken::Muted,
                                           "Approve access in your browser. If it did not open, visit:"),
                1,
                0);
        if (auto appended = append_component(result, instruction, width); !appended) {
            return std::unexpected(appended.error());
        }
        cch::tui::Text url_text(impl_->theme.foreground(ThemeToken::Accent, impl_->authorization_url), 1, 0);
        if (auto appended = append_component(result, url_text, width); !appended) {
            return std::unexpected(appended.error());
        }
        cch::tui::Text copy_text(impl_->copy_hint, 1, 0);
        if (auto appended = append_component(result, copy_text, width); !appended) {
            return std::unexpected(appended.error());
        }
        cch::tui::Spacer paste_spacer(1);
        if (auto appended = append_component(result, paste_spacer, width); !appended) {
            return std::unexpected(appended.error());
        }
        cch::tui::Text paste_instruction(
                impl_->theme.foreground(ThemeToken::Muted,
                        "If the browser runs on another machine, paste the URL it was redirected to:"),
                1,
                0);
        if (auto appended = append_component(result, paste_instruction, width); !appended) {
            return std::unexpected(appended.error());
        }
        impl_->redirect_input_row = result.lines.size();
        if (impl_->redirect_input) {
            if (auto appended = append_component(result, *impl_->redirect_input, width); !appended) {
                return std::unexpected(appended.error());
            }
        }
        cch::tui::Spacer footer_spacer(1);
        if (auto appended = append_component(result, footer_spacer, width); !appended) {
            return std::unexpected(appended.error());
        }
        const auto footer =
                impl_->hint("tui.select.confirm", "submit") + " • " + impl_->hint("tui.select.cancel", "cancel");
        cch::tui::Text footer_text(impl_->theme.foreground(ThemeToken::Dim, footer), 1, 0);
        if (auto appended = append_component(result, footer_text, width); !appended) {
            return std::unexpected(appended.error());
        }
        break;
    }
    }

    DynamicBorder bottom_border(impl_->theme.foreground_hook(ThemeToken::Accent));
    if (auto appended = append_component(result, bottom_border, width); !appended) {
        return std::unexpected(appended.error());
    }
    return result;
}

void McpManagerView::invalidate() {
    if (impl_->list) impl_->list->invalidate();
    if (impl_->redirect_input) impl_->redirect_input->invalidate();
}

cch::tui::InputAdmissionOutcome McpManagerView::handle_input(const cch::tui::InputEventVariant& input) {
    const auto* key = std::get_if<cch::tui::KeyEvent>(&input);
    if (key != nullptr && key->type == cch::tui::KeyEventType::Release) {
        return cch::tui::InputAdmissionOutcome::Unhandled;
    }

    if (impl_->screen == Impl::Screen::Menu) {
        if (!impl_->list) {
            if (key != nullptr && impl_->keybindings->matches(*key, "tui.select.cancel")) {
                if (impl_->on_cancel) impl_->on_cancel();
                return cch::tui::InputAdmissionOutcome::Consumed;
            }
            return cch::tui::InputAdmissionOutcome::Unhandled;
        }
        static_cast<void>(impl_->list->handle_input(input));
        return cch::tui::InputAdmissionOutcome::Consumed;
    }

    if (impl_->screen == Impl::Screen::Redirect) {
        if (key != nullptr) {
            if (impl_->keybindings->matches(*key, "tui.select.confirm")) {
                const auto value = impl_->redirect_input ? impl_->redirect_input->value() : std::string{};
                const auto first = value.find_first_not_of(" \t\r\n");
                if (first != std::string::npos) {
                    const auto last = value.find_last_not_of(" \t\r\n");
                    if (impl_->on_redirect_submit) {
                        impl_->on_redirect_submit(value.substr(first, last - first + 1));
                    }
                }
                return cch::tui::InputAdmissionOutcome::Consumed;
            }
            if (impl_->keybindings->matches(*key, "tui.select.cancel")) {
                if (impl_->on_redirect_cancel) impl_->on_redirect_cancel();
                return cch::tui::InputAdmissionOutcome::Consumed;
            }
            if (impl_->keybindings->matches(*key, "app.message.copy")) {
                impl_->copy_authorization_url();
                return cch::tui::InputAdmissionOutcome::Consumed;
            }
        }
        if (impl_->redirect_input) {
            static_cast<void>(impl_->redirect_input->handle_input(input));
            return cch::tui::InputAdmissionOutcome::Consumed;
        }
        return cch::tui::InputAdmissionOutcome::Unhandled;
    }

    // pi's `status` sets content without an input handler: keys are ignored.
    return cch::tui::InputAdmissionOutcome::Unhandled;
}

void McpManagerView::set_focused(bool focused) {
    impl_->focused = focused;
    if (impl_->list) impl_->list->set_focused(focused);
    if (impl_->redirect_input) impl_->redirect_input->set_focused(focused);
}

bool McpManagerView::focused() const { return impl_->focused; }

std::optional<cch::tui::CursorPosition> McpManagerView::cursor_location() const {
    if (impl_->screen != Impl::Screen::Redirect || !impl_->redirect_input) return std::nullopt;
    const auto cursor = impl_->redirect_input->cursor_location();
    if (!cursor) return std::nullopt;
    return cch::tui::CursorPosition{
            .column = cursor->column,
            .row = cursor->row + impl_->redirect_input_row,
    };
}

} // namespace cch::coding_agent::tui
