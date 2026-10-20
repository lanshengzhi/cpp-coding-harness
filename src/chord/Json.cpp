#include <cch/chord/Json.hpp>

#include <cmath>
#include <string>
#include <utility>
#include <variant>

namespace cch::chord {

namespace {

[[nodiscard]] support::Expected<Json> from_support_value(const support::JsonValue& value) {
    if (const auto* number = value.get_if<double>()) {
        if (!std::isfinite(*number)) {
            return std::unexpected(support::make_error(
                    support::ErrorCode::Validation, "value contains a non-finite number and is not strict JSON"));
        }
        return Json{*number};
    }
    if (const auto* text = value.get_if<std::string>()) return Json{*text};
    if (const auto* flag = value.get_if<bool>()) return Json{*flag};
    if (value.holds<std::nullptr_t>()) return Json{nullptr};
    if (const auto* array = value.get_if<support::JsonValue::array_t>()) {
        Json::Array converted;
        converted.reserve(array->size());
        for (const auto& item : *array) {
            auto child = from_support_value(item);
            if (!child) return std::unexpected(std::move(child.error()));
            converted.push_back(std::move(*child));
        }
        return Json{std::move(converted)};
    }
    Json::Object converted;
    for (const auto& [key, item] : value.get<support::JsonValue::object_t>()) {
        auto child = from_support_value(item);
        if (!child) return std::unexpected(std::move(child.error()));
        converted.emplace(key, std::move(*child));
    }
    return Json{std::move(converted)};
}

[[nodiscard]] support::JsonValue to_support_value(const Json& value) {
    if (const auto* number = value.as_number()) return support::JsonValue{*number};
    if (const auto* text = value.as_string()) return support::JsonValue{*text};
    if (const auto* flag = value.as_boolean()) return support::JsonValue{*flag};
    if (value.is_null()) return support::JsonValue{nullptr};
    if (const auto* array = value.as_array()) {
        support::JsonValue::array_t converted;
        converted.reserve(array->size());
        for (const auto& item : *array)
            converted.push_back(to_support_value(item));
        return support::JsonValue{std::move(converted)};
    }
    support::JsonValue::object_t converted;
    for (const auto& [key, item] : *value.as_object())
        converted.emplace(key, to_support_value(item));
    return support::JsonValue{std::move(converted)};
}

} // namespace

Json::Json(Array value) : storage_(std::make_shared<const Array>(std::move(value))) {}

Json::Json(Object value) : storage_(std::make_shared<const Object>(std::move(value))) {}

bool Json::is_null() const noexcept { return std::holds_alternative<std::nullptr_t>(storage_); }

const bool* Json::as_boolean() const noexcept { return std::get_if<bool>(&storage_); }

const double* Json::as_number() const noexcept { return std::get_if<double>(&storage_); }

const std::string* Json::as_string() const noexcept { return std::get_if<std::string>(&storage_); }

const Json::Array* Json::as_array() const noexcept {
    const auto* value = std::get_if<std::shared_ptr<const Array>>(&storage_);
    return value == nullptr ? nullptr : value->get();
}

const Json::Object* Json::as_object() const noexcept {
    const auto* value = std::get_if<std::shared_ptr<const Object>>(&storage_);
    return value == nullptr ? nullptr : value->get();
}

bool Json::operator==(const Json& other) const {
    if (storage_.index() != other.storage_.index()) return false;
    if (const auto* value = std::get_if<bool>(&storage_)) return *value == *std::get_if<bool>(&other.storage_);
    if (const auto* value = std::get_if<double>(&storage_)) return *value == *std::get_if<double>(&other.storage_);
    if (const auto* value = std::get_if<std::string>(&storage_))
        return *value == *std::get_if<std::string>(&other.storage_);
    if (std::holds_alternative<std::nullptr_t>(storage_)) return true;
    if (const auto* value = std::get_if<std::shared_ptr<const Array>>(&storage_)) {
        return **value == **std::get_if<std::shared_ptr<const Array>>(&other.storage_);
    }
    return **std::get_if<std::shared_ptr<const Object>>(&storage_) ==
           **std::get_if<std::shared_ptr<const Object>>(&other.storage_);
}

bool Json::shares_container_with(const Json& other) const noexcept {
    if (const auto* value = std::get_if<std::shared_ptr<const Array>>(&storage_)) {
        const auto* other_value = std::get_if<std::shared_ptr<const Array>>(&other.storage_);
        return other_value != nullptr && value->get() == other_value->get();
    }
    if (const auto* value = std::get_if<std::shared_ptr<const Object>>(&storage_)) {
        const auto* other_value = std::get_if<std::shared_ptr<const Object>>(&other.storage_);
        return other_value != nullptr && value->get() == other_value->get();
    }
    return false;
}

support::Expected<Json> Json::from_support(const support::JsonValue& value) { return from_support_value(value); }

support::JsonValue Json::to_support() const { return to_support_value(*this); }

} // namespace cch::chord
