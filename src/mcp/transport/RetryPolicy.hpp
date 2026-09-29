#pragma once

#include <cch/support/Error.hpp>

#include <string_view>

namespace cch::mcp::transport {

/// Why one exchange failed, as the MCP Host's retry policy classifies it.
///
/// The 2026-07-28 revision removed `Mcp-Session-Id` and SSE resumability, so
/// there is nothing to resume and nothing to dedupe against: a retry is a
/// brand-new exchange with a brand-new JSON-RPC id. The policy is therefore
/// stated per failure class rather than left to a transparent replay, and the
/// only class the transport re-attempts on its own is the one where the
/// Upstream provably never saw the request (ADR 0064, spec #833 "Wire and
/// transport").
enum class McpFailureClass {
    /// The request was refused before a byte of it was written, so no Upstream
    /// can have observed it and re-attempting it cannot duplicate a
    /// `tools/call`. The one class the transport retries.
    RequestNotDelivered,
    /// The request was written and the Upstream may have acted on it. A replay
    /// could duplicate a side-effecting `tools/call`, so the in-flight request
    /// fails and a retry is the caller's new exchange with a new id.
    RequestDelivered,
    /// The response passed the retention bound and was terminated. Re-sending
    /// it invites the same flood.
    ResponseFlooded,
    /// The caller cancelled the call. Never retried: cancellation is the
    /// caller's decision, and repeating it would spend the caller's budget
    /// against their intent.
    Cancelled,
    /// The transport refused to send the request at all, for example an
    /// endpoint that is not `https://`. Deterministic, so a retry fails the
    /// same way.
    Rejected,
};

/// The declared per-class policy. Only `RequestNotDelivered` is retried, and
/// only within one exchange's attempt budget; every other class ends the
/// exchange. `RequestDelivered` is the class that matters most: a broken
/// response stream and a response-stream flood both land there, so a lost
/// in-flight request is never replayed behind the caller's back.
///
/// An Upstream refusal is not here because it is not a transport failure: the
/// seam returns a non-success status to the client stack, which reports it as
/// a protocol failure, and re-sending the same request would only ask the same
/// question twice.
[[nodiscard]] bool is_retryable(McpFailureClass failure_class) noexcept;

/// The class one attempt's terminal failure belongs to. `request_delivered` is
/// the conservative record the transport keeps: it becomes true as soon as the
/// transport starts writing the request, because a partial write may still have
/// reached the Upstream.
[[nodiscard]] McpFailureClass classify_failure(cch::support::ErrorCode code, bool request_delivered) noexcept;

[[nodiscard]] std::string_view describe(McpFailureClass failure_class) noexcept;

} // namespace cch::mcp::transport
