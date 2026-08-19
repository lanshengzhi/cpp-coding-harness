#pragma once

#include <cch/agent/AgentEvent.hpp>
#include <cch/support/Error.hpp>

#include <boost/asio/awaitable.hpp>

#include <memory>
#include <optional>

namespace cch::harness::session {
class SessionStore;
} // namespace cch::harness::session

namespace cch::coding_agent::runtime {

class SessionPersistence;

/// Strong persistence participant for one Agent run.
///
/// The stateful Agent advances Live Session State and delivers weak observers
/// before invoking this commitment. Persistable completed messages are then
/// admitted to the session's off-loop persistence channel; their outcomes
/// return through the Session's serialized mailbox in FIFO order (ADR 0040
/// §Session Event Commitment and Close). An admission failure vetoes the run
/// through the returned verdict; the first recorded persistence failure is
/// reported unwrapped by conclude() after the channel drains. In-memory
/// sessions have no channel: the sink commits the same message kinds straight
/// into the store's live tree (bounded in-memory work, ADR 0040), exactly
/// like pi's non-persisting SessionManager, which keeps the same in-memory
/// entries.
class SessionEventCommitment final {
public:
    /// A null persistence with a null (or persisted) store keeps the sink a
    /// successful no-op; a null persistence with an in-memory store commits
    /// directly into the store's live tree.
    explicit SessionEventCommitment(
        std::shared_ptr<SessionPersistence> persistence,
        std::shared_ptr<harness::session::SessionStore> store = nullptr);

    SessionEventCommitment(const SessionEventCommitment&) = delete;
    SessionEventCommitment& operator=(const SessionEventCommitment&) = delete;

    /// The strong per-run callback handed to the stateful Agent. It borrows
    /// this commitment and must not outlive it. The returned verdict is the
    /// admission result: a latched persistence failure or a saturated mailbox
    /// veto the run.
    [[nodiscard]] agent::AgentEventCommitter sink();

    /// The run's single final verdict, after the persistence channel has
    /// drained: a recorded persistence failure first, then an unfinished
    /// Agent prompt, then the Agent prompt result.
    [[nodiscard]] boost::asio::awaitable<support::ExpectedVoid> conclude(
        std::optional<support::ExpectedVoid> agent_result);

private:
    std::shared_ptr<SessionPersistence> persistence_;
    /// The in-memory session's store (null or persisted-path stores keep the
    /// no-op sink); appends land in its live tree without disk I/O.
    std::shared_ptr<harness::session::SessionStore> store_;
};

} // namespace cch::coding_agent::runtime
