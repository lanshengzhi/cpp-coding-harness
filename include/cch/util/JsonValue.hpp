#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace cch::util {

struct JsonValue {
    using null_t = std::nullptr_t;
    using array_t = std::vector<JsonValue>;
    using object_t = std::map<std::string, JsonValue>;
    using value_t = std::variant<null_t, double, std::string, bool, array_t, object_t>;

    value_t data{nullptr};

    JsonValue() = default;
    JsonValue(null_t) : data(nullptr) {}
    JsonValue(double value) : data(value) {}
    JsonValue(int value) : data(static_cast<double>(value)) {}
    JsonValue(std::string value) : data(std::move(value)) {}
    JsonValue(const char* value) : data(std::string(value)) {}
    JsonValue(bool value) : data(value) {}
    JsonValue(array_t value) : data(std::move(value)) {}
    JsonValue(object_t value) : data(std::move(value)) {}

    template <typename T>
    [[nodiscard]] T& get() {
        return std::get<T>(data);
    }

    template <typename T>
    [[nodiscard]] const T& get() const {
        return std::get<T>(data);
    }

    template <typename T>
    [[nodiscard]] T* get_if() noexcept {
        return std::get_if<T>(&data);
    }

    template <typename T>
    [[nodiscard]] const T* get_if() const noexcept {
        return std::get_if<T>(&data);
    }

    template <typename T>
    [[nodiscard]] bool holds() const noexcept {
        return std::holds_alternative<T>(data);
    }

    [[nodiscard]] std::string& get_string() {
        return get<std::string>();
    }

    [[nodiscard]] const std::string& get_string() const {
        return get<std::string>();
    }

    [[nodiscard]] bool& get_boolean() {
        return get<bool>();
    }

    [[nodiscard]] const bool& get_boolean() const {
        return get<bool>();
    }

    [[nodiscard]] double& get_number() {
        return get<double>();
    }

    [[nodiscard]] const double& get_number() const {
        return get<double>();
    }

    [[nodiscard]] array_t& get_array() {
        return get<array_t>();
    }

    [[nodiscard]] const array_t& get_array() const {
        return get<array_t>();
    }

    [[nodiscard]] object_t& get_object() {
        return get<object_t>();
    }

    [[nodiscard]] const object_t& get_object() const {
        return get<object_t>();
    }

    [[nodiscard]] JsonValue& at(const std::string& key) {
        return get_object().at(key);
    }

    [[nodiscard]] const JsonValue& at(const std::string& key) const {
        return get_object().at(key);
    }
};

} // namespace cch::util
