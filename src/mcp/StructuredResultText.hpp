#pragma once

// JavaScript JSON.stringify(value, null, 2) for model-visible MCP results.
#include "support/Json.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cch::mcp::detail {

[[nodiscard]] inline std::optional<std::uint32_t> javascript_array_index(std::string_view key) {
    if (key.empty()) {
        return std::nullopt;
    }
    if (key == "0") {
        return 0;
    }
    if (key.front() == '0') {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    for (const char character : key) {
        if (character < '0' || character > '9') {
            return std::nullopt;
        }
        value = value * 10 + static_cast<std::uint64_t>(character - '0');
        if (value > 4294967294ULL) {
            return std::nullopt;
        }
    }
    return static_cast<std::uint32_t>(value);
}

/// ECMAScript `Number::toString` / `JSON.stringify` number text. The shared
/// `support::write_json` deliberately reproduces the old Glaze formatter, whose
/// notation thresholds and `E` exponent differ from JavaScript, so the owner
/// formats its own numbers for the model text. The exact differences, measured
/// against Node's `JSON.stringify` for the values a `structuredContent` member
/// can carry:
///
/// | value                 | `JSON.stringify` | `support::write_json` |
/// |-----------------------|------------------|-----------------------|
/// | `1e20`                | `100000000000000000000` | `1E20`          |
/// | `1e21`                | `1e+21`          | `1E21`               |
/// | `1e-7`                | `1e-7`           | `1E-7`               |
/// | `0.000001`            | `0.000001`       | `1E-6`               |
/// | `1.2345678901234567e30` | `1.2345678901234567e+30` | `1.2345678901234567E30` |
/// | `-0`                  | `0`              | `-0`                 |
///
/// The shared formatter stays as it is: it is the wire format of every other
/// JSON value in the project, and changing it would be a cross-cutting change
/// outside this slice. Only the model-visible `structuredContent` text uses
/// the JavaScript notation, because only that text is contractually the bytes
/// pi's client produces.
[[nodiscard]] inline std::string javascript_number_text(double value) {
    if (std::isnan(value) || std::isinf(value)) {
        return "null";
    }
    if (value == 0.0) {
        return "0";
    }
    char buffer[64];
    const auto converted = std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::scientific);
    if (converted.ec != std::errc{}) {
        return "null";
    }
    const char* const end = converted.ptr;
    const char* cursor = buffer;
    const bool negative = cursor < end && *cursor == '-';
    if (negative) {
        ++cursor;
    }
    std::string digits;
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
    if (exponent_negative) {
        exponent = -exponent;
    }
    while (digits.size() > 1 && digits.back() == '0') {
        digits.pop_back();
    }

    const int k = static_cast<int>(digits.size());
    const int n = exponent + 1;
    std::string out;
    if (negative) {
        out += '-';
    }
    if (k <= n && n <= 21) {
        out += digits;
        out.append(static_cast<std::size_t>(n - k), '0');
    } else if (0 < n && n <= 21) {
        out.append(digits, 0, static_cast<std::size_t>(n));
        out += '.';
        out.append(digits, static_cast<std::size_t>(n), std::string::npos);
    } else if (-6 < n && n <= 0) {
        out += "0.";
        out.append(static_cast<std::size_t>(-n), '0');
        out += digits;
    } else {
        out += digits[0];
        if (k > 1) {
            out += '.';
            out.append(digits, 1, std::string::npos);
        }
        out += 'e';
        const int magnitude = n - 1;
        if (magnitude >= 0) {
            out += '+';
            out += std::to_string(magnitude);
        } else {
            out += '-';
            out += std::to_string(-magnitude);
        }
    }
    return out;
}

[[nodiscard]] inline support::ExpectedVoid append_javascript_json(
        const support::JsonValue& value, std::string& out, std::size_t indentation) {
    if (const auto* object = value.get_if<support::JsonValue::object_t>()) {
        if (object->empty()) {
            out += "{}";
            return {};
        }
        using Member = support::JsonValue::object_t::value_type;
        std::vector<const Member*> members;
        for (const auto& member : *object) {
            members.push_back(&member);
        }
        std::stable_sort(members.begin(), members.end(), [](const Member* left, const Member* right) {
            const auto a = javascript_array_index(left->first);
            const auto b = javascript_array_index(right->first);
            return a && (!b || *a < *b);
        });
        out += "{\n";
        for (std::size_t i = 0; i < members.size(); ++i) {
            out.append(indentation + 2, ' ');
            auto key = support::write_json(support::JsonValue{members[i]->first});
            if (!key) return std::unexpected(key.error());
            out += *key;
            out += ": ";
            if (auto result = append_javascript_json(members[i]->second, out, indentation + 2); !result) return result;
            out += i + 1 < members.size() ? ",\n" : "\n";
        }
        out.append(indentation, ' ');
        out += '}';
        return {};
    }
    if (const auto* array = value.get_if<support::JsonValue::array_t>()) {
        if (array->empty()) {
            out += "[]";
            return {};
        }
        out += "[\n";
        for (std::size_t i = 0; i < array->size(); ++i) {
            out.append(indentation + 2, ' ');
            if (auto result = append_javascript_json((*array)[i], out, indentation + 2); !result) return result;
            out += i + 1 < array->size() ? ",\n" : "\n";
        }
        out.append(indentation, ' ');
        out += ']';
        return {};
    }
    if (const auto* number = value.get_if<double>()) {
        out += javascript_number_text(*number);
        return {};
    }
    auto scalar = support::write_json(value);
    if (!scalar) return std::unexpected(scalar.error());
    out += *scalar;
    return {};
}

[[nodiscard]] inline std::optional<std::string> format_structured_content(const support::JsonValue& result) {
    const auto* object = result.get_if<support::JsonValue::object_t>();
    if (object == nullptr) return std::nullopt;
    const auto structured = object->find("structuredContent");
    if (structured == object->end()) return std::nullopt;
    std::string out;
    if (!append_javascript_json(structured->second, out, 0)) return std::nullopt;
    return out;
}

} // namespace cch::mcp::detail
