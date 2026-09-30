#pragma once

#include <cch/support/Error.hpp>

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
    static support::Expected<SessionJournal> create_new(
        const std::filesystem::path& path, std::string_view header_line);

    /// Validate a new session file's path without creating anything: the same
    /// safety and uniqueness checks as `create_new`, but the file appears only
    /// when `flush_new` writes the header and pending lines (pi
    /// `SessionManager`'s delayed first flush; the flush trigger lives in the
    /// owning JsonlSessionStore).
    static support::Expected<SessionJournal> create_deferred(
        const std::filesystem::path& path);

    /// Whether the session file still waits for its first flush.
    /// `create_new`/`open_existing` journals are never deferred.
    [[nodiscard]] bool deferred() const { return deferred_; }

    /// Write the deferred session file exclusively (fails if the file appeared
    /// since `create_deferred`): the header line followed by every pending
    /// line, in order. On success the journal is no longer deferred and
    /// `append_line` writes through. On failure no file remains and the
    /// journal stays deferred, so the next flush retries the whole batch.
    [[nodiscard]] support::ExpectedVoid flush_new(
        std::string_view header_line, const std::vector<std::string>& lines);

    /// Open an existing session file for append. Validates path safety and
    /// permission rules but does not parse contents.
    static support::Expected<SessionJournal> open_existing(const std::filesystem::path& path);

    /// Append a single line (caller is responsible for trailing newline).
    /// A deferred journal rejects appends: its lines go through `flush_new`.
    [[nodiscard]] support::ExpectedVoid append_line(std::string_view line) const;

    /// Read all lines from the file. Preserves empty lines in the returned
    /// vector; callers normally skip blank lines.
    [[nodiscard]] support::Expected<std::vector<std::string>> read_lines() const;

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

private:
    static support::ExpectedVoid validate_session_path_for_open(
        const std::filesystem::path& path, bool must_exist);
    /// The shared publication-time prefix of `create_new`/`create_deferred`:
    /// path safety plus the not-already-existing check.
    static support::ExpectedVoid validate_new_session_path(
        const std::filesystem::path& path);
    static support::ExpectedVoid ensure_private_permissions(
        const std::filesystem::path& path, bool existing);
    static support::ExpectedVoid write_new_file_exclusive(
        const std::filesystem::path& path, std::string_view content);

    std::filesystem::path path_;
    bool deferred_{false};
};

} // namespace cch::harness::session
