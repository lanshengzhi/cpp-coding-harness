#pragma once

#include "util/Json.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>

namespace cch::tests {

namespace detail {

[[nodiscard]] inline bool json_numbers_close(double expected, double actual) {
    return std::abs(expected - actual) <=
           1e-12 + 1e-9 * std::max(std::abs(expected), std::abs(actual));
}

[[nodiscard]] inline std::string mismatch_message(
    const std::string& path,
    const util::JsonValue& expected,
    const util::JsonValue& actual) {
    auto expected_text = util::write_json(expected);
    auto actual_text = util::write_json(actual);
    return path + ": expected " +
           (expected_text ? *expected_text : std::string{"<unserializable>"}) +
           ", actual " +
           (actual_text ? *actual_text : std::string{"<unserializable>"});
}

} // namespace detail

/// Structural JSON comparison for pi differential goldens. Numbers compare
/// with a 1e-9 relative + 1e-12 absolute tolerance to absorb IEEE-754
/// evaluation-order differences between V8 and C++ arithmetic; every other
/// value compares exactly. Returns the first mismatch path and values, or
/// nullopt when the values match.
[[nodiscard]] inline std::optional<std::string> json_mismatch(
    const util::JsonValue& expected,
    const util::JsonValue& actual,
    std::string path = "$") {
    if (const auto* expected_number = expected.get_if<double>()) {
        const auto* actual_number = actual.get_if<double>();
        if (!actual_number) {
            return detail::mismatch_message(path, expected, actual);
        }
        if (!detail::json_numbers_close(*expected_number, *actual_number)) {
            return detail::mismatch_message(path, expected, actual);
        }
        return std::nullopt;
    }
    if (const auto* expected_object = expected.get_if<util::JsonValue::object_t>()) {
        const auto* actual_object = actual.get_if<util::JsonValue::object_t>();
        if (!actual_object) {
            return detail::mismatch_message(path, expected, actual);
        }
        for (const auto& [key, value] : *expected_object) {
            const auto found = actual_object->find(key);
            if (found == actual_object->end()) {
                return path + ": missing actual key \"" + key + "\"";
            }
            if (auto mismatch = json_mismatch(value, found->second, path + "." + key)) {
                return mismatch;
            }
        }
        for (const auto& [key, value] : *actual_object) {
            if (!expected_object->contains(key)) {
                return path + ": unexpected actual key \"" + key + "\"";
            }
        }
        return std::nullopt;
    }
    if (const auto* expected_array = expected.get_if<util::JsonValue::array_t>()) {
        const auto* actual_array = actual.get_if<util::JsonValue::array_t>();
        if (!actual_array) {
            return detail::mismatch_message(path, expected, actual);
        }
        const auto shared = std::min(expected_array->size(), actual_array->size());
        for (std::size_t index = 0; index < shared; ++index) {
            if (auto mismatch = json_mismatch(
                    (*expected_array)[index],
                    (*actual_array)[index],
                    path + "[" + std::to_string(index) + "]")) {
                return mismatch;
            }
        }
        if (expected_array->size() != actual_array->size()) {
            return path + ": expected " + std::to_string(expected_array->size()) +
                   " elements, actual " + std::to_string(actual_array->size());
        }
        return std::nullopt;
    }
    if (expected.data.index() != actual.data.index()) {
        return detail::mismatch_message(path, expected, actual);
    }
    if (const auto* expected_string = expected.get_if<std::string>()) {
        if (*expected_string != actual.get_string()) {
            return detail::mismatch_message(path, expected, actual);
        }
        return std::nullopt;
    }
    if (const auto* expected_flag = expected.get_if<bool>()) {
        if (*expected_flag != actual.get_boolean()) {
            return detail::mismatch_message(path, expected, actual);
        }
    }
    return std::nullopt;
}

} // namespace cch::tests
