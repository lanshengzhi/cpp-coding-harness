#pragma once

// One spelling of a comma-joined tool-name list for user-visible diagnostics,
// shared by tool selection and session assembly so the two error texts cannot
// drift (§11.5-adjacent de-duplication; used only inside cch_coding_agent).

#include <span>
#include <string>

namespace cch::coding_agent::runtime::detail {

/// Join tool names as `a, b, c`; an empty list yields an empty string.
[[nodiscard]] inline std::string join_tool_names(std::span<const std::string> names) {
    std::string joined;
    for (const auto& name : names) {
        if (!joined.empty()) {
            joined += ", ";
        }
        joined += name;
    }
    return joined;
}

} // namespace cch::coding_agent::runtime::detail
