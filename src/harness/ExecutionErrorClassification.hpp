#pragma once

#include <cch/harness/ExecutionEnv.hpp>
#include <cch/util/Error.hpp>

namespace cch::harness {

enum class ExecutionErrorOrigin {
    Request,
    Process,
};

[[nodiscard]] inline ExecutionError classify_execution_error(
    const util::Error& error,
    ExecutionErrorOrigin origin) {
    auto code = ExecutionErrorCode::SpawnError;
    if (origin == ExecutionErrorOrigin::Request) {
        if (error.detail.find("disabled") != std::string::npos) {
            code = ExecutionErrorCode::ShellUnavailable;
        }
    } else if (error.code == util::ErrorCode::Timeout) {
        code = ExecutionErrorCode::Timeout;
    } else if (error.detail.find("callback") != std::string::npos) {
        code = ExecutionErrorCode::CallbackError;
    }
    return ExecutionError{
        .code = code,
        .message = error.detail,
    };
}

} // namespace cch::harness
