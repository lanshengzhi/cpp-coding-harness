#pragma once

#include <cch/ai/Content.hpp>
#include <cch/ai/Tool.hpp>
#include <cch/support/Error.hpp>
#include <cch/support/JsonValue.hpp>
#include "support/BoundedText.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace cch::agent {

[[nodiscard]] inline std::string bounded_tool_argument_text(
    std::string text,
    std::size_t max_bytes,
    std::string_view suffix) {
    return support::bounded_text(text, max_bytes, suffix);
}

[[nodiscard]] inline std::string bounded_tool_argument_component(
    std::string component,
    std::size_t max_bytes) {
    return bounded_tool_argument_text(
        std::move(component),
        max_bytes,
        " [truncated]");
}

[[nodiscard]] inline std::string bounded_tool_argument_diagnostic(std::string diagnostic) {
    return bounded_tool_argument_text(
        std::move(diagnostic),
        4096,
        " [diagnostic truncated]");
}

/** Parse, clone, coerce, and validate one call against its private executable profile. */
[[nodiscard]] support::Expected<support::JsonValue> prepare_tool_arguments(
    const ai::Tool& tool,
    const ai::ToolCallContent& call);

} // namespace cch::agent
