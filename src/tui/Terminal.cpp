#include <cch/tui/Terminal.hpp>

#include <cch/support/Error.hpp>
#include <array>
#include <charconv>
#include <limits>
#include <string_view>

namespace cch::tui {

support::ExpectedVoid Terminal::move_cursor_down(std::size_t rows) {
    if (rows == 0) return {};

    std::array<char, std::numeric_limits<std::size_t>::digits10 + 4> sequence{};
    sequence[0] = '\x1b';
    sequence[1] = '[';
    const auto formatted = std::to_chars(sequence.data() + 2, sequence.data() + sequence.size() - 1, rows);
    if (formatted.ec != std::errc{}) {
        return std::unexpected(
                support::make_error(support::ErrorCode::Unknown, "Failed to encode terminal cursor movement"));
    }
    *formatted.ptr = 'B';
    return write(std::string_view(sequence.data(), static_cast<std::size_t>(formatted.ptr - sequence.data()) + 1));
}

} // namespace cch::tui
