#pragma once

#include <cch/agent/harness/ExecutionEnv.hpp>
#include <cch/support/Error.hpp>

namespace cch::harness {

[[nodiscard]] inline ExecutionError classify_process_execution_error(
    const support::Error& error) {
    auto code = ExecutionErrorCode::SpawnError;
    if (error.code == support::ErrorCode::Cancelled) {
        code = ExecutionErrorCode::Aborted;
    } else if (error.code == support::ErrorCode::Timeout) {
        code = ExecutionErrorCode::Timeout;
    } else if (error.message.find("callback") != std::string::npos ||
               error.detail.find("callback") != std::string::npos) {
        code = ExecutionErrorCode::CallbackError;
    }
    return ExecutionError{
        .code = code,
        .message = error.detail.empty() ? error.message : error.detail,
    };
}

} // namespace cch::harness
