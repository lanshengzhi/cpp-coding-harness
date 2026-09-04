#include "AuthFlowController.hpp"

#include <cch/agent/AgentContext.hpp>

#include "coding_agent/AgentSession.hpp"
#include "coding_agent/tui/ErrorPresentation.hpp"
#include "coding_agent/tui/InteractiveView.hpp"
#include "coding_agent/tui/KeybindingHints.hpp"
#include "coding_agent/tui/LoginPresentation.hpp"
#include "coding_agent/tui/ModalPresenter.hpp"
#include "coding_agent/tui/PromptSlot.hpp"
#include "coding_agent/tui/SharedKeybindings.hpp"
#include "coding_agent/tui/Theme.hpp"

#include "support/AsyncResultBridge.hpp"

#include <cch/ai/Auth.hpp>
#include <cch/coding_agent/ModelRuntime.hpp>
#include <cch/support/Error.hpp>
#include <cch/tui/SelectList.hpp>

#include <boost/asio/redirect_error.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <algorithm>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace cch::coding_agent::tui {
namespace {

[[nodiscard]] support::Error prompt_cancelled_error() {
    return support::make_error(support::ErrorCode::Cancelled, "Login cancelled");
}

[[nodiscard]] bool is_unknown_model(const ai::Model& model) {
    return model.provider == agent::detail::kDefaultModel.provider &&
        model.id == agent::detail::kDefaultModel.id &&
        model.api == agent::detail::kDefaultModel.api;
}

} // namespace

AuthFlowController::AuthFlowController(
    boost::asio::any_io_executor executor,
    ModalPresenter& presenter,
    std::weak_ptr<void> host_lifetime,
    AuthFlowHostHooks hooks,
    std::shared_ptr<SharedKeybindings> keybindings)
    : executor_(std::move(executor)),
      presenter_(&presenter),
      host_lifetime_(std::move(host_lifetime)),
      hooks_(std::move(hooks)),
      keybindings_(std::move(keybindings)) {}

void AuthFlowController::open_login(std::string provider_ref) {
    if (closed_ || hooks_.post_on_executor == nullptr) return;
    auto self = shared_from_this();
    hooks_.post_on_executor([self, provider_ref = std::move(provider_ref)]() mutable {
        self->spawn(
            [self, provider_ref = std::move(provider_ref)]() mutable
                -> boost::asio::awaitable<void> {
                co_await self->handle_login_command(std::move(provider_ref));
            },
            "Native TUI login flow failed");
    });
}

void AuthFlowController::open_logout() {
    if (closed_ || hooks_.post_on_executor == nullptr) return;
    auto self = shared_from_this();
    hooks_.post_on_executor([self] {
        self->spawn(
            [self]() -> boost::asio::awaitable<void> {
                co_await self->run_logout();
            },
            "Native TUI logout flow failed");
    });
}

void AuthFlowController::close() {
    if (closed_.exchange(true)) return;
    // Cancel every admitted dialog and resolve every admitted prompt slot so
    // each detached login flow reaches a terminal outcome and finish() can
    // await quiescence (ADR 0040); a second `/login` may have admitted a
    // concurrent flow whose dialog replaced the slot.
    for (auto& dialog : active_dialogs_) {
        dialog->cancel();
    }
    active_dialogs_.clear();
    for (auto& slot : active_prompt_slots_) {
        slot->resolve(std::unexpected(prompt_cancelled_error()));
    }
    active_prompt_slots_.clear();
    presenter_->restore_prompt_slot();
}

void AuthFlowController::track_dialog(std::shared_ptr<LoginDialogComponent> dialog) {
    active_dialogs_.push_back(std::move(dialog));
}

void AuthFlowController::untrack_dialog(
    const std::shared_ptr<LoginDialogComponent>& dialog) {
    std::erase(active_dialogs_, dialog);
}

void AuthFlowController::track_prompt_slot(std::shared_ptr<PromptSlot> slot) {
    active_prompt_slots_.push_back(std::move(slot));
}

void AuthFlowController::untrack_prompt_slot(const std::shared_ptr<PromptSlot>& slot) {
    std::erase(active_prompt_slots_, slot);
}

bool AuthFlowController::is_live() {
    auto* session = current_session();
    return !closed_ && session != nullptr && session->is_open() &&
        hooks_.live_theme != nullptr && keybindings_ != nullptr;
}

AgentSession* AuthFlowController::current_session() {
    return hooks_.current_session != nullptr ? hooks_.current_session() : nullptr;
}

void AuthFlowController::spawn(
    std::move_only_function<boost::asio::awaitable<void>()> start,
    std::string failure_label) {
    if (closed_ || hooks_.spawn_flow == nullptr) return;
    auto self = shared_from_this();
    auto host_lifetime = host_lifetime_.lock();
    if (host_lifetime == nullptr) return;
    hooks_.spawn_flow(
        [self, host_lifetime = std::move(host_lifetime), start = std::move(start)]()
            mutable -> boost::asio::awaitable<void> {
            co_await start();
        },
        std::move(failure_label));
}

std::size_t AuthFlowController::action_generation() {
    return hooks_.action_generation != nullptr ? hooks_.action_generation() : 0;
}

OpenBrowserSink AuthFlowController::open_browser_hook() {
    const auto weak = weak_from_this();
    const auto captured_generation = action_generation();
    return [weak, captured_generation](std::string url) {
        if (const auto self = weak.lock(); self &&
            self->action_generation() == captured_generation &&
            self->hooks_.open_browser != nullptr) {
            self->hooks_.open_browser(captured_generation, std::move(url));
        }
    };
}

LoginDialogActionSink AuthFlowController::dialog_invalidate_hook() {
    const auto weak = weak_from_this();
    return [weak] {
        if (const auto self = weak.lock()) self->presenter_->request_render();
    };
}

boost::asio::awaitable<void> AuthFlowController::handle_login_command(
    std::string provider_ref) {
    auto* session = current_session();
    if (session == nullptr) co_return;
    auto runtime = session->model_runtime();
    if (!runtime) co_return;
    const auto captured_generation = action_generation();
    // pi awaits getAvailable() before presenting login options. A completion
    // admitted before a Session replacement must not present against the
    // replacement (ADR 0040 generation retirement).
    static_cast<void>(co_await support::detail::await_async_result(runtime->get_available()));
    if (closed_ || captured_generation != action_generation()) co_return;
    const auto ref = interactive_view_detail::trim_editor_submission(std::move(provider_ref));
    if (ref.empty()) {
        show_login_auth_type_selector(std::nullopt);
        co_return;
    }
    auto matches = find_login_provider_options(
        compute_login_provider_options(*runtime), ref);
    if (matches.size() == 1) {
        co_await start_provider_login(matches.front());
        co_return;
    }
    if (matches.size() > 1) {
        const auto same_provider = std::all_of(
            matches.begin(), matches.end(), [&](const auto& option) {
                return option.id == matches.front().id;
            });
        if (same_provider) {
            show_login_auth_type_selector(std::move(matches));
            co_return;
        }
    }
    show_login_provider_selector(std::nullopt, ref);
}

void AuthFlowController::show_login_auth_type_selector(
    std::optional<std::vector<AuthSelectorProvider>> provider_options) {
    if (!is_live()) return;
    const std::string subscription_label{login_subscription_label()};
    const std::string api_key_label{login_api_key_label()};
    std::vector<std::string> options;
    bool has_oauth = true;
    bool has_api_key = true;
    if (provider_options) {
        has_oauth = std::any_of(
            provider_options->begin(), provider_options->end(), [](const auto& option) {
                return option.auth_type == AuthSelectorType::OAuth;
            });
        has_api_key = std::any_of(
            provider_options->begin(), provider_options->end(), [](const auto& option) {
                return option.auth_type == AuthSelectorType::ApiKey;
            });
    }
    if (has_oauth) options.push_back(subscription_label);
    if (has_api_key) options.push_back(api_key_label);
    if (options.empty()) {
        presenter_->show_status(std::string{login_methods_empty_message()});
        return;
    }
    if (provider_options && options.size() == 1 && !provider_options->empty()) {
        const auto self = shared_from_this();
        const auto option = provider_options->front();
        spawn(
            [self, option]() -> boost::asio::awaitable<void> {
                co_await self->start_provider_login(option);
            },
            "Native TUI login flow failed");
        return;
    }
    const std::string title = provider_options && !provider_options->empty()
        ? "Select authentication method for " + provider_options->front().name + ":"
        : "Select authentication method:";
    // The generation that presented this picker: a selection admitted after a
    // Session replacement must not start a login against the replacement
    // (ADR 0040 callback lifetime).
    const auto selector_generation = action_generation();
    const auto weak = weak_from_this();
    std::vector<cch::tui::SelectItem> items;
    items.reserve(options.size());
    for (const auto& option : options) {
        items.push_back(cch::tui::SelectItem{.value = option, .label = option});
    }
    const auto& theme = hooks_.live_theme();
    auto selector = std::make_shared<cch::tui::SelectList>(std::move(items),
            cch::tui::SelectListOptions{
                    .theme = theme.select_list_theme(),
                    .on_select = [weak, provider_options, subscription_label, selector_generation](
                                         const cch::tui::SelectItem& item) -> support::ExpectedVoid {
                        if (const auto self = weak.lock()) {
                            self->hooks_.post_on_executor([self,
                                                                  provider_options,
                                                                  subscription_label,
                                                                  selector_generation,
                                                                  selected = item.value]() mutable {
                                if (self->closed_ || self->action_generation() != selector_generation) {
                                    return;
                                }
                                self->presenter_->restore_prompt_slot();
                                const auto type = selected == subscription_label ? AuthSelectorType::OAuth
                                                                                 : AuthSelectorType::ApiKey;
                                if (provider_options) {
                                    const auto found = std::find_if(provider_options->begin(),
                                            provider_options->end(),
                                            [type](const auto& option) { return option.auth_type == type; });
                                    if (found == provider_options->end()) return;
                                    const auto option = *found;
                                    self->spawn(
                                            [self, option]() -> boost::asio::awaitable<void> {
                                                co_await self->start_provider_login(option);
                                            },
                                            "Native TUI login flow failed");
                                    return;
                                }
                                self->show_login_provider_selector(type, "");
                            });
                        }
                        return {};
                    },
                    .on_cancel = [weak]() -> support::ExpectedVoid {
                        if (const auto self = weak.lock()) {
                            self->hooks_.post_on_executor([self] { self->presenter_->restore_prompt_slot(); });
                        }
                        return {};
                    },
                    .keybindings = keybindings_->get(),
                    .title = title,
                    .hint = generic_select_list_hint(*keybindings_->get()),
                    .border_hook = theme.foreground_hook(ThemeToken::Border),
            });
    presenter_->replace_prompt_slot(std::move(selector));
}

void AuthFlowController::show_login_provider_selector(
    std::optional<AuthSelectorType> filter,
    std::string initial_search) {
    if (!is_live()) return;
    auto* session = current_session();
    auto runtime = session != nullptr ? session->model_runtime() : nullptr;
    if (!runtime) return;
    auto options = compute_login_provider_options(*runtime, filter);
    if (options.empty()) {
        presenter_->show_status(std::string{login_provider_selector_empty_message(filter)});
        return;
    }
    const auto weak = weak_from_this();
    // The generation that presented this picker (see
    // show_login_auth_type_selector).
    const auto selector_generation = action_generation();
    auto selector = std::make_shared<OAuthSelectorComponent>(
        hooks_.live_theme(),
        keybindings_->get(),
        AuthSelectorMode::Login,
        options,
        [weak, options, selector_generation](std::string provider_id, AuthSelectorType type) {
            if (const auto self = weak.lock()) {
                self->hooks_.post_on_executor(
                    [self,
                     options,
                     selector_generation,
                     provider_id = std::move(provider_id),
                     type]() mutable {
                        if (self->closed_ ||
                            self->action_generation() != selector_generation) {
                            return;
                        }
                        self->presenter_->restore_prompt_slot();
                        const auto found = std::find_if(
                            options.begin(),
                            options.end(),
                            [&](const auto& option) {
                                return option.id == provider_id && option.auth_type == type;
                            });
                        if (found == options.end()) return;
                        const auto option = *found;
                        self->spawn(
                            [self, option]() -> boost::asio::awaitable<void> {
                                co_await self->start_provider_login(option);
                            },
                            "Native TUI login flow failed");
                    });
            }
        },
        [weak, filter] {
            if (const auto self = weak.lock()) {
                self->hooks_.post_on_executor([self, filter] {
                    self->presenter_->restore_prompt_slot();
                    // pi: cancelling a filtered picker returns to the
                    // auth-type picker.
                    if (filter) self->show_login_auth_type_selector(std::nullopt);
                });
            }
        },
        std::move(initial_search));
    presenter_->replace_prompt_slot(std::move(selector));
}

boost::asio::awaitable<void> AuthFlowController::start_provider_login(
    AuthSelectorProvider option) {
    if (option.auth_type == AuthSelectorType::OAuth) {
        co_await run_login_dialog(option.id, option.name, ai::AuthType::OAuth);
        co_return;
    }
    if (option.has_login) {
        co_await run_login_dialog(option.id, option.name, ai::AuthType::ApiKey);
        co_return;
    }
    show_ambient_auth_dialog(option);
}

void AuthFlowController::show_ambient_auth_dialog(const AuthSelectorProvider& option) {
    if (!is_live()) return;
    const auto weak = weak_from_this();
    auto dialog = std::make_shared<LoginDialogComponent>(
        hooks_.live_theme(),
        keybindings_->get(),
        option.name + " setup",
        dialog_invalidate_hook(),
        open_browser_hook(),
        [weak] {
            if (const auto self = weak.lock()) {
                self->hooks_.post_on_executor(
                    [self] { self->presenter_->restore_prompt_slot(); });
            }
        });
    dialog->show_info(
        option.method_name.value_or("Authentication") +
            " is configured outside cch.",
        {},
        true);
    presenter_->replace_prompt_slot(std::move(dialog));
}

boost::asio::awaitable<void> AuthFlowController::run_login_dialog(
    std::string provider_id,
    std::string provider_name,
    ai::AuthType type) {
    auto* session = current_session(); // borrowed; re-resolved after awaits.
    if (session == nullptr) co_return;
    auto runtime = session->model_runtime();
    if (!runtime || !is_live()) co_return;
    const auto captured_generation = action_generation();
    const auto previous_model = session->snapshot().agent_state.model;
    auto dialog = std::make_shared<LoginDialogComponent>(
        hooks_.live_theme(),
        keybindings_->get(),
        "Login to " + provider_name,
        dialog_invalidate_hook(),
        open_browser_hook());
    track_dialog(dialog);
    presenter_->replace_prompt_slot(dialog);

    ai::AuthInteraction interaction;
    interaction.stop_token = dialog->stop_token();
    const auto self = shared_from_this();
    interaction.prompt = [self, dialog](ai::AuthPrompt prompt) -> cch::support::AsyncResult<std::string> {
        return cch::support::detail::make_async_result(
                [self, dialog, prompt = std::move(prompt)]() mutable
                        -> boost::asio::awaitable<support::Expected<std::string>> {
                    co_return co_await self->show_auth_prompt(dialog, std::move(prompt));
                });
    };
    interaction.notify = [self, dialog](const ai::AuthEvent& event) {
        self->notify_auth_dialog(*dialog, event);
    };
    const auto completion_provider_id = provider_id;
    auto result = co_await support::detail::await_async_result(
            runtime->login(std::move(provider_id), type, std::move(interaction)));
    untrack_dialog(dialog);
    // A late login completion admitted before a Session replacement must not
    // restore the slot or report against the replacement (ADR 0040).
    if (closed_ || captured_generation != action_generation()) co_return;
    presenter_->restore_prompt_slot();
    if (result) {
        co_await complete_provider_authentication(
            completion_provider_id, provider_name, type, previous_model);
        co_return;
    }
    // Login Cancellation suppresses failure UI on the stable cancelled kind
    // (#328); every other failure shows pi's failure text.
    if (result.error().code != support::ErrorCode::Cancelled) {
        presenter_->show_error(
            (type == ai::AuthType::OAuth
                 ? "Failed to login to " + provider_name
                 : "Failed to save API key for " + provider_name) +
            ": " + combined_error_text(result.error()));
    }
}

boost::asio::awaitable<void> AuthFlowController::complete_provider_authentication(
    std::string provider_id,
    std::string provider_name,
    ai::AuthType type,
    ai::Model previous_model) {
    auto* session = current_session(); // borrowed; re-resolved after awaits.
    if (session == nullptr) co_return;
    const auto captured_generation = action_generation();
    auto runtime = session->model_runtime();
    if (!runtime) co_return;
    auto available = co_await support::detail::await_async_result(runtime->get_available());
    if (closed_ || captured_generation != action_generation()) co_return;
    session = current_session();
    if (session == nullptr) co_return;
    const auto selector_type = type == ai::AuthType::OAuth
        ? AuthSelectorType::OAuth
        : AuthSelectorType::ApiKey;
    const auto action_label = login_action_label(selector_type, provider_name);
    std::optional<ai::Model> selected_model;
    std::optional<std::string> selection_error;
    if (is_unknown_model(previous_model)) {
        std::vector<ai::Model> provider_models;
        if (available) {
            for (const auto& model : *available) {
                if (model.provider == provider_id) provider_models.push_back(model);
            }
        }
        const auto default_id = ModelRuntime::default_model_for_provider(provider_id);
        if (!default_id) {
            selection_error =
                login_selection_error_no_default_model(action_label, provider_id);
        } else if (provider_models.empty()) {
            selection_error = login_selection_error_no_models(action_label);
        } else {
            const auto found = std::find_if(
                provider_models.begin(),
                provider_models.end(),
                [&](const auto& model) { return model.id == *default_id; });
            if (found == provider_models.end()) {
                selection_error =
                    login_selection_error_default_unavailable(action_label, *default_id);
            } else {
                auto set = co_await session->set_model(*found);
                if (closed_ || captured_generation != action_generation()) co_return;
                if (set) {
                    selected_model = *found;
                } else {
                    selection_error = login_selection_error_select_failed(
                        action_label, set.error().message);
                }
            }
        }
    }
    // pi's updateAvailableProviderCount/footer invalidate/editor border hooks
    // land with the session UI binding; availability refresh above is the data
    // effect here.
    const auto auth_path = auth_path_display(runtime->agent_dir());
    presenter_->show_status(login_success_status(
        action_label,
        selected_model ? std::optional{selected_model->id} : std::nullopt,
        auth_path));
    if (selection_error) presenter_->show_error(*selection_error);
}

boost::asio::awaitable<support::Expected<std::string>> AuthFlowController::show_auth_select(
    std::shared_ptr<LoginDialogComponent> dialog,
    ai::AuthPromptSelect select,
    std::optional<std::stop_token> per_prompt) {
    const auto executor = co_await boost::asio::this_coro::executor;
    auto slot = std::make_shared<PromptSlot>(executor);
    track_prompt_slot(slot);
    std::vector<cch::tui::SelectItem> items;
    items.reserve(select.options.size());
    for (const auto& option : select.options) {
        items.push_back(cch::tui::SelectItem{.value = option.label, .label = option.label});
    }
    const auto& theme = hooks_.live_theme();
    const auto weak = weak_from_this();
    auto selector = std::make_shared<cch::tui::SelectList>(std::move(items),
            cch::tui::SelectListOptions{
                    .theme = theme.select_list_theme(),
                    .on_select = [weak, slot, dialog, options = select.options](
                                         const cch::tui::SelectItem& item) -> support::ExpectedVoid {
                        if (const auto self = weak.lock()) {
                            self->hooks_.post_on_executor(
                                    [self, slot, dialog, label = item.value, options = std::move(options)]() mutable {
                                        // pi restoreDialog, then resolve the option id.
                                        self->presenter_->replace_prompt_slot(dialog);
                                        const auto found = std::find_if(options.begin(),
                                                options.end(),
                                                [&](const auto& option) { return option.label == label; });
                                        if (found != options.end()) {
                                            slot->resolve(found->id);
                                        } else {
                                            slot->resolve(std::unexpected(prompt_cancelled_error()));
                                        }
                                    });
                        }
                        return {};
                    },
                    .on_cancel = [weak, slot, dialog]() -> support::ExpectedVoid {
                        if (const auto self = weak.lock()) {
                            self->hooks_.post_on_executor([self, slot, dialog] {
                                self->presenter_->replace_prompt_slot(dialog);
                                slot->resolve(std::unexpected(prompt_cancelled_error()));
                            });
                        }
                        return {};
                    },
                    .keybindings = keybindings_->get(),
                    .title = select.message,
                    .hint = generic_select_list_hint(*keybindings_->get()),
                    .border_hook = theme.foreground_hook(ThemeToken::Border),
            });
    presenter_->replace_prompt_slot(std::move(selector));

    // pi's per-prompt race: an aborted per-prompt token rejects without
    // touching the slot's UI (the flow's unwind restores the editor).
    std::optional<std::stop_callback<std::move_only_function<void()>>> on_abort;
    if (per_prompt) {
        on_abort.emplace(*per_prompt, [slot] {
            slot->resolve(std::unexpected(prompt_cancelled_error()));
        });
    }
    boost::system::error_code error;
    auto result = co_await slot->channel.async_receive(
        boost::asio::redirect_error(boost::asio::use_awaitable, error));
    untrack_prompt_slot(slot);
    if (closed_) co_return std::unexpected(prompt_cancelled_error());
    if (error) {
        co_return std::unexpected(support::make_error(
            support::ErrorCode::Unknown,
            "login select channel failed",
            error.message()));
    }
    co_return std::move(result);
}

boost::asio::awaitable<support::Expected<std::string>> AuthFlowController::show_auth_prompt(
    std::shared_ptr<LoginDialogComponent> dialog,
    ai::AuthPrompt prompt) {
    auto per_prompt = std::move(prompt.stop_token);
    if (per_prompt && per_prompt->stop_requested()) {
        co_return std::unexpected(prompt_cancelled_error());
    }
    if (closed_) co_return std::unexpected(prompt_cancelled_error());
    if (auto* select = std::get_if<ai::AuthPromptSelect>(&prompt.kind)) {
        co_return co_await show_auth_select(
            std::move(dialog), std::move(*select), std::move(per_prompt));
    }
    if (const auto* manual = std::get_if<ai::AuthPromptManualCode>(&prompt.kind)) {
        if (!per_prompt) co_return co_await dialog->show_manual_input(manual->message);
        std::stop_callback on_abort(*per_prompt, [dialog] {
            dialog->cancel_pending_prompt();
        });
        co_return co_await dialog->show_manual_input(manual->message);
    }
    std::string message;
    std::optional<std::string> placeholder;
    if (const auto* text = std::get_if<ai::AuthPromptText>(&prompt.kind)) {
        message = text->message;
        placeholder = text->placeholder;
    } else if (const auto* secret = std::get_if<ai::AuthPromptSecret>(&prompt.kind)) {
        message = secret->message;
        placeholder = secret->placeholder;
    }
    if (!per_prompt) {
        co_return co_await dialog->show_prompt(
            std::move(message), std::move(placeholder));
    }
    std::stop_callback on_abort(*per_prompt, [dialog] {
        dialog->cancel_pending_prompt();
    });
    co_return co_await dialog->show_prompt(
        std::move(message), std::move(placeholder));
}

void AuthFlowController::notify_auth_dialog(
    LoginDialogComponent& dialog,
    const ai::AuthEvent& event) {
    if (closed_) return;
    if (const auto* url = std::get_if<ai::AuthUrl>(&event.kind)) {
        dialog.show_auth(url->url, url->instructions);
        return;
    }
    if (const auto* device = std::get_if<ai::AuthDeviceCode>(&event.kind)) {
        dialog.show_device_code(device->user_code, device->verification_uri);
        dialog.show_waiting("Waiting for authentication...");
        return;
    }
    if (const auto* info = std::get_if<ai::AuthInfo>(&event.kind)) {
        std::vector<std::pair<std::string, std::optional<std::string>>> links;
        links.reserve(info->links.size());
        for (const auto& link : info->links) {
            links.emplace_back(link.url, link.label);
        }
        dialog.show_info(info->message, std::move(links));
        return;
    }
    if (const auto* progress = std::get_if<ai::AuthProgress>(&event.kind)) {
        dialog.show_progress(progress->message);
    }
}

boost::asio::awaitable<void> AuthFlowController::run_logout() {
    auto* session = current_session();
    if (session == nullptr) co_return;
    auto runtime = session->model_runtime();
    if (!runtime) co_return;
    const auto captured_generation = action_generation();
    auto credentials = co_await support::detail::await_async_result(runtime->list_credentials());
    if (closed_ || captured_generation != action_generation()) co_return;
    if (!credentials) {
        presenter_->show_error("Logout failed: " + combined_error_text(credentials.error()));
        co_return;
    }
    auto options = compute_logout_provider_options(*runtime, std::move(*credentials));
    if (options.empty()) {
        presenter_->show_status(std::string{logout_no_credentials_message()});
        co_return;
    }
    if (!is_live()) co_return;
    // The generation that presented this picker (see
    // show_login_auth_type_selector).
    const auto selector_generation = action_generation();
    const auto weak = weak_from_this();
    auto selector = std::make_shared<OAuthSelectorComponent>(
        hooks_.live_theme(),
        keybindings_->get(),
        AuthSelectorMode::Logout,
        options,
        [weak, options, selector_generation](std::string provider_id, AuthSelectorType) {
            if (const auto self = weak.lock()) {
                self->hooks_.post_on_executor(
                    [self, options, selector_generation, provider_id = std::move(provider_id)]() mutable {
                        if (self->closed_ ||
                            self->action_generation() != selector_generation) {
                            return;
                        }
                        self->presenter_->restore_prompt_slot();
                        const auto found = std::find_if(
                            options.begin(),
                            options.end(),
                            [&](const auto& option) { return option.id == provider_id; });
                        if (found == options.end()) return;
                        const auto option = *found;
                        self->spawn(
                            [self, option]() -> boost::asio::awaitable<void> {
                                co_await self->run_logout_provider(option);
                            },
                            "Native TUI logout flow failed");
                    });
            }
        },
        [weak] {
            if (const auto self = weak.lock()) {
                self->hooks_.post_on_executor(
                    [self] { self->presenter_->restore_prompt_slot(); });
            }
        });
    presenter_->replace_prompt_slot(std::move(selector));
}

boost::asio::awaitable<void> AuthFlowController::run_logout_provider(
    AuthSelectorProvider option) {
    auto* session = current_session();
    if (session == nullptr) co_return;
    auto runtime = session->model_runtime();
    if (!runtime) co_return;
    const auto captured_generation = action_generation();
    if (auto logged_out = co_await support::detail::await_async_result(runtime->logout(option.id)); !logged_out) {
        if (closed_ || captured_generation != action_generation()) co_return;
        presenter_->show_error("Logout failed: " + combined_error_text(logged_out.error()));
        co_return;
    }
    // pi: updateAvailableProviderCount after logout.
    static_cast<void>(co_await support::detail::await_async_result(runtime->get_available()));
    if (closed_ || captured_generation != action_generation()) co_return;
    presenter_->show_status(logout_success_message(option.auth_type, option.name));
}

} // namespace cch::coding_agent::tui
