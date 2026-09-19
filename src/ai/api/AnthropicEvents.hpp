#pragma once

#include "ai/api/Termination.hpp"
#include "ai/providers/Provider.hpp"
#include "ai/providers/SseParser.hpp"

#include <cch/ai/InferenceFailure.hpp>
#include <cch/ai/Message.hpp>
#include <cch/ai/StreamEvent.hpp>
#include <cch/support/Error.hpp>

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace cch::ai::api {

/// One open Anthropic content block, keyed by the provider's block index.
struct AnthropicBlockSlot {
    enum class Kind { Thinking, Text, ToolCall };

    Kind kind{Kind::Text};
    std::size_t content_index{};
    std::string partial_arguments;
};

/// Per-attempt Anthropic SSE state: the open blocks plus the terminal
/// bookkeeping the finalize step needs. One value per streaming attempt.
struct AnthropicEventState {
    std::map<std::size_t, AnthropicBlockSlot> slots{};
    bool saw_message_start{false};
    bool saw_message_stop{false};
    std::optional<TerminationResult> termination{std::nullopt};
};

/// One decoded Anthropic SSE event → stream events on `sink`.
[[nodiscard]] support::ExpectedVoid process_anthropic_sse_event(const providers::SseEvent& event,
        const Model& model,
        AssistantMessage& assistant,
        AnthropicEventState& state,
        AssistantEventSink& sink,
        std::optional<InferenceFailure>& inference_failure);

/// Terminal verification for one attempt: `message_stop` observed, a stop
/// reason resolved, and a provider-reported failure surfaced as a Stream error.
[[nodiscard]] support::ExpectedVoid finalize_anthropic_stream(AnthropicEventState& state, AssistantMessage& assistant);

} // namespace cch::ai::api
