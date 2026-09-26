#pragma once

#include "coding_agent/tui/Theme.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace cch::coding_agent::tui {

/// pi `os.homedir()` as this repository reads it elsewhere
/// (`Footer.cpp`, `cch::tui`'s image-path shortening): `$HOME`, then
/// `$USERPROFILE`, and the empty path when neither is set. One home lookup for
/// the whole module, so the `~` expansion behind `resolve_to_cwd` and the
/// shortening behind `shorten_path` cannot drift apart.
[[nodiscard]] std::filesystem::path home_directory();

/// pi `core/tools/path-utils.ts:48` `resolveToCwd`, which is ADR 0057's
/// uniform path resolution: unicode-space normalization, leading-`@` stripping,
/// and `~` expansion against `$HOME`, then an absolute path honored anywhere
/// after lexical normalization and a relative one resolved against the tool
/// execution's cwd.
///
/// The resolution is lexical, so this performs no filesystem I/O.
/// `harness::WorkspaceFileSystem` owns the same normalization for filesystem
/// access, but that is a capability this renderer seam may not take: its
/// `resolve_to_cwd` is private, it resolves relative paths against the
/// *workspace root* rather than the tool execution's cwd this seam is handed,
/// and a string-out pure renderer has no filesystem to call it with. Keeping
/// the semantics identical here is what stops a `SKILL.md` reached through `@`
/// or a unicode space from being classified one way by the filesystem and
/// another way here.
[[nodiscard]] std::string resolve_to_cwd(std::string_view path, std::string_view cwd);

/// pi `core/tools/render-utils.ts:10` `shortenPath`: a home-prefixed path
/// renders as `~` + the remainder. It is home shortening only — pi does not
/// make paths cwd-relative here, and neither does this.
[[nodiscard]] std::string shorten_path(std::string_view path);

/// pi `render-utils.ts:75` `renderToolPath`: the accent-coloured, `~`-shortened
/// path a read/write/edit title shows, wrapped in an OSC 8 `file://` hyperlink
/// when the terminal reports hyperlink support. `raw_path` is the call's raw
/// path argument, where nullopt is pi's `null` (the argument is present and not
/// a string, which renders `[invalid arg]`) and an empty view is an absent or
/// null argument (which renders `...`, the progressive slot pi shows while the
/// arguments are still streaming).
[[nodiscard]] std::string render_tool_path(
        const LiveTheme& theme, std::string_view cwd, std::optional<std::string_view> raw_path);

} // namespace cch::coding_agent::tui
