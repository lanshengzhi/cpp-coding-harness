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

} // namespace cch::ai::providers
