// T12 retryability classification evidence (#361): the C++ port of pi's
// `isRetryableAssistantError` (`packages/ai/src/utils/retry.ts`) — transient
// provider/network patterns retry, quota/billing/provider-limit patterns never
// retry, non-error terminals and empty error messages never retry, and the
// non-retryable limit check wins over a matching retryable pattern.

#include <cch/ai/Message.hpp>
#include "ai/utils/RetryClassifier.hpp"

#include "../../third_party/catch2/catch_test_macros.hpp"

#include <string>
#include <vector>

using namespace cch;

namespace {

[[nodiscard]] ai::AssistantMessage error_terminal(std::string message) {
    auto terminal = ai::assistant_text_message("");
    terminal.stop_reason = ai::AssistantStopReason::Error;
    terminal.error_message = std::move(message);
    return terminal;
}

} // namespace

TEST_CASE(
    "isRetryableAssistantError retries transient provider and network patterns",
    "[ai][retry][issue361]") {
    // Generic provider load / HTTP status / server-side transients.
    for (const std::string message :
         {"overloaded", "model overloaded; retry later",
          "Rate limit reached", "rate-limit hit", "rate_limit exceeded",
          "too many requests", "HTTP 429 Too Many Requests", "429",
          "500 Internal Server Error", "502 Bad Gateway", "503",
          "504 Gateway Timeout", "524 A Timeout Occurred",
          "service unavailable", "service_unavailable",
          "server error", "internal error"}) {
        CHECK(ai::is_retryable_assistant_error(error_terminal(message)));
    }
    // Network / proxy / fetch / WebSocket transport failures.
    for (const std::string message :
         {"network error", "network_error", "connection error",
          "connection refused", "connection lost",
          "the other side closed the connection", "fetch failed",
          "getaddrinfo ENOTFOUND", "ENOTFOUND host", "EAI_AGAIN",
          "upstream connect error", "upstream_connect",
          "reset before headers", "socket hang up",
          "socket connection was closed", "timed out", "time out",
          "request timeout", "connection terminated",
          "websocket closed", "websocket error"}) {
        CHECK(ai::is_retryable_assistant_error(error_terminal(message)));
    }
    // Premature stream endings and explicit retry guidance.
    for (const std::string message :
         {"stream ended without a stop reason",
          "Anthropic stream ended before message_stop",
          "stream ended before a terminal response event",
          "http2 request did not get a response",
          "retry delay requested by provider",
          "you can retry your request", "please retry your request",
          "try your request again", "ResourceExhausted"}) {
        CHECK(ai::is_retryable_assistant_error(error_terminal(message)));
    }
    // Case-insensitive matching (pi `i` flag).
    CHECK(ai::is_retryable_assistant_error(
        error_terminal("UPSTREAM CONNECT FAILED")));
    CHECK(ai::is_retryable_assistant_error(
        error_terminal("Socket Hang Up")));
}

TEST_CASE(
    "isRetryableAssistantError never retries quota, billing, or provider-limit patterns",
    "[ai][retry][issue361]") {
    for (const std::string message :
         {"GoUsageLimitError", "FreeUsageLimitError",
          "Monthly usage limit reached", "available balance",
          "insufficient_quota", "out of budget", "quota exceeded",
          "billing error", "billing", "You have exceeded your billing limit"}) {
        CHECK_FALSE(ai::is_retryable_assistant_error(
            error_terminal(message)));
    }
    // The non-retryable limit check wins even when a retryable pattern also
    // matches (pi checks NON_RETRYABLE first).
    CHECK_FALSE(ai::is_retryable_assistant_error(
        error_terminal("insufficient_quota: 429 quota exceeded")));
    CHECK_FALSE(ai::is_retryable_assistant_error(
        error_terminal("Monthly usage limit reached (503)")));
}

TEST_CASE(
    "isRetryableAssistantError requires an error terminal with an error message",
    "[ai][retry][issue361]") {
    auto stopped = ai::assistant_text_message("fine");
    stopped.stop_reason = ai::AssistantStopReason::Stop;
    CHECK_FALSE(ai::is_retryable_assistant_error(stopped));

    auto aborted = ai::assistant_text_message("");
    aborted.stop_reason = ai::AssistantStopReason::Aborted;
    aborted.error_message = "overloaded";
    CHECK_FALSE(ai::is_retryable_assistant_error(aborted));

    auto tool_use = ai::assistant_text_message("");
    tool_use.stop_reason = ai::AssistantStopReason::ToolUse;
    tool_use.error_message = "overloaded";
    CHECK_FALSE(ai::is_retryable_assistant_error(tool_use));

    auto no_message = error_terminal("");
    CHECK_FALSE(ai::is_retryable_assistant_error(no_message));

    // Unmatched error text is not retryable.
    CHECK_FALSE(ai::is_retryable_assistant_error(
        error_terminal("model returned an unexpected response")));
}
