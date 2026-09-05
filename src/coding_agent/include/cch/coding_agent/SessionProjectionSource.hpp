#pragma once

#include <cch/coding_agent/AgentSessionSnapshot.hpp>

#include <cstdint>
#include <functional>
#include <memory>

namespace cch::coding_agent {

/// Pure read-only projection interface exposed by the Headless Core.
/// Allows presentation layers (TUI, Web, GUI, remote inspector) to sample
/// authoritative session state asynchronously without lock contention or
/// synchronous presentation callbacks (ADR 0051).
class SessionProjectionSource {
public:
    virtual ~SessionProjectionSource() = default;

    /// Monotonically increasing version counter. Incremented on any state
    /// mutation (message chunks, lifecycle transitions, tool events).
    [[nodiscard]] virtual uint64_t state_version() const noexcept = 0;

    /// Returns an immutable, copy-on-write snapshot of the session state.
    /// Thread-safe and lock-free for readers.
    [[nodiscard]] virtual std::shared_ptr<const AgentSessionSnapshot> snapshot() const = 0;

    /// Registers a non-blocking notification listener called when state changes.
    /// The listener MUST NOT render synchronously or call back into the Core with locks held.
    virtual void set_dirty_listener(std::move_only_function<void()> on_dirty) = 0;
};

} // namespace cch::coding_agent
