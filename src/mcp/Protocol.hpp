#pragma once

#include <cch/support/JsonValue.hpp>

#include <chrono>
#include <cstddef>
#include <string_view>

namespace cch::mcp::protocol {

/// The one private constants point of the MCP wire layer (ADR 0064). The
/// conformance target is the released 2026-07-28 revision; tracking a later
/// draft is a maintenance task that touches only this file, never a scattered
/// spelling across the package.

/// The protocol revision this build speaks and advertises.
inline constexpr std::string_view kProtocolVersion{"2026-07-28"};

/// The reserved `io.modelcontextprotocol/*` request `_meta` keys. They are
/// reserved, so a caller-supplied `_meta` can neither override nor extend
/// them: the client stack rewrites both on every request.
inline constexpr std::string_view kMetaProtocolVersionKey{"io.modelcontextprotocol/protocolVersion"};
inline constexpr std::string_view kMetaClientCapabilitiesKey{"io.modelcontextprotocol/clientCapabilities"};

/// The `server/discover` result member carrying the revision the Upstream
/// speaks. It is the same value the reserved `_meta` key carries, reached as
/// a result field rather than as per-request metadata.
inline constexpr std::string_view kResultProtocolVersion{"protocolVersion"};

/// The `clientCapabilities` this build advertises: exactly
/// `elicitation: {form: {}, url: {}}` — no `roots`, no `sampling`, no
/// `extensions` (ADR 0064). Rebuilt from scratch for every request.
[[nodiscard]] inline cch::support::JsonValue client_capabilities() {
    using JsonValue = cch::support::JsonValue;
    return JsonValue::object_t{
            {"elicitation", JsonValue::object_t{{"form", JsonValue::object_t{}}, {"url", JsonValue::object_t{}}}}};
}

/// The Streamable HTTP request headers the 2026-07-28 revision requires
/// (SEP-2243). `kHeaderName` accompanies the methods that name an Upstream
/// resource; `kHeaderParamPrefix` carries a tool parameter the Upstream
/// annotated with `x-mcp-header`.
inline constexpr std::string_view kHeaderProtocolVersion{"MCP-Protocol-Version"};
inline constexpr std::string_view kHeaderMethod{"Mcp-Method"};
inline constexpr std::string_view kHeaderName{"Mcp-Name"};
inline constexpr std::string_view kHeaderParamPrefix{"Mcp-Param-"};

/// Marks a base64-encoded header value. A value is sent verbatim only when
/// every one of its bytes is printable US-ASCII and it does not begin with
/// this sentinel; anything else is sent as `base64:<payload>` so the sentinel
/// stays unambiguous for the server.
inline constexpr std::string_view kHeaderValueBase64Sentinel{"base64:"};

/// Tool annotation declaring which parameters are mirrored into `Mcp-Param-*`
/// request headers.
inline constexpr std::string_view kAnnotationHeaderParams{"x-mcp-header"};

/// The Modern Era methods the client stack invokes.
inline constexpr std::string_view kMethodDiscover{"server/discover"};
inline constexpr std::string_view kMethodListTools{"tools/list"};
inline constexpr std::string_view kMethodCallTool{"tools/call"};

/// The Modern Era `tools/call` result discriminators. Anything else is an
/// unrecognized `resultType` and fails exactly one tool call.
inline constexpr std::string_view kResultTypeCallResult{"call_result"};
inline constexpr std::string_view kResultTypeInputRequired{"input_required"};

/// `tools/list_changed`. pike does not subscribe to `subscriptions/listen`,
/// so this notification is safely ignored wherever it arrives: it never
/// reconnects and never triggers a catalog refresh. Catalog freshness is
/// driven by the server-provided `ttlMs` hint instead (ADR 0064, spec #833
/// story 17).
inline constexpr std::string_view kNotificationToolsListChanged{"notifications/tools/list_changed"};

/// The JSON-RPC error an Upstream returns when it needs a client capability
/// this build does not advertise. It fails exactly one tool call.
inline constexpr int kErrorMissingRequiredClientCapability{-32021};

inline constexpr std::string_view kJsonRpcVersion{"2.0"};

/// The declared server capabilities a Modern Era probe may carry. An
/// unrecognized declared capability fails the probe rather than being
/// believed: advertising a capability the host does not implement is exactly
/// the non-conformance the defensive matrix exists to catch.
inline constexpr std::string_view kServerCapabilityTools{"tools"};

/// Defensive containment limits (spec #833 story 24; issue #836). A buggy or
/// malicious Upstream is bounded by these rather than allowed to consume the
/// session; both are measured policy recorded in `docs/runtime-capacities.md`
/// and are not caller-tunable.
inline constexpr std::size_t kMaxToolsPerUpstream{5000};
inline constexpr std::size_t kMaxListPagesPerUpstream{1000};

/// One `tools/call` exchange is bounded twice: it gets the 30 s default and it
/// can never be given more than the 300 s cap, whatever a caller asks for. The
/// cap is containment, not tuning, so it is not configurable (spec #833 story
/// 24; issue #839).
inline constexpr std::chrono::milliseconds kDefaultRequestTimeout{std::chrono::seconds{30}};
inline constexpr std::chrono::milliseconds kMaxRequestTimeout{std::chrono::seconds{300}};

/// The per-Upstream reconnect ladder (spec #833 story 9; issue #839). A failed
/// connection waits `kInitialReconnectBackoff`, doubling per consecutive
/// attempt and never exceeding `kMaxReconnectBackoff`, and gives up after
/// `kMaxConnectAttempts` consecutive transport-level failures rather than
/// retrying forever: a flapping or permanently dead Upstream costs a bounded
/// amount of work — at most ten probes of a dead endpoint — and the ladder
/// restarts only when the owner asks for a connection again.
inline constexpr std::chrono::milliseconds kInitialReconnectBackoff{std::chrono::milliseconds{250}};
inline constexpr std::chrono::milliseconds kMaxReconnectBackoff{std::chrono::seconds{30}};
inline constexpr std::size_t kMaxConnectAttempts{10};

/// The whole of one Upstream connection's cleanup — stopping the in-flight
/// calls, dropping the connection, and cancelling the armed reconnect — is
/// bounded to this. It is the backstop for a transport that ignores
/// cancellation: a connection that cannot be quiesced in time is released
/// anyway, and the operations still outstanding are reported as abandoned
/// (ADR 0011; spec #833 story 10).
inline constexpr std::chrono::milliseconds kConnectionCleanupBound{std::chrono::milliseconds{1000}};

/// The bound on any Upstream-supplied text kept as a diagnostic. Diagnostics
/// are redacted before they are truncated (CODING_STANDARDS.md §10.2).
inline constexpr std::size_t kMaxDiagnosticBytes{1024};

/// The `Content-Type` an Upstream answers a Streamable HTTP POST with when it
/// streams progress and server notifications ahead of the response the request
/// is waiting for. A reply with any other content type is a whole response
/// body, read once.
inline constexpr std::string_view kContentTypeEventStream{"text/event-stream"};

/// The bound on the response bytes one exchange retains, whether they arrive
/// as one JSON body or as the assembled payload of an event stream
/// (spec #833 story 24). It is the flood bound: a response that passes it is
/// terminated rather than drained, so a flooding Upstream cannot hold the
/// connection open or stall another call. Not caller-tunable.
inline constexpr std::size_t kMaxResponseBytes{8 * 1024 * 1024};

} // namespace cch::mcp::protocol
