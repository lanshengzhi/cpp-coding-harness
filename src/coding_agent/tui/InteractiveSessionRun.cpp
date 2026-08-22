#include "InteractiveSessionRun.hpp"

#include "coding_agent/runtime/SessionFactory.hpp"
#include "coding_agent/tui/ClipboardWrite.hpp"
#include "coding_agent/tui/OpenBrowser.hpp"

#include <csignal>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace cch::coding_agent::tui {

InteractiveSessionRun::InteractiveSessionRun(std::shared_ptr<State> state)
    : state_(std::move(state)) {}

InteractiveSessionRun::InteractiveSessionRun(InteractiveSessionRun&&) noexcept = default;
InteractiveSessionRun& InteractiveSessionRun::operator=(InteractiveSessionRun&&) noexcept = default;
InteractiveSessionRun::~InteractiveSessionRun() = default;

const runtime::InteractiveSessionFacts& InteractiveSessionRun::session_facts() const noexcept {
    static const runtime::InteractiveSessionFacts kEmptyFacts{};
    return state_ ? state_->session_facts : kEmptyFacts;
}

const std::filesystem::path& InteractiveSessionRun::agent_config_directory() const noexcept {
    static const std::filesystem::path kEmptyPath{};
    return state_ ? state_->agent_config_directory : kEmptyPath;
}

const std::optional<std::string>& InteractiveSessionRun::initial_prompt() const noexcept {
    static const std::optional<std::string> kEmptyPrompt{std::nullopt};
    return state_ ? state_->initial_prompt : kEmptyPrompt;
}

const PromptOptions& InteractiveSessionRun::initial_prompt_options() const noexcept {
    static const PromptOptions kEmptyOptions{};
    return state_ ? state_->initial_prompt_options : kEmptyOptions;
}

const std::optional<std::string>& InteractiveSessionRun::model_fallback_message() const noexcept {
    static const std::optional<std::string> kEmptyFallback{std::nullopt};
    return state_ ? state_->model_fallback_message : kEmptyFallback;
}

bool InteractiveSessionRun::has_clipboard_reader() const noexcept {
    return state_ && state_->clipboard_reader != nullptr;
}

const std::optional<runtime::AgentSessionCreationRequest>& InteractiveSessionRun::boot_request() const noexcept {
    static const std::optional<runtime::AgentSessionCreationRequest> kEmptyRequest{std::nullopt};
    return state_ ? state_->boot_request : kEmptyRequest;
}

bool InteractiveSessionRun::creation_failure_reported() const noexcept {
    return state_ && state_->creation_failure_reported.load(std::memory_order_relaxed);
}

void InteractiveSessionRun::open_browser(std::string url) const {
    coding_agent::tui::open_browser(std::move(url));
}

bool InteractiveSessionRun::write_clipboard_text(const std::string& text) const {
    return coding_agent::tui::write_clipboard_text(text);
}

void InteractiveSessionRun::suspend_process() const {
    (void)::kill(0, SIGTSTP);
}

support::Expected<coding_agent::CreateAgentSessionResult> InteractiveSessionRun::replace_session(
    runtime::AgentSessionCreationRequest request) const {
    if (!state_) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Unknown,
            "InteractiveSessionRun is not initialized"));
    }
    request.provide_user_shell = true;
    if (state_->runtime_root) {
        request.execution_runtime_target = state_->runtime_root->make_target();
    }
    if (state_->shared_runtime) {
        request.model_runtime = state_->shared_runtime;
    }
    return coding_agent::create_agent_session(
        std::move(request),
        state_->session_facts,
        coding_agent::runtime::AssemblyOverrides{
            .models = state_->models,
            .user_shell = nullptr,
        });
}

void InteractiveSessionRun::report_boot_diagnostics(
    const std::vector<coding_agent::SessionDiagnostic>& diagnostics) const {
    if (!state_ || !state_->error_stream) return;
    for (const auto& diag : diagnostics) {
        const char* severity = "info";
        switch (diag.severity) {
        case coding_agent::SessionDiagnostic::Severity::Info:
            severity = "info";
            break;
        case coding_agent::SessionDiagnostic::Severity::Warning:
            severity = "warn";
            break;
        case coding_agent::SessionDiagnostic::Severity::Error:
            severity = "error";
            break;
        }
        std::string category = "session";
        std::string code = diag.code;
        if (const auto split = code.find(':'); split != std::string::npos) {
            category = code.substr(0, split);
            code = code.substr(split + 1);
        }
        *state_->error_stream << '[' << category << ':' << severity << "] " << code << ": " << diag.message;
        if (diag.path) {
            *state_->error_stream << " (" << *diag.path << ')';
        }
        *state_->error_stream << '\n';
    }
}

void InteractiveSessionRun::report_boot_creation_failure(
    const support::Error& error) const {
    if (!state_) return;
    state_->creation_failure_reported.store(true, std::memory_order_relaxed);
    if (!state_->error_stream) return;
    *state_->error_stream << (state_->is_resume_target
                                 ? "could not resume session: "
                                 : "could not create session: ")
                         << error.message;
    if (!error.detail.empty() && error.detail != error.message) {
        *state_->error_stream << ": " << error.detail;
    }
    *state_->error_stream << '\n';
    if (error.context && !error.context->empty()) {
        *state_->error_stream << "note: " << *error.context << '\n';
    }
}

support::Expected<TuiActionResultVariant> InteractiveSessionRun::dispatch_action(
    std::size_t /* action_generation */,
    TuiActionVariant action) const {
    return std::visit(
        [this](auto&& payload) -> support::Expected<TuiActionResultVariant> {
            using T = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<T, OpenBrowserAction>) {
                open_browser(std::move(payload.url));
                return TuiActionResultVariant{std::monostate{}};
            } else if constexpr (std::is_same_v<T, WriteClipboardAction>) {
                return TuiActionResultVariant{write_clipboard_text(payload.text)};
            } else if constexpr (std::is_same_v<T, SuspendProcessAction>) {
                suspend_process();
                return TuiActionResultVariant{std::monostate{}};
            } else if constexpr (std::is_same_v<T, ReplaceSessionAction>) {
                auto created = replace_session(std::move(payload.request));
                return TuiActionResultVariant{std::move(created)};
            } else if constexpr (std::is_same_v<T, ReportBootDiagnosticsAction>) {
                report_boot_diagnostics(payload.diagnostics);
                return TuiActionResultVariant{std::monostate{}};
            } else if constexpr (std::is_same_v<T, ReportBootCreationFailureAction>) {
                report_boot_creation_failure(payload.error);
                return TuiActionResultVariant{std::monostate{}};
            } else {
                return TuiActionResultVariant{std::monostate{}};
            }
        },
        std::move(action));
}

TuiActionSink InteractiveSessionRun::make_action_sink() const {
    if (!state_) return nullptr;
    if (state_->custom_action_sink) {
        return std::move(state_->custom_action_sink);
    }
    const auto state = state_;
    return [state](std::size_t action_generation, TuiActionVariant action)
        -> support::Expected<TuiActionResultVariant> {
        InteractiveSessionRun run(state);
        return run.dispatch_action(action_generation, std::move(action));
    };
}

InteractiveModeConfig InteractiveSessionRun::to_config() {
    if (!state_) return InteractiveModeConfig{};
    TuiActionSink sink = make_action_sink();
    return InteractiveModeConfig{
        .agent_config_directory = state_->agent_config_directory,
        .clipboard_reader = std::move(state_->clipboard_reader),
        .initial_prompt = state_->initial_prompt,
        .initial_prompt_options = state_->initial_prompt_options,
        .model_fallback_message = state_->model_fallback_message,
        .action_sink = std::move(sink),
        .session_facts = state_->session_facts,
        .boot_request = std::move(state_->boot_request),
    };
}

InteractiveSessionRunBuilder::InteractiveSessionRunBuilder()
    : state_(std::make_shared<InteractiveSessionRun::State>()) {}

InteractiveSessionRunBuilder::~InteractiveSessionRunBuilder() = default;
InteractiveSessionRunBuilder::InteractiveSessionRunBuilder(InteractiveSessionRunBuilder&&) noexcept = default;
InteractiveSessionRunBuilder& InteractiveSessionRunBuilder::operator=(InteractiveSessionRunBuilder&&) noexcept = default;

InteractiveSessionRunBuilder& InteractiveSessionRunBuilder::with_session_facts(
    runtime::InteractiveSessionFacts facts) noexcept {
    state_->session_facts = std::move(facts);
    return *this;
}

InteractiveSessionRunBuilder& InteractiveSessionRunBuilder::with_agent_config_directory(
    std::filesystem::path dir) noexcept {
    state_->agent_config_directory = std::move(dir);
    return *this;
}

InteractiveSessionRunBuilder& InteractiveSessionRunBuilder::with_initial_prompt(
    std::optional<std::string> prompt) noexcept {
    state_->initial_prompt = std::move(prompt);
    return *this;
}

InteractiveSessionRunBuilder& InteractiveSessionRunBuilder::with_initial_prompt_options(
    PromptOptions options) noexcept {
    state_->initial_prompt_options = std::move(options);
    return *this;
}

InteractiveSessionRunBuilder& InteractiveSessionRunBuilder::with_model_fallback_message(
    std::optional<std::string> message) noexcept {
    state_->model_fallback_message = std::move(message);
    return *this;
}

InteractiveSessionRunBuilder& InteractiveSessionRunBuilder::with_clipboard_reader(
    std::unique_ptr<AsyncClipboardReader> reader) noexcept {
    state_->clipboard_reader = std::move(reader);
    return *this;
}

InteractiveSessionRunBuilder& InteractiveSessionRunBuilder::with_boot_request(
    std::optional<runtime::AgentSessionCreationRequest> request) noexcept {
    if (request) {
        state_->is_resume_target = std::holds_alternative<coding_agent::ExplicitResumeSessionTarget>(
            request->session_target);
    }
    state_->boot_request = std::move(request);
    return *this;
}

InteractiveSessionRunBuilder& InteractiveSessionRunBuilder::with_runtime_root(
    std::shared_ptr<harness::RuntimeRoot> runtime_root) noexcept {
    state_->runtime_root = std::move(runtime_root);
    return *this;
}

InteractiveSessionRunBuilder& InteractiveSessionRunBuilder::with_shared_runtime(
    std::shared_ptr<coding_agent::ModelRuntime> shared_runtime) noexcept {
    state_->shared_runtime = std::move(shared_runtime);
    return *this;
}

InteractiveSessionRunBuilder& InteractiveSessionRunBuilder::with_models(
    std::shared_ptr<ai::Models> models) noexcept {
    state_->models = std::move(models);
    return *this;
}

InteractiveSessionRunBuilder& InteractiveSessionRunBuilder::with_error_stream(
    std::ostream* error_stream) noexcept {
    state_->error_stream = error_stream;
    return *this;
}

InteractiveSessionRunBuilder& InteractiveSessionRunBuilder::with_is_resume_target(
    bool is_resume_target) noexcept {
    state_->is_resume_target = is_resume_target;
    return *this;
}

InteractiveSessionRunBuilder& InteractiveSessionRunBuilder::with_action_sink(
    TuiActionSink sink) noexcept {
    state_->custom_action_sink = std::move(sink);
    return *this;
}

InteractiveSessionRun InteractiveSessionRunBuilder::build() {
    return InteractiveSessionRun(std::move(state_));
}

} // namespace cch::coding_agent::tui
