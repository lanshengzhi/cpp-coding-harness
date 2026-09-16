#include "WorkspaceFileSystem.hpp"

#include "AtomicWrite.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace cch::harness {

WorkspaceFileSystem::WorkspaceFileSystem() : temporary_state_(std::make_shared<TemporaryState>()) {}

WorkspaceFileSystem::WorkspaceFileSystem(
        std::filesystem::path workspace, std::shared_ptr<const AuthorizedSkillRoots> skill_read_roots)
    : root_(canonicalized(std::move(workspace))), skill_read_roots_(std::move(skill_read_roots)),
      temporary_state_(std::make_shared<TemporaryState>()) {}

support::Expected<WorkspaceFileSystem> WorkspaceFileSystem::create(const std::filesystem::path& workspace) {
    std::error_code ec;
    if (!std::filesystem::exists(workspace, ec) || !std::filesystem::is_directory(workspace, ec)) {
        return std::unexpected(workspace_error("workspace does not exist or is not a directory"));
    }
    return WorkspaceFileSystem(workspace);
}

support::Expected<std::filesystem::path> WorkspaceFileSystem::resolve_addressed_path(
        const std::string& requested) const {
    if (requested.empty()) {
        return std::unexpected(workspace_error(
                "path is required; use a workspace-relative path, or an absolute path under " + root_.string()));
    }
    if (requested.find('\0') != std::string::npos) {
        return std::unexpected(workspace_error("NUL bytes are not allowed in paths"));
    }
    std::filesystem::path queried(requested);
    if (queried.is_absolute()) {
        // Absolute paths are accepted when they normalize to a location inside
        // the workspace root. Anything else stays rejected: the open-time
        // symlink guards below still apply to accepted paths.
        auto target = queried.lexically_normal();
        if (target != root_ && target.filename().empty()) {
            target = target.parent_path();
        }
        if (!inside_lexically(target)) {
            return std::unexpected(
                    workspace_error("path is outside the workspace: " + requested +
                                    "; use a workspace-relative path, or an absolute path under " + root_.string()));
        }
        return target;
    }
    auto normalized = queried.lexically_normal();
    for (const auto& part : normalized) {
        if (part == "..") {
            return std::unexpected(
                    workspace_error("path escapes workspace: " + requested +
                                    "; use a workspace-relative path, or an absolute path under " + root_.string()));
        }
    }
    if (normalized == ".") {
        return root_;
    }
    auto target = (root_ / normalized).lexically_normal();
    if (target != root_ && target.filename().empty()) {
        target = target.parent_path();
    }
    if (!inside_lexically(target)) {
        return std::unexpected(
                workspace_error("path escapes workspace: " + requested +
                                "; use a workspace-relative path, or an absolute path under " + root_.string()));
    }
    return target;
}

std::expected<std::string, FileError> WorkspaceFileSystem::read_text_file_for_write(
        const std::string& path, std::stop_token stop_token) const {
    if (stop_token.stop_requested()) {
        return std::unexpected(FileError{
                .code = FileErrorCode::Aborted,
                .message = "Operation aborted",
                .path = std::string{path},
        });
    }

    auto target = resolve_write_path(path);
    if (!target) {
        return std::unexpected(util_error_to_file_error(target.error(), path));
    }

    int parent_errno = 0;
    auto parent_guard = open_parent_directory(*target, false, &parent_errno);
    if (!parent_guard) {
        if (parent_errno == ENOENT) {
            return std::unexpected(FileError{
                    .code = FileErrorCode::NotFound,
                    .message = "path not found: " + path,
                    .path = std::string{path},
            });
        }
        return std::unexpected(util_error_to_file_error(parent_guard.error(), path));
    }

    std::uintmax_t file_size = 0;
    auto fd = open_regular_file_in_parent(parent_guard->get(), target->filename().string(), path, &file_size);
    if (!fd) {
        return std::unexpected(fd.error());
    }
    return read_bounded_from_open_file(fd->get(), file_size, path, kFileSystemCapacity.max_file_bytes, stop_token);
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

support::Expected<std::filesystem::path> WorkspaceFileSystem::resolve_write_path(const std::string& requested) const {
    if (requested.empty()) {
        return std::unexpected(workspace_error("path is required; use a workspace-relative path or an absolute path"));
    }
    if (requested.find('\0') != std::string::npos) {
        return std::unexpected(workspace_error("NUL bytes are not allowed in paths"));
    }
    // pi resolveToCwd semantics (#619): normalize unicode spaces, strip a
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

const std::filesystem::path* WorkspaceFileSystem::authorizing_skill_root(
        const std::filesystem::path& target, const std::vector<std::filesystem::path>& roots) const {
    const std::filesystem::path* best = nullptr;
    std::size_t best_length = 0;
    for (const auto& root : roots) {
        const auto relative = target.lexically_relative(root);
        if (relative.empty() || relative == "." || relative.is_absolute()) {
            continue;
        }
        if (*relative.begin() == "..") {
            continue;
        }
        // Longest authorizing root wins when roots nest.
        const auto length = root.string().size();
        if (best == nullptr || length > best_length) {
            best = &root;
            best_length = length;
        }
    }
    return best;
}

support::Expected<std::filesystem::path> WorkspaceFileSystem::resolve_read_path(const std::string& requested) const {
    if (requested.find('\0') != std::string::npos) {
        return std::unexpected(workspace_error("NUL bytes are not allowed in paths"));
    }
    if (auto addressed = resolve_addressed_path(requested); addressed) {
        return addressed;
    } else {
        auto error = std::move(addressed.error());
        const std::filesystem::path queried(requested);
        if (!queried.is_absolute()) {
            return std::unexpected(std::move(error));
        }
        auto target = queried.lexically_normal();
        if (target.filename().empty()) {
            target = target.parent_path();
        }
        if (skill_read_roots_ != nullptr && authorizing_skill_root(target, *skill_read_roots_->snapshot()) != nullptr) {
            return target;
        }
        return std::unexpected(
                workspace_error("path is outside the workspace and no loaded skill directory: " + requested +
                                "; use a workspace-relative path, an absolute path under " + root_.string() +
                                ", or an absolute path under a loaded skill directory"));
    }
}

std::expected<support::UniqueFd, FileError> WorkspaceFileSystem::open_regular_file_for_read(
        const std::string& requested, std::uintmax_t* size, std::stop_token stop_token) const {
    if (stop_token.stop_requested()) {
        return std::unexpected(FileError{
                .code = FileErrorCode::Aborted,
                .message = "Operation aborted",
                .path = std::string{requested},
        });
    }

    auto target = resolve_read_path(requested);
    if (!target) {
        return std::unexpected(util_error_to_file_error(target.error(), requested));
    }

    int parent_errno = 0;
    support::Expected<support::UniqueFd> parent_guard = inside_lexically(*target)
                                                                ? open_parent_directory(*target, false, &parent_errno)
                                                                : open_authorized_skill_parent(*target, &parent_errno);
    if (!parent_guard) {
        if (parent_errno == ENOENT) {
            return std::unexpected(FileError{
                    .code = FileErrorCode::NotFound,
                    .message = "path not found: " + requested,
                    .path = std::string{requested},
            });
        }
        return std::unexpected(util_error_to_file_error(parent_guard.error(), requested));
    }

    const auto filename = target->filename().string();
    return open_regular_file_in_parent(parent_guard->get(), filename, requested, size);
}

std::expected<support::UniqueFd, FileError> WorkspaceFileSystem::open_regular_file_in_parent(
        int parent_fd, const std::string& filename, const std::string& requested, std::uintmax_t* size) const {
    struct stat status{};
    if (::fstatat(parent_fd, filename.c_str(), &status, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno == ENOENT) {
            return std::unexpected(FileError{
                    .code = FileErrorCode::NotFound,
                    .message = "path not found: " + requested,
                    .path = std::string{requested},
            });
        }
        return std::unexpected(FileError{
                .code = FileErrorCode::PermissionDenied,
                .message = "could not inspect file for reading: " + requested,
                .path = std::string{requested},
        });
    }
    if (S_ISLNK(status.st_mode)) {
        return std::unexpected(FileError{
                .code = FileErrorCode::PermissionDenied,
                .message = "refusing to read through symlink: " + requested,
                .path = std::string{requested},
        });
    }
    if (!S_ISREG(status.st_mode)) {
        return std::unexpected(FileError{
                .code = FileErrorCode::IsDirectory,
                .message = "path is not a regular file: " + requested,
                .path = std::string{requested},
        });
    }

    // O_NONBLOCK is harmless for regular files and closes the FIFO race
    // between the no-follow type check and openat.
    support::UniqueFd fd(::openat(parent_fd, filename.c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
    if (!fd) {
        if (errno == ELOOP) {
            return std::unexpected(FileError{
                    .code = FileErrorCode::PermissionDenied,
                    .message = "refusing to read through symlink: " + requested,
                    .path = std::string{requested},
            });
        }
        if (errno == ENOENT) {
            return std::unexpected(FileError{
                    .code = FileErrorCode::NotFound,
                    .message = "path not found: " + requested,
                    .path = std::string{requested},
            });
        }
        return std::unexpected(FileError{
                .code = FileErrorCode::PermissionDenied,
                .message = "could not open file for reading: " + requested,
                .path = std::string{requested},
        });
    }

    if (::fstat(fd.get(), &status) != 0 || !S_ISREG(status.st_mode)) {
        return std::unexpected(FileError{
                .code = FileErrorCode::IsDirectory,
                .message = "path is not a regular file: " + requested,
                .path = std::string{requested},
        });
    }
    if (size) {
        *size = status.st_size < 0 ? 0 : static_cast<std::uintmax_t>(status.st_size);
    }
    return fd;
}

support::Expected<std::string> WorkspaceFileSystem::read_existing_file(
        const std::string& requested, std::stop_token stop_token) const {
    std::uintmax_t ignored_size = 0;
    auto fd = open_regular_file_for_read(requested, &ignored_size, stop_token);
    if (!fd) {
        return std::unexpected(workspace_error(fd.error().message));
    }

    std::string content;
    char buffer[4096];
    ssize_t n = 0;
    while ((n = ::read(fd->get(), buffer, sizeof(buffer))) > 0) {
        if (stop_token.stop_requested()) {
            return std::unexpected(workspace_error("Operation aborted"));
        }
        content.append(buffer, static_cast<std::size_t>(n));
    }
    if (n < 0) {
        return std::unexpected(workspace_error("could not read file: " + requested));
    }
    return content;
}

std::expected<std::string, FileError> WorkspaceFileSystem::read_existing_file_bounded(
        const std::string& requested, std::size_t max_bytes, std::stop_token stop_token) const {
    std::uintmax_t file_size = 0;
    auto fd = open_regular_file_for_read(requested, &file_size, stop_token);
    if (!fd) {
        return std::unexpected(fd.error());
    }
    return read_bounded_from_open_file(fd->get(), file_size, requested, max_bytes, stop_token);
}

std::expected<std::string, FileError> WorkspaceFileSystem::read_bounded_from_open_file(int file_fd,
        std::uintmax_t file_size,
        const std::string& requested,
        std::size_t max_bytes,
        std::stop_token stop_token) const {
    if (file_size > max_bytes) {
        return std::unexpected(FileError{
                .code = FileErrorCode::ResourceLimit,
                .message = "file exceeds the filesystem result limit",
                .path = std::string{requested},
        });
    }

    std::string content;
    content.reserve(static_cast<std::size_t>(file_size));
    char buffer[4096];
    ssize_t n = 0;
    while ((n = ::read(file_fd, buffer, sizeof(buffer))) > 0) {
        if (stop_token.stop_requested()) {
            return std::unexpected(FileError{
                    .code = FileErrorCode::Aborted,
                    .message = "Operation aborted",
                    .path = std::string{requested},
            });
        }
        const auto count = static_cast<std::size_t>(n);
        if (count > max_bytes - content.size()) {
            return std::unexpected(FileError{
                    .code = FileErrorCode::ResourceLimit,
                    .message = "file exceeds the filesystem result limit",
                    .path = std::string{requested},
            });
        }
        content.append(buffer, count);
    }
    if (n < 0) {
        return std::unexpected(FileError{
                .code = FileErrorCode::Unknown,
                .message = "could not read file: " + requested,
                .path = std::string{requested},
        });
    }
    return content;
}

support::Expected<std::size_t> WorkspaceFileSystem::write_file(
        const std::string& requested, std::string_view content, bool create_parents, std::stop_token stop_token) const {
    auto target = resolve_write_path(requested);
    if (!target) {
        return std::unexpected(target.error());
    }

    auto parent_guard = open_parent_directory(*target, create_parents);
    if (!parent_guard) {
        return std::unexpected(parent_guard.error());
    }

    const auto filename = target->filename().string();
    struct stat target_status{};
    if (::fstatat(parent_guard->get(), filename.c_str(), &target_status, AT_SYMLINK_NOFOLLOW) == 0) {
        if (S_ISLNK(target_status.st_mode)) {
            return std::unexpected(workspace_error("refusing to write through final symlink: " + requested));
        }
        if (!S_ISREG(target_status.st_mode)) {
            return std::unexpected(workspace_error("target is not a regular file: " + requested));
        }
    } else if (errno != ENOENT) {
        return std::unexpected(workspace_error("could not inspect target: " + requested));
    }

    auto written = write_atomic_file_at(parent_guard->get(), filename, content, stop_token);
    if (!written) {
        return std::unexpected(written.error());
    }
    return content.size();
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

bool WorkspaceFileSystem::inside(const std::filesystem::path& path) const {
    std::error_code ec;
    auto canonical = std::filesystem::weakly_canonical(path, ec);
    if (ec) {
        return false;
    }
    return inside_lexically(canonical);
}

bool WorkspaceFileSystem::inside_lexically(const std::filesystem::path& path) const {
    auto rel = path.lexically_normal().lexically_relative(root_);
    if (rel.empty() || rel == ".") {
        return true;
    }
    if (rel.is_absolute()) {
        return false;
    }
    auto first = rel.begin();
    return first == rel.end() || *first != "..";
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

} // namespace cch::harness
