#pragma once

#include "cch/util/Error.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace cch::harness::session {

/// Durable, append-only line storage for a JSONL session file.
///
/// SessionJournal knows nothing about message schemas, redaction, or tree
/// semantics. It only creates/reads/writes raw lines with the safety rules
/// required for sensitive transcript files (owner-only permissions, symlink
/// rejection, O_NOFOLLOW append where available).
class SessionJournal {
public:
    /// Create a new session file at `path` containing `header_line` followed by
    /// a newline. Fails if the file already exists or if permissions cannot be
    /// made owner-only.
    static util::Expected<SessionJournal> create_new(
        const std::filesystem::path& path, std::string_view header_line);

    /// Open an existing session file for append. Validates path safety and
    /// permission rules but does not parse contents.
    static util::Expected<SessionJournal> open_existing(const std::filesystem::path& path);

    /// Append a single line (caller is responsible for trailing newline).
    [[nodiscard]] util::ExpectedVoid append_line(std::string_view line) const;

    /// Read all lines from the file. Preserves empty lines in the returned
    /// vector; callers normally skip blank lines.
    [[nodiscard]] util::Expected<std::vector<std::string>> read_lines() const;

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

private:
    static util::ExpectedVoid validate_session_path_for_open(
        const std::filesystem::path& path, bool must_exist);
    static util::ExpectedVoid ensure_private_permissions(
        const std::filesystem::path& path, bool existing);
    static util::ExpectedVoid write_new_file_exclusive(
        const std::filesystem::path& path, std::string_view content);

    std::filesystem::path path_;
};

} // namespace cch::harness::session
