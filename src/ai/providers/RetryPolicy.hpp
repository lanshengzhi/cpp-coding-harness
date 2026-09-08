#pragma once

#include <cch/ai/Auth.hpp>
#include <cch/ai/InferenceFailure.hpp>
#include <cch/support/Error.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace cch::ai::providers {

struct ProviderFailure {
    bool network_error{false};
    std::optional<int> status{std::nullopt};
    ProviderHeaders headers{};
    /// Diagnostic text is retained for reporting only. It is never consulted
    /// by retry classification.
    std::string message{};
    std::optional<InferenceFailure> inference_failure{std::nullopt};
};

/// Map stable provider error codes to the product's failure vocabulary. Unknown
/// codes are non-retryable InvalidRequest; the raw code remains diagnostic.
[[nodiscard]] InferenceFailureKind inference_failure_kind_from_provider_code(
    std::string_view provider_code) noexcept;
[[nodiscard]] InferenceFailureKind inference_failure_kind_from_http_status(
    int status) noexcept;
[[nodiscard]] InferenceFailureKind inference_failure_kind_from_transport(
    support::ErrorCode code) noexcept;
/// Extract a stable provider code from a JSON error payload. Human-readable
/// message fields are intentionally ignored; malformed or code-less payloads
/// return no code and let the HTTP/transport category decide the outcome.
[[nodiscard]] std::optional<std::string> provider_error_code_from_payload(
    std::string_view payload);

[[nodiscard]] bool is_retryable_provider_failure(const ProviderFailure& failure);
[[nodiscard]] support::Expected<std::uint64_t> provider_retry_delay_ms(
    const ProviderFailure& failure,
    std::uint32_t retry_index,
    std::optional<std::uint64_t> max_retry_delay_ms,
    std::int64_t now_epoch_ms);

} // namespace cch::ai::providers
