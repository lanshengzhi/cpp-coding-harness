#pragma once

// Clipboard image/text paste support for the Native TUI editor (pi
// `pasteImage`/`pasteText` subset): an image payload is written to a unique
// `pi-clipboard-*` temp file and its path is inserted into the editor,
// otherwise the clipboard text is inserted directly. Extraction #506 from
// the pre-#506 interactive monolith.
//
// Repository-private `cch_coding_agent` implementation header: not part of
// an Owner Interface, not installed, never exported.

#include <boost/asio/awaitable.hpp>

#include <optional>
#include <string>

namespace cch::coding_agent::tui {

class AsyncClipboardReader;

/// Read the next paste payload from the clipboard: the temp-file path of a
/// supported image, or the clipboard text. Empty when no usable payload is
/// available. Baseline clipboard failures are intentionally silent (pi
/// treats paste as best-effort). Executor-confined like the reader.
[[nodiscard]] boost::asio::awaitable<std::optional<std::string>>
read_clipboard_insert_content(AsyncClipboardReader& reader);

} // namespace cch::coding_agent::tui
