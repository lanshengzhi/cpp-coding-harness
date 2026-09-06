#pragma once

#include <cch/coding_agent/AgentSessionSnapshot.hpp>

#include <cstdint>
#include <functional>
#include <memory>

namespace cch::coding_agent {

/// Pure read-only projection interface exposed by the Headless Core.
/// Presentation layers sample this source from an independent frame ticker:
/// `state_version()` is the cheap change check, and `snapshot()` is sampled
/// only when that version differs from the last rendered version. Core event
/// ingestion publishes only an O(1) version/dirty edge; the immutable deep
/// value is materialized on demand by `snapshot()` and reused until the next
/// version. A ticker may therefore coalesce any number of dirty notifications
/// into one render pass (issue #600/#601).
class SessionProjectionSource {
public:
    virtual ~SessionProjectionSource() = default;

    /// Monotonically increasing version counter. Incremented on any state
    /// mutation (message chunks, lifecycle transitions, tool events). Reads
    /// are atomic and never acquire a Core mutex.
    [[nodiscard]] virtual std::uint64_t state_version() const noexcept = 0;

    /// Returns the immutable snapshot for the latest sampled version. The
    /// first sample after a version change may materialize the complete value;
    /// repeated samples at that version return the same shared instance.
    [[nodiscard]] virtual std::shared_ptr<const AgentSessionSnapshot> snapshot() const = 0;

    /// Registers a non-blocking notification listener called after a state
    /// version changes. The callback is a dirty edge for a frame ticker, not a
    /// render request; it MUST NOT render synchronously or call back into the
    /// Core with locks held.
    virtual void set_dirty_listener(std::move_only_function<void()> on_dirty) = 0;
};

} // namespace cch::coding_agent
