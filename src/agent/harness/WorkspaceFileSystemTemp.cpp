#include "WorkspaceFileSystem.hpp"

#include <cerrno>
#include <cstring>
#include <string_view>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace cch::harness {
namespace {

[[nodiscard]] FileError temporary_error(
    FileErrorCode code,
    std::string message,
    std::optional<std::string> path = std::nullopt) {
    return FileError{
        .code = code,
        .message = std::move(message),
        .path = std::move(path),
    };
}

[[nodiscard]] bool valid_component(std::string_view component) {
    return component.find('/') == std::string_view::npos &&
        component.find('\0') == std::string_view::npos &&
        component != "." && component != "..";
}

[[nodiscard]] bool same_object(const struct stat& left, const struct stat& right) noexcept {
    return left.st_dev == right.st_dev && left.st_ino == right.st_ino &&
           (left.st_mode & S_IFMT) == (right.st_mode & S_IFMT);
}

} // namespace

support::Expected<void> WorkspaceFileSystem::ensure_temporary_directory() const {
    if (temporary_state_->temporary_directory) {
        return {};
    }

    auto root_fd = open_workspace_root();
    if (!root_fd) {
        return std::unexpected(root_fd.error());
    }

    support::UniqueFd directory(::openat(root_fd->get(), ".cch-tmp", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (!directory && errno == ENOENT) {
        if (::mkdirat(root_fd->get(), ".cch-tmp", 0700) != 0 && errno != EEXIST) {
            return std::unexpected(
                    workspace_error("could not create temporary directory: " + std::string(std::strerror(errno))));
        }
        directory.reset(::openat(root_fd->get(), ".cch-tmp", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    }
    if (!directory) {
        return std::unexpected(
                workspace_error("could not open temporary directory: " + std::string(std::strerror(errno))));
    }

    temporary_state_->temporary_directory = std::move(directory);
    return {};
}

void WorkspaceFileSystem::cleanup_temporary_resources() const noexcept {
    if (!temporary_state_) {
        return;
    }

    std::vector<TemporaryResource> resources;
    {
        std::lock_guard lock(temporary_state_->mutex);
        if (temporary_state_->cleanup_started) {
            return;
        }
        temporary_state_->cleanup_started = true;
        resources = std::move(temporary_state_->resources);
        temporary_state_->resources.clear();
    }

    const auto& directory = temporary_state_->temporary_directory;
    if (!directory) {
        return;
    }

    for (auto iterator = resources.rbegin(); iterator != resources.rend(); ++iterator) {
        auto& resource = *iterator;
        if (!resource.descriptor) {
            continue;
        }

        struct stat owned_status{};
        if (::fstat(resource.descriptor.get(), &owned_status) != 0) {
            continue;
        }
        struct stat current_status{};
        if (::fstatat(directory.get(), resource.name.c_str(), &current_status, AT_SYMLINK_NOFOLLOW) != 0 ||
                !same_object(owned_status, current_status)) {
            continue;
        }

        if (resource.directory) {
            support::UniqueFd resource_directory(
                    ::openat(directory.get(), resource.name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
            if (!resource_directory) {
                continue;
            }
            struct stat opened_status{};
            if (::fstat(resource_directory.get(), &opened_status) != 0 || !same_object(owned_status, opened_status)) {
                continue;
            }
            (void)remove_directory_contents(resource_directory.get());
        }

        struct stat final_status{};
        if (::fstatat(directory.get(), resource.name.c_str(), &final_status, AT_SYMLINK_NOFOLLOW) != 0 ||
                !same_object(owned_status, final_status)) {
            continue;
        }
        const int flags = resource.directory ? AT_REMOVEDIR : 0;
        (void)::unlinkat(directory.get(), resource.name.c_str(), flags);
    }
}

std::expected<std::string, FileError> WorkspaceFileSystem::createTempDir(
    std::optional<std::string> prefix) const {
    const std::string pfx = prefix.value_or("tmp-");
    if (!valid_component(pfx)) {
        return std::unexpected(temporary_error(
            FileErrorCode::Invalid,
            "temporary directory prefix is not a filename component"));
    }

    std::unique_lock lock(temporary_state_->mutex);
    if (temporary_state_->cleanup_started) {
        return std::unexpected(
                temporary_error(FileErrorCode::Aborted, "temporary-resource cleanup has already started"));
    }
    if (auto ensured = ensure_temporary_directory(); !ensured) {
        return std::unexpected(temporary_error(FileErrorCode::PermissionDenied, ensured.error().message));
    }

    const int directory_fd = temporary_state_->temporary_directory.get();
    for (int index = 0; index < 1000; ++index) {
        const auto name = pfx + std::to_string(index);
        if (::mkdirat(directory_fd, name.c_str(), 0700) != 0) {
            if (errno == EEXIST) {
                continue;
            }
            return std::unexpected(temporary_error(
                FileErrorCode::PermissionDenied,
                "could not create temporary directory: " + std::string(std::strerror(errno))));
        }

        support::UniqueFd descriptor(
                ::openat(directory_fd, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (!descriptor) {
            if (errno == ENOENT || errno == ELOOP) {
                continue;
            }
            return std::unexpected(temporary_error(FileErrorCode::PermissionDenied,
                    "could not open temporary directory: " + std::string(std::strerror(errno))));
        }

        temporary_state_->resources.push_back(TemporaryResource{
                .name = name,
                .descriptor = std::move(descriptor),
                .directory = true,
        });
        return (root_ / ".cch-tmp" / name).string();
    }
    return std::unexpected(temporary_error(
        FileErrorCode::Unknown,
        "could not create temp directory"));
}

std::expected<std::string, FileError> WorkspaceFileSystem::createTempFile(
    std::optional<std::string> prefix,
    std::optional<std::string> suffix) const {
    const std::string pfx = prefix.value_or("");
    const std::string sfx = suffix.value_or("");
    if (!valid_component(pfx) || !valid_component(sfx)) {
        return std::unexpected(temporary_error(
            FileErrorCode::Invalid,
            "temporary file prefix and suffix must be filename components"));
    }

    std::unique_lock lock(temporary_state_->mutex);
    if (temporary_state_->cleanup_started) {
        return std::unexpected(
                temporary_error(FileErrorCode::Aborted, "temporary-resource cleanup has already started"));
    }
    if (auto ensured = ensure_temporary_directory(); !ensured) {
        return std::unexpected(temporary_error(FileErrorCode::PermissionDenied, ensured.error().message));
    }

    const int directory_fd = temporary_state_->temporary_directory.get();
    for (int index = 0; index < 1000; ++index) {
        const auto name = pfx + std::to_string(index) + sfx;
        support::UniqueFd descriptor(
                ::openat(directory_fd, name.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
        if (!descriptor) {
            if (errno == EEXIST || errno == ELOOP) {
                continue;
            }
            return std::unexpected(temporary_error(FileErrorCode::PermissionDenied,
                    "could not create temporary file: " + std::string(std::strerror(errno))));
        }

        temporary_state_->resources.push_back(TemporaryResource{
                .name = name,
                .descriptor = std::move(descriptor),
                .directory = false,
        });
        return (root_ / ".cch-tmp" / name).string();
    }
    return std::unexpected(temporary_error(
        FileErrorCode::Unknown,
        "could not create temp file"));
}

} // namespace cch::harness
