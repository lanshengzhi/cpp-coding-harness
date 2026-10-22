#pragma once

// pi v1.3.0 `AgentSessionRuntime.importFromJsonl` — the file half of
// `/import` (agent-session-runtime.ts:359-406). The seam resolves the
// source path, derives the destination inside the session directory with
// pi's collision suffix, and copies the source file there. The runtime
// switch itself is the Native TUI's ordinary session replacement
// (`SessionFlowController`), so this module owns only the file facts.
//
// Repository-private `cch_coding_agent` implementation header: not part of
// an Owner Interface, not installed, never exported.

#include <cch/support/Error.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace cch::coding_agent::tui {

/// pi `getPathCommandArgument` (`/import`): the path argument of one path
/// command submission. The argument is the first whitespace-delimited token;
/// a quoted token keeps its inner text (the quotes are stripped) and an
/// unterminated quote yields no argument at all.
///
/// pi's spelling: `"/import"` alone carries no argument; `"/import \"a
/// b.jsonl\""` carries `a b.jsonl`; `"/import john's/x.jsonl"` carries
/// `john's/x.jsonl` unchanged.
[[nodiscard]] std::optional<std::string> path_command_argument(std::string_view argument);

/// pi `SessionImportFileNotFoundError`: the `/import` argument names no
/// existing file. The message is pi's verbatim wording, so the command can
/// report `Failed to import session: <message>` unchanged.
[[nodiscard]] inline support::Error session_import_file_not_found_error(const std::filesystem::path& resolved_path) {
    return support::make_error(support::ErrorCode::Session, "File not found: " + resolved_path.string());
}

/// The destination pi's `importFromJsonl` copies the source file to: the
/// session directory joined with the source's base name, or the first free
/// `<name>-<n><ext>` variant when that name is already taken.
///
/// pi's rule (`importFromJsonl`): the source is resolved first; when the
/// resolved destination is the resolved source itself the file is already
/// stored there and is not copied again. Otherwise the first free
/// `-1`, `-2`, ... suffix is chosen; the suffix is appended to the name and
/// the extension is preserved.
[[nodiscard]] std::filesystem::path import_destination_path(
        const std::filesystem::path& session_dir, const std::filesystem::path& resolved_source);

/// pi `importFromJsonl`'s copy step: copy the resolved source to its
/// destination inside the session directory, creating the directory when it
/// is missing. The source that already sits at the destination is left
/// alone (pi copies only when the source is not already stored there).
///
/// Returns the destination path. A source that is not already stored there
/// is copied with pi's exclusive semantics: the destination must not exist.
[[nodiscard]] support::Expected<std::filesystem::path> copy_imported_session(
        const std::filesystem::path& session_dir, const std::filesystem::path& resolved_source);

} // namespace cch::coding_agent::tui
