#pragma once

#include <cerrno>
#include <cstddef>
#include <expected>
#include <string_view>

#include <unistd.h>

namespace cch::harness {

enum class PosixWriteErrorKind {
    Write,
    Fsync,
};

struct PosixWriteError {
    PosixWriteErrorKind kind;
    int error_number;
};

[[nodiscard]] inline std::expected<void, PosixWriteError> write_all_fsync(
    int fd,
    std::string_view content) noexcept {
    const char* data = content.data();
    std::size_t remaining = content.size();
    while (remaining > 0) {
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
    while (::fsync(fd) != 0) {
        if (errno == EINTR) {
            continue;
        }
        return std::unexpected(PosixWriteError{
            .kind = PosixWriteErrorKind::Fsync,
            .error_number = errno,
        });
    }
    return {};
}

} // namespace cch::harness
