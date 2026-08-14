#pragma once

#include <cch/support/AsyncResult.hpp>
#include <cch/util/Error.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <stop_token>
#include <string>
#include <variant>
#include <vector>

namespace cch::harness {

// ---------------------------------------------------------------------------
// Pi-shaped passive value contracts
// ---------------------------------------------------------------------------

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
    Unknown,
};

/// Error returned by pi-shaped filesystem operations.
struct FileError {
    FileErrorCode code{FileErrorCode::Unknown};
    std::string message;
    std::optional<std::string> path;
};

/// Stable, backend-independent execution error codes.
enum class ExecutionErrorCode {
    Aborted,
    Busy,
    Timeout,
    ShellUnavailable,
    SpawnError,
    CallbackError,
    NotSupported,
    Unknown,
};

/// Error returned by pi-shaped shell operations.
struct ExecutionError {
    ExecutionErrorCode code{ExecutionErrorCode::Unknown};
    std::string message;
};

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

/// Effective user-level configuration for local Shell launches. Authorization
/// remains an assembly decision and is intentionally not represented here.
struct ShellConfig {
    std::optional<std::string> shell_path{std::nullopt};
    std::optional<std::string> command_prefix{std::nullopt};
};

/// Options for Shell::exec.
struct ExecOptions {
    /// Working directory override (relative resolved against workspace).
    std::optional<std::string> cwd;
    /// Environment variable overrides. Non-secret keys shadow the base env.
    std::optional<std::map<std::string, std::string>> env;
    /// Wall-clock timeout. Zero / unset means no timeout.
    std::optional<std::chrono::milliseconds> timeout;
    /// Active prompt cancellation token.
    std::stop_token stop_token{};
    /// Called with stdout chunks as they are produced.
    std::optional<std::move_only_function<void(std::string_view)>> onStdout;
    /// Called with stderr chunks as they are produced.
    std::optional<std::move_only_function<void(std::string_view)>> onStderr;
};

/// Result of a pi-shaped shell execution with split streams.
struct ShellExecResult {
    std::string stdout_output;
    std::string stderr_output;
    int exitCode{0};
};

// ---------------------------------------------------------------------------
// Error conversion helpers (pi-shaped → util::Error)
// ---------------------------------------------------------------------------

[[nodiscard]] inline util::Error to_util_error(FileError e) {
    auto code = util::ErrorCode::Workspace;
    switch (e.code) {
    case FileErrorCode::Aborted:
        code = util::ErrorCode::Cancelled;
        break;
    case FileErrorCode::Busy:
        code = util::ErrorCode::Busy;
        break;
    case FileErrorCode::NotFound:
        code = util::ErrorCode::Validation;
        break;
    case FileErrorCode::PermissionDenied:
        code = util::ErrorCode::Workspace;
        break;
    case FileErrorCode::NotDirectory:
    case FileErrorCode::IsDirectory:
    case FileErrorCode::Invalid:
        code = util::ErrorCode::Validation;
        break;
    case FileErrorCode::NotSupported:
        code = util::ErrorCode::Unknown;
        break;
    case FileErrorCode::Unknown:
        code = util::ErrorCode::Unknown;
        break;
    }
    return util::make_error(code, e.message, e.message, e.path);
}

[[nodiscard]] inline util::Error to_util_error(ExecutionError e) {
    auto code = util::ErrorCode::Process;
    switch (e.code) {
    case ExecutionErrorCode::Aborted:
        code = util::ErrorCode::Cancelled;
        break;
    case ExecutionErrorCode::Busy:
        code = util::ErrorCode::Busy;
        break;
    case ExecutionErrorCode::Timeout:
        code = util::ErrorCode::Timeout;
        break;
    case ExecutionErrorCode::ShellUnavailable:
        code = util::ErrorCode::Process;
        break;
    case ExecutionErrorCode::SpawnError:
        code = util::ErrorCode::Process;
        break;
    case ExecutionErrorCode::CallbackError:
        code = util::ErrorCode::Process;
        break;
    case ExecutionErrorCode::NotSupported:
        code = util::ErrorCode::Unknown;
        break;
    case ExecutionErrorCode::Unknown:
        code = util::ErrorCode::Unknown;
        break;
    }
    return util::make_error(code, e.message, e.message);
}

// ---------------------------------------------------------------------------
// AsyncExecutionEnv — public capability seam
// ---------------------------------------------------------------------------

/// Complete filesystem and shell capability contract (ADR 0006). Every
/// operation is required: capability availability is decided during
/// session/tool assembly, not discovered after a method is invoked.
class AsyncExecutionEnv {
public:
    virtual ~AsyncExecutionEnv() = default;

    /// Root workspace path of the environment.
    [[nodiscard]] virtual const std::filesystem::path& workspace() const = 0;

    // -- pi-shaped filesystem methods -----------------------------------------

    /// Return an absolute addressed path without requiring it to exist and without following symlinks.
    [[nodiscard]] virtual support::AsyncResult<std::string, FileError> absolutePath(
        std::string path,
        std::stop_token stop_token) = 0;

    /// Join path segments without requiring the result to exist.
    [[nodiscard]] virtual support::AsyncResult<std::string, FileError> joinPath(
        std::vector<std::string> parts,
        std::stop_token stop_token) = 0;

    /// Read entire UTF-8 text file.
    [[nodiscard]] virtual support::AsyncResult<std::string, FileError> readTextFile(
        std::string path,
        std::stop_token stop_token) = 0;

    /// Read UTF-8 text lines. Stops after maxLines if set.
    [[nodiscard]] virtual support::AsyncResult<std::vector<std::string>, FileError> readTextLines(
        std::string path,
        std::optional<int> maxLines,
        std::stop_token stop_token) = 0;

    /// Read entire binary file.
    [[nodiscard]] virtual support::AsyncResult<BinaryData, FileError> readBinaryFile(
        std::string path,
        std::stop_token stop_token) = 0;

    /// Create or overwrite a file, creating parent directories.
    [[nodiscard]] virtual support::AsyncResult<void, FileError> writeFile(
        std::string path,
        WriteContent content,
        std::stop_token stop_token) = 0;

    /// Create or append to a file, creating parent directories.
    [[nodiscard]] virtual support::AsyncResult<void, FileError> appendFile(
        std::string path,
        WriteContent content,
        std::stop_token stop_token) = 0;

    /// Return metadata for the addressed path without following symlinks.
    [[nodiscard]] virtual support::AsyncResult<FileInfo, FileError> fileInfo(
        std::string path,
        std::stop_token stop_token) = 0;

    /// List direct children of a directory without following symlinks.
    [[nodiscard]] virtual support::AsyncResult<std::vector<FileInfo>, FileError> listDir(
        std::string path,
        std::stop_token stop_token) = 0;

    /// Return canonical path for an existing path, resolving symlinks.
    [[nodiscard]] virtual support::AsyncResult<std::string, FileError> canonicalPath(
        std::string path,
        std::stop_token stop_token) = 0;

    /// Return false for missing paths. Other errors return FileError.
    [[nodiscard]] virtual support::AsyncResult<bool, FileError> exists(
        std::string path,
        std::stop_token stop_token) = 0;

    /// Create a directory, recursively when requested.
    [[nodiscard]] virtual support::AsyncResult<void, FileError> createDir(
        std::string path,
        bool recursive,
        std::stop_token stop_token) = 0;

    /// Remove a file or directory, recursively when requested.
    [[nodiscard]] virtual support::AsyncResult<void, FileError> remove(
        std::string path,
        bool recursive,
        std::stop_token stop_token) = 0;

    /// Create a workspace-contained temporary directory and return its addressed path.
    [[nodiscard]] virtual support::AsyncResult<std::string, FileError> createTempDir(
        std::optional<std::string> prefix,
        std::stop_token stop_token) = 0;

    /// Create a workspace-contained temporary file and return its addressed path.
    [[nodiscard]] virtual support::AsyncResult<std::string, FileError> createTempFile(
        std::optional<std::string> prefix,
        std::optional<std::string> suffix,
        std::stop_token stop_token) = 0;

    /// Release owned resources. Must be best-effort and must not throw.
    [[nodiscard]] virtual support::AsyncResult<void, FileError> cleanup() {
        return support::AsyncResult<void, FileError>{std::expected<void, FileError>{}};
    }

    // -- pi-shaped shell method ------------------------------------------------

    /// Execute a shell command with split streams.
    [[nodiscard]] virtual support::AsyncResult<ShellExecResult, ExecutionError> exec(
        std::string command,
        ExecOptions options = {}) = 0;
};

} // namespace cch::harness
