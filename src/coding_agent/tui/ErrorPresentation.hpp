#pragma once

#include <cch/support/Error.hpp>

#include <format>
#include <string>

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

} // namespace cch::coding_agent::tui
