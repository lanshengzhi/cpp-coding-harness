#pragma once

#include <cch/agent/harness/FileSystem.hpp>
#include <cch/support/Error.hpp>
#include "support/UniqueFd.hpp"

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace cch::harness {

/// Workspace-scoped filesystem operations with containment and symlink safety.
///
/// All addressed-path operations reject absolute paths, ".." escapes, and
/// symlinks that resolve outside the workspace. Metadata and listing use
/// lstat-equivalent no-follow semantics.
class SyncLocalExecutionEnv;

class WorkspaceFileSystem {
public:
    WorkspaceFileSystem();
    explicit WorkspaceFileSystem(std::filesystem::path workspace);

    static support::Expected<WorkspaceFileSystem> create(const std::filesystem::path& workspace);

    [[nodiscard]] const std::filesystem::path& root() const { return root_; }

    /// Resolve a workspace-relative path to an absolute addressed path,
    /// validating containment. Does not require the path to exist.
    [[nodiscard]] support::Expected<std::filesystem::path> resolve_addressed_path(const std::string& requested) const;

    // Legacy tool-shaped operations used by private project-resource adapters.
    [[nodiscard]] support::Expected<std::string> read_existing_file(const std::string& requested) const;
    [[nodiscard]] support::Expected<std::size_t> write_file(
        const std::string& requested,
        const std::string& content,
        bool create_parents) const;

    // Pi-shaped filesystem operations.
    [[nodiscard]] std::expected<std::string, FileError> absolutePath(const std::string& path) const;
    [[nodiscard]] std::expected<std::string, FileError> joinPath(const std::vector<std::string>& parts) const;
    [[nodiscard]] std::expected<std::string, FileError> readTextFile(const std::string& path) const;
    [[nodiscard]] std::expected<std::vector<std::string>, FileError> readTextLines(
        const std::string& path,
        std::optional<int> maxLines = std::nullopt) const;
    [[nodiscard]] std::expected<BinaryData, FileError> readBinaryFile(const std::string& path) const;
    [[nodiscard]] std::expected<void, FileError> writeFile(
        const std::string& path,
        const WriteContent& content) const;
    [[nodiscard]] std::expected<void, FileError> appendFile(
        const std::string& path,
        const WriteContent& content) const;
    [[nodiscard]] std::expected<FileInfo, FileError> fileInfo(const std::string& path) const;
    [[nodiscard]] std::expected<std::vector<FileInfo>, FileError> listDir(const std::string& path) const;
    [[nodiscard]] std::expected<std::string, FileError> canonicalPath(const std::string& path) const;
    [[nodiscard]] std::expected<bool, FileError> exists(const std::string& path) const;
    [[nodiscard]] std::expected<void, FileError> createDir(
        const std::string& path,
        bool recursive = true) const;
    [[nodiscard]] std::expected<void, FileError> remove(
        const std::string& path,
        bool recursive = false) const;

    // Workspace-contained temporary resources.
    [[nodiscard]] std::expected<std::string, FileError> createTempDir(
        std::optional<std::string> prefix = std::nullopt) const;
    [[nodiscard]] std::expected<std::string, FileError> createTempFile(
        std::optional<std::string> prefix = std::nullopt,
        std::optional<std::string> suffix = std::nullopt) const;

private:
    friend class SyncLocalExecutionEnv;

    struct TemporaryState final {
        std::mutex mutex;
        bool cleanup_started{false};
        std::vector<std::filesystem::path> resources;
    };

    [[nodiscard]] static support::Error workspace_error(std::string message);
    [[nodiscard]] static FileError util_error_to_file_error(const support::Error& error, const std::string& path);

    [[nodiscard]] support::Expected<support::UniqueFd> open_workspace_root() const;
    [[nodiscard]] support::Expected<support::UniqueFd> open_parent_directory(
        const std::filesystem::path& target,
        bool create_missing) const;
    [[nodiscard]] support::Expected<void> create_parent_directories(const std::filesystem::path& target) const;
    [[nodiscard]] std::expected<std::string, FileError> read_existing_file_bounded(
        const std::string& requested,
        std::size_t max_bytes) const;
    [[nodiscard]] bool track_temporary_resource(const std::filesystem::path& path) const;

    /// Remove this instance's tracked temporary resources only. Errors are
    /// intentionally ignored so cleanup remains best-effort and idempotent.
    void cleanup_temporary_resources() const noexcept;

    [[nodiscard]] bool inside(const std::filesystem::path& path) const;
    [[nodiscard]] bool inside_lexically(const std::filesystem::path& path) const;
    [[nodiscard]] static std::filesystem::path canonicalized(std::filesystem::path workspace);
    [[nodiscard]] static std::filesystem::path default_root();

    std::filesystem::path root_{default_root()};
    std::shared_ptr<TemporaryState> temporary_state_;
};

} // namespace cch::harness
