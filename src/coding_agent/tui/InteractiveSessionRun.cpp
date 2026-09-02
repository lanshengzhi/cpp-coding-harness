#include "InteractiveSessionRun.hpp"

#include "coding_agent/runtime/SessionFactory.hpp"
#include "support/AsyncResultBridge.hpp"
#include "coding_agent/tui/ClipboardWrite.hpp"
#include "coding_agent/tui/OpenBrowser.hpp"

#include <cch/coding_agent/AgentConfigDir.hpp>

#include <csignal>
#include <ostream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>

namespace cch::coding_agent::tui {
namespace {

[[nodiscard]] std::filesystem::path intent_workspace(const SessionIntentVariant& intent) {
    return std::visit(
            [](const auto& value) -> std::filesystem::path {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, BindExistingSession>) {
                    return value.session != nullptr ? value.session->workspace() : std::filesystem::path{};
                } else {
                    return value.request.workspace;
                }
            },
            intent);
}

} // namespace

ProjectResourceFileSystems make_authorized_project_resource_filesystems(
        std::shared_ptr<harness::RuntimeRoot> runtime_root,
        std::filesystem::path workspace,
        std::filesystem::path agent_config_directory,
        std::filesystem::path home_directory) {
    return runtime::SessionFactory::make_authorized_project_resource_filesystems(std::move(runtime_root),
            std::move(workspace),
            std::move(agent_config_directory),
            std::move(home_directory));
}

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

std::shared_ptr<harness::RuntimeRoot> InteractiveSessionRun::runtime_root() const noexcept {
    return state_ ? state_->runtime_root : nullptr;
}

ProjectResourceFileSystems InteractiveSessionRun::take_project_resource_filesystems() noexcept {
    if (!state_) {
        return {};
    }
    return std::exchange(state_->project_resource_filesystems, {});
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

std::unique_ptr<AsyncClipboardReader> InteractiveSessionRun::take_clipboard_reader() noexcept {
    if (!state_) return nullptr;
    return std::move(state_->clipboard_reader);
}

const SessionIntentVariant& InteractiveSessionRun::session_intent() const noexcept {
    static const SessionIntentVariant kEmptyIntent{BindExistingSession{nullptr}};
    return state_ ? state_->session_intent : kEmptyIntent;
}

SessionIntentVariant InteractiveSessionRun::take_session_intent() noexcept {
    if (!state_) return BindExistingSession{nullptr};
    return std::exchange(state_->session_intent, BindExistingSession{nullptr});
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

AsyncSessionReplacementSink InteractiveSessionRun::make_async_session_replacement_sink() const {
    if (!state_) return nullptr;
    if (state_->custom_async_session_replacement_sink.has_value()) {
        return std::move(state_->custom_async_session_replacement_sink.value());
    }
    const auto state = state_;
    return [state](std::size_t /* action_generation */,
                   runtime::AgentSessionCreationRequest request,
                   std::stop_token stop_token) -> support::AsyncResult<coding_agent::CreateAgentSessionResult> {
        request.provide_user_shell = true;
        if (state->runtime_root) {
            request.execution_runtime_target = state->runtime_root->make_target();
        }
        return coding_agent::create_agent_session_async(std::move(request),
                state->session_facts,
                coding_agent::runtime::AssemblyOverrides{
                        .model_runtime = state->shared_runtime,
                        .cli_fake = state->model_runtime_cli_fake,
                        .models = state->models,
                        .user_shell = nullptr,
                },
                stop_token);
    };
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
    if (state_->custom_action_sink.has_value()) {
        return std::move(state_->custom_action_sink.value());
    }
    const auto state = state_;
    return [state](std::size_t action_generation, TuiActionVariant action)
        -> support::Expected<TuiActionResultVariant> {
        InteractiveSessionRun run(state);
        return run.dispatch_action(action_generation, std::move(action));
    };
}

InteractiveSessionRunBuilder::InteractiveSessionRunBuilder()
    : state_(std::make_shared<InteractiveSessionRun::State>()) {}

InteractiveSessionRunBuilder::~InteractiveSessionRunBuilder() = default;
InteractiveSessionRunBuilder::InteractiveSessionRunBuilder(InteractiveSessionRunBuilder&&) noexcept = default;
InteractiveSessionRunBuilder& InteractiveSessionRunBuilder::operator=(InteractiveSessionRunBuilder&&) noexcept = default;

InteractiveSessionRunBuilder& InteractiveSessionRunBuilder::with_session_intent(
    SessionIntentVariant intent) noexcept {
    if (auto* defer = std::get_if<DeferBoot>(&intent)) {
        state_->is_resume_target = std::holds_alternative<coding_agent::ExplicitResumeSessionTarget>(
            defer->request.session_target);
    } else {
        state_->is_resume_target = false;
    }
    state_->session_intent = std::move(intent);
    return *this;
}

InteractiveSessionRunBuilder& InteractiveSessionRunBuilder::with_session(
    AgentSession& session) noexcept {
    return with_session(&session);
}

InteractiveSessionRunBuilder& InteractiveSessionRunBuilder::with_session(
    AgentSession* session) noexcept {
    state_->is_resume_target = false;
    state_->session_intent = BindExistingSession{.session = session};
    return *this;
}

InteractiveSessionRunBuilder& InteractiveSessionRunBuilder::with_defer_boot(
    runtime::AgentSessionCreationRequest request) noexcept {
    state_->is_resume_target = std::holds_alternative<coding_agent::ExplicitResumeSessionTarget>(
        request.session_target);
    state_->session_intent = DeferBoot{.request = std::move(request)};
    return *this;
}

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

InteractiveSessionRunBuilder& InteractiveSessionRunBuilder::with_runtime_root(
    std::shared_ptr<harness::RuntimeRoot> runtime_root) noexcept {
    state_->runtime_root = std::move(runtime_root);
    return *this;
}

InteractiveSessionRunBuilder& InteractiveSessionRunBuilder::with_project_resource_filesystems(
        ProjectResourceFileSystems filesystems) noexcept {
    state_->project_resource_filesystems = std::move(filesystems);
    return *this;
}

InteractiveSessionRunBuilder& InteractiveSessionRunBuilder::with_shared_runtime(
    std::shared_ptr<coding_agent::ModelRuntime> shared_runtime) noexcept {
    state_->shared_runtime = std::move(shared_runtime);
    return *this;
}

InteractiveSessionRunBuilder& InteractiveSessionRunBuilder::with_model_runtime_cli_fake(
        bool model_runtime_cli_fake) noexcept {
    state_->model_runtime_cli_fake = model_runtime_cli_fake;
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

InteractiveSessionRunBuilder& InteractiveSessionRunBuilder::with_async_session_replacement_sink(
        AsyncSessionReplacementSink sink) noexcept {
    state_->custom_async_session_replacement_sink = std::move(sink);
    return *this;
}

InteractiveSessionRun InteractiveSessionRunBuilder::build() {
    if (!state_->project_resource_filesystems.workspace && state_->runtime_root) {
        const auto workspace = intent_workspace(state_->session_intent);
        if (!workspace.empty()) {
            state_->project_resource_filesystems = make_authorized_project_resource_filesystems(
                    state_->runtime_root, workspace, state_->agent_config_directory, coding_agent::home_directory());
        }
    }
    return InteractiveSessionRun(std::move(state_));
}

} // namespace cch::coding_agent::tui
