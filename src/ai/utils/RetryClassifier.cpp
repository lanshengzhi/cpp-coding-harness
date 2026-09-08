#include "ai/utils/RetryClassifier.hpp"

namespace cch::ai {

bool is_retryable_inference_failure(const InferenceFailure& failure) noexcept {
    return !failure.output_started &&
           (failure.kind == InferenceFailureKind::RateLimited ||
            failure.kind == InferenceFailureKind::TransientTransportFailure);
}

bool requires_reauthentication(const InferenceFailure& failure) noexcept {
    return !failure.output_started && failure.kind == InferenceFailureKind::Unauthorized;
}

} // namespace cch::ai
