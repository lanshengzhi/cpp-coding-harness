#pragma once

#include "../../../include/cch/agent/AgentEvent.hpp"
#include "../../../include/cch/harness/session/SessionStore.hpp"
#include "../../../include/cch/util/Error.hpp"

#include <optional>

namespace cch::coding_agent::runtime {

/// Strong persistence participant for one Agent run.
///
/// The stateful Agent advances Live Session State and delivers weak observers
/// before invoking this commitment. Completed persistable messages are then
/// appended as Session Entries. The first storage failure stops the run and is
/// reported unwrapped by conclude().
class SessionEventCommitment final {
public:
    explicit SessionEventCommitment(harness::session::SessionStore& store);

    SessionEventCommitment(const SessionEventCommitment&) = delete;
    SessionEventCommitment& operator=(const SessionEventCommitment&) = delete;

    /// The strong per-run callback handed to the stateful Agent. It borrows
    /// this commitment and must not outlive it.
    [[nodiscard]] agent::AgentEventCommitter sink();

    /// The run's single final verdict: a recorded persistence failure first,
    /// then an unfinished Agent prompt, then the Agent prompt result.
    [[nodiscard]] util::ExpectedVoid conclude(
        std::optional<util::ExpectedVoid> agent_result) const;

private:
    harness::session::SessionStore& store_;
    std::optional<util::Error> failure_;
};

} // namespace cch::coding_agent::runtime
