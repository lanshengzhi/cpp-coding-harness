#pragma once

#include <map>
#include <string>
#include <string_view>

namespace cch::coding_agent {

/// The Pending Elicitation modes this build can put in front of the user
/// (spec #833 stories 26-27). The set is deliberately not the protocol's: a
/// mode this build cannot render is refused locally as one failed tool call
/// rather than shown as a dialog that cannot collect an answer.
enum class McpElicitationMode { Form, Url };

/// What the user did with one Pending Elicitation. All three are answers the
/// Upstream is told about on the retried request.
enum class McpElicitationAction { Accept, Decline, Cancel };

/// The action's spelling as the user-facing control names it.
[[nodiscard]] std::string_view to_string(McpElicitationAction action) noexcept;

/// One Pending Elicitation as this Owner projects it (issue #845; spec #833
/// stories 26-29). It is a passive mirror of what the MCP Host asked: the
/// `frontend_tui` reads it from here and never from the MCP package itself
/// (ADR 0065).
///
/// The Upstream's opaque continuation token is deliberately **absent**. It is
/// the server's own bookkeeping, the host echoes it unchanged, and no
/// presentation surface can read, normalize, or reconstruct it by not holding
/// it at all.
struct McpPendingElicitation {
    /// The handle the answer is addressed to, unique among the elicitations
    /// this session currently has pending.
    std::string elicitation_id{};
    /// The Server Id of the Upstream that asked, so a dialog can say which
    /// server is waiting.
    std::string server_id{};
    /// The Upstream's own tool name — the work the user is authorizing is the
    /// server's, and the reverse mapping that produces a Qualified Tool Name
    /// belongs to the tool publication projection.
    std::string tool_name{};
    McpElicitationMode mode{McpElicitationMode::Url};
    /// The Upstream's identifier for this request, echoed back in the answer.
    /// Empty when the Upstream named none.
    std::string request_id{};
    /// Bounded, redacted text the Upstream asked the user with.
    std::string message{};
    /// URL mode: the address the dialog shows and the open-browser action
    /// targets. Opening it is not an answer.
    std::string url{};
    /// Form mode: the Upstream's own JSON Schema as text, uninterpreted by
    /// this Owner (issue #846 renders it).
    std::string form_schema{};
};

/// The user's answer to one Pending Elicitation.
///
/// `form_values` is the form mode's answer and is empty in URL mode; it is
/// here so the shared Multi Round-Trip loop is identical for both modes and
/// adding form-mode rendering changes no part of the answer path.
struct McpElicitationAnswer {
    std::string elicitation_id{};
    McpElicitationAction action{McpElicitationAction::Accept};
    std::map<std::string, std::string, std::less<>> form_values{};
};

} // namespace cch::coding_agent
