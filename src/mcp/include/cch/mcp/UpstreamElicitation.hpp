#pragma once

#include <cch/mcp/UpstreamDelay.hpp>
#include <cch/support/AsyncResult.hpp>
#include <cch/support/Error.hpp>
#include <cch/support/JsonValue.hpp>

#include <chrono>
#include <functional>
#include <stop_token>
#include <string>

namespace cch::mcp {

/// The two Pending Elicitation modes pike advertises and can express an
/// answer for (ADR 0064: `elicitation: {form: {}, url: {}}`).
///
/// The enum is the whole of the mode split this package owns, and it is
/// complete on purpose: an input request whose type is neither of these is
/// **undeclared**, and an undeclared type is a single failed tool call rather
/// than a mode this build guesses at.
enum class ElicitationMode { Form, Url };

/// What the user did with one Pending Elicitation. All three are answers the
/// Upstream is told about on the retried request; only a *stopped wait* (the
/// session's stop token, or the elicitation bound expiring) suppresses the
/// retry and fails the call.
enum class ElicitationAction { Accept, Decline, Cancel };

/// The action's spelling on the wire, as the retried request carries it.
[[nodiscard]] std::string_view to_string(ElicitationAction action) noexcept;

/// One Pending Elicitation as the product presents it.
///
/// The value is passive: the MCP Host never sees a dialog, a terminal, or a
/// selection, and this package never renders one. The Server Id and the
/// Upstream's own tool name travel with it so a dialog can say which server
/// asked, and `form_schema` is the Upstream's own JSON Schema carried
/// **uninterpreted** — this package does not validate against it, and a mode
/// that arrives without a usable schema is the presentation layer's problem.
struct ElicitationRequest {
    /// The Server Id of the Upstream that raised the request.
    std::string server_id{};
    /// The Upstream's own tool name, not the Qualified Tool Name: the
    /// elicitation is about the server's work, and the reverse mapping that
    /// produces a Qualified Tool Name belongs to the session that published
    /// the tool.
    std::string tool_name{};
    ElicitationMode mode{ElicitationMode::Url};
    /// The Upstream's identifier for this request, echoed back in the answer.
    /// Empty when the Upstream named none, in which case the answer is
    /// matched positionally.
    std::string request_id{};
    /// Bounded, redacted text the Upstream asked the user with.
    std::string message{};
    /// URL mode: the address the dialog shows and the open-browser action
    /// targets. The host never fetches it, never resolves it, and never
    /// treats opening it as an answer.
    std::string url{};
    /// Form mode: the Upstream's own JSON Schema, uninterpreted.
    cch::support::JsonValue form_schema{};
};

/// The user's answer to one Pending Elicitation. `form_content` is the form
/// mode's answer and is a null value in URL mode; it is carried here so the
/// shared MRTR loop is identical for both modes and adding form-mode
/// rendering changes no part of the loop.
struct ElicitationAnswer {
    ElicitationAction action{ElicitationAction::Accept};
    std::string request_id{};
    cch::support::JsonValue form_content{cch::support::JsonValue{}};
};

/// The one seam through which the MCP Host asks the user a Pending Elicitation
/// question.
///
/// It is a passive value, not a callback into a dialog: the port takes the
/// request and a `stop_token` and returns a pending operation, so the MRTR
/// loop owns the wait, the bound, and the cancellation rather than a UI
/// coroutine. A port that fails returns the failure as the operation's
/// terminal outcome, and the MRTR loop then fails exactly one tool call.
using ElicitationPrompter = std::move_only_function<cch::support::AsyncResult<ElicitationAnswer>(
        ElicitationRequest request, std::stop_token stop_token)>;

/// The port plus the two things a wait needs to be bounded: the owner's timer
/// and how long the wait may last.
///
/// The timer is the same seam the reconnect ladder and the connection cleanup
/// bound use, so a session that has one has this; a port without a timer can
/// still ask, but its wait is then bounded only by the caller's stop token.
struct UpstreamElicitationPort {
    ElicitationPrompter prompter{nullptr};
    UpstreamDelay delay{nullptr};
    std::chrono::milliseconds timeout{std::chrono::minutes{5}};
};

} // namespace cch::mcp
