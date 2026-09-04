#include "SessionFlowController.hpp"

#include "coding_agent/runtime/AgentSessionInteractiveAccess.hpp"
#include "coding_agent/tui/ErrorPresentation.hpp"
#include "coding_agent/tui/KeybindingHints.hpp"
#include "coding_agent/tui/PromptSlot.hpp"
#include "coding_agent/tui/SharedKeybindings.hpp"
#include "coding_agent/tui/Theme.hpp"
#include "support/AsyncResultBridge.hpp"

#include <cch/coding_agent/AgentConfigDir.hpp>
#include <cch/coding_agent/ProjectResources.hpp>
#include <cch/coding_agent/ProjectTrust.hpp>
#include <cch/support/Error.hpp>
#include <cch/tui/SelectList.hpp>

#include <boost/asio/redirect_error.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cch::coding_agent::tui {
namespace {

[[nodiscard]] support::Error prompt_cancelled_error() {
    return support::make_error(support::ErrorCode::Cancelled, "Login cancelled");
}

[[nodiscard]] support::Error project_resource_detection_unavailable_error() {
    return support::make_error(
            support::ErrorCode::Workspace, "Project resource detection filesystem capability is unavailable");
}

[[nodiscard]] std::string format_project_trust_prompt(
    const std::filesystem::path& cwd) {
    return std::format(
        "Trust project folder?\n{}\n\nThis allows cch to load .pi settings "
        "and resources.",
        cwd.string());
}

[[nodiscard]] std::vector<SessionDiagnostic> convert_trust_diagnostics(
    const std::vector<ProjectTrustDiagnostic>& diagnostics) {
    std::vector<SessionDiagnostic> converted;
    converted.reserve(diagnostics.size());
    for (const auto& diagnostic : diagnostics) {
        converted.push_back(SessionDiagnostic{
            .severity =
                diagnostic.severity == ProjectTrustDiagnosticSeverity::Error
                ? SessionDiagnostic::Severity::Error
                : SessionDiagnostic::Severity::Warning,
            .code = "trust:" + diagnostic.code,
            .message = diagnostic.message,
            .path = diagnostic.path,
        });
    }
    return converted;
}

/// The hint row of the retired generic string-list selector, rebuilt as
/// plain chrome text for the shared SelectList with the registry's live key
/// labels (SelectList chrome rows carry no per-key styling).
[[nodiscard]] std::string generic_list_hint(const cch::tui::KeybindingRegistry& keybindings) {
    const auto key_label = [&keybindings](std::string_view action) {
        const auto text = keybindings.key_text(action);
        return text.empty() ? std::string{"Unbound"} : format_key_text(text);
    };
    return "↑↓ navigate  " + key_label("tui.select.confirm") + " select  " + key_label("tui.select.cancel") + " cancel";
}

} // namespace

boost::asio::awaitable<support::Expected<ProjectResourceDetectionResult>>
SessionFlowController::detect_project_resources_for(const std::filesystem::path& workspace) {
    if (hooks_.project_resource_filesystems == nullptr) {
        co_return std::unexpected(project_resource_detection_unavailable_error());
    }
    auto filesystems = hooks_.project_resource_filesystems(workspace);
    auto detection = co_await support::detail::await_async_result(coding_agent::detect_project_resources(
            std::move(filesystems), coding_agent::home_directory() / ".agents" / "skills", stop_source_.get_token()));
    if (!detection) {
        co_return std::unexpected(harness::to_util_error(std::move(detection.error())));
    }
    co_return std::move(*detection);
}

boost::asio::awaitable<support::ExpectedVoid> SessionFlowController::arm_auto_trust_on_reload(
        std::filesystem::path workspace, std::optional<bool> trust_override) {
    auto_trust_on_reload_cwd_.reset();
    boot_detection_workspace_.reset();
    boot_detection_.reset();
    // pi `resolveProjectTrusted`: an override decides first, before any
    // resource detection or store walk.
    if (trust_override.has_value() || closed_) {
        co_return support::ExpectedVoid{};
    }
    auto detection = co_await detect_project_resources_for(workspace);
    if (closed_ || stop_source_.stop_requested()) {
        co_return support::ExpectedVoid{};
    }
    if (!detection) {
        co_return std::unexpected(std::move(detection.error()));
    }
    boot_detection_workspace_ = workspace;
    boot_detection_ = std::move(*detection);
    if (!needs_project_trust_resolution(*boot_detection_)) {
        auto_trust_on_reload_cwd_ = std::move(workspace);
    }
    co_return support::ExpectedVoid{};
}

boost::asio::awaitable<support::Expected<bool>> SessionFlowController::resolve_boot_trust(
        std::filesystem::path workspace, std::optional<bool> trust_override) {
    // pi `resolveProjectTrusted`: an override decides first, before any
    // resource detection or store walk.
    if (trust_override.has_value()) {
        boot_detection_workspace_.reset();
        boot_detection_.reset();
        co_return support::Expected<bool>{*trust_override};
    }

    support::Expected<ProjectResourceDetectionResult> detection{ProjectResourceDetectionResult{}};
    if (boot_detection_workspace_ && *boot_detection_workspace_ == workspace) {
        detection = support::Expected<ProjectResourceDetectionResult>{
                std::move(*std::exchange(boot_detection_, std::nullopt))};
        boot_detection_workspace_.reset();
    } else {
        detection = co_await detect_project_resources_for(workspace);
    }
    if (closed_ || stop_source_.stop_requested()) {
        co_return false;
    }
    if (!detection) {
        co_return std::unexpected(std::move(detection.error()));
    }
    const bool trust_needed = needs_project_trust_resolution(*detection);
    ProjectTrustStore store{coding_agent::trust_store_file_path()};
    const auto default_trust = settings_manager_
        ? settings_manager_->default_project_trust().value_or(DefaultProjectTrust::Ask)
        : DefaultProjectTrust::Ask;
    auto resolved = resolve_project_trust(
        workspace,
        trust_needed,
        store,
        default_trust,
        trust_override);
    if (!resolved.diagnostics.empty() && hooks_.report_boot_diagnostics != nullptr) {
        hooks_.report_boot_diagnostics(
            action_generation(),
            convert_trust_diagnostics(std::move(resolved.diagnostics)));
    }
    if (resolved.source != ProjectTrustSource::DefaultAskNoUi) {
        co_return resolved.decision == ProjectTrustDecision::Trusted;
    }

    auto option = co_await show_boot_trust_prompt(workspace);
    if (!option) co_return false;
    if (!option->updates.empty()) {
        if (auto saved = store.setMany(option->updates); !saved) {
            presenter_->show_error(combined_error_text(saved.error()));
        }
    }
    co_return option->trusted;
}

boost::asio::awaitable<support::Expected<bool>>
SessionFlowController::maybe_save_implicit_project_trust_after_reload() {
    // The armed workspace is consumed by exactly one decision attempt. The
    // Session pointer and action generation are retained across the async
    // detection so replacement cannot save a decision for a stale Session.
    const auto arm = std::exchange(auto_trust_on_reload_cwd_, std::nullopt);
    auto* session = current_session();
    if (!arm || session == nullptr) co_return false;
    const auto captured_generation = action_generation();
    const auto* captured_session = session;
    const auto cwd = session->workspace();
    if (*arm != cwd ||
        !detail::AgentSessionInteractiveAccess::is_project_trusted(*session)) {
        co_return false;
    }
    auto detection = co_await detect_project_resources_for(cwd);
    if (closed_ || stop_source_.stop_requested() || captured_generation != action_generation() ||
            current_session() != captured_session) {
        co_return false;
    }
    if (!detection) {
        co_return std::unexpected(std::move(detection.error()));
    }
    if (!needs_project_trust_resolution(*detection) ||
            !detail::AgentSessionInteractiveAccess::is_project_trusted(*session)) {
        co_return false;
    }

    ProjectTrustStore store{coding_agent::trust_store_file_path()};
    auto entry = store.getEntry(cwd);
    if (!entry) {
        if (hooks_.show_warning != nullptr) {
            hooks_.show_warning(
                "Could not save project trust after reload: " + entry.error().message);
        }
        presenter_->invalidate();
        co_return false;
    }
    if (entry->has_value()) co_return false;
    if (auto saved = store.setMany(std::vector<ProjectTrustUpdate>{
            ProjectTrustUpdate{
                .path = cwd.string(),
                .decision = ProjectTrustDecision::Trusted,
            },
        });
        !saved) {
        if (hooks_.show_warning != nullptr) {
            hooks_.show_warning(
                "Could not save project trust after reload: " + saved.error().message);
        }
        presenter_->invalidate();
        co_return false;
    }
    co_return true;
}

void SessionFlowController::show_trust_selector() {
    if (!is_live()) return;
    auto* session = current_session();
    if (session == nullptr || !session->is_open() || hooks_.live_theme == nullptr ||
        keybindings_ == nullptr) {
        return;
    }
    auto self = shared_from_this();
    spawn(
        [self]() -> boost::asio::awaitable<void> {
            co_await self->run_trust_selector();
        },
        "Native TUI trust flow failed");
}

boost::asio::awaitable<std::optional<ProjectTrustOption>>
SessionFlowController::show_boot_trust_prompt(
    const std::filesystem::path& workspace) {
    if (!is_live() || hooks_.live_theme == nullptr || keybindings_ == nullptr) {
        co_return std::nullopt;
    }
    auto options = get_project_trust_options(
        workspace, /*include_session_only*/ true);
    const auto executor = co_await boost::asio::this_coro::executor;
    auto slot = std::make_shared<PromptSlot>(executor);
    track_prompt_slot(slot);
    std::vector<cch::tui::SelectItem> items;
    items.reserve(options.size());
    for (const auto& option : options) {
        items.push_back(cch::tui::SelectItem{.value = option.label, .label = option.label});
    }
    const auto& theme = hooks_.live_theme();
    const auto weak = weak_from_this();
    auto selector = std::make_shared<cch::tui::SelectList>(std::move(items),
            cch::tui::SelectListOptions{
                    .theme = theme.select_list_theme(),
                    .on_select = [weak, slot](const cch::tui::SelectItem& item) -> support::ExpectedVoid {
                        if (const auto self = weak.lock()) {
                            self->post([self, slot, label = item.value]() mutable {
                                self->presenter_->restore_prompt_slot();
                                slot->resolve(std::move(label));
                            });
                        }
                        return {};
                    },
                    .on_cancel = [weak, slot]() -> support::ExpectedVoid {
                        if (const auto self = weak.lock()) {
                            self->post([self, slot] {
                                self->presenter_->restore_prompt_slot();
                                slot->resolve(std::unexpected(prompt_cancelled_error()));
                            });
                        }
                        return {};
                    },
                    .keybindings = keybindings_->get(),
                    .title = format_project_trust_prompt(workspace),
                    .hint = generic_list_hint(*keybindings_->get()),
                    .border_hook = theme.foreground_hook(ThemeToken::Border),
            });
    presenter_->replace_prompt_slot(std::move(selector));

    boost::system::error_code error;
    auto selected = co_await slot->channel.async_receive(
        boost::asio::redirect_error(boost::asio::use_awaitable, error));
    untrack_prompt_slot(slot);
    if (closed_ || error || !selected) co_return std::nullopt;
    for (auto& option : options) {
        if (option.label == *selected) co_return option;
    }
    co_return std::nullopt;
}

boost::asio::awaitable<void> SessionFlowController::run_trust_selector() {
    auto* session = current_session();
    if (!is_live() || session == nullptr || !session->is_open() ||
        hooks_.live_theme == nullptr || keybindings_ == nullptr) {
        co_return;
    }
    const auto cwd = session->workspace();
    const auto* captured_session = session;
    auto options = get_project_trust_options(cwd, /*include_session_only*/ false);
    const auto executor = co_await boost::asio::this_coro::executor;
    auto slot = std::make_shared<PromptSlot>(executor);
    track_prompt_slot(slot);
    const auto captured_generation = action_generation();
    std::vector<cch::tui::SelectItem> items;
    items.reserve(options.size());
    for (const auto& option : options) {
        items.push_back(cch::tui::SelectItem{.value = option.label, .label = option.label});
    }
    const auto& theme = hooks_.live_theme();
    const auto weak = weak_from_this();
    auto selector = std::make_shared<cch::tui::SelectList>(std::move(items),
            cch::tui::SelectListOptions{
                    .theme = theme.select_list_theme(),
                    .on_select = [weak, slot](const cch::tui::SelectItem& item) -> support::ExpectedVoid {
                        if (const auto self = weak.lock()) {
                            self->post([self, slot, label = item.value]() mutable {
                                self->presenter_->restore_prompt_slot();
                                slot->resolve(std::move(label));
                            });
                        }
                        return {};
                    },
                    .on_cancel = [weak, slot]() -> support::ExpectedVoid {
                        if (const auto self = weak.lock()) {
                            self->post([self, slot] {
                                self->presenter_->restore_prompt_slot();
                                slot->resolve(std::unexpected(prompt_cancelled_error()));
                            });
                        }
                        return {};
                    },
                    .keybindings = keybindings_->get(),
                    .title = format_project_trust_prompt(cwd),
                    .hint = generic_list_hint(*keybindings_->get()),
                    .border_hook = theme.foreground_hook(ThemeToken::Border),
            });
    presenter_->replace_prompt_slot(std::move(selector));

    boost::system::error_code error;
    auto selected = co_await slot->channel.async_receive(
        boost::asio::redirect_error(boost::asio::use_awaitable, error));
    untrack_prompt_slot(slot);
    // A decision admitted before a Session replacement must not persist
    // trust against the replacement's workspace (ADR 0040).
    if (closed_ || error || captured_generation != action_generation() || current_session() != captured_session ||
            current_session()->workspace() != cwd) {
        co_return;
    }
    presenter_->restore_prompt_slot();
    if (!selected) co_return;
    for (auto& option : options) {
        if (option.label != *selected) continue;
        if (!option.updates.empty()) {
            ProjectTrustStore store{coding_agent::trust_store_file_path()};
            if (auto saved = store.setMany(option.updates); !saved) {
                presenter_->show_error(combined_error_text(saved.error()));
            }
        }
        presenter_->show_status(std::format("Saved trust decision: {}. Restart pike for this to take effect.",
                option.trusted ? "trusted" : "untrusted"));
        break;
    }
}

} // namespace cch::coding_agent::tui
