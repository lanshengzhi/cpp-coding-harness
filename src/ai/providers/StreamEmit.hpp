#pragma once

#include <cch/ai/StreamEvent.hpp>

#include <exception>

namespace cch::ai::providers {

/// Emits one assistant stream event through the consumer-owned sink; an empty
/// sink is tolerated as a no-op.
[[nodiscard]] inline support::ExpectedVoid emit(
    ai::AssistantEventSink& sink,
    const ai::AssistantStreamEvent& event) {
    if (!sink) {
        return {};
    }
    try {
        return sink(event);
    } catch (const std::exception& error) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Unknown,
            "Assistant event sink failed",
            error.what()));
    } catch (...) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Unknown,
            "Assistant event sink failed"));
    }
}

} // namespace cch::ai::providers
