#pragma once

#include "coding_agent/tui/ClipboardReader.hpp"

#include "coding_agent/AgentSession.hpp"
#include "coding_agent/runtime/AgentSessionCreationRequest.hpp"
#include <cch/tui/Keybindings.hpp>
#include <cch/support/Error.hpp>

#include <boost/asio/awaitable.hpp>

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace cch::coding_agent {
class AgentSession;
} // namespace cch::coding_agent

namespace cch::tui {
class Terminal;
} // namespace cch::tui

namespace cch::coding_agent::tui {

// ── Closed application-level action seam (ADR 0040) ──────────────────────────
//
// Broad positional application callbacks become one closed action value and
// one move-only sink: every application-level Native TUI operation is one
// alternative of `TuiActionVariant`, carried to the composition host through
// `TuiActionSink`. Payloads are owned passive values; the host performs the
// operation and returns the operation's result. Render-state requests stay
// separate and may coalesce, but this path never drops an admitted action.

/// Open a URL in the platform browser (pi `openBrowser`: detached argv spawn,
/// never through a shell, best-effort). Fire-and-forget; the login dialog's
/// auth-URL view requests it (ADR 0032).
struct OpenBrowserAction {
    std::string url;
};

/// Write text to the system clipboard (pi `copyToClipboard` platform-tools
/// path). The host returns whether a clipboard tool ran successfully.
struct WriteClipboardAction {
    std::string text;
};

/// Suspend the process group (pi `handleCtrlZ` `process.kill(0, "SIGTSTP")`).
/// Fire-and-forget; a null host performs the platform SIGTSTP directly.
struct SuspendProcessAction {};

/// In-session session replacement (pi `AgentSessionRuntime` createRuntime
/// closure): create the next Agent Session for `switchSession`/`newSession`/
/// `fork`, and the boot path's deferred boot-session creation. The host
/// applies the CLI-owned facts and returns the created session for binding.
///
/// Field ownership seam (issue #507): session trust is engine-authoritative —
/// `project_trust_override` arrives resolved (the CLI `--approve`/
/// `--no-approve` override, the boot prompt decision, or the boot-workspace
/// inheritance, pi `projectTrustByCwd`) and the host only fills it in when
/// the engine left it unset. The pure CLI-owned resource and model facts are
/// host-authoritative (the host re-application is load-bearing for the fields
/// `make_session_request` deliberately omits — `no_themes`, `theme_paths`,
/// `no_context_files`, `system_prompt`, `append_system_prompt`); host-only
/// capabilities (User Shell, Runtime target, shared Models runtime) are
/// always host-set. See `cli/SessionReplacementHost.hpp`.
struct ReplaceSessionAction {
    runtime::AgentSessionCreationRequest request;
};

/// Report session-creation diagnostics on the boot path (pi
/// `reportDiagnostics`); the host wires this to stderr.
struct ReportBootDiagnosticsAction {
    std::vector<SessionDiagnostic> diagnostics;
};

/// Report boot-session creation failure (pi `print_creation_failure`); the
/// host wires this to stderr so the boot reports the error before exiting.
struct ReportBootCreationFailureAction {
    support::Error error;
};

/// One closed application-level Native TUI operation. Each alternative is an
/// owned passive payload; the composition host performs the operation.
using TuiActionVariant = std::variant<
    OpenBrowserAction,
    WriteClipboardAction,
    SuspendProcessAction,
    ReplaceSessionAction,
    ReportBootDiagnosticsAction,
    ReportBootCreationFailureAction>;

/// The result the host returns for one dispatched action. Fire-and-forget
/// operations return `std::monostate`; `WriteClipboardAction` returns the
/// clipboard-tool success; `ReplaceSessionAction` returns the created session
/// (or its creation error).
using TuiActionResultVariant = std::variant<
    std::monostate,
    bool,
    support::Expected<coding_agent::CreateAgentSessionResult>>;

/// Move-only sink carrying closed Native TUI actions to the composition host.
/// The host dispatches on `TuiActionVariant` and returns the matching
/// `TuiActionResultVariant`. Each action is stamped with the session
/// generation that admitted it; a host may reject actions from a retired
/// generation (ADR 0040, issue #461; consumed by the in-session replacement
/// host in #466). Delivery is lossless: every admitted action is carried
/// exactly once (render-state coalescing never applies here). A null sink
/// falls back to the TUI-local platform defaults for environment operations;
/// session replacement reports an unavailable host.
using TuiActionSink = std::move_only_function<
    support::Expected<TuiActionResultVariant>(std::size_t action_generation,
                                           TuiActionVariant action)>;
struct InteractiveModeConfig {
    std::filesystem::path agent_config_directory;
    std::unique_ptr<AsyncClipboardReader> clipboard_reader{nullptr};
    std::optional<std::string> initial_prompt{std::nullopt};
    PromptOptions initial_prompt_options{};
    /// pi `modelFallbackMessage` (sdk.ts `createAgentSession`): shown as a
    /// `Warning: <message>` boot line in the chat container (pi
    /// `interactive-mode.ts` `showWarning`). Absent in print mode.
    std::optional<std::string> model_fallback_message{std::nullopt};
    /// One move-only sink carrying every application-level Native TUI
    /// operation to the composition host (ADR 0040). Null falls back to the
    /// TUI-local platform defaults for environment operations (browser,
    /// clipboard, process suspend) and reports session replacement as
    /// unavailable; the CLI interactive host always supplies it and tests
    /// inject recorders through it.
    TuiActionSink action_sink{nullptr};
    /// CLI-owned facts reused for in-session session replacement requests.
    runtime::InteractiveSessionFacts session_facts{};
    /// Boot path (pi main.ts `createRuntime` + `resolveProjectTrust`): when
    /// set, the state creates the boot session itself after the boot trust
    /// prompt resolves (the main-TUI overlay, G2), instead of binding a
    /// pre-created session. The CLI interactive host sets this; focused
    /// tests bind pre-created sessions.
    std::optional<runtime::AgentSessionCreationRequest> boot_request{std::nullopt};
};

/// Run the private Native TUI composition until its exit binding is received.
/// The borrowed Agent Session and Terminal must outlive the returned coroutine.
[[nodiscard]] boost::asio::awaitable<support::ExpectedVoid> run_interactive_mode(
    AgentSession& session,
    cch::tui::Terminal& terminal,
    InteractiveModeConfig config = {});

/// Boot the private Native TUI composition with session creation deferred to
/// the boot (pi main.ts `createAgentSessionRuntime`): the TUI starts first,
/// the boot trust prompt resolves as an overlay when a trust-requiring
/// resource exists and no override is set (G2 record), then the session is
/// created through the config's `boot_request`/`action_sink` with the
/// decided trust, bound, and the initial prompt submitted. The Terminal must
/// outlive the returned coroutine.
[[nodiscard]] boost::asio::awaitable<support::ExpectedVoid> run_interactive_mode_boot(
    cch::tui::Terminal& terminal,
    InteractiveModeConfig config);

} // namespace cch::coding_agent::tui
