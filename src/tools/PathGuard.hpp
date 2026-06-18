#pragma once

#include "../../include/cch/util/Error.hpp"
#include "AtomicWrite.hpp"

#include <filesystem>
#include <memory>
#include <string>

#if defined(__unix__) || defined(__APPLE__)
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#else
#include <fstream>
#include <sstream>
#endif

namespace cch::tools {

class PathGuard {
public:
    PathGuard() = default;
    explicit PathGuard(std::filesystem::path workspace)
        : root_(std::filesystem::weakly_canonical(std::move(workspace))) {}

    static util::Expected<PathGuard> create(const std::filesystem::path& workspace) {
        std::error_code ec;
        if (!std::filesystem::exists(workspace, ec) || !std::filesystem::is_directory(workspace, ec)) {
            return std::unexpected(workspace_error("workspace does not exist or is not a directory"));
        }
        return PathGuard(workspace);
    }

    [[nodiscard]] const std::filesystem::path& root() const { return root_; }

    /**
     * Open an existing regular file inside the workspace without following
     * symlinks.  The returned content is read from the file descriptor obtained
     * via openat(O_NOFOLLOW) relative to the workspace root, closing the
     * symlink TOCTOU window left by resolve_existing_file + std::ifstream.
     */
    [[nodiscard]] util::Expected<std::string> read_existing_file(const std::string& requested) const {
        auto target = lexical_workspace_path(requested);
        if (!target) {
            return std::unexpected(target.error());
        }

#if defined(__unix__) || defined(__APPLE__)
        auto parent_fd = open_parent_directory(*target, false);
        if (!parent_fd) {
            return std::unexpected(parent_fd.error());
        }
        auto parent_guard = make_fd_guard(*parent_fd);

        auto filename = target->filename().string();
        int fd = ::openat(*parent_fd, filename.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (fd == -1) {
            if (errno == ELOOP) {
                return std::unexpected(workspace_error("refusing to read through symlink: " + requested));
            }
            return std::unexpected(workspace_error("could not open file for reading: " + requested));
        }
        auto fd_guard = make_fd_guard(fd);

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

    /**
     * Write content to a path inside the workspace.  Parent directories are
     * created without following symlinks on POSIX (mkdirat chain), and the
     * actual write uses write_atomic_file which opens the parent directory with
     * O_NOFOLLOW and uses openat/renameat.
     */
    [[nodiscard]] util::Expected<std::size_t> write_file(
        const std::string& requested,
        const std::string& content,
        bool create_parents) const {
        auto target = lexical_workspace_path(requested);
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
        auto parent_guard = make_fd_guard(*parent_fd);
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

private:
    [[nodiscard]] static util::Error workspace_error(std::string message) {
        return util::make_error(util::ErrorCode::Workspace, message, message);
    }

#if defined(__unix__) || defined(__APPLE__)
    [[nodiscard]] static std::unique_ptr<int, void (*)(int*)> make_fd_guard(int fd) {
        return std::unique_ptr<int, void (*)(int*)>(new int(fd), [](int* p) {
            if (p && *p != -1) {
                ::close(*p);
            }
            delete p;
        });
    }

    [[nodiscard]] util::Expected<int> open_workspace_root() const {
        int fd = ::open(root_.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (fd == -1) {
            return std::unexpected(workspace_error("could not open workspace root: " + std::string(std::strerror(errno))));
        }
        return fd;
    }

    [[nodiscard]] std::string relative_to_root(const std::filesystem::path& target) const {
        auto rel = target.lexically_normal().lexically_relative(root_);
        if (rel.empty() || rel == ".") {
            return ".";
        }
        return rel.string();
    }

    [[nodiscard]] util::Expected<int> open_parent_directory(
        const std::filesystem::path& target,
        bool create_missing) const {
        auto root_fd = open_workspace_root();
        if (!root_fd) {
            return std::unexpected(root_fd.error());
        }

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

        int current_fd = *root_fd;
        auto current_guard = make_fd_guard(current_fd);
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
            if (current_fd != *root_fd) {
                ::close(current_fd);
            }
            current_fd = next_fd;
            current_guard = make_fd_guard(current_fd);
        }

        // Ownership transferred to caller.
        return *current_guard.release();
    }

    [[nodiscard]] util::Expected<void> create_parent_directories(const std::filesystem::path& target) const {
        auto parent_fd = open_parent_directory(target, true);
        if (!parent_fd) {
            return std::unexpected(parent_fd.error());
        }
        auto guard = make_fd_guard(*parent_fd);
        (void)guard;
        return {};
    }
#endif

    [[nodiscard]] util::Expected<std::filesystem::path> lexical_workspace_path(const std::string& requested) const {
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

    [[nodiscard]] bool inside(const std::filesystem::path& path) const { return inside_lexically(std::filesystem::weakly_canonical(path)); }

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

    [[nodiscard]] util::Expected<std::filesystem::path> resolve_for_write(const std::string& requested, bool create_parents) const {
        auto target = lexical_workspace_path(requested);
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

} // namespace cch::tools
