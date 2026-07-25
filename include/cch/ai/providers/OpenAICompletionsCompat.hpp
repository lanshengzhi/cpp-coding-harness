#pragma once

#include <optional>

namespace cch::ai::providers {

// Every flag here has an observable effect on the emitted request or the
// parsed response. Deferred pi-parity controls add no inert fields
// (ADR 0019); they return together with the StreamChatRequest controls that
// make them observable.
struct OpenAICompletionsCompat {
    std::optional<bool> supports_store;
    std::optional<bool> supports_developer_role;
    std::optional<bool> supports_usage_in_streaming;
    std::optional<bool> requires_tool_result_name;
    std::optional<bool> requires_assistant_after_tool_result;
    std::optional<bool> requires_thinking_as_text;
};

} // namespace cch::ai::providers
