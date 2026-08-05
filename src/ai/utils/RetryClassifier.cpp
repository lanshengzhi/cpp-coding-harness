// Turn auto-retry retryability classification, ported from pi
// `packages/ai/src/utils/retry.ts` (`isRetryableAssistantError`,
// `NON_RETRYABLE_PROVIDER_LIMIT_ERROR_PATTERN`,
// `RETRYABLE_PROVIDER_ERROR_PATTERN`). The patterns are translated to
// `std::regex` ECMAScript syntax with the same `i` flag semantics: `.` matches
// any non-newline character, so pi's `.?` word gaps ("rate.?limit") accept a
// single arbitrary separator character exactly like the JS regex.

#include "ai/utils/RetryClassifier.hpp"

#include <regex>
#include <string>
#include <vector>

namespace cch::ai {
namespace {

/// pi `NON_RETRYABLE_PROVIDER_LIMIT_ERROR_PATTERN` — subscription/account
/// limits returned as 429-style JSON error types and generic
/// quota/budget/billing exhaustion wording.
[[nodiscard]] const std::vector<std::regex>& non_retryable_limit_patterns() {
    static const std::vector<std::regex> patterns{
        std::regex{R"(GoUsageLimitError)", std::regex_constants::icase},
        std::regex{R"(FreeUsageLimitError)", std::regex_constants::icase},
        std::regex{R"(Monthly usage limit reached)", std::regex_constants::icase},
        std::regex{R"(available balance)", std::regex_constants::icase},
        std::regex{R"(insufficient_quota)", std::regex_constants::icase},
        std::regex{R"(out of budget)", std::regex_constants::icase},
        std::regex{R"(quota exceeded)", std::regex_constants::icase},
        std::regex{R"(billing)", std::regex_constants::icase},
    };
    return patterns;
}

/// pi `RETRYABLE_PROVIDER_ERROR_PATTERN` — generic provider load, HTTP status,
/// server-side transient failures, network/proxy/fetch transport failures,
/// WebSocket close/error text, premature stream endings, provider-requested
/// retry guidance, and gRPC-based exhaustion.
[[nodiscard]] const std::vector<std::regex>& retryable_provider_patterns() {
    static const std::vector<std::regex> patterns{
        std::regex{R"(overloaded)", std::regex_constants::icase},
        std::regex{R"(rate.?limit)", std::regex_constants::icase},
        std::regex{R"(too many requests)", std::regex_constants::icase},
        std::regex{R"(429)", std::regex_constants::icase},
        std::regex{R"(500)", std::regex_constants::icase},
        std::regex{R"(502)", std::regex_constants::icase},
        std::regex{R"(503)", std::regex_constants::icase},
        std::regex{R"(504)", std::regex_constants::icase},
        std::regex{R"(524)", std::regex_constants::icase},
        std::regex{R"(service.?unavailable)", std::regex_constants::icase},
        std::regex{R"(server.?error)", std::regex_constants::icase},
        std::regex{R"(internal.?error)", std::regex_constants::icase},
        std::regex{R"(provider.?returned.?error)", std::regex_constants::icase},
        std::regex{R"(network.?error)", std::regex_constants::icase},
        std::regex{R"(connection.?error)", std::regex_constants::icase},
        std::regex{R"(connection.?refused)", std::regex_constants::icase},
        std::regex{R"(connection.?lost)", std::regex_constants::icase},
        std::regex{R"(other side closed)", std::regex_constants::icase},
        std::regex{R"(fetch failed)", std::regex_constants::icase},
        std::regex{R"(getaddrinfo)", std::regex_constants::icase},
        std::regex{R"(ENOTFOUND)", std::regex_constants::icase},
        std::regex{R"(EAI_AGAIN)", std::regex_constants::icase},
        std::regex{R"(upstream.?connect)", std::regex_constants::icase},
        std::regex{R"(reset before headers)", std::regex_constants::icase},
        std::regex{R"(socket hang up)", std::regex_constants::icase},
        std::regex{R"(socket connection was closed)", std::regex_constants::icase},
        std::regex{R"(timed? out)", std::regex_constants::icase},
        std::regex{R"(timeout)", std::regex_constants::icase},
        std::regex{R"(terminated)", std::regex_constants::icase},
        std::regex{R"(websocket.?closed)", std::regex_constants::icase},
        std::regex{R"(websocket.?error)", std::regex_constants::icase},
        std::regex{R"(ended without)", std::regex_constants::icase},
        std::regex{R"(stream ended before message_stop)", std::regex_constants::icase},
        std::regex{R"(stream ended before a terminal response event)", std::regex_constants::icase},
        std::regex{R"(http2 request did not get a response)", std::regex_constants::icase},
        std::regex{R"(retry delay)", std::regex_constants::icase},
        std::regex{R"(you can retry your request)", std::regex_constants::icase},
        std::regex{R"(try your request again)", std::regex_constants::icase},
        std::regex{R"(please retry your request)", std::regex_constants::icase},
        std::regex{R"(ResourceExhausted)", std::regex_constants::icase},
    };
    return patterns;
}

[[nodiscard]] bool matches_any(
    const std::vector<std::regex>& patterns,
    const std::string& text) {
    for (const auto& pattern : patterns) {
        if (std::regex_search(text, pattern)) {
            return true;
        }
    }
    return false;
}

} // namespace

bool is_retryable_assistant_error(const ai::AssistantMessage& message) {
    if (message.stop_reason != ai::AssistantStopReason::Error ||
        !message.error_message) {
        return false;
    }
    const std::string& error_message = *message.error_message;
    if (matches_any(non_retryable_limit_patterns(), error_message)) {
        return false;
    }
    return matches_any(retryable_provider_patterns(), error_message);
}

} // namespace cch::ai
