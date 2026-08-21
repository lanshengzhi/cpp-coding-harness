#pragma once

#include "coding_agent/BoundedText.hpp"

#include <cch/support/Error.hpp>

#include <format>
#include <string>
#include <utility>

namespace cch::coding_agent::tui {

/// The chat-diagnostic error text: `message: detail (context)` with a
/// redundant detail and an empty context dropped.
[[nodiscard]] inline std::string combined_error_text(const support::Error& error) {
    std::string text = error.message;
    if (!error.detail.empty() && error.detail != error.message) {
        text = std::format("{}: {}", text, error.detail);
    }
    if (error.context && !error.context->empty()) {
        text = std::format("{} ({})", text, *error.context);
    }
    return text;
}

/// One bounded chat-presentation error carrying `message` with the source
/// error's code and its bounded, redacted combined text as the detail.
[[nodiscard]] inline support::Error presentation_error(
    const support::Error& error,
    std::string message) {
    return support::make_error(
        error.code,
        std::move(message),
        bounded_redacted_presentation(combined_error_text(error)));
}

/// The startup failure presentation (pi's boot-failure stderr line).
[[nodiscard]] inline support::Error startup_error(const support::Error& error) {
    return presentation_error(error, "Native TUI startup failed");
}

/// One bounded presentation error aggregating a primary failure with the
/// restoration failure that followed it (e.g. startup plus terminal
/// restore).
[[nodiscard]] inline support::Error aggregate_presentation_errors(
    const support::Error& primary,
    const support::Error& restoration,
    std::string message) {
    return support::make_error(
        primary.code,
        std::move(message),
        bounded_redacted_presentation(std::format(
            "primary: {}; restoration: {}",
            combined_error_text(primary),
            combined_error_text(restoration))));
}

} // namespace cch::coding_agent::tui
