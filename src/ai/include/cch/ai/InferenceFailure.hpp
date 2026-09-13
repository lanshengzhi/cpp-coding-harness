#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace cch::ai {

/// Structured outcome category emitted by a Provider or its transport adapter.
/// Human-readable diagnostics are deliberately not part of this decision value:
/// recovery policy must not change when a Provider rewords an error.
enum class InferenceFailureKind {
    Unauthorized,
    RateLimited,
    ContextOverflow,
    TransientTransportFailure,
    InvalidRequest,
    Cancelled,
};

/// Provider failure metadata carried alongside the diagnostic Assistant Error
/// event. `output_started` distinguishes a request that can safely be retried
/// from one whose stream already emitted output; `suggested_backoff_ms` is a
/// Provider/transport hint, while `provider_code` preserves the raw wire code
/// for diagnostics and telemetry.
struct InferenceFailure {
    InferenceFailureKind kind{InferenceFailureKind::InvalidRequest};
    bool output_started{false};
    std::optional<std::uint64_t> suggested_backoff_ms{std::nullopt};
    std::optional<std::string> provider_code{std::nullopt};
};

/// Decide whether the session RecoveryPolicy may retry a failed model turn.
/// This is intentionally a pure decision over the structured Provider outcome;
/// diagnostic text and raw Provider codes do not affect the result. A stream
/// that already emitted output is not safe for turn-level replay.
[[nodiscard]] bool is_retryable_inference_failure(const InferenceFailure& failure) noexcept;

/// Unauthorized is terminal for the current request and requires the session
/// to surface its re-authentication guidance instead of retrying blindly.
[[nodiscard]] bool requires_reauthentication(const InferenceFailure& failure) noexcept;

} // namespace cch::ai
