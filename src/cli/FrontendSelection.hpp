#pragma once

#include "cli/CliConfig.hpp"
#include <cch/util/Error.hpp>

namespace cch::cli {

enum class Frontend {
    Print,
    Json,
    Rpc,
    NativeTui,
};

struct FrontendEnvironment {
    bool stdin_is_terminal{false};
    bool stdout_is_terminal{false};
    bool native_tui_supported{false};
};

/// Observe the process streams and compile-time Native TUI platform support.
[[nodiscard]] FrontendEnvironment detect_frontend_environment();

/// Resolve one frontend before Agent Session creation. JSON and RPC are
/// explicit protocol intents; text leaves TTY-based selection unchanged.
[[nodiscard]] util::Expected<Frontend> select_frontend(
    const CliConfig& config,
    FrontendEnvironment environment);

} // namespace cch::cli
