#pragma once

#include <cch/support/AsyncResult.hpp>
#include <cch/support/Error.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace cch::harness {

/// Kind of a filesystem object. Symlinks are not followed automatically.
enum class FileKind {
    File,
    Directory,
    Symlink,
};

/// Stable, backend-independent file error codes.
enum class FileErrorCode {
    Aborted,
    Busy,
    NotFound,
    PermissionDenied,
    NotDirectory,
    IsDirectory,
    Invalid,
    NotSupported,
    ResourceLimit,
    Unknown,
};

/// Error returned by pi-shaped filesystem operations.
struct FileError {
    FileErrorCode code{FileErrorCode::Unknown};
    std::string message;
    std::optional<std::string> path;
};

/// Fixed result capacities for the filesystem capability. These values are
/// deliberately not caller-configurable: the Local Adapter pre-charges the
/// corresponding bounded result before it starts blocking work.
struct FileSystemCapacity {
    std::size_t max_file_bytes{512 * 1024};
    std::size_t max_text_lines{8 * 1024};
    std::size_t max_text_lines_result_bytes{512 * 1024};
    std::size_t max_directory_entries{2 * 1024};
    std::size_t max_directory_result_bytes{512 * 1024};
};

inline constexpr FileSystemCapacity kFileSystemCapacity{};

/// Metadata for one filesystem object. Symlink targets are not followed.
struct FileInfo {
    /// Basename of the addressed path.
    std::string name;
    /// Absolute, syntactically normalized addressed path.
    std::string path;
    /// Object kind without following symlinks.
    FileKind kind{FileKind::File};
    /// Size in bytes.
    std::uint64_t size{0};
    /// Modification time as milliseconds since Unix epoch.
    std::int64_t mtimeMs{0};
};

/// Binary data container for file I/O.
using BinaryData = std::vector<std::byte>;

/// Content passed to write/append: either UTF-8 text or raw bytes.
using WriteContent = std::variant<std::string, BinaryData>;

/// Error conversion for consumers that use the shared support channel.
[[nodiscard]] inline support::Error to_util_error(FileError e) {
    auto code = support::ErrorCode::Workspace;
    switch (e.code) {
    case FileErrorCode::Aborted:
        code = support::ErrorCode::Cancelled;
        break;
    case FileErrorCode::Busy:
        code = support::ErrorCode::Busy;
        break;
    case FileErrorCode::NotFound:
        code = support::ErrorCode::Validation;
        break;
    case FileErrorCode::PermissionDenied:
        code = support::ErrorCode::Workspace;
        break;
    case FileErrorCode::NotDirectory:
    case FileErrorCode::IsDirectory:
    case FileErrorCode::Invalid:
        code = support::ErrorCode::Validation;
        break;
    case FileErrorCode::NotSupported:
        code = support::ErrorCode::Unknown;
        break;
    case FileErrorCode::ResourceLimit:
        code = support::ErrorCode::ResourceLimit;
        break;
    case FileErrorCode::Unknown:
        code = support::ErrorCode::Unknown;
        break;
    }
    return support::make_error(code, e.message, e.message, e.path);
}

/// Complete asynchronous filesystem capability. It owns the addressed root,
/// result-capacity policy, temporary-resource tracking, and cleanup contract.
/// Implementations must keep blocking work private and must not expose an
/// executor, scheduler, or Boost.Asio type (ADR 0040).
class AsyncFileSystem {
public:
    virtual ~AsyncFileSystem() = default;

    /// Root workspace path of the capability.
    [[nodiscard]] virtual const std::filesystem::path& workspace() const = 0;

    /// Return an absolute addressed path without requiring it to exist and
    /// without following symlinks.
    [[nodiscard]] virtual support::AsyncResult<std::string, FileError> absolutePath(
            std::string path, std::stop_token stop_token) = 0;

    /// Join path segments without requiring the result to exist.
    [[nodiscard]] virtual support::AsyncResult<std::string, FileError> joinPath(
            std::vector<std::string> parts, std::stop_token stop_token) = 0;

    /// Read an entire UTF-8 text file, subject to the fixed file-byte limit.
    [[nodiscard]] virtual support::AsyncResult<std::string, FileError> readTextFile(
            std::string path, std::stop_token stop_token) = 0;

    /// Read UTF-8 text lines, subject to fixed byte and line limits. maxLines
    /// can reduce the returned result but can never raise either fixed limit.
    [[nodiscard]] virtual support::AsyncResult<std::vector<std::string>, FileError> readTextLines(
            std::string path, std::optional<int> maxLines, std::stop_token stop_token) = 0;

    /// Read an entire binary file, subject to the fixed file-byte limit.
    [[nodiscard]] virtual support::AsyncResult<BinaryData, FileError> readBinaryFile(
            std::string path, std::stop_token stop_token) = 0;

    /// Create or overwrite a file, creating parent directories.
    [[nodiscard]] virtual support::AsyncResult<void, FileError> writeFile(
            std::string path, WriteContent content, std::stop_token stop_token) = 0;

    /// Create or append to a file, creating parent directories.
    [[nodiscard]] virtual support::AsyncResult<void, FileError> appendFile(
            std::string path, WriteContent content, std::stop_token stop_token) = 0;

    /// Return metadata for the addressed path without following symlinks.
    [[nodiscard]] virtual support::AsyncResult<FileInfo, FileError> fileInfo(
            std::string path, std::stop_token stop_token) = 0;

    /// List direct children of a directory without following symlinks. The
    /// complete result is rejected when either fixed directory limit is hit.
    [[nodiscard]] virtual support::AsyncResult<std::vector<FileInfo>, FileError> listDir(
            std::string path, std::stop_token stop_token) = 0;

    /// Return the canonical path for an existing path, resolving symlinks.
    [[nodiscard]] virtual support::AsyncResult<std::string, FileError> canonicalPath(
            std::string path, std::stop_token stop_token) = 0;

    /// Return false for missing paths. Other errors return FileError.
    [[nodiscard]] virtual support::AsyncResult<bool, FileError> exists(
            std::string path, std::stop_token stop_token) = 0;

    /// Create a directory, recursively when requested.
    [[nodiscard]] virtual support::AsyncResult<void, FileError> createDir(
            std::string path, bool recursive, std::stop_token stop_token) = 0;

    /// Remove a file or directory, recursively when requested.
    [[nodiscard]] virtual support::AsyncResult<void, FileError> remove(
            std::string path, bool recursive, std::stop_token stop_token) = 0;

    /// Create and track a workspace-contained temporary directory.
    [[nodiscard]] virtual support::AsyncResult<std::string, FileError> createTempDir(
            std::optional<std::string> prefix, std::stop_token stop_token) = 0;

    /// Create and track a workspace-contained temporary file.
    [[nodiscard]] virtual support::AsyncResult<std::string, FileError> createTempFile(
            std::optional<std::string> prefix, std::optional<std::string> suffix, std::stop_token stop_token) = 0;

    /// Asynchronously remove only this capability's tracked temporary
    /// resources. Cleanup is idempotent and best-effort; it never sweeps a
    /// shared temporary directory.
    [[nodiscard]] virtual support::AsyncResult<void, FileError> cleanup() = 0;
};

} // namespace cch::harness
