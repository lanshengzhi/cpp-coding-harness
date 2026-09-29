#pragma once

#include <cch/support/BoundedText.hpp>
#include <cch/support/Error.hpp>
#include "mcp/Protocol.hpp"
#include "mcp/Redaction.hpp"

#include <string>
#include <string_view>
#include <utility>

namespace cch::mcp::diagnostics {

/// Any text the MCP Host reports about what an Upstream MCP Server did. It is
/// redacted before it is truncated, so a complete `[REDACTED]` marker
/// survives the bound, and it is bounded by the one diagnostic limit in
/// `mcp/Protocol.hpp` (CODING_STANDARDS.md §10.2).
///
/// This is the package's one private diagnostics point: the client stack
/// (issue #838) and the connection machinery (issue #839) both bound their
/// diagnostics here rather than through a second rule of their own.
[[nodiscard]] inline std::string bounded(std::string text) {
    return cch::support::bounded_redacted_text(std::move(text), protocol::kMaxDiagnosticBytes, "...");
}

/// The package's **one** redact-then-bound operation, for a text a known
/// credential value reached: a value echoed back by the Upstream carries no
/// key for the shape-based rules to match, so the value itself is erased, and
/// it is erased *before* the bound, so no truncation point can leave a part of
/// it behind (issue #838, CODING_STANDARDS.md §10.2, §10.7). An absent secret
/// leaves the text exactly as the one-argument `bounded` would.
///
/// Every redaction point in this package calls this overload — the credential
/// path (`UpstreamAuth.cpp`), the client stack and connection machinery
/// (`UpstreamClient.cpp`), the OAuth flow (`UpstreamOAuth.cpp`), and the
/// loopback callback listener (`OAuthCallbackServer.cpp`) — so "redacted, then
/// bounded" is one rule rather than one per call site.
[[nodiscard]] inline std::string bounded(std::string text, std::string_view secret) {
    return bounded(redaction::erase_credential(std::move(text), secret));
}

/// One error's bounded diagnostic. An `Error::context` is never carried into
/// it, because a JSON parse failure puts the whole untrusted response body
/// there.
[[nodiscard]] inline std::string of(const cch::support::Error& error) {
    return bounded(error.detail.empty() ? error.message : error.message + ": " + error.detail);
}

/// The same diagnostic with the connection's resolved credential erased from
/// it, so an Upstream that echoes the bearer back in a message or in a detail
/// cannot put it in a diagnostic that reaches a status surface or a session
/// record (issue #838).
[[nodiscard]] inline std::string of(const cch::support::Error& error, std::string_view secret) {
    return bounded(error.detail.empty() ? error.message : error.message + ": " + error.detail, secret);
}

} // namespace cch::mcp::diagnostics
