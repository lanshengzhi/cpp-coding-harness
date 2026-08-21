#include "SessionFlowController.hpp"

#include "agent/harness/WorkspaceFileSystem.hpp"
#include "coding_agent/runtime/AgentSessionInteractiveAccess.hpp"
#include "coding_agent/tui/ErrorPresentation.hpp"
#include "coding_agent/tui/PromptSlot.hpp"
#include "coding_agent/tui/SharedKeybindings.hpp"
#include "coding_agent/tui/StringListSelector.hpp"

#include <cch/coding_agent/AgentConfigDir.hpp>
#include <cch/coding_agent/ProjectResources.hpp>
#include <cch/coding_agent/ProjectTrust.hpp>
#include <cch/support/Error.hpp>

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

} // namespace

void SessionFlowController::arm_auto_trust_on_reload(
    std::filesystem::path workspace,
    std::optional<bool> trust_override) {
    // Preserved bounded synchronous filesystem reads (ADR 0040 §6.1: bounded
    // in-memory value work remains synchronous; these walked the workspace
    // synchronously in the pre-extraction host and the extraction introduces
    // no new blocking work or host seam requirement).
    auto_trust_on_reload_cwd_.reset();
    if (trust_override.has_value()) return;
    auto fs = harness::WorkspaceFileSystem::create(workspace);
    if (!fs) return;
    auto detection = detect_project_resources(
        *fs, coding_agent::home_directory() / ".agents" / "skills");
    if (!needs_project_trust_resolution(detection)) {
        auto_trust_on_reload_cwd_ = std::move(workspace);
    }
}

boost::asio::awaitable<bool> SessionFlowController::resolve_boot_trust(
    std::filesystem::path workspace,
    std::optional<bool> trust_override) {
    // pi `resolveProjectTrusted`: an override decides first, before any
    // resource detection or store walk.
    if (trust_override.has_value()) co_return *trust_override;

    auto fs = harness::WorkspaceFileSystem::create(workspace);
    ProjectResourceDetectionResult detection;
    if (fs) {
        detection = detect_project_resources(
            *fs, coding_agent::home_directory() / ".agents" / "skills");
    }
    const bool trust_needed = needs_project_trust_resolution(detection);
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

bool SessionFlowController::maybe_save_implicit_project_trust_after_reload() {
    // Preserved bounded synchronous trust-store/resource reads (ADR 0040
    // §6.1); see arm_auto_trust_on_reload().
    // The armed workspace is consumed by exactly one decision attempt.
    const auto arm = std::exchange(auto_trust_on_reload_cwd_, std::nullopt);
    auto* session = current_session();
    if (!arm || session == nullptr) return false;
    const auto& cwd = session->workspace();
    if (*arm != cwd ||
        !detail::AgentSessionInteractiveAccess::is_project_trusted(*session)) {
        return false;
    }
    auto fs = harness::WorkspaceFileSystem::create(cwd);
    if (!fs) return false;
    auto detection = detect_project_resources(
        *fs, coding_agent::home_directory() / ".agents" / "skills");
    if (!needs_project_trust_resolution(detection)) return false;

    ProjectTrustStore store{coding_agent::trust_store_file_path()};
    auto entry = store.getEntry(cwd);
    if (!entry) {
        if (hooks_.show_warning != nullptr) {
            hooks_.show_warning(
                "Could not save project trust after reload: " + entry.error().message);
        }
        presenter_->invalidate();
        return false;
    }
    if (entry->has_value()) return false;
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
        return false;
    }
    return true;
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
    std::vector<std::string> labels;
    labels.reserve(options.size());
    for (const auto& option : options) labels.push_back(option.label);
    const auto weak = weak_from_this();
    auto selector = std::make_shared<StringListSelector>(
        hooks_.live_theme(),
        keybindings_->get(),
        format_project_trust_prompt(workspace),
        std::move(labels),
        [weak, slot](std::string label) {
            if (const auto self = weak.lock()) {
                self->post(
                    [self, slot, label = std::move(label)]() mutable {
                        self->presenter_->restore_prompt_slot();
                        slot->resolve(std::move(label));
                    });
            }
        },
        [weak, slot] {
            if (const auto self = weak.lock()) {
                self->post([self, slot] {
                    self->presenter_->restore_prompt_slot();
                    slot->resolve(std::unexpected(prompt_cancelled_error()));
                });
            }
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
    auto options = get_project_trust_options(cwd, /*include_session_only*/ false);
    const auto executor = co_await boost::asio::this_coro::executor;
    auto slot = std::make_shared<PromptSlot>(executor);
    track_prompt_slot(slot);
    const auto captured_generation = action_generation();
    std::vector<std::string> labels;
    labels.reserve(options.size());
    for (const auto& option : options) labels.push_back(option.label);
    const auto weak = weak_from_this();
    auto selector = std::make_shared<StringListSelector>(
        hooks_.live_theme(),
        keybindings_->get(),
        format_project_trust_prompt(cwd),
        std::move(labels),
        [weak, slot](std::string label) {
            if (const auto self = weak.lock()) {
                self->post(
                    [self, slot, label = std::move(label)]() mutable {
                        self->presenter_->restore_prompt_slot();
                        slot->resolve(std::move(label));
                    });
            }
        },
        [weak, slot] {
            if (const auto self = weak.lock()) {
                self->post([self, slot] {
                    self->presenter_->restore_prompt_slot();
                    slot->resolve(std::unexpected(prompt_cancelled_error()));
                });
            }
        });
    presenter_->replace_prompt_slot(std::move(selector));

    boost::system::error_code error;
    auto selected = co_await slot->channel.async_receive(
        boost::asio::redirect_error(boost::asio::use_awaitable, error));
    untrack_prompt_slot(slot);
    // A decision admitted before a Session replacement must not persist
    // trust against the replacement's workspace (ADR 0040).
    if (closed_ || error || captured_generation != action_generation()) co_return;
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
        presenter_->show_status(std::format(
            "Saved trust decision: {}. Restart cch for this to take effect.",
            option.trusted ? "trusted" : "untrusted"));
        break;
    }
}

} // namespace cch::coding_agent::tui
