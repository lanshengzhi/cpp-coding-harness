#include "WorkspaceFileSystem.hpp"

#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace cch::harness {
namespace {

void remember_errno(int* failure_errno, int value) noexcept {
    if (failure_errno) {
        *failure_errno = value;
    }
}

} // namespace

support::Expected<support::UniqueFd> WorkspaceFileSystem::open_workspace_root() const {
    support::UniqueFd fd(::open(root_.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (!fd) {
        return std::unexpected(workspace_error("could not open workspace root: " + std::string(std::strerror(errno))));
    }
    return fd;
}

support::Expected<support::UniqueFd> WorkspaceFileSystem::open_parent_directory(
        const std::filesystem::path& target, bool create_missing, int* failure_errno) const {
    if (failure_errno) {
        *failure_errno = 0;
    }

    auto root_guard = open_workspace_root();
    const int root_error = errno;
    if (!root_guard) {
        remember_errno(failure_errno, root_error);
        return std::unexpected(root_guard.error());
    }

    const auto parent = target.parent_path().lexically_normal();
    const auto relative = parent.lexically_relative(root_);
    if (relative.empty() || relative == ".") {
        support::UniqueFd duplicate(::dup(root_guard->get()));
        if (!duplicate) {
            remember_errno(failure_errno, errno);
            return std::unexpected(
                    workspace_error("could not duplicate workspace fd: " + std::string(std::strerror(errno))));
        }
        return duplicate;
    }
    if (relative.is_absolute()) {
        return std::unexpected(workspace_error("parent path escapes workspace"));
    }

    support::UniqueFd current_guard(root_guard->release());
    for (const auto& part : relative) {
        if (part == "." || part.empty()) {
            continue;
        }
        if (part == "..") {
            return std::unexpected(workspace_error("parent path escapes workspace"));
        }

        support::UniqueFd next_fd(
                ::openat(current_guard.get(), part.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        const int open_error = errno;
        if (!next_fd && open_error == ENOENT && create_missing) {
            if (::mkdirat(current_guard.get(), part.c_str(), 0755) != 0) {
                const int mkdir_error = errno;
                if (mkdir_error != EEXIST) {
                    remember_errno(failure_errno, mkdir_error);
                    return std::unexpected(workspace_error(
                            "could not create parent directory: " + std::string(std::strerror(mkdir_error))));
                }
            }
            next_fd.reset(::openat(current_guard.get(), part.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
            if (!next_fd) {
                remember_errno(failure_errno, errno);
                return std::unexpected(workspace_error(
                        "could not open created parent directory: " + std::string(std::strerror(errno))));
            }
        } else if (!next_fd) {
            remember_errno(failure_errno, open_error);
            return std::unexpected(
                    workspace_error("could not open parent directory: " + std::string(std::strerror(open_error))));
        }
        current_guard = std::move(next_fd);
    }

    return current_guard;
}

support::Expected<void> WorkspaceFileSystem::validate_directory(const std::filesystem::path& target) const {
    if (target == root_) {
        auto root_guard = open_workspace_root();
        if (!root_guard) {
            return std::unexpected(root_guard.error());
        }
        return {};
    }

    auto parent_guard = open_parent_directory(target, false);
    if (!parent_guard) {
        return std::unexpected(parent_guard.error());
    }

    const auto filename = target.filename().string();
    struct stat status{};
    if (::fstatat(parent_guard->get(), filename.c_str(), &status, AT_SYMLINK_NOFOLLOW) != 0) {
        return std::unexpected(workspace_error("cwd does not exist or cannot be inspected: " + target.string()));
    }
    if (!S_ISDIR(status.st_mode)) {
        return std::unexpected(workspace_error("cwd does not exist or is not a directory: " + target.string()));
    }

    support::UniqueFd directory(
            ::openat(parent_guard->get(), filename.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (!directory) {
        return std::unexpected(
                workspace_error("cwd could not be opened without following symlinks: " + target.string()));
    }
    return {};
}

support::Expected<void> WorkspaceFileSystem::create_parent_directories(const std::filesystem::path& target) const {
    auto parent_fd = open_parent_directory(target, true);
    if (!parent_fd) {
        return std::unexpected(parent_fd.error());
    }
    return {};
}

bool WorkspaceFileSystem::remove_directory_contents(
        int directory_fd, std::stop_token stop_token, bool* cancelled) const noexcept {
    if (cancelled) {
        *cancelled = false;
    }
    support::UniqueFd duplicate(::dup(directory_fd));
    if (!duplicate) {
        return false;
    }

    DIR* directory = ::fdopendir(duplicate.get());
    if (!directory) {
        return false;
    }
    (void)duplicate.release();

    bool success = true;
    for (;;) {
        if (stop_token.stop_requested()) {
            if (cancelled) {
                *cancelled = true;
            }
            success = false;
            break;
        }
        errno = 0;
        const auto* entry = ::readdir(directory);
        if (!entry) {
            if (errno != 0) {
                success = false;
            }
            break;
        }
        if (entry->d_name[0] == '.' &&
                (entry->d_name[1] == '\0' || (entry->d_name[1] == '.' && entry->d_name[2] == '\0'))) {
            continue;
        }

        struct stat status{};
        if (::fstatat(directory_fd, entry->d_name, &status, AT_SYMLINK_NOFOLLOW) != 0) {
            if (errno != ENOENT) {
                success = false;
            }
            continue;
        }

        if (S_ISDIR(status.st_mode)) {
            support::UniqueFd child_fd(
                    ::openat(directory_fd, entry->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
            if (!child_fd) {
                if (errno == ENOENT) {
                    continue;
                }

                struct stat replacement{};
                if (::fstatat(directory_fd, entry->d_name, &replacement, AT_SYMLINK_NOFOLLOW) == 0 &&
                        !S_ISDIR(replacement.st_mode)) {
                    if (::unlinkat(directory_fd, entry->d_name, 0) != 0 && errno != ENOENT) {
                        success = false;
                    }
                } else {
                    success = false;
                }
                continue;
            }

            bool child_cancelled = false;
            if (!remove_directory_contents(child_fd.get(), stop_token, &child_cancelled)) {
                success = false;
            }
            if (child_cancelled) {
                if (cancelled) {
                    *cancelled = true;
                }
                break;
            }
            if (stop_token.stop_requested()) {
                if (cancelled) {
                    *cancelled = true;
                }
                break;
            }
            if (::unlinkat(directory_fd, entry->d_name, AT_REMOVEDIR) != 0 && errno != ENOENT) {
                success = false;
            }
            continue;
        }

        if (stop_token.stop_requested()) {
            if (cancelled) {
                *cancelled = true;
            }
            success = false;
            break;
        }
        if (::unlinkat(directory_fd, entry->d_name, 0) != 0 && errno != ENOENT) {
            success = false;
        }
    }

    if (::closedir(directory) != 0) {
        success = false;
    }
    return success;
}

} // namespace cch::harness
