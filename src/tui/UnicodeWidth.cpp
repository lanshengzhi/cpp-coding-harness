#include "tui/UnicodeWidth.hpp"

#include <cch/support/Error.hpp>
#include <utf8proc.h>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <format>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace cch::tui::detail {
namespace {

[[nodiscard]] bool is_regional_indicator(char32_t codepoint) {
    return codepoint >= 0x1F1E6 && codepoint <= 0x1F1FF;
}

[[nodiscard]] std::size_t utf8_sequence_length(unsigned char lead) {
    if (lead < 0x80) return 1;
    if ((lead & 0xE0) == 0xC0) return 2;
    if ((lead & 0xF0) == 0xE0) return 3;
    if ((lead & 0xF8) == 0xF0) return 4;
    return 0;
}

[[nodiscard]] bool parse_sgr_number(std::string_view parameter, int& value) {
    if (parameter.empty()) {
        value = 0;
        return true;
    }
    const auto* first = parameter.data();
    const auto* last = first + parameter.size();
    const auto [end, error] = std::from_chars(first, last, value);
    return error == std::errc{} && end == last;
}

[[nodiscard]] bool validate_sgr_parameters(std::string_view parameters) {
    std::vector<int> values;
    std::size_t start = 0;
    while (start <= parameters.size()) {
        const auto separator = parameters.find(';', start);
        const auto end = separator == std::string_view::npos ? parameters.size() : separator;
        int value = 0;
        if (!parse_sgr_number(parameters.substr(start, end - start), value)) return false;
        values.push_back(value);
        if (separator == std::string_view::npos) break;
        start = separator + 1;
    }

    const auto is_color_channel = [](int value) { return value >= 0 && value <= 255; };
    for (std::size_t index = 0; index < values.size(); ++index) {
        const auto value = values[index];
        if (value == 0 || (value >= 1 && value <= 5) || value == 7 || value == 8 || value == 9 ||
            (value >= 22 && value <= 25) || (value >= 27 && value <= 29) ||
            (value >= 30 && value <= 37) || value == 39 ||
            (value >= 40 && value <= 47) || value == 49 ||
            (value >= 90 && value <= 97) || (value >= 100 && value <= 107)) {
            continue;
        }
        if (value != 38 && value != 48) return false;
        if (index + 2 < values.size() && values[index + 1] == 5 &&
            is_color_channel(values[index + 2])) {
            index += 2;
            continue;
        }
        if (index + 4 < values.size() && values[index + 1] == 2 &&
            is_color_channel(values[index + 2]) && is_color_channel(values[index + 3]) &&
            is_color_channel(values[index + 4])) {
            index += 4;
            continue;
        }
        return false;
    }
    return true;
}

[[nodiscard]] support::Expected<AnsiCode> parse_supported_ansi(
    std::string_view text,
    std::size_t position) {
    if (position >= text.size() || text[position] != '\x1b') {
        return std::unexpected(invalid_terminal_text("Expected an ANSI escape sequence"));
    }
    if (position + 1 >= text.size()) {
        return std::unexpected(invalid_terminal_text("Incomplete ANSI escape sequence"));
    }

    const auto kind = text[position + 1];
    if (kind == '[') {
        std::size_t cursor = position + 2;
        while (cursor < text.size()) {
            const auto byte = static_cast<unsigned char>(text[cursor]);
            if (byte >= 0x40 && byte <= 0x7E) break;
            if (!((byte >= '0' && byte <= '9') || byte == ';')) {
                return std::unexpected(invalid_terminal_text("Unsupported CSI sequence"));
            }
            ++cursor;
        }
        if (cursor >= text.size()) {
            return std::unexpected(invalid_terminal_text("Incomplete CSI sequence"));
        }
        if (text[cursor] != 'm') {
            return std::unexpected(invalid_terminal_text("Only SGR CSI sequences are supported"));
        }
        const auto parameters = text.substr(position + 2, cursor - position - 2);
        if (!validate_sgr_parameters(parameters)) {
            return std::unexpected(invalid_terminal_text("Malformed SGR parameters"));
        }
        return AnsiCode{
            .code = std::string(text.substr(position, cursor + 1 - position)),
            .length = cursor + 1 - position,
        };
    }

    if (kind == ']') {
        std::size_t cursor = position + 2;
        std::size_t terminator_length = 0;
        while (cursor < text.size()) {
            const auto byte = static_cast<unsigned char>(text[cursor]);
            if (byte == 0x07) {
                terminator_length = 1;
                break;
            }
            if (byte == 0x1B && cursor + 1 < text.size() && text[cursor + 1] == '\\') {
                terminator_length = 2;
                break;
            }
            if (byte < 0x20 || byte == 0x7F) {
                return std::unexpected(invalid_terminal_text("Unsupported control in OSC sequence"));
            }
            ++cursor;
        }
        if (terminator_length == 0) {
            return std::unexpected(invalid_terminal_text("Incomplete OSC sequence"));
        }
        const auto body = text.substr(position + 2, cursor - position - 2);
        // OSC 8 hyperlinks carry `8;<params>;<uri>` bodies. OSC 133 prompt
        // zones (`133;A`/`133;B`/`133;C`, pi's coding-agent user/assistant
        // message wrappers) are accepted as zero-width control tokens so
        // message lines can carry them through the differential renderer.
        const auto is_hyperlink =
            body.starts_with("8;") && body.find(';', 2) != std::string_view::npos;
        const auto is_osc133 =
            (body == "133;A" || body == "133;B" || body == "133;C");
        if (!is_hyperlink && !is_osc133) {
            return std::unexpected(invalid_terminal_text(
                "Only OSC 8 hyperlinks and OSC 133 zones are supported"));
        }
        return AnsiCode{
            .code = std::string(text.substr(position, cursor + terminator_length - position)),
            .length = cursor + terminator_length - position,
        };
    }

    return std::unexpected(invalid_terminal_text("Unsupported ANSI escape sequence"));
}

[[nodiscard]] std::size_t next_grapheme_end(std::string_view text, std::size_t position) {
    const auto [first, first_bytes] = decode_utf8(text, position);
    if (first_bytes == 0) return position;

    auto previous = static_cast<utf8proc_int32_t>(first);
    auto end = position + first_bytes;
    utf8proc_int32_t state = 0;
    while (end < text.size()) {
        if (text[end] == '\x1b' || text[end] == '\r' || text[end] == '\n' || text[end] == '\t') break;
        const auto [current, current_bytes] = decode_utf8(text, end);
        if (current_bytes == 0 || current < 0x20 || current == 0x7F ||
            (current >= 0x80 && current <= 0x9F)) {
            break;
        }
        const auto current_utf8proc = static_cast<utf8proc_int32_t>(current);
        if (utf8proc_grapheme_break_stateful(previous, current_utf8proc, &state) != 0) break;
        end += current_bytes;
        previous = current_utf8proc;
    }
    return end;
}

} // namespace

std::pair<char32_t, std::size_t> decode_utf8(std::string_view text, std::size_t position) {
    if (position >= text.size()) return {0xFFFD, 0};

    const auto lead = static_cast<unsigned char>(text[position]);
    const auto length = utf8_sequence_length(lead);
    if (length == 1) return {lead, 1};
    if (length == 0 || position + length > text.size()) return {0xFFFD, 1};

    char32_t codepoint = 0;
    for (std::size_t index = 1; index < length; ++index) {
        const auto continuation = static_cast<unsigned char>(text[position + index]);
        if ((continuation & 0xC0) != 0x80) return {0xFFFD, 1};
    }
    if (length == 2) {
        codepoint = ((lead & 0x1F) << 6) |
                    (static_cast<unsigned char>(text[position + 1]) & 0x3F);
        if (codepoint < 0x80) return {0xFFFD, 1};
    } else if (length == 3) {
        codepoint = ((lead & 0x0F) << 12) |
                    ((static_cast<unsigned char>(text[position + 1]) & 0x3F) << 6) |
                    (static_cast<unsigned char>(text[position + 2]) & 0x3F);
        if (codepoint < 0x800 || (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
            return {0xFFFD, 1};
        }
    } else {
        codepoint = ((lead & 0x07) << 18) |
                    ((static_cast<unsigned char>(text[position + 1]) & 0x3F) << 12) |
                    ((static_cast<unsigned char>(text[position + 2]) & 0x3F) << 6) |
                    (static_cast<unsigned char>(text[position + 3]) & 0x3F);
        if (codepoint < 0x10000 || codepoint > 0x10FFFF) return {0xFFFD, 1};
    }
    return {codepoint, length};
}

std::size_t codepoint_width(char32_t codepoint) {
    if (codepoint == '\t') return 3;
    const auto width = utf8proc_charwidth(static_cast<utf8proc_int32_t>(codepoint));
    return width > 0 ? static_cast<std::size_t>(width) : 0;
}

std::size_t grapheme_width(std::string_view cluster) {
    std::size_t width = 0;
    std::size_t trailing_width = 0;
    std::size_t position = 0;
    std::size_t regional_indicators = 0;
    char32_t base_codepoint{0};
    bool has_base = false;
    bool has_emoji_base = false;
    bool has_vs16 = false;
    bool has_keycap = false;
    bool has_joiner = false;

    while (position < cluster.size()) {
        const auto [codepoint, bytes] = decode_utf8(cluster, position);
        if (bytes == 0) break;
        const auto* property = utf8proc_get_property(static_cast<utf8proc_int32_t>(codepoint));
        const auto is_emoji_base =
            property->boundclass == UTF8PROC_BOUNDCLASS_EXTENDED_PICTOGRAPHIC ||
            property->boundclass == UTF8PROC_BOUNDCLASS_E_BASE ||
            property->boundclass == UTF8PROC_BOUNDCLASS_E_BASE_GAZ;
        const auto codepoint_columns = codepoint_width(codepoint);
        const auto adds_trailing_width =
            has_base && ((codepoint >= 0xFF00 && codepoint <= 0xFFEF) ||
                         codepoint == 0x0E33 || codepoint == 0x0EB3);
        if (adds_trailing_width) {
            trailing_width += codepoint == 0x0E33 || codepoint == 0x0EB3
                                  ? 1
                                  : codepoint_columns;
        } else {
            width = std::max(width, codepoint_columns);
        }
        if (!has_base && codepoint_columns > 0) {
            base_codepoint = codepoint;
            has_base = true;
        }
        regional_indicators += is_regional_indicator(codepoint) ? 1 : 0;
        has_emoji_base = has_emoji_base || is_emoji_base;
        has_vs16 = has_vs16 || codepoint == 0xFE0F;
        has_keycap = has_keycap || codepoint == 0x20E3;
        has_joiner = has_joiner || codepoint == 0x200D;
        position += bytes;
    }

    const auto is_keycap_base = has_base &&
                                (base_codepoint == '#' || base_codepoint == '*' ||
                                 (base_codepoint >= '0' && base_codepoint <= '9'));
    if (regional_indicators > 0 || (has_vs16 && has_emoji_base) ||
        (has_keycap && is_keycap_base) || (has_joiner && has_emoji_base)) {
        return 2;
    }
    return width + trailing_width;
}

std::optional<AnsiCode> extract_ansi_code(std::string_view text, std::size_t position) {
    if (position >= text.size() || text[position] != '\x1b') return std::nullopt;
    auto parsed = parse_supported_ansi(text, position);
    if (!parsed) return std::nullopt;
    return std::move(*parsed);
}

support::Expected<std::vector<TerminalToken>> tokenize_terminal_output(std::string_view text) {
    std::vector<TerminalToken> tokens;
    std::size_t position = 0;
    while (position < text.size()) {
        const auto byte = static_cast<unsigned char>(text[position]);
        if (byte == 0x1B) {
            auto ansi = parse_supported_ansi(text, position);
            if (!ansi) return std::unexpected(ansi.error());
            tokens.push_back(TerminalToken{
                .kind = ansi->code.starts_with("\x1b[")
                            ? TerminalTokenKind::Sgr
                            : TerminalTokenKind::Hyperlink,
                .text = std::move(ansi->code),
                .width = 0,
            });
            position += ansi->length;
            continue;
        }
        if (text[position] == '\r') {
            if (position + 1 < text.size() && text[position + 1] == '\n') ++position;
            tokens.push_back({.kind = TerminalTokenKind::Newline, .text = "\n", .width = 0});
            ++position;
            continue;
        }
        if (text[position] == '\n') {
            tokens.push_back({.kind = TerminalTokenKind::Newline, .text = "\n", .width = 0});
            ++position;
            continue;
        }
        if (text[position] == '\t') {
            for (std::size_t count = 0; count < 3; ++count) {
                tokens.push_back({.kind = TerminalTokenKind::Grapheme, .text = " ", .width = 1});
            }
            ++position;
            continue;
        }

        const auto [codepoint, bytes] = decode_utf8(text, position);
        if (bytes == 0) break;
        if (codepoint == 0xFFFD) {
            tokens.push_back({
                .kind = TerminalTokenKind::Grapheme,
                .text = "\xef\xbf\xbd",
                .width = 1,
            });
            position += bytes;
            continue;
        }
        if ((codepoint < 0x20) || codepoint == 0x7F ||
            (codepoint >= 0x80 && codepoint <= 0x9F)) {
            return std::unexpected(invalid_terminal_text(
                "Unsupported control character in terminal text",
                std::format("U+{:04X}", static_cast<unsigned>(codepoint))));
        }

        const auto end = next_grapheme_end(text, position);
        auto cluster = std::string(text.substr(position, end - position));
        tokens.push_back({
            .kind = TerminalTokenKind::Grapheme,
            .text = std::move(cluster),
            .width = grapheme_width(text.substr(position, end - position)),
        });
        position = end;
    }
    return tokens;
}

std::vector<std::string> split_graphemes(std::string_view text) {
    auto tokens = tokenize_terminal_output(text);
    if (!tokens) return {};
    std::vector<std::string> result;
    for (auto& token : *tokens) {
        if (token.kind == TerminalTokenKind::Grapheme) result.push_back(std::move(token.text));
    }
    return result;
}

std::string AnsiStyleState::get_active_codes() const {
    std::vector<std::string> parameters;
    if (bold) parameters.emplace_back("1");
    if (dim) parameters.emplace_back("2");
    if (italic) parameters.emplace_back("3");
    if (underline) parameters.emplace_back("4");
    if (blink) parameters.emplace_back("5");
    if (inverse) parameters.emplace_back("7");
    if (hidden) parameters.emplace_back("8");
    if (strikethrough) parameters.emplace_back("9");
    if (!fg_color.empty()) parameters.push_back(fg_color);
    if (!bg_color.empty()) parameters.push_back(bg_color);

    std::string result;
    if (!parameters.empty()) {
        result = "\x1b[";
        for (std::size_t index = 0; index < parameters.size(); ++index) {
            if (index != 0) result += ';';
            result += parameters[index];
        }
        result += 'm';
    }
    if (!hyperlink.empty()) {
        result += "\x1b]8;" + hyperlink_params + ";" + hyperlink + "\x07";
    }
    return result;
}

bool AnsiStyleState::has_sgr_codes() const {
    return bold || dim || italic || underline || blink || inverse || hidden ||
           strikethrough || !fg_color.empty() || !bg_color.empty();
}

bool AnsiStyleState::has_active_codes() const {
    return has_sgr_codes() || !hyperlink.empty();
}

std::string AnsiStyleState::get_line_end_reset() const {
    std::string result;
    if (!hyperlink.empty()) result += "\x1b]8;;\x07";
    if (has_sgr_codes()) result += "\x1b[0m";
    return result;
}

void AnsiStyleState::process_ansi(std::string_view code) {
    if (code.starts_with("\x1b]8;")) {
        const auto terminator_size = code.ends_with('\x07') ? 1U : 2U;
        if (code.size() < 5 + terminator_size) return;
        const auto body = code.substr(4, code.size() - 4 - terminator_size);
        const auto separator = body.find(';');
        if (separator == std::string_view::npos) return;
        hyperlink_params = std::string(body.substr(0, separator));
        hyperlink = std::string(body.substr(separator + 1));
        if (hyperlink.empty()) hyperlink_params.clear();
        return;
    }
    if (!code.starts_with("\x1b[") || !code.ends_with('m')) return;

    const auto source = code.substr(2, code.size() - 3);
    std::vector<int> parameters;
    std::size_t start = 0;
    while (start <= source.size()) {
        const auto separator = source.find(';', start);
        const auto end = separator == std::string_view::npos ? source.size() : separator;
        int value = 0;
        if (!parse_sgr_number(source.substr(start, end - start), value)) return;
        parameters.push_back(value);
        if (separator == std::string_view::npos) break;
        start = separator + 1;
    }
    if (parameters.empty()) parameters.push_back(0);

    for (std::size_t index = 0; index < parameters.size(); ++index) {
        const auto value = parameters[index];
        switch (value) {
        case 0: reset(); break;
        case 1: bold = true; break;
        case 2: dim = true; break;
        case 3: italic = true; break;
        case 4: underline = true; break;
        case 5: blink = true; break;
        case 7: inverse = true; break;
        case 8: hidden = true; break;
        case 9: strikethrough = true; break;
        case 22: bold = false; dim = false; break;
        case 23: italic = false; break;
        case 24: underline = false; break;
        case 25: blink = false; break;
        case 27: inverse = false; break;
        case 28: hidden = false; break;
        case 29: strikethrough = false; break;
        case 39: fg_color.clear(); break;
        case 49: bg_color.clear(); break;
        default:
            if ((value >= 30 && value <= 37) || (value >= 90 && value <= 97)) {
                fg_color = std::to_string(value);
            } else if ((value >= 40 && value <= 47) || (value >= 100 && value <= 107)) {
                bg_color = std::to_string(value);
            } else if ((value == 38 || value == 48) && index + 2 < parameters.size() &&
                       parameters[index + 1] == 5) {
                const auto color = std::format("{};5;{}", value, parameters[index + 2]);
                (value == 38 ? fg_color : bg_color) = color;
                index += 2;
            } else if ((value == 38 || value == 48) && index + 4 < parameters.size() &&
                       parameters[index + 1] == 2) {
                const auto color = std::format(
                    "{};2;{};{};{}",
                    value,
                    parameters[index + 2],
                    parameters[index + 3],
                    parameters[index + 4]);
                (value == 38 ? fg_color : bg_color) = color;
                index += 4;
            }
            break;
        }
    }
}

void AnsiStyleState::reset() {
    bold = false;
    dim = false;
    italic = false;
    underline = false;
    blink = false;
    inverse = false;
    hidden = false;
    strikethrough = false;
    fg_color.clear();
    bg_color.clear();
}

support::Expected<std::string> normalize_terminal_output(std::string_view text) {
    auto tokens = tokenize_terminal_output(text);
    if (!tokens) return std::unexpected(tokens.error());
    return normalized_text(*tokens);
}

support::Expected<std::string> prepare_rendered_line(std::string_view line, std::size_t width) {
    auto tokens = tokenize_terminal_output(line);
    if (!tokens) return std::unexpected(tokens.error());
    auto line_width = token_width(*tokens);
    if (!line_width) return std::unexpected(line_width.error());
    if (*line_width > width) {
        return std::unexpected(invalid_terminal_text(
            "TUI component rendered a line wider than its width bound",
            std::format("line width {} exceeds visible width {}", *line_width, width)));
    }

    AnsiStyleState style;
    auto result = normalized_text(*tokens);
    for (const auto& token : *tokens) {
        if (token.kind != TerminalTokenKind::Grapheme) style.process_ansi(token.text);
    }
    result += style.get_line_end_reset();
    return result;
}

} // namespace cch::tui::detail
