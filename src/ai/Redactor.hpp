#pragma once

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>

namespace cch::ai {

inline constexpr std::string_view kRedactionMarker{"[REDACTED]"};

[[nodiscard]] inline std::string normalized_secret_key(std::string_view key) {
    std::string normalized;
    normalized.reserve(key.size());
    for (const unsigned char ch : key) {
        if (std::isalnum(ch)) {
            normalized.push_back(static_cast<char>(std::toupper(ch)));
        }
    }
    return normalized;
}

[[nodiscard]] inline bool looks_secret_key(std::string_view key) {
    const auto normalized = normalized_secret_key(key);
    if (normalized == "TOTALTOKENS" || normalized == "TOKENSBEFORE") {
        return false;
    }
    return normalized.find("APIKEY") != std::string::npos || normalized.find("TOKEN") != std::string::npos ||
           normalized.find("SECRET") != std::string::npos || normalized.find("PASSWORD") != std::string::npos ||
           normalized.find("AUTHORIZATION") != std::string::npos || normalized.find("OPENAI") != std::string::npos;
}

namespace redactor_detail {

inline bool is_key_character(unsigned char ch) {
    return std::isalnum(ch) || ch == '_' || ch == '-';
}

inline void redact_prefixed_tokens(
    std::string& text,
    std::string_view prefix,
    std::size_t required_suffix,
    bool uppercase_digits_only = false) {
    std::size_t position = 0;
    while ((position = text.find(prefix, position)) != std::string::npos) {
        std::size_t end = position + prefix.size();
        while (end < text.size() && is_key_character(static_cast<unsigned char>(text[end]))) {
            if (uppercase_digits_only && !(std::isdigit(static_cast<unsigned char>(text[end])) ||
                                           (text[end] >= 'A' && text[end] <= 'Z'))) {
                break;
            }
            ++end;
        }
        if (end - position - prefix.size() < required_suffix) {
            position += prefix.size();
            continue;
        }
        text.replace(position, end - position, kRedactionMarker);
        position += kRedactionMarker.size();
    }
}

inline std::optional<std::string> assignment_key(
    const std::string& text,
    std::size_t separator) {
    std::size_t end = separator;
    while (end > 0 && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    if (end == 0) {
        return std::nullopt;
    }

    if (text[end - 1] == '"') {
        if (end < 2) {
            return std::nullopt;
        }
        const auto begin_quote = text.rfind('"', end - 2);
        if (begin_quote == std::string::npos) {
            return std::nullopt;
        }
        return text.substr(begin_quote + 1, end - begin_quote - 2);
    }

    std::size_t begin = end;
    while (begin > 0 && is_key_character(static_cast<unsigned char>(text[begin - 1]))) {
        --begin;
    }
    if (begin == end) {
        return std::nullopt;
    }
    return text.substr(begin, end - begin);
}

inline void redact_assignments(std::string& text) {
    std::size_t separator = 0;
    while ((separator = text.find_first_of(":=", separator)) != std::string::npos) {
        auto key = assignment_key(text, separator);
        if (!key || !looks_secret_key(*key)) {
            ++separator;
            continue;
        }

        std::size_t value_begin = separator + 1;
        while (value_begin < text.size() && std::isspace(static_cast<unsigned char>(text[value_begin]))) {
            ++value_begin;
        }
        if (value_begin == text.size()) {
            break;
        }

        std::size_t value_end = value_begin;
        const char quote = text[value_begin] == '"' || text[value_begin] == '\''
            ? text[value_begin]
            : '\0';
        const bool quoted = quote != '\0';
        if (quoted) {
            ++value_begin;
            value_end = value_begin;
            while (value_end < text.size()) {
                if (text[value_end] == '\\' && value_end + 1 < text.size()) {
                    value_end += 2;
                    continue;
                }
                if (text[value_end] == quote) {
                    break;
                }
                ++value_end;
            }
        } else {
            const bool authorization = normalized_secret_key(*key).find("AUTHORIZATION") != std::string::npos;
            while (value_end < text.size() && text[value_end] != ',' && text[value_end] != '}' &&
                   text[value_end] != '\n' && text[value_end] != '\r' &&
                   (authorization || !std::isspace(static_cast<unsigned char>(text[value_end])))) {
                ++value_end;
            }
        }

        text.replace(value_begin, value_end - value_begin, kRedactionMarker);
        separator = value_begin + kRedactionMarker.size() + (quoted ? 1 : 0);
    }
}

} // namespace redactor_detail

[[nodiscard]] inline std::string redact_text(std::string text) {
    redactor_detail::redact_assignments(text);
    redactor_detail::redact_prefixed_tokens(text, "sk-", 8);
    redactor_detail::redact_prefixed_tokens(text, "AKIA", 16, true);
    return text;
}

} // namespace cch::ai
