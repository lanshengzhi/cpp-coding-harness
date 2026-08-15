#pragma once

// Private Glaze serialization layer (build-performance-plan Stage 5). The
// shared `support/Json.hpp` provides `JsonValue` I/O without Glaze; everything
// that needs Glaze — the `glz::generic` conversions and the generic
// `read_json<T>` / `write_json(const T&)` templates for Glaze DTOs — lives
// here. Translation units that serialize DTOs include this header; translation
// units that only touch `support::JsonValue` include `support/Json.hpp` and
// never parse Glaze. This header is private: no Owner Interface header may
// include it, and Glaze must not appear in an Owner Interface.

#include "support/Json.hpp"

#include <glaze/glaze.hpp>

#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace cch::support {

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

/// Generic Glaze-DTO read. `JsonValue` itself is handled by `util/Json.hpp`
/// (`read_json` without template arguments); a misplaced `read_json<JsonValue>`
/// is rejected with a clear message rather than routed into Glaze.
template <typename T>
[[nodiscard]] Expected<T> read_json(std::string_view json) {
    static_assert(
        !std::is_same_v<T, JsonValue>,
        "JsonValue I/O lives in util/Json.hpp: call read_json without template arguments");
    auto parsed = glz::read_json<T>(json);
    if (!parsed) {
        return std::unexpected(glaze_error(
            parsed.error(), json, ErrorCode::JsonParse, "failed to parse JSON"));
    }
    return std::move(parsed).value();
}

/// Generic Glaze-DTO write; the `JsonValue` overload in `util/Json.hpp` wins
/// for `JsonValue` arguments.
template <typename T>
[[nodiscard]] Expected<std::string> write_json(const T& value) {
    auto serialized = glz::write_json(value);
    if (!serialized) {
        return std::unexpected(glaze_error(
            serialized.error(), {}, ErrorCode::JsonSerialize, "failed to serialize JSON"));
    }
    return std::move(serialized).value();
}

} // namespace cch::support
