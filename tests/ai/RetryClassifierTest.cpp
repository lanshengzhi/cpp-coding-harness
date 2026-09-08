#include "ai/utils/RetryClassifier.hpp"

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <utility>

using namespace cch;

namespace {

[[nodiscard]] ai::InferenceFailure failure(
        ai::InferenceFailureKind kind,
        bool output_started = false,
        std::string provider_code = {}) {
    return ai::InferenceFailure{
            .kind = kind,
            .output_started = output_started,
            .suggested_backoff_ms = std::nullopt,
            .provider_code = provider_code.empty()
                ? std::nullopt
                : std::optional<std::string>{std::move(provider_code)},
    };
}

} // namespace

TEST_CASE(
    "structured inference failures allow retry for rate limits and transient transports",
    "[ai][retry][issue624][spec]") {
    for (const auto kind : {
                 ai::InferenceFailureKind::RateLimited,
                 ai::InferenceFailureKind::TransientTransportFailure,
         }) {
        CHECK(ai::is_retryable_inference_failure(failure(kind)));
    }

    // A Provider may attach a server hint and a raw code without changing the
    // RecoveryPolicy category.
    auto rate_limited = failure(
            ai::InferenceFailureKind::RateLimited,
            false,
            "rate_limit_exceeded");
    rate_limited.suggested_backoff_ms = 2500;
    CHECK(ai::is_retryable_inference_failure(rate_limited));
}

TEST_CASE(
    "structured inference failures never retry authorization overflow invalid or cancelled outcomes",
    "[ai][retry][issue624][spec]") {
    for (const auto kind : {
                 ai::InferenceFailureKind::Unauthorized,
                 ai::InferenceFailureKind::ContextOverflow,
                 ai::InferenceFailureKind::InvalidRequest,
                 ai::InferenceFailureKind::Cancelled,
         }) {
        CHECK_FALSE(ai::is_retryable_inference_failure(failure(kind)));
    }
}

TEST_CASE(
    "retry classification is unchanged when provider diagnostics are reworded",
    "[ai][retry][issue624][spec]") {
    // The raw provider code and human-readable diagnostic are observations;
    // the stable structured kind is the only retry input.
    const auto first_wording = failure(
            ai::InferenceFailureKind::TransientTransportFailure,
            false,
            "temporary_network_failure_a");
    const auto second_wording = failure(
            ai::InferenceFailureKind::TransientTransportFailure,
            false,
            "temporary_network_failure_b");
    CHECK(ai::is_retryable_inference_failure(first_wording));
    CHECK(ai::is_retryable_inference_failure(second_wording));
}

TEST_CASE(
    "a started inference output is not retried even when its failure is transient",
    "[ai][retry][issue624][spec]") {
    CHECK_FALSE(ai::is_retryable_inference_failure(
            failure(ai::InferenceFailureKind::TransientTransportFailure, true)));
    CHECK_FALSE(ai::is_retryable_inference_failure(
            failure(ai::InferenceFailureKind::RateLimited, true)));
}
