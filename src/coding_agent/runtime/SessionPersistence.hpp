#pragma once

#include <cch/ai/Message.hpp>
#include <cch/support/Error.hpp>

#include <boost/asio/awaitable.hpp>

#include <memory>
#include <optional>

namespace cch::harness {
class RuntimeTarget;
} // namespace cch::harness

namespace cch::harness::session {
class SessionStore;
} // namespace cch::harness::session

namespace cch::coding_agent::runtime {

/// Session-scoped Session Event Commitment channel (ADR 0040 §Session Event
/// Commitment and Close): encoding and persistence execute off the Runtime
/// loop on admitted workers, and every outcome returns to the Session's
/// serialized mailbox in FIFO admission order.
///
/// Strictly one append is in flight per session; further submissions queue
/// behind it (each holding its mailbox admission) so the store's entry order
/// always matches the live-state order the events were observed in. The
/// first failure is session-scoped sticky state: later submissions fail fast
/// and the runtime rejects later prompts with the recorded typed failure.
///
/// Submissions run on the Session's driving executor while mailbox delivery
/// runs on the RuntimeTarget's loop; all mutable state sits behind one small
/// mutex. Every admitted operation completes exactly once, so the mailbox
/// sequence can never stall.
class SessionPersistence final {
public:
    SessionPersistence(
        std::shared_ptr<harness::session::SessionStore> store,
        std::shared_ptr<harness::RuntimeTarget> target);
    ~SessionPersistence();
    SessionPersistence(const SessionPersistence&) = delete;
    SessionPersistence& operator=(const SessionPersistence&) = delete;
    SessionPersistence(SessionPersistence&&) = delete;
    SessionPersistence& operator=(SessionPersistence&&) = delete;

    /// Synchronous admission from the Session's driving executor (the
    /// Agent's strong committer sink). Returns the latched failure when one
    /// exists; a typed Busy when the mailbox is saturated (latching it);
    /// otherwise success: the append is admitted and its outcome returns
    /// through the Session's serialized mailbox in FIFO order.
    [[nodiscard]] support::ExpectedVoid submit_message_append(
        ai::MessageVariant message);

    /// Await until every admitted append settled (the chain is idle). The
    /// recorded first failure is observed separately through failure().
    [[nodiscard]] boost::asio::awaitable<void> drain();

    /// The session-scoped sticky first persistence failure, if any.
    [[nodiscard]] std::optional<support::Error> failure() const;

private:
    struct State;
    struct PendingOp;

    /// Post one pending operation to a Runtime worker (requires the state
    /// lock). The admission always completes exactly once.
    static void start_operation_locked(
        const std::shared_ptr<State>& state,
        PendingOp op) noexcept;
    /// Mailbox delivery on the RuntimeTarget's loop: latch the first
    /// failure, start the next queued append, or release drain waiters.
    static void deliver_result(
        const std::shared_ptr<State>& state,
        support::ExpectedVoid outcome) noexcept;

    std::shared_ptr<State> state_;
};

} // namespace cch::coding_agent::runtime
