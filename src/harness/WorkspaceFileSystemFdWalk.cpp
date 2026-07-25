#include "WorkspaceFileSystem.hpp"

#if defined(__unix__) || defined(__APPLE__)
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace cch::harness {

#if defined(__unix__) || defined(__APPLE__)
util::Expected<UniqueFd> WorkspaceFileSystem::open_workspace_root() const {
    UniqueFd fd(::open(root_.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (!fd) {
        return std::unexpected(workspace_error(
            "could not open workspace root: " + std::string(std::strerror(errno))));
    }
    return fd;
}

util::Expected<UniqueFd> WorkspaceFileSystem::open_parent_directory(
    const std::filesystem::path& target,
    bool create_missing) const {
    auto root_guard = open_workspace_root();
    if (!root_guard) {
        return std::unexpected(root_guard.error());
    }

    auto parent = target.parent_path();
    std::error_code parent_ec;
    if (parent.empty() || std::filesystem::weakly_canonical(parent, parent_ec) == root_) {
        UniqueFd dup_fd(::dup(root_guard->get()));
        if (!dup_fd) {
            return std::unexpected(workspace_error(
                "could not duplicate workspace fd: " + std::string(std::strerror(errno))));
        }
        return dup_fd;
    }

    auto rel = parent.lexically_normal().lexically_relative(root_);
    if (rel.empty() || rel == ".") {
        UniqueFd dup_fd(::dup(root_guard->get()));
        if (!dup_fd) {
            return std::unexpected(workspace_error(
                "could not duplicate workspace fd: " + std::string(std::strerror(errno))));
        }
        return dup_fd;
    }

    UniqueFd current_guard(root_guard->release());
    for (const auto& part : rel) {
        if (part == "." || part.empty()) {
            continue;
        }
        if (part == "..") {
            return std::unexpected(workspace_error("parent path escapes workspace"));
        }
        UniqueFd next_fd(::openat(
            current_guard.get(),
            part.c_str(),
            O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (!next_fd) {
            if (errno == ENOENT && create_missing) {
                if (::mkdirat(current_guard.get(), part.c_str(), 0755) != 0) {
                    return std::unexpected(workspace_error(
                        "could not create parent directory: " + std::string(std::strerror(errno))));
                }
                next_fd.reset(::openat(
                    current_guard.get(),
                    part.c_str(),
                    O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
                if (!next_fd) {
                    return std::unexpected(workspace_error(
                        "could not open created parent directory: " + std::string(std::strerror(errno))));
                }
            } else {
                return std::unexpected(workspace_error(
                    "could not open parent directory: " + std::string(std::strerror(errno))));
            }
        }
        current_guard = std::move(next_fd);
    }

    return current_guard;
}

util::Expected<void> WorkspaceFileSystem::create_parent_directories(
    const std::filesystem::path& target) const {
    auto parent_fd = open_parent_directory(target, true);
    if (!parent_fd) {
        return std::unexpected(parent_fd.error());
    }
    return {};
}
#endif

} // namespace cch::harness
