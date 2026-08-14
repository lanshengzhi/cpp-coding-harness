#pragma once

#include <expected>
#include <optional>
#include <string>
#include <utility>

namespace cch::support {

/// Shared, pi-neutral error channel owned by the `cch_support` package
/// (ADR 0039). An Owner may expose a more specific `std::expected<T, E>` when
/// its domain contract requires one; this is the default cross-Owner channel.
enum class ErrorCode {
    Unknown,
    JsonParse,
    JsonSerialize,
    Network,
    Timeout,
    Busy,
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
    /// pi `MissingSessionCwdError`: the resumed session's stored header cwd
    /// no longer exists (session-cwd.ts). The CLI prints the error's message
    /// verbatim (no "could not resume session:" prefix) and exits 1.
    MissingSessionCwd,
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
    case ErrorCode::Busy:
        return "busy";
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
    case ErrorCode::MissingSessionCwd:
        return "missing_session_cwd";
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

} // namespace cch::support
