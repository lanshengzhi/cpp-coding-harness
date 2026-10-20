#include <cch/chord/StrictJson.hpp>

#include <cmath>
#include <string>
#include <utility>
#include <variant>

namespace cch::chord {

namespace {

[[nodiscard]] support::Expected<support::JsonValue> copy_value(const support::JsonValue& value) {
    if (const auto* number = value.get_if<double>()) {
        if (!std::isfinite(*number)) {
            return std::unexpected(support::make_error(
                    support::ErrorCode::Validation, "value contains a non-finite number and is not strict JSON"));
        }
        return support::JsonValue{*number};
    }
    if (const auto* text = value.get_if<std::string>()) return support::JsonValue{*text};
    if (const auto* flag = value.get_if<bool>()) return support::JsonValue{*flag};
    if (value.holds<std::nullptr_t>()) return support::JsonValue{nullptr};
    if (const auto* array = value.get_if<support::JsonValue::array_t>()) {
        support::JsonValue::array_t copied;
        copied.reserve(array->size());
        for (const auto& item : *array) {
            auto item_copy = copy_value(item);
            if (!item_copy) return std::unexpected(std::move(item_copy.error()));
            copied.push_back(std::move(*item_copy));
        }
        return support::JsonValue{std::move(copied)};
    }
    const auto& object = value.get<support::JsonValue::object_t>();
    support::JsonValue::object_t copied;
    for (const auto& [key, item] : object) {
        auto item_copy = copy_value(item);
        if (!item_copy) return std::unexpected(std::move(item_copy.error()));
        copied.emplace(key, std::move(*item_copy));
    }
    return support::JsonValue{std::move(copied)};
}

[[nodiscard]] bool is_strict(const support::JsonValue& value) {
    if (const auto* number = value.get_if<double>()) return std::isfinite(*number);
    if (const auto* array = value.get_if<support::JsonValue::array_t>()) {
        for (const auto& item : *array) {
            if (!is_strict(item)) return false;
        }
        return true;
    }
    if (const auto* object = value.get_if<support::JsonValue::object_t>()) {
        for (const auto& [key, item] : *object) {
            if (!is_strict(item)) return false;
        }
        return true;
    }
    return true;
}

} // namespace

support::Expected<support::JsonValue> copy_strict_json(const support::JsonValue& value) { return copy_value(value); }

bool is_strict_json_value(const support::JsonValue& value) { return is_strict(value); }

} // namespace cch::chord
