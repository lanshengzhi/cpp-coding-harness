#include <cch/tui/TerminalColors.hpp>

#include <charconv>
#include <cmath>

namespace cch::tui {

namespace {

[[nodiscard]] bool case_insensitive_equals(char left, char right) noexcept {
    if (left >= 'A' && left <= 'Z') left = static_cast<char>(left + ('a' - 'A'));
    if (right >= 'A' && right <= 'Z') right = static_cast<char>(right + ('a' - 'A'));
    return left == right;
}

[[nodiscard]] bool starts_with_case_insensitive(std::string_view value, std::string_view prefix) noexcept {
    if (value.size() < prefix.size()) return false;
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (!case_insensitive_equals(value[i], prefix[i])) return false;
    }
    return true;
}

} // namespace

std::optional<int> parse_osc_hex_channel(std::string_view channel) {
    if (channel.empty() || channel.size() > 8) return std::nullopt;
    std::uint64_t value = 0;
    std::uint64_t maximum = 0;
    for (const auto character : channel) {
        int digit = 0;
        if (character >= '0' && character <= '9') {
            digit = character - '0';
        } else if (character >= 'a' && character <= 'f') {
            digit = 10 + character - 'a';
        } else if (character >= 'A' && character <= 'F') {
            digit = 10 + character - 'A';
        } else {
            return std::nullopt;
        }
        value = value * 16 + static_cast<std::uint64_t>(digit);
        maximum = maximum * 16 + 15;
    }
    if (maximum == 0) return std::nullopt;
    return static_cast<int>(std::round(static_cast<double>(value) / static_cast<double>(maximum) * 255.0));
}

std::optional<RgbColor> parse_osc_color_value(std::string_view raw_value) {
    while (!raw_value.empty() && (raw_value.front() == ' ' || raw_value.front() == '\t' ||
                                  raw_value.front() == '\r' || raw_value.front() == '\n')) {
        raw_value.remove_prefix(1);
    }
    while (!raw_value.empty() && (raw_value.back() == ' ' || raw_value.back() == '\t' ||
                                  raw_value.back() == '\r' || raw_value.back() == '\n')) {
        raw_value.remove_suffix(1);
    }
    if (raw_value.empty()) return std::nullopt;

    if (raw_value.front() == '#') {
        const auto hex = raw_value.substr(1);
        if (hex.size() == 6) {
            const auto r = parse_osc_hex_channel(hex.substr(0, 2));
            const auto g = parse_osc_hex_channel(hex.substr(2, 2));
            const auto b = parse_osc_hex_channel(hex.substr(4, 2));
            if (!r || !g || !b) return std::nullopt;
            return RgbColor{
                .red = static_cast<double>(*r),
                .green = static_cast<double>(*g),
                .blue = static_cast<double>(*b),
            };
        }
        if (hex.size() == 12) {
            const auto r = parse_osc_hex_channel(hex.substr(0, 4));
            const auto g = parse_osc_hex_channel(hex.substr(4, 4));
            const auto b = parse_osc_hex_channel(hex.substr(8, 4));
            if (!r || !g || !b) return std::nullopt;
            return RgbColor{
                .red = static_cast<double>(*r),
                .green = static_cast<double>(*g),
                .blue = static_cast<double>(*b),
            };
        }
        return std::nullopt;
    }

    std::string_view rgb_value = raw_value;
    if (starts_with_case_insensitive(rgb_value, "rgba:")) {
        rgb_value.remove_prefix(5);
    } else if (starts_with_case_insensitive(rgb_value, "rgb:")) {
        rgb_value.remove_prefix(4);
    } else {
        return std::nullopt;
    }

    const auto first_slash = rgb_value.find('/');
    if (first_slash == std::string_view::npos) return std::nullopt;
    const auto second_slash = rgb_value.find('/', first_slash + 1);
    if (second_slash == std::string_view::npos) return std::nullopt;

    const auto red_part = rgb_value.substr(0, first_slash);
    const auto green_part = rgb_value.substr(first_slash + 1, second_slash - first_slash - 1);
    const auto after_second = rgb_value.substr(second_slash + 1);
    const auto third_slash = after_second.find('/');
    const auto blue_part = (third_slash == std::string_view::npos) ? after_second : after_second.substr(0, third_slash);

    const auto r = parse_osc_hex_channel(red_part);
    const auto g = parse_osc_hex_channel(green_part);
    const auto b = parse_osc_hex_channel(blue_part);
    if (!r || !g || !b) return std::nullopt;
    return RgbColor{
        .red = static_cast<double>(*r),
        .green = static_cast<double>(*g),
        .blue = static_cast<double>(*b),
    };
}

std::optional<OscColorResponse> parse_osc_color_response(std::string_view data) {
    if (!data.starts_with("\x1b]")) return std::nullopt;
    std::string_view inner;
    if (data.ends_with('\x07')) {
        inner = data.substr(2, data.size() - 3);
    } else if (data.ends_with("\x1b\\")) {
        inner = data.substr(2, data.size() - 4);
    } else {
        return std::nullopt;
    }

    OscColorTarget target;
    std::string_view raw_value;

    if (inner.starts_with("10;")) {
        target.kind = OscColorTargetKind::Foreground;
        raw_value = inner.substr(3);
    } else if (inner.starts_with("11;")) {
        target.kind = OscColorTargetKind::Background;
        raw_value = inner.substr(3);
    } else if (inner.starts_with("4;")) {
        const auto semi = inner.find(';', 2);
        if (semi == std::string_view::npos) return std::nullopt;
        const auto index_str = inner.substr(2, semi - 2);
        if (index_str.empty() || index_str.size() > 3) return std::nullopt;
        int palette_index = 0;
        const auto [end, error] = std::from_chars(index_str.data(), index_str.data() + index_str.size(), palette_index);
        if (error != std::errc{} || end != index_str.data() + index_str.size() || palette_index < 0) {
            return std::nullopt;
        }
        target.kind = OscColorTargetKind::Palette;
        target.palette_index = palette_index;
        raw_value = inner.substr(semi + 1);
    } else {
        return std::nullopt;
    }

    if (raw_value.find('\x07') != std::string_view::npos || raw_value.find('\x1b') != std::string_view::npos) {
        return std::nullopt;
    }

    return OscColorResponse{
        .target = target,
        .rgb = parse_osc_color_value(raw_value),
    };
}

std::optional<TerminalColorScheme> parse_terminal_color_scheme_report(std::string_view data) {
    if (data.empty()) return std::nullopt;
    std::optional<TerminalColorScheme> last;
    std::size_t offset = 0;
    while (offset < data.size()) {
        const auto remaining = data.substr(offset);
        if (!remaining.starts_with("\x1b[?997;")) return std::nullopt;
        const auto n_pos = remaining.find('n');
        if (n_pos == std::string_view::npos) return std::nullopt;
        const auto code = remaining.substr(7, n_pos - 7);
        if (code == "1") {
            last = TerminalColorScheme::Dark;
        } else if (code == "2") {
            last = TerminalColorScheme::Light;
        } else {
            return std::nullopt;
        }
        offset += n_pos + 1;
    }
    return last;
}

std::string build_terminal_color_query() {
    std::string query = "\x1b]10;?\x07\x1b]11;?\x07";
    for (int i = 0; i < 16; ++i) {
        query += "\x1b]4;";
        query += std::to_string(i);
        query += ";?\x07";
    }
    query += "\x1b[c";
    return query;
}

} // namespace cch::tui
