#pragma once

#include <cch/ai/Context.hpp>
#include <cch/ai/Model.hpp>
#include <cch/ai/Provider.hpp>
#include <cch/support/Error.hpp>
#include <cch/support/JsonValue.hpp>

namespace cch::ai::api {

enum class AdapterKind {
    OpenAICodexResponses,
    OpenAIResponses,
    AnthropicMessages,
};

/// Build one transport-independent wire payload using the shared normalized
/// history and streamSimple values. No provider SDK DTO crosses this boundary.
[[nodiscard]] support::Expected<support::JsonValue> build_adapter_payload(
    AdapterKind adapter,
    const Model& model,
    const AiContext& context,
    const ProviderStreamOptions& options);

/// Build the Responses input items pi replays for one completed assistant
/// message when continuing a Codex WebSocket session: no system prompt, no
/// tool outputs (pi `convertResponsesMessages` with `includeSystemPrompt:
/// false` filtered to non-output items). Used for the `previous_response_id`
/// input delta on a reused socket.
[[nodiscard]] support::Expected<support::JsonValue::array_t> build_responses_continuation_items(
    const Model& model,
    const AssistantMessage& assistant);

} // namespace cch::ai::api
