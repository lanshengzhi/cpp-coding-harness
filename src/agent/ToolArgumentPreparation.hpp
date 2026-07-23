#pragma once

#include "../../include/cch/ai/Content.hpp"
#include "../../include/cch/ai/Tool.hpp"
#include "../../include/cch/util/Error.hpp"
#include "../../include/cch/util/JsonValue.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace cch::agent {

[[nodiscard]] inline std::string bounded_tool_argument_text(
    std::string text,
    std::size_t max_bytes,
    std::string_view suffix) {
    if (text.size() <= max_bytes) {
        return text;
    }
    if (max_bytes <= suffix.size()) {
        return std::string(suffix.substr(0, max_bytes));
    }
    std::size_t keep = max_bytes - suffix.size();
    while (keep > 0 &&
           (static_cast<unsigned char>(text[keep]) & 0xc0) == 0x80) {
        --keep;
    }
    text.resize(keep);
    text += suffix;
    return text;
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
[[nodiscard]] util::Expected<util::JsonValue> prepare_tool_arguments(
    const ai::Tool& tool,
    const ai::ToolCallContent& call);

} // namespace cch::agent
