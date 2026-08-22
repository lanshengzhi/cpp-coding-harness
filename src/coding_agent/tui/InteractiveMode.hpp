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

class InteractiveSessionRun;

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
/// installs the host-only capabilities on the request and crosses the
/// Session Assembly boundary with the CLI-owned facts; the boundary
/// re-applies them and returns the created session for binding.
///
/// Field ownership seam (issue #507): session trust is engine-authoritative —
/// `project_trust_override` arrives resolved (the CLI `--approve`/
/// `--no-approve` override, the boot prompt decision, or the boot-workspace
/// inheritance, pi `projectTrustByCwd`) and the boundary's facts merge only
/// fills it in when the engine left it unset. The pure CLI-owned resource and
/// model facts are re-applied unconditionally (load-bearing for the fields
/// `make_session_request` deliberately omits — `no_themes`, `theme_paths`,
/// `no_context_files`, `system_prompt`, `append_system_prompt`); host-only
/// capabilities (User Shell, Runtime target, shared Models runtime) are
/// always host-set. The merge lives behind the Session Assembly boundary
/// (`SessionFactory::apply_cli_facts`).
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

/// Run the private Native TUI composition until its exit binding is received.
/// The session intent in `run` selects whether to bind a pre-created Agent
/// Session or defer creation to the boot trust prompt.
/// The Terminal must outlive the returned coroutine.
[[nodiscard]] boost::asio::awaitable<support::ExpectedVoid> run_interactive_mode(
    cch::tui::Terminal& terminal,
    InteractiveSessionRun run);

} // namespace cch::coding_agent::tui
