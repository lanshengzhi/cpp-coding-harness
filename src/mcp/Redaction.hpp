#pragma once

#include <cch/support/JsonValue.hpp>

#include <string>
#include <string_view>

namespace cch::mcp::redaction {

/// The MCP Host's one approved credential-erasure implementation (ADR 0064,
/// ADR 0065; issue #838). Every piece of text the package hands to a caller
/// passes through here, and the package exposes no other redaction path
/// (CODING_STANDARDS.md §10.1). The diagnostic *bound* is not implemented
/// here: it lives in the one private diagnostics point, `mcp/Diagnostics.hpp`,
/// which the client stack and the connection machinery share (issue #839).
[[nodiscard]] std::string bounded_diagnostic(std::string text);

/// Erase one known credential value from a text, wherever it appears. A token
/// echoed without its `Authorization` key carries no key for the shape-based
/// rules to match, so the value itself is what is erased.
///
/// Erasing the resolved credential is the authority of issue #838, which
/// exists so that a leaked bearer in a log, a transcript, or a diagnostic
/// cannot happen (CODING_STANDARDS.md §10.7). A value too short to identify
/// without destroying the text around it is left to the shape-based rules.
[[nodiscard]] std::string erase_credential(std::string text, std::string_view secret);

/// `bounded_diagnostic` for a text a known credential value reached: the value
/// is erased first, so the shape-based redaction and the bound both see a text
/// that no longer carries the secret, and neither a bound nor a truncation
/// point can leave a part of it behind (CODING_STANDARDS.md §10.2, §10.7).
[[nodiscard]] std::string redacted_text(std::string text, std::string_view secret);

/// The same erasure over a JSON value's string leaves, for an Upstream tool
/// result that echoed a credential back: the value reaches neither the model's
/// context nor a session record with the secret still in it. Numbers, booleans,
/// and structure are untouched — only text an Upstream supplied is rewritten.
[[nodiscard]] cch::support::JsonValue redacted_value(cch::support::JsonValue value, std::string_view secret);

} // namespace cch::mcp::redaction
