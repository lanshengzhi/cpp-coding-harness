#pragma once

#include <cch/ai/StreamEvent.hpp>

namespace cch::ai::providers {

/// Emits one assistant stream event through the consumer-owned sink; an empty
/// sink is tolerated as a no-op.
[[nodiscard]] inline support::ExpectedVoid emit(
    ai::AssistantEventSink& sink,
    const ai::AssistantStreamEvent& event) {
    if (!sink) {
        return {};
    }
    return sink(event);
}

/// Emits the one AssistantStartEvent for a stream attempt, flipping `started`
/// so repeat calls are no-ops.
[[nodiscard]] inline support::ExpectedVoid emit_start(
        ai::AssistantEventSink& sink, const ai::AssistantMessage& assistant, bool& started) {
    if (started) {
        return {};
    }
    started = true;
    return emit(sink, ai::AssistantStartEvent{.partial = assistant});
}

} // namespace cch::ai::providers
