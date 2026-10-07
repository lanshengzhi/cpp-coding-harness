// The `/mcp` interactive flow (see the header). pi source at `7c10bd43`
// (v1.0.4): `packages/coding-agent/src/extensions/mcp/index.ts`
// (`registerCommand("mcp")`, `pickServer`, `loginCommand`, `signInWithUi`,
// `manage`, `runAction`) and `ui.ts` (`McpUi.menu`'s subscribe shape).

#include "coding_agent/tui/McpManagerFlow.hpp"

#include "coding_agent/tui/SharedKeybindings.hpp"
#include "coding_agent/tui/Theme.hpp"

#include "support/AsyncResultBridge.hpp"

#include <boost/asio/awaitable.hpp>

#include <algorithm>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::coding_agent::tui {
namespace {

/// pi `pickServer`'s eligibility notice for OAuth-less picks
/// (`oauthPick.none`), verbatim.
[[nodiscard]] std::string oauth_none_message() {
    return "No enabled MCP server uses OAuth. Only HTTP servers without an Authorization header do.";
}

/// pi `pickServer`'s unknown-name notice, verbatim.
[[nodiscard]] std::string no_server_named(std::string_view name) {
    return std::format("No MCP server named \"{}\".", name);
}

/// The shared state bridging the sign-in screen's submit sink to the
/// awaitable the manager's sign-in races against the browser callback.
/// Exactly one completion is stored; completing releases the wait (a late
/// completion after the OAuth race settled is discarded by the flow).
struct RedirectState {
    std::optional<support::AsyncCompletion<std::optional<std::string>, support::Error>> pending{};

    void complete(std::optional<std::string> value) noexcept {
        if (!pending) {
            return;
        }
        auto completion = std::move(*pending);
        pending.reset();
        completion(std::move(value));
    }
};

} // namespace

McpManagerFlow::McpManagerFlow(boost::asio::any_io_executor executor,
        ModalPresenter& presenter,
        std::weak_ptr<void> host_lifetime,
        McpFlowHostHooks hooks,
        std::shared_ptr<SharedKeybindings> keybindings)
    : executor_(std::move(executor)),
      presenter_(&presenter),
      host_lifetime_(std::move(host_lifetime)),
      hooks_(std::move(hooks)),
      keybindings_(std::move(keybindings)) {}

void McpManagerFlow::post(std::move_only_function<void()> action) {
    if (hooks_.post_on_executor) {
        hooks_.post_on_executor(std::move(action));
    }
}

runtime::McpSessionManager* McpManagerFlow::manager() {
    return hooks_.mcp_manager != nullptr ? hooks_.mcp_manager() : nullptr;
}

bool McpManagerFlow::live() { return hooks_.is_live != nullptr && hooks_.is_live(); }

void McpManagerFlow::show_status(std::string text) {
    if (presenter_ != nullptr) {
        presenter_->show_status(std::move(text));
    }
}

void McpManagerFlow::show_error(std::string text) {
    if (presenter_ != nullptr) {
        presenter_->show_error(std::move(text));
    }
}

void McpManagerFlow::show_warning(std::string text) {
    if (presenter_ != nullptr) {
        presenter_->show_warning(std::move(text));
    }
}

// ── Completion snapshot and the manager-change subscription ─────────────────

void McpManagerFlow::refresh_completion() {
    auto* current = manager();
    if (current == nullptr) {
        completion_ = std::make_shared<const McpCompletionSnapshot>();
        subscribed_manager_ = nullptr;
        drained_warnings_ = 0;
        return;
    }
    McpManagerPresenter presenter(*current);
    auto snapshot = std::make_shared<McpCompletionSnapshot>();
    snapshot->servers = presenter.servers();
    snapshot->config_errors = current->config_errors();
    snapshot->overridden = current->overridden();
    snapshot->agent_dir = current->agent_dir();
    completion_ = std::move(snapshot);

    if (subscribed_manager_ != current) {
        subscribed_manager_ = current;
        drained_warnings_ = 0;
        // The manager's single listener slot: this subscription owns both
        // the completion refresh and the panel's re-presentation.
        const auto weak = weak_from_this();
        presenter.subscribe([weak] {
            if (const auto self = weak.lock()) {
                self->post([self] { self->on_manager_change(); });
            }
        });
        // pi `ctx.ui.notify(..., "warning")`: forward the manager's
        // reachability warning into the host's notification channel.
        current->set_notify_warning_sink([weak](std::string_view message) {
            if (const auto self = weak.lock()) {
                self->post([self, text = std::string{message}] { self->show_warning(std::move(text)); });
            }
        });
    }
    // Drain the warnings latched before the sink existed (the discovery
    // warning fires during session bind, before the TUI subscribes).
    const auto& warnings = current->warnings();
    for (std::size_t index = drained_warnings_; index < warnings.size(); ++index) {
        show_warning(warnings[index]);
    }
    drained_warnings_ = warnings.size();
}

void McpManagerFlow::on_manager_change() {
    refresh_completion();
    if (panel_ && panel_->screen != Screen::sign_in && panel_->screen != Screen::status) {
        // pi `ui.menu(…, subscribe)`: a manager change re-presents the open
        // menu. The sign-in and status screens are not menus.
        present_current_menu();
    }
}

// ── Panel hosting (pi `showMcpManager` + `manage`) ──────────────────────────

void McpManagerFlow::ensure_panel() {
    if (panel_.has_value()) {
        return;
    }
    Panel panel;
    const auto weak = weak_from_this();
    panel.view = std::make_shared<McpManagerView>(
            hooks_.live_theme(),
            keybindings_ != nullptr ? keybindings_->get() : nullptr,
            [weak](std::string value) {
                if (const auto self = weak.lock()) {
                    self->post([self, value = std::move(value)]() mutable { self->on_menu_confirm(std::move(value)); });
                }
            },
            [weak] {
                if (const auto self = weak.lock()) {
                    self->post([self] { self->on_menu_cancel(); });
                }
            },
            [weak](std::string text) {
                if (const auto self = weak.lock()) {
                    self->post([self, text = std::move(text)]() mutable {
                        if (self->hooks_.copy_text) {
                            self->hooks_.copy_text(std::move(text));
                        }
                    });
                }
            });
    panel_ = std::move(panel);
    presenter_->replace_prompt_slot(panel_->view);
}

void McpManagerFlow::close_panel() {
    if (!panel_.has_value()) {
        return;
    }
    panel_.reset();
    if (presenter_ != nullptr) {
        presenter_->restore_prompt_slot();
    }
}

void McpManagerFlow::present_servers() {
    if (!panel_.has_value()) {
        return;
    }
    auto* current = manager();
    if (current == nullptr) {
        close_panel();
        print_status_report();
        return;
    }
    panel_->screen = Screen::servers;
    panel_->server.clear();
    panel_->view->show_menu(McpManagerPresenter(*current).servers_menu());
    if (presenter_ != nullptr) {
        presenter_->invalidate();
    }
}

void McpManagerFlow::present_server(std::string_view name) {
    if (!panel_.has_value()) {
        return;
    }
    auto* current = manager();
    if (current == nullptr) {
        close_panel();
        print_status_report();
        return;
    }
    panel_->screen = Screen::server;
    panel_->server = std::string{name};
    panel_->view->show_menu(McpManagerPresenter(*current).server_menu(name));
    if (presenter_ != nullptr) {
        presenter_->invalidate();
    }
}

void McpManagerFlow::present_current_menu() {
    if (!panel_.has_value()) {
        return;
    }
    auto* current = manager();
    if (current == nullptr) {
        close_panel();
        return;
    }
    McpManagerPresenter presenter(*current);
    switch (panel_->screen) {
    case Screen::servers:
        panel_->view->show_menu(presenter.servers_menu());
        break;
    case Screen::server:
        panel_->view->show_menu(presenter.server_menu(panel_->server));
        break;
    case Screen::exposure:
        panel_->view->show_menu(presenter.exposure_menu(panel_->server));
        break;
    case Screen::tools:
        panel_->view->show_menu(presenter.tools_menu(panel_->server));
        break;
    case Screen::sign_in:
    case Screen::status:
        break;
    }
    if (presenter_ != nullptr) {
        presenter_->invalidate();
    }
}

void McpManagerFlow::open_manager() {
    if (!live()) {
        return;
    }
    if (hooks_.overlay_active && hooks_.overlay_active()) {
        return;
    }
    if (manager() == nullptr || presenter_ == nullptr) {
        // pi's non-TUI branch: the plain status report.
        print_status_report();
        return;
    }
    if (panel_.has_value()) {
        // Re-entry presents the servers menu again (pi opens a fresh
        // manager over the same live state).
        present_servers();
        return;
    }
    ensure_panel();
    present_servers();
}

void McpManagerFlow::print_status_report() {
    auto* current = manager();
    std::vector<McpServerView> servers;
    std::vector<std::string> config_errors;
    std::vector<std::string> overridden;
    std::filesystem::path agent_dir;
    if (current != nullptr) {
        McpManagerPresenter presenter(*current);
        servers = presenter.servers();
        config_errors = current->config_errors();
        overridden = current->overridden();
        agent_dir = current->agent_dir();
    }
    const auto report = mcp_format_status(servers, config_errors, overridden, agent_dir);
    std::string line;
    for (const char character : report) {
        if (character == '\n') {
            show_status(std::exchange(line, {}));
        } else {
            line.push_back(character);
        }
    }
    if (!line.empty()) {
        show_status(std::move(line));
    }
}

void McpManagerFlow::show_status_lines(const std::vector<McpServerView>& servers) {
    auto* current = manager();
    const auto report = mcp_format_status(
            servers, current != nullptr ? current->config_errors() : std::vector<std::string>{},
            current != nullptr ? current->overridden() : std::vector<std::string>{},
            current != nullptr ? current->agent_dir() : std::filesystem::path{});
    std::string line;
    for (const char character : report) {
        if (character == '\n') {
            show_status(std::exchange(line, {}));
        } else {
            line.push_back(character);
        }
    }
    if (!line.empty()) {
        show_status(std::move(line));
    }
}

void McpManagerFlow::on_menu_confirm(std::string value) {
    if (!panel_.has_value()) {
        return;
    }
    switch (panel_->screen) {
    case Screen::servers:
        // pi `manage`: the confirmed row is the server name; open its menu.
        present_server(value);
        return;
    case Screen::server: {
        const auto action = parse_mcp_server_action(value);
        if (!action.has_value()) {
            present_servers();
            return;
        }
        run_server_action(panel_->server, *action);
        return;
    }
    case Screen::exposure: {
        const auto exposure = mcp::parse_mcp_exposure(value);
        const std::string name = panel_->server;
        if (!exposure.has_value()) {
            present_server(name);
            return;
        }
        // pi `chooseExposure`'s save step: write the exposure and return to
        // the server menu (the manager change re-presents it too).
        auto self = shared_from_this();
        auto host_lifetime = host_lifetime_.lock();
        if (host_lifetime == nullptr || hooks_.spawn_flow == nullptr) {
            return;
        }
        hooks_.spawn_flow(
                [self, host_lifetime = std::move(host_lifetime), name, exposure = *exposure]() mutable
                        -> boost::asio::awaitable<void> {
                    auto* current = self->manager();
                    if (current == nullptr) {
                        co_return;
                    }
                    McpManagerPresenter presenter(*current);
                    auto saved = co_await support::detail::await_async_result(
                            presenter.set_exposure(name, exposure));
                    if (!saved) {
                        self->show_error(saved.error().message);
                    }
                    self->present_server(name);
                },
                "/mcp exposure");
        return;
    }
    case Screen::tools:
        // The tools list's confirm is "back".
        present_server(panel_->server);
        return;
    case Screen::sign_in:
    case Screen::status:
        return;
    }
}

void McpManagerFlow::on_menu_cancel() {
    if (!panel_.has_value()) {
        return;
    }
    switch (panel_->screen) {
    case Screen::servers:
        close_panel();
        return;
    case Screen::server:
        present_servers();
        return;
    case Screen::exposure:
    case Screen::tools:
        present_server(panel_->server);
        return;
    case Screen::sign_in:
    case Screen::status:
        return;
    }
}

// ── Server-menu actions (pi `runAction`) ────────────────────────────────────

void McpManagerFlow::run_server_action(std::string name, McpServerAction action) {
    if (action == McpServerAction::SignIn) {
        run_sign_in(std::move(name), /* close_after */ false);
        return;
    }
    auto self = shared_from_this();
    auto host_lifetime = host_lifetime_.lock();
    if (host_lifetime == nullptr || hooks_.spawn_flow == nullptr) {
        return;
    }
    hooks_.spawn_flow(
            [self, host_lifetime = std::move(host_lifetime), name, action]() mutable
                    -> boost::asio::awaitable<void> {
                auto* current = self->manager();
                if (current == nullptr) {
                    co_return;
                }
                McpManagerPresenter presenter(*current);
                auto outcome = co_await support::detail::await_async_result(
                        presenter.run_action(name, action));
                if (!outcome) {
                    self->show_error(outcome.error().message);
                    self->present_server(name);
                    co_return;
                }
                if (outcome->next_menu.has_value()) {
                    // `Tools` and `Exposure` return their sub-screen.
                    self->panel_->screen =
                            action == McpServerAction::Tools ? Screen::tools : Screen::exposure;
                    self->panel_->view->show_menu(std::move(*outcome->next_menu));
                    if (self->presenter_ != nullptr) {
                        self->presenter_->invalidate();
                    }
                    co_return;
                }
                // Side-effecting actions return to the server menu; the
                // manager change re-presents it, and the explicit call covers
                // actions that changed nothing.
                self->present_server(name);
            },
            "/mcp action");
}

void McpManagerFlow::run_sign_in(std::string name, bool close_after) {
    auto* current = manager();
    if (current == nullptr || !live()) {
        return;
    }
    ensure_panel();
    const std::string title = "Sign in to " + name;
    panel_->screen = Screen::sign_in;
    panel_->server = name;
    // pi `signInWithUi`: the status line while the authorization server is
    // contacted.
    panel_->view->show_status(title, "Contacting the authorization server…");
    if (presenter_ != nullptr) {
        presenter_->invalidate();
    }

    const auto state = std::make_shared<RedirectState>();
    runtime::McpSignInPrompt prompt;
    const auto weak = weak_from_this();
    prompt.show_authorization_url = [weak, state, title](std::string_view url) {
        if (const auto self = weak.lock()) {
            self->post([self, state, title, url = std::string{url}]() mutable {
                if (!self->panel_.has_value()) {
                    return;
                }
                // pi `signInWithUi`: open the browser and show the
                // redirect-URL screen racing the paste fallback.
                if (self->hooks_.open_browser) {
                    self->hooks_.open_browser(url);
                }
                self->panel_->view->show_redirect_url(
                        title,
                        url,
                        [state](std::string value) {
                            // The view trims; complete the manager's wait.
                            state->complete(std::optional<std::string>{std::move(value)});
                        },
                        [state] { state->complete(std::nullopt); });
                if (self->presenter_ != nullptr) {
                    self->presenter_->invalidate();
                }
            });
        }
    };
    prompt.prompt_for_redirect_url = [state]() -> boost::asio::awaitable<std::optional<std::string>> {
        auto outcome = co_await support::detail::await_async_result(
                support::AsyncResult<std::optional<std::string>>{
                        support::AsyncProducer<std::optional<std::string>, support::Error>{
                                [state](support::AsyncCompletion<std::optional<std::string>, support::Error>
                                                completion) mutable noexcept {
                                    state->pending = std::move(completion);
                                }}});
        // A cancelled wait (the browser callback or the timeout won, or the
        // flow ended) resolves as no pasted URL.
        if (!outcome) {
            co_return std::nullopt;
        }
        co_return std::move(*outcome);
    };

    auto self = shared_from_this();
    auto host_lifetime = host_lifetime_.lock();
    if (host_lifetime == nullptr || hooks_.spawn_flow == nullptr) {
        return;
    }
    hooks_.spawn_flow(
            [self, host_lifetime = std::move(host_lifetime), name, title, prompt = std::move(prompt),
                    state, close_after]() mutable -> boost::asio::awaitable<void> {
                auto* running = self->manager();
                if (running == nullptr) {
                    co_return;
                }
                McpManagerPresenter presenter(*running);
                auto outcome =
                        co_await support::detail::await_async_result(presenter.run_action(name, McpServerAction::SignIn, std::move(prompt)));
                // Release the paste wait if it is still pending (the browser
                // callback or the timeout won the race); the late completion
                // is discarded.
                state->complete(std::nullopt);
                if (close_after) {
                    self->close_panel();
                } else {
                    self->present_server(name);
                }
                if (!outcome) {
                    self->show_error(outcome.error().message);
                    co_return;
                }
                if (outcome->message.has_value()) {
                    // pi: "Sign-in cancelled." is info, every other failure an
                    // error.
                    if (*outcome->message == "Sign-in cancelled.") {
                        self->show_status(*outcome->message);
                    } else {
                        self->show_error(*outcome->message);
                    }
                    co_return;
                }
                // pi `loginCommand`'s success line with the tool count the
                // reconnected server now offers.
                std::size_t tools = 0;
                for (const auto& view : presenter.servers()) {
                    if (view.entry.name == name && view.connection) {
                        tools = view.connection->tools.size();
                    }
                }
                self->show_status(std::format("Signed in to MCP server \"{}\" ({} tools).", name, tools));
            },
            "/mcp login");
}

// ── Subcommands (pi's `registerCommand("mcp")` handler) ─────────────────────

void McpManagerFlow::handle_command(std::string argument) {
    std::vector<std::string> tokens;
    std::string token;
    for (const char character : argument) {
        if (character == ' ' || character == '\t' || character == '\n' || character == '\r') {
            if (!token.empty()) {
                tokens.push_back(std::exchange(token, {}));
            }
        } else {
            token.push_back(character);
        }
    }
    if (!token.empty()) {
        tokens.push_back(std::move(token));
    }
    if (tokens.empty()) {
        open_manager();
        return;
    }
    if (tokens.size() > 2) {
        show_warning(std::string{mcp_usage()});
        return;
    }
    const std::string& action = tokens[0];
    const std::string name = tokens.size() == 2 ? tokens[1] : std::string{};
    if (action == "login") {
        run_login(name);
    } else if (action == "logout") {
        run_logout(name);
    } else if (action == "reconnect") {
        run_reconnect(name);
    } else {
        show_warning(std::string{mcp_usage()});
    }
}

std::optional<std::string> McpManagerFlow::pick_server(const std::string& name, const PickOptions& options) {
    auto* current = manager();
    if (current == nullptr) {
        return std::nullopt;
    }
    const auto servers = McpManagerPresenter(*current).servers();
    const auto is_eligible = [&options](const McpServerView& view) {
        return options.eligible != nullptr && options.eligible(view);
    };
    if (!name.empty()) {
        const auto found = std::ranges::find_if(servers, [&](const McpServerView& view) {
            return view.entry.name == name;
        });
        if (found == servers.end()) {
            show_error(no_server_named(name));
            return std::nullopt;
        }
        if (!is_eligible(*found)) {
            show_error(options.none_message);
            return std::nullopt;
        }
        return name;
    }
    std::vector<McpServerView> candidates;
    for (const auto& view : servers) {
        if (is_eligible(view)) {
            candidates.push_back(view);
        }
    }
    if (candidates.empty()) {
        show_status(options.none_message);
        return std::nullopt;
    }
    if (candidates.size() == 1) {
        return candidates.front().entry.name;
    }
    if (options.preferred != nullptr) {
        std::vector<McpServerView> preferred;
        for (const auto& view : candidates) {
            if (options.preferred(view)) {
                preferred.push_back(view);
            }
        }
        if (preferred.size() == 1) {
            return preferred.front().entry.name;
        }
    }
    // pi shows a select menu here; the listing names the candidates with the
    // `run /mcp login <name>` status shape so the user can type the command.
    show_status_lines(candidates);
    return std::nullopt;
}

void McpManagerFlow::run_login(std::string name) {
    if (manager() == nullptr) {
        return;
    }
    const auto oauth = [](const McpServerView& view) {
        return view.connection.has_value() && view.connection->oauth;
    };
    const auto needs_auth = [](const McpServerView& view) {
        return view.connection.has_value() && view.connection->state == McpServerViewState::NeedsAuth;
    };
    std::string target = name;
    if (target.empty()) {
        // pi `pickServer(oauthPick)`: the needs-auth servers are the login
        // candidates; one uses it directly, several list with the
        // `run /mcp login <name>` line.
        auto* current = manager();
        const auto servers = McpManagerPresenter(*current).servers();
        std::vector<McpServerView> auth_needed;
        std::vector<McpServerView> eligible;
        for (const auto& view : servers) {
            if (oauth(view)) {
                eligible.push_back(view);
            }
            if (needs_auth(view)) {
                auth_needed.push_back(view);
            }
        }
        if (auth_needed.size() == 1) {
            target = auth_needed.front().entry.name;
        } else if (auth_needed.empty()) {
            if (eligible.empty()) {
                show_status(oauth_none_message());
                return;
            }
            if (eligible.size() == 1) {
                target = eligible.front().entry.name;
            } else {
                show_status_lines(eligible);
                return;
            }
        } else {
            show_status_lines(auth_needed);
            return;
        }
    } else {
        PickOptions options{
                .eligible = std::move(oauth),
                .preferred = nullptr,
                .none_message = oauth_none_message(),
        };
        const auto picked = pick_server(target, options);
        if (!picked.has_value()) {
            return;
        }
        target = *picked;
    }
    run_sign_in(std::move(target), /* close_after */ true);
}

void McpManagerFlow::run_logout(std::string name) {
    auto* current = manager();
    if (current == nullptr) {
        return;
    }
    PickOptions options{
            .eligible = [](const McpServerView& view) {
                return view.connection.has_value() && view.connection->oauth;
            },
            .preferred = nullptr,
            .none_message = oauth_none_message(),
    };
    const auto picked = pick_server(name, options);
    if (!picked.has_value()) {
        return;
    }
    auto self = shared_from_this();
    auto host_lifetime = host_lifetime_.lock();
    if (host_lifetime == nullptr || hooks_.spawn_flow == nullptr) {
        return;
    }
    hooks_.spawn_flow(
            [self, host_lifetime = std::move(host_lifetime), name = *picked]() mutable
                    -> boost::asio::awaitable<void> {
                auto* running = self->manager();
                if (running == nullptr) {
                    co_return;
                }
                const auto removed =
                        co_await support::detail::await_async_result(running->sign_out(name));
                if (!removed) {
                    self->show_error(removed.error().message);
                    co_return;
                }
                // pi's two verbatim outcomes.
                if (*removed) {
                    self->show_status(std::format("Signed out of MCP server \"{}\".", name));
                } else {
                    self->show_status(std::format("No stored credentials for MCP server \"{}\".", name));
                }
            },
            "/mcp logout");
}

void McpManagerFlow::run_reconnect(std::string name) {
    auto* current = manager();
    if (current == nullptr) {
        return;
    }
    PickOptions options{
            .eligible = [](const McpServerView& view) { return view.connection.has_value(); },
            .preferred = [](const McpServerView& view) {
                return view.connection.has_value() &&
                       (view.connection->state == McpServerViewState::Failed ||
                               view.connection->state == McpServerViewState::Disconnected);
            },
            .none_message = "No enabled MCP server to reconnect.",
    };
    const auto picked = pick_server(name, options);
    if (!picked.has_value()) {
        return;
    }
    auto self = shared_from_this();
    auto host_lifetime = host_lifetime_.lock();
    if (host_lifetime == nullptr || hooks_.spawn_flow == nullptr) {
        return;
    }
    hooks_.spawn_flow(
            [self, host_lifetime = std::move(host_lifetime), name = *picked]() mutable
                    -> boost::asio::awaitable<void> {
                auto* running = self->manager();
                if (running == nullptr) {
                    co_return;
                }
                const auto failure =
                        co_await support::detail::await_async_result(running->reconnect(name));
                if (!failure) {
                    self->show_error(failure.error().message);
                    co_return;
                }
                if (failure->has_value()) {
                    self->show_error(**failure);
                    co_return;
                }
                // pi's success line with the described state.
                std::string described = "connected";
                for (const auto& view : McpManagerPresenter(*running).servers()) {
                    if (view.entry.name == name) {
                        described = mcp_describe_state(view);
                    }
                }
                self->show_status(std::format("Reconnected to MCP server \"{}\" ({}).", name, described));
            },
            "/mcp reconnect");
}

} // namespace cch::coding_agent::tui
