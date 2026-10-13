#pragma once

// Reusable TUI diagnostics (#1016): the frame writer accounting, the global
// debug-key callback, and the debug key identifier exposed through the shared
// `ITui` contract. These are the C++ equivalents of the frozen pi TUI
// diagnostics consumers drive directly: `TuiBase.onDebug`, `TuiBase.fullRedraws`,
// and the `PI_TUI_WRITE_LOG` terminal write log.

#include <cstddef>
#include <functional>
#include <string_view>

namespace cch::tui {

/// Terminal writes a frame renderer issued while presenting one frame. A write
/// the terminal refuses (Busy backpressure or a terminal failure) was attempted
/// but never committed, so refused or still-queued work does not count as
/// committed output (ADR 0067 frame-commit semantics).
struct TuiWriteAccounting {
    /// Write calls the renderer issued, refused calls included.
    std::size_t attempted_writes{0};
    /// Write calls the terminal accepted.
    std::size_t committed_writes{0};
    /// Payload bytes carried by the committed write calls.
    std::size_t committed_bytes{0};

    bool operator==(const TuiWriteAccounting&) const = default;
};

/// Callback for the global debug key (pi `TuiBase.onDebug`).
using TuiDebugHook = std::move_only_function<void()>;

/// Debug-key identifier matched with `matches_key` (pi `matchesKey(data, "shift+ctrl+d")`).
inline constexpr std::string_view kTuiDebugKey = "shift+ctrl+d";

} // namespace cch::tui
