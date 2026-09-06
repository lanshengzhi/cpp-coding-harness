#pragma once

#include <cch/coding_agent/AgentSessionSnapshot.hpp>

#include <cstdint>
#include <functional>
#include <memory>

namespace cch::coding_agent {

/// Pure read-only projection interface exposed by the Headless Core (ADR 0051).
///
/// The #601 ticker consumes this contract as follows:
/// 1. Sample `state_version()` at the start of a frame.
/// 2. If it differs from the last rendered version, sample `snapshot()` once,
///    compose one frame from that immutable value, and record the sampled
///    version. A dirty notification may schedule a coalesced immediate
///    preview (drained by ready handlers); the ticker frame stays the
///    counted authoritative render.
/// 3. On the next frame, repeat the version comparison; any number of Core
///    updates between ticks therefore coalesces into one frame.
///
/// The Core publishes a versioned O(1) dirty edge on its serialized execution
/// domain. `snapshot()` materializes the public deep value only when a sampled
/// version is not already cached. Its returned object is immutable and safe to
/// retain and read without a Core mutex. Sampling itself must occur on the Core
/// serialized execution domain; if an update races a sampling call, the ticker
/// observes the newer version on its next frame.
class SessionProjectionSource {
public:
    virtual ~SessionProjectionSource() = default;

    /// Monotonically increasing version counter. Incremented once after each
    /// published state mutation: Agent lifecycle events (message chunks,
    /// lifecycle transitions, tool events), direct Session state changes, and
    /// session-event observer-failure diagnostics (the one session-assembly
    /// mutation with a published value). Pure notification events with no
    /// published value (retry countdowns, compaction indicators) do not bump
    /// the version. Reads are atomic and never acquire a Core mutex.

    [[nodiscard]] virtual std::uint64_t state_version() const noexcept = 0;

    /// Returns the immutable value for the latest sampled publication. The
    /// first sample after a version change may materialize the complete value;
    /// repeated samples at that version return the same shared instance.
    [[nodiscard]] virtual std::shared_ptr<const AgentSessionSnapshot> snapshot() const = 0;

    /// Replaces the single dirty-edge listener. It is called synchronously
    /// after the version increment, on the Core's serialized domain, without
    /// a Core mutex held. The listener must be non-blocking and must not
    /// render or mutate the Core; it should only set a ticker flag or post to
    /// the consumer's executor. Throwing listeners are deactivated.
    virtual void set_dirty_listener(std::move_only_function<void()> on_dirty) = 0;
};

} // namespace cch::coding_agent
