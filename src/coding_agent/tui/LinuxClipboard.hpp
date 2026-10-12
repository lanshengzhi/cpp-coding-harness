#pragma once

// The Linux clipboard reader the Native TUI composition installs as the
// injected clipboard capability (pi `readClipboardText` in `clipboard.ts`):
// text through pi's helper-command order with an optional injected platform
// helper behind it. #993 ships no in-process Linux provider, so images and
// clipboard file paths come from that helper alone.
//
// Repository-private `cch_coding_agent` implementation header: not part of an
// Owner Interface, not installed, never exported.

#include "coding_agent/tui/ClipboardProcess.hpp"

#include <cch/tui/NativeClipboard.hpp>

#include <memory>

namespace cch::coding_agent::tui {

/// Build the Linux clipboard reader for one observed environment.
///
/// Text follows pi exactly: `termux-clipboard-get`, then `wl-paste --no-newline
/// --type text` under `WAYLAND_DISPLAY`, then `xclip -selection clipboard -out`
/// and `xsel --clipboard --output` under `DISPLAY`, 5000 ms each. The first
/// helper that exits 0 wins — an empty buffer is a successful *empty* read and
/// does not fall through — and every helper failure is swallowed into "no
/// content". `native_fallback` is asked only when no helper succeeded, and never
/// gains content the commands did not report.
///
/// A read whose stop token is requested while a helper runs completes with
/// `ErrorCode::Cancelled` after the helper is killed and reaped. The returned
/// reader must outlive its pending reads (the composition owns it for the run's
/// lifetime).
[[nodiscard]] std::unique_ptr<cch::tui::NativeClipboard> make_linux_clipboard_reader(
        ClipboardEnvironment environment,
        std::unique_ptr<cch::tui::NativeClipboard> native_fallback = nullptr);

} // namespace cch::coding_agent::tui
