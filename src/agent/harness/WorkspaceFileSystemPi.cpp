#include "WorkspaceFileSystem.hpp"

#include <algorithm>
#include <chrono>
#include <dirent.h>
#include <sstream>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace cch::harness {

std::expected<std::string, FileError> WorkspaceFileSystem::absolutePath(const std::string& path) const {
    auto resolved = resolve_addressed_path(path);
    if (!resolved) {
        return std::unexpected(util_error_to_file_error(resolved.error(), path));
    }
    return resolved->string();
}

std::expected<std::string, FileError> WorkspaceFileSystem::joinPath(
    const std::vector<std::string>& parts) const {
    std::filesystem::path result = root_;
    for (const auto& part : parts) {
        result /= part;
    }
    auto rel = result.lexically_normal().lexically_relative(root_);
    if (rel.is_absolute() || (!rel.empty() && *rel.begin() == "..")) {
        return std::unexpected(FileError{FileErrorCode::Invalid, "path escapes workspace", std::nullopt});
    }
    return result.string();
}

std::expected<std::string, FileError> WorkspaceFileSystem::readTextFile(const std::string& path) const {
    return read_existing_file_bounded(path, kFileSystemCapacity.max_file_bytes);
}

std::expected<std::vector<std::string>, FileError> WorkspaceFileSystem::readTextLines(
    const std::string& path,
    std::optional<int> maxLines) const {
    auto content = read_existing_file_bounded(path, kFileSystemCapacity.max_file_bytes);
    if (!content) {
        return std::unexpected(content.error());
    }

    const auto requested_lines = maxLines && *maxLines > 0
        ? static_cast<std::size_t>(*maxLines)
        : maxLines
            ? std::size_t{0}
            : kFileSystemCapacity.max_text_lines;
    const auto line_limit = std::min(requested_lines, kFileSystemCapacity.max_text_lines);
    std::vector<std::string> lines;
    lines.reserve(line_limit);
    if (line_limit == 0) {
        return lines;
    }
    std::istringstream input(*content);
    std::string line;
    std::size_t result_bytes{0};
    while (std::getline(input, line)) {
        if (lines.size() >= line_limit) {
            if (maxLines && *maxLines >= 0 && requested_lines <= kFileSystemCapacity.max_text_lines) {
                break;
            }
            return std::unexpected(FileError{
                .code = FileErrorCode::ResourceLimit,
                .message = "text-line result exceeds the filesystem result limit",
                .path = std::string{path},
            });
        }
        if (line.size() > kFileSystemCapacity.max_text_lines_result_bytes - result_bytes) {
            return std::unexpected(FileError{
                .code = FileErrorCode::ResourceLimit,
                .message = "text-line result exceeds the filesystem result limit",
                .path = std::string{path},
            });
        }
        result_bytes += line.size();
        lines.push_back(std::move(line));
    }
    return lines;
}

std::expected<BinaryData, FileError> WorkspaceFileSystem::readBinaryFile(const std::string& path) const {
    auto content = read_existing_file_bounded(path, kFileSystemCapacity.max_file_bytes);
    if (!content) {
        return std::unexpected(content.error());
    }
    BinaryData result;
    result.reserve(content->size());
    for (char ch : *content) {
        result.push_back(static_cast<std::byte>(ch));
    }
    return result;
}

std::expected<void, FileError> WorkspaceFileSystem::writeFile(
    const std::string& path,
    const WriteContent& content) const {
    std::string data;
    if (std::holds_alternative<std::string>(content)) {
        data = std::get<std::string>(content);
    } else {
        const auto& bin = std::get<BinaryData>(content);
        data.assign(reinterpret_cast<const char*>(bin.data()), bin.size());
    }
    auto result = write_file(path, data, true);
    if (!result) {
        return std::unexpected(util_error_to_file_error(result.error(), path));
    }
    return {};
}

std::expected<void, FileError> WorkspaceFileSystem::appendFile(
    const std::string& path,
    const WriteContent& content) const {
    std::string data;
    if (std::holds_alternative<std::string>(content)) {
        data = std::get<std::string>(content);
    } else {
        const auto& bin = std::get<BinaryData>(content);
        data.assign(reinterpret_cast<const char*>(bin.data()), bin.size());
    }

    const auto limit = kFileSystemCapacity.max_file_bytes;
    auto present = exists(path);
    if (!present) {
        return std::unexpected(present.error());
    }

    std::string combined;
    if (*present) {
        auto existing = read_existing_file_bounded(path, limit);
        if (!existing) {
            return std::unexpected(existing.error());
        }
        if (data.size() > limit - existing->size()) {
            return std::unexpected(FileError{
                    .code = FileErrorCode::ResourceLimit,
                    .message = "append would exceed the filesystem result limit",
                    .path = std::string{path},
            });
        }
        combined.reserve(existing->size() + data.size());
        combined = std::move(*existing);
        combined.append(data);
    } else {
        if (data.size() > limit) {
            return std::unexpected(FileError{
                    .code = FileErrorCode::ResourceLimit,
                    .message = "append would exceed the filesystem result limit",
                    .path = std::string{path},
            });
        }
        combined = std::move(data);
    }

    auto result = write_file(path, combined, true);
    if (!result) {
        return std::unexpected(util_error_to_file_error(result.error(), path));
    }
    return {};
}

std::expected<FileInfo, FileError> WorkspaceFileSystem::fileInfo(const std::string& path) const {
    auto resolved = resolve_addressed_path(path);
    if (!resolved) {
        return std::unexpected(util_error_to_file_error(resolved.error(), path));
    }

    struct stat st {};
    if (*resolved == root_) {
        auto root_fd = open_workspace_root();
        if (!root_fd || ::fstat(root_fd->get(), &st) != 0) {
            return std::unexpected(FileError{
                FileErrorCode::NotFound,
                "path not found: " + path,
                std::string{path}});
        }
    } else {
        auto parent_fd = open_parent_directory(*resolved, false);
        if (!parent_fd) {
            return std::unexpected(util_error_to_file_error(parent_fd.error(), path));
        }
        const auto filename = resolved->filename().string();
        if (::fstatat(parent_fd->get(), filename.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
            if (errno == ENOENT) {
                return std::unexpected(
                        FileError{FileErrorCode::NotFound, "path not found: " + path, std::string{path}});
            }
            if (errno == EACCES) {
                return std::unexpected(
                        FileError{FileErrorCode::PermissionDenied, "permission denied: " + path, std::string{path}});
            }
            return std::unexpected(FileError{FileErrorCode::Unknown, "could not stat: " + path, std::string{path}});
        }
    }

    FileKind kind = FileKind::File;
    if (S_ISDIR(st.st_mode)) {
        kind = FileKind::Directory;
    } else if (S_ISLNK(st.st_mode)) {
        kind = FileKind::Symlink;
    }

    auto name = resolved->filename().string();
    auto mtimeMs = static_cast<std::int64_t>(st.st_mtim.tv_sec) * 1000 +
                   static_cast<std::int64_t>(st.st_mtim.tv_nsec) / 1'000'000;
    return FileInfo{
        .name = name.empty() ? resolved->string() : name,
        .path = resolved->string(),
        .kind = kind,
        .size = static_cast<std::uint64_t>(st.st_size),
        .mtimeMs = mtimeMs,
    };
}

std::expected<std::vector<FileInfo>, FileError> WorkspaceFileSystem::listDir(const std::string& path) const {
    auto resolved = resolve_addressed_path(path);
    if (!resolved) {
        return std::unexpected(util_error_to_file_error(resolved.error(), path));
    }

    support::UniqueFd directory_fd;
    if (*resolved == root_) {
        auto root_fd = open_workspace_root();
        if (!root_fd) {
            return std::unexpected(util_error_to_file_error(root_fd.error(), path));
        }
        directory_fd = std::move(*root_fd);
    } else {
        auto parent_fd = open_parent_directory(*resolved, false);
        if (!parent_fd) {
            return std::unexpected(util_error_to_file_error(parent_fd.error(), path));
        }
        const auto filename = resolved->filename().string();
        struct stat status{};
        if (::fstatat(parent_fd->get(), filename.c_str(), &status, AT_SYMLINK_NOFOLLOW) != 0) {
            if (errno == ENOENT) {
                return std::unexpected(
                        FileError{FileErrorCode::NotFound, "path not found: " + path, std::string{path}});
            }
            return std::unexpected(FileError{
                    FileErrorCode::PermissionDenied, "could not inspect directory: " + path, std::string{path}});
        }
        if (!S_ISDIR(status.st_mode)) {
            return std::unexpected(
                    FileError{FileErrorCode::NotDirectory, "not a directory: " + path, std::string{path}});
        }
        directory_fd.reset(
                ::openat(parent_fd->get(), filename.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (!directory_fd) {
            return std::unexpected(
                    FileError{FileErrorCode::PermissionDenied, "could not open directory: " + path, std::string{path}});
        }
    }

    const int directory_number = directory_fd.release();
    DIR* directory = ::fdopendir(directory_number);
    if (!directory) {
        support::UniqueFd failed_directory(directory_number);
        return std::unexpected(
                FileError{FileErrorCode::PermissionDenied, "could not read directory: " + path, std::string{path}});
    }
    const int listing_fd = ::dirfd(directory);
    if (listing_fd < 0) {
        (void)::closedir(directory);
        return std::unexpected(
                FileError{FileErrorCode::PermissionDenied, "could not read directory: " + path, std::string{path}});
    }

    std::vector<FileInfo> results;
    results.reserve(kFileSystemCapacity.max_directory_entries);
    std::size_t result_bytes{0};
    bool read_failed = false;
    for (;;) {
        errno = 0;
        const auto* entry = ::readdir(directory);
        if (!entry) {
            read_failed = errno != 0;
            break;
        }
        if (entry->d_name[0] == '.' &&
                (entry->d_name[1] == '\0' || (entry->d_name[1] == '.' && entry->d_name[2] == '\0'))) {
            continue;
        }

        struct stat status{};
        if (::fstatat(listing_fd, entry->d_name, &status, AT_SYMLINK_NOFOLLOW) != 0) {
            continue;
        }

        FileKind kind = FileKind::File;
        if (S_ISDIR(status.st_mode)) {
            kind = FileKind::Directory;
        } else if (S_ISLNK(status.st_mode)) {
            kind = FileKind::Symlink;
        }

        const std::string child_name{entry->d_name};
        const auto child_path = (*resolved / child_name).string();
        const auto child_bytes = sizeof(FileInfo) + child_name.size() + child_path.size();
        if (results.size() >= kFileSystemCapacity.max_directory_entries ||
            child_bytes > kFileSystemCapacity.max_directory_result_bytes - result_bytes) {
            (void)::closedir(directory);
            return std::unexpected(FileError{
                .code = FileErrorCode::ResourceLimit,
                .message = "directory result exceeds the filesystem result limit",
                .path = std::string{path},
            });
        }

        result_bytes += child_bytes;
        results.push_back(FileInfo{
                .name = child_name,
                .path = child_path,
                .kind = kind,
                .size = static_cast<std::uint64_t>(status.st_size),
                .mtimeMs = static_cast<std::int64_t>(status.st_mtim.tv_sec) * 1000 +
                           static_cast<std::int64_t>(status.st_mtim.tv_nsec) / 1'000'000,
        });
    }
    const bool close_failed = ::closedir(directory) != 0;
    if (read_failed || close_failed) {
        return std::unexpected(
                FileError{FileErrorCode::Unknown, "could not read directory: " + path, std::string{path}});
    }
    return results;
}

std::expected<std::string, FileError> WorkspaceFileSystem::canonicalPath(const std::string& path) const {
    auto resolved = resolve_addressed_path(path);
    if (!resolved) {
        return std::unexpected(util_error_to_file_error(resolved.error(), path));
    }
    std::error_code ec;
    auto canonical = std::filesystem::canonical(*resolved, ec);
    if (ec) {
        return std::unexpected(FileError{
            FileErrorCode::NotFound,
            "could not canonicalize: " + path,
            std::string{path}});
    }
    if (!inside_lexically(canonical)) {
        return std::unexpected(FileError{
            FileErrorCode::Invalid,
            "canonical path escapes workspace: " + path,
            std::string{path}});
    }
    return canonical.string();
}

std::expected<bool, FileError> WorkspaceFileSystem::exists(const std::string& path) const {
    auto resolved = resolve_addressed_path(path);
    if (!resolved) {
        return std::unexpected(util_error_to_file_error(resolved.error(), path));
    }

    if (*resolved == root_) {
        auto root_fd = open_workspace_root();
        if (!root_fd) {
            return std::unexpected(util_error_to_file_error(root_fd.error(), path));
        }
        return true;
    }

    int parent_errno = 0;
    auto parent_fd = open_parent_directory(*resolved, false, &parent_errno);
    if (!parent_fd) {
        if (parent_errno == ENOENT) {
            return false;
        }
        return std::unexpected(util_error_to_file_error(parent_fd.error(), path));
    }

    struct stat status{};
    const auto filename = resolved->filename().string();
    if (::fstatat(parent_fd->get(), filename.c_str(), &status, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno == ENOENT || errno == ENOTDIR) {
            return false;
        }
        return std::unexpected(FileError{
            FileErrorCode::Invalid,
            "could not check path: " + path,
            std::string{path}});
    }
    return true;
}

std::expected<void, FileError> WorkspaceFileSystem::createDir(
    const std::string& path,
    bool recursive) const {
    auto resolved = resolve_addressed_path(path);
    if (!resolved) {
        return std::unexpected(util_error_to_file_error(resolved.error(), path));
    }
    if (*resolved == root_) {
        return {};
    }

    auto parent_fd = open_parent_directory(*resolved, recursive);
    if (!parent_fd) {
        return std::unexpected(util_error_to_file_error(parent_fd.error(), path));
    }
    const auto filename = resolved->filename().string();
    if (::mkdirat(parent_fd->get(), filename.c_str(), 0755) != 0) {
        if (errno == EEXIST) {
            return {};
        }
        return std::unexpected(FileError{
            FileErrorCode::Invalid,
            "could not create directory: " + std::string(std::strerror(errno)),
            std::string{path}});
    }
    return {};
}

std::expected<void, FileError> WorkspaceFileSystem::remove(
    const std::string& path,
    bool recursive) const {
    auto resolved = resolve_addressed_path(path);
    if (!resolved) {
        return std::unexpected(util_error_to_file_error(resolved.error(), path));
    }
    if (*resolved == root_) {
        return std::unexpected(FileError{
            FileErrorCode::Invalid,
            "cannot remove workspace root",
            std::string{path}});
    }

    auto parent_fd = open_parent_directory(*resolved, false);
    if (!parent_fd) {
        return std::unexpected(util_error_to_file_error(parent_fd.error(), path));
    }

    const auto filename = resolved->filename().string();
    struct stat status{};
    if (::fstatat(parent_fd->get(), filename.c_str(), &status, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno == ENOENT) {
            return std::unexpected(FileError{FileErrorCode::NotFound, "path not found: " + path, std::string{path}});
        }
        return std::unexpected(
                FileError{FileErrorCode::PermissionDenied, "could not inspect path: " + path, std::string{path}});
    }

    if (S_ISDIR(status.st_mode) && recursive) {
        support::UniqueFd directory_fd(
                ::openat(parent_fd->get(), filename.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (!directory_fd) {
            if (errno == ENOENT) {
                return std::unexpected(
                        FileError{FileErrorCode::NotFound, "path not found: " + path, std::string{path}});
            }
            return std::unexpected(
                    FileError{FileErrorCode::PermissionDenied, "could not open directory: " + path, std::string{path}});
        }
        if (!remove_directory_contents(directory_fd.get())) {
            return std::unexpected(FileError{FileErrorCode::PermissionDenied,
                    "could not remove directory contents: " + path,
                    std::string{path}});
        }
        if (::unlinkat(parent_fd->get(), filename.c_str(), AT_REMOVEDIR) != 0) {
            return std::unexpected(FileError{
                    FileErrorCode::PermissionDenied, "could not remove directory: " + path, std::string{path}});
        }
        return {};
    }

    const int flags = S_ISDIR(status.st_mode) ? AT_REMOVEDIR : 0;
    if (::unlinkat(parent_fd->get(), filename.c_str(), flags) != 0) {
        return std::unexpected(FileError{FileErrorCode::PermissionDenied,
                "could not remove: " + std::string(std::strerror(errno)),
                std::string{path}});
    }
    return {};
}

} // namespace cch::harness
