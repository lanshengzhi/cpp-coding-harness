#pragma once

#include <cch/mcp/UpstreamTool.hpp>
#include <cch/support/JsonValue.hpp>

#include <functional>
#include <optional>
#include <string>
#include <utility>

namespace cch::mcp {

/// One `tools/call` the MCP Host issues. The call carries the whole
/// descriptor rather than a bare name so the client stack can mirror the
/// tool's validated `x-mcp-header` parameters into request headers without
/// re-reading the catalog; the caller registers only descriptors a
/// `tools/list` result actually yielded.
struct UpstreamToolCall {
    UpstreamToolDescriptor tool{};
    support::JsonValue arguments{};
};

/// One `tools/call` outcome.
///
/// Every protocol violation by the Upstream — a JSON-RPC error response, an
/// unrecognized `resultType`, an unexpected Multi Round-Trip `input_required`
/// result, a malformed result DTO — arrives here as one failed call rather
/// than as a client-stack error, so a buggy or hostile Upstream degrades to a
/// single failed tool call and the next ordinary call still succeeds (ADR
/// 0008, spec #833 stories 23 and 32). A transport failure stays an
/// operation error so cancellation and network loss remain distinguishable.
struct UpstreamToolCallResult {
    bool is_error{false};
    /// The server's `content` for a completed call, preserved verbatim so the
    /// generic tool renderer receives it unchanged.
    support::JsonValue content{};
    /// Bounded, redacted explanation of why the call failed; empty on success
    /// and on a call the Upstream itself reported as failed.
    std::string diagnostic{};
};

/// One `notifications/progress` notification an Upstream sent for a call in
/// flight (spec #833 story 31). It is a passive value with no Upstream
/// identity in it: a notification that names no progress token this connection
/// minted is dropped before it becomes one of these, so a progress line can
/// never belong to a call other than the one it arrived on.
struct UpstreamToolProgress {
    /// The Upstream's own counter. The revision requires it; a value that is
    /// not a finite number is dropped with the notification.
    double progress{0.0};
    /// The expected total, when the Upstream declared one. Absent means the
    /// operation is not countable, not that it is at zero.
    std::optional<double> total{std::nullopt};
    /// Optional human-readable status, already bounded and redacted by the
    /// package. Empty when the Upstream sent none.
    std::string message{};
};

/// The one progress sink. It is a weak observer of the call it belongs to: it
/// runs on the operation's own domain while that call's exchange is being
/// read, it is never called concurrently with itself, and it may only record
/// or forward the value (ADR 0040 §Connection strength) — the same contract
/// `UpstreamStatusSink` has. It is called only for notifications that name the
/// calling call's own progress token, and never after that call has settled.
using UpstreamProgressSink = std::move_only_function<void(const UpstreamToolProgress&)>;

} // namespace cch::mcp
