#pragma once

#include <cch/ai/Message.hpp>

namespace cch::ai {

/// Classify whether a failed assistant message looks like a transient provider
/// or transport error (pi `isRetryableAssistantError` in
/// `packages/ai/src/utils/retry.ts`), so callers can decide if the last
/// assistant turn should be restarted. This does not implement retry policy:
/// callers first handle context overflow separately, then apply their own
/// retry budget, backoff, and reporting before restarting the assistant turn.
///
/// Retryable iff the message is an `error` terminal carrying an error message
/// that matches a transient provider/network pattern without also matching a
/// non-retryable quota/billing/provider-limit pattern.
[[nodiscard]] bool is_retryable_assistant_error(
    const ai::AssistantMessage& message);

} // namespace cch::ai
