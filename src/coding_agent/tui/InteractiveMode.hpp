#pragma once

#include "coding_agent/tui/ClipboardReader.hpp"

#include "coding_agent/AgentSession.hpp"
#include "coding_agent/runtime/AgentSessionCreationRequest.hpp"
#include <cch/tui/Keybindings.hpp>
#include <cch/util/Error.hpp>

#include <boost/asio/awaitable.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace cch::coding_agent {
class AgentSession;
} // namespace cch::coding_agent

namespace cch::tui {
class Terminal;
} // namespace cch::tui

namespace cch::coding_agent::tui {

/// CLI-owned facts reused for in-session session replacement requests (pi's
/// `createRuntime` closure captures the CLI model selection and resource
/// flags; the workspace and session target change per flow).
struct InteractiveSessionFacts {
    std::optional<bool> project_trust_override;
    bool no_skills{false};
    bool no_prompt_templates{false};
    std::vector<std::string> prompt_template_paths;
    std::optional<std::string> provider;
    std::optional<std::string> model;
    std::vector<std::string> models;
    std::optional<std::string> api_key;
};

/// In-session session replacement (pi `AgentSessionRuntime` createRuntime
/// closure): creates the next Agent Session for `switchSession`/
/// `newSession`/`fork`. The interactive host supplies it; focused tests
/// inject a deterministic factory.
using SessionFactorySink = std::move_only_function<
    util::Expected<CreateAgentSessionResult>(runtime::AgentSessionCreationRequest)>;

struct InteractiveModeConfig {
    std::filesystem::path agent_config_directory;
    cch::tui::KeybindingPlatform platform{cch::tui::native_keybinding_platform()};
    std::unique_ptr<AsyncClipboardReader> clipboard_reader{nullptr};
    std::optional<std::string> initial_prompt{std::nullopt};
    PromptOptions initial_prompt_options{};
    /// pi `modelFallbackMessage` (sdk.ts `createAgentSession`): shown as a
    /// `Warning: <message>` boot line in the chat container (pi
    /// `interactive-mode.ts` `showWarning`). Absent in print mode.
    std::optional<std::string> model_fallback_message{std::nullopt};
    /// Browser opening for the login dialog's auth-URL view (pi
    /// `openBrowser`: detached argv spawn, never through a shell, best-effort).
    /// Null installs the real platform spawn; tests inject a recorder.
    std::move_only_function<void(std::string)> open_browser_sink{nullptr};
    /// Clipboard writing for the tree's `app.message.copy` and the main
    /// editor's copy action (pi `copyToClipboard` platform-tools path). Null
    /// installs the real platform tools; tests inject a recorder.
    std::move_only_function<bool(std::string)> clipboard_write_sink{nullptr};
    /// The suspend process action (pi `handleCtrlZ` `process.kill(0,
    /// "SIGTSTP")`). Null sends SIGTSTP to the process group; tests inject a
    /// recorder so the test process is never stopped.
    std::move_only_function<void()> suspend_process_sink{nullptr};
    /// In-session session replacement factory (pi `AgentSessionRuntime`
    /// `createRuntime`). Null installs no replacement: the session flows
    /// report an error. The interactive host always supplies it.
    SessionFactorySink session_factory{nullptr};
    /// CLI-owned facts reused for in-session session replacement requests.
    InteractiveSessionFacts session_facts{};
};

/// Run the private Native TUI composition until its exit binding is received.
/// The borrowed Agent Session and Terminal must outlive the returned coroutine.
[[nodiscard]] boost::asio::awaitable<util::ExpectedVoid> run_interactive_mode(
    AgentSession& session,
    cch::tui::Terminal& terminal,
    InteractiveModeConfig config = {});

} // namespace cch::coding_agent::tui
