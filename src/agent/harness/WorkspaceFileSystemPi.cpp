#include "WorkspaceFileSystem.hpp"

#include "WorkspaceFileSystemErrors.hpp"
#include "WorkspaceFileSystemIo.hpp"

#include "AtomicWrite.hpp"

#include <algorithm>
#include <dirent.h>
#include <limits>
#include <sstream>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace cch::harness {

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

std::expected<std::string, FileError> WorkspaceFileSystem::readTextFile(
        const std::string& path, std::stop_token stop_token) const {
    return read_existing_file_bounded(path, kFileSystemCapacity.max_file_bytes, stop_token);
}

std::expected<std::vector<std::string>, FileError> WorkspaceFileSystem::readTextLines(
        const std::string& path, std::optional<int> maxLines, std::stop_token stop_token) const {
    if (maxLines && *maxLines <= 0) {
        auto resolved = resolve_to_cwd(path);
        if (!resolved) {
            return std::unexpected(util_error_to_file_error(resolved.error(), path));
        }
        return std::vector<std::string>{};
    }

    const auto requested_lines = maxLines ? static_cast<std::size_t>(*maxLines) : kFileSystemCapacity.max_text_lines;
    const auto line_limit = std::min(requested_lines, kFileSystemCapacity.max_text_lines);
    std::uintmax_t file_size = 0;
    auto fd = open_regular_file_for_read(path, &file_size, stop_token);
    if (!fd) {
        return std::unexpected(fd.error());
    }
    if (file_size > kFileSystemCapacity.max_file_bytes) {
        return std::unexpected(file_result_limit_error(path));
    }

    std::vector<std::string> lines;
    lines.reserve(line_limit);
    std::string line;
    std::size_t result_bytes{0};
    std::size_t total_bytes{0};
    bool line_has_bytes{false};

    // One line finalizer shared by the newline and EOF boundaries: the
    // per-result byte check and the move into the result.
    const auto finish_line = [&]() -> std::expected<void, FileError> {
        if (line.size() > kFileSystemCapacity.max_text_lines_result_bytes - result_bytes) {
            return std::unexpected(text_line_result_limit_error(path));
        }
        result_bytes += line.size();
        lines.push_back(std::move(line));
        line.clear();
        line_has_bytes = false;
        return {};
    };

    auto read = read_file_chunks(
            fd->get(), path, stop_token,
            [&](const char* buffer, std::size_t bytes) -> std::expected<void, FileError> {
                if (bytes > kFileSystemCapacity.max_file_bytes - total_bytes) {
                    return std::unexpected(file_result_limit_error(path));
                }
                total_bytes += bytes;

                for (std::size_t index = 0; index < bytes; ++index) {
                    const char value = buffer[index];
                    if (value != '\n') {
                        if (lines.size() < line_limit) {
                            if (line.size() >= kFileSystemCapacity.max_text_lines_result_bytes - result_bytes) {
                                return std::unexpected(text_line_result_limit_error(path));
                            }
                            line.push_back(value);
                        }
                        line_has_bytes = true;
                        continue;
                    }

                    if (lines.size() >= line_limit) {
                        if (!maxLines || requested_lines > kFileSystemCapacity.max_text_lines) {
                            return std::unexpected(text_line_result_limit_error(path));
                        }
                        line.clear();
                        line_has_bytes = false;
                        continue;
                    }

                    if (!line.empty() && line.back() == '\r') {
                        line.pop_back();
                    }
                    if (auto finished = finish_line(); !finished) {
                        return std::unexpected(finished.error());
                    }
                }
                return {};
            });
    if (!read) {
        return std::unexpected(read.error());
    }

    if (line_has_bytes && lines.size() < line_limit) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (auto finished = finish_line(); !finished) {
            return std::unexpected(finished.error());
        }
    } else if (line_has_bytes && lines.size() >= line_limit &&
               (!maxLines || requested_lines > kFileSystemCapacity.max_text_lines)) {
        return std::unexpected(text_line_result_limit_error(path));
    }
    return lines;
}


std::expected<BinaryData, FileError> WorkspaceFileSystem::readBinaryFile(
        const std::string& path, std::stop_token stop_token) const {
    std::uintmax_t file_size = 0;
    auto fd = open_regular_file_for_read(path, &file_size, stop_token);
    if (!fd) {
        return std::unexpected(fd.error());
    }
    if (file_size > kFileSystemCapacity.max_file_bytes) {
        return std::unexpected(file_result_limit_error(path));
    }

    BinaryData result;
    result.reserve(static_cast<std::size_t>(file_size));
    std::size_t total_bytes{0};
    auto read = read_file_chunks(
            fd->get(), path, stop_token, [&](const char* buffer, std::size_t bytes) -> std::expected<void, FileError> {
                if (bytes > kFileSystemCapacity.max_file_bytes - total_bytes) {
                    return std::unexpected(file_result_limit_error(path));
                }
                total_bytes += bytes;
                result.reserve(total_bytes);
                for (std::size_t index = 0; index < bytes; ++index) {
                    result.push_back(static_cast<std::byte>(buffer[index]));
                }
                return {};
            });
    if (!read) {
        return std::unexpected(read.error());
    }
    return result;
}

std::expected<void, FileError> WorkspaceFileSystem::writeFile(
        const std::string& path, const WriteContent& content, std::stop_token stop_token) const {
    std::string_view data;
    if (std::holds_alternative<std::string>(content)) {
        data = std::get<std::string>(content);
    } else {
        const auto& bin = std::get<BinaryData>(content);
        if (!bin.empty()) {
            data = std::string_view{reinterpret_cast<const char*>(bin.data()), bin.size()};
        }
    }
    auto result = write_file(path, data, true, stop_token);
    if (!result) {
        return std::unexpected(util_error_to_file_error(result.error(), path));
    }
    return {};
}

std::expected<void, FileError> WorkspaceFileSystem::appendFile(
        const std::string& path, const WriteContent& content, std::stop_token stop_token) const {
    // Append is a read-plus-rewrite operation. Serialize it per capability so
    // concurrent admitted appends cannot both read the same old contents and
    // silently lose one another's suffix.
    std::unique_lock mutation_lock(temporary_state_->mutex);
    if (stop_token.stop_requested()) {
return std::unexpected(operation_aborted_error(path));
    }

    std::string_view data;
    if (std::holds_alternative<std::string>(content)) {
        data = std::get<std::string>(content);
    } else {
        const auto& bin = std::get<BinaryData>(content);
        if (!bin.empty()) {
            data = std::string_view{reinterpret_cast<const char*>(bin.data()), bin.size()};
        }
    }

    auto target = resolve_to_cwd(path);
    if (!target) {
        return std::unexpected(util_error_to_file_error(target.error(), path));
    }
    auto parent_guard = open_parent_directory(*target, true);
    if (!parent_guard) {
        return std::unexpected(util_error_to_file_error(parent_guard.error(), path));
    }

    const auto filename = target->filename().string();
    support::UniqueFd source;
    std::size_t source_bytes{0};
    struct stat target_status{};
    if (::fstatat(parent_guard->get(), filename.c_str(), &target_status, AT_SYMLINK_NOFOLLOW) == 0) {
        if (S_ISLNK(target_status.st_mode)) {
            return std::unexpected(FileError{
                    .code = FileErrorCode::PermissionDenied,
                    .message = "refusing to append through final symlink: " + path,
                    .path = std::string{path},
            });
        }
        if (!S_ISREG(target_status.st_mode)) {
            return std::unexpected(FileError{
                    .code = FileErrorCode::IsDirectory,
                    .message = "path is not a regular file: " + path,
                    .path = std::string{path},
            });
        }
        if (target_status.st_size < 0 ||
                static_cast<std::uintmax_t>(target_status.st_size) > kFileSystemCapacity.max_file_bytes) {
return std::unexpected(file_result_limit_error(path));
        }

        source.reset(::openat(parent_guard->get(), filename.c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
        if (!source) {
            if (errno == ELOOP) {
                return std::unexpected(FileError{
                        .code = FileErrorCode::PermissionDenied,
                        .message = "refusing to append through final symlink: " + path,
                        .path = std::string{path},
                });
            }
            return std::unexpected(FileError{
                    .code = errno == ENOENT ? FileErrorCode::NotFound : FileErrorCode::PermissionDenied,
                    .message = "could not open file for appending: " + path,
                    .path = std::string{path},
            });
        }

        if (::fstat(source.get(), &target_status) != 0 || !S_ISREG(target_status.st_mode)) {
            return std::unexpected(FileError{
                    .code = FileErrorCode::IsDirectory,
                    .message = "path is not a regular file: " + path,
                    .path = std::string{path},
            });
        }
        source_bytes = target_status.st_size < 0 ? 0 : static_cast<std::size_t>(target_status.st_size);
    } else if (errno != ENOENT) {
        return std::unexpected(FileError{
                .code = FileErrorCode::PermissionDenied,
                .message = "could not inspect target for appending: " + path,
                .path = std::string{path},
        });
    }

    if (data.size() > kFileSystemCapacity.max_file_bytes - source_bytes) {
        return std::unexpected(FileError{
                .code = FileErrorCode::ResourceLimit,
                .message = "append would exceed the filesystem result limit",
                .path = std::string{path},
        });
    }

    support::ExpectedVoid result;
    if (source) {
        result = append_atomic_file_at(parent_guard->get(), filename, source.get(), source_bytes, data, stop_token);
    } else {
        result = write_atomic_file_at(parent_guard->get(), filename, data, stop_token);
    }
    if (!result) {
        return std::unexpected(util_error_to_file_error(result.error(), path));
    }
    return {};
}

std::expected<FileInfo, FileError> WorkspaceFileSystem::fileInfo(const std::string& path) const {
    auto resolved = resolve_to_cwd(path);
    if (!resolved) {
        return std::unexpected(util_error_to_file_error(resolved.error(), path));
    }

    struct stat st{};
    // Roots (the workspace root or a filesystem root) have no addressable
    // parent; open them directly instead of the parent+filename walk.
    if (*resolved == root_ || *resolved == resolved->root_path()) {
        int root_errno = 0;
        auto root_fd = open_root_directory(*resolved, "root directory", &root_errno);
        if (!root_fd) {
            if (root_errno == ENOENT) {
return std::unexpected(path_not_found_error(path));
            }
            return std::unexpected(util_error_to_file_error(root_fd.error(), path));
        }
        if (::fstat(root_fd->get(), &st) != 0) {
            return std::unexpected(path_not_found_error(path));
        }
    } else {
        int parent_errno = 0;
        auto parent_fd = open_parent_directory(*resolved, false, &parent_errno);
        if (!parent_fd) {
            if (parent_errno == ENOENT) {
                return std::unexpected(path_not_found_error(path));
            }
            return std::unexpected(util_error_to_file_error(parent_fd.error(), path));
        }
        const auto filename = resolved->filename().string();
        if (::fstatat(parent_fd->get(), filename.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
            if (errno == ENOENT) {
                return std::unexpected(path_not_found_error(path));
            }
            if (is_permission_errno(errno)) {
                return std::unexpected(permission_denied_error(path));
            }
            return std::unexpected(FileError{FileErrorCode::Unknown, "could not stat: " + path, std::string{path}});
        }
    }

    FileKind kind = FileKind::File;
    if (S_ISDIR(st.st_mode)) {
        kind = FileKind::Directory;
    } else if (S_ISLNK(st.st_mode)) {
        kind = FileKind::Symlink;
    } else if (!S_ISREG(st.st_mode)) {
        return std::unexpected(FileError{FileErrorCode::Invalid, "Unsupported file type: " + path, std::string{path}});
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

std::expected<std::vector<FileInfo>, FileError> WorkspaceFileSystem::listDir(
        const std::string& path, std::stop_token stop_token) const {
    auto resolved = resolve_to_cwd(path);
    if (!resolved) {
        return std::unexpected(util_error_to_file_error(resolved.error(), path));
    }

    support::UniqueFd directory_fd;
    // Roots (the workspace root or a filesystem root) have no addressable
    // parent; open them directly instead of the parent+filename walk.
    if (*resolved == root_ || *resolved == resolved->root_path()) {
        if (auto root_fd = open_root_directory(*resolved); !root_fd) {
            return std::unexpected(util_error_to_file_error(root_fd.error(), path));
        } else {
            directory_fd = std::move(*root_fd);
        }
    } else {
        int parent_errno = 0;
        auto parent_fd = open_parent_directory(*resolved, false, &parent_errno);
        if (!parent_fd) {
            if (parent_errno == ENOENT) {
                return std::unexpected(path_not_found_error(path));
            }
            return std::unexpected(util_error_to_file_error(parent_fd.error(), path));
        }
        const auto filename = resolved->filename().string();
        struct stat status{};
        if (::fstatat(parent_fd->get(), filename.c_str(), &status, AT_SYMLINK_NOFOLLOW) != 0) {
            if (errno == ENOENT) {
                return std::unexpected(path_not_found_error(path));
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
        if (stop_token.stop_requested()) {
            (void)::closedir(directory);
            return std::unexpected(operation_aborted_error(path));
        }
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
        } else if (!S_ISREG(status.st_mode)) {
            // FileKind intentionally has no representation for sockets,
            // devices, and FIFOs. Match the pi adapter's listDir behavior by
            // omitting unsupported entries rather than claiming they are
            // regular files.
            continue;
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

std::expected<bool, FileError> WorkspaceFileSystem::exists(const std::string& path) const {
    auto resolved = resolve_to_cwd(path);
    if (!resolved) {
        return std::unexpected(util_error_to_file_error(resolved.error(), path));
    }

    // Roots (the workspace root or a filesystem root) have no addressable
    // parent; opening them directly answers existence.
    if (*resolved == root_ || *resolved == resolved->root_path()) {
        if (auto root_fd = open_root_directory(*resolved); !root_fd) {
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
        if (is_permission_errno(errno)) {
            return std::unexpected(permission_denied_error(path));
        }
        return std::unexpected(FileError{FileErrorCode::Invalid, "could not check path: " + path, std::string{path}});
    }
    return true;
}

std::expected<void, FileError> WorkspaceFileSystem::createDir(const std::string& path, bool recursive) const {
    auto resolved = resolve_to_cwd(path);
    if (!resolved) {
        return std::unexpected(util_error_to_file_error(resolved.error(), path));
    }
    if (*resolved == root_ || *resolved == resolved->root_path()) {
        // The workspace root and every filesystem root already exist as
        // directories.
        return {};
    }

    int parent_errno = 0;
    auto parent_fd = open_parent_directory(*resolved, recursive, &parent_errno);
    if (!parent_fd) {
        if (parent_errno == ENOENT) {
            return std::unexpected(path_not_found_error(path));
        }
        return std::unexpected(util_error_to_file_error(parent_fd.error(), path));
    }
    const auto filename = resolved->filename().string();
    if (::mkdirat(parent_fd->get(), filename.c_str(), 0755) != 0) {
        if (errno == EEXIST) {
            struct stat status{};
            if (::fstatat(parent_fd->get(), filename.c_str(), &status, AT_SYMLINK_NOFOLLOW) == 0 &&
                    S_ISDIR(status.st_mode)) {
                return {};
            }
            return std::unexpected(FileError{
                    .code = FileErrorCode::Invalid,
                    .message = "path exists but is not a directory: " + path,
                    .path = std::string{path},
            });
        }
        return std::unexpected(FileError{
                .code = is_permission_errno(errno) ? FileErrorCode::PermissionDenied : FileErrorCode::Invalid,
                .message = "could not create directory: " + std::string(std::strerror(errno)),
                .path = std::string{path},
        });
    }
    return {};
}

std::expected<void, FileError> WorkspaceFileSystem::remove(
        const std::string& path, bool recursive, std::stop_token stop_token) const {
    auto resolved = resolve_to_cwd(path);
    if (!resolved) {
        return std::unexpected(util_error_to_file_error(resolved.error(), path));
    }
    if (*resolved == root_) {
        return std::unexpected(FileError{FileErrorCode::Invalid, "cannot remove workspace root", std::string{path}});
    }
    if (*resolved == resolved->root_path()) {
        return std::unexpected(FileError{FileErrorCode::Invalid, "cannot remove filesystem root", std::string{path}});
    }

    int parent_errno = 0;
    auto parent_fd = open_parent_directory(*resolved, false, &parent_errno);
    if (!parent_fd) {
        if (parent_errno == ENOENT) {
            return std::unexpected(path_not_found_error(path));
        }
        return std::unexpected(util_error_to_file_error(parent_fd.error(), path));
    }

    const auto filename = resolved->filename().string();
    struct stat status{};
    if (::fstatat(parent_fd->get(), filename.c_str(), &status, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno == ENOENT) {
            return std::unexpected(path_not_found_error(path));
        }
        return std::unexpected(
                FileError{FileErrorCode::PermissionDenied, "could not inspect path: " + path, std::string{path}});
    }

    if (S_ISDIR(status.st_mode) && recursive) {
        support::UniqueFd directory_fd(
                ::openat(parent_fd->get(), filename.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (!directory_fd) {
            if (errno == ENOENT) {
                return std::unexpected(path_not_found_error(path));
            }
            return std::unexpected(
                    FileError{FileErrorCode::PermissionDenied, "could not open directory: " + path, std::string{path}});
        }
        bool cancelled = false;
        if (!remove_directory_contents(directory_fd.get(), stop_token, &cancelled)) {
            if (cancelled) {
                return std::unexpected(FileError{
                        .code = FileErrorCode::Aborted,
                        .message = "Operation aborted; already-performed filesystem side effects may remain",
                        .path = std::string{path},
                });
            }
            return std::unexpected(FileError{FileErrorCode::PermissionDenied,
                    "could not remove directory contents: " + path,
                    std::string{path}});
        }
        if (stop_token.stop_requested()) {
            return std::unexpected(FileError{
                    .code = FileErrorCode::Aborted,
                    .message = "Operation aborted; already-performed filesystem side effects may remain",
                    .path = std::string{path},
            });
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
