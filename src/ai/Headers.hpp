#pragma once

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace cch::ai {

/// ASCII case-insensitive header name equality (HTTP field names are ASCII).
[[nodiscard]] inline bool header_name_equal(std::string_view left, std::string_view right) {
    return std::ranges::equal(left, right, [](char left_character, char right_character) {
        const auto lower = [](char character) {
            return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character;
        };
        return lower(left_character) == lower(right_character);
    });
}

/// Case-insensitive lookup of one header value in any name/value container.
template <typename Headers>
[[nodiscard]] std::optional<std::string_view> find_header(const Headers& headers, std::string_view name) {
    for (const auto& [candidate, value] : headers) {
        if (header_name_equal(candidate, name)) {
            return value;
        }
    }
    return std::nullopt;
}

/// Sets one header, replacing any existing entry regardless of name casing.
template <typename Headers> void set_header(Headers& headers, std::string name, std::string value) {
    std::erase_if(headers, [&name](const auto& header) { return header_name_equal(header.first, name); });
    headers.emplace(std::move(name), std::move(value));
}

/// Erases every entry matching the name, any casing.
template <typename Headers> void erase_header(Headers& headers, std::string_view name) {
    std::erase_if(headers, [name](const auto& header) { return header_name_equal(header.first, name); });
}

/// True when a header with a non-empty value is present.
template <typename Headers> [[nodiscard]] bool has_header(const Headers& headers, std::string_view name) {
    return std::ranges::any_of(headers,
            [name](const auto& header) { return header_name_equal(header.first, name) && !header.second.empty(); });
}

/// True when a header is present whose value has a non-whitespace character.
template <typename Headers> [[nodiscard]] bool has_non_empty_header(const Headers& headers, std::string_view name) {
    return std::ranges::any_of(headers, [name](const auto& entry) {
        return header_name_equal(entry.first, name) && std::ranges::any_of(entry.second, [](char character) {
            return character != ' ' && character != '\t' && character != '\r' && character != '\n';
        });
    });
}

/// Merges overrides into target with case-insensitive replacement per name.
template <typename Target, typename Overrides> void merge_headers(Target& target, const Overrides& overrides) {
    for (const auto& [name, value] : overrides) {
        erase_header(target, name);
        target.emplace(name, value);
    }
}

} // namespace cch::ai
