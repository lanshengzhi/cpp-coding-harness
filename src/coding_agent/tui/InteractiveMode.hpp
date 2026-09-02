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
/// Session replacement is not an action value: it crosses the asynchronous
/// `AsyncSessionReplacementSink` strong capability exclusively (issue #581).
using TuiActionVariant = std::variant<OpenBrowserAction,
        WriteClipboardAction,
        SuspendProcessAction,
        ReportBootDiagnosticsAction,
        ReportBootCreationFailureAction>;

/// The result the host returns for one dispatched action. Fire-and-forget
/// operations return `std::monostate`; `WriteClipboardAction` returns the
/// clipboard-tool success.
using TuiActionResultVariant = std::variant<std::monostate, bool>;

/// Move-only sink carrying closed Native TUI actions to the composition host.
/// The host dispatches on `TuiActionVariant` and returns the matching
/// `TuiActionResultVariant`. Each action is stamped with the session
/// generation that admitted it; a host may reject actions from a retired
/// generation (ADR 0040, issue #461; consumed by the in-session replacement
/// host in #466). Delivery is lossless: every admitted action is carried
/// exactly once (render-state coalescing never applies here). A null sink
/// falls back to the TUI-local platform defaults for environment operations.
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
