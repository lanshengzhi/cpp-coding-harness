#pragma once

#include <cch/support/Error.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>

namespace cch::coding_agent {

/// pi `AgentSessionEvent` `auto_retry_start` — emitted before each turn
/// auto-retry backoff sleep (agent-session.ts `_prepareRetry`), so retry is
/// observable exactly like pi.
struct AutoRetryStartEvent {
    /// 1-indexed retry attempt about to run.
    int attempt{0};
    /// pi `settings.retry.maxRetries` (default 3).
    int max_attempts{0};
    /// Backoff delay before this attempt: `baseDelayMs * 2^(attempt-1)`.
    std::int64_t delay_ms{0};
    /// The failed assistant message's error message (pi defaults to
    /// "Unknown error" when absent).
    std::string error_message{};
};

/// pi `AgentSessionEvent` `auto_retry_end` — emitted when retry settles: a
/// non-error assistant message completed (`success: true`), the final retry
/// failed (`success: false` with the final error message), or the backoff
/// sleep was aborted (`success: false` with pi's "Retry cancelled").
struct AutoRetryEndEvent {
    /// Whether the retried run completed with a non-error assistant message.
    bool success{false};
    /// The attempt that settled (0 when no retry ran).
    int attempt{0};
    /// Failure reason: the final assistant error message, or pi's
    /// "Retry cancelled" for an aborted backoff.
    std::optional<std::string> final_error{};
};

/// pi `AgentSessionEvent` `compaction_start` — emitted before a manual or
/// automatic compaction runs, so the Compaction status indicator can show
/// with pi's reason-specific wording.
struct CompactionStartEvent {
    /// pi `reason`: `"manual"` | `"threshold"` | `"overflow"`.
    std::string reason{};
};

/// pi `AgentSessionEvent` `compaction_end` — emitted when compaction
/// settles: the result was applied, or the run was aborted / failed.
struct CompactionEndEvent {
    /// pi `reason`: `"manual"` | `"threshold"` | `"overflow"`.
    std::string reason{};
    /// Whether the compaction was cancelled before applying (pi `aborted`).
    bool aborted{false};
    /// Failure message when the summarization failed (pi `errorMessage`).
    std::optional<std::string> error_message{std::nullopt};
};

/// Session-assembly lifecycle events beyond the Agent lifecycle stream (pi
/// `AgentSessionEvent` subset: `auto_retry_start`/`auto_retry_end` and the
/// compaction start/end pair).
using AgentSessionEvent = std::variant<
    AutoRetryStartEvent,
    AutoRetryEndEvent,
    CompactionStartEvent,
    CompactionEndEvent>;

/// Weak observer for session-assembly events. The runtime owns the registry:
/// a failing observer is deactivated and never vetoes retry progress (the
/// same weak-observer philosophy as Agent subscriptions, ADR 0017).
using AgentSessionEventSink =
    std::move_only_function<support::ExpectedVoid(const AgentSessionEvent&)>;

/// RAII handle for one session-event observer. Destroying the handle or
/// calling unsubscribe() stops event delivery. Idempotent.
class SessionEventSubscription {
public:
    SessionEventSubscription() = default;
    SessionEventSubscription(SessionEventSubscription&&) noexcept;
    SessionEventSubscription& operator=(SessionEventSubscription&&) noexcept;
    ~SessionEventSubscription();
    SessionEventSubscription(const SessionEventSubscription&) = delete;
    SessionEventSubscription& operator=(const SessionEventSubscription&) = delete;

    /// Unsubscribe from further events. Idempotent.
    void unsubscribe();

    /// True while the callback remains registered.
    [[nodiscard]] explicit operator bool() const;

    struct Impl;

private:
    /// The Agent Session implementation owns the observer registry this
    /// handle unregisters from.
    friend class AgentSession;
    std::unique_ptr<Impl> impl_;
};

} // namespace cch::coding_agent
