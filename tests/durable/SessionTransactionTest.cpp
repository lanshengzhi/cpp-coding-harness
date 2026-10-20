#include <cch/durable/Session.hpp>
#include <cch/durable/Storage.hpp>
#include <cch/support/JsonAccess.hpp>

#include "durable/FrozenExpectation.hpp"
#include "durable/GatedStorage.hpp"
#include "durable/Outcome.hpp"

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

/// The frozen scenario names of the `audit-1063` family.
constexpr std::string_view kN1ReadAfterWrite =
        "a table read after the first table write is rejected and the transaction still commits its document";
constexpr std::string_view kN2CloseRejects =
        "a close rejects every later session operation and closes the storage backend";
constexpr std::string_view kN3FailedCommit =
        "a failed commit leaves the storage sequence unchanged and the session reopens from what was committed";
constexpr std::string_view kN4FailedCallback =
        "a commit callback that fails after staging writes publishes nothing and leaves the session usable";
constexpr std::string_view kN5Reopen =
        "the same public operation yields the same session and transaction records on a reopen over memory storage";

/// The frozen document kind of the captured scenarios. The frozen artifact
/// names it in the published document changes.
constexpr std::string_view kLatestDocumentKind = "audit1063.latest";

/// One frozen expectation, loaded once per case.
[[nodiscard]] FrozenScenario frozen(std::string_view name) {
    auto scenario = FrozenScenario::load(name);
    REQUIRE(scenario.has_value());
    return *scenario;
}

/// The document definition of the captured scenarios: the frozen artifact's
/// document value is `{text, revision}`.
[[nodiscard]] DocumentDefinition latest_document() {
    return define_document(std::string{kLatestDocumentKind}, std::move_only_function<chord::Json()>{[] {
        return chord::Json{chord::Json::Object{{"text", chord::Json{std::string{""}}}, {"revision", chord::Json{0}}}};
    }});
}

/// The document address of a conversation-scoped document.
[[nodiscard]] DocumentAddress conversation_address(ConversationId conversation_id) {
    return DocumentAddress{.conversation_id = conversation_id};
}

/// The path of one member of a document value.
[[nodiscard]] chord::Path member_path(std::string key) { return chord::Path{chord::path_key(std::move(key))}; }

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

} // namespace

// Frozen evidence: fixtures/pi-v1.3.0/audit-1063/session-transactions.json
// "a table read after the first table write is rejected and the transaction
// still commits its document".
//
// Discriminating case: a transaction that appends an entry and then reads a
// table must be rejected with the frozen ReadAfterWrite message, and the
// staged entry must be rolled back with it. A transaction that merely logs
// the rejection, or that persists the staged entry anyway, fails this case.
TEST_CASE("a table read after the first table write is rejected and the transaction still commits its document",
        "[durable][session][transaction][read-after-write][issue1063][spec]") {
    const FrozenScenario scenario = frozen(kN1ReadAfterWrite);

    MemoryStorage storage;
    auto session = Session::create(storage);
    std::vector<CommitPublication> publications;
    REQUIRE(session->subscribe_commits(
            [&publications](const CommitPublication& publication) { publications.push_back(publication); }));

    const ConversationId conversation_id = create_conversation(*session);
    const auto definition = latest_document();

    // The frozen artifact names the rejected read's exact error name and
    // message.
    const auto* conversation_read = scenario.object_member("conversationRead");
    REQUIRE(conversation_read != nullptr);
    const auto* frozen_name = support::json_member(*conversation_read, "name");
    const auto* frozen_message = support::json_member(*conversation_read, "message");
    REQUIRE(frozen_name != nullptr);
    REQUIRE(frozen_message != nullptr);

    // The frozen template of the rejection, taken from the frozen artifact's
    // own conversation-family message.
    const std::string_view frozen_template = "() cannot read tables after the first table write";

    // Every frozen read family carries that exact message shape, including the
    // task family, whose durable task records are out of this slice's scope.
    for (const std::string_view family : {"conversationRead", "taskRead", "entryRead", "scanRead"}) {
        const auto* frozen_read = scenario.object_member(family);
        REQUIRE(frozen_read != nullptr);
        const auto* frozen_read_message = support::json_member(*frozen_read, "message");
        REQUIRE(frozen_read_message != nullptr);
        const std::string& message = frozen_read_message->get<std::string>();
        REQUIRE(message.starts_with("Tx."));
        REQUIRE(message.ends_with(frozen_template));
        CHECK(message.substr(3, message.size() - 3 - frozen_template.size()) != "");
    }

    // The read families this slice owns, each rejected after the first table
    // write with the frozen message.
    struct ReadCase {
        std::string_view frozen_family;
        std::string_view method;
    };
    constexpr ReadCase kReadCases[] = {
            {"conversationRead", "conversation"},
            {"entryRead", "entry"},
            {"scanRead", "scanConversations"},
            {"scanRead", "scanEntries"},
    };
    for (const ReadCase& read_case : kReadCases) {
        auto outcome = session->commit([&](Transaction& transaction) -> support::Expected<void> {
            auto entry = transaction.append_entry(conversation_id, "audit1063.note", chord::Json{1.0});
            if (!entry) {
                return std::unexpected(std::move(entry.error()));
            }
            if (read_case.method == "conversation") {
                auto read = transaction.conversation(conversation_id);
                if (!read) {
                    return std::unexpected(std::move(read.error()));
                }
            } else if (read_case.method == "entry") {
                auto read = transaction.entry(entry->id);
                if (!read) {
                    return std::unexpected(std::move(read.error()));
                }
            } else if (read_case.method == "scanConversations") {
                auto read = transaction.scan_conversations(10);
                if (!read) {
                    return std::unexpected(std::move(read.error()));
                }
            } else {
                auto read = transaction.scan_entries(conversation_id, 10);
                if (!read) {
                    return std::unexpected(std::move(read.error()));
                }
            }
            return {};
        });
        const Outcome<void> terminal = consume(std::move(outcome));
        REQUIRE_FALSE(terminal.has_value());
        const std::string expected_message =
                std::string{"Tx."} + std::string{read_case.method} + std::string{frozen_template};
        CHECK(terminal.error().message == expected_message);

        // The frozen artifact's own message for the families it names is the
        // same message the C++ side reports.
        if (read_case.method != "scanEntries") {
            const auto* frozen_read = scenario.object_member(read_case.frozen_family);
            REQUIRE(frozen_read != nullptr);
            const auto* frozen_read_message = support::json_member(*frozen_read, "message");
            REQUIRE(frozen_read_message != nullptr);
            CHECK(frozen_read_message->get<std::string>() == expected_message);
        }
    }

    // The frozen artifact records the rejected read's error name; the C++
    // error channel carries the same discriminator in its code.
    CHECK(frozen_name->get<std::string>() == "ReadAfterWrite");
    CHECK(frozen_message->get<std::string>() == "Tx.conversation() cannot read tables after the first table write");

    // Document access stays available after the table write, and the same
    // commit stores the document value.
    auto document_commit = run_commit(*session, [&](Transaction& transaction) -> support::Expected<void> {
        auto entry = transaction.append_entry(conversation_id, "audit1063.note", chord::Json{5.0});
        if (!entry) {
            return std::unexpected(std::move(entry.error()));
        }
        auto draft = transaction.doc(definition, conversation_address(conversation_id));
        if (!draft) {
            return std::unexpected(std::move(draft.error()));
        }
        if (auto set = draft->set(member_path("text"), chord::Json{std::string{"after-write"}}); !set) {
            return std::unexpected(std::move(set.error()));
        }
        if (auto set = draft->set(member_path("revision"), chord::Json{1.0}); !set) {
            return std::unexpected(std::move(set.error()));
        }
        return {};
    });
    REQUIRE(document_commit.has_value());

    // The frozen snapshot is the committed document value.
    const auto* frozen_snapshot = scenario.object_member("snapshot");
    REQUIRE(frozen_snapshot != nullptr);
    auto snapshot = consume(session->snapshot(definition, conversation_address(conversation_id)));
    REQUIRE(snapshot.has_value());
    REQUIRE(snapshot.value().has_value());
    CHECK(json_equals(*snapshot.value(), *frozen_snapshot));

    // The rejected reads rolled their staged entries back: the frozen artifact
    // records exactly one entry, the one of the document commit.
    auto entries = run_commit(*session, [&](Transaction& transaction) -> support::Expected<void> {
        auto scanned = transaction.scan_entries(conversation_id, 10);
        if (!scanned) {
            return std::unexpected(std::move(scanned.error()));
        }
        CHECK(scanned->size() == 1);
        return {};
    });
    REQUIRE(entries.has_value());

    // The frozen artifact records the publication count.
    const auto frozen_publication_count = scenario.number_member("publicationCount");
    REQUIRE(frozen_publication_count.has_value());
    CHECK(static_cast<double>(publications.size()) == *frozen_publication_count);
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1063/session-transactions.json
// "a close rejects every later session operation and closes the storage
// backend".
//
// Discriminating case: a close must reject every later session operation AND
// close the storage backend exactly once. An implementation that only seals
// the Session, or that closes the backend twice, fails this case.
TEST_CASE("a close rejects every later session operation and closes the storage backend",
        "[durable][session][close][issue1063][spec]") {
    const FrozenScenario scenario = frozen(kN2CloseRejects);

    GatedStorage storage;
    auto session = Session::create(storage);
    std::size_t close_notifications = 0;
    REQUIRE(session->subscribe_close([&close_notifications] { close_notifications += 1; }));

    const ConversationId conversation_id = create_conversation(*session);
    const auto definition = latest_document();

    auto closed = consume(session->close());
    REQUIRE(closed.has_value());

    // The frozen artifact names the rejected operations' exact message.
    const auto* frozen_commit = scenario.object_member("commitAfterClose");
    const auto* frozen_snapshot = scenario.object_member("snapshotAfterClose");
    REQUIRE(frozen_commit != nullptr);
    REQUIRE(frozen_snapshot != nullptr);
    const auto* frozen_commit_message = support::json_member(*frozen_commit, "message");
    const auto* frozen_snapshot_message = support::json_member(*frozen_snapshot, "message");
    REQUIRE(frozen_commit_message != nullptr);
    REQUIRE(frozen_snapshot_message != nullptr);
    const std::string expected_message = frozen_commit_message->get<std::string>();
    CHECK(expected_message == "Session is closed");
    CHECK(frozen_snapshot_message->get<std::string>() == expected_message);

    // Every later session operation is rejected.
    auto commit_after_close = session->commit([](Transaction& transaction) -> support::Expected<void> {
        auto record = transaction.create_conversation();
        if (!record) {
            return std::unexpected(std::move(record.error()));
        }
        return {};
    });
    const Outcome<void> commit_terminal = consume(std::move(commit_after_close));
    REQUIRE_FALSE(commit_terminal.has_value());
    CHECK(commit_terminal.error().message == expected_message);

    auto snapshot_after_close = session->snapshot(definition, conversation_address(conversation_id));
    const Outcome<std::optional<chord::Json>> snapshot_terminal = consume(std::move(snapshot_after_close));
    REQUIRE_FALSE(snapshot_terminal.has_value());
    CHECK(snapshot_terminal.error().message == expected_message);

    // The close listener ran exactly once, and the storage backend was closed
    // exactly once: the frozen artifact records both counts.
    const auto frozen_notifications = scenario.number_member("closeNotifications");
    const auto frozen_close_calls = scenario.number_member("storageCloseCalls");
    REQUIRE(frozen_notifications.has_value());
    REQUIRE(frozen_close_calls.has_value());
    CHECK(static_cast<double>(close_notifications) == *frozen_notifications);
    CHECK(static_cast<double>(storage.close_calls()) == *frozen_close_calls);

    // The frozen artifact records the session's terminal reason.
    const auto* frozen_end = scenario.object_member("sessionEnd");
    REQUIRE(frozen_end != nullptr);
    const auto* frozen_reason = support::json_member(*frozen_end, "reason");
    REQUIRE(frozen_reason != nullptr);
    CHECK(frozen_reason->get<std::string>() == "closed");
    CHECK_FALSE(session->failed());
    CHECK(session->closed().reason == SessionEndReason::Closed);
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1063/session-transactions.json
// "a failed commit leaves the storage sequence unchanged and the session
// reopens from what was committed".
//
// Discriminating case: the failed commit must consume no storage sequence and
// must not reach the storage at all, so the next successful commit takes the
// sequence the failed one would have taken. An implementation that consumes a
// sequence on a failed commit fails this case.
TEST_CASE("a failed commit leaves the storage sequence unchanged and the session reopens from what was committed",
        "[durable][session][failure][sequence][issue1063][spec]") {
    const FrozenScenario scenario = frozen(kN3FailedCommit);

    GatedStorage storage;
    auto session = Session::create(storage);
    std::vector<CommitPublication> publications;
    REQUIRE(session->subscribe_commits(
            [&publications](const CommitPublication& publication) { publications.push_back(publication); }));

    const ConversationId conversation_id = create_conversation(*session);
    REQUIRE(run_commit(*session, [&](Transaction& transaction) -> support::Expected<void> {
        auto entry = transaction.append_entry(conversation_id, "audit1063.note", chord::Json{1.0});
        if (!entry) {
            return std::unexpected(std::move(entry.error()));
        }
        return {};
    }).has_value());

    const std::size_t commits_before_failure = storage.commit_calls();
    const std::size_t publications_before_failure = publications.size();

    // The failing callback stages a write and then reports an error.
    auto failure = session->commit([&](Transaction& transaction) -> support::Expected<void> {
        auto entry = transaction.append_entry(conversation_id, "audit1063.note", chord::Json{2.0});
        if (!entry) {
            return std::unexpected(std::move(entry.error()));
        }
        return std::unexpected(support::make_error(support::ErrorCode::Session, "callback failed"));
    });
    const Outcome<void> failure_terminal = consume(std::move(failure));
    REQUIRE_FALSE(failure_terminal.has_value());
    CHECK(failure_terminal.error().message == "callback failed");

    // The failed callback never reached the storage and published nothing.
    const auto* frozen_failure = scenario.object_member("failure");
    REQUIRE(frozen_failure != nullptr);
    const auto* frozen_failure_message = support::json_member(*frozen_failure, "message");
    REQUIRE(frozen_failure_message != nullptr);
    CHECK(frozen_failure_message->get<std::string>() == failure_terminal.error().message);

    const auto frozen_commits_before = scenario.number_member("storageCommitCallsBeforeFailure");
    const auto frozen_commits_after = scenario.number_member("storageCommitCallsAfterFailure");
    REQUIRE(frozen_commits_before.has_value());
    REQUIRE(frozen_commits_after.has_value());
    CHECK(static_cast<double>(commits_before_failure) == *frozen_commits_before);
    CHECK(static_cast<double>(storage.commit_calls()) == *frozen_commits_after);
    CHECK(publications.size() == publications_before_failure);

    // The next successful commit takes the sequence the failed one would have
    // taken: the frozen artifact records that sequence.
    REQUIRE(run_commit(*session, [&](Transaction& transaction) -> support::Expected<void> {
        auto entry = transaction.append_entry(conversation_id, "audit1063.note", chord::Json{3.0});
        if (!entry) {
            return std::unexpected(std::move(entry.error()));
        }
        return {};
    }).has_value());
    REQUIRE(publications.size() == publications_before_failure + 1);
    const auto frozen_later_seq = scenario.number_member("laterCommitSeq");
    REQUIRE(frozen_later_seq.has_value());
    CHECK(static_cast<double>(publications.back().seq) == *frozen_later_seq);

    // A fresh session over the same storage observes only the committed
    // records: the failed callback's staged entry is not among them.
    auto reopened = Session::create(storage);
    std::size_t reopened_entry_count = 0;
    auto reopened_outcome = reopened->commit([&](Transaction& transaction) -> support::Expected<void> {
        auto scanned = transaction.scan_entries(conversation_id, 10);
        if (!scanned) {
            return std::unexpected(std::move(scanned.error()));
        }
        reopened_entry_count = scanned->size();
        return {};
    });
    REQUIRE(consume(std::move(reopened_outcome)).has_value());

    const auto* frozen_reopened_ids = scenario.array_member("reopenedEntryIds");
    REQUIRE(frozen_reopened_ids != nullptr);
    CHECK(static_cast<double>(reopened_entry_count) == static_cast<double>(frozen_reopened_ids->size()));
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1063/session-transactions.json
// "a commit callback that fails after staging writes publishes nothing and
// leaves the session usable".
//
// Discriminating case: the staged-but-not-committed write must leave no trace
// (no entry, no document change, no publication), and the session must still
// accept a later commit whose entry ID is higher than the failed callback's
// consumed ID. An implementation that persists the partial batch, or that
// reuses the failed callback's ID, fails this case.
TEST_CASE("a commit callback that fails after staging writes publishes nothing and leaves the session usable",
        "[durable][session][failure][rollback][issue1063][spec]") {
    const FrozenScenario scenario = frozen(kN4FailedCallback);

    MemoryStorage storage;
    auto session = Session::create(storage);
    std::vector<CommitPublication> publications;
    REQUIRE(session->subscribe_commits(
            [&publications](const CommitPublication& publication) { publications.push_back(publication); }));

    const ConversationId conversation_id = create_conversation(*session);
    const auto definition = latest_document();

    // The committed baseline the failing callback must not disturb.
    REQUIRE(run_commit(*session, [&](Transaction& transaction) -> support::Expected<void> {
        auto draft = transaction.doc(definition, conversation_address(conversation_id));
        if (!draft) {
            return std::unexpected(std::move(draft.error()));
        }
        if (auto set = draft->set(member_path("text"), chord::Json{std::string{"baseline"}}); !set) {
            return std::unexpected(std::move(set.error()));
        }
        if (auto set = draft->set(member_path("revision"), chord::Json{1.0}); !set) {
            return std::unexpected(std::move(set.error()));
        }
        return {};
    }).has_value());

    auto snapshot_before = consume(session->snapshot(definition, conversation_address(conversation_id)));
    REQUIRE(snapshot_before.has_value());
    REQUIRE(snapshot_before.value().has_value());
    const std::size_t publications_before = publications.size();

    // The failing callback stages an entry and mutates the committed document,
    // then reports an error.
    std::optional<EntryId> minted_entry_id;
    auto failure = session->commit([&](Transaction& transaction) -> support::Expected<void> {
        auto entry = transaction.append_entry(conversation_id, "audit1063.note", chord::Json{7.0});
        if (!entry) {
            return std::unexpected(std::move(entry.error()));
        }
        minted_entry_id = entry->id;
        auto draft = transaction.doc(definition, conversation_address(conversation_id));
        if (!draft) {
            return std::unexpected(std::move(draft.error()));
        }
        if (auto set = draft->set(member_path("text"), chord::Json{std::string{"lost"}}); !set) {
            return std::unexpected(std::move(set.error()));
        }
        if (auto set = draft->set(member_path("revision"), chord::Json{2.0}); !set) {
            return std::unexpected(std::move(set.error()));
        }
        return std::unexpected(support::make_error(support::ErrorCode::Session, "callback failed"));
    });
    const Outcome<void> failure_terminal = consume(std::move(failure));
    REQUIRE_FALSE(failure_terminal.has_value());
    CHECK(failure_terminal.error().message == "callback failed");
    REQUIRE(minted_entry_id.has_value());

    // The staged entry was not persisted.
    std::size_t entries_after_failure = 0;
    auto scan = session->commit([&](Transaction& transaction) -> support::Expected<void> {
        auto scanned = transaction.scan_entries(conversation_id, 10);
        if (!scanned) {
            return std::unexpected(std::move(scanned.error()));
        }
        entries_after_failure = scanned->size();
        return {};
    });
    REQUIRE(consume(std::move(scan)).has_value());

    const auto* frozen_entries = scenario.array_member("entriesAfterFailure");
    REQUIRE(frozen_entries != nullptr);
    CHECK(entries_after_failure == frozen_entries->size());

    // The committed document value is unchanged.
    auto snapshot_after = consume(session->snapshot(definition, conversation_address(conversation_id)));
    REQUIRE(snapshot_after.has_value());
    REQUIRE(snapshot_after.value().has_value());
    const auto* frozen_snapshot = scenario.object_member("snapshotBefore");
    REQUIRE(frozen_snapshot != nullptr);
    CHECK(json_equals(*snapshot_after.value(), *frozen_snapshot));

    // Nothing was published.
    const auto frozen_publication_count = scenario.number_member("publicationCountAfterFailure");
    REQUIRE(frozen_publication_count.has_value());
    CHECK(static_cast<double>(publications.size()) == *frozen_publication_count);
    CHECK(publications.size() == publications_before);

    // The session is still usable, and the next commit mints the next ID: the
    // failed callback's ID was consumed but not stored.
    std::optional<EntryId> next_entry_id;
    auto next = session->commit([&](Transaction& transaction) -> support::Expected<void> {
        auto entry = transaction.append_entry(conversation_id, "audit1063.note", chord::Json{8.0});
        if (!entry) {
            return std::unexpected(std::move(entry.error()));
        }
        next_entry_id = entry->id;
        return {};
    });
    REQUIRE(consume(std::move(next)).has_value());
    REQUIRE(next_entry_id.has_value());
    CHECK(next_entry_id->value > minted_entry_id->value);

    const auto frozen_minted = scenario.number_member("mintedEntryId");
    const auto frozen_next = scenario.number_member("nextEntryId");
    REQUIRE(frozen_minted.has_value());
    REQUIRE(frozen_next.has_value());
    CHECK(static_cast<double>(minted_entry_id->value) == *frozen_minted);
    CHECK(static_cast<double>(next_entry_id->value) == *frozen_next);
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1063/session-transactions.json
// "the same public operation yields the same session and transaction records
// on a reopen over memory storage".
//
// Discriminating case: the second session over the same storage observes the
// same records and the same commit sequence per entry, and the ID namespace
// continues above every previously minted or stored ID. An implementation that
// restarts the ID namespace, or that loses the entry's commit sequence, fails
// this case.
TEST_CASE("the same public operation yields the same session and transaction records on a reopen over memory storage",
        "[durable][session][reopen][identity][issue1063][spec]") {
    const FrozenScenario scenario = frozen(kN5Reopen);

    MemoryStorage storage;
    auto session = Session::create(storage);
    const ConversationId conversation_id = create_conversation(*session);
    const auto definition = latest_document();

    std::optional<EntryId> entry_id;
    REQUIRE(run_commit(*session, [&](Transaction& transaction) -> support::Expected<void> {
        auto entry = transaction.append_entry(conversation_id, "audit1063.note", chord::Json{9.0});
        if (!entry) {
            return std::unexpected(std::move(entry.error()));
        }
        entry_id = entry->id;
        return {};
    }).has_value());
    REQUIRE(entry_id.has_value());

    REQUIRE(run_commit(*session, [&](Transaction& transaction) -> support::Expected<void> {
        auto draft = transaction.doc(definition, conversation_address(conversation_id));
        if (!draft) {
            return std::unexpected(std::move(draft.error()));
        }
        if (auto set = draft->set(member_path("text"), chord::Json{std::string{"kept"}}); !set) {
            return std::unexpected(std::move(set.error()));
        }
        if (auto set = draft->set(member_path("revision"), chord::Json{4.0}); !set) {
            return std::unexpected(std::move(set.error()));
        }
        return {};
    }).has_value());

    // The frozen artifact records the entry's commit sequence.
    const auto stored_entry = storage.entry(*entry_id);
    REQUIRE(stored_entry.has_value());
    REQUIRE((*stored_entry).has_value());
    const auto frozen_commit_seq = scenario.number_member("entryCommitSeq");
    REQUIRE(frozen_commit_seq.has_value());
    CHECK(static_cast<double>((*stored_entry)->commit_seq) == *frozen_commit_seq);

    // The second session over the same storage observes the same records.
    auto reopened = Session::create(storage);
    std::optional<EntryRecord> reopened_entry;
    std::size_t reopened_entry_count = 0;
    auto reopened_outcome = reopened->commit([&](Transaction& transaction) -> support::Expected<void> {
        auto entry = transaction.entry(*entry_id);
        if (!entry) {
            return std::unexpected(std::move(entry.error()));
        }
        if ((*entry).has_value()) {
            reopened_entry = *(*entry);
        }
        auto scanned = transaction.scan_entries(conversation_id, 10);
        if (!scanned) {
            return std::unexpected(std::move(scanned.error()));
        }
        reopened_entry_count = scanned->size();
        return {};
    });
    REQUIRE(consume(std::move(reopened_outcome)).has_value());
    REQUIRE(reopened_entry.has_value());

    const auto frozen_entry_id = scenario.number_member("entryId");
    const auto frozen_entry_kind = scenario.string_member("entryKind");
    const auto frozen_reopened_entry_id = scenario.number_member("reopenedEntryId");
    const auto frozen_reopened_entry_kind = scenario.string_member("reopenedEntryKind");
    const auto frozen_reopened_count = scenario.number_member("reopenedEntryCount");
    REQUIRE(frozen_entry_id.has_value());
    REQUIRE(frozen_entry_kind.has_value());
    REQUIRE(frozen_reopened_entry_id.has_value());
    REQUIRE(frozen_reopened_entry_kind.has_value());
    REQUIRE(frozen_reopened_count.has_value());

    CHECK(static_cast<double>(entry_id->value) == *frozen_entry_id);
    CHECK(reopened_entry->kind == *frozen_entry_kind);
    CHECK(static_cast<double>(reopened_entry->id.value) == *frozen_reopened_entry_id);
    CHECK(reopened_entry->kind == *frozen_reopened_entry_kind);
    CHECK(static_cast<double>(reopened_entry_count) == *frozen_reopened_count);

    // The second session observes the same committed document value.
    auto reopened_snapshot = consume(reopened->snapshot(definition, conversation_address(conversation_id)));
    REQUIRE(reopened_snapshot.has_value());
    REQUIRE(reopened_snapshot.value().has_value());
    const auto* frozen_snapshot = scenario.object_member("reopenedSnapshot");
    REQUIRE(frozen_snapshot != nullptr);
    CHECK(json_equals(*reopened_snapshot.value(), *frozen_snapshot));

    // The ID namespace continues above every previously minted or stored ID.
    std::optional<EntryId> reopened_next_entry_id;
    auto reopened_next = reopened->commit([&](Transaction& transaction) -> support::Expected<void> {
        auto entry = transaction.append_entry(conversation_id, "audit1063.note", chord::Json{10.0});
        if (!entry) {
            return std::unexpected(std::move(entry.error()));
        }
        reopened_next_entry_id = entry->id;
        return {};
    });
    REQUIRE(consume(std::move(reopened_next)).has_value());
    REQUIRE(reopened_next_entry_id.has_value());
    CHECK(reopened_next_entry_id->value > entry_id->value);

    const auto frozen_reopened_next = scenario.number_member("reopenedNextEntryId");
    REQUIRE(frozen_reopened_next.has_value());
    CHECK(static_cast<double>(reopened_next_entry_id->value) == *frozen_reopened_next);
}

// A commit publishes one sequence and mints increasing ids.
//
// Discriminating case: the publication sequence must strictly increase per
// adopted batch, the minted IDs must strictly increase, and the entry's
// commit sequence must equal the publication's sequence. An implementation
// that reuses a sequence, reuses an ID, or loses the entry's commit sequence
// fails this case.
TEST_CASE("a commit publishes one sequence and mints increasing ids",
        "[durable][session][identity][sequence][issue1063][spec]") {
    MemoryStorage storage;
    auto session = Session::create(storage);
    std::vector<CommitPublication> publications;
    REQUIRE(session->subscribe_commits(
            [&publications](const CommitPublication& publication) { publications.push_back(publication); }));

    const ConversationId conversation_id = create_conversation(*session);

    Seq previous_seq = 0;
    std::int64_t previous_id = 0;
    for (int round = 0; round < 3; ++round) {
        std::optional<EntryId> entry_id;
        auto outcome = session->commit([&](Transaction& transaction) -> support::Expected<void> {
            auto entry = transaction.append_entry(conversation_id, "audit1063.note", chord::Json{round});
            if (!entry) {
                return std::unexpected(std::move(entry.error()));
            }
            entry_id = entry->id;
            return {};
        });
        REQUIRE(consume(std::move(outcome)).has_value());
        REQUIRE(entry_id.has_value());

        REQUIRE(publications.size() == static_cast<std::size_t>(round) + 2);
        const Seq seq = publications.back().seq;
        CHECK(seq > previous_seq);
        CHECK(entry_id->value > previous_id);

        // The entry's commit sequence is the publication's sequence.
        const auto stored_entry = storage.entry(*entry_id);
        REQUIRE(stored_entry.has_value());
        REQUIRE((*stored_entry).has_value());
        CHECK((*stored_entry)->commit_seq == seq);

        previous_seq = seq;
        previous_id = entry_id->value;
    }
}
