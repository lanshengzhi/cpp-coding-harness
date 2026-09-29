#pragma once

#include <cch/support/AsyncResult.hpp>
#include <cch/support/Error.hpp>

#include <chrono>
#include <functional>
#include <stop_token>

namespace cch::mcp {

/// One delay on the caller's own execution domain, supplied by the owner of
/// that domain: `cch_mcp` owns no event loop, and timer state stays local to
/// the operation that needs it (ADR 0040). The operation completes when the
/// delay elapses and fails with `Cancelled` when `stop_token` fires first.
///
/// One seam, two uses: the per-Upstream reconnect ladder and connection
/// cleanup bound (issue #839), and the Pending Elicitation wait bound
/// (issue #845). Both are "wait, but not forever, and stop on request", so
/// they share the one spelling rather than growing a second timer port.
using UpstreamDelay = std::move_only_function<cch::support::AsyncResult<void>(
        std::chrono::milliseconds delay, std::stop_token stop_token)>;

} // namespace cch::mcp
