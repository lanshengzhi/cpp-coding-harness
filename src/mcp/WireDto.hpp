#pragma once

#include <cch/mcp/UpstreamServer.hpp>
#include <cch/mcp/UpstreamTool.hpp>
#include <cch/support/Error.hpp>
#include <cch/support/JsonValue.hpp>

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cch::mcp::dto {

/// One decoded `tools/list` entry: either the tool the catalog admits, or the
/// reason the catalog rejects it. A rejected tool is dropped on its own, so a
/// single bad annotation cannot cost a server its whole catalog; a tool that
/// cannot even be named fails the page instead, because there is nothing to
/// skip cleanly.
struct ToolListEntry {
    std::optional<cch::mcp::UpstreamToolDescriptor> tool{};
    std::string rejection{};
};

/// One decoded `tools/list` page.
struct ToolListPage {
    std::vector<cch::mcp::UpstreamToolDescriptor> tools{};
    std::optional<std::string> next_cursor{};
    /// The `ttlMs` freshness hint this page carries, absent when the page
    /// carries none. Read only from the page that completes the walk, so a
    /// hint describing an incomplete catalog is never read.
    std::optional<std::chrono::milliseconds> freshness{std::nullopt};
    /// The `cacheScope` this page carries, absent when the page carries none.
    std::optional<std::string> cache_scope{std::nullopt};
};

/// One decoded `tools/call` result carrying the server's own outcome.
struct ToolCallOutcome {
    bool is_error{false};
    cch::support::JsonValue content{};
};

/// The `type` of one input request an `input_required` result declares. Only
/// the two modes this build advertises are declared; any other value is an
/// **undeclared** type and fails the one tool call without asking the user.
enum class InputRequestType { Url, Form, Undeclared };

/// One input request lifted out of an `input_required` result. The schema in
/// form mode is the Upstream's own JSON Schema, carried as an uninterpreted
/// value: the MRTR loop does not validate against it, and rendering it is the
/// presentation layer's business.
struct InputRequest {
    /// The Upstream's own identifier for this request, echoed back in the
    /// answer. Absent when the Upstream did not name it, in which case the
    /// answer is matched positionally.
    std::string id{};
    InputRequestType type{InputRequestType::Undeclared};
    /// Bounded, redacted text the Upstream asked the user with. Never the
    /// `requestState`, which is opaque and is not read here.
    std::string message{};
    /// URL mode: the address the dialog shows and the open-browser action
    /// targets. Never fetched, never resolved, and never treated as consent.
    std::string url{};
    /// Form mode: the Upstream's own JSON Schema, uninterpreted.
    cch::support::JsonValue form_schema{};
};

/// One decoded `input_required` result: the input requests the Upstream is
/// blocked on, plus its opaque continuation token.
///
/// `request_state` is the token's **source text**, not a decoded value, so the
/// retry can splice it back byte-for-byte. It is bounded only for safety; an
/// over-long token is a bounded diagnostic, never a truncated one, because a
/// truncated token is a different token.
struct ToolCallInputRequest {
    std::vector<InputRequest> requests{};
    std::string request_state{};
};

// The wire DTOs of the released 2026-07-28 revision that this build decodes.
// The decoders are the contract the tests snapshot, so implementation and
// golden cannot drift wrong together. Each is fail-closed: a required field
// that is missing or of the wrong type fails the decode rather than decoding
// to a default, and no diagnostic ever echoes an Upstream-supplied value that
// could carry a credential.

/// Decode a `server/discover` result. The declared capability set is checked
/// against the capabilities this build implements, so an unrecognized
/// capability fails the probe.
[[nodiscard]] cch::support::Expected<cch::mcp::UpstreamServerInfo> read_discover_result(
        const cch::support::JsonValue& result);

/// Decode one `tools/list` page. A tool that cannot even be named fails the
/// page; a tool whose `x-mcp-header` annotation is invalid is rejected on its
/// own and never reaches the catalog, so it can never be registered or called.
[[nodiscard]] cch::support::Expected<ToolListPage> read_tool_list_page(const cch::support::JsonValue& result);

/// Decode a `tools/call` result. An absent or unrecognized `resultType` and
/// a malformed result DTO both fail the decode, which the client stack turns
/// into exactly one failed tool call. An `input_required` result is **not** a
/// decode failure: read it with `read_input_required_result`.
[[nodiscard]] cch::support::Expected<ToolCallOutcome> read_tool_call_result(const cch::support::JsonValue& result);

/// Whether a decoded `tools/call` result is a Multi Round-Trip
/// `input_required` result rather than a completed call.
[[nodiscard]] bool is_input_required(const cch::support::JsonValue& result);

/// Decode an `input_required` `tools/call` result. `raw_result` is the
/// result object's **source text** as the Upstream sent it, from which the
/// opaque `requestState` is taken verbatim; a result with no source text, or
/// one whose `requestState` is present but unrecoverable, is a protocol
/// violation rather than a token this host may invent.
///
/// A member of `inputRequests` whose `type` is neither `url` nor `form` is
/// **not** rejected here: the loop reports the undeclared type as one failed
/// tool call, so the server learns the host cannot express an answer for it
/// while the host still fails only the one call (ADR 0008).
[[nodiscard]] cch::support::Expected<ToolCallInputRequest> read_input_required_result(
        const cch::support::JsonValue& result, std::string_view raw_result);

} // namespace cch::mcp::dto
