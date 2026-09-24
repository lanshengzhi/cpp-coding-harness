#pragma once

#include <cch/support/JsonValue.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace cch::ai {

/// Typed member accessors over the passive JsonValue object. Every lookup is
/// presence- and type-checked; a missing or wrongly typed member yields
/// nullopt rather than a default value.

[[nodiscard]] inline const support::JsonValue::object_t* json_object(const support::JsonValue& value) {
    return value.get_if<support::JsonValue::object_t>();
}

[[nodiscard]] inline const support::JsonValue* json_member(
        const support::JsonValue::object_t& value, std::string_view name) {
    const auto found = value.find(std::string{name});
    return found == value.end() ? nullptr : &found->second;
}

[[nodiscard]] inline const support::JsonValue::object_t* json_object_member(
        const support::JsonValue::object_t& value, std::string_view name) {
    const auto* found = json_member(value, name);
    return found ? found->get_if<support::JsonValue::object_t>() : nullptr;
}

[[nodiscard]] inline const support::JsonValue::array_t* json_array_member(
        const support::JsonValue::object_t& value, std::string_view name) {
    const auto* found = json_member(value, name);
    return found ? found->get_if<support::JsonValue::array_t>() : nullptr;
}

[[nodiscard]] inline std::optional<std::string_view> json_string_member(
        const support::JsonValue::object_t& value, std::string_view name) {
    const auto* found = json_member(value, name);
    const auto* text = found ? found->get_if<std::string>() : nullptr;
    return text ? std::optional<std::string_view>{*text} : std::nullopt;
}

/// Present boolean member, strictly typed.
[[nodiscard]] inline std::optional<bool> json_bool_member(
        const support::JsonValue::object_t& value, std::string_view name) {
    const auto* found = json_member(value, name);
    const auto* flag = found ? found->get_if<bool>() : nullptr;
    return flag ? std::optional<bool>{*flag} : std::nullopt;
}

/// Raw numeric member, unvalidated (JSON numbers parse as doubles). Callers
/// apply their own finiteness/sign rules.
[[nodiscard]] inline std::optional<double> json_number_member(
        const support::JsonValue::object_t& value, std::string_view name) {
    const auto* found = json_member(value, name);
    const auto* number = found ? found->get_if<double>() : nullptr;
    return number ? std::optional<double>{*number} : std::nullopt;
}

/// Non-negative integer member, validated to fit std::int64_t.
[[nodiscard]] inline std::optional<std::int64_t> json_integer_member(
        const support::JsonValue::object_t& value, std::string_view name) {
    const auto number = json_number_member(value, name);
    if (!number || !std::isfinite(*number) || *number < 0 ||
            *number > static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
        return std::nullopt;
    }
    return static_cast<std::int64_t>(*number);
}

} // namespace cch::ai
