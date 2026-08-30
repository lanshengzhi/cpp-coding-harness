#pragma once

#include <cch/support/Error.hpp>
#include "PosixWrite.hpp"
#include "support/UniqueFd.hpp"

#include <string>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace cch::harness {

inline support::Error write_error(std::string message) {
    return support::make_error(support::ErrorCode::Workspace, message, message);
}

[[nodiscard]] inline bool atomic_write_same_object(const struct stat& left, const struct stat& right) noexcept {
    return left.st_dev == right.st_dev && left.st_ino == right.st_ino &&
           (left.st_mode & S_IFMT) == (right.st_mode & S_IFMT);
}

inline void unlink_atomic_temp_if_owned(
        int directory_fd, const std::string& filename, const struct stat& expected) noexcept {
    struct stat current{};
    if (::fstatat(directory_fd, filename.c_str(), &current, AT_SYMLINK_NOFOLLOW) != 0 ||
            !atomic_write_same_object(expected, current)) {
        return;
    }
    (void)::unlinkat(directory_fd, filename.c_str(), 0);
}

/// Atomically replace a file below an already-opened, no-follow directory.
/// The descriptor keeps every parent component anchored while candidate
/// inspection, creation, and replacement take place.
inline support::ExpectedVoid write_atomic_file_at(
        int directory_fd, const std::string& target_filename, const std::string& content) {
    mode_t mode = S_IRUSR | S_IWUSR;
    struct stat existing_status {};
    if (::fstatat(directory_fd, target_filename.c_str(), &existing_status, AT_SYMLINK_NOFOLLOW) == 0) {
        if (S_ISLNK(existing_status.st_mode)) {
            return std::unexpected(write_error("refusing to write through final symlink"));
        }
        mode = existing_status.st_mode & 0777;
    } else if (errno != ENOENT) {
        return std::unexpected(write_error("could not inspect target: " + std::string(std::strerror(errno))));
    }

    std::string temp_filename;
    for (int suffix = 0; suffix < 100; ++suffix) {
        auto candidate = "." + target_filename + ".tmp-" + std::to_string(suffix);
        struct stat candidate_status{};
        if (::fstatat(directory_fd, candidate.c_str(), &candidate_status, AT_SYMLINK_NOFOLLOW) == 0) {
            continue;
        }
        if (errno != ENOENT) {
            return std::unexpected(
                    write_error("could not inspect temporary file: " + std::string(std::strerror(errno))));
        }
        temp_filename = std::move(candidate);
        break;
    }
    if (temp_filename.empty()) {
        return std::unexpected(write_error("could not allocate temporary file for atomic write"));
    }

    const int flags = O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW;
    support::UniqueFd file_fd(::openat(directory_fd, temp_filename.c_str(), flags, mode));
    if (!file_fd) {
        return std::unexpected(write_error("could not create temporary file: " + std::string(std::strerror(errno))));
    }
    struct stat temporary_status{};
    if (::fstat(file_fd.get(), &temporary_status) != 0) {
        return std::unexpected(write_error("could not inspect temporary file: " + std::string(std::strerror(errno))));
    }
    if (auto persisted = write_all_fsync(file_fd.get(), content); !persisted) {
        const auto message = std::string(std::strerror(persisted.error().error_number));
        unlink_atomic_temp_if_owned(directory_fd, temp_filename, temporary_status);
        if (persisted.error().kind == PosixWriteErrorKind::Write) {
            return std::unexpected(write_error("could not write temporary file: " + message));
        }
        return std::unexpected(write_error("could not flush temporary file: " + message));
    }
    if (file_fd.close() != 0) {
        const auto message = std::string(std::strerror(errno));
        unlink_atomic_temp_if_owned(directory_fd, temp_filename, temporary_status);
        return std::unexpected(write_error("could not close temporary file: " + message));
    }

    struct stat target_status{};
    if (::fstatat(directory_fd, target_filename.c_str(), &target_status, AT_SYMLINK_NOFOLLOW) == 0 &&
            S_ISLNK(target_status.st_mode)) {
        unlink_atomic_temp_if_owned(directory_fd, temp_filename, temporary_status);
        return std::unexpected(write_error("refusing to write through final symlink"));
    }
    if (::renameat(directory_fd, temp_filename.c_str(), directory_fd, target_filename.c_str()) != 0) {
        const auto message = std::string(std::strerror(errno));
        unlink_atomic_temp_if_owned(directory_fd, temp_filename, temporary_status);
        return std::unexpected(write_error("could not replace target atomically: " + message));
    }
    return {};
}

} // namespace cch::harness
