#pragma once

#include <cch/support/AsyncResult.hpp>
#include <cch/support/Error.hpp>

#include <chrono>
#include <expected>
#include <functional>
#include <map>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <string_view>

namespace cch::harness {

/// Stable, backend-independent execution error codes.
enum class ExecutionErrorCode {
    Aborted,
    Busy,
    Timeout,
    ShellUnavailable,
    SpawnError,
    CallbackError,
    NotSupported,
    Unknown,
};

/// Error returned by pi-shaped shell operations.
struct ExecutionError {
    ExecutionErrorCode code{ExecutionErrorCode::Unknown};
    std::string message;
};

/// Effective user-level configuration for local Shell launches. Authorization
/// remains an assembly decision and is intentionally not represented here.
struct ShellConfig {
    std::optional<std::string> shell_path{std::nullopt};
    std::optional<std::string> command_prefix{std::nullopt};
};

/// Options for Shell::exec.
struct ExecOptions {
    /// Working directory override (relative resolved against workspace).
    std::optional<std::string> cwd;
    /// Environment variable overrides. Non-secret keys shadow the base env.
    std::optional<std::map<std::string, std::string>> env;
    /// Wall-clock timeout. Zero / unset means no timeout.
    std::optional<std::chrono::milliseconds> timeout;
    /// Active prompt cancellation token.
    std::stop_token stop_token{};
    /// Called with stdout chunks as they are produced. Returning an error
    /// deactivates the callback while the process output continues draining.
    std::optional<std::move_only_function<support::ExpectedVoid(std::string_view)>> onStdout;
    /// Called with stderr chunks as they are produced. Failure handling is the
    /// same as onStdout.
    std::optional<std::move_only_function<support::ExpectedVoid(std::string_view)>> onStderr;
};

/// Result of a pi-shaped shell execution with split streams.
struct ShellExecResult {
    std::string stdout_output;
    std::string stderr_output;
    int exitCode{0};
};

/// Error conversion for consumers that use the shared support channel.
[[nodiscard]] inline support::Error to_util_error(ExecutionError e) {
    auto code = support::ErrorCode::Process;
    switch (e.code) {
    case ExecutionErrorCode::Aborted:
        code = support::ErrorCode::Cancelled;
        break;
    case ExecutionErrorCode::Busy:
        code = support::ErrorCode::Busy;
        break;
    case ExecutionErrorCode::Timeout:
        code = support::ErrorCode::Timeout;
        break;
    case ExecutionErrorCode::ShellUnavailable:
    case ExecutionErrorCode::SpawnError:
    case ExecutionErrorCode::CallbackError:
        code = support::ErrorCode::Process;
        break;
    case ExecutionErrorCode::NotSupported:
    case ExecutionErrorCode::Unknown:
        code = support::ErrorCode::Unknown;
        break;
    }
    return support::make_error(code, e.message, e.message);
}

/// Complete asynchronous Shell capability. Process and environment policy
/// stay behind this seam; no scheduler or process implementation type crosses
/// the Owner Interface.
class AsyncShell {
public:
    virtual ~AsyncShell() = default;

    /// Execute a shell command with split streams.
    [[nodiscard]] virtual support::AsyncResult<ShellExecResult, ExecutionError> exec(
            std::string command, ExecOptions options = {}) = 0;
};

} // namespace cch::harness
