#include <cch/durable/Session.hpp>
#include <cch/durable/Storage.hpp>

#include "durable/GatedStorage.hpp"
#include "durable/Outcome.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace cch;
using namespace cch::durable;
using namespace cch::tests;
using namespace cch::tests::durable;

namespace {

/// One committed-read job body: count the committed conversations.
[[nodiscard]] support::Expected<chord::Json> count_conversations(Storage& storage) {
    auto conversations = storage.scan_conversations(100);
    if (!conversations) {
        return std::unexpected(std::move(conversations.error()));
    }
    return chord::Json{static_cast<double>(conversations->size())};
}

/// Run one commit that creates one conversation and return its terminal outcome.
[[nodiscard]] Outcome<void> run_create_conversation_commit(Session& session) {
    return consume(session.commit([](Transaction& transaction) -> support::Expected<void> {
        auto conversation = transaction.create_conversation();
        if (!conversation) {
            return std::unexpected(std::move(conversation.error()));
        }
        return {};
    }));
}

/// The completion order one in-flight commit, one queued read and one queued
/// commit produce after the gate releases, with the read in the middle.
const std::vector<std::string> kCommitReadCommitOrder{
        "first-done",
        "read-runs",
        "read-done",
        "second-runs",
        "second-done",
};

/// Releases the held commit when the current scope exits, so a failing
/// REQUIRE can never strand a thread blocked behind the gate.
struct HeldCommitRelease {
    GatedStorage& storage;
    ~HeldCommitRelease() {
        if (storage.holds_commit()) {
            storage.release_held_commit();
        }
    }
};

/// Yield for the remainder of one bounded negative-observation window: the
/// caller asserts afterwards that nothing settled during it. A generous
/// liveness backstop, not a performance bound (CODING_STANDARDS.md §11.9):
/// the events asserted absent cannot happen before the gate releases, so a
/// correct implementation always pays the full window.
template <typename Predicate> void wait_until(const Predicate& predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
}

} // namespace

// Frozen contract: pi-v1.3.0 `SessionImpl.commitWith` and `SessionImpl.readOnLine`
// join the same FIFO `#enqueue` tail (verified against the frozen baseline with an
// isolated probe, /tmp/cch-1080-pi-frozen at 71023aec1546d6d677a65669c5665c0927ce0dd0).
//
// Discriminating case: hold the first Storage commit unsettled, then admit a
// committed read. A read that takes only the mutation line observes zero
// committed conversations and completes before the storage settles; the
// frozen read waits on the same FIFO tail, so its callback must not run and
// its completion must not arrive before the held commit settles, and after
// the release it must observe the committed conversation.
TEST_CASE("committed reads wait for earlier admitted commits",
        "[durable][session][read-order][ordering][issue1164][spec]") {
    GatedStorage storage;
    auto session = Session::create(storage);

    storage.hold_next_commit();
    HeldCommitRelease release_held{storage};
    std::vector<std::string> events;
    auto pending_commit = session->commit([](Transaction& transaction) -> support::Expected<void> {
        auto conversation = transaction.create_conversation();
        if (!conversation) {
            return std::unexpected(std::move(conversation.error()));
        }
        return {};
    });
    std::move(pending_commit).start([&events](support::Expected<void> outcome) mutable noexcept {
        CHECK(outcome.has_value());
        events.push_back("commit-done");
    });
    REQUIRE(storage.holds_commit());

    std::atomic_bool read_ran{false};
    std::atomic_bool read_done{false};
    std::optional<support::Expected<chord::Json>> read_outcome;
    std::jthread reader([&] {
        auto operation = session->read_committed([&](Storage& store) -> support::Expected<chord::Json> {
            read_ran.store(true);
            return count_conversations(store);
        });
        std::move(operation).start(
                [&events, &read_outcome, &read_done](support::Expected<chord::Json> outcome) mutable noexcept {
                    events.push_back("read-done");
                    read_outcome = std::move(outcome);
                    read_done.store(true);
                });
    });

    // Bounded liveness window: the read must not complete while the earlier
    // commit is unsettled, and its callback must not have run at all.
    wait_until([&] { return read_done.load(); });
    const bool read_settled_early = read_done.load();
    INFO("read completed before storage settlement: " << read_settled_early);
    CHECK_FALSE(read_settled_early);
    CHECK_FALSE(read_ran.load());

    storage.release_held_commit();
    reader.join();
    const auto completion_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!read_done.load() && std::chrono::steady_clock::now() < completion_deadline) {
        std::this_thread::yield();
    }
    REQUIRE(read_done.load());
    REQUIRE(read_outcome.has_value());
    REQUIRE(read_outcome->has_value());
    REQUIRE((*read_outcome)->as_number() != nullptr);
    CHECK(*(*read_outcome)->as_number() == 1.0);
    CHECK(events == std::vector<std::string>{"commit-done", "read-done"});
    CHECK(session->closed().reason == SessionEndReason::Open);
}

// Frozen contract: one FIFO tail orders a read after earlier commits and a
// commit admitted after the read behind it.
//
// Discriminating case: with the first commit held, admit a read and then a
// second commit. The read must run after the first commit settles and before
// the second commit's callback starts, and it must observe exactly the first
// commit's conversation: a read that also waits for commits admitted after it
// would observe 2, and a read that overtakes the first commit would observe 0.
TEST_CASE("commits admitted after a committed read run after it completes",
        "[durable][session][read-order][ordering][issue1164][spec]") {
    GatedStorage storage;
    auto session = Session::create(storage);

    storage.hold_next_commit();
    HeldCommitRelease release_held{storage};
    std::vector<std::string> events;
    std::optional<double> observed_count{-1.0};

    auto first = session->commit([](Transaction& transaction) -> support::Expected<void> {
        auto conversation = transaction.create_conversation();
        if (!conversation) {
            return std::unexpected(std::move(conversation.error()));
        }
        return {};
    });
    std::move(first).start([&events](support::Expected<void> outcome) mutable noexcept {
        CHECK(outcome.has_value());
        events.push_back("first-done");
    });
    REQUIRE(storage.holds_commit());

    auto read = session->read_committed([&events, &observed_count](Storage& store) -> support::Expected<chord::Json> {
        events.push_back("read-runs");
        auto outcome = count_conversations(store);
        if (outcome) {
            if (const double* number = outcome->as_number(); number != nullptr) {
                observed_count = *number;
            }
        }
        return outcome;
    });
    std::move(read).start([&events](support::Expected<chord::Json> outcome) mutable noexcept {
        CHECK(outcome.has_value());
        events.push_back("read-done");
    });

    auto second = session->commit([&events](Transaction& transaction) -> support::Expected<void> {
        events.push_back("second-runs");
        auto conversation = transaction.create_conversation();
        if (!conversation) {
            return std::unexpected(std::move(conversation.error()));
        }
        return {};
    });
    std::move(second).start([&events](support::Expected<void> outcome) mutable noexcept {
        CHECK(outcome.has_value());
        events.push_back("second-done");
    });

    wait_until([&] { return !events.empty(); });
    CHECK(events.empty());

    storage.release_held_commit();
    wait_until([&] { return events.size() == kCommitReadCommitOrder.size(); });
    REQUIRE(events.size() == kCommitReadCommitOrder.size());
    CHECK(events == kCommitReadCommitOrder);
    REQUIRE(observed_count.has_value());
    CHECK(*observed_count == 1.0);
}

// Frozen contract: `#enqueue` keeps draining after a failure, so a job that
// waited on the tail rejects with `SessionFailed` without running.
//
// Discriminating case: hold the first commit, admit a read and a second
// commit, then fail the held commit itself. The read must reject with the
// frozen SessionFailed message and its callback must never run; the commit
// admitted after the read rejects with SessionFailed after the read.
TEST_CASE("a failing held commit rejects a queued committed read without running it",
        "[durable][session][read-order][failure][issue1164][spec]") {
    GatedStorage storage;
    auto session = Session::create(storage);

    storage.hold_next_commit();
    std::optional<support::Expected<void>> commit_outcome;
    auto pending_commit = session->commit([](Transaction& transaction) -> support::Expected<void> {
        auto conversation = transaction.create_conversation();
        if (!conversation) {
            return std::unexpected(std::move(conversation.error()));
        }
        return {};
    });
    std::move(pending_commit).start([&commit_outcome](support::Expected<void> outcome) mutable noexcept {
        commit_outcome = std::move(outcome);
    });
    REQUIRE(storage.holds_commit());

    std::atomic_bool read_ran{false};
    std::vector<std::string> events;
    auto read = session->read_committed([&](Storage&) -> support::Expected<chord::Json> {
        read_ran.store(true);
        return chord::Json{0.0};
    });
    std::move(read).start([&events](support::Expected<chord::Json> outcome) mutable noexcept {
        CHECK_FALSE(outcome.has_value());
        events.push_back("read-rejected");
    });

    auto queued = session->commit([](Transaction& transaction) -> support::Expected<void> {
        auto conversation = transaction.create_conversation();
        if (!conversation) {
            return std::unexpected(std::move(conversation.error()));
        }
        return {};
    });
    std::move(queued).start([&events](support::Expected<void> outcome) mutable noexcept {
        CHECK_FALSE(outcome.has_value());
        events.push_back("queued-rejected");
    });

    storage.fail_held_commit("storage vanished");
    REQUIRE(commit_outcome.has_value());
    REQUIRE_FALSE(commit_outcome->has_value());
    CHECK(commit_outcome->error().message == "storage vanished");
    REQUIRE(read_ran.load() == false);
    REQUIRE(events.size() == 2);
    CHECK(events == std::vector<std::string>({"read-rejected", "queued-rejected"}));
    CHECK(session->failed());
    CHECK(storage.close_calls() == 1);

    // A read admitted after the failure reports the frozen SessionFailed
    // message, not the storage error and not "Session is closed".
    auto after = consume(
            session->read_committed([](Storage&) -> support::Expected<chord::Json> { return chord::Json{0.0}; }));
    REQUIRE_FALSE(after.has_value());
    CHECK(after.error().message == "Session failed after a storage error; close and reopen it");
}

// Frozen contract: `close()` seals admission, then enqueues the storage close
// on the same tail, so admitted reads settle before the backend closes.
//
// Discriminating case: close with a held commit and a queued read. The read
// must run after the commit settles and before the storage close, and close
// must complete only after the read. A close that drops or cancels the
// admitted read, or closes the backend while the read waits, fails this case.
TEST_CASE("close settles an admitted committed read before closing storage",
        "[durable][session][read-order][close][issue1164][spec]") {
    GatedStorage storage;
    auto session = Session::create(storage);

    storage.hold_next_commit();
    HeldCommitRelease release_held{storage};
    std::optional<support::Expected<void>> commit_outcome;
    auto pending_commit = session->commit([](Transaction& transaction) -> support::Expected<void> {
        auto conversation = transaction.create_conversation();
        if (!conversation) {
            return std::unexpected(std::move(conversation.error()));
        }
        return {};
    });
    std::move(pending_commit).start([&commit_outcome](support::Expected<void> outcome) mutable noexcept {
        commit_outcome = std::move(outcome);
    });
    REQUIRE(storage.holds_commit());

    std::atomic_bool read_ran{false};
    std::optional<support::Expected<chord::Json>> read_outcome;
    auto read = session->read_committed([&](Storage& store) -> support::Expected<chord::Json> {
        read_ran.store(true);
        return count_conversations(store);
    });
    std::move(read).start([&read_outcome](support::Expected<chord::Json> outcome) mutable noexcept {
        read_outcome = std::move(outcome);
    });

    std::optional<support::Expected<void>> close_outcome;
    auto closing = session->close();
    std::move(closing).start(
            [&close_outcome](support::Expected<void> outcome) mutable noexcept { close_outcome = std::move(outcome); });
    wait_until([&] { return storage.close_calls() > 0 || close_outcome.has_value(); });
    CHECK(storage.close_calls() == 0);
    CHECK_FALSE(close_outcome.has_value());
    CHECK_FALSE(read_ran.load());

    storage.release_held_commit();
    wait_until([&] { return close_outcome.has_value() && read_outcome.has_value(); });
    REQUIRE(close_outcome.has_value());
    CHECK(close_outcome->has_value());
    REQUIRE(read_outcome.has_value());
    REQUIRE(read_outcome->has_value());
    REQUIRE((*read_outcome)->as_number() != nullptr);
    CHECK(*(*read_outcome)->as_number() == 1.0);
    CHECK(storage.close_calls() == 1);
    CHECK(session->closed().reason == SessionEndReason::Closed);

    auto rejected = consume(
            session->read_committed([](Storage&) -> support::Expected<chord::Json> { return chord::Json{0.0}; }));
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error().message == "Session is closed");
}

// Frozen contract: close drains the whole tail in admission order, whatever
// mix of commits and reads it holds.
//
// Discriminating case: close with a held commit, a queued commit, a queued
// read and another queued commit. Every job settles in admission order, the
// read observes both committed conversations, and the storage close is last.
TEST_CASE("close drains a mixed queue of commits and committed reads in admission order",
        "[durable][session][read-order][close][ordering][issue1164][spec]") {
    GatedStorage storage;
    auto session = Session::create(storage);

    storage.hold_next_commit();
    HeldCommitRelease release_held{storage};
    std::vector<std::string> events;
    std::optional<double> observed_count{-1.0};

    auto first = session->commit([](Transaction& transaction) -> support::Expected<void> {
        auto conversation = transaction.create_conversation();
        if (!conversation) {
            return std::unexpected(std::move(conversation.error()));
        }
        return {};
    });
    std::move(first).start([&events](support::Expected<void> outcome) mutable noexcept {
        CHECK(outcome.has_value());
        events.push_back("first-done");
    });
    REQUIRE(storage.holds_commit());

    auto second = session->commit([&events](Transaction& transaction) -> support::Expected<void> {
        events.push_back("second-runs");
        auto conversation = transaction.create_conversation();
        if (!conversation) {
            return std::unexpected(std::move(conversation.error()));
        }
        return {};
    });
    std::move(second).start([&events](support::Expected<void> outcome) mutable noexcept {
        CHECK(outcome.has_value());
        events.push_back("second-done");
    });

    auto read = session->read_committed([&events, &observed_count](Storage& store) -> support::Expected<chord::Json> {
        events.push_back("read-runs");
        auto outcome = count_conversations(store);
        if (outcome) {
            if (const double* number = outcome->as_number(); number != nullptr) {
                observed_count = *number;
            }
        }
        return outcome;
    });
    std::move(read).start([&events](support::Expected<chord::Json> outcome) mutable noexcept {
        CHECK(outcome.has_value());
        events.push_back("read-done");
    });

    auto third = session->commit([&events](Transaction& transaction) -> support::Expected<void> {
        events.push_back("third-runs");
        auto conversation = transaction.create_conversation();
        if (!conversation) {
            return std::unexpected(std::move(conversation.error()));
        }
        return {};
    });
    std::move(third).start([&events](support::Expected<void> outcome) mutable noexcept {
        CHECK(outcome.has_value());
        events.push_back("third-done");
    });

    std::optional<support::Expected<void>> close_outcome;
    auto closing = session->close();
    std::move(closing).start([&events, &close_outcome](support::Expected<void> outcome) mutable noexcept {
        events.push_back("close-done");
        close_outcome = std::move(outcome);
    });

    wait_until([&] { return !events.empty(); });
    CHECK(events.empty());
    CHECK(storage.close_calls() == 0);

    storage.release_held_commit();
    const std::vector<std::string> expected{
            "first-done",
            "second-runs",
            "second-done",
            "read-runs",
            "read-done",
            "third-runs",
            "third-done",
            "close-done",
    };
    wait_until([&] { return events.size() == expected.size(); });
    REQUIRE(events.size() == expected.size());
    CHECK(events == expected);
    REQUIRE(observed_count.has_value());
    CHECK(*observed_count == 2.0);
    REQUIRE(close_outcome.has_value());
    CHECK(close_outcome->has_value());
    CHECK(storage.close_calls() == 1);
    CHECK(session->closed().reason == SessionEndReason::Closed);
}

// The frozen `readOnLine` job's own error rejects its read only; the Session
// stays healthy, matching the C++ committed-read error propagation of the
// snapshot family.
//
// Discriminating case: a read callback that reports an error must deliver
// exactly that error to the read's completion, and a later commit must still
// succeed. A Session that fails on the read error breaks the later commit.
TEST_CASE("a committed read error rejects only the read and leaves the session usable",
        "[durable][session][read-order][failure][issue1164][spec]") {
    MemoryStorage storage;
    auto session = Session::create(storage);
    REQUIRE(run_create_conversation_commit(*session).has_value());

    auto rejected = consume(session->read_committed([](Storage&) -> support::Expected<chord::Json> {
        return std::unexpected(support::make_error(support::ErrorCode::Validation, "inspection query is invalid"));
    }));
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error().message == "inspection query is invalid");
    CHECK_FALSE(session->failed());
    CHECK(session->closed().reason == SessionEndReason::Open);

    auto read = consume(session->read_committed(
            [](Storage& store) -> support::Expected<chord::Json> { return count_conversations(store); }));
    REQUIRE(read.has_value());
    REQUIRE(read.has_value());
    REQUIRE(read.value().as_number() != nullptr);
    CHECK(*read.value().as_number() == 1.0);
    REQUIRE(run_create_conversation_commit(*session).has_value());
    CHECK_FALSE(session->failed());
}

// The C++ AsyncResult convention completes a ready value inline (ADR 0040);
// over a synchronous storage the whole line settles at start(). The ordering
// guarantee is unchanged: the read observes every commit admitted before it.
TEST_CASE(
        "committed reads complete inline over synchronous storage", "[durable][session][read-order][issue1164][spec]") {
    MemoryStorage storage;
    auto session = Session::create(storage);
    REQUIRE(run_create_conversation_commit(*session).has_value());

    auto read = consume(session->read_committed(
            [](Storage& store) -> support::Expected<chord::Json> { return count_conversations(store); }));
    REQUIRE(read.has_value());
    REQUIRE(read.has_value());
    REQUIRE(read.value().as_number() != nullptr);
    CHECK(*read.value().as_number() == 1.0);

    // The same read issued before any commit observes the empty committed
    // state without creating anything.
    MemoryStorage empty_storage;
    auto empty_session = Session::create(empty_storage);
    auto empty = consume(empty_session->read_committed(
            [](Storage& store) -> support::Expected<chord::Json> { return count_conversations(store); }));
    REQUIRE(empty.has_value());
    REQUIRE(empty.has_value());
    REQUIRE(empty.value().as_number() != nullptr);
    CHECK(*empty.value().as_number() == 0.0);
}

// A completion callback may admit the next operation on the same line: the
// recursive mutation line and the pump's reentrancy guard make the admitted
// read run without a deadlock after the enclosing completion returns. This
// is the #1150 completion convention:
// the Session outlives the operation until its completion callback returns.
//
// Discriminating case: a read admitted from inside a commit completion must
// observe that commit's conversation. A read that reorders ahead of the
// enclosing commit observes zero and fails this case.
TEST_CASE("a committed read admitted from a commit completion observes that commit",
        "[durable][session][read-order][reentrancy][issue1164][spec]") {
    MemoryStorage storage;
    auto session = Session::create(storage);

    std::optional<support::Expected<chord::Json>> read_outcome;
    std::optional<support::Expected<void>> commit_outcome;
    auto commit = session->commit([](Transaction& transaction) -> support::Expected<void> {
        auto conversation = transaction.create_conversation();
        if (!conversation) {
            return std::unexpected(std::move(conversation.error()));
        }
        return {};
    });
    std::move(commit).start([&](support::Expected<void> outcome) mutable noexcept {
        commit_outcome = std::move(outcome);
        if (!commit_outcome->has_value()) {
            return;
        }
        // Admit the read from inside the completion callback: it joins the
        // line this callback still holds, so it must observe this commit.
        // Its completion runs only after this callback returns (the #1150
        // completion convention), so the outcome is asserted below.
        auto read = session->read_committed(
                [](Storage& store) -> support::Expected<chord::Json> { return count_conversations(store); });
        std::move(read).start([&read_outcome](support::Expected<chord::Json> result) mutable noexcept {
            read_outcome = std::move(result);
        });
    });
    REQUIRE(commit_outcome.has_value());
    REQUIRE(commit_outcome->has_value());
    REQUIRE(read_outcome.has_value());
    REQUIRE(read_outcome->has_value());
    REQUIRE((*read_outcome)->as_number() != nullptr);
    CHECK(*(*read_outcome)->as_number() == 1.0);
    CHECK(session->closed().reason == SessionEndReason::Open);
}
