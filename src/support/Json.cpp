#include "Json.hpp"

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace cch::support {
namespace {

/// Hand-rolled JSON parser producing the passive `support::JsonValue`. This is
/// the non-serialization replacement for the Glaze-backed `read_json<JsonValue>`
/// (build-performance-plan Stage 5): it must stay byte-compatible with the
/// values the Glaze path produced for every input the suite exercises.
///
/// Matched Glaze behaviors: strict JSON grammar (leading zeros, bare `.`/`e`
/// and dangling commas are rejected; numbers overflow/underflow the double
/// range on parse error), `\uXXXX` escapes including surrogate pairs (lone
/// surrogates are rejected), duplicate object keys keep the last value, and
/// any trailing content after the first complete value is ignored.
struct Parser {
    std::string_view text;
    std::size_t index{0};

    [[nodiscard]] Error fail(std::string detail) const {
        return make_error(
            ErrorCode::JsonParse,
            "failed to parse JSON",
            "syntax error at offset " + std::to_string(index) + ": " + std::move(detail),
            std::string(text));
    }

    void skip_ws() {
        while (index < text.size() &&
               (text[index] == ' ' || text[index] == '\t' || text[index] == '\n' ||
                text[index] == '\r')) {
            ++index;
        }
    }

    [[nodiscard]] bool consume(char expected) {
        if (index < text.size() && text[index] == expected) {
            ++index;
            return true;
        }
        return false;
    }

    [[nodiscard]] Expected<JsonValue> parse_value();

    [[nodiscard]] Expected<JsonValue> parse_object() {
        ++index; // '{'
        skip_ws();
        JsonValue::object_t object;
        if (consume('}')) {
            return JsonValue{std::move(object)};
        }
        while (true) {
            skip_ws();
            auto key = parse_string();
            if (!key) {
                return std::unexpected(key.error());
            }
            skip_ws();
            if (!consume(':')) {
                return std::unexpected(fail("expected ':' after object key"));
            }
            auto value = parse_value();
            if (!value) {
                return std::unexpected(value.error());
            }
            object.insert_or_assign(std::move(*key), std::move(*value));
            skip_ws();
            if (consume('}')) {
                return JsonValue{std::move(object)};
            }
            if (!consume(',')) {
                return std::unexpected(fail("expected ',' or '}' in object"));
            }
            skip_ws();
            if (index < text.size() && text[index] == '}') {
                return std::unexpected(fail("trailing comma in object"));
            }
        }
    }

    [[nodiscard]] Expected<JsonValue> parse_array() {
        ++index; // '['
        skip_ws();
        JsonValue::array_t array;
        if (consume(']')) {
            return JsonValue{std::move(array)};
        }
        while (true) {
            auto value = parse_value();
            if (!value) {
                return std::unexpected(value.error());
            }
            array.push_back(std::move(*value));
            skip_ws();
            if (consume(']')) {
                return JsonValue{std::move(array)};
            }
            if (!consume(',')) {
                return std::unexpected(fail("expected ',' or ']' in array"));
            }
            skip_ws();
            if (index < text.size() && text[index] == ']') {
                return std::unexpected(fail("trailing comma in array"));
            }
        }
    }

    /// Append the code point encoded as UTF-8.
    static void append_utf8(std::string& out, std::uint32_t code_point) {
        if (code_point < 0x80) {
            out.push_back(static_cast<char>(code_point));
        } else if (code_point < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
            out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
        } else if (code_point < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (code_point >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (code_point >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
        }
    }

    [[nodiscard]] std::optional<std::uint32_t> parse_hex4() {
        if (index + 4 > text.size()) {
            return std::nullopt;
        }
        std::uint32_t value = 0;
        for (std::size_t i = 0; i < 4; ++i) {
            const char c = text[index + static_cast<std::size_t>(i)];
            value <<= 4;
            if (c >= '0' && c <= '9') {
                value |= static_cast<std::uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                value |= static_cast<std::uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                value |= static_cast<std::uint32_t>(c - 'A' + 10);
            } else {
                return std::nullopt;
            }
        }
        index += 4;
        return value;
    }

    [[nodiscard]] Expected<std::string> parse_string() {
        if (!consume('"')) {
            return std::unexpected(fail("expected string"));
        }
        std::string out;
        while (index < text.size()) {
            const char c = text[index];
            if (c == '"') {
                ++index;
                return out;
            }
            if (c == '\\') {
                ++index;
                if (index >= text.size()) {
                    return std::unexpected(fail("unterminated escape sequence"));
                }
                const char escaped = text[index];
                switch (escaped) {
                case '"':
                    out.push_back('"');
                    ++index;
                    break;
                case '\\':
                    out.push_back('\\');
                    ++index;
                    break;
                case '/':
                    out.push_back('/');
                    ++index;
                    break;
                case 'b':
                    out.push_back('\b');
                    ++index;
                    break;
                case 'f':
                    out.push_back('\f');
                    ++index;
                    break;
                case 'n':
                    out.push_back('\n');
                    ++index;
                    break;
                case 'r':
                    out.push_back('\r');
                    ++index;
                    break;
                case 't':
                    out.push_back('\t');
                    ++index;
                    break;
                case 'u': {
                    ++index;
                    auto first = parse_hex4();
                    if (!first) {
                        return std::unexpected(fail("invalid \\u escape"));
                    }
                    if (*first >= 0xD800 && *first <= 0xDBFF) {
                        // High surrogate: a following low surrogate is required.
                        if (index + 1 < text.size() && text[index] == '\\' && text[index + 1] == 'u') {
                            index += 2;
                            auto second = parse_hex4();
                            if (!second || *second < 0xDC00 || *second > 0xDFFF) {
                                return std::unexpected(fail("invalid surrogate pair"));
                            }
                            const std::uint32_t code_point =
                                0x10000 + ((*first - 0xD800) << 10) + (*second - 0xDC00);
                            append_utf8(out, code_point);
                        } else {
                            return std::unexpected(fail("unpaired high surrogate"));
                        }
                    } else if (*first >= 0xDC00 && *first <= 0xDFFF) {
                        return std::unexpected(fail("unpaired low surrogate"));
                    } else {
                        append_utf8(out, *first);
                    }
                    break;
                }
                default:
                    return std::unexpected(fail("invalid escape sequence"));
                }
                continue;
            }
            if (static_cast<unsigned char>(c) < 0x20U) {
                return std::unexpected(fail("unescaped control character in string"));
            }
            out.push_back(c);
            ++index;
        }
        return std::unexpected(fail("unterminated string"));
    }

    [[nodiscard]] Expected<double> parse_number() {
        const std::size_t start = index;
        // Optional leading minus: consume it when present; the caller
        // validates the remaining number shape below. The [[nodiscard]]
        // result is intentionally unused here.
        (void)consume('-');
        if (index >= text.size()) {
            return std::unexpected(fail("invalid number"));
        }
        if (text[index] == '0') {
            ++index;
            if (index < text.size() && text[index] >= '0' && text[index] <= '9') {
                return std::unexpected(fail("leading zero in number"));
            }
        } else if (text[index] >= '1' && text[index] <= '9') {
            while (index < text.size() && text[index] >= '0' && text[index] <= '9') {
                ++index;
            }
        } else {
            return std::unexpected(fail("invalid number"));
        }
        if (index < text.size() && text[index] == '.') {
            ++index;
            if (index >= text.size() || text[index] < '0' || text[index] > '9') {
                return std::unexpected(fail("fraction requires digits"));
            }
            while (index < text.size() && text[index] >= '0' && text[index] <= '9') {
                ++index;
            }
        }
        if (index < text.size() && (text[index] == 'e' || text[index] == 'E')) {
            const std::size_t exponent_marker = index;
            ++index;
            if (index < text.size() && (text[index] == '+' || text[index] == '-')) {
                ++index;
            }
            if (index >= text.size() || text[index] < '0' || text[index] > '9') {
                // Like Glaze, an exponent marker without digits is not part of
                // the number; the token ends before the 'e' (top-level trailing
                // content is ignored, container delimiters reject it below).
                index = exponent_marker;
            } else {
                while (index < text.size() && text[index] >= '0' && text[index] <= '9') {
                    ++index;
                }
            }
        }
        const std::string_view token = text.substr(start, index - start);
        double value = 0.0;
        const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value);
        if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size()) {
            return std::unexpected(fail("number out of range"));
        }
        return value;
    }
};

[[nodiscard]] Expected<JsonValue> Parser::parse_value() {
    skip_ws();
    if (index >= text.size()) {
        return std::unexpected(fail("unexpected end of input"));
    }
    const char c = text[index];
    if (c == '"') {
        auto string = parse_string();
        if (!string) {
            return std::unexpected(string.error());
        }
        return JsonValue{std::move(*string)};
    }
    if (c == '{') {
        return parse_object();
    }
    if (c == '[') {
        return parse_array();
    }
    if (c == 't') {
        if (text.substr(index, 4) == "true") {
            index += 4;
            return JsonValue{true};
        }
        return std::unexpected(fail("invalid literal"));
    }
    if (c == 'f') {
        if (text.substr(index, 5) == "false") {
            index += 5;
            return JsonValue{false};
        }
        return std::unexpected(fail("invalid literal"));
    }
    if (c == 'n') {
        if (text.substr(index, 4) == "null") {
            index += 4;
            return JsonValue{nullptr};
        }
        return std::unexpected(fail("invalid literal"));
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        auto number = parse_number();
        if (!number) {
            return std::unexpected(number.error());
        }
        return JsonValue{*number};
    }
    return std::unexpected(fail("unexpected character"));
}

/// Serialize one `JsonValue` compactly, byte-identical to the previous
/// `glz::write_json(json_to_glaze(value))` output. Reproduced Glaze number
/// rules: shortest round-trip digits; fixed notation while the first digit's
/// decimal exponent is in [-4, 15] and scientific (uppercase `E`, no `+`,
/// no leading zero) outside that range; `-0` for negative zero; `null` for
/// non-finite. Strings escape only `"`, `\\`, `\b`, `\f`, `\n`, `\r`, `\t`;
/// every other byte (including control characters and UTF-8) is emitted raw,
/// matching Glaze's writer.
[[nodiscard]] std::string format_number(double value) {
    if (std::isnan(value) || std::isinf(value)) {
        return "null";
    }
    if (value == 0.0) {
        return std::signbit(value) ? "-0" : "0";
    }
    char buffer[64];
    const auto converted = std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::scientific);
    if (converted.ec != std::errc{}) {
        // Unreachable for finite doubles; keep a safe fallback rather than
        // indexing a failed conversion.
        return "null";
    }
    // `std::to_chars` does not null-terminate and the bytes past `ptr` are
    // unspecified stack garbage; every read below must stay below `end`.
    const char* const end = converted.ptr;
    const char* cursor = buffer;
    std::string digits;
    bool negative = false;
    if (cursor < end && *cursor == '-') {
        negative = true;
        ++cursor;
    }
    if (cursor < end) {
        digits.push_back(*cursor++);
    }
    if (cursor < end && *cursor == '.') {
        ++cursor;
        while (cursor < end && *cursor >= '0' && *cursor <= '9') {
            digits.push_back(*cursor++);
        }
    }
    if (cursor < end && (*cursor == 'e' || *cursor == 'E')) {
        ++cursor;
    }
    bool exponent_negative = false;
    if (cursor < end && (*cursor == '+' || *cursor == '-')) {
        exponent_negative = *cursor == '-';
        ++cursor;
    }
    int exponent = 0;
    while (cursor < end && *cursor >= '0' && *cursor <= '9') {
        exponent = exponent * 10 + (*cursor - '0');
        ++cursor;
    }
    const int decimal_exponent = exponent_negative ? -exponent : exponent;
    while (digits.size() > 1 && digits.back() == '0') {
        digits.pop_back();
    }

    std::string out;
    if (negative) {
        out += '-';
    }
    if (decimal_exponent >= -4 && decimal_exponent <= 15) {
        if (decimal_exponent >= 0) {
            const std::size_t integer_length = static_cast<std::size_t>(decimal_exponent) + 1;
            if (digits.size() <= integer_length) {
                out += digits;
                out.append(integer_length - digits.size(), '0');
            } else {
                out.append(digits, 0, integer_length);
                out += '.';
                out.append(digits, integer_length, std::string::npos);
            }
        } else {
            out += "0.";
            out.append(static_cast<std::size_t>(-decimal_exponent - 1), '0');
            out += digits;
        }
    } else {
        out += digits[0];
        if (digits.size() > 1) {
            out += '.';
            out.append(digits, 1, std::string::npos);
        }
        out += 'E';
        const int magnitude = decimal_exponent < 0 ? -decimal_exponent : decimal_exponent;
        if (decimal_exponent < 0) {
            out += '-';
        }
        char exponent_buffer[16];
        std::size_t written = 0;
        int remaining = magnitude;
        do {
            exponent_buffer[written++] = static_cast<char>('0' + (remaining % 10));
            remaining /= 10;
        } while (remaining > 0);
        while (written > 0) {
            out += exponent_buffer[--written];
        }
    }
    return out;
}

[[nodiscard]] std::string write_value(const JsonValue& value) {
    if (const auto* object = value.get_if<JsonValue::object_t>()) {
        std::string out = "{";
        bool first = true;
        for (const auto& [key, member] : *object) {
            if (!first) {
                out += ',';
            }
            first = false;
            out += write_value(JsonValue{key});
            out += ':';
            out += write_value(member);
        }
        out += '}';
        return out;
    }
    if (const auto* array = value.get_if<JsonValue::array_t>()) {
        std::string out = "[";
        bool first = true;
        for (const auto& element : *array) {
            if (!first) {
                out += ',';
            }
            first = false;
            out += write_value(element);
        }
        out += ']';
        return out;
    }
    if (const auto* text = value.get_if<std::string>()) {
        std::string out = "\"";
        for (const unsigned char c : *text) {
            switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                out.push_back(static_cast<char>(c));
                break;
            }
        }
        out += '"';
        return out;
    }
    if (const auto* flag = value.get_if<bool>()) {
        return *flag ? "true" : "false";
    }
    if (const auto* number = value.get_if<double>()) {
        return format_number(*number);
    }
    return "null";
}

} // namespace

Expected<JsonValue> read_json(std::string_view json) {
    Parser parser{json, 0};
    return parser.parse_value();
}

Expected<std::string> write_json(const JsonValue& value) {
    return write_value(value);
}

} // namespace cch::support
