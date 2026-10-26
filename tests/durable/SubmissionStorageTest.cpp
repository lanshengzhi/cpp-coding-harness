#include <cch/durable/JsonlStorage.hpp>
#include <cch/durable/Session.hpp>
#include <cch/durable/SqliteStorage.hpp>
#include <cch/durable/Storage.hpp>

#include "durable/GatedStorage.hpp"
#include "durable/Outcome.hpp"
#include "support/TempWorkspace.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace cch;
using namespace cch::durable;
using namespace cch::tests;
using namespace cch::tests::durable;

namespace {

/// Run one commit and return its terminal outcome.
[[nodiscard]] Outcome<void> run_commit(
        Session& session, std::move_only_function<support::Expected<void>(Transaction&)> change) {
    return consume(session.commit(std::move(change)));
}

/// The committed conversation of a fresh session.
[[nodiscard]] ConversationId create_conversation(Session& session) {
    std::optional<ConversationId> created;
    auto outcome = run_commit(session, [&created](Transaction& transaction) -> support::Expected<void> {
        auto record = transaction.create_conversation();
        if (!record) {
            return std::unexpected(std::move(record.error()));
        }
        created = record->id;
        return {};
    });
    REQUIRE(outcome.has_value());
    REQUIRE(created.has_value());
    return *created;
}

/// Create one queued input submission and return its minted record.
[[nodiscard]] SubmissionRecord create_queued_submission(
        Session& session, ConversationId conversation_id, std::optional<std::string> request_id = std::nullopt) {
    std::optional<SubmissionRecord> created;
    auto outcome = run_commit(
            session, [&conversation_id, &request_id, &created](Transaction& transaction) -> support::Expected<void> {
                auto record = transaction.create_submission(SubmissionRecord{.id = {},
                        .conversation_id = conversation_id,
                        .request_id = std::move(request_id),
                        .type = "input",
                        .status = "queued"});
                if (!record) {
                    return std::unexpected(std::move(record.error()));
                }
                created = *record;
                return {};
            });
    REQUIRE(outcome.has_value());
    REQUIRE(created.has_value());
    return *created;
}

/// Append one entry and return its record.
[[nodiscard]] EntryRecord append_entry(Session& session, ConversationId conversation_id, std::string kind) {
    std::optional<EntryRecord> created;
    auto outcome = run_commit(
            session, [&conversation_id, &kind, &created](Transaction& transaction) -> support::Expected<void> {
                auto record =
                        transaction.append_entry(conversation_id, std::move(kind), chord::Json{chord::Json::Object{}});
                if (!record) {
                    return std::unexpected(std::move(record.error()));
                }
                created = *record;
                return {};
            });
    REQUIRE(outcome.has_value());
    REQUIRE(created.has_value());
    return *created;
}

/// Read one committed submission by id through a transaction.
[[nodiscard]] std::optional<SubmissionRecord> read_submission(Session& session, SubmissionId id) {
    std::optional<SubmissionRecord> read;
    auto outcome = run_commit(session, [&id, &read](Transaction& transaction) -> support::Expected<void> {
        auto record = transaction.submission(id);
        if (!record) {
            return std::unexpected(std::move(record.error()));
        }
        read = std::move(*record);
        return {};
    });
    REQUIRE(outcome.has_value());
    return read;
}

} // namespace

// Frozen contract: packages/durable/src/session/transaction.ts createSubmission
// mints a Session-global ID and stages the create; Storage.submission and
// Storage.submissionByRequest read it back (frozen `Storage.submission`,
// `Storage.submissionByRequest`).
TEST_CASE("a submission record round-trips by id and by conversation-scoped request id",
        "[durable][submission][issue1080]") {
    MemoryStorage storage;
    auto session = Session::create(storage);
    const ConversationId conversation_id = create_conversation(*session);

    SubmissionRecord created = create_queued_submission(*session, conversation_id, std::string{"request-1"});
    REQUIRE(created.id.value > 0);
    CHECK(created.conversation_id == conversation_id);
    CHECK(created.type == "input");
    CHECK(created.status == "queued");
    CHECK_FALSE(created.entry.has_value());
    CHECK_FALSE(created.answer.has_value());

    // The transaction reads see the committed record by id and by its
    // conversation-scoped dedup key, and miss for foreign keys.
    std::optional<SubmissionRecord> by_id;
    std::optional<SubmissionRecord> by_request;
    std::optional<SubmissionRecord> by_other_request;
    const ConversationId other_conversation = create_conversation(*session);
    auto outcome = run_commit(*session,
            [&created, &conversation_id, &by_id, &by_request, &by_other_request, &other_conversation](
                    Transaction& transaction) -> support::Expected<void> {
                auto read_id = transaction.submission(created.id);
                if (!read_id) {
                    return std::unexpected(std::move(read_id.error()));
                }
                by_id = std::move(*read_id);
                auto read_request = transaction.submission_by_request(conversation_id, "request-1");
                if (!read_request) {
                    return std::unexpected(std::move(read_request.error()));
                }
                by_request = std::move(*read_request);
                auto read_other = transaction.submission_by_request(other_conversation, "request-1");
                if (!read_other) {
                    return std::unexpected(std::move(read_other.error()));
                }
                by_other_request = std::move(*read_other);
                auto read_missing = transaction.submission_by_request(conversation_id, "unknown");
                if (!read_missing) {
                    return std::unexpected(std::move(read_missing.error()));
                }
                CHECK_FALSE((*read_missing).has_value());
                return {};
            });
    REQUIRE(outcome.has_value());
    REQUIRE(by_id.has_value());
    CHECK(*by_id == created);
    REQUIRE(by_request.has_value());
    CHECK(*by_request == created);
    CHECK_FALSE(by_other_request.has_value());
}

// Frozen contract: packages/durable/src/session/transaction.ts L442-457 —
// settleSubmission/placeSubmission STAGE changes that resolve at assembly
// against the transaction's own candidates first, then committed state, so
// they are not table reads and work after the first table write of the same
// commit.
TEST_CASE("a staged submission placement resolves at assembly after other writes in the same commit",
        "[durable][submission][transaction][issue1080]") {
    MemoryStorage storage;
    auto session = Session::create(storage);
    const ConversationId conversation_id = create_conversation(*session);
    const SubmissionRecord queued = create_queued_submission(*session, conversation_id, std::string{"request-1"});

    // The same commit appends the transcript entry (a table write) and then
    // places the queued submission at it: the staged change resolves at
    // assembly against committed state, so the placement survives the entry
    // write that would reject a table read.
    auto outcome =
            run_commit(*session, [&conversation_id, &queued](Transaction& transaction) -> support::Expected<void> {
                auto entry = transaction.append_entry(conversation_id,
                        "pi.user",
                        chord::Json{chord::Json::Object{{"text", chord::Json{std::string{"hello"}}}}});
                if (!entry) {
                    return std::unexpected(std::move(entry.error()));
                }
                auto placed = transaction.place_submission(queued.id, entry->id);
                if (!placed) {
                    return std::unexpected(std::move(placed.error()));
                }
                return {};
            });
    REQUIRE(outcome.has_value());

    const std::optional<SubmissionRecord> placed = read_submission(*session, queued.id);
    REQUIRE(placed.has_value());
    CHECK(placed->status == "placed");
    REQUIRE(placed->entry.has_value());
    CHECK(placed->request_id == std::optional<std::string>{std::string{"request-1"}});

    // The answer settles in the same commit as the assistant entry that
    // produced it: staged again, resolved at assembly.
    std::optional<EntryId> answer_id;
    auto answer_outcome = run_commit(
            *session, [&conversation_id, &queued, &answer_id](Transaction& transaction) -> support::Expected<void> {
                auto answer =
                        transaction.append_entry(conversation_id, "pi.assistant", chord::Json{chord::Json::Object{}});
                if (!answer) {
                    return std::unexpected(std::move(answer.error()));
                }
                answer_id = answer->id;
                auto settled = transaction.settle_submission(
                        queued.id, SubmissionSettlement{.status = "done", .answer = answer->id});
                if (!settled) {
                    return std::unexpected(std::move(settled.error()));
                }
                return {};
            });
    REQUIRE(answer_outcome.has_value());
    REQUIRE(answer_id.has_value());

    const std::optional<SubmissionRecord> done = read_submission(*session, queued.id);
    REQUIRE(done.has_value());
    CHECK(done->status == "done");
    CHECK(done->entry == placed->entry);
    CHECK(done->answer == std::optional<EntryId>{*answer_id});
}

// Frozen contract: a create and its settle in ONE commit resolve against the
// transaction's own staged candidate, not committed state (transaction.ts
// `#submissions.get(id) ?? storage.submission(id)`).
TEST_CASE("a submission created and settled in one commit resolves against its staged candidate",
        "[durable][submission][transaction][issue1080]") {
    MemoryStorage storage;
    auto session = Session::create(storage);
    const ConversationId conversation_id = create_conversation(*session);

    std::optional<SubmissionId> created_id;
    auto outcome =
            run_commit(*session, [&conversation_id, &created_id](Transaction& transaction) -> support::Expected<void> {
                auto created = transaction.create_submission(SubmissionRecord{.id = {},
                        .conversation_id = conversation_id,
                        .request_id = std::string{"request-2"},
                        .type = "input",
                        .status = "queued"});
                if (!created) {
                    return std::unexpected(std::move(created.error()));
                }
                created_id = created->id;
                // A queued input settles unanswered without ever touching committed
                // state: the staged create is the candidate the assembly resolves.
                auto settled = transaction.settle_submission(
                        created->id, SubmissionSettlement{.status = "unanswered", .reason = std::string{"aborted"}});
                if (!settled) {
                    return std::unexpected(std::move(settled.error()));
                }
                return {};
            });
    REQUIRE(outcome.has_value());
    REQUIRE(created_id.has_value());

    const std::optional<SubmissionRecord> settled = read_submission(*session, *created_id);
    REQUIRE(settled.has_value());
    CHECK(settled->status == "unanswered");
    CHECK(settled->reason == std::optional<std::string>{std::string{"aborted"}});
    CHECK_FALSE(settled->entry.has_value());
    CHECK(settled->request_id == std::optional<std::string>{std::string{"request-2"}});
}

// Frozen contract: applySubmissionChange (transaction.ts L67-82). Placement
// turns a queued input placed and a queued write done; only a placed input can
// be answered; a settled record stays settled.
TEST_CASE("submission terminal transitions mirror the frozen record model",
        "[durable][submission][transaction][issue1080]") {
    MemoryStorage storage;
    auto session = Session::create(storage);
    const ConversationId conversation_id = create_conversation(*session);

    // Only a placed input can be answered: settling a queued submission done
    // rejects the commit at assembly and publishes nothing.
    const SubmissionRecord queued = create_queued_submission(*session, conversation_id, std::string{"request-1"});
    const auto answer = append_entry(*session, conversation_id, "pi.assistant");
    auto rejected = run_commit(*session, [&queued, &answer](Transaction& transaction) -> support::Expected<void> {
        auto settled =
                transaction.settle_submission(queued.id, SubmissionSettlement{.status = "done", .answer = answer.id});
        if (!settled) {
            return std::unexpected(std::move(settled.error()));
        }
        return {};
    });
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error().message == "Submission " + std::to_string(queued.id.value) + " is not a placed input");
    CHECK(read_submission(*session, queued.id) == queued);

    const EntryId entry_id = answer.id;

    // A queued input settles unanswered; a placement staged behind that
    // settle in the SAME commit resolves against the staged candidate, and a
    // settled record stays settled (frozen no-op), so the commit succeeds and
    // the submission remains unanswered.
    auto settle_then_place =
            run_commit(*session, [&queued, &entry_id](Transaction& transaction) -> support::Expected<void> {
                auto settled = transaction.settle_submission(
                        queued.id, SubmissionSettlement{.status = "unanswered", .reason = std::string{"aborted"}});
                if (!settled) {
                    return std::unexpected(std::move(settled.error()));
                }
                auto placed = transaction.place_submission(queued.id, entry_id);
                if (!placed) {
                    return std::unexpected(std::move(placed.error()));
                }
                return {};
            });
    REQUIRE(settle_then_place.has_value());
    const std::optional<SubmissionRecord> unanswered = read_submission(*session, queued.id);
    REQUIRE(unanswered.has_value());
    CHECK(unanswered->status == "unanswered");
    CHECK_FALSE(unanswered->entry.has_value());

    // A placed submission cannot be placed again: the placement rejects at
    // assembly against the transaction's staged candidate.
    const SubmissionRecord placed_first = create_queued_submission(*session, conversation_id, std::string{"request-2"});
    auto place_first =
            run_commit(*session, [&placed_first, &entry_id](Transaction& transaction) -> support::Expected<void> {
                auto placed = transaction.place_submission(placed_first.id, entry_id);
                if (!placed) {
                    return std::unexpected(std::move(placed.error()));
                }
                return {};
            });
    REQUIRE(place_first.has_value());
    auto rejected_place =
            run_commit(*session, [&placed_first, &entry_id](Transaction& transaction) -> support::Expected<void> {
                auto placed = transaction.place_submission(placed_first.id, entry_id);
                if (!placed) {
                    return std::unexpected(std::move(placed.error()));
                }
                return {};
            });
    REQUIRE_FALSE(rejected_place.has_value());
    CHECK(rejected_place.error().message == "Submission " + std::to_string(placed_first.id.value) + " is not queued");

    // A settled record stays settled: re-settling a done submission is a
    // frozen no-op, not an error.
    auto settle_done =
            run_commit(*session, [&placed_first, &answer](Transaction& transaction) -> support::Expected<void> {
                auto settled = transaction.settle_submission(
                        placed_first.id, SubmissionSettlement{.status = "done", .answer = answer.id});
                if (!settled) {
                    return std::unexpected(std::move(settled.error()));
                }
                return {};
            });
    REQUIRE(settle_done.has_value());
    auto settle_again = run_commit(*session, [&placed_first](Transaction& transaction) -> support::Expected<void> {
        auto settled = transaction.settle_submission(
                placed_first.id, SubmissionSettlement{.status = "unanswered", .reason = std::string{"late"}});
        if (!settled) {
            return std::unexpected(std::move(settled.error()));
        }
        return {};
    });
    REQUIRE(settle_again.has_value());
    const std::optional<SubmissionRecord> still_done = read_submission(*session, placed_first.id);
    REQUIRE(still_done.has_value());
    CHECK(still_done->status == "done");
    CHECK(still_done->answer == std::optional<EntryId>{answer.id});

    // Placement turns a queued WRITE done (frozen `SubmissionRecord` write
    // family has no placed state).
    std::optional<SubmissionId> write_id;
    auto create_write =
            run_commit(*session, [&conversation_id, &write_id](Transaction& transaction) -> support::Expected<void> {
                auto created = transaction.create_submission(SubmissionRecord{.id = {},
                        .conversation_id = conversation_id,
                        .request_id = std::string{"request-3"},
                        .type = "write",
                        .status = "queued"});
                if (!created) {
                    return std::unexpected(std::move(created.error()));
                }
                write_id = created->id;
                return {};
            });
    REQUIRE(create_write.has_value());
    auto place_write =
            run_commit(*session, [&write_id, &entry_id](Transaction& transaction) -> support::Expected<void> {
                auto placed = transaction.place_submission(*write_id, entry_id);
                if (!placed) {
                    return std::unexpected(std::move(placed.error()));
                }
                return {};
            });
    REQUIRE(place_write.has_value());
    const std::optional<SubmissionRecord> write_done = read_submission(*session, *write_id);
    REQUIRE(write_done.has_value());
    CHECK(write_done->status == "done");
    CHECK(write_done->entry == std::optional<EntryId>{entry_id});
}

// Frozen contract: Session publications carry the submission change as
// `{type: "submission", value}` (frozen pi `CommitChange` for a submission
// write; session assembles publications from the adopted batch).
TEST_CASE("a committed submission publishes the frozen submission change",
        "[durable][submission][publication][issue1080]") {
    GatedStorage storage;
    auto session = Session::create(storage);
    std::vector<CommitPublication> publications;
    REQUIRE(session->subscribe_commits(
            [&publications](const CommitPublication& publication) { publications.push_back(publication); }));

    const ConversationId conversation_id = create_conversation(*session);
    const SubmissionRecord created = create_queued_submission(*session, conversation_id, std::string{"request-1"});

    // The conversation commit published no submission change; the submission
    // commit published exactly the frozen change shape.
    REQUIRE(publications.size() == 2);
    for (const CommitChange& change : publications[0].changes) {
        CHECK(std::get_if<SubmissionCommitChange>(&change) == nullptr);
    }
    bool found = false;
    for (const CommitChange& change : publications[1].changes) {
        if (const auto* submission = std::get_if<SubmissionCommitChange>(&change)) {
            found = true;
            CHECK(submission->type == "submission");
            CHECK(submission->value == created);
        }
    }
    CHECK(found);

    // A staged placement publishes the replaced record, not the created one.
    const auto answer = append_entry(*session, conversation_id, "pi.assistant");
    const EntryId entry_id = answer.id;
    auto placed = run_commit(*session, [&created, &entry_id](Transaction& transaction) -> support::Expected<void> {
        auto place = transaction.place_submission(created.id, entry_id);
        if (!place) {
            return std::unexpected(std::move(place.error()));
        }
        return {};
    });
    REQUIRE(placed.has_value());
    REQUIRE(publications.size() == 4);
    std::optional<SubmissionCommitChange> settled_change;
    for (const CommitChange& change : publications[3].changes) {
        if (const auto* submission = std::get_if<SubmissionCommitChange>(&change)) {
            settled_change = *submission;
        }
    }
    REQUIRE(settled_change.has_value());
    CHECK(settled_change->type == "submission");
    CHECK(settled_change->value.id == created.id);
    CHECK(settled_change->value.status == "placed");
    CHECK(settled_change->value.entry == std::optional<EntryId>{entry_id});

    // Settle the placed submission done: the change replaces the record and
    // publishes.
    const std::size_t commits_before_done = storage.commit_calls();
    auto settle_done = run_commit(*session, [&created, &entry_id](Transaction& transaction) -> support::Expected<void> {
        auto settled =
                transaction.settle_submission(created.id, SubmissionSettlement{.status = "done", .answer = entry_id});
        if (!settled) {
            return std::unexpected(std::move(settled.error()));
        }
        return {};
    });
    REQUIRE(settle_done.has_value());
    CHECK(storage.commit_calls() == commits_before_done + 1);
    REQUIRE(publications.size() == 5);
    std::optional<SubmissionCommitChange> done_change;
    for (const CommitChange& change : publications[4].changes) {
        if (const auto* submission = std::get_if<SubmissionCommitChange>(&change)) {
            done_change = *submission;
        }
    }
    REQUIRE(done_change.has_value());
    CHECK(done_change->value.status == "done");
    CHECK(done_change->value.answer == std::optional<EntryId>{entry_id});

    // Discriminating case: a change that resolves against committed state
    // WITHOUT changing the record (a settled record stays settled) publishes
    // no write at all — no storage batch, no publication — even though the
    // commit itself succeeds.
    const std::size_t commits_before_noop = storage.commit_calls();
    auto settle_noop = run_commit(*session, [&created](Transaction& transaction) -> support::Expected<void> {
        auto settled = transaction.settle_submission(
                created.id, SubmissionSettlement{.status = "unanswered", .reason = std::string{"late"}});
        if (!settled) {
            return std::unexpected(std::move(settled.error()));
        }
        return {};
    });
    REQUIRE(settle_noop.has_value());
    CHECK(storage.commit_calls() == commits_before_noop);
    CHECK(publications.size() == 5);
    const std::optional<SubmissionRecord> after_noop = read_submission(*session, created.id);
    REQUIRE(after_noop.has_value());
    CHECK(after_noop->status == "done");
}

// Frozen contract: `Storage.scanSubmissions` scans oldest first; the C++
// mirror scan_submissions serves inspect and tests.
TEST_CASE("scan_submissions returns committed records oldest first", "[durable][submission][scan][issue1080]") {
    MemoryStorage storage;
    auto session = Session::create(storage);
    const ConversationId conversation_id = create_conversation(*session);
    const SubmissionRecord first = create_queued_submission(*session, conversation_id, std::string{"request-1"});
    const SubmissionRecord second = create_queued_submission(*session, conversation_id, std::string{"request-2"});
    const SubmissionRecord third = create_queued_submission(*session, conversation_id, std::string{"request-3"});

    auto scanned = storage.scan_submissions(10);
    REQUIRE(scanned.has_value());
    REQUIRE(scanned->size() == 3);
    CHECK((*scanned)[0].id == first.id);
    CHECK((*scanned)[1].id == second.id);
    CHECK((*scanned)[2].id == third.id);

    auto limited = storage.scan_submissions(2);
    REQUIRE(limited.has_value());
    REQUIRE(limited->size() == 2);
    CHECK((*limited)[0].id == first.id);
    CHECK((*limited)[1].id == second.id);

    // A replacement keeps one record per submission, in place.
    const EntryId entry_id = append_entry(*session, conversation_id, "pi.user").id;
    auto place = run_commit(*session, [&first, &entry_id](Transaction& transaction) -> support::Expected<void> {
        auto placed = transaction.place_submission(first.id, entry_id);
        if (!placed) {
            return std::unexpected(std::move(placed.error()));
        }
        return {};
    });
    REQUIRE(place.has_value());
    auto rescanned = storage.scan_submissions(10);
    REQUIRE(rescanned.has_value());
    REQUIRE(rescanned->size() == 3);
    CHECK((*rescanned)[0].id == first.id);
    CHECK((*rescanned)[0].status == "placed");
}

// Frozen contract: the submission table reads are `#read` families, rejected
// after the first table write with the frozen message shape.
TEST_CASE("a submission table read after the first table write is rejected",
        "[durable][submission][transaction][read-after-write][issue1080]") {
    MemoryStorage storage;
    auto session = Session::create(storage);
    const ConversationId conversation_id = create_conversation(*session);
    const SubmissionRecord created = create_queued_submission(*session, conversation_id, std::string{"request-1"});

    const auto rejected_id =
            run_commit(*session, [&created, &conversation_id](Transaction& transaction) -> support::Expected<void> {
                auto written = transaction.append_entry(conversation_id, "pi.user", chord::Json{chord::Json::Object{}});
                if (!written) {
                    return std::unexpected(std::move(written.error()));
                }
                auto read = transaction.submission(created.id);
                if (!read) {
                    return std::unexpected(std::move(read.error()));
                }
                return {};
            });
    REQUIRE_FALSE(rejected_id.has_value());
    CHECK(rejected_id.error().message == "Tx.submission() cannot read tables after the first table write");

    const auto rejected_request =
            run_commit(*session, [&conversation_id](Transaction& transaction) -> support::Expected<void> {
                auto written = transaction.append_entry(conversation_id, "pi.user", chord::Json{chord::Json::Object{}});
                if (!written) {
                    return std::unexpected(std::move(written.error()));
                }
                auto read = transaction.submission_by_request(conversation_id, "request-1");
                if (!read) {
                    return std::unexpected(std::move(read.error()));
                }
                return {};
            });
    REQUIRE_FALSE(rejected_request.has_value());
    CHECK(rejected_request.error().message ==
            "Tx.submissionByRequest() cannot read tables after the first table write");
}

// Frozen contract: every backend persists the same submission record shape and
// the ID namespace continues across a reopen (frozen `Storage.submission`,
// `Storage.submissionByRequest`; recovery replays committed records).
TEST_CASE("submission records persist across a close and reopen of every storage backend",
        "[durable][submission][reopen][issue1080]") {
    // Memory keeps serving the same records with no reopen (identity).
    {
        MemoryStorage storage;
        auto session = Session::create(storage);
        const ConversationId conversation_id = create_conversation(*session);
        const SubmissionRecord created = create_queued_submission(*session, conversation_id, std::string{"request-1"});
        const EntryId entry_id = append_entry(*session, conversation_id, "pi.user").id;
        auto settled = run_commit(*session, [&created, &entry_id](Transaction& transaction) -> support::Expected<void> {
            auto placed = transaction.place_submission(created.id, entry_id);
            if (!placed) {
                return std::unexpected(std::move(placed.error()));
            }
            return {};
        });
        REQUIRE(settled.has_value());

        auto by_id = storage.submission(created.id);
        REQUIRE(by_id.has_value());
        REQUIRE((*by_id).has_value());
        CHECK(**by_id == SubmissionRecord{.id = created.id,
                                 .conversation_id = conversation_id,
                                 .request_id = std::string{"request-1"},
                                 .type = "input",
                                 .status = "placed",
                                 .entry = entry_id,
                                 .answer = std::nullopt,
                                 .reason = std::nullopt,
                                 .detail = std::nullopt});
        auto by_request = storage.submission_by_request(conversation_id, "request-1");
        REQUIRE(by_request.has_value());
        REQUIRE((*by_request).has_value());
        CHECK(**by_request == **by_id);
    }

    // JSONL: close and reopen the same directory through the public seam.
    {
        TempWorkspace workspace;
        auto opened = JsonlStorage::open(workspace.path());
        REQUIRE(opened.has_value());
        std::unique_ptr<JsonlStorage> storage = std::move(*opened);
        SubmissionId created_id;
        ConversationId conversation_id;
        EntryId entry_id;
        {
            auto session = Session::create(*storage);
            conversation_id = create_conversation(*session);
            const SubmissionRecord created =
                    create_queued_submission(*session, conversation_id, std::string{"request-1"});
            created_id = created.id;
            entry_id = append_entry(*session, conversation_id, "pi.user").id;
            auto settled =
                    run_commit(*session, [&created_id, &entry_id](Transaction& transaction) -> support::Expected<void> {
                        auto placed = transaction.place_submission(created_id, entry_id);
                        if (!placed) {
                            return std::unexpected(std::move(placed.error()));
                        }
                        return {};
                    });
            REQUIRE(settled.has_value());
            REQUIRE(consume(session->close()).has_value());
        }
        auto reopened = JsonlStorage::open(workspace.path());
        REQUIRE(reopened.has_value());
        auto by_id = (*reopened)->submission(created_id);
        REQUIRE(by_id.has_value());
        REQUIRE((*by_id).has_value());
        CHECK((**by_id).id == created_id);
        CHECK((**by_id).status == "placed");
        CHECK((**by_id).entry == std::optional<EntryId>{entry_id});
        CHECK((**by_id).request_id == std::optional<std::string>{std::string{"request-1"}});
        auto by_request = (*reopened)->submission_by_request(conversation_id, "request-1");
        REQUIRE(by_request.has_value());
        REQUIRE((*by_request).has_value());
        CHECK(**by_request == **by_id);
        auto scanned = (*reopened)->scan_submissions(10);
        REQUIRE(scanned.has_value());
        REQUIRE(scanned->size() == 1);
        CHECK((*scanned)[0] == **by_id);
    }

    // SQLite: close and reopen the same database through the public seam.
    {
        TempWorkspace workspace;
        const auto path = workspace.path() / "storage.sqlite";
        auto opened = SqliteStorage::open(path);
        REQUIRE(opened.has_value());
        std::unique_ptr<SqliteStorage> storage = std::move(*opened);
        SubmissionId created_id;
        ConversationId conversation_id;
        {
            auto session = Session::create(*storage);
            conversation_id = create_conversation(*session);
            const SubmissionRecord created =
                    create_queued_submission(*session, conversation_id, std::string{"request-1"});
            created_id = created.id;
            REQUIRE(consume(session->close()).has_value());
        }
        auto reopened = SqliteStorage::open(path);
        REQUIRE(reopened.has_value());
        auto by_id = (*reopened)->submission(created_id);
        REQUIRE(by_id.has_value());
        REQUIRE((*by_id).has_value());
        CHECK((**by_id).id == created_id);
        CHECK((**by_id).status == "queued");
        CHECK((**by_id).type == "input");
        CHECK((**by_id).request_id == std::optional<std::string>{std::string{"request-1"}});
        auto by_request = (*reopened)->submission_by_request(conversation_id, "request-1");
        REQUIRE(by_request.has_value());
        REQUIRE((*by_request).has_value());
        CHECK(**by_request == **by_id);
        auto scanned = (*reopened)->scan_submissions(10);
        REQUIRE(scanned.has_value());
        REQUIRE(scanned->size() == 1);
        CHECK((*scanned)[0] == **by_id);

        // The ID namespace continues across the reopen.
        auto session = Session::create(**reopened);
        const SubmissionRecord second = create_queued_submission(*session, conversation_id, std::string{"request-2"});
        CHECK(second.id.value > created_id.value);
    }
}
