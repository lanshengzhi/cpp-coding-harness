#pragma once

#include <cch/ai/ChatClient.hpp>

#include <exception>

namespace cch::ai::providers {

/// Emits one assistant stream event through the consumer-owned sink; an empty
/// sink is tolerated as a no-op.
[[nodiscard]] inline util::ExpectedVoid emit(
    ai::AssistantEventSink& sink,
    const ai::AssistantStreamEvent& event) {
    if (!sink) {
        return {};
    }
    try {
        return sink(event);
    } catch (const std::exception& error) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Unknown,
            "Assistant event sink failed",
            error.what()));
    } catch (...) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Unknown,
            "Assistant event sink failed"));
    }
}

} // namespace cch::ai::providers
