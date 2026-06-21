#pragma once

#include "../../include/cch/util/Error.hpp"
#include "../../include/cch/util/JsonValue.hpp"

#include <glaze/glaze.hpp>

#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace cch::util {

[[nodiscard]] inline Error glaze_error(
    const glz::error_ctx& error,
    std::string_view json,
    ErrorCode code,
    std::string message) {
    return make_error(
        code,
        std::move(message),
        glz::format_error(error, json),
        std::string(json));
}

[[nodiscard]] inline JsonValue json_from_glaze(const glz::generic& value);
[[nodiscard]] inline glz::generic json_to_glaze(const JsonValue& value);

[[nodiscard]] inline JsonValue json_from_glaze_array(const glz::generic::array_t& values) {
    JsonValue::array_t converted;
    converted.reserve(values.size());
    for (const auto& value : values) {
        converted.push_back(json_from_glaze(value));
    }
    return JsonValue{std::move(converted)};
}

[[nodiscard]] inline JsonValue json_from_glaze_object(const glz::generic::object_t& values) {
    JsonValue::object_t converted;
    for (const auto& [key, value] : values) {
        converted.emplace(std::string(key), json_from_glaze(value));
    }
    return JsonValue{std::move(converted)};
}

[[nodiscard]] inline JsonValue json_from_glaze(const glz::generic& value) {
    if (const auto* object = value.get_if<glz::generic::object_t>()) {
        return json_from_glaze_object(*object);
    }
    if (const auto* array = value.get_if<glz::generic::array_t>()) {
        return json_from_glaze_array(*array);
    }
    if (const auto* text = value.get_if<std::string>()) {
        return JsonValue{*text};
    }
    if (const auto* flag = value.get_if<bool>()) {
        return JsonValue{*flag};
    }
    if (const auto* number = value.get_if<double>()) {
        return JsonValue{*number};
    }
    return JsonValue{nullptr};
}

[[nodiscard]] inline glz::generic json_to_glaze_array(const JsonValue::array_t& values) {
    glz::generic::array_t converted;
    converted.reserve(values.size());
    for (const auto& value : values) {
        converted.push_back(json_to_glaze(value));
    }
    glz::generic result;
    result = converted;
    return result;
}

[[nodiscard]] inline glz::generic json_to_glaze_object(const JsonValue::object_t& values) {
    glz::generic::object_t converted;
    for (const auto& [key, value] : values) {
        converted.insert(std::make_pair(key, json_to_glaze(value)));
    }
    glz::generic result;
    result = converted;
    return result;
}

[[nodiscard]] inline glz::generic json_to_glaze(const JsonValue& value) {
    if (const auto* object = value.get_if<JsonValue::object_t>()) {
        return json_to_glaze_object(*object);
    }
    if (const auto* array = value.get_if<JsonValue::array_t>()) {
        return json_to_glaze_array(*array);
    }
    glz::generic result;
    if (const auto* text = value.get_if<std::string>()) {
        result = *text;
        return result;
    }
    if (const auto* flag = value.get_if<bool>()) {
        result = *flag;
        return result;
    }
    if (const auto* number = value.get_if<double>()) {
        result = *number;
        return result;
    }
    result = nullptr;
    return result;
}

[[nodiscard]] inline Expected<std::string> write_json(const JsonValue& value) {
    auto serialized = glz::write_json(json_to_glaze(value));
    if (!serialized) {
        return std::unexpected(glaze_error(serialized.error(), {}, ErrorCode::JsonSerialize, "failed to serialize JSON"));
    }
    return std::move(serialized).value();
}

template <typename T>
[[nodiscard]] Expected<T> read_json(std::string_view json) {
    if constexpr (std::is_same_v<T, JsonValue>) {
        auto parsed = glz::read_json<glz::generic>(json);
        if (!parsed) {
            return std::unexpected(glaze_error(parsed.error(), json, ErrorCode::JsonParse, "failed to parse JSON"));
        }
        return json_from_glaze(std::move(parsed).value());
    } else {
        auto parsed = glz::read_json<T>(json);
        if (!parsed) {
            return std::unexpected(glaze_error(parsed.error(), json, ErrorCode::JsonParse, "failed to parse JSON"));
        }
        return std::move(parsed).value();
    }
}

template <typename T>
[[nodiscard]] Expected<std::string> write_json(const T& value) {
    auto serialized = glz::write_json(value);
    if (!serialized) {
        return std::unexpected(glaze_error(serialized.error(), {}, ErrorCode::JsonSerialize, "failed to serialize JSON"));
    }
    return std::move(serialized).value();
}

} // namespace cch::util
