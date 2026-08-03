#pragma once

#include <expected>
#include <optional>
#include <string>
#include <utility>

namespace cch::util {

enum class ErrorCode {
    Unknown,
    JsonParse,
    JsonSerialize,
    Network,
    Timeout,
    Cancelled,
    ModelSource,
    ModelValidation,
    Provider,
    Stream,
    Auth,
    OAuth,
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
    case ErrorCode::ModelSource:
        return "model_source";
    case ErrorCode::ModelValidation:
        return "model_validation";
    case ErrorCode::Provider:
        return "provider";
    case ErrorCode::Stream:
        return "stream";
    case ErrorCode::Auth:
        return "auth";
    case ErrorCode::OAuth:
        return "oauth";
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

} // namespace cch::util
