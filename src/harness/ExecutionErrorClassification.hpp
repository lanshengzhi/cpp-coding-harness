#pragma once

#include <cch/harness/ExecutionEnv.hpp>
#include <cch/util/Error.hpp>

namespace cch::harness {

[[nodiscard]] inline ExecutionError classify_process_execution_error(
    const util::Error& error) {
    auto code = ExecutionErrorCode::SpawnError;
    if (error.code == util::ErrorCode::Cancelled) {
        code = ExecutionErrorCode::Aborted;
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
