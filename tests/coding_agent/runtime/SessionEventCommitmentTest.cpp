#include "coding_agent/runtime/SessionEventCommitment.hpp"

#include "coding_agent/runtime/SessionPersistence.hpp"

#include <cch/agent/harness/session/SessionStore.hpp>
#include "agent/harness/RuntimeRoot.hpp"
#include "agent/harness/session/SessionJournalTestHooks.hpp"
#include "support/PumpUntil.hpp"
#include "support/TempWorkspace.hpp"

#include <catch2/catch_test_macros.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
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

using tests::pump_until;

ai::UserMessage user_msg(std::string text) {
    return ai::user_text_message(std::move(text));
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

    explicit CommitmentChannel(harness::RuntimeLimits limits = {}) {
        harness::session::SessionMetadata metadata{
            .session_id = "commitment-test",
            .created_at = "2026-07-05T00:00:00Z",
            .workspace = workspace.path(),
            .provider = "fake",
            .model = "fake-model",
        };
        auto jsonl =
            harness::session::SessionStore::create_new(session_path, metadata);
        REQUIRE(jsonl);
        store =
            std::make_shared<harness::session::SessionStore>(std::move(*jsonl));
        root.emplace(io, limits);
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

    [[nodiscard]] support::ExpectedVoid conclude(
        runtime::SessionEventCommitment& commitment,
        std::optional<support::ExpectedVoid> agent_result) {
        return run_awaitable(*io, commitment.conclude(std::move(agent_result)));
    }

    [[nodiscard]] std::vector<std::string> persisted_user_texts() const {
        auto loaded = harness::session::SessionStore::load(session_path);
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
    CHECK(channel.conclude(commitment, support::ExpectedVoid{}).has_value());
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

    support::ExpectedVoid wrapped = std::unexpected(support::make_error(
        support::ErrorCode::Unknown,
        "agent event commitment failed"));
    auto verdict = channel.conclude(commitment, std::move(wrapped));
    REQUIRE_FALSE(verdict.has_value());
    CHECK(verdict.error().code == support::ErrorCode::Session);
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
    auto verdict = channel.conclude(commitment, support::ExpectedVoid{});
    REQUIRE_FALSE(verdict.has_value());

    // The recorded failure is session-scoped sticky state: later admissions
    // fail fast with the same typed failure.
    auto rejected = sink(agent::MessageEndEvent{user_msg("two")});
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error().code == support::ErrorCode::Session);
    CHECK(rejected.error().message == "could not persist session entry");
}

TEST_CASE(
    "Session Event Commitment reports an unfinished Agent prompt",
    "[coding_agent][runtime][commitment][issue36]") {
    CommitmentChannel channel;
    runtime::SessionEventCommitment commitment{channel.persistence};

    auto verdict = channel.conclude(commitment, std::nullopt);
    REQUIRE_FALSE(verdict.has_value());
    CHECK(verdict.error().code == support::ErrorCode::Unknown);
    CHECK(verdict.error().message == "stateful Agent prompt did not finish");
}

TEST_CASE(
    "Session Event Commitment passes through Agent failures when storage succeeded",
    "[coding_agent][runtime][commitment][issue36]") {
    CommitmentChannel channel;
    runtime::SessionEventCommitment commitment{channel.persistence};
    auto sink = commitment.sink();
    REQUIRE(sink(agent::MessageEndEvent{user_msg("hello")}).has_value());

    support::ExpectedVoid failed = std::unexpected(support::make_error(
        support::ErrorCode::Provider,
        "provider exploded"));
    auto verdict = channel.conclude(commitment, std::move(failed));
    REQUIRE_FALSE(verdict.has_value());
    CHECK(verdict.error().code == support::ErrorCode::Provider);
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
    CHECK(channel.conclude(commitment, support::ExpectedVoid{}).has_value());

    CHECK(
        channel.persisted_user_texts() ==
        std::vector<std::string>{"one", "two", "three"});
}

TEST_CASE(
    "Session Event Commitment vetoes the run with a typed Busy when admission is saturated",
    "[coding_agent][runtime][commitment][issue464]") {
    // Both the ordinary and reserved budgets are exhausted: control work has
    // no lane left, so the commitment is rejected with a typed Busy.
    CommitmentChannel channel{harness::RuntimeLimits{
        .worker_count = 2,
        .max_admitted_operations = 0,
        .max_admitted_bytes = 16 * 1024 * 1024,
        .max_reserved_operations = 0,
        .max_reserved_bytes = 16 * 1024 * 1024,
    }};
    runtime::SessionEventCommitment commitment{channel.persistence};
    auto sink = commitment.sink();

    auto verdict = sink(agent::MessageEndEvent{user_msg("hello")});
    REQUIRE_FALSE(verdict.has_value());
    CHECK(verdict.error().code == support::ErrorCode::Busy);

    // The saturation is latched like any persistence failure.
    auto concluded = channel.conclude(commitment, support::ExpectedVoid{});
    REQUIRE_FALSE(concluded.has_value());
    CHECK(concluded.error().code == support::ErrorCode::Busy);
    CHECK(channel.persisted_user_texts().empty());
}

TEST_CASE(
    "Session Event Commitment without a persistence channel is a successful no-op",
    "[coding_agent][runtime][commitment][issue464]") {
    CommitmentChannel channel;
    runtime::SessionEventCommitment commitment{nullptr};
    auto sink = commitment.sink();

    REQUIRE(sink(agent::MessageEndEvent{user_msg("hello")}).has_value());
    CHECK(channel.conclude(commitment, support::ExpectedVoid{}).has_value());
    CHECK(channel.persisted_user_texts().empty());
}

TEST_CASE(
    "Session Event Commitment appends completed messages to an in-memory store's live tree",
    "[coding_agent][runtime][commitment][issue491]") {
    // In-memory sessions have no off-loop channel; the sink commits straight
    // into the store's live tree (pi's non-persisting SessionManager keeps
    // the same in-memory entries).
    tests::TempWorkspace workspace;
    harness::session::SessionMetadata metadata{
        .session_id = "commitment-memory",
        .created_at = "2026-07-05T00:00:00Z",
        .workspace = workspace.path(),
        .provider = "fake",
        .model = "fake-model",
    };
    auto store = std::make_shared<harness::session::SessionStore>(
        harness::session::SessionStore::in_memory(metadata));
    runtime::SessionEventCommitment commitment{nullptr, store};
    auto sink = commitment.sink();

    REQUIRE(sink(agent::AgentStartEvent{}).has_value());
    REQUIRE(sink(agent::MessageStartEvent{user_msg("draft")}).has_value());
    ai::BashExecutionMessage bash;
    bash.command = "ls";
    REQUIRE(sink(agent::MessageEndEvent{std::move(bash)}).has_value());
    REQUIRE(sink(agent::MessageEndEvent{user_msg("one")}).has_value());
    REQUIRE(sink(agent::MessageEndEvent{user_msg("two")}).has_value());

    const auto entries = store->entries();
    REQUIRE(entries.size() == 2);
    CHECK(
        entries.front().kind == harness::session::SessionEntryKind::Message);
    CHECK(store->leaf_id() == entries.back().entry_id);
    const auto context = store->build_context();
    REQUIRE(context.messages.size() == 2);
    CHECK(
        ai::text_from_user_message(
            std::get<ai::UserMessage>(context.messages[0])) == "one");
    CHECK(
        ai::text_from_user_message(
            std::get<ai::UserMessage>(context.messages[1])) == "two");

    boost::asio::io_context io;
    CHECK(run_awaitable(io, commitment.conclude(support::ExpectedVoid{}))
              .has_value());
}

TEST_CASE(
    "persistence control work is admitted while the ordinary runtime budget is saturated",
    "[coding_agent][runtime][commitment][issue465]") {
    // A small ordinary budget, a full reserved control lane (defaults).
    CommitmentChannel channel{harness::RuntimeLimits{
        .worker_count = 2,
        .max_admitted_operations = 4,
        .max_admitted_bytes = 16 * 1024 * 1024,
    }};

    // Fill the ordinary budget with bulk work that stays admitted.
    std::vector<harness::RuntimeTarget::Admission> bulk;
    for (std::size_t index = 0; index < 4; ++index) {
        auto admission = channel.target->try_admit(16);
        REQUIRE(admission.has_value());
        bulk.push_back(std::move(*admission));
    }
    CHECK_FALSE(channel.target->try_admit(16).has_value());

    // The persistence append is control work (ADR 0040 §Admission): reserved
    // admission means ordinary bulk saturation cannot reject it.
    auto appended =
        channel.persistence->submit_message_append(user_msg("control work"));
    CHECK(appended.has_value());

    // Settle the bulk work, then the append outcome reaches the mailbox and
    // the persistence chain goes idle.
    for (auto& admission : bulk) {
        std::move(admission).complete([]() noexcept {});
    }
    std::atomic<bool> drained{false};
    std::atomic<bool> done{false};
    boost::asio::co_spawn(
        *channel.io,
        [&]() -> boost::asio::awaitable<void> {
            co_await channel.persistence->drain();
            drained.store(true, std::memory_order_release);
            done.store(true, std::memory_order_release);
        },
        boost::asio::detached);
    REQUIRE(pump_until(*channel.io, done));
    CHECK(drained.load(std::memory_order_acquire));
    CHECK_FALSE(channel.persistence->failure().has_value());
    CHECK(
        channel.persisted_user_texts() ==
        std::vector<std::string>{"control work"});
}
