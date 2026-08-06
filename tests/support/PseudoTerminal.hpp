#pragma once

#if defined(__linux__) || defined(__APPLE__)

#include "util/UniqueFd.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace cch::tests {

struct PseudoTerminal {
    util::UniqueFd master;
    util::UniqueFd slave;
    std::string slave_name;
};

[[nodiscard]] inline std::optional<PseudoTerminal> open_pseudo_terminal(
    std::size_t columns = 80,
    std::size_t rows = 24) {
    util::UniqueFd master(::posix_openpt(O_RDWR | O_NOCTTY));
    if (!master || ::grantpt(master.get()) != 0 || ::unlockpt(master.get()) != 0) return std::nullopt;
    const auto* slave_name = ::ptsname(master.get());
    if (slave_name == nullptr) return std::nullopt;
    util::UniqueFd slave(::open(slave_name, O_RDWR | O_NOCTTY));
    if (!slave) return std::nullopt;

    winsize dimensions{
        .ws_row = static_cast<unsigned short>(rows),
        .ws_col = static_cast<unsigned short>(columns),
        .ws_xpixel = 0,
        .ws_ypixel = 0,
    };
    if (::ioctl(slave.get(), TIOCSWINSZ, &dimensions) != 0) return std::nullopt;
    return PseudoTerminal{
        .master = std::move(master),
        .slave = std::move(slave),
        .slave_name = slave_name,
    };
}

[[nodiscard]] inline bool same_terminal_state(const termios& left, const termios& right) {
    return left.c_iflag == right.c_iflag && left.c_oflag == right.c_oflag &&
        left.c_cflag == right.c_cflag && left.c_lflag == right.c_lflag &&
        ::cfgetispeed(&left) == ::cfgetispeed(&right) &&
        ::cfgetospeed(&left) == ::cfgetospeed(&right) &&
        std::memcmp(left.c_cc, right.c_cc, NCCS) == 0;
}

template <typename Predicate>
[[nodiscard]] bool wait_until(
    Predicate predicate,
    std::chrono::milliseconds timeout = std::chrono::seconds(1)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return predicate();
}

[[nodiscard]] inline std::string read_available(
    int descriptor,
    std::chrono::milliseconds timeout = std::chrono::milliseconds(100)) {
    std::string output;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::array<char, 4096> buffer{};
    while (std::chrono::steady_clock::now() < deadline) {
        pollfd item{.fd = descriptor, .events = POLLIN, .revents = 0};
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        const auto ready = ::poll(&item, 1, static_cast<int>(remaining.count()));
        if (ready <= 0 || (item.revents & POLLIN) == 0) break;
        const auto count = ::read(descriptor, buffer.data(), buffer.size());
        if (count <= 0) break;
        output.append(buffer.data(), static_cast<std::size_t>(count));
    }
    return output;
}

} // namespace cch::tests

#endif
