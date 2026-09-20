#pragma once

#include <cch/agent/harness/FileSystem.hpp>

#include <cerrno>
#include <optional>
#include <string>

namespace cch::harness {

/// Shared errno classification for the pi-shaped filesystem operations
/// (issue #702): inaccessible paths report PermissionDenied instead of
/// folding into Invalid or NotFound.
[[nodiscard]] inline bool is_permission_errno(int errno_value) noexcept {
    return errno_value == EACCES || errno_value == EPERM;
}

/// The honest outcome for a path the process may not resolve.
[[nodiscard]] inline FileError permission_denied_error(const std::string& path) {
    return FileError{
            .code = FileErrorCode::PermissionDenied,
            .message = "permission denied: " + path,
            .path = path,
    };
}

[[nodiscard]] inline FileError path_not_found_error(const std::string& path) {
    return FileError{
            .code = FileErrorCode::NotFound,
            .message = "path not found: " + path,
            .path = std::string{path},
    };
}

[[nodiscard]] inline FileError operation_aborted_error(const std::string& path) {
    return FileError{
            .code = FileErrorCode::Aborted,
            .message = "Operation aborted",
            .path = std::string{path},
    };
}

/// The fixed file-byte cap behind reads of whole files.
[[nodiscard]] inline FileError file_result_limit_error(const std::string& path) {
    return FileError{
            .code = FileErrorCode::ResourceLimit,
            .message = "file exceeds the filesystem result limit",
            .path = std::string{path},
    };
}

/// The per-line and result-byte caps behind `readTextLines`.
[[nodiscard]] inline FileError text_line_result_limit_error(const std::string& path) {
    return FileError{
            .code = FileErrorCode::ResourceLimit,
            .message = "text-line result exceeds the filesystem result limit",
            .path = std::string{path},
    };
}

} // namespace cch::harness
