#pragma once

#include <cch/ai/InferenceFailure.hpp>

namespace cch::ai {

/// Decide whether the session RecoveryPolicy may retry a failed model turn.
/// This is intentionally a pure decision over the structured Provider outcome;
/// diagnostic text and raw Provider codes do not affect the result. A stream
/// that already emitted output is not safe for turn-level replay.
[[nodiscard]] bool is_retryable_inference_failure(
    const InferenceFailure& failure) noexcept;

/// Unauthorized is terminal for the current request and requires the session
/// to surface its re-authentication guidance instead of retrying blindly.
[[nodiscard]] bool requires_reauthentication(const InferenceFailure& failure) noexcept;

} // namespace cch::ai
