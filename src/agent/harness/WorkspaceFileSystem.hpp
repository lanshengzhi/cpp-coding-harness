#pragma once

#include <cch/agent/harness/FileSystem.hpp>
#include <cch/agent/harness/LocalFileSystem.hpp>
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

/// Workspace-scoped filesystem operations with containment and symlink safety.
///
/// Addressed-path operations accept workspace-relative paths and absolute
/// paths normalizing inside the workspace root, and reject ".." escapes and
/// symlinks that resolve outside the workspace. Read operations additionally
/// accept absolute paths under the session's authorized skill roots; writes
/// stay workspace-contained. Metadata and listing use lstat-equivalent
/// no-follow semantics.
class AsyncLocalFileSystem;
class AsyncLocalShell;

class WorkspaceFileSystem {
public:
    WorkspaceFileSystem();
    explicit WorkspaceFileSystem(
            std::filesystem::path workspace, std::shared_ptr<const AuthorizedSkillRoots> skill_read_roots = nullptr);

    static support::Expected<WorkspaceFileSystem> create(const std::filesystem::path& workspace);

    [[nodiscard]] const std::filesystem::path& root() const { return root_; }

    /// Resolve a workspace-relative path to an absolute addressed path,
    /// validating containment. Does not require the path to exist.
    [[nodiscard]] support::Expected<std::filesystem::path> resolve_addressed_path(const std::string& requested) const;

    /// Read-scoped resolution: the addressed-path contract plus absolute
    /// paths under the authorized skill roots. Writes keep using
    /// `resolve_addressed_path` so they stay workspace-contained.
    [[nodiscard]] support::Expected<std::filesystem::path> resolve_read_path(const std::string& requested) const;

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
    // the same containment implementation; no parallel path validation.
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

    [[nodiscard]] support::Expected<support::UniqueFd> open_workspace_root() const;
    [[nodiscard]] support::Expected<support::UniqueFd> open_parent_directory(
            const std::filesystem::path& target, bool create_missing, int* failure_errno = nullptr) const;
    // The returned pointer borrows the roots argument.
    [[nodiscard]] const std::filesystem::path* authorizing_skill_root(
            const std::filesystem::path& target, const std::vector<std::filesystem::path>& roots) const;
    [[nodiscard]] support::Expected<support::UniqueFd> walk_child_directories(support::UniqueFd base,
            const std::filesystem::path& relative,
            bool create_missing,
            int* failure_errno) const;
    [[nodiscard]] support::Expected<support::UniqueFd> open_authorized_skill_parent(
            const std::filesystem::path& target, int* failure_errno = nullptr) const;
    [[nodiscard]] support::Expected<void> validate_directory(const std::filesystem::path& target) const;
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

    [[nodiscard]] bool inside(const std::filesystem::path& path) const;
    [[nodiscard]] bool inside_lexically(const std::filesystem::path& path) const;
    [[nodiscard]] static std::filesystem::path canonicalized(std::filesystem::path workspace);
    [[nodiscard]] static std::filesystem::path default_root();

    std::filesystem::path root_{default_root()};
    std::shared_ptr<const AuthorizedSkillRoots> skill_read_roots_;
    std::shared_ptr<TemporaryState> temporary_state_;
};

} // namespace cch::harness
