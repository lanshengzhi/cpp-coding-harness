#pragma once

#include <cch/support/BoundedText.hpp>
#include <cch/support/Error.hpp>
#include "mcp/Protocol.hpp"

#include <string>
#include <utility>

namespace cch::mcp::diagnostics {

/// Any text the MCP Host reports about what an Upstream MCP Server did. It is
/// redacted before it is truncated, so a complete `[REDACTED]` marker
/// survives the bound, and it is bounded by the one diagnostic limit in
/// `mcp/Protocol.hpp` (CODING_STANDARDS.md §10.2).
[[nodiscard]] inline std::string bounded(std::string text) {
    return cch::support::bounded_redacted_text(std::move(text), protocol::kMaxDiagnosticBytes, "...");
}

/// One error's bounded diagnostic. An `Error::context` is never carried into
/// it, because a JSON parse failure puts the whole untrusted response body
/// there.
[[nodiscard]] inline std::string of(const cch::support::Error& error) {
    return bounded(error.detail.empty() ? error.message : error.message + ": " + error.detail);
}

} // namespace cch::mcp::diagnostics
