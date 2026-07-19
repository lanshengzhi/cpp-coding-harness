#include "../../../third_party/catch2/catch_test_macros.hpp"

#include "coding_agent/runtime/SessionEventCommitment.hpp"

#include <deque>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace cch;

namespace runtime = cch::coding_agent::runtime;

namespace {

ai::UserMessage user_msg(std::string text) {
    ai::UserMessage msg;
    msg.content.push_back(ai::TextContent{std::move(text), std::nullopt});
    return msg;
}

class FakeStore final : public harness::session::SessionStore {
public:
    std::vector<std::string>* log{nullptr};
    std::vector<ai::MessageVariant> appended;
    int appends{0};
    int fail_on_append{0}; // 1-based; 0 = never
    util::Error failure = util::make_error(
        util::ErrorCode::Session,
        "could not persist session entry");

    util::ExpectedVoid append(const ai::MessageVariant& message) override {
        ++appends;
        if (log != nullptr) {
            log->push_back("store");
        }
        if (fail_on_append > 0 && appends == fail_on_append) {
            return std::unexpected(failure);
        }
        appended.push_back(message);
        return {};
    }

    std::optional<std::filesystem::path> path() const override { return std::nullopt; }
};

runtime::SubscriberEntry make_subscriber(
    int id,
    std::vector<std::string>& log,
    std::string name) {
    runtime::SubscriberEntry entry;
    entry.id = id;
    entry.sink = [&log, name = std::move(name)](
                     const agent::AgentLifecycleEvent&) mutable -> util::ExpectedVoid {
        log.push_back(name);
        return {};
    };
    return entry;
}

util::Expected<agent::AsyncAgentRunResult> ok_run_result() {
    return agent::AsyncAgentRunResult{};
}

} // namespace

TEST_CASE(
    "Session Event Commitment advances Live Session State first, delivers to "
    "subscribers second, and appends Session Entries last",
    "[coding-agent][runtime][commitment]") {
    std::vector<ai::MessageVariant> history;
    std::deque<runtime::SubscriberEntry> subscribers;
    std::vector<std::string> log;
    FakeStore store;
    store.log = &log;

    subscribers.push_back(make_subscriber(0, log, "sub1"));
    subscribers.push_back(make_subscriber(1, log, "sub2"));

    runtime::SessionEventCommitment commitment{history, subscribers, store};
    auto sink = commitment.sink();

    const auto delivered = sink(agent::MessageEndEvent{user_msg("hello")});
    REQUIRE(delivered.has_value());

    const std::vector<std::string> expected{"sub1", "sub2", "store"};
    CHECK(log == expected);
    REQUIRE(history.size() == 1);
    CHECK(std::holds_alternative<ai::UserMessage>(history.front()));
    REQUIRE(store.appended.size() == 1);

    CHECK(commitment.conclude(ok_run_result()).has_value());
}

TEST_CASE(
    "Session Event Commitment lets subscribers observe the already-advanced "
    "Live Session State",
    "[coding-agent][runtime][commitment]") {
    std::vector<ai::MessageVariant> history;
    std::deque<runtime::SubscriberEntry> subscribers;
    FakeStore store;

    std::size_t observed_history_size{0};
    runtime::SubscriberEntry observer;
    observer.id = 0;
    observer.sink = [&history, &observed_history_size](
                        const agent::AgentLifecycleEvent& event) mutable -> util::ExpectedVoid {
        if (std::holds_alternative<agent::MessageEndEvent>(event)) {
            observed_history_size = history.size();
        }
        return {};
    };
    subscribers.push_back(std::move(observer));

    runtime::SessionEventCommitment commitment{history, subscribers, store};
    auto sink = commitment.sink();

    REQUIRE(sink(agent::MessageEndEvent{user_msg("hello")}).has_value());
    CHECK(observed_history_size == 1);
}

TEST_CASE(
    "Session Event Commitment persists only incrementally persisted Session Entries",
    "[coding-agent][runtime][commitment]") {
    std::vector<ai::MessageVariant> history;
    std::deque<runtime::SubscriberEntry> subscribers;
    FakeStore store;

    runtime::SessionEventCommitment commitment{history, subscribers, store};
    auto sink = commitment.sink();

    // Non-message lifecycle events neither advance state nor persist.
    REQUIRE(sink(agent::AgentStartEvent{}).has_value());
    REQUIRE(sink(agent::MessageStartEvent{user_msg("draft")}).has_value());
    CHECK(history.empty());
    CHECK(store.appended.empty());

    // Bash executions advance Live Session State but are not Session Entries.
    ai::BashExecutionMessage bash;
    bash.command = "ls";
    REQUIRE(sink(agent::MessageEndEvent{std::move(bash)}).has_value());
    REQUIRE(history.size() == 1);
    CHECK(std::holds_alternative<ai::BashExecutionMessage>(history.front()));
    CHECK(store.appended.empty());

    // User, assistant, and tool-result messages persist incrementally.
    REQUIRE(sink(agent::MessageEndEvent{user_msg("hello")}).has_value());
    CHECK(store.appended.size() == 1);
}

TEST_CASE(
    "Session Event Commitment reports the first subscriber failure unwrapped "
    "ahead of the agent-loop error",
    "[coding-agent][runtime][commitment]") {
    std::vector<ai::MessageVariant> history;
    std::deque<runtime::SubscriberEntry> subscribers;
    std::vector<std::string> log;
    FakeStore store;
    store.log = &log;

    runtime::SubscriberEntry rejecting;
    rejecting.id = 0;
    rejecting.sink = [](const agent::AgentLifecycleEvent& event) mutable -> util::ExpectedVoid {
        if (std::holds_alternative<agent::MessageEndEvent>(event)) {
            return std::unexpected(util::make_error(
                util::ErrorCode::Unknown,
                "synthetic subscriber failure"));
        }
        return {};
    };
    subscribers.push_back(std::move(rejecting));
    subscribers.push_back(make_subscriber(1, log, "later"));

    runtime::SessionEventCommitment commitment{history, subscribers, store};
    auto sink = commitment.sink();

    const auto delivered = sink(agent::MessageEndEvent{user_msg("hello")});
    REQUIRE_FALSE(delivered.has_value());
    CHECK(delivered.error().message == "synthetic subscriber failure");

    // Delivery stops at the failing subscriber; persistence is not attempted.
    // Live Session State still advanced.
    CHECK(log.empty());
    CHECK(store.appended.empty());
    CHECK(history.size() == 1);

    // The loop's wrapped view of the same failure is discarded.
    util::Expected<agent::AsyncAgentRunResult> loop_result = std::unexpected(
        util::make_error(
            util::ErrorCode::Unknown,
            "agent event sink failed",
            "synthetic subscriber failure"));
    const auto verdict = commitment.conclude(std::move(loop_result));
    REQUIRE_FALSE(verdict.has_value());
    CHECK(verdict.error().message == "synthetic subscriber failure");
    CHECK(verdict.error().detail.empty());
}

TEST_CASE(
    "Session Event Commitment reports a persistence failure unwrapped after "
    "successful delivery",
    "[coding-agent][runtime][commitment]") {
    std::vector<ai::MessageVariant> history;
    std::deque<runtime::SubscriberEntry> subscribers;
    std::vector<std::string> log;
    FakeStore store;
    store.log = &log;
    store.fail_on_append = 1;

    subscribers.push_back(make_subscriber(0, log, "sub1"));

    runtime::SessionEventCommitment commitment{history, subscribers, store};
    auto sink = commitment.sink();

    const auto delivered = sink(agent::MessageEndEvent{user_msg("hello")});
    REQUIRE_FALSE(delivered.has_value());
    CHECK(delivered.error().code == util::ErrorCode::Session);
    CHECK(delivered.error().message == "could not persist session entry");

    const std::vector<std::string> expected{"sub1", "store"};
    CHECK(log == expected);
    CHECK(history.size() == 1);

    const auto verdict = commitment.conclude(ok_run_result());
    REQUIRE_FALSE(verdict.has_value());
    CHECK(verdict.error().code == util::ErrorCode::Session);
    CHECK(verdict.error().message == "could not persist session entry");
}

TEST_CASE(
    "Session Event Commitment conclude reports an unfinished loop",
    "[coding-agent][runtime][commitment]") {
    std::vector<ai::MessageVariant> history;
    std::deque<runtime::SubscriberEntry> subscribers;
    FakeStore store;

    runtime::SessionEventCommitment commitment{history, subscribers, store};
    const auto verdict = commitment.conclude(std::nullopt);
    REQUIRE_FALSE(verdict.has_value());
    CHECK(verdict.error().code == util::ErrorCode::Unknown);
    CHECK(verdict.error().message == "async loop did not finish");
}

TEST_CASE(
    "Session Event Commitment conclude passes an agent-loop error through "
    "when no commitment failed",
    "[coding-agent][runtime][commitment]") {
    std::vector<ai::MessageVariant> history;
    std::deque<runtime::SubscriberEntry> subscribers;
    FakeStore store;

    runtime::SessionEventCommitment commitment{history, subscribers, store};
    util::Expected<agent::AsyncAgentRunResult> loop_result = std::unexpected(
        util::make_error(util::ErrorCode::Provider, "provider exploded"));
    const auto verdict = commitment.conclude(std::move(loop_result));
    REQUIRE_FALSE(verdict.has_value());
    CHECK(verdict.error().code == util::ErrorCode::Provider);
    CHECK(verdict.error().message == "provider exploded");
}

TEST_CASE(
    "Session Event Commitment conclude succeeds for a finished loop without "
    "failures",
    "[coding-agent][runtime][commitment]") {
    std::vector<ai::MessageVariant> history;
    std::deque<runtime::SubscriberEntry> subscribers;
    FakeStore store;

    runtime::SessionEventCommitment commitment{history, subscribers, store};
    CHECK(commitment.conclude(ok_run_result()).has_value());
}

TEST_CASE(
    "Session Event Commitment delivers to the run-start snapshot for the "
    "whole run",
    "[coding-agent][runtime][commitment]") {
    std::vector<ai::MessageVariant> history;
    std::deque<runtime::SubscriberEntry> subscribers;
    std::vector<std::string> log;
    FakeStore store;

    subscribers.push_back(make_subscriber(0, log, "early"));
    subscribers.push_back(make_subscriber(1, log, "inactive"));
    subscribers.back().active = false;
    subscribers.push_back(make_subscriber(2, log, "late"));

    runtime::SessionEventCommitment commitment{history, subscribers, store};
    auto sink = commitment.sink();

    // Mid-run changes must not alter this run's delivery: an unsubscribe
    // keeps receiving, a re-activated entry does not join, and a new
    // subscription (which may reallocate a vector-backed registry) neither
    // joins nor invalidates the snapshot.
    subscribers[0].active = false;
    subscribers[1].active = true;
    subscribers.push_back(make_subscriber(3, log, "newcomer"));

    REQUIRE(sink(agent::MessageEndEvent{user_msg("hello")}).has_value());
    const std::vector<std::string> expected{"early", "late"};
    CHECK(log == expected);
}
