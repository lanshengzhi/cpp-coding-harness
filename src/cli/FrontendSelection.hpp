#pragma once

#include "cli/CliConfig.hpp"
#include <cch/support/Error.hpp>

namespace cch::cli {

/// The pi frontend set: one-shot print output and the interactive Native TUI
/// (pi `runPrintMode` / `runInteractiveMode`). The removed JSON and RPC
/// frontends leave no enumerator and no dispatch (pi-coding-agent phase,
/// ADR 0036).
enum class Frontend {
    Print,
    Interactive,
};

struct FrontendEnvironment {
    bool stdin_is_terminal{false};
    bool stdout_is_terminal{false};
};

/// Observe the process streams and compile-time Native TUI platform support.
[[nodiscard]] FrontendEnvironment detect_frontend_environment();

/// Resolve one frontend before Agent Session creation. `--print`/`-p` and
/// either non-TTY stream select one-shot print output; interactive
/// stdin/stdout selects the Native TUI on supported platforms (pi's
/// TTY-based selection, `--mode text` leaves it unchanged).
[[nodiscard]] support::Expected<Frontend> select_frontend(
    const CliConfig& config,
    FrontendEnvironment environment);

} // namespace cch::cli
