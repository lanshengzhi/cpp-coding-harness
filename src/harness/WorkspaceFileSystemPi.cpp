#include "WorkspaceFileSystem.hpp"

#include <chrono>
#include <sstream>

#if defined(__unix__) || defined(__APPLE__)
#include <cerrno>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>
#endif

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
    auto content = read_existing_file(path);
    if (!content) {
        return std::unexpected(util_error_to_file_error(content.error(), path));
    }
    return *content;
}

std::expected<std::vector<std::string>, FileError> WorkspaceFileSystem::readTextLines(
    const std::string& path,
    std::optional<int> maxLines) const {
    auto content = read_existing_file(path);
    if (!content) {
        return std::unexpected(util_error_to_file_error(content.error(), path));
    }
    std::vector<std::string> lines;
    std::istringstream input(*content);
    std::string line;
    int count = 0;
    while (std::getline(input, line)) {
        if (maxLines && count >= *maxLines) {
            break;
        }
        lines.push_back(std::move(line));
        ++count;
    }
    return lines;
}

std::expected<BinaryData, FileError> WorkspaceFileSystem::readBinaryFile(const std::string& path) const {
    auto content = read_existing_file(path);
    if (!content) {
        return std::unexpected(util_error_to_file_error(content.error(), path));
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

    auto existing = read_existing_file(path);
    auto resolved = resolve_addressed_path(path);
    if (!resolved) {
        return std::unexpected(util_error_to_file_error(resolved.error(), path));
    }

    std::string combined;
    if (existing) {
        combined = *existing + data;
    } else {
        combined = data;
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

#if defined(__unix__) || defined(__APPLE__)
    struct stat st {};
    if (::lstat(resolved->c_str(), &st) != 0) {
        if (errno == ENOENT) {
            return std::unexpected(FileError{
                FileErrorCode::NotFound,
                "path not found: " + path,
                std::string{path}});
        }
        if (errno == EACCES) {
            return std::unexpected(FileError{
                FileErrorCode::PermissionDenied,
                "permission denied: " + path,
                std::string{path}});
        }
        return std::unexpected(FileError{
            FileErrorCode::Unknown,
            "could not stat: " + path,
            std::string{path}});
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
#else
    std::error_code ec;
    auto status = std::filesystem::symlink_status(*resolved, ec);
    if (ec) {
        return std::unexpected(FileError{
            FileErrorCode::NotFound,
            "path not found: " + path,
            std::string{path}});
    }
    if (!std::filesystem::exists(status)) {
        return std::unexpected(FileError{
            FileErrorCode::NotFound,
            "path not found: " + path,
            std::string{path}});
    }

    FileKind kind = FileKind::File;
    if (std::filesystem::is_directory(status)) {
        kind = FileKind::Directory;
    } else if (std::filesystem::is_symlink(status)) {
        kind = FileKind::Symlink;
    }

    auto name = resolved->filename().string();
    auto mtime = std::filesystem::last_write_time(*resolved, ec);
    auto mtimeMs = ec ? 0 : std::chrono::duration_cast<std::chrono::milliseconds>(
        mtime.time_since_epoch()).count();
    return FileInfo{
        .name = name.empty() ? resolved->string() : name,
        .path = resolved->string(),
        .kind = kind,
        .size = static_cast<std::uint64_t>(std::filesystem::file_size(*resolved, ec)),
        .mtimeMs = static_cast<std::int64_t>(mtimeMs),
    };
#endif
}

std::expected<std::vector<FileInfo>, FileError> WorkspaceFileSystem::listDir(const std::string& path) const {
    auto resolved = resolve_addressed_path(path);
    if (!resolved) {
        return std::unexpected(util_error_to_file_error(resolved.error(), path));
    }

    std::error_code ec;
    auto status = std::filesystem::symlink_status(*resolved, ec);
    if (ec) {
        return std::unexpected(FileError{
            FileErrorCode::NotFound,
            "path not found: " + path,
            std::string{path}});
    }
    if (!std::filesystem::is_directory(status)) {
        return std::unexpected(FileError{
            FileErrorCode::NotDirectory,
            "not a directory: " + path,
            std::string{path}});
    }

    std::vector<FileInfo> results;
    for (const auto& entry : std::filesystem::directory_iterator(*resolved, ec)) {
        if (ec) {
            break;
        }
        auto child_status = std::filesystem::symlink_status(entry.path(), ec);
        if (ec) {
            continue;
        }

        FileKind kind = FileKind::File;
        if (std::filesystem::is_directory(child_status)) {
            kind = FileKind::Directory;
        } else if (std::filesystem::is_symlink(child_status)) {
            kind = FileKind::Symlink;
        }

        auto child_name = entry.path().filename().string();
        std::uint64_t size = 0;
        std::int64_t mtimeMs = 0;
#if defined(__unix__) || defined(__APPLE__)
        struct stat st {};
        if (::lstat(entry.path().c_str(), &st) == 0) {
            size = static_cast<std::uint64_t>(st.st_size);
            mtimeMs = static_cast<std::int64_t>(st.st_mtim.tv_sec) * 1000 +
                      static_cast<std::int64_t>(st.st_mtim.tv_nsec) / 1'000'000;
        }
#else
        size = static_cast<std::uint64_t>(std::filesystem::file_size(entry.path(), ec));
        auto mtime = std::filesystem::last_write_time(entry.path(), ec);
        if (!ec) {
            mtimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(mtime.time_since_epoch()).count();
        }
#endif
        results.push_back(FileInfo{
            .name = child_name,
            .path = entry.path().string(),
            .kind = kind,
            .size = size,
            .mtimeMs = mtimeMs,
        });
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
    if (!inside(canonical)) {
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
    std::error_code ec;
    auto status = std::filesystem::symlink_status(*resolved, ec);
    if (ec && ec.default_error_condition() != std::errc::no_such_file_or_directory) {
        return std::unexpected(FileError{
            FileErrorCode::Invalid,
            "could not check path: " + path,
            std::string{path}});
    }
    return std::filesystem::exists(status);
}

std::expected<void, FileError> WorkspaceFileSystem::createDir(
    const std::string& path,
    bool recursive) const {
    auto resolved = resolve_addressed_path(path);
    if (!resolved) {
        return std::unexpected(util_error_to_file_error(resolved.error(), path));
    }
#if defined(__unix__) || defined(__APPLE__)
    if (recursive) {
        auto created = create_parent_directories(*resolved);
        if (!created) {
            return std::unexpected(util_error_to_file_error(created.error(), path));
        }
    }
    if (::mkdir(resolved->c_str(), 0755) != 0) {
        if (errno == EEXIST) {
            return {};
        }
        return std::unexpected(FileError{
            FileErrorCode::Invalid,
            "could not create directory: " + std::string(std::strerror(errno)),
            std::string{path}});
    }
#else
    std::error_code ec;
    std::filesystem::create_directories(*resolved, ec);
    if (ec) {
        return std::unexpected(FileError{
            FileErrorCode::Invalid,
            "could not create directory: " + ec.message(),
            std::string{path}});
    }
#endif
    return {};
}

std::expected<void, FileError> WorkspaceFileSystem::remove(
    const std::string& path,
    bool recursive) const {
    auto resolved = resolve_addressed_path(path);
    if (!resolved) {
        return std::unexpected(util_error_to_file_error(resolved.error(), path));
    }

    std::error_code ec;
    auto canonical = std::filesystem::weakly_canonical(*resolved, ec);
    if (ec) {
        return std::unexpected(FileError{
            .code = FileErrorCode::Invalid,
            .message = "could not resolve path: " + path,
            .path = std::string{path},
        });
    }
    if (canonical == root_) {
        return std::unexpected(FileError{
            FileErrorCode::Invalid,
            "cannot remove workspace root",
            std::string{path}});
    }

    auto status = std::filesystem::symlink_status(*resolved, ec);
    if (ec) {
        return std::unexpected(FileError{
            FileErrorCode::NotFound,
            "path not found: " + path,
            std::string{path}});
    }

    if (std::filesystem::is_symlink(status)) {
        std::filesystem::remove(*resolved, ec);
        if (ec) {
            return std::unexpected(FileError{
                FileErrorCode::PermissionDenied,
                "could not remove symlink: " + ec.message(),
                std::string{path}});
        }
        return {};
    }

    if (std::filesystem::is_directory(status) && recursive) {
        for (const auto& entry : std::filesystem::directory_iterator(*resolved, ec)) {
            if (ec) {
                break;
            }
            auto child = entry.path().string();
            auto child_status = std::filesystem::symlink_status(entry.path(), ec);
            if (!ec && std::filesystem::is_symlink(child_status)) {
                std::filesystem::remove(entry.path(), ec);
            } else if (!ec && std::filesystem::is_directory(child_status)) {
                auto child_result = remove(entry.path().lexically_relative(root_).string(), true);
                if (!child_result) {
                    return child_result;
                }
            } else {
                std::filesystem::remove(entry.path(), ec);
            }
        }
    }

    std::filesystem::remove(*resolved, ec);
    if (ec) {
        return std::unexpected(FileError{
            FileErrorCode::PermissionDenied,
            "could not remove: " + ec.message(),
            std::string{path}});
    }
    return {};
}

} // namespace cch::harness
