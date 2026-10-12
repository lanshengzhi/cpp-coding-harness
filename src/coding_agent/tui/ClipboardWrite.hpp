#pragma once

// Linux clipboard copy through pi's platform-tools path (pi `clipboard.ts`
// `copyToClipboard`): the tool order per environment, the WSL Windows round
// trip, and the OSC 52 route through the terminal that owns the copy. #993
// replaces the pre-#993 path, which read `getenv` inline, tried only one tool
// per environment, never reached for OSC 52, and reported success whenever an
// X11 tool merely ran.
//
// Repository-private `cch_coding_agent` implementation header: not part of an
// Owner Interface, not installed, never exported.

#include "coding_agent/tui/ClipboardProcess.hpp"

#include <functional>
#include <string>
#include <string_view>

namespace cch::coding_agent::tui {

/// Writes one OSC 52 sequence on the terminal that owns the copy (pi
/// `emitOsc52`'s `process.stdout.write`). pi writes the sequence and reports
/// success without inspecting the write, so neither does this seam: the copy
/// result never depends on what the terminal did with the bytes.
using ClipboardOsc52Writer = std::move_only_function<void(std::string_view)>;

/// The environment and terminal capabilities one copy runs under. The
/// environment is observed at the composition boundary and injected from there;
/// the OSC 52 writer is installed by the owner of the terminal the copy writes
/// through (the interactive engine). No clipboard policy reads the process
/// environment itself.
struct ClipboardWriteOptions {
    ClipboardEnvironment environment{};
    ClipboardOsc52Writer write_osc52{};
};

/// pi `copyToClipboard`'s outcome on the closed action seam: the success bit and
/// the message pi throws for the status line (`Clipboard unavailable: ...`),
/// which the caller shows verbatim.
struct ClipboardCopyResult {
    bool copied{false};
    std::string error{};
};

/// Copy `text` exactly as pi does. Linux writers run in pi's order —
/// `termux-clipboard-set`, `wl-copy`, `xclip -selection clipboard`,
/// `xsel --clipboard --input`, 5000 ms each — and the first helper to exit 0
/// wins. WSL then reaches the Windows clipboard, preferring OSC 52 under
/// `WT_SESSION` and otherwise reading the text from a UTF-8 temp file through
/// PowerShell. OSC 52 is emitted through the terminal when the session is
/// remote (`SSH_CONNECTION`/`SSH_CLIENT`/`MOSH_CONNECTION`) or when a headless
/// Linux session has no other clipboard route; a desktop Linux session with a
/// display and failing helpers reports failure instead, because OSC 52 cannot be
/// verified (#9618). A copy whose OSC 52 payload exceeds
/// `kMaxOsc52EncodedLength` reports the size-limit failure.
///
/// The WSL round trip and the helper commands are bounded by their pi timeouts,
/// so this never blocks longer than one timeout per attempted candidate and
/// never leaves a child process behind. `options` is mutable only because the
/// move-only OSC 52 sink has no const call operator.
[[nodiscard]] ClipboardCopyResult write_clipboard_text(std::string_view text, ClipboardWriteOptions& options);

} // namespace cch::coding_agent::tui
