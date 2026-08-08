#pragma once

#include <string>

namespace cch::coding_agent::tui {

/// Write text to the system clipboard through pi's platform-tools path
/// (`clipboard.ts` `copyToClipboard`): `pbcopy` on macOS, `clip` on Windows,
/// and on Linux the Termux/Wayland/X11 tools (`termux-clipboard-set`,
/// `wl-copy`, `xclip -selection clipboard`, `xsel --clipboard --input`) in
/// pi's fallback order. The X11 tools claim success when they run, exactly
/// like pi's `copyToX11Clipboard`; a Wayland `wl-copy` must exit 0. The
/// remote-session OSC 52 fallback stays Deferred with no placeholder.
///
/// Returns false when no tool could run (or `wl-copy` failed without an
/// X11 fallback); the caller surfaces pi's error status.
[[nodiscard]] bool write_clipboard_text(std::string_view text);

} // namespace cch::coding_agent::tui
