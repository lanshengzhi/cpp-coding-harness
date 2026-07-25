#pragma once

// POSIX-only RAII owner for a raw file descriptor. Private to src/harness;
// replaces unique_ptr<int> fd guards that heap-allocated an int per descriptor.

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

namespace cch::harness {

#if defined(__unix__) || defined(__APPLE__)

class UniqueFd final {
public:
    UniqueFd() = default;
    explicit UniqueFd(int fd) noexcept : fd_(fd) {}
    UniqueFd(UniqueFd&& other) noexcept : fd_(other.release()) {}
    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }
    ~UniqueFd() { reset(); }
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] explicit operator bool() const noexcept { return fd_ != -1; }

    // Relinquishes ownership without closing; the caller now owns the fd.
    int release() noexcept {
        const int fd = fd_;
        fd_ = -1;
        return fd;
    }

    // Closes the held descriptor (if any) and reports close(2)'s result so
    // callers can detect deferred write-back failures. Always releases
    // ownership; the descriptor is never closed twice.
    [[nodiscard]] int close() noexcept {
        if (fd_ == -1) {
            return 0;
        }
        const int fd = fd_;
        fd_ = -1;
        return ::close(fd);
    }

    // Closes the held descriptor (if any) and adopts fd.
    void reset(int fd = -1) noexcept {
        if (fd_ != -1) {
            ::close(fd_);
        }
        fd_ = fd;
    }

private:
    int fd_{-1};
};

#endif

} // namespace cch::harness
