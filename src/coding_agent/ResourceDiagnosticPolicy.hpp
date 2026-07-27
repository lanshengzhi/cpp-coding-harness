#pragma once

#include "util/BoundedText.hpp"

#include <cstddef>
#include <string>
#include <utility>

namespace cch::coding_agent::detail {

inline constexpr std::size_t kMaxResourceDiagnostics = 64;
inline constexpr std::size_t kMaxResourceDiagnosticTextBytes = 1024;

inline void bound_resource_diagnostic_text(std::string& text) {
    text = util::bounded_redacted_text(
        std::move(text),
        kMaxResourceDiagnosticTextBytes,
        "...[truncated]");
}

} // namespace cch::coding_agent::detail
