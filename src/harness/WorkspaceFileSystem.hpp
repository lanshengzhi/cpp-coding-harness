#pragma once

#include "../../include/cch/harness/ExecutionEnv.hpp"
#include "../../include/cch/util/Error.hpp"
#include "AtomicWrite.hpp"
#include "UniqueFd.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#else
#include <fstream>
#include <sstream>
#endif

namespace cch::harness {

/// Workspace-scoped filesystem operations with containment and symlink safety.
///
/// All addressed-path operations reject absolute paths, ".." escapes, and
/// symlinks that resolve outside the workspace.  Metadata and listing use
/// lstat-equivalent no-follow semantics.
class WorkspaceFileSystem {
public:
    WorkspaceFileSystem() = default;
    explicit WorkspaceFileSystem(std::filesystem::path workspace)
        : root_(std::filesystem::weakly_canonical(std::move(workspace))) {}

    static util::Expected<WorkspaceFileSystem> create(const std::filesystem::path& workspace) {
        std::error_code ec;
        if (!std::filesystem::exists(workspace, ec) || !std::filesystem::is_directory(workspace, ec)) {
            return std::unexpected(workspace_error("workspace does not exist or is not a directory"));
        }
        return WorkspaceFileSystem(workspace);
    }

    [[nodiscard]] const std::filesystem::path& root() const { return root_; }

    /// Resolve a workspace-relative path to an absolute addressed path,
    /// validating containment. Does not require the path to exist.
    [[nodiscard]] util::Expected<std::filesystem::path> resolve_addressed_path(const std::string& requested) const {
        if (requested.empty()) {
            return std::unexpected(workspace_error("path is required"));
        }
        std::filesystem::path relative(requested);
        if (relative.is_absolute()) {
            return std::unexpected(workspace_error("absolute paths are not allowed: " + requested));
        }
        auto normalized = relative.lexically_normal();
        for (const auto& part : normalized) {
            if (part == "..") {
                return std::unexpected(workspace_error("path escapes workspace: " + requested));
            }
        }
        auto target = (root_ / normalized).lexically_normal();
        if (!inside_lexically(target)) {
            return std::unexpected(workspace_error("path escapes workspace: " + requested));
        }
        return target;
    }

    // -----------------------------------------------------------------------
    // Legacy tool-shaped operations (compatible with existing PathGuard API)
    // -----------------------------------------------------------------------

    [[nodiscard]] util::Expected<std::string> read_existing_file(const std::string& requested) const {
        auto target = resolve_addressed_path(requested);
        if (!target) {
            return std::unexpected(target.error());
        }

#if defined(__unix__) || defined(__APPLE__)
        auto parent_fd = open_parent_directory(*target, false);
        if (!parent_fd) {
            return std::unexpected(parent_fd.error());
        }
        UniqueFd parent_guard(*parent_fd);

        auto filename = target->filename().string();
        int fd = ::openat(*parent_fd, filename.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (fd == -1) {
            if (errno == ELOOP) {
                return std::unexpected(workspace_error("refusing to read through symlink: " + requested));
            }
            return std::unexpected(workspace_error("could not open file for reading: " + requested));
        }
        UniqueFd fd_guard(fd);

        struct stat st {};
        if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
            return std::unexpected(workspace_error("path is not a regular file: " + requested));
        }

        std::string content;
        char buffer[4096];
        ssize_t n = 0;
        while ((n = ::read(fd, buffer, sizeof(buffer))) > 0) {
            content.append(buffer, static_cast<std::size_t>(n));
        }
        if (n < 0) {
            return std::unexpected(workspace_error("could not read file: " + requested));
        }
        return content;
#else
        std::error_code ec;
        auto canonical = std::filesystem::canonical(*target, ec);
        if (ec) {
            return std::unexpected(workspace_error("path does not exist inside workspace: " + requested));
        }
        if (!inside(canonical)) {
            return std::unexpected(workspace_error("path escapes workspace: " + requested));
        }
        if (!std::filesystem::is_regular_file(canonical, ec)) {
            return std::unexpected(workspace_error("path is not a regular file: " + requested));
        }
        std::ifstream input(canonical, std::ios::binary);
        if (!input) {
            return std::unexpected(workspace_error("could not open file for reading: " + requested));
        }
        std::ostringstream buffer;
        buffer << input.rdbuf();
        return buffer.str();
#endif
    }

    [[nodiscard]] util::Expected<std::size_t> write_file(
        const std::string& requested,
        const std::string& content,
        bool create_parents) const {
        auto target = resolve_addressed_path(requested);
        if (!target) {
            return std::unexpected(target.error());
        }

#if defined(__unix__) || defined(__APPLE__)
        auto parent = target->parent_path();
        if (parent.empty()) {
            parent = root_;
        }
        if (!std::filesystem::exists(parent)) {
            if (!create_parents) {
                return std::unexpected(workspace_error("parent directory does not exist: " + requested));
            }
            auto created = create_parent_directories(*target);
            if (!created) {
                return std::unexpected(created.error());
            }
        }
        auto parent_fd = open_parent_directory(*target, create_parents);
        if (!parent_fd) {
            return std::unexpected(parent_fd.error());
        }
        UniqueFd parent_guard(*parent_fd);
        (void)parent_guard;
        auto target_status = std::filesystem::symlink_status(*target);
        if (std::filesystem::is_symlink(target_status)) {
            return std::unexpected(workspace_error("refusing to write through final symlink: " + requested));
        }
        if (std::filesystem::exists(target_status) && !std::filesystem::is_regular_file(target_status)) {
            return std::unexpected(workspace_error("target is not a regular file: " + requested));
        }
#else
        auto resolved = resolve_for_write(requested, create_parents);
        if (!resolved) {
            return std::unexpected(resolved.error());
        }
        target = *resolved;
#endif
        auto written = write_atomic_file(*target, content);
        if (!written) {
            return std::unexpected(written.error());
        }
        return content.size();
    }

    // -----------------------------------------------------------------------
    // Pi-shaped filesystem operations
    // -----------------------------------------------------------------------

    /// Return an absolute addressed path without following symlinks.
    [[nodiscard]] std::expected<std::string, FileError> absolutePath(const std::string& path) const {
        auto resolved = resolve_addressed_path(path);
        if (!resolved) {
            return std::unexpected(util_error_to_file_error(resolved.error(), path));
        }
        return resolved->string();
    }

    /// Join path segments without requiring existence.
    [[nodiscard]] std::expected<std::string, FileError> joinPath(const std::vector<std::string>& parts) const {
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

    /// Read entire UTF-8 text file.
    [[nodiscard]] std::expected<std::string, FileError> readTextFile(const std::string& path) const {
        auto content = read_existing_file(path);
        if (!content) {
            return std::unexpected(util_error_to_file_error(content.error(), path));
        }
        return *content;
    }

    /// Read UTF-8 text lines, stopping after maxLines if set.
    [[nodiscard]] std::expected<std::vector<std::string>, FileError> readTextLines(
        const std::string& path,
        std::optional<int> maxLines = std::nullopt) const {
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

    /// Read entire binary file.
    [[nodiscard]] std::expected<BinaryData, FileError> readBinaryFile(const std::string& path) const {
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

    /// Create or overwrite a file with text or binary content.
    [[nodiscard]] std::expected<void, FileError> writeFile(
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

    /// Append text or binary content to a file.
    [[nodiscard]] std::expected<void, FileError> appendFile(
        const std::string& path,
        const WriteContent& content) const {
        std::string data;
        if (std::holds_alternative<std::string>(content)) {
            data = std::get<std::string>(content);
        } else {
            const auto& bin = std::get<BinaryData>(content);
            data.assign(reinterpret_cast<const char*>(bin.data()), bin.size());
        }

        // Read existing content (may fail if file doesn't exist yet).
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

    /// Return lstat-equivalent metadata without following symlinks.
    [[nodiscard]] std::expected<FileInfo, FileError> fileInfo(const std::string& path) const {
        auto resolved = resolve_addressed_path(path);
        if (!resolved) {
            return std::unexpected(util_error_to_file_error(resolved.error(), path));
        }

#if defined(__unix__) || defined(__APPLE__)
        struct stat st {};
        if (::lstat(resolved->c_str(), &st) != 0) {
            if (errno == ENOENT) {
                return std::unexpected(FileError{FileErrorCode::NotFound, "path not found: " + path, std::string{path}});
            }
            if (errno == EACCES) {
                return std::unexpected(FileError{FileErrorCode::PermissionDenied, "permission denied: " + path, std::string{path}});
            }
            return std::unexpected(FileError{FileErrorCode::Unknown, "could not stat: " + path, std::string{path}});
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
            return std::unexpected(FileError{FileErrorCode::NotFound, "path not found: " + path, std::string{path}});
        }
        if (!std::filesystem::exists(status)) {
            return std::unexpected(FileError{FileErrorCode::NotFound, "path not found: " + path, std::string{path}});
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

    /// List direct children without following symlinks.
    [[nodiscard]] std::expected<std::vector<FileInfo>, FileError> listDir(const std::string& path) const {
        auto resolved = resolve_addressed_path(path);
        if (!resolved) {
            return std::unexpected(util_error_to_file_error(resolved.error(), path));
        }

        std::error_code ec;
        auto status = std::filesystem::symlink_status(*resolved, ec);
        if (ec) {
            return std::unexpected(FileError{FileErrorCode::NotFound, "path not found: " + path, std::string{path}});
        }
        if (!std::filesystem::is_directory(status)) {
            return std::unexpected(FileError{FileErrorCode::NotDirectory, "not a directory: " + path, std::string{path}});
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

    /// Return canonical path for an existing path, resolving symlinks.
    /// Rejects resolved targets outside the workspace.
    [[nodiscard]] std::expected<std::string, FileError> canonicalPath(const std::string& path) const {
        auto resolved = resolve_addressed_path(path);
        if (!resolved) {
            return std::unexpected(util_error_to_file_error(resolved.error(), path));
        }
        std::error_code ec;
        auto canonical = std::filesystem::canonical(*resolved, ec);
        if (ec) {
            return std::unexpected(FileError{FileErrorCode::NotFound, "could not canonicalize: " + path, std::string{path}});
        }
        if (!inside(canonical)) {
            return std::unexpected(FileError{FileErrorCode::Invalid, "canonical path escapes workspace: " + path, std::string{path}});
        }
        return canonical.string();
    }

    /// Return false for missing paths. Other errors return FileError.
    [[nodiscard]] std::expected<bool, FileError> exists(const std::string& path) const {
        auto resolved = resolve_addressed_path(path);
        if (!resolved) {
            return std::unexpected(util_error_to_file_error(resolved.error(), path));
        }
        std::error_code ec;
        auto status = std::filesystem::symlink_status(*resolved, ec);
        // no_such_file_or_directory is a normal condition — return false.
        if (ec && ec.default_error_condition() != std::errc::no_such_file_or_directory) {
            return std::unexpected(FileError{FileErrorCode::Invalid, "could not check path: " + path, std::string{path}});
        }
        return std::filesystem::exists(status);
    }

    /// Create a directory (recursive by default).
    [[nodiscard]] std::expected<void, FileError> createDir(
        const std::string& path,
        bool recursive = true) const {
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
            return std::unexpected(FileError{FileErrorCode::Invalid, "could not create directory: " + ec.message(), std::string{path}});
        }
#endif
        return {};
    }

    /// Remove a file or directory. Rejects symlink escape and workspace root removal.
    [[nodiscard]] std::expected<void, FileError> remove(
        const std::string& path,
        bool recursive = false) const {
        auto resolved = resolve_addressed_path(path);
        if (!resolved) {
            return std::unexpected(util_error_to_file_error(resolved.error(), path));
        }

        // Reject workspace root removal.
        auto canonical = std::filesystem::weakly_canonical(*resolved);
        if (canonical == root_) {
            return std::unexpected(FileError{
                FileErrorCode::Invalid, "cannot remove workspace root", std::string{path}});
        }

        std::error_code ec;
        auto status = std::filesystem::symlink_status(*resolved, ec);
        if (ec) {
            return std::unexpected(FileError{FileErrorCode::NotFound, "path not found: " + path, std::string{path}});
        }

        if (std::filesystem::is_symlink(status)) {
            // Remove the symlink itself, not its target.
            std::filesystem::remove(*resolved, ec);
            if (ec) {
                return std::unexpected(FileError{
                    FileErrorCode::PermissionDenied, "could not remove symlink: " + ec.message(), std::string{path}});
            }
            return {};
        }

        if (std::filesystem::is_directory(status) && recursive) {
            // Recursive removal without following symlinks to outside.
            for (const auto& entry : std::filesystem::directory_iterator(*resolved, ec)) {
                if (ec) {
                    break;
                }
                auto child = entry.path().string();
                // Remove child symlinks directly (don't recurse into them).
                auto child_status = std::filesystem::symlink_status(entry.path(), ec);
                if (!ec && std::filesystem::is_symlink(child_status)) {
                    std::filesystem::remove(entry.path(), ec);
                } else if (!ec && std::filesystem::is_directory(child_status)) {
                    auto child_result = remove(
                        entry.path().lexically_relative(root_).string(), true);
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
                FileErrorCode::PermissionDenied, "could not remove: " + ec.message(), std::string{path}});
        }
        return {};
    }

    /// Create a workspace-contained temp directory.
    [[nodiscard]] std::expected<std::string, FileError> createTempDir(
        std::optional<std::string> prefix = std::nullopt) const {
        auto tmp_area = root_ / ".cch-tmp";
        std::error_code ec;
        std::filesystem::create_directories(tmp_area, ec);
        // Not an error if it already exists.

        std::string pfx = prefix.value_or("tmp-");
        for (int i = 0; i < 1000; ++i) {
            auto candidate = tmp_area / (pfx + std::to_string(i));
            auto status = std::filesystem::symlink_status(candidate, ec);
            if (!ec && std::filesystem::exists(status)) {
                continue;
            }
            if (::mkdir(candidate.c_str(), 0700) == 0) {
                return candidate.string();
            }
            if (errno == EEXIST) {
                continue;
            }
            break;
        }
        return std::unexpected(FileError{FileErrorCode::Unknown, "could not create temp directory", std::nullopt});
    }

    /// Create a workspace-contained temp file.
    [[nodiscard]] std::expected<std::string, FileError> createTempFile(
        std::optional<std::string> prefix = std::nullopt,
        std::optional<std::string> suffix = std::nullopt) const {
        auto tmp_area = root_ / ".cch-tmp";
        std::error_code ec;
        std::filesystem::create_directories(tmp_area, ec);

        std::string pfx = prefix.value_or("");
        std::string sfx = suffix.value_or("");
        for (int i = 0; i < 1000; ++i) {
            auto candidate = tmp_area / (pfx + std::to_string(i) + sfx);
            auto status = std::filesystem::symlink_status(candidate, ec);
            if (!ec && std::filesystem::exists(status)) {
                continue;
            }
            std::ofstream out(candidate, std::ios::binary);
            if (out) {
                out.close();
                return candidate.string();
            }
            break;
        }
        return std::unexpected(FileError{FileErrorCode::Unknown, "could not create temp file", std::nullopt});
    }

private:
    [[nodiscard]] static util::Error workspace_error(std::string message) {
        return util::make_error(util::ErrorCode::Workspace, message, message);
    }

    [[nodiscard]] static FileError util_error_to_file_error(const util::Error& e, const std::string& path) {
        FileErrorCode code = FileErrorCode::Unknown;
        switch (e.code) {
        case util::ErrorCode::Workspace:
            code = FileErrorCode::PermissionDenied;
            break;
        case util::ErrorCode::Validation:
            code = FileErrorCode::Invalid;
            break;
        case util::ErrorCode::Cancelled:
            code = FileErrorCode::Aborted;
            break;
        default:
            break;
        }
        return FileError{code, e.message, std::string{path}};
    }

#if defined(__unix__) || defined(__APPLE__)
    [[nodiscard]] util::Expected<int> open_workspace_root() const {
        int fd = ::open(root_.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (fd == -1) {
            return std::unexpected(workspace_error("could not open workspace root: " + std::string(std::strerror(errno))));
        }
        return fd;
    }

    [[nodiscard]] util::Expected<int> open_parent_directory(
        const std::filesystem::path& target,
        bool create_missing) const {
        auto root_fd = open_workspace_root();
        if (!root_fd) {
            return std::unexpected(root_fd.error());
        }
        UniqueFd root_guard(*root_fd);

        auto parent = target.parent_path();
        if (parent.empty() || std::filesystem::weakly_canonical(parent) == root_) {
            int dup_fd = ::dup(*root_fd);
            if (dup_fd == -1) {
                return std::unexpected(workspace_error("could not duplicate workspace fd: " + std::string(std::strerror(errno))));
            }
            return dup_fd;
        }

        auto rel = parent.lexically_normal().lexically_relative(root_);
        if (rel.empty() || rel == ".") {
            int dup_fd = ::dup(*root_fd);
            if (dup_fd == -1) {
                return std::unexpected(workspace_error("could not duplicate workspace fd: " + std::string(std::strerror(errno))));
            }
            return dup_fd;
        }

        int current_fd = root_guard.release();
        UniqueFd current_guard(current_fd);
        for (const auto& part : rel) {
            if (part == "." || part.empty()) {
                continue;
            }
            if (part == "..") {
                return std::unexpected(workspace_error("parent path escapes workspace"));
            }
            int next_fd = ::openat(current_fd, part.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (next_fd == -1) {
                if (errno == ENOENT && create_missing) {
                    if (::mkdirat(current_fd, part.c_str(), 0755) != 0) {
                        return std::unexpected(workspace_error("could not create parent directory: " + std::string(std::strerror(errno))));
                    }
                    next_fd = ::openat(current_fd, part.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
                    if (next_fd == -1) {
                        return std::unexpected(workspace_error("could not open created parent directory: " + std::string(std::strerror(errno))));
                    }
                } else {
                    return std::unexpected(workspace_error("could not open parent directory: " + std::string(std::strerror(errno))));
                }
            }
            current_guard.reset(next_fd);
            current_fd = next_fd;
        }

        return current_guard.release();
    }

    [[nodiscard]] util::Expected<void> create_parent_directories(const std::filesystem::path& target) const {
        auto parent_fd = open_parent_directory(target, true);
        if (!parent_fd) {
            return std::unexpected(parent_fd.error());
        }
        const UniqueFd guard(*parent_fd);
        (void)guard;
        return {};
    }
#endif

    [[nodiscard]] bool inside(const std::filesystem::path& path) const {
        return inside_lexically(std::filesystem::weakly_canonical(path));
    }

    [[nodiscard]] bool inside_lexically(const std::filesystem::path& path) const {
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

    [[nodiscard]] bool has_symlink_component(const std::filesystem::path& lexical_parent) const {
        auto rel = lexical_parent.lexically_normal().lexically_relative(root_);
        if (rel.empty() || rel == ".") {
            return false;
        }
        std::error_code ec;
        std::filesystem::path cursor = root_;
        for (const auto& part : rel) {
            if (part == "." || part.empty()) {
                continue;
            }
            if (part == "..") {
                return true;
            }
            cursor /= part;
            auto status = std::filesystem::symlink_status(cursor, ec);
            if (ec) {
                return status.type() != std::filesystem::file_type::not_found;
            }
            if (!std::filesystem::exists(status)) {
                return false;
            }
            if (std::filesystem::is_symlink(status)) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] util::Expected<std::filesystem::path> resolve_for_write(
        const std::string& requested, bool create_parents) const {
        auto target = resolve_addressed_path(requested);
        if (!target) {
            return std::unexpected(target.error());
        }
        std::error_code ec;
        auto parent = target->parent_path();
        if (parent.empty()) {
            parent = root_;
        }
        if (!std::filesystem::exists(parent, ec)) {
            if (!create_parents) {
                return std::unexpected(workspace_error("parent directory does not exist: " + requested));
            }
            if (has_symlink_component(parent)) {
                return std::unexpected(workspace_error("parent path contains a symlink: " + requested));
            }
            std::filesystem::create_directories(parent, ec);
            if (ec) {
                return std::unexpected(workspace_error("could not create parent directory: " + ec.message()));
            }
        }
        if (has_symlink_component(parent)) {
            return std::unexpected(workspace_error("parent path contains a symlink: " + requested));
        }
        auto parent_canonical = std::filesystem::canonical(parent, ec);
        if (ec || !inside(parent_canonical)) {
            return std::unexpected(workspace_error("parent path escapes workspace: " + requested));
        }
        auto status = std::filesystem::symlink_status(*target, ec);
        if (ec && status.type() != std::filesystem::file_type::not_found) {
            return std::unexpected(workspace_error("could not inspect target: " + requested));
        }
        if (std::filesystem::is_symlink(status)) {
            return std::unexpected(workspace_error("refusing to write through final symlink: " + requested));
        }
        if (std::filesystem::exists(status) && !std::filesystem::is_regular_file(status)) {
            return std::unexpected(workspace_error("target is not a regular file: " + requested));
        }
        return *target;
    }

    std::filesystem::path root_{std::filesystem::current_path()};
};

} // namespace cch::harness
