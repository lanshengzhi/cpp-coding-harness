// Path normalization and resolution: the single pi `resolveToCwd` seam
// (ADR 0057), the pi-shaped path queries built on it, and the root and
// error helpers every filesystem operation reports through.
#include "WorkspaceFileSystem.hpp"

#include "WorkspaceFileSystemErrors.hpp"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace cch::harness {
WorkspaceFileSystem::WorkspaceFileSystem() : temporary_state_(std::make_shared<TemporaryState>()) {}

WorkspaceFileSystem::WorkspaceFileSystem(std::filesystem::path workspace)
    : root_(canonicalized(std::move(workspace))), temporary_state_(std::make_shared<TemporaryState>()) {}

support::Expected<WorkspaceFileSystem> WorkspaceFileSystem::create(const std::filesystem::path& workspace) {
    std::error_code ec;
    if (!std::filesystem::exists(workspace, ec) || !std::filesystem::is_directory(workspace, ec)) {
        return std::unexpected(workspace_error("workspace does not exist or is not a directory"));
    }
    return WorkspaceFileSystem(workspace);
}

namespace {

/// pi `utils/paths.ts` UNICODE_SPACES (verbatim set): no-break and narrow
/// no-break style spaces normalize to an ASCII space before resolution.
[[nodiscard]] std::string normalize_unicode_spaces(std::string_view input) {
    std::string out;
    out.reserve(input.size());
    for (std::size_t i = 0; i < input.size();) {
        const auto byte = static_cast<unsigned char>(input[i]);
        if (byte == 0xC2 && i + 1 < input.size() && static_cast<unsigned char>(input[i + 1]) == 0xA0) {
            out += ' ';
            i += 2;
            continue;
        }
        if (byte == 0xE2 && i + 2 < input.size()) {
            const auto b1 = static_cast<unsigned char>(input[i + 1]);
            const auto b2 = static_cast<unsigned char>(input[i + 2]);
            if ((b1 == 0x80 && ((b2 >= 0x80 && b2 <= 0x8A) || b2 == 0xAF)) || (b1 == 0x81 && b2 == 0x9F)) {
                out += ' ';
                i += 3;
                continue;
            }
        }
        if (byte == 0xE3 && i + 2 < input.size() && static_cast<unsigned char>(input[i + 1]) == 0x80 &&
                static_cast<unsigned char>(input[i + 2]) == 0x80) {
            out += ' ';
            i += 3;
            continue;
        }
        out += input[i++];
    }
    return out;
}
} // namespace

support::Expected<std::filesystem::path> WorkspaceFileSystem::resolve_to_cwd(const std::string& requested) const {
    if (requested.empty()) {
        return std::unexpected(workspace_error("path is required; use a workspace-relative path or an absolute path"));
    }
    if (requested.find('\0') != std::string::npos) {
        return std::unexpected(workspace_error("NUL bytes are not allowed in paths"));
    }
    // pi resolveToCwd semantics (ADR 0057): normalize unicode spaces, strip a
    // leading "@" mention prefix, and expand "~" against $HOME (pi
    // normalizePath), then honor absolute paths after lexical normalization
    // regardless of containment; relative paths resolve against the
    // workspace root with ".." handled by normalization rather than
    // rejection. pi's file:// URL conversion is not mirrored: URLs are not
    // paths on this seam. The open-time no-follow symlink guards still apply.
    std::string preprocessed = normalize_unicode_spaces(requested);
    if (preprocessed.starts_with('@')) {
        preprocessed.erase(0, 1);
    }
    if (preprocessed == "~" || preprocessed.starts_with("~/")) {
        if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
            preprocessed = home + preprocessed.substr(1);
        }
    }
    const std::filesystem::path queried(preprocessed);
    auto target = queried.is_absolute() ? queried.lexically_normal() : (root_ / queried).lexically_normal();
    if (target != root_ && target.filename().empty()) {
        target = target.parent_path();
    }
    return target;
}

std::expected<std::string, FileError> WorkspaceFileSystem::absolutePath(const std::string& path) const {
    auto resolved = resolve_to_cwd(path);
    if (!resolved) {
        return std::unexpected(util_error_to_file_error(resolved.error(), path));
    }
    return resolved->string();
}

std::expected<std::string, FileError> WorkspaceFileSystem::joinPath(const std::vector<std::string>& parts) const {
    std::filesystem::path result = root_;
    for (const auto& part : parts) {
        if (part.find('\0') != std::string::npos) {
            return std::unexpected(FileError{
                    .code = FileErrorCode::Invalid,
                    .message = "NUL bytes are not allowed in paths",
                    .path = std::nullopt,
            });
        }
        result /= part;
    }
    // No containment (ADR 0057): segments join against the workspace root
    // and normalize lexically; results landing outside the root are
    // returned, not rejected.
    return result.lexically_normal().string();
}

std::expected<std::string, FileError> WorkspaceFileSystem::canonicalPath(const std::string& path) const {
    auto resolved = resolve_to_cwd(path);
    if (!resolved) {
        return std::unexpected(util_error_to_file_error(resolved.error(), path));
    }
    std::error_code ec;
    auto canonical = std::filesystem::canonical(*resolved, ec);
    if (ec) {
        // An unsearchable parent surfaces as a real EACCES here; report it
        // honestly instead of claiming the path is missing (issue #702).
        if (ec == std::errc::permission_denied || ec == std::errc::operation_not_permitted) {
            return std::unexpected(permission_denied_error(path));
        }
        return std::unexpected(
                FileError{FileErrorCode::NotFound, "could not canonicalize: " + path, std::string{path}});
    }
    return canonical.string();
}

std::filesystem::path WorkspaceFileSystem::canonicalized(std::filesystem::path workspace) {
    std::error_code ec;
    auto canonical = std::filesystem::weakly_canonical(workspace, ec);
    if (ec) {
        return workspace;
    }
    return canonical;
}

std::filesystem::path WorkspaceFileSystem::default_root() {
    std::error_code ec;
    auto cwd = std::filesystem::current_path(ec);
    if (ec) {
        return std::filesystem::path{"."};
    }
    return cwd;
}

support::Error WorkspaceFileSystem::workspace_error(std::string message) {
    return support::make_error(support::ErrorCode::Workspace, message, message);
}

FileError WorkspaceFileSystem::util_error_to_file_error(const support::Error& error, const std::string& path) {
    FileErrorCode code = FileErrorCode::Unknown;
    switch (error.code) {
    case support::ErrorCode::Workspace:
        code = FileErrorCode::PermissionDenied;
        break;
    case support::ErrorCode::Validation:
        code = FileErrorCode::Invalid;
        break;
    case support::ErrorCode::Cancelled:
        code = FileErrorCode::Aborted;
        break;
    case support::ErrorCode::ResourceLimit:
        code = FileErrorCode::ResourceLimit;
        break;
    default:
        break;
    }
    return FileError{code, error.message, std::string{path}};
}

} // namespace cch::harness
