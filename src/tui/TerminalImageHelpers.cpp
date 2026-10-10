#include <cch/tui/TerminalImage.hpp>

#include <cstddef>
#include <cstdint>
#include <format>
#include <random>
#include <string>
#include <string_view>

namespace cch::tui {
namespace {

constexpr std::string_view kKittyPrefix{"\x1b_G"};
constexpr std::string_view kITerm2Prefix{"\x1b]1337;File="};

} // namespace

TerminalImageHandle allocate_image_id() {
    thread_local std::mt19937 generator{std::random_device{}()};
    thread_local std::uniform_int_distribution<std::uint32_t> distribution{1, 0xffffffffU};
    return {.value = distribution(generator)};
}

std::string delete_kitty_image(TerminalImageHandle handle) {
    return std::format("\x1b_Ga=d,d=I,i={},q=2\x1b\\", handle.value);
}

std::string delete_all_kitty_images() { return "\x1b_Ga=d,d=A,q=2\x1b\\"; }

std::string delete_all_kitty_placements() { return "\x1b_Ga=d,d=a,q=2\x1b\\"; }

bool is_image_line(std::string_view line) {
    return line.find(kKittyPrefix) != std::string_view::npos || line.find(kITerm2Prefix) != std::string_view::npos;
}

} // namespace cch::tui
