#pragma once

#include <filesystem>
#include <string>

namespace cch::coding_agent::session_paths {

/// One identity for a newly created Agent Session. The timestamp is shared by
/// the persisted header and automatic filename; generating this value has no
/// filesystem side effects.
struct AutomaticSessionIdentity {
    std::string session_id;
    std::string created_at;
};

/// Fully derived default persisted target. All paths are passive values;
/// derived directory/file paths remain empty when the sessions root is
/// unavailable or relative.
struct AutomaticSessionTarget {
    std::filesystem::path sessions_root;
    std::filesystem::path workspace;
    std::filesystem::path workspace_directory;
    std::filesystem::path session_path;
    AutomaticSessionIdentity identity;
};

/// Encode an already-resolved workspace using pi's readable framing: remove
/// one leading root separator, replace '/', '\\', and ':' with '-', then wrap
/// the result in double hyphens.
[[nodiscard]] std::string encode_workspace_key(const std::filesystem::path& resolved_workspace);

/// Generate a lowercase RFC 4122 UUID v4 and one millisecond UTC ISO-8601
/// timestamp for a newly created session.
[[nodiscard]] AutomaticSessionIdentity generate_automatic_session_identity();

/// `<file-safe UTC timestamp>_<Session ID>.jsonl`, where ':' and '.' in the
/// identity timestamp are replaced with '-'.
[[nodiscard]] std::string automatic_session_filename(const AutomaticSessionIdentity& identity);

/// Compose the sessions root, encoded workspace directory, and automatic file
/// path without inspecting or creating filesystem state.
[[nodiscard]] AutomaticSessionTarget make_automatic_session_target(
    std::filesystem::path sessions_root,
    std::filesystem::path resolved_workspace,
    AutomaticSessionIdentity identity);

} // namespace cch::coding_agent::session_paths
