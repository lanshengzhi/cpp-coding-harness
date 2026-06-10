#pragma once

#include <glaze/glaze.hpp>

#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace cch::util {

enum class ErrorCode {
    Unknown,
    JsonParse,
    JsonSerialize,
    Network,
    Timeout,
    Cancelled,
    Provider,
    Tool,
    Session,
    Validation,
    Workspace,
    Process,
};

struct Error {
    ErrorCode code{ErrorCode::Unknown};
    std::string message;
    std::string detail;
    std::optional<std::string> context;
};

template <typename T>
using Expected = std::expected<T, Error>;

using ExpectedVoid = std::expected<void, Error>;

[[nodiscard]] inline std::string to_string(ErrorCode code) {
    switch (code) {
    case ErrorCode::Unknown:
        return "unknown";
    case ErrorCode::JsonParse:
        return "json_parse";
    case ErrorCode::JsonSerialize:
        return "json_serialize";
    case ErrorCode::Network:
        return "network";
    case ErrorCode::Timeout:
        return "timeout";
    case ErrorCode::Cancelled:
        return "cancelled";
    case ErrorCode::Provider:
        return "provider";
    case ErrorCode::Tool:
        return "tool";
    case ErrorCode::Session:
        return "session";
    case ErrorCode::Validation:
        return "validation";
    case ErrorCode::Workspace:
        return "workspace";
    case ErrorCode::Process:
        return "process";
    }
    return "unknown";
}

[[nodiscard]] inline Error make_error(
    ErrorCode code,
    std::string message,
    std::string detail = {},
    std::optional<std::string> context = std::nullopt) {
    return Error{code, std::move(message), std::move(detail), std::move(context)};
}

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

template <typename T>
[[nodiscard]] Expected<T> read_json(std::string_view json) {
    auto parsed = glz::read_json<T>(json);
    if (!parsed) {
        return std::unexpected(glaze_error(parsed.error(), json, ErrorCode::JsonParse, "failed to parse JSON"));
    }
    return std::move(parsed).value();
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
