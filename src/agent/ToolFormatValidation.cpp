#include "ToolArgumentDetail.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

namespace cch::agent {
namespace {

[[nodiscard]] bool is_ascii_alpha(unsigned char character) {
    return (character >= 'a' && character <= 'z') ||
           (character >= 'A' && character <= 'Z');
}

[[nodiscard]] bool is_ascii_digit(unsigned char character) {
    return character >= '0' && character <= '9';
}

[[nodiscard]] bool is_ascii_hex(unsigned char character) {
    return is_ascii_digit(character) ||
           (character >= 'a' && character <= 'f') ||
           (character >= 'A' && character <= 'F');
}

[[nodiscard]] bool has_valid_percent_encoding(std::string_view value) {
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] != '%') continue;
        if (index + 2 >= value.size() ||
            !is_ascii_hex(static_cast<unsigned char>(value[index + 1])) ||
            !is_ascii_hex(static_cast<unsigned char>(value[index + 2]))) {
            return false;
        }
        index += 2;
    }
    return true;
}

[[nodiscard]] bool has_forbidden_reference_character(std::string_view value, bool ascii_only) {
    for (const unsigned char character : value) {
        if (character <= 0x20 || character == 0x7f || character == '\\' ||
            (ascii_only && character > 0x7f)) {
            return true;
        }
    }
    return false;
}

// The regexes below constrain every matched group to digits, so parsing

// a matched group cannot fail and the from_chars result needs no check.
template <typename T>
[[nodiscard]] T parse_matched_number(const std::ssub_match& match) {
    const char* const first = &*match.first;
    T value{};
    std::from_chars(first, first + match.length(), value);
    return value;
}


[[nodiscard]] bool is_leap_year(int year) {
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

[[nodiscard]] bool is_date_format(std::string_view value) {
    static const std::regex expression(R"(^(\d{4})-(\d{2})-(\d{2})$)");
    std::smatch match;
    const std::string text(value);
    if (!std::regex_match(text, match, expression)) return false;
    const int year = parse_matched_number<int>(match[1]);
    const int month = parse_matched_number<int>(match[2]);
    const int day = parse_matched_number<int>(match[3]);
    constexpr std::array<int, 13> days{0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12 || day < 1) return false;
    const int maximum = month == 2 && is_leap_year(year) ? 29 : days[month];
    return day <= maximum;
}

[[nodiscard]] bool is_time_format(std::string_view value) {
    static const std::regex expression(
        R"(^(\d{2}):(\d{2}):(\d{2}(?:\.\d+)?)(?:[Zz]|([+-])(\d{2}):(\d{2}))$)");
    std::smatch match;
    const std::string text(value);
    if (!std::regex_match(text, match, expression)) return false;
    const int hour = parse_matched_number<int>(match[1]);
    const int minute = parse_matched_number<int>(match[2]);
    const double second = parse_matched_number<double>(match[3]);
    const int zone_sign = match[4].str() == "-" ? -1 : 1;
    const int zone_hour = match[5].matched ? parse_matched_number<int>(match[5]) : 0;
    const int zone_minute = match[6].matched ? parse_matched_number<int>(match[6]) : 0;
    if (zone_hour > 23 || zone_minute > 59) return false;
    if (hour <= 23 && minute <= 59 && second < 60) return true;
    const int utc_minute = minute - zone_minute * zone_sign;
    const int utc_hour = hour - zone_hour * zone_sign - (utc_minute < 0 ? 1 : 0);
    return (utc_hour == 23 || utc_hour == -1) &&
           (utc_minute == 59 || utc_minute == -1) && second < 61;
}

[[nodiscard]] bool is_date_time_format(std::string_view value) {
    const auto separator = value.find_first_of("Tt");
    return separator != std::string_view::npos &&
           value.find_first_of("Tt", separator + 1) == std::string_view::npos &&
           is_date_format(value.substr(0, separator)) &&
           is_time_format(value.substr(separator + 1));
}

[[nodiscard]] bool is_ipv4_format(std::string_view value) {
    int segments = 0;
    std::size_t start = 0;
    while (start <= value.size()) {
        const auto end = value.find('.', start);
        const auto segment = value.substr(
            start,
            end == std::string_view::npos ? value.size() - start : end - start);
        if (segment.empty() || segment.size() > 3 ||
            (segment.size() > 1 && segment.front() == '0')) {
            return false;
        }
        int number = 0;
        for (const unsigned char character : segment) {
            if (!is_ascii_digit(character)) return false;
            number = number * 10 + (character - '0');
        }
        if (number > 255) return false;
        ++segments;
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return segments == 4;
}

[[nodiscard]] bool is_ipv6_format(std::string_view value) {
    if (value.empty()) return false;
    int groups = 0;
    bool compressed = false;
    std::size_t index = 0;
    if (value.starts_with("::")) {
        if (value.size() == 2) return true;
        compressed = true;
        index = 2;
    }
    while (index < value.size()) {
        const auto start = index;
        while (index < value.size() &&
               is_ascii_hex(static_cast<unsigned char>(value[index]))) {
            ++index;
        }
        const auto digits = index - start;
        if (digits == 0) return false;
        if (index < value.size() && value[index] == '.') {
            if (!is_ipv4_format(value.substr(start))) return false;
            groups += 2;
            index = value.size();
            break;
        }
        if (digits > 4) return false;
        ++groups;
        if (index == value.size()) break;
        if (value[index] != ':') return false;
        ++index;
        if (index < value.size() && value[index] == ':') {
            if (compressed || (index + 1 < value.size() && value[index + 1] == ':')) {
                return false;
            }
            compressed = true;
            ++index;
            if (index == value.size()) break;
        }
    }
    return compressed ? groups <= 7 : groups == 8;
}

[[nodiscard]] bool is_ascii_hostname_label(std::string_view label) {
    if (label.empty() || label.size() > 63 || label.front() == '-' || label.back() == '-') {
        return false;
    }
    for (const unsigned char character : label) {
        if (!is_ascii_alpha(character) && !is_ascii_digit(character) && character != '-') {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool is_hostname_format(std::string_view value) {
    if (value.empty() || value.size() > 253 || value.back() == '.') return false;
    std::size_t start = 0;
    while (start <= value.size()) {
        const auto end = value.find('.', start);
        const auto label = value.substr(
            start,
            end == std::string_view::npos ? value.size() - start : end - start);
        if (!is_ascii_hostname_label(label)) return false;
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return true;
}

[[nodiscard]] bool is_unicode_hostname_separator_at(
    std::string_view value,
    std::size_t index) {
    constexpr std::array<std::string_view, 3> separators{
        "\xE3\x80\x82", // U+3002 IDEOGRAPHIC FULL STOP
        "\xEF\xBC\x8E", // U+FF0E FULLWIDTH FULL STOP
        "\xEF\xBD\xA1", // U+FF61 HALFWIDTH IDEOGRAPHIC FULL STOP
    };
    return std::any_of(separators.begin(), separators.end(), [&](std::string_view separator) {
        return value.substr(index).starts_with(separator);
    });
}

[[nodiscard]] bool contains_unicode_hostname_separator(std::string_view value) {
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (is_unicode_hostname_separator_at(value, index)) return true;
    }
    return false;
}

[[nodiscard]] std::string canonicalize_hostname_separators(std::string_view value) {
    std::string canonical;
    canonical.reserve(value.size());
    for (std::size_t index = 0; index < value.size();) {
        if (is_unicode_hostname_separator_at(value, index)) {
            canonical.push_back('.');
            index += 3;
        } else {
            canonical.push_back(value[index]);
            ++index;
        }
    }
    return canonical;
}

[[nodiscard]] std::size_t utf16_code_unit_count(std::string_view value) {
    std::size_t count = 0;
    for (const auto point : decode_utf8(value)) {
        count += point > 0xffff ? 2 : 1;
    }
    return count;
}

[[nodiscard]] bool is_idn_hostname_format(std::string_view value) {
    const auto canonical = canonicalize_hostname_separators(value);
    if (canonical.empty() || utf16_code_unit_count(canonical) > 253 ||
        canonical.find(' ') != std::string::npos) {
        return false;
    }
    const bool all_ascii = std::all_of(
        canonical.begin(), canonical.end(),
        [](unsigned char character) { return character < 0x80; });
    if (all_ascii) return is_hostname_format(canonical);

    std::size_t start = 0;
    while (start <= canonical.size()) {
        const auto end = canonical.find('.', start);
        const auto label = std::string_view(canonical).substr(
            start,
            end == std::string_view::npos ? canonical.size() - start : end - start);
        if (label.empty() || utf16_code_unit_count(label) > 63 ||
            label.front() == '-' || label.back() == '-') {
            return false;
        }
        for (const unsigned char character : label) {
            if (character < 0x20 || character == 0x7f) return false;
        }
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return true;
}


[[nodiscard]] bool is_unicode_letter_or_number(std::uint32_t point) {
    return (point >= 0x00c0 && point <= 0x02ff) ||
           (point >= 0x0370 && point <= 0x052f) ||
           (point >= 0x0531 && point <= 0x058f) ||
           (point >= 0x05d0 && point <= 0x05ea) ||
           (point >= 0x0620 && point <= 0x06ff) ||
           (point >= 0x0710 && point <= 0x08ff) ||
           (point >= 0x0900 && point <= 0x1fff) ||
           (point >= 0x2c00 && point <= 0x2dff) ||
           (point >= 0x3040 && point <= 0xd7af) ||
           (point >= 0xf900 && point <= 0xfaff) ||
           (point >= 0xff10 && point <= 0xff5a) ||
           (point >= 0x10000 && point <= 0x1efff) ||
           (point >= 0x20000 && point <= 0x3134f);
}

[[nodiscard]] bool is_email_format(std::string_view value, bool international) {
    const auto separator = value.find('@');
    if (separator == std::string_view::npos || separator == 0 ||
        separator + 1 == value.size() || value.find('@', separator + 1) != std::string_view::npos) {
        return false;
    }
    const auto local = value.substr(0, separator);
    if (local.front() == '.' || local.back() == '.' ||
        local.find("..") != std::string_view::npos) {
        return false;
    }
    constexpr std::string_view punctuation = "!#$%&'*+/=?^_`{|}~-";
    for (const auto point : utf8_code_points(local)) {
        if (point <= 0x7f) {
            const auto character = static_cast<unsigned char>(point);
            if (is_ascii_alpha(character) || is_ascii_digit(character) || character == '.' ||
                punctuation.find(static_cast<char>(character)) != std::string_view::npos) {
                continue;
            }
            return false;
        }
        if (!international || !is_unicode_letter_or_number(point)) return false;
    }
    const auto domain = value.substr(separator + 1);
    if (international && contains_unicode_hostname_separator(domain)) {
        return false;
    }
    return international ? is_idn_hostname_format(domain)
                         : is_hostname_format(domain);
}

[[nodiscard]] bool has_valid_scheme(std::string_view value) {
    const auto colon = value.find(':');
    if (colon == std::string_view::npos || colon == 0 ||
        !is_ascii_alpha(static_cast<unsigned char>(value.front()))) {
        return false;
    }
    for (std::size_t index = 1; index < colon; ++index) {
        const auto character = static_cast<unsigned char>(value[index]);
        if (!is_ascii_alpha(character) && !is_ascii_digit(character) &&
            character != '+' && character != '-' && character != '.') {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool is_uri_unreserved(unsigned char character) {
    return is_ascii_alpha(character) || is_ascii_digit(character) ||
           character == '-' || character == '.' || character == '_' || character == '~';
}

[[nodiscard]] bool is_uri_sub_delimiter(unsigned char character) {
    constexpr std::string_view delimiters = "!$&'()*+,;=";
    return delimiters.find(static_cast<char>(character)) != std::string_view::npos;
}

[[nodiscard]] bool is_uri_path_character(unsigned char character) {
    return is_uri_unreserved(character) || is_uri_sub_delimiter(character) ||
           character == ':' || character == '@';
}

[[nodiscard]] bool is_baseline_uri(std::string_view value) {
    if (!has_valid_scheme(value)) return false;
    std::size_t index = value.find(':') + 1;

    if (value.substr(index).starts_with("//")) {
        index += 2;
        const auto authority_start = index;
        auto at = std::string_view::npos;
        for (std::size_t cursor = index; cursor < value.size(); ++cursor) {
            if (value[cursor] == '@') {
                at = cursor;
                break;
            }
            if (value[cursor] == '/' || value[cursor] == '?' || value[cursor] == '#') break;
        }
        if (at != std::string_view::npos) {
            for (std::size_t cursor = authority_start; cursor < at; ++cursor) {
                const auto character = static_cast<unsigned char>(value[cursor]);
                if (character == '[' || character == ']') return false;
                if (character == '%') {
                    if (cursor + 2 >= at ||
                        !is_ascii_hex(static_cast<unsigned char>(value[cursor + 1])) ||
                        !is_ascii_hex(static_cast<unsigned char>(value[cursor + 2]))) {
                        return false;
                    }
                    cursor += 2;
                } else if (!is_uri_unreserved(character) &&
                           !is_uri_sub_delimiter(character) && character != ':') {
                    return false;
                }
            }
            index = at + 1;
        }

        if (index < value.size() && value[index] == '[') {
            ++index;
            while (index < value.size() && value[index] != ']') ++index;
            if (index == value.size()) return false;
            ++index;
        } else {
            while (index < value.size()) {
                const auto character = static_cast<unsigned char>(value[index]);
                if (character == '/' || character == '?' || character == '#' || character == ':') {
                    break;
                }
                if (character < 0x80 &&
                    !is_uri_unreserved(character) && !is_uri_sub_delimiter(character)) {
                    return false;
                }
                ++index;
            }
        }
        if (index < value.size() && value[index] == ':') {
            ++index;
            while (index < value.size()) {
                const auto character = static_cast<unsigned char>(value[index]);
                if (character == '/' || character == '?' || character == '#') break;
                if (!is_ascii_digit(character)) return false;
                ++index;
            }
        }
    }

    while (index < value.size()) {
        const auto character = static_cast<unsigned char>(value[index]);
        if (character == '%') {
            if (index + 2 >= value.size() ||
                !is_ascii_hex(static_cast<unsigned char>(value[index + 1])) ||
                !is_ascii_hex(static_cast<unsigned char>(value[index + 2]))) {
                return false;
            }
            index += 3;
            continue;
        }
        if (character > 0x7f ||
            (!is_uri_path_character(character) && character != '/' &&
             character != '?' && character != '#')) {
            return false;
        }
        ++index;
    }
    return true;
}

[[nodiscard]] bool is_baseline_iri(std::string_view value) {
    if (!has_valid_scheme(value)) return false;
    const auto colon = value.find(':');
    const auto scheme = value.substr(0, colon);
    const auto remainder = value.substr(colon + 1);
    if (remainder.empty()) return false;
    if (scheme == "http" || scheme == "https" || scheme == "ftp" ||
        scheme == "ws" || scheme == "wss") {
        if (!remainder.starts_with("//")) return false;
        const auto host_start = colon + 3;
        const auto host_end = value.find_first_of("/?#", host_start);
        if (host_start == value.size() || host_end == host_start) return false;
    }
    for (const unsigned char character : value) {
        if (character < 0x20 || character == 0x7f || character == '\\') return false;
    }
    return true;
}

[[nodiscard]] bool is_uri_format(std::string_view value, bool allow_unicode) {
    return allow_unicode ? is_baseline_iri(value) : is_baseline_uri(value);
}

[[nodiscard]] bool is_uri_reference_format(std::string_view value, bool allow_unicode) {
    if (!allow_unicode) {
        constexpr std::string_view forbidden = "[]{}<>^`|";
        for (const unsigned char character : value) {
            if (character > 0x7f || character <= 0x20 || character == 0x7f ||
                character == '\\' ||
                forbidden.find(static_cast<char>(character)) != std::string_view::npos) {
                return false;
            }
        }
        return true;
    }

    if (has_forbidden_reference_character(value, false) ||
        !has_valid_percent_encoding(value)) {
        return false;
    }
    if (value.empty()) return true;
    if (has_valid_scheme(value)) return is_uri_format(value, true);
    if (value.size() >= 3 && is_ascii_alpha(static_cast<unsigned char>(value.front()))) {
        const auto slash_pair = value.find("//");
        const auto colon = value.find(':');
        if (slash_pair != std::string_view::npos && colon == std::string_view::npos &&
            slash_pair > 0 && value[slash_pair - 1] != '/') {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool is_json_pointer(std::string_view value) {
    if (value.empty()) return true;
    if (value.front() != '/') return false;
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] != '~') continue;
        if (index + 1 >= value.size() ||
            (value[index + 1] != '0' && value[index + 1] != '1')) {
            return false;
        }
        ++index;
    }
    return true;
}

[[nodiscard]] bool is_json_pointer_uri_fragment(std::string_view value) {
    if (value.empty() || value.front() != '#') return false;
    const auto pointer = value.substr(1);
    if (!is_json_pointer(pointer) || !has_valid_percent_encoding(pointer)) return false;
    for (const unsigned char character : pointer) {
        if (character <= 0x20 || character >= 0x7f) return false;
    }
    return true;
}

[[nodiscard]] bool is_relative_json_pointer(std::string_view value) {
    std::size_t index = 0;
    while (index < value.size() && is_ascii_digit(static_cast<unsigned char>(value[index]))) {
        ++index;
    }
    if (index == 0 || (index > 1 && value.front() == '0')) return false;
    if (index == value.size()) return true;
    if (value[index] == '#') return index + 1 == value.size();
    return is_json_pointer(value.substr(index));
}

[[nodiscard]] bool is_uri_template_format(std::string_view value) {
    constexpr std::string_view operators = "+#./;?&=,!@|";
    constexpr std::string_view kForbiddenLiterals = "\"'<>\\^`{|}";
    std::size_t index = 0;
    while (index < value.size()) {
        const auto character = static_cast<unsigned char>(value[index]);
        if (character == '%') {
            if (index + 2 >= value.size() ||
                !is_ascii_hex(static_cast<unsigned char>(value[index + 1])) ||
                !is_ascii_hex(static_cast<unsigned char>(value[index + 2]))) {
                return false;
            }
            index += 3;
            continue;
        }
        if (character != '{') {
            if (character <= 0x20 || kForbiddenLiterals.find(static_cast<char>(character)) != std::string_view::npos) {
                return false;
            }
            ++index;
            continue;
        }

        ++index;
        if (index < value.size() &&
            operators.find(value[index]) != std::string_view::npos) {
            ++index;
        }
        bool expect_variable = true;
        while (true) {
            const auto variable_start = index;
            while (index < value.size()) {
                const auto variable_character = static_cast<unsigned char>(value[index]);
                if (is_ascii_alpha(variable_character) || is_ascii_digit(variable_character) ||
                    variable_character == '_') {
                    ++index;
                    continue;
                }
                if (variable_character == '%' && index + 2 < value.size() &&
                    is_ascii_hex(static_cast<unsigned char>(value[index + 1])) &&
                    is_ascii_hex(static_cast<unsigned char>(value[index + 2]))) {
                    index += 3;
                    continue;
                }
                break;
            }
            if (index == variable_start) return false;
            expect_variable = false;

            if (index < value.size() && value[index] == ':') {
                ++index;
                const auto modifier_start = index;
                if (index >= value.size() || value[index] < '1' || value[index] > '9') return false;
                while (index < value.size() && is_ascii_digit(static_cast<unsigned char>(value[index])) &&
                       index - modifier_start < 4) {
                    ++index;
                }
                if (index < value.size() && is_ascii_digit(static_cast<unsigned char>(value[index]))) {
                    return false;
                }
            } else if (index < value.size() && value[index] == '*') {
                ++index;
            }

            if (index < value.size() && value[index] == ',') {
                ++index;
                expect_variable = true;
                continue;
            }
            break;
        }
        if (expect_variable || index >= value.size() || value[index] != '}') return false;
        ++index;
    }
    return true;
}

[[nodiscard]] bool is_public_url_ipv4(std::string_view host) {
    if (!is_ipv4_format(host)) return false;
    std::array<int, 4> octets{};
    std::size_t start = 0;
    for (std::size_t index = 0; index < octets.size(); ++index) {
        const auto end = host.find('.', start);
        const auto length = end == std::string_view::npos ? host.size() - start : end - start;
        std::from_chars(host.data() + start, host.data() + start + length, octets[index]);
        start = end == std::string_view::npos ? host.size() : end + 1;
    }
    if (octets[0] == 10 || octets[0] == 127 ||
        (octets[0] == 169 && octets[1] == 254) ||
        (octets[0] == 192 && octets[1] == 168) ||
        (octets[0] == 172 && octets[1] >= 16 && octets[1] <= 31)) {
        return false;
    }
    return octets[0] >= 1 && octets[0] <= 223 &&
           octets[3] >= 1 && octets[3] <= 254;
}

[[nodiscard]] bool is_url_format(std::string_view value) {
    const auto colon = value.find(':');
    if (colon == std::string_view::npos) return false;
    std::string scheme(value.substr(0, colon));
    std::transform(scheme.begin(), scheme.end(), scheme.begin(), [](unsigned char character) {
        return character >= 'A' && character <= 'Z'
                   ? static_cast<char>(character - 'A' + 'a')
                   : static_cast<char>(character);
    });
    if (scheme != "http" && scheme != "https" && scheme != "ftp") return false;
    if (!value.substr(colon + 1).starts_with("//")) return false;
    for (const unsigned char character : value) {
        if (character <= 0x20 || character == 0x7f) return false;
    }

    const auto authority_start = colon + 3;
    const auto authority_end = value.find('/', authority_start);
    auto authority = value.substr(
        authority_start,
        authority_end == std::string_view::npos
            ? value.size() - authority_start
            : authority_end - authority_start);
    if (authority.empty() || authority.find_first_of("?#") != std::string_view::npos) return false;
    if (const auto at = authority.rfind('@'); at != std::string_view::npos) {
        if (at == 0 || at + 1 == authority.size()) return false;
        authority.remove_prefix(at + 1);
    }

    auto host = authority;
    if (const auto port = authority.rfind(':'); port != std::string_view::npos) {
        host = authority.substr(0, port);
        const auto digits = authority.substr(port + 1);
        if (digits.size() < 2 || digits.size() > 5 ||
            !std::all_of(digits.begin(), digits.end(), [](unsigned char character) {
                return is_ascii_digit(character);
            })) {
            return false;
        }
    }
    if (host.empty()) return false;
    if (is_ipv4_format(host)) return is_public_url_ipv4(host);

    std::vector<std::string_view> labels;
    std::size_t start = 0;
    while (start <= host.size()) {
        const auto end = host.find('.', start);
        const auto label = host.substr(
            start,
            end == std::string_view::npos ? host.size() - start : end - start);
        if (label.empty() || label.front() == '-' || label.back() == '-') return false;
        for (const unsigned char character : label) {
            if (!is_ascii_alpha(character) && !is_ascii_digit(character) &&
                character != '-' && character < 0x80) {
                return false;
            }
        }
        labels.push_back(label);
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    if (labels.size() < 2) return false;
    const auto top_level = utf8_code_points(labels.back());
    if (top_level.size() < 2 ||
        !std::all_of(top_level.begin(), top_level.end(), [](std::uint32_t point) {
            return point > 0x7f || is_ascii_alpha(static_cast<unsigned char>(point));
        })) {
        return false;
    }
    return true;
}
[[nodiscard]] bool is_combining_modifier(std::uint32_t point) {
    return (point >= 0x0300 && point <= 0x036f) ||
           (point >= 0x1ab0 && point <= 0x1aff) ||
           (point >= 0x1dc0 && point <= 0x1dff) ||
           (point >= 0xfe20 && point <= 0xfe2f) ||
           (point >= 0xfe00 && point <= 0xfe0f);
}

[[nodiscard]] std::size_t grapheme_count(std::string_view value) {
    const auto points = utf8_code_points(value);
    std::size_t count = 0;
    for (std::size_t index = 0; index < points.size();) {
        const auto first = points[index++];
        while (index < points.size() && is_combining_modifier(points[index])) ++index;
        while (index + 1 < points.size() && points[index] == 0x200d) {
            index += 2;
            while (index < points.size() && is_combining_modifier(points[index])) ++index;
        }
        if (first >= 0x1f1e6 && first <= 0x1f1ff && index < points.size() &&
            points[index] >= 0x1f1e6 && points[index] <= 0x1f1ff) {
            ++index;
        }
        ++count;
    }
    return count;
}

[[nodiscard]] bool triggers_typebox_grapheme_fallback(std::uint32_t point) {
    return point > 0xffff ||
           (point >= 0x0300 && point <= 0x036f) ||
           point == 0x200d;
}


} // namespace

[[nodiscard]] bool matches_format(FormatKind format, std::string_view value) {
    switch (format) {
    case FormatKind::DateTime:
        return is_date_time_format(value);
    case FormatKind::Date:
        return is_date_format(value);
    case FormatKind::Duration: {
        static const std::regex expression(
            R"(^P((\d+Y(\d+M(\d+D)?)?|\d+M(\d+D)?|\d+D)(T(\d+H(\d+M(\d+S)?)?|\d+M(\d+S)?|\d+S))?|T(\d+H(\d+M(\d+S)?)?|\d+M(\d+S)?|\d+S)|\d+W)$)");
        return std::regex_match(value.begin(), value.end(), expression);
    }
    case FormatKind::Email:
        return is_email_format(value, false);
    case FormatKind::Hostname:
        return is_hostname_format(value);
    case FormatKind::IdnEmail:
        return is_email_format(value, true);
    case FormatKind::IdnHostname:
        return is_idn_hostname_format(value);
    case FormatKind::Ipv4:
        return is_ipv4_format(value);
    case FormatKind::Ipv6:
        return is_ipv6_format(value);
    case FormatKind::IriReference:
        return is_uri_reference_format(value, true);
    case FormatKind::Iri:
        return is_uri_format(value, true);
    case FormatKind::JsonPointerUriFragment:
        return is_json_pointer_uri_fragment(value);
    case FormatKind::JsonPointer:
        return is_json_pointer(value);
    case FormatKind::Regex:
        if (value.empty()) return false;
        // libstdc++ cannot report a malformed pattern without exceptions
        // (issue #487): reject structurally invalid patterns before any
        // `std::regex` construction, same as SessionSelectorSearch.
        if (!structurally_valid_regex(value)) return false;
        (void)std::regex(std::string(value), std::regex::ECMAScript);
        return true;
    case FormatKind::RelativeJsonPointer:
        return is_relative_json_pointer(value);
    case FormatKind::Time:
        return is_time_format(value);
    case FormatKind::UriReference:
        return is_uri_reference_format(value, false);
    case FormatKind::UriTemplate:
        return is_uri_template_format(value);
    case FormatKind::Uri:
        return is_uri_format(value, false);
    case FormatKind::Url:
        return is_url_format(value);
    case FormatKind::Uuid: {
        static const std::regex expression(
            R"(^(?:urn:uuid:)?[0-9a-fA-F]{8}-(?:[0-9a-fA-F]{4}-){3}[0-9a-fA-F]{12}$)");
        return std::regex_match(value.begin(), value.end(), expression);
    }
    }
    return false;
}


[[nodiscard]] bool typebox_min_length_matches(
    std::string_view value,
    std::size_t min_length) {
    if (min_length == 0) return true;
    std::size_t fast_length = 0;
    for (const auto point : utf8_code_points(value)) {
        if (triggers_typebox_grapheme_fallback(point)) {
            return grapheme_count(value) >= min_length;
        }
        ++fast_length;
        if (fast_length >= min_length) return true;
    }
    return false;
}

[[nodiscard]] bool typebox_max_length_matches(
    std::string_view value,
    std::size_t max_length) {
    std::size_t fast_length = 0;
    for (const auto point : utf8_code_points(value)) {
        if (triggers_typebox_grapheme_fallback(point)) {
            return grapheme_count(value) <= max_length;
        }
        ++fast_length;
        if (fast_length > max_length) return false;
    }
    return true;
}



} // namespace cch::agent
