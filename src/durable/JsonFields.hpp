#pragma once

#include <cch/chord/Json.hpp>
#include <cch/support/Error.hpp>

#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace cch::durable {

/// One member of a JSON object, or `nullptr` when the value is not an object or
/// has no such member.
[[nodiscard]] inline const chord::Json* member(const chord::Json& value, std::string_view key) {
    const auto* object = value.as_object();
    if (object == nullptr) return nullptr;
    const auto found = object->find(std::string{key});
    return found == object->end() ? nullptr : &found->second;
}

/// One JSON number as a domain integer; a non-integer or an out-of-range value
/// is rejected. The strict bounds avoid double rounding INT64_MAX up to 2^63.
[[nodiscard]] inline support::Expected<std::int64_t> as_integer(const chord::Json& json) {
    const auto* number = json.as_number();
    if (number == nullptr || !(*number >= -0x1p63 && *number < 0x1p63) || std::trunc(*number) != *number)
        return std::unexpected(support::make_error(support::ErrorCode::Validation, "Expected an integer field"));
    return static_cast<std::int64_t>(*number);
}

/// A JSON number for one domain integer (strict JSON holds numbers as doubles).
[[nodiscard]] inline chord::Json numeric(std::int64_t value) { return chord::Json{static_cast<double>(value)}; }

/// A `Validation` error for a rejected value.
[[nodiscard]] inline support::Error validation(std::string message) {
    return support::make_error(support::ErrorCode::Validation, std::move(message));
}

} // namespace cch::durable
