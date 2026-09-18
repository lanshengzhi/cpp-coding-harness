#include "OpenAICodexResponsesAdapter.hpp"

#include "MessageConversion.hpp"
#include "ai/Headers.hpp"
#include "ai/Timestamps.hpp"
#include "ai/api/PartialJson.hpp"
#include "ai/api/ResponsesEventProcessor.hpp"
#include "ai/auth/Pkce.hpp"
#include "ai/providers/ProviderError.hpp"
#include "ai/providers/RetryPolicy.hpp"
#include "ai/providers/SseParser.hpp"
#include "ai/providers/StreamEmit.hpp"
#include "ai/providers/StreamExecutionEngine.hpp"
#include "support/ExpectedMacros.hpp"
#include "support/Json.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "CodexWebSocketCache.hpp"

namespace cch::ai::api {

[[nodiscard]] std::optional<JsonArray> cached_input_delta(
        const support::JsonValue& body, const CodexContinuation& continuation) {
    if (!request_bodies_match_except_input(body, continuation.last_request_body)) {
        return std::nullopt;
    }
    const auto* body_object = body.get_if<JsonObject>();
    if (!body_object) {
        return std::nullopt;
    }
    JsonArray current;
    if (const auto found = body_object->find("input"); found != body_object->end()) {
        const auto* items = found->second.get_if<JsonArray>();
        if (!items) {
            return std::nullopt;
        }
        current = *items;
    }
    JsonArray baseline;
    if (const auto found = continuation.last_request_body.get_if<JsonObject>(); found != nullptr) {
        if (const auto input = found->find("input"); input != found->end()) {
            const auto* items = input->second.get_if<JsonArray>();
            if (items) {
                baseline = *items;
            }
        }
    }
    baseline.insert(baseline.end(), continuation.last_response_items.begin(), continuation.last_response_items.end());
    if (current.size() < baseline.size()) {
        return std::nullopt;
    }
    const auto baseline_bytes = support::write_json(support::JsonValue{baseline});
    const auto prefix_bytes =
            support::write_json(support::JsonValue{JsonArray{current.begin(), current.begin() + baseline.size()}});
    if (!baseline_bytes || !prefix_bytes || *baseline_bytes != *prefix_bytes) {
        return std::nullopt;
    }
    return JsonArray{current.begin() + baseline.size(), current.end()};
}

} // namespace cch::ai::api
