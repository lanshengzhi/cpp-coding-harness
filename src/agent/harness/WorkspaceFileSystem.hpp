#pragma once

#include <cch/agent/harness/FileSystem.hpp>
#include <cch/support/Error.hpp>
#include "support/UniqueFd.hpp"

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace cch::harness {

/// Workspace-rooted filesystem operations with uniform pi `resolveToCwd`
/// path resolution and symlink safety.
///
/// Every operation resolves paths through pi `resolveToCwd` semantics (ADR
/// 0057): unicode-space normalization, leading-"@" stripping, and `~`
/// expansion against $HOME, then absolute paths are honored anywhere after
/// lexical normalization and relative paths resolve against the workspace
/// root with ".." resolved by normalization rather than rejected. There is
/// no workspace containment, path allowlist, or resolution-scope split;
/// filesystem access authority matches the host user process and
/// inaccessible paths surface standard OS-level errors. The no-follow
/// symlink policy and atomic writes apply uniformly. Metadata and listing
/// use lstat-equivalent no-follow semantics.
class AsyncLocalFileSystem;
class AsyncLocalShell;

class WorkspaceFileSystem {
public:
    WorkspaceFileSystem();
    explicit WorkspaceFileSystem(std::filesystem::path workspace);

    static support::Expected<WorkspaceFileSystem> create(const std::filesystem::path& workspace);

    [[nodiscard]] const std::filesystem::path& root() const { return root_; }

    // Legacy tool-shaped operations used by private project-resource adapters.
    [[nodiscard]] support::Expected<std::string> read_existing_file(
            const std::string& requested, std::stop_token stop_token = {}) const;
    [[nodiscard]] support::Expected<std::size_t> write_file(const std::string& requested,
            std::string_view content,
            bool create_parents,
            std::stop_token stop_token = {}) const;

    // Pi-shaped filesystem operations.
    [[nodiscard]] std::expected<std::string, FileError> absolutePath(const std::string& path) const;
    [[nodiscard]] std::expected<std::string, FileError> joinPath(const std::vector<std::string>& parts) const;
    [[nodiscard]] std::expected<std::string, FileError> readTextFile(
            const std::string& path, std::stop_token stop_token = {}) const;
    [[nodiscard]] std::expected<std::vector<std::string>, FileError> readTextLines(
            const std::string& path, std::optional<int> maxLines = std::nullopt, std::stop_token stop_token = {}) const;
    [[nodiscard]] std::expected<BinaryData, FileError> readBinaryFile(
            const std::string& path, std::stop_token stop_token = {}) const;
    [[nodiscard]] std::expected<void, FileError> writeFile(
            const std::string& path, const WriteContent& content, std::stop_token stop_token = {}) const;
    [[nodiscard]] std::expected<void, FileError> appendFile(
            const std::string& path, const WriteContent& content, std::stop_token stop_token = {}) const;
    [[nodiscard]] std::expected<FileInfo, FileError> fileInfo(const std::string& path) const;
    [[nodiscard]] std::expected<std::vector<FileInfo>, FileError> listDir(
            const std::string& path, std::stop_token stop_token = {}) const;
    [[nodiscard]] std::expected<std::string, FileError> canonicalPath(const std::string& path) const;
    [[nodiscard]] std::expected<bool, FileError> exists(const std::string& path) const;
    [[nodiscard]] std::expected<void, FileError> createDir(const std::string& path, bool recursive = true) const;
    [[nodiscard]] std::expected<void, FileError> remove(
            const std::string& path, bool recursive = false, std::stop_token stop_token = {}) const;

    // Workspace-contained temporary resources.
    [[nodiscard]] std::expected<std::string, FileError> createTempDir(
            std::optional<std::string> prefix = std::nullopt) const;
    [[nodiscard]] std::expected<std::string, FileError> createTempFile(
            std::optional<std::string> prefix = std::nullopt, std::optional<std::string> suffix = std::nullopt) const;

private:
    friend class AsyncLocalFileSystem;
    // The Local Shell Adapter validates working-directory overrides through
    // the same resolveToCwd resolution and directory validation; no parallel
    // path validation.
    friend class AsyncLocalShell;

    struct TemporaryResource final {
        std::string name;
        support::UniqueFd descriptor;
        bool directory{false};
    };

    struct TemporaryState final {
        std::mutex mutex;
        bool cleanup_started{false};
        support::UniqueFd temporary_directory;
        std::vector<TemporaryResource> resources;
    };

    [[nodiscard]] static support::Error workspace_error(std::string message);
    [[nodiscard]] static FileError util_error_to_file_error(const support::Error& error, const std::string& path);

    /// The single path-resolution operation behind every filesystem
    /// operation (pi `resolveToCwd`, ADR 0057): pi preprocessing
    /// (unicode-space normalization, leading-"@" stripping, `~` expansion
    /// against $HOME), then absolute paths are honored anywhere after lexical
    /// normalization; relative paths resolve against the workspace root with
    /// ".." handled by normalization rather than rejection. Does not require
    /// the path to exist.
    [[nodiscard]] support::Expected<std::filesystem::path> resolve_to_cwd(const std::string& requested) const;

    [[nodiscard]] support::Expected<support::UniqueFd> open_workspace_root() const;
    /// Open a root path directly (O_DIRECTORY, no-follow), naming the root in
    /// the failure message. Roots have no addressable parent, so the
    /// parent+filename walk cannot open them.
    [[nodiscard]] support::Expected<support::UniqueFd> open_root_directory(
            const std::filesystem::path& root, std::string_view description = "root directory") const;
    [[nodiscard]] support::Expected<support::UniqueFd> open_parent_directory(
            const std::filesystem::path& target, bool create_missing, int* failure_errno = nullptr) const;
    [[nodiscard]] support::Expected<support::UniqueFd> walk_child_directories(support::UniqueFd base,
            const std::filesystem::path& relative,
            bool create_missing,
            int* failure_errno) const;
    [[nodiscard]] support::Expected<void> validate_directory(const std::filesystem::path& target) const;
    /// Shared open tail for regular-file reads once the parent directory is
    /// held open: no-follow inspection, symlink refusal, and size reporting.
    [[nodiscard]] std::expected<support::UniqueFd, FileError> open_regular_file_in_parent(
            int parent_fd, const std::string& filename, const std::string& requested, std::uintmax_t* size) const;
    [[nodiscard]] std::expected<std::string, FileError> read_bounded_from_open_file(int file_fd,
            std::uintmax_t file_size,
            const std::string& requested,
            std::size_t max_bytes,
            std::stop_token stop_token) const;
    [[nodiscard]] std::expected<support::UniqueFd, FileError> open_regular_file_for_read(
            const std::string& requested, std::uintmax_t* size, std::stop_token stop_token = {}) const;
    [[nodiscard]] support::Expected<void> create_parent_directories(const std::filesystem::path& target) const;
    [[nodiscard]] bool remove_directory_contents(
            int directory_fd, std::stop_token stop_token = {}, bool* cancelled = nullptr) const noexcept;
    [[nodiscard]] std::expected<std::string, FileError> read_existing_file_bounded(
            const std::string& requested, std::size_t max_bytes, std::stop_token stop_token = {}) const;
    [[nodiscard]] std::expected<BinaryData, FileError> read_binary_file_bounded(
            const std::string& requested, std::size_t max_bytes, std::stop_token stop_token = {}) const;
    [[nodiscard]] support::Expected<void> ensure_temporary_directory() const;

    /// Remove this instance's tracked temporary resources only. Errors are
    /// intentionally ignored so cleanup remains best-effort and idempotent.
    void cleanup_temporary_resources() const noexcept;

    [[nodiscard]] static std::filesystem::path canonicalized(std::filesystem::path workspace);
    [[nodiscard]] static std::filesystem::path default_root();

    std::filesystem::path root_{default_root()};
    std::shared_ptr<TemporaryState> temporary_state_;
};

} // namespace cch::harness
