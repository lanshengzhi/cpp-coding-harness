#pragma once

#include <cch/support/Error.hpp>
#include "PosixWrite.hpp"
#include "support/UniqueFd.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <stop_token>
#include <string>
#include <string_view>
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

namespace detail {

template <typename Writer>
[[nodiscard]] inline support::ExpectedVoid write_atomic_file_at_impl(
        int directory_fd, const std::string& target_filename, Writer writer, std::stop_token stop_token) {
    mode_t mode = S_IRUSR | S_IWUSR;
    struct stat existing_status{};
    if (::fstatat(directory_fd, target_filename.c_str(), &existing_status, AT_SYMLINK_NOFOLLOW) == 0) {
        if (S_ISLNK(existing_status.st_mode)) {
            return std::unexpected(write_error("refusing to write through final symlink"));
        }
        if (S_ISREG(existing_status.st_mode) && (existing_status.st_mode & 0222) == 0) {
            return std::unexpected(write_error("permission denied: target is not writable"));
        }
        mode = existing_status.st_mode & 0777;
    } else if (errno != ENOENT) {
        return std::unexpected(write_error("could not inspect target: " + std::string(std::strerror(errno))));
    }

    std::string temp_filename;
    support::UniqueFd file_fd;
    for (int suffix = 0; suffix < 100; ++suffix) {
        // Keep the temporary name independent of the target basename. A legal
        // NAME_MAX target must not become unwriteable because the target name
        // was copied into the temporary name.
        auto candidate = ".cch-atomic-" + std::to_string(suffix);
        struct stat candidate_status{};
        if (::fstatat(directory_fd, candidate.c_str(), &candidate_status, AT_SYMLINK_NOFOLLOW) == 0) {
            continue;
        }
        if (errno != ENOENT) {
            return std::unexpected(
                    write_error("could not inspect temporary file: " + std::string(std::strerror(errno))));
        }

        const int flags = O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW;
        file_fd.reset(::openat(directory_fd, candidate.c_str(), flags, mode));
        if (!file_fd) {
            if (errno == EEXIST) {
                continue;
            }
            return std::unexpected(
                    write_error("could not create temporary file: " + std::string(std::strerror(errno))));
        }
        temp_filename = std::move(candidate);
        break;
    }
    if (temp_filename.empty()) {
        return std::unexpected(write_error("could not allocate temporary file for atomic write"));
    }

    struct stat temporary_status{};
    if (::fstat(file_fd.get(), &temporary_status) != 0) {
        return std::unexpected(write_error("could not inspect temporary file: " + std::string(std::strerror(errno))));
    }
    if (auto persisted = writer(file_fd.get(), stop_token); !persisted) {
        if (persisted.error().kind == PosixWriteErrorKind::Cancelled) {
            unlink_atomic_temp_if_owned(directory_fd, temp_filename, temporary_status);
            return std::unexpected(
                    support::make_error(support::ErrorCode::Cancelled, "Operation aborted", "Operation aborted"));
        }
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

    if (stop_token.stop_requested()) {
        unlink_atomic_temp_if_owned(directory_fd, temp_filename, temporary_status);
        return std::unexpected(
                support::make_error(support::ErrorCode::Cancelled, "Operation aborted", "Operation aborted"));
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

} // namespace detail

/// Atomically replace a file below an already-opened, no-follow directory.
/// The descriptor keeps every parent component anchored while candidate
/// inspection, creation, and replacement take place.
inline support::ExpectedVoid write_atomic_file_at(int directory_fd,
        const std::string& target_filename,
        std::string_view content,
        std::stop_token stop_token = {}) {
    return detail::write_atomic_file_at_impl(
            directory_fd,
            target_filename,
            [&content](int file_fd, std::stop_token token) { return write_all_fsync(file_fd, content, token); },
            stop_token);
}

/// Atomically append a bounded source descriptor and suffix without retaining
/// a second complete copy of the resulting file in memory.
inline support::ExpectedVoid append_atomic_file_at(int directory_fd,
        const std::string& target_filename,
        int source_fd,
        std::size_t source_bytes,
        std::string_view suffix,
        std::stop_token stop_token = {}) {
    return detail::write_atomic_file_at_impl(
            directory_fd,
            target_filename,
            [source_fd, source_bytes, suffix](
                    int file_fd, std::stop_token token) -> std::expected<void, PosixWriteError> {
                std::size_t remaining = source_bytes;
                char buffer[4096];
                while (remaining > 0) {
                    if (token.stop_requested()) {
                        return std::unexpected(PosixWriteError{
                                .kind = PosixWriteErrorKind::Cancelled,
                                .error_number = ECANCELED,
                        });
                    }
                    const auto requested = std::min<std::size_t>(remaining, sizeof(buffer));
                    const auto count = ::read(source_fd, buffer, requested);
                    if (count < 0) {
                        if (errno == EINTR) {
                            continue;
                        }
                        return std::unexpected(PosixWriteError{
                                .kind = PosixWriteErrorKind::Write,
                                .error_number = errno,
                        });
                    }
                    if (count == 0) {
                        return std::unexpected(PosixWriteError{
                                .kind = PosixWriteErrorKind::Write,
                                .error_number = EIO,
                        });
                    }
                    if (auto written = write_all(
                                file_fd, std::string_view{buffer, static_cast<std::size_t>(count)}, token);
                            !written) {
                        return std::unexpected(written.error());
                    }
                    remaining -= static_cast<std::size_t>(count);
                }
                return write_all(file_fd, suffix, token);
            },
            stop_token);
}

} // namespace cch::harness
