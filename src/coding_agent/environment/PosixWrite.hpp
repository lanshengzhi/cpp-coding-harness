#pragma once

#include <cerrno>
#include <cstddef>
#include <expected>
#include <stop_token>
#include <string_view>

#include <unistd.h>

namespace cch::harness {

enum class PosixWriteErrorKind {
    Write,
    Fsync,
    Cancelled,
};

struct PosixWriteError {
    PosixWriteErrorKind kind;
    int error_number;
};

[[nodiscard]] inline std::expected<void, PosixWriteError> write_all(
        int fd, std::string_view content, std::stop_token stop_token = {}) noexcept {
    const char* data = content.data();
    std::size_t remaining = content.size();
    while (remaining > 0) {
        if (stop_token.stop_requested()) {
            return std::unexpected(PosixWriteError{
                    .kind = PosixWriteErrorKind::Cancelled,
                    .error_number = ECANCELED,
            });
        }
        const auto written = ::write(fd, data, remaining);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return std::unexpected(PosixWriteError{
                    .kind = PosixWriteErrorKind::Write,
                    .error_number = errno,
            });
        }
        if (written == 0) {
            return std::unexpected(PosixWriteError{
                    .kind = PosixWriteErrorKind::Write,
                    .error_number = EIO,
            });
        }
        data += written;
        remaining -= static_cast<std::size_t>(written);
    }
    return {};
}

[[nodiscard]] inline std::expected<void, PosixWriteError> fsync_file(int fd, std::stop_token stop_token = {}) noexcept {
    if (stop_token.stop_requested()) {
        return std::unexpected(PosixWriteError{
                .kind = PosixWriteErrorKind::Cancelled,
                .error_number = ECANCELED,
        });
    }
    while (::fsync(fd) != 0) {
        if (errno == EINTR) {
            if (stop_token.stop_requested()) {
                return std::unexpected(PosixWriteError{
                        .kind = PosixWriteErrorKind::Cancelled,
                        .error_number = ECANCELED,
                });
            }
            continue;
        }
        return std::unexpected(PosixWriteError{
                .kind = PosixWriteErrorKind::Fsync,
                .error_number = errno,
        });
    }
    return {};
}

[[nodiscard]] inline std::expected<void, PosixWriteError> write_all_fsync(
        int fd, std::string_view content, std::stop_token stop_token = {}) noexcept {
    if (auto written = write_all(fd, content, stop_token); !written) {
        return std::unexpected(written.error());
    }
    return fsync_file(fd, stop_token);
}

} // namespace cch::harness
