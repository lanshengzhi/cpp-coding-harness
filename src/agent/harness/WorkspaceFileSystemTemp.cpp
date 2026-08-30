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

[[nodiscard]] bool is_missing(const std::error_code& error) {
    return error == std::errc::no_such_file_or_directory;
}

} // namespace

bool WorkspaceFileSystem::track_temporary_resource(const std::filesystem::path& path) const {
    if (!temporary_state_ || !inside_lexically(path)) {
        return false;
    }
    std::lock_guard lock(temporary_state_->mutex);
    if (temporary_state_->cleanup_started) {
        return false;
    }
    temporary_state_->resources.push_back(path);
    return true;
}

void WorkspaceFileSystem::cleanup_temporary_resources() const noexcept {
    if (!temporary_state_) {
        return;
    }

    std::vector<std::filesystem::path> resources;
    {
        std::lock_guard lock(temporary_state_->mutex);
        if (temporary_state_->cleanup_started) {
            return;
        }
        temporary_state_->cleanup_started = true;
        resources = std::move(temporary_state_->resources);
        temporary_state_->resources.clear();
    }

    // Only exact paths recorded by this WorkspaceFileSystem are considered.
    // In particular, the shared `.cch-tmp` directory itself is never swept.
    for (auto iterator = resources.rbegin(); iterator != resources.rend(); ++iterator) {
        const auto& resource = *iterator;
        if (!inside_lexically(resource)) {
            continue;
        }
        std::error_code ec;
        const auto status = std::filesystem::symlink_status(resource, ec);
        if (ec) {
            continue;
        }
        if (std::filesystem::is_directory(status) && !std::filesystem::is_symlink(status)) {
            std::filesystem::remove_all(resource, ec);
        } else {
            std::filesystem::remove(resource, ec);
        }
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
    {
        std::lock_guard lock(temporary_state_->mutex);
        if (temporary_state_->cleanup_started) {
            return std::unexpected(temporary_error(
                FileErrorCode::Aborted,
                "temporary-resource cleanup has already started"));
        }
    }

    const auto tmp_area = root_ / ".cch-tmp";
    std::error_code ec;
    auto area_status = std::filesystem::symlink_status(tmp_area, ec);
    if (ec && !is_missing(ec)) {
        return std::unexpected(temporary_error(
            FileErrorCode::PermissionDenied,
            "could not inspect temporary directory: " + ec.message()));
    }
    if (!ec && std::filesystem::is_symlink(area_status)) {
        return std::unexpected(temporary_error(
            FileErrorCode::PermissionDenied,
            "refusing to use a symlink as the temporary directory"));
    }
    if (ec || !std::filesystem::exists(area_status)) {
        if (::mkdir(tmp_area.c_str(), 0700) != 0 && errno != EEXIST) {
            return std::unexpected(temporary_error(
                FileErrorCode::PermissionDenied,
                "could not create temporary directory: " + std::string(std::strerror(errno))));
        }
        area_status = std::filesystem::symlink_status(tmp_area, ec);
    }
    if (ec || !std::filesystem::is_directory(area_status) || std::filesystem::is_symlink(area_status)) {
        return std::unexpected(temporary_error(
            FileErrorCode::PermissionDenied,
            "temporary path is not a directory"));
    }

    for (int index = 0; index < 1000; ++index) {
        const auto candidate = tmp_area / (pfx + std::to_string(index));
        const auto status = std::filesystem::symlink_status(candidate, ec);
        if (!ec && std::filesystem::exists(status)) {
            continue;
        }
        if (ec && !is_missing(ec)) {
            return std::unexpected(temporary_error(
                FileErrorCode::Unknown,
                "could not inspect temporary directory entry: " + ec.message()));
        }
        if (::mkdir(candidate.c_str(), 0700) != 0) {
            if (errno == EEXIST) {
                continue;
            }
            continue;
        }
        if (!track_temporary_resource(candidate)) {
            std::error_code cleanup_error;
            std::filesystem::remove(candidate, cleanup_error);
            return std::unexpected(temporary_error(
                FileErrorCode::Aborted,
                "temporary-resource cleanup started before creation completed"));
        }
        return candidate.string();
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
    {
        std::lock_guard lock(temporary_state_->mutex);
        if (temporary_state_->cleanup_started) {
            return std::unexpected(temporary_error(
                FileErrorCode::Aborted,
                "temporary-resource cleanup has already started"));
        }
    }

    const auto tmp_area = root_ / ".cch-tmp";
    std::error_code ec;
    auto area_status = std::filesystem::symlink_status(tmp_area, ec);
    if (ec && !is_missing(ec)) {
        return std::unexpected(temporary_error(
            FileErrorCode::PermissionDenied,
            "could not inspect temporary directory: " + ec.message()));
    }
    if (!ec && std::filesystem::is_symlink(area_status)) {
        return std::unexpected(temporary_error(
            FileErrorCode::PermissionDenied,
            "refusing to use a symlink as the temporary directory"));
    }
    if (ec || !std::filesystem::exists(area_status)) {
        if (::mkdir(tmp_area.c_str(), 0700) != 0 && errno != EEXIST) {
            return std::unexpected(temporary_error(
                FileErrorCode::PermissionDenied,
                "could not create temporary directory: " + std::string(std::strerror(errno))));
        }
        area_status = std::filesystem::symlink_status(tmp_area, ec);
    }
    if (ec || !std::filesystem::is_directory(area_status) || std::filesystem::is_symlink(area_status)) {
        return std::unexpected(temporary_error(
            FileErrorCode::PermissionDenied,
            "temporary path is not a directory"));
    }

    for (int index = 0; index < 1000; ++index) {
        const auto candidate = tmp_area / (pfx + std::to_string(index) + sfx);
        support::UniqueFd fd(::open(
            candidate.c_str(),
            O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
            0600));
        if (!fd) {
            if (errno == EEXIST) {
                continue;
            }
            continue;
        }
        if (!track_temporary_resource(candidate)) {
            std::error_code cleanup_error;
            std::filesystem::remove(candidate, cleanup_error);
            return std::unexpected(temporary_error(
                FileErrorCode::Aborted,
                "temporary-resource cleanup started before creation completed"));
        }
        return candidate.string();
    }
    return std::unexpected(temporary_error(
        FileErrorCode::Unknown,
        "could not create temp file"));
}

} // namespace cch::harness
