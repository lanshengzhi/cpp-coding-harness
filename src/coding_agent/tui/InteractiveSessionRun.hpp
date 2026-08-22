#pragma once

#include "coding_agent/tui/ClipboardReader.hpp"
#include "coding_agent/tui/InteractiveMode.hpp"
#include "coding_agent/runtime/AgentSessionCreationRequest.hpp"
#include "coding_agent/AgentSession.hpp"
#include "agent/harness/RuntimeRoot.hpp"
#include <cch/coding_agent/ModelRuntime.hpp>
#include <cch/ai/Models.hpp>
#include <cch/support/Error.hpp>

#include <atomic>
#include <filesystem>
#include <iosfwd>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cch::coding_agent::tui {

class InteractiveSessionRun;
class InteractiveSessionRunBuilder;

/// InteractiveSessionRun: The Native TUI's intake composition object (#517).
/// Carries CLI-owned facts, run-intent values (initial prompt with options,
/// model-fallback warning), and capability injections (clipboard reader).
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
    [[nodiscard]] const std::optional<std::string>& initial_prompt() const noexcept;
    [[nodiscard]] const PromptOptions& initial_prompt_options() const noexcept;
    [[nodiscard]] const std::optional<std::string>& model_fallback_message() const noexcept;
    [[nodiscard]] bool has_clipboard_reader() const noexcept;
    [[nodiscard]] const std::optional<runtime::AgentSessionCreationRequest>& boot_request() const noexcept;
    [[nodiscard]] bool creation_failure_reported() const noexcept;

    // ── Owned host effects (closed action seam dispatch) ────────────────

    void open_browser(std::string url) const;
    [[nodiscard]] bool write_clipboard_text(const std::string& text) const;
    void suspend_process() const;
    [[nodiscard]] support::Expected<coding_agent::CreateAgentSessionResult> replace_session(
        runtime::AgentSessionCreationRequest request) const;
    void report_boot_diagnostics(
        const std::vector<coding_agent::SessionDiagnostic>& diagnostics) const;
    void report_boot_creation_failure(const support::Error& error) const;

    [[nodiscard]] support::Expected<TuiActionResultVariant> dispatch_action(
        std::size_t action_generation,
        TuiActionVariant action) const;

    // ── Action sink and Stage 3 config conversion ────────────────────────

    [[nodiscard]] TuiActionSink make_action_sink() const;
    [[nodiscard]] InteractiveModeConfig to_config();

private:
    friend class InteractiveSessionRunBuilder;

    struct State {
        runtime::InteractiveSessionFacts session_facts{};
        std::filesystem::path agent_config_directory{};
        std::optional<std::string> initial_prompt{std::nullopt};
        PromptOptions initial_prompt_options{};
        std::optional<std::string> model_fallback_message{std::nullopt};
        std::unique_ptr<AsyncClipboardReader> clipboard_reader{nullptr};
        std::optional<runtime::AgentSessionCreationRequest> boot_request{std::nullopt};

        std::shared_ptr<harness::RuntimeRoot> runtime_root{nullptr};
        std::shared_ptr<coding_agent::ModelRuntime> shared_runtime{nullptr};
        std::shared_ptr<ai::Models> models{nullptr};
        std::ostream* error_stream{nullptr}; // borrowed error stream; must outlive run operations when supplied
        bool is_resume_target{false};
        std::atomic<bool> creation_failure_reported{false};
        TuiActionSink custom_action_sink{nullptr};
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
    InteractiveSessionRunBuilder& with_boot_request(
        std::optional<runtime::AgentSessionCreationRequest> request) noexcept;
    InteractiveSessionRunBuilder& with_runtime_root(
        std::shared_ptr<harness::RuntimeRoot> runtime_root) noexcept;
    InteractiveSessionRunBuilder& with_shared_runtime(
        std::shared_ptr<coding_agent::ModelRuntime> shared_runtime) noexcept;
    InteractiveSessionRunBuilder& with_models(
        std::shared_ptr<ai::Models> models) noexcept;
    InteractiveSessionRunBuilder& with_error_stream(
        std::ostream* error_stream) noexcept;
    InteractiveSessionRunBuilder& with_is_resume_target(
        bool is_resume_target) noexcept;
    InteractiveSessionRunBuilder& with_action_sink(
        TuiActionSink sink) noexcept;

    [[nodiscard]] InteractiveSessionRun build();

private:
    std::shared_ptr<InteractiveSessionRun::State> state_;
};

} // namespace cch::coding_agent::tui
