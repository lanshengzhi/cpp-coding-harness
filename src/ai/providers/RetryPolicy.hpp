#pragma once

#include <cch/ai/Auth.hpp>
#include <cch/ai/InferenceFailure.hpp>
#include <cch/support/Error.hpp>
#include <cch/support/JsonValue.hpp>

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

/// Extract a provider-supplied retry delay from response headers. A missing or
/// malformed hint returns nullopt; the caller supplies exponential fallback.
[[nodiscard]] std::optional<std::uint64_t> provider_backoff_hint_ms(
        const ProviderFailure& failure, std::int64_t now_epoch_ms);
/// Extract a provider-supplied retry delay from a streamed JSON error payload.
/// Both the root object and its nested `error` object are checked. Human
/// wording is never consulted.
[[nodiscard]] std::optional<std::uint64_t> provider_backoff_hint_ms(
        const support::JsonValue::object_t& payload, std::int64_t now_epoch_ms);
[[nodiscard]] std::optional<std::uint64_t> provider_backoff_hint_ms(
        std::string_view payload, std::int64_t now_epoch_ms);

[[nodiscard]] bool is_retryable_provider_failure(const ProviderFailure& failure);

/// Recovery decision table:
/// Unauthorized -> terminal re-auth guidance; never retry or compact.
/// RateLimited/TransientTransportFailure -> bounded backoff retry when no
/// output was emitted; otherwise fail without replaying external effects.
/// ContextOverflow -> compaction-and-retry in the Session layer.
/// InvalidRequest/Cancelled -> terminal failure/cancellation.
[[nodiscard]] support::Expected<std::uint64_t> provider_retry_delay_ms(
    const ProviderFailure& failure,
    std::uint32_t retry_index,
    std::optional<std::uint64_t> max_retry_delay_ms,
    std::int64_t now_epoch_ms);

} // namespace cch::ai::providers
