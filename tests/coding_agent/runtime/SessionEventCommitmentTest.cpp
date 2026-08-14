#include "coding_agent/runtime/SessionEventCommitment.hpp"

#include "coding_agent/runtime/SessionPersistence.hpp"

#include <cch/agent/harness/session/JsonlSessionStore.hpp>
#include "harness/RuntimeRoot.hpp"
#include "harness/session/SessionJournalTestHooks.hpp"
#include "support/TempWorkspace.hpp"

#include <catch2/catch_test_macros.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

using namespace cch;
namespace runtime = cch::coding_agent::runtime;

namespace {

ai::UserMessage user_msg(std::string text) {
    return ai::user_text_message(std::move(text));
}

/// Pump one loop until `done` becomes true (or the budget expires). Used
/// instead of `io.run()` because a live RuntimeRoot holds a work guard, so
/// `run()` would not return while the root is alive.
[[nodiscard]] bool pump_until(
    boost::asio::io_context& io,
    const std::atomic<bool>& done,
    std::chrono::milliseconds budget = std::chrono::milliseconds{2000}) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (!done.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        if (io.stopped()) {
            io.restart();
        }
        (void)io.poll();
        std::this_thread::sleep_for(std::chrono::microseconds{100});
    }
    return done.load(std::memory_order_acquire);
}

template <typename T>
[[nodiscard]] T run_awaitable(
    boost::asio::io_context& io,
    boost::asio::awaitable<T> operation) {
    std::optional<T> result;
    std::exception_ptr exception;
    std::atomic<bool> done{false};
    boost::asio::co_spawn(
        io,
        std::move(operation),
        [&](std::exception_ptr completion_exception, T value) {
            exception = completion_exception;
            result.emplace(std::move(value));
            done.store(true, std::memory_order_release);
        });
    REQUIRE(pump_until(io, done));
    REQUIRE(exception == nullptr);
    return std::move(*result);
}

/// A real commitment channel: JSONL store in a TempWorkspace behind the
/// closed facade, admitted through a real RuntimeRoot mailbox.
struct CommitmentChannel {
    tests::TempWorkspace workspace;
    std::filesystem::path session_path = workspace.path() / "commitment.jsonl";
    std::shared_ptr<boost::asio::io_context> io =
        std::make_shared<boost::asio::io_context>();
    std::optional<harness::RuntimeRoot> root;
    std::shared_ptr<harness::RuntimeTarget> target;
    std::shared_ptr<harness::session::SessionStore> store;
    std::shared_ptr<runtime::SessionPersistence> persistence;

    explicit CommitmentChannel(std::size_t max_admitted_operations = 64) {
        harness::session::SessionMetadata metadata{
            .session_id = "commitment-test",
            .created_at = "2026-07-05T00:00:00Z",
            .workspace = workspace.path(),
            .provider = "fake",
            .model = "fake-model",
        };
        auto jsonl =
            harness::session::JsonlSessionStore::create_new(session_path, metadata);
        REQUIRE(jsonl);
        store =
            std::make_shared<harness::session::SessionStore>(std::move(*jsonl));
        root.emplace(io, 2, max_admitted_operations, 16 * 1024 * 1024);
        target = root->make_target();
        persistence =
            std::make_shared<runtime::SessionPersistence>(store, target);
    }

    CommitmentChannel(const CommitmentChannel&) = delete;
    CommitmentChannel& operator=(const CommitmentChannel&) = delete;

    ~CommitmentChannel() {
        persistence.reset();
        if (root) {
            root->close();
            // Keep pumping the shared loop so every admitted terminal reaches
            // the target mailbox before the root and loop are destroyed.
            while (io->poll() != 0) {
            }
        }
    }

    [[nodiscard]] util::ExpectedVoid conclude(
        runtime::SessionEventCommitment& commitment,
        std::optional<util::ExpectedVoid> agent_result) {
        return run_awaitable(*io, commitment.conclude(std::move(agent_result)));
    }

    [[nodiscard]] std::vector<std::string> persisted_user_texts() const {
        auto loaded = harness::session::JsonlSessionStore::load(session_path);
        REQUIRE(loaded);
        std::vector<std::string> texts;
        for (const auto& message : loaded->messages) {
            if (const auto* user = std::get_if<ai::UserMessage>(&message)) {
                texts.push_back(ai::text_from_user_message(*user));
            }
        }
        return texts;
    }
};

} // namespace

TEST_CASE(
    "Session Event Commitment persists only completed Session Entry messages",
    "[coding_agent][runtime][commitment][issue36]") {
    CommitmentChannel channel;
    runtime::SessionEventCommitment commitment{channel.persistence};
    auto sink = commitment.sink();

    REQUIRE(sink(agent::AgentStartEvent{}).has_value());
    REQUIRE(sink(agent::MessageStartEvent{user_msg("draft")}).has_value());

    ai::BashExecutionMessage bash;
    bash.command = "ls";
    REQUIRE(sink(agent::MessageEndEvent{std::move(bash)}).has_value());

    REQUIRE(sink(agent::MessageEndEvent{user_msg("hello")}).has_value());
    CHECK(channel.conclude(commitment, util::ExpectedVoid{}).has_value());
    CHECK(channel.persisted_user_texts() == std::vector<std::string>{"hello"});
}

TEST_CASE(
    "Session Event Commitment returns the first persistence failure unwrapped after drain",
    "[coding_agent][runtime][commitment][issue36]") {
    CommitmentChannel channel;
    runtime::SessionEventCommitment commitment{channel.persistence};
    auto sink = commitment.sink();

    // The append executes off the loop: the admission verdict is success and
    // the storage failure surfaces at conclude, after the channel drains.
    harness::session::testing::fail_nth_append_for_test(channel.session_path, 1);
    REQUIRE(sink(agent::MessageEndEvent{user_msg("one")}).has_value());

    util::ExpectedVoid wrapped = std::unexpected(util::make_error(
        util::ErrorCode::Unknown,
        "agent event commitment failed"));
    auto verdict = channel.conclude(commitment, std::move(wrapped));
    REQUIRE_FALSE(verdict.has_value());
    CHECK(verdict.error().code == util::ErrorCode::Session);
    CHECK(verdict.error().message == "could not persist session entry");
    CHECK(channel.persisted_user_texts().empty());
}

TEST_CASE(
    "Session Event Commitment fails fast once a persistence failure is latched",
    "[coding_agent][runtime][commitment][issue36]") {
    CommitmentChannel channel;
    runtime::SessionEventCommitment commitment{channel.persistence};
    auto sink = commitment.sink();

    harness::session::testing::fail_nth_append_for_test(channel.session_path, 1);
    REQUIRE(sink(agent::MessageEndEvent{user_msg("one")}).has_value());
    auto verdict = channel.conclude(commitment, util::ExpectedVoid{});
    REQUIRE_FALSE(verdict.has_value());

    // The recorded failure is session-scoped sticky state: later admissions
    // fail fast with the same typed failure.
    auto rejected = sink(agent::MessageEndEvent{user_msg("two")});
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error().code == util::ErrorCode::Session);
    CHECK(rejected.error().message == "could not persist session entry");
}

TEST_CASE(
    "Session Event Commitment reports an unfinished Agent prompt",
    "[coding_agent][runtime][commitment][issue36]") {
    CommitmentChannel channel;
    runtime::SessionEventCommitment commitment{channel.persistence};

    auto verdict = channel.conclude(commitment, std::nullopt);
    REQUIRE_FALSE(verdict.has_value());
    CHECK(verdict.error().code == util::ErrorCode::Unknown);
    CHECK(verdict.error().message == "stateful Agent prompt did not finish");
}

TEST_CASE(
    "Session Event Commitment passes through Agent failures when storage succeeded",
    "[coding_agent][runtime][commitment][issue36]") {
    CommitmentChannel channel;
    runtime::SessionEventCommitment commitment{channel.persistence};
    auto sink = commitment.sink();
    REQUIRE(sink(agent::MessageEndEvent{user_msg("hello")}).has_value());

    util::ExpectedVoid failed = std::unexpected(util::make_error(
        util::ErrorCode::Provider,
        "provider exploded"));
    auto verdict = channel.conclude(commitment, std::move(failed));
    REQUIRE_FALSE(verdict.has_value());
    CHECK(verdict.error().code == util::ErrorCode::Provider);
    CHECK(verdict.error().message == "provider exploded");
}

TEST_CASE(
    "Session Event Commitment persists admitted messages in FIFO order",
    "[coding_agent][runtime][commitment][issue464]") {
    CommitmentChannel channel;
    runtime::SessionEventCommitment commitment{channel.persistence};
    auto sink = commitment.sink();

    REQUIRE(sink(agent::MessageEndEvent{user_msg("one")}).has_value());
    REQUIRE(sink(agent::MessageEndEvent{user_msg("two")}).has_value());
    REQUIRE(sink(agent::MessageEndEvent{user_msg("three")}).has_value());
    CHECK(channel.conclude(commitment, util::ExpectedVoid{}).has_value());

    CHECK(
        channel.persisted_user_texts() ==
        std::vector<std::string>{"one", "two", "three"});
}

TEST_CASE(
    "Session Event Commitment vetoes the run with a typed Busy when admission is saturated",
    "[coding_agent][runtime][commitment][issue464]") {
    CommitmentChannel channel{/*max_admitted_operations=*/0};
    runtime::SessionEventCommitment commitment{channel.persistence};
    auto sink = commitment.sink();

    auto verdict = sink(agent::MessageEndEvent{user_msg("hello")});
    REQUIRE_FALSE(verdict.has_value());
    CHECK(verdict.error().code == util::ErrorCode::Busy);

    // The saturation is latched like any persistence failure.
    auto concluded = channel.conclude(commitment, util::ExpectedVoid{});
    REQUIRE_FALSE(concluded.has_value());
    CHECK(concluded.error().code == util::ErrorCode::Busy);
    CHECK(channel.persisted_user_texts().empty());
}

TEST_CASE(
    "Session Event Commitment without a persistence channel is a successful no-op",
    "[coding_agent][runtime][commitment][issue464]") {
    CommitmentChannel channel;
    runtime::SessionEventCommitment commitment{nullptr};
    auto sink = commitment.sink();

    REQUIRE(sink(agent::MessageEndEvent{user_msg("hello")}).has_value());
    CHECK(channel.conclude(commitment, util::ExpectedVoid{}).has_value());
    CHECK(channel.persisted_user_texts().empty());
}
