#pragma once

#include <cch/util/Error.hpp>

#include <boost/asio/awaitable.hpp>

#include <optional>
#include <string>

namespace cch::coding_agent::tui {

/// Resolve the external editor command (pi `SettingsManager
/// getExternalEditorCommand` with the settings field out of the subset —
/// env-only per G2): `$VISUAL`, then `$EDITOR`, then the platform default
/// (`notepad` on Windows, `nano` elsewhere). Empty when no env var is set
/// and the default cannot be determined.
[[nodiscard]] std::string external_editor_command();

/// pi `external-editor.ts` `editInExternalEditor`: write `content` to a
/// `prompt.md` inside a fresh `pi-editor-*` temp directory, print pi's
/// launch notice, run the command (argv-split; `filePath` appended) with
/// inherited stdio, and read the file back with one trailing newline
/// stripped. Cleanup of the temp directory is best effort (pi's `finally`
/// `rmSync`). Returns the edited content on a clean exit, or `std::nullopt`
/// when the editor failed or the file could not be read.
[[nodiscard]] boost::asio::awaitable<util::Expected<std::optional<std::string>>>
edit_in_external_editor(std::string command, std::string content);

} // namespace cch::coding_agent::tui
