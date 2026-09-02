#pragma once

#include "coding_agent/tui/ClipboardReader.hpp"
#include "coding_agent/tui/InteractiveMode.hpp"
#include "coding_agent/runtime/AgentSessionCreationRequest.hpp"
#include "coding_agent/AgentSession.hpp"
#include "agent/harness/RuntimeRoot.hpp"
#include <cch/ai/Models.hpp>
#include <cch/coding_agent/ModelRuntime.hpp>
#include <cch/coding_agent/ProjectResources.hpp>
#include <cch/support/AsyncResult.hpp>
#include <cch/support/Error.hpp>

#include <atomic>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace cch::coding_agent::tui {

class InteractiveSessionRun;
class InteractiveSessionRunBuilder;

using AsyncSessionReplacementSink =
        std::move_only_function<support::AsyncResult<coding_agent::CreateAgentSessionResult>(
                std::size_t, runtime::AgentSessionCreationRequest, std::stop_token)>;

/// Build the one composition-owned filesystem capability collection used by
/// Native TUI trust detection. Every capability shares one Runtime target;
/// the collection includes the workspace, its known ancestor roots, and the
/// Agent Config Directory and user `.agents` roots when supplied. It never
/// derives capabilities from a path discovered by resource loading.
[[nodiscard]] ProjectResourceFileSystems make_authorized_project_resource_filesystems(
        std::shared_ptr<harness::RuntimeRoot> runtime_root,
        std::filesystem::path workspace,
        std::filesystem::path agent_config_directory,
        std::filesystem::path home_directory);

/// Intent for the Interactive Session Run: either bind a pre-created Agent
/// Session, or defer session creation until after the boot trust prompt
/// resolves in the TUI overlay.
struct BindExistingSession {
    AgentSession* session{nullptr}; // borrowed; must outlive the interactive run
};

struct DeferBoot {
    runtime::AgentSessionCreationRequest request{};
};

using SessionIntentVariant = std::variant<BindExistingSession, DeferBoot>;

/// InteractiveSessionRun: The Native TUI's intake composition object (#517).
/// Carries CLI-owned facts, session intent (BindExistingSession vs DeferBoot),
/// run-intent values (initial prompt with options, model-fallback warning), and
/// capability injections (clipboard reader).
/// Owns the host effects for the closed application action seam (browser,
/// clipboard, process suspend, session replacement, boot diagnostics/failure
/// reporting) so host effects live beside the values they capture.
class InteractiveSessionRun final {
public:
    InteractiveSessionRun(InteractiveSessionRun&&) noexcept;
    InteractiveSessionRun& operator=(InteractiveSessionRun&&) noexcept;
    ~InteractiveSessionRun();
    InteractiveSessionRun(const InteractiveSessionRun&) = delete;
    InteractiveSessionRun& operator=(const InteractiveSessionRun&) = delete;

    // ── Carried facts, intent, and capabilities ──────────────────────────

    [[nodiscard]] const runtime::InteractiveSessionFacts& session_facts() const noexcept;
    [[nodiscard]] const std::filesystem::path& agent_config_directory() const noexcept;
    [[nodiscard]] std::shared_ptr<harness::RuntimeRoot> runtime_root() const noexcept;
    [[nodiscard]] ProjectResourceFileSystems take_project_resource_filesystems() noexcept;
    [[nodiscard]] const std::optional<std::string>& initial_prompt() const noexcept;
    [[nodiscard]] const PromptOptions& initial_prompt_options() const noexcept;
    [[nodiscard]] const std::optional<std::string>& model_fallback_message() const noexcept;
    [[nodiscard]] bool has_clipboard_reader() const noexcept;
    [[nodiscard]] std::unique_ptr<AsyncClipboardReader> take_clipboard_reader() noexcept;
    [[nodiscard]] const SessionIntentVariant& session_intent() const noexcept;
    [[nodiscard]] SessionIntentVariant take_session_intent() noexcept;
    [[nodiscard]] bool creation_failure_reported() const noexcept;

    // ── Owned host effects (closed action seam dispatch) ────────────────

    void open_browser(std::string url) const;
    [[nodiscard]] bool write_clipboard_text(const std::string& text) const;
    void suspend_process() const;
    [[nodiscard]] support::Expected<coding_agent::CreateAgentSessionResult> replace_session(
        runtime::AgentSessionCreationRequest request) const;
    [[nodiscard]] AsyncSessionReplacementSink make_async_session_replacement_sink() const;
    void report_boot_diagnostics(
        const std::vector<coding_agent::SessionDiagnostic>& diagnostics) const;
    void report_boot_creation_failure(const support::Error& error) const;

    [[nodiscard]] support::Expected<TuiActionResultVariant> dispatch_action(
        std::size_t action_generation,
        TuiActionVariant action) const;

    // ── Action sink ──────────────────────────────────────────────────────

    [[nodiscard]] TuiActionSink make_action_sink() const;

private:
    friend class InteractiveSessionRunBuilder;

    struct State {
        runtime::InteractiveSessionFacts session_facts{};
        std::filesystem::path agent_config_directory{};
        std::optional<std::string> initial_prompt{std::nullopt};
        PromptOptions initial_prompt_options{};
        std::optional<std::string> model_fallback_message{std::nullopt};
        std::unique_ptr<AsyncClipboardReader> clipboard_reader{nullptr};
        SessionIntentVariant session_intent{BindExistingSession{nullptr}};

        std::shared_ptr<harness::RuntimeRoot> runtime_root{nullptr};
        ProjectResourceFileSystems project_resource_filesystems{};
        std::shared_ptr<coding_agent::ModelRuntime> shared_runtime{nullptr};
        bool model_runtime_cli_fake{false};
        std::shared_ptr<ai::Models> models{nullptr};
        std::ostream* error_stream{nullptr}; // borrowed error stream; must outlive run operations when supplied
        bool is_resume_target{false};
        std::atomic<bool> creation_failure_reported{false};
        std::optional<AsyncSessionReplacementSink> custom_async_session_replacement_sink{std::nullopt};
        std::optional<TuiActionSink> custom_action_sink{std::nullopt};
    };

    explicit InteractiveSessionRun(std::shared_ptr<State> state);

    std::shared_ptr<State> state_;
};

/// Builder for InteractiveSessionRun: the single place interactive-run assembly happens.
class InteractiveSessionRunBuilder final {
public:
    InteractiveSessionRunBuilder();
    InteractiveSessionRunBuilder(InteractiveSessionRunBuilder&&) noexcept;
    InteractiveSessionRunBuilder& operator=(InteractiveSessionRunBuilder&&) noexcept;
    ~InteractiveSessionRunBuilder();
    InteractiveSessionRunBuilder(const InteractiveSessionRunBuilder&) = delete;
    InteractiveSessionRunBuilder& operator=(const InteractiveSessionRunBuilder&) = delete;

    InteractiveSessionRunBuilder& with_session_intent(
        SessionIntentVariant intent) noexcept;
    InteractiveSessionRunBuilder& with_session(
        AgentSession& session) noexcept;
    InteractiveSessionRunBuilder& with_session(
        AgentSession* session) noexcept;
    InteractiveSessionRunBuilder& with_defer_boot(
        runtime::AgentSessionCreationRequest request) noexcept;
    InteractiveSessionRunBuilder& with_session_facts(
        runtime::InteractiveSessionFacts facts) noexcept;
    InteractiveSessionRunBuilder& with_agent_config_directory(
        std::filesystem::path dir) noexcept;
    InteractiveSessionRunBuilder& with_initial_prompt(
        std::optional<std::string> prompt) noexcept;
    InteractiveSessionRunBuilder& with_initial_prompt_options(
        PromptOptions options) noexcept;
    InteractiveSessionRunBuilder& with_model_fallback_message(
        std::optional<std::string> message) noexcept;
    InteractiveSessionRunBuilder& with_clipboard_reader(
        std::unique_ptr<AsyncClipboardReader> reader) noexcept;
    InteractiveSessionRunBuilder& with_runtime_root(
        std::shared_ptr<harness::RuntimeRoot> runtime_root) noexcept;
    InteractiveSessionRunBuilder& with_project_resource_filesystems(ProjectResourceFileSystems filesystems) noexcept;
    InteractiveSessionRunBuilder& with_shared_runtime(
        std::shared_ptr<coding_agent::ModelRuntime> shared_runtime) noexcept;
    InteractiveSessionRunBuilder& with_model_runtime_cli_fake(bool model_runtime_cli_fake) noexcept;
    InteractiveSessionRunBuilder& with_models(
        std::shared_ptr<ai::Models> models) noexcept;
    InteractiveSessionRunBuilder& with_error_stream(
        std::ostream* error_stream) noexcept;
    InteractiveSessionRunBuilder& with_is_resume_target(
        bool is_resume_target) noexcept;
    InteractiveSessionRunBuilder& with_action_sink(
        TuiActionSink sink) noexcept;
    InteractiveSessionRunBuilder& with_async_session_replacement_sink(AsyncSessionReplacementSink sink) noexcept;

    [[nodiscard]] InteractiveSessionRun build();

private:
    std::shared_ptr<InteractiveSessionRun::State> state_;
};

} // namespace cch::coding_agent::tui
