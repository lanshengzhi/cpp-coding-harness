#pragma once

#include <cch/mcp/UpstreamTool.hpp>
#include <cch/support/JsonValue.hpp>

#include <string>

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

} // namespace cch::mcp
