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

// Frozen contract: a kind token narrows entry reads and checks the payload
// type at append, while the stored record keeps its JSON representation.
TEST_CASE("typed entry definitions encode payloads and narrow transaction reads",
        "[durable][session][entry][typed][issue1063][spec]") {
    struct NotePayload {
        std::string text;
        bool operator==(const NotePayload&) const = default;
    };

    auto definition_result = define_entry<NotePayload>(
            "audit1063.note",
            [](const NotePayload& value) -> support::Expected<chord::Json> {
                return chord::Json{chord::Json::Object{{"text", chord::Json{value.text}}}};
            },
            [](const chord::Json& value) -> support::Expected<NotePayload> {
                const chord::Json::Object* object = value.as_object();
                if (object == nullptr) {
                    return std::unexpected(
                            support::make_error(support::ErrorCode::Validation, "Note payload must be an object"));
                }
                const auto text = object->find("text");
                if (text == object->end() || text->second.as_string() == nullptr) {
                    return std::unexpected(
                            support::make_error(support::ErrorCode::Validation, "Note payload requires text"));
                }
                return NotePayload{.text = *text->second.as_string()};
            });
    REQUIRE(definition_result.has_value());
    EntryDefinition<NotePayload> definition = std::move(*definition_result);

    MemoryStorage storage;
    auto session = Session::create(storage);
    const ConversationId conversation_id = create_conversation(*session);
    std::optional<EntryId> created_id;
    REQUIRE(run_commit(*session,
            [&definition, &conversation_id, &created_id](Transaction& transaction) -> support::Expected<void> {
                auto created = transaction.append_entry(definition, conversation_id, NotePayload{.text = "typed"});
                if (!created) {
                    return std::unexpected(std::move(created.error()));
                }
                CHECK(created->kind == "audit1063.note");
                CHECK(created->data.text == "typed");
                created_id = created->id;
                return {};
            }).has_value());
    REQUIRE(created_id.has_value());

    std::optional<TypedEntry<NotePayload>> read_entry;
    REQUIRE(run_commit(
            *session, [&definition, &created_id, &read_entry](Transaction& transaction) -> support::Expected<void> {
                auto read = transaction.entry(definition, *created_id);
                if (!read) {
                    return std::unexpected(std::move(read.error()));
                }
                read_entry = std::move(*read);
                return {};
            }).has_value());
    REQUIRE(read_entry.has_value());
    CHECK(read_entry->data.text == "typed");
    CHECK(definition.is(EntryRecord{.id = *created_id,
            .conversation_id = conversation_id,
            .kind = "audit1063.note",
            .data = chord::Json{chord::Json::Object{{"text", chord::Json{"typed"}}}}}));
    CHECK_FALSE(definition.is(
            EntryRecord{.id = *created_id, .conversation_id = conversation_id, .kind = "audit1063.other"}));

    auto invalid_definition = define_entry<NotePayload>("", {}, {});
    CHECK_FALSE(invalid_definition.has_value());

    const auto json_definition_result = define_entry("audit1063.json");
    REQUIRE(json_definition_result.has_value());
    EntryDefinition<chord::Json> json_definition = *json_definition_result;
    std::optional<EntryId> self_head_entry;
    REQUIRE(run_commit(*session,
            [&json_definition, &conversation_id, &self_head_entry](
                    Transaction& transaction) -> support::Expected<void> {
                auto appended = transaction.append_entry(json_definition,
                        conversation_id,
                        chord::Json{chord::Json::Object{{"payload", chord::Json{true}}}},
                        SelfEntryHead{});
                if (!appended) {
                    return std::unexpected(std::move(appended.error()));
                }
                self_head_entry = appended->id;
                CHECK(appended->head == self_head_entry);
                return {};
            }).has_value());
    REQUIRE(self_head_entry.has_value());
    std::optional<TypedEntry<chord::Json>> read_json_entry;
    REQUIRE(run_commit(*session,
            [&json_definition, &self_head_entry, &read_json_entry](
                    Transaction& transaction) -> support::Expected<void> {
                auto read = transaction.entry(json_definition, *self_head_entry);
                if (!read) {
                    return std::unexpected(std::move(read.error()));
                }
                read_json_entry = std::move(*read);
                return {};
            }).has_value());
    REQUIRE(read_json_entry.has_value());
    CHECK(read_json_entry->data == chord::Json{chord::Json::Object{{"payload", chord::Json{true}}}});
    CHECK(read_json_entry->head == self_head_entry);

    // An explicit EntryId head selects an existing context head, not the entry
    // being appended: the returned record and the typed read both keep it. An
    // implementation that only honours SelfEntryHead leaves the head absent
    // and fails those checks.
    std::optional<EntryId> explicit_head_entry;
    REQUIRE(run_commit(*session,
            [&json_definition, &conversation_id, &self_head_entry, &explicit_head_entry](
                    Transaction& transaction) -> support::Expected<void> {
                auto appended = transaction.append_entry(json_definition,
                        conversation_id,
                        chord::Json{chord::Json::Object{{"payload", chord::Json{false}}}},
                        EntryHeadVariant{*self_head_entry});
                if (!appended) {
                    return std::unexpected(std::move(appended.error()));
                }
                explicit_head_entry = appended->id;
                CHECK(appended->id != *self_head_entry);
                CHECK(appended->head == self_head_entry);
                return {};
            }).has_value());
    REQUIRE(explicit_head_entry.has_value());
    REQUIRE(run_commit(*session,
            [&json_definition, &self_head_entry, &explicit_head_entry](
                    Transaction& transaction) -> support::Expected<void> {
                auto read = transaction.entry(json_definition, *explicit_head_entry);
                if (!read) {
                    return std::unexpected(std::move(read.error()));
                }
                CHECK(read->has_value());
                if (read->has_value()) {
                    CHECK((*read)->head == self_head_entry);
                }
                return {};
            }).has_value());
}

// Frozen contract: a document record's `created_at` is the sequence of the
// commit that created that incarnation. Later changes and a new Session over
// the same Storage must preserve that original sequence.
TEST_CASE("a document record keeps its creation sequence across changes and a new session",
        "[durable][session][document][sequence][issue1063][spec]") {
    MemoryStorage storage;
    auto session = Session::create(storage);
    std::vector<CommitPublication> publications;
    REQUIRE(session->subscribe_commits(
            [&publications](const CommitPublication& publication) { publications.push_back(publication); }));

    const ConversationId conversation_id = create_conversation(*session);
    const auto definition = latest_document();
    const DocumentAddress address = conversation_address(conversation_id);

    REQUIRE(run_commit(*session, [&definition, &address](Transaction& transaction) -> support::Expected<void> {
        auto draft = transaction.doc(definition, address);
        if (!draft) {
            return std::unexpected(std::move(draft.error()));
        }
        return draft->set(member_path("text"), chord::Json{std::string{"created"}});
    }).has_value());
    REQUIRE(publications.size() == 2);
    CHECK(publications.back().seq == 2);
    REQUIRE(publications.back().changes.size() == 1);
    const auto* created = std::get_if<DocumentCommitChange>(&publications.back().changes.front());
    REQUIRE(created != nullptr);
    const DocumentId document_id = created->record.id;
    CHECK(created->record.created_at == 2);

    const auto update_document = [&](Session& target, std::string text) {
        return run_commit(target,
                [&definition, &address, text = std::move(text)](Transaction& transaction) -> support::Expected<void> {
                    auto draft = transaction.doc(definition, address);
                    if (!draft) {
                        return std::unexpected(std::move(draft.error()));
                    }
                    return draft->set(member_path("text"), chord::Json{std::move(text)});
                });
    };

    REQUIRE(update_document(*session, "updated").has_value());
    REQUIRE(publications.size() == 3);
    CHECK(publications.back().seq == 3);
    REQUIRE(publications.back().changes.size() == 1);
    const auto* updated = std::get_if<DocumentCommitChange>(&publications.back().changes.front());
    REQUIRE(updated != nullptr);
    CHECK(updated->record.id == document_id);
    CHECK(updated->record.created_at == 2);

    auto reopened = Session::create(storage);
    std::vector<CommitPublication> reopened_publications;
    REQUIRE(reopened->subscribe_commits([&reopened_publications](const CommitPublication& publication) {
        reopened_publications.push_back(publication);
    }));
    REQUIRE(update_document(*reopened, "reopened update").has_value());
    REQUIRE(reopened_publications.size() == 1);
    CHECK(reopened_publications.front().seq == 4);
    REQUIRE(reopened_publications.front().changes.size() == 1);
    const auto* reopened_update = std::get_if<DocumentCommitChange>(&reopened_publications.front().changes.front());
    REQUIRE(reopened_update != nullptr);
    CHECK(reopened_update->record.id == document_id);
    CHECK(reopened_update->record.created_at == 2);
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1063/session-transactions.json
// "a prepared draft is not observable before Storage settles its commit".
//
// Discriminating case: hold the public Storage commit after Session preparation,
// then read and subscribe through the public Session. Both must still expose the
// previous committed revision until the backend settles; a Session that adopts
// or publishes when the callback returns fails this case.
TEST_CASE("a prepared document draft stays invisible until storage settles its commit",
        "[durable][session][transaction][visibility][issue1063][spec]") {
    GatedStorage storage;
    auto session = Session::create(storage);
    std::vector<CommitPublication> publications;
    REQUIRE(session->subscribe_commits(
            [&publications](const CommitPublication& publication) { publications.push_back(publication); }));

    const auto definition = latest_document();
    const DocumentAddress address{};
    REQUIRE(run_commit(*session, [&definition, &address](Transaction& transaction) -> support::Expected<void> {
        auto draft = transaction.doc(definition, address);
        if (!draft) {
            return std::unexpected(std::move(draft.error()));
        }
        return draft->set(member_path("text"), chord::Json{std::string{"committed"}});
    }).has_value());
    REQUIRE(publications.size() == 1);

    storage.hold_next_commit();
    std::optional<support::Expected<void>> settled;
    auto pending_commit = session->commit([&definition, &address](Transaction& transaction) -> support::Expected<void> {
        auto draft = transaction.doc(definition, address);
        if (!draft) {
            return std::unexpected(std::move(draft.error()));
        }
        return draft->set(member_path("text"), chord::Json{std::string{"pending"}});
    });
    std::move(pending_commit).start([&settled](support::Expected<void> outcome) mutable noexcept {
        settled = std::move(outcome);
    });
    REQUIRE(storage.holds_commit());
    CHECK_FALSE(settled.has_value());

    auto before_settlement = consume(session->snapshot(definition, address));
    REQUIRE(before_settlement.has_value());
    REQUIRE(before_settlement.value().has_value());
    CHECK(*before_settlement.value() == chord::Json{chord::Json::Object{{"text", chord::Json{std::string{"committed"}}},
                                                {"revision", chord::Json{0}}}});
    CHECK(publications.size() == 1);

    storage.release_held_commit();
    REQUIRE(settled.has_value());
    CHECK(settled->has_value());
    CHECK(publications.size() == 2);

    auto after_settlement = consume(session->snapshot(definition, address));
    REQUIRE(after_settlement.has_value());
    REQUIRE(after_settlement.value().has_value());
    CHECK(*after_settlement.value() == chord::Json{chord::Json::Object{{"text", chord::Json{std::string{"pending"}}},
                                               {"revision", chord::Json{0}}}});
}

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
        auto outcome =
                session->commit([&conversation_id, &read_case](Transaction& transaction) -> support::Expected<void> {
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
    auto document_commit =
            run_commit(*session, [&conversation_id, &definition](Transaction& transaction) -> support::Expected<void> {
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
    auto entries = run_commit(*session, [&conversation_id](Transaction& transaction) -> support::Expected<void> {
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

// Frozen contract: close seals admission, settles an already-admitted storage
// commit, and only then closes the backend.
//
// Discriminating case: hold a commit after admission, request close, and release
// the storage commit. The existing batch must publish successfully before
// Storage.close; a close that closes the backend immediately fails this case.
TEST_CASE("close settles an admitted commit before closing storage",
        "[durable][session][close][transaction][issue1063][spec]") {
    GatedStorage storage;
    auto session = Session::create(storage);
    std::vector<CommitPublication> publications;
    REQUIRE(session->subscribe_commits(
            [&publications](const CommitPublication& publication) { publications.push_back(publication); }));
    std::size_t close_notifications = 0;
    REQUIRE(session->subscribe_close([&close_notifications] { close_notifications += 1; }));

    storage.hold_next_commit();
    std::optional<support::Expected<void>> commit_result;
    auto pending_commit = session->commit([](Transaction& transaction) -> support::Expected<void> {
        auto conversation = transaction.create_conversation();
        if (!conversation) {
            return std::unexpected(std::move(conversation.error()));
        }
        return {};
    });
    std::move(pending_commit).start([&commit_result](support::Expected<void> outcome) mutable noexcept {
        commit_result = std::move(outcome);
    });
    REQUIRE(storage.holds_commit());
    CHECK_FALSE(commit_result.has_value());

    std::optional<support::Expected<void>> close_result;
    auto closing = session->close();
    std::move(closing).start(
            [&close_result](support::Expected<void> outcome) mutable noexcept { close_result = std::move(outcome); });
    CHECK_FALSE(close_result.has_value());
    CHECK(storage.close_calls() == 0);
    CHECK(close_notifications == 1);
    CHECK(session->closed().reason == SessionEndReason::Open);

    auto rejected = consume(session->commit([](Transaction& transaction) -> support::Expected<void> {
        auto conversation = transaction.create_conversation();
        if (!conversation) {
            return std::unexpected(std::move(conversation.error()));
        }
        return {};
    }));
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error().message == "Session is closed");

    storage.release_held_commit();
    REQUIRE(commit_result.has_value());
    CHECK(commit_result->has_value());
    REQUIRE(close_result.has_value());
    CHECK(close_result->has_value());
    CHECK(publications.size() == 1);
    CHECK(publications.front().seq == 1);
    CHECK(storage.close_calls() == 1);
    CHECK(close_notifications == 1);
    CHECK(session->closed().reason == SessionEndReason::Closed);
}

// Frozen contract: admitted commit callbacks execute in one serial transaction line.
//
// Discriminating case: hold the first Storage commit and start a second
// transaction. Its callback must wait for the first commit to settle, so it
// reads and updates the newly committed document revision rather than losing
// the first update.
TEST_CASE("commit callbacks run in serial storage settlement order",
        "[durable][session][transaction][ordering][issue1063][spec]") {
    GatedStorage storage;
    auto session = Session::create(storage);
    const auto definition = latest_document();
    REQUIRE(run_commit(*session, [&definition](Transaction& transaction) -> support::Expected<void> {
        auto draft = transaction.doc(definition, DocumentAddress{});
        if (!draft) {
            return std::unexpected(std::move(draft.error()));
        }
        return draft->set(member_path("text"), chord::Json{std::string{"initial"}});
    }).has_value());

    storage.hold_next_commit();
    std::optional<support::Expected<void>> first_result;
    auto first = session->commit([&definition](Transaction& transaction) -> support::Expected<void> {
        auto draft = transaction.doc(definition, DocumentAddress{});
        if (!draft) {
            return std::unexpected(std::move(draft.error()));
        }
        return draft->set(member_path("text"), chord::Json{std::string{"first"}});
    });
    std::move(first).start(
            [&first_result](support::Expected<void> outcome) mutable noexcept { first_result = std::move(outcome); });
    REQUIRE(storage.holds_commit());

    bool second_callback_ran = false;
    std::optional<support::Expected<void>> second_result;
    auto second =
            session->commit([&definition, &second_callback_ran](Transaction& transaction) -> support::Expected<void> {
                second_callback_ran = true;
                auto draft = transaction.doc(definition, DocumentAddress{});
                if (!draft) {
                    return std::unexpected(std::move(draft.error()));
                }
                return draft->set(member_path("text"), chord::Json{std::string{"second"}});
            });
    std::move(second).start(
            [&second_result](support::Expected<void> outcome) mutable noexcept { second_result = std::move(outcome); });
    CHECK_FALSE(second_callback_ran);
    CHECK_FALSE(second_result.has_value());

    storage.release_held_commit();
    REQUIRE(first_result.has_value());
    CHECK(first_result->has_value());
    REQUIRE(second_result.has_value());
    CHECK(second_result->has_value());
    CHECK(second_callback_ran);
    const auto snapshot = consume(session->snapshot(definition, DocumentAddress{}));
    REQUIRE(snapshot.has_value());
    REQUIRE(snapshot.value().has_value());
    const auto* object = (*snapshot.value()).as_object();
    REQUIRE(object != nullptr);
    const auto text_member = object->find("text");
    REQUIRE(text_member != object->end());
    REQUIRE(text_member->second.as_string() != nullptr);
    CHECK(*text_member->second.as_string() == "second");
    CHECK(session->closed().reason == SessionEndReason::Open);
}

TEST_CASE("close settles queued commits in FIFO order",
        "[durable][session][close][transaction][ordering][issue1063][spec]") {
    GatedStorage storage;
    auto session = Session::create(storage);
    const auto definition = latest_document();
    std::size_t publication_count = 0;
    REQUIRE(session->subscribe_commits([&publication_count](const CommitPublication&) { publication_count += 1; }));
    REQUIRE(run_commit(*session, [&definition](Transaction& transaction) -> support::Expected<void> {
        auto draft = transaction.doc(definition, DocumentAddress{});
        if (!draft) {
            return std::unexpected(std::move(draft.error()));
        }
        return draft->set(member_path("text"), chord::Json{std::string{"initial"}});
    }).has_value());

    storage.hold_next_commit();
    std::vector<std::string> callback_order;
    std::vector<int> completion_order;
    const auto enqueue_update = [&](std::string value, int ordinal) {
        auto operation = session->commit([&callback_order, &definition, value = std::move(value)](
                                                 Transaction& transaction) -> support::Expected<void> {
            callback_order.push_back(value);
            auto draft = transaction.doc(definition, DocumentAddress{});
            if (!draft) {
                return std::unexpected(std::move(draft.error()));
            }
            return draft->set(member_path("text"), chord::Json{value});
        });
        std::move(operation).start([&completion_order, ordinal](support::Expected<void> outcome) mutable noexcept {
            CHECK(outcome.has_value());
            completion_order.push_back(ordinal);
        });
    };

    enqueue_update("first", 1);
    REQUIRE(storage.holds_commit());
    enqueue_update("second", 2);
    enqueue_update("third", 3);
    std::optional<support::Expected<void>> close_result;
    auto closing = session->close();
    std::move(closing).start(
            [&close_result](support::Expected<void> outcome) mutable noexcept { close_result = std::move(outcome); });
    CHECK(callback_order == std::vector<std::string>{"first"});
    CHECK(completion_order.empty());
    CHECK(publication_count == 1);
    CHECK(storage.commit_calls() == 2);

    storage.release_held_commit();
    REQUIRE(close_result.has_value());
    CHECK(close_result->has_value());
    CHECK(callback_order == std::vector<std::string>{"first", "second", "third"});
    CHECK(completion_order == std::vector<int>{1, 2, 3});
    CHECK(publication_count == 4);
    CHECK(storage.commit_calls() == 4);
    CHECK(storage.close_calls() == 1);
    CHECK(session->closed().reason == SessionEndReason::Closed);
}

// Discriminating case: a storage failure must not reject the commits admitted
// after the failing one before the failing commit reports, and it must reject
// them in admission order. An implementation that rejects the queue from the
// failure hook, or that skips the queue, fails this case.
TEST_CASE("a storage failure settles queued commits in admission order",
        "[durable][session][failure][transaction][ordering][issue1063][spec]") {
    GatedStorage storage;
    auto session = Session::create(storage);
    const ConversationId conversation_id = create_conversation(*session);

    storage.hold_next_commit();
    std::vector<int> completion_order;
    std::vector<std::optional<support::Expected<void>>> results(3);
    const auto enqueue = [&](int ordinal) {
        auto operation =
                session->commit([conversation_id, ordinal](Transaction& transaction) -> support::Expected<void> {
                    auto entry = transaction.append_entry(
                            conversation_id, "audit1063.note", chord::Json{static_cast<double>(ordinal)});
                    if (!entry) {
                        return std::unexpected(std::move(entry.error()));
                    }
                    return {};
                });
        std::move(operation).start(
                [&results, &completion_order, ordinal](support::Expected<void> outcome) mutable noexcept {
                    results[static_cast<std::size_t>(ordinal - 1)] = std::move(outcome);
                    completion_order.push_back(ordinal);
                });
    };

    enqueue(1);
    REQUIRE(storage.holds_commit());
    enqueue(2);
    enqueue(3);
    CHECK(completion_order.empty());

    // The second commit fails once the first one settles. The third commit was
    // admitted before that failure, so it must be rejected after the second
    // completion, not before it.
    storage.fail_next_commit("storage failed");
    storage.release_held_commit();

    REQUIRE(results[0].has_value());
    CHECK(results[0]->has_value());
    REQUIRE(results[1].has_value());
    CHECK_FALSE(results[1]->has_value());
    REQUIRE(results[2].has_value());
    CHECK_FALSE(results[2]->has_value());
    CHECK(completion_order == std::vector<int>{1, 2, 3});
    CHECK(storage.close_calls() == 1);
    CHECK(session->failed());
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
    std::optional<EntryId> committed_entry_id;
    REQUIRE(run_commit(
            *session, [&conversation_id, &committed_entry_id](Transaction& transaction) -> support::Expected<void> {
                auto entry = transaction.append_entry(conversation_id, "audit1063.note", chord::Json{1.0});
                if (!entry) {
                    return std::unexpected(std::move(entry.error()));
                }
                committed_entry_id = entry->id;
                return {};
            }).has_value());
    REQUIRE(committed_entry_id.has_value());

    // The frozen artifact records the committed entry's identity.
    const auto frozen_committed_entry_id = scenario.number_member("committedEntryId");
    REQUIRE(frozen_committed_entry_id.has_value());
    CHECK(static_cast<double>(committed_entry_id->value) == *frozen_committed_entry_id);

    const std::size_t commits_before_failure = storage.commit_calls();
    const std::size_t publications_before_failure = publications.size();

    // The failing callback stages a write and then reports an error.
    auto failure = session->commit([&conversation_id](Transaction& transaction) -> support::Expected<void> {
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
    std::optional<EntryId> later_entry_id;
    REQUIRE(run_commit(
            *session, [&conversation_id, &later_entry_id](Transaction& transaction) -> support::Expected<void> {
                auto entry = transaction.append_entry(conversation_id, "audit1063.note", chord::Json{3.0});
                if (!entry) {
                    return std::unexpected(std::move(entry.error()));
                }
                later_entry_id = entry->id;
                return {};
            }).has_value());
    REQUIRE(later_entry_id.has_value());

    // The frozen artifact records the later entry's identity: the failed
    // callback consumed an ID, so the later entry is the one after it.
    const auto frozen_later_entry_id = scenario.number_member("laterEntryId");
    REQUIRE(frozen_later_entry_id.has_value());
    CHECK(static_cast<double>(later_entry_id->value) == *frozen_later_entry_id);

    REQUIRE(publications.size() == publications_before_failure + 1);
    const auto frozen_later_seq = scenario.number_member("laterCommitSeq");
    REQUIRE(frozen_later_seq.has_value());
    CHECK(static_cast<double>(publications.back().seq) == *frozen_later_seq);

    // A fresh session over the same storage observes only the committed
    // records: the failed callback's staged entry is not among them.
    auto reopened = Session::create(storage);
    std::size_t reopened_entry_count = 0;
    std::vector<EntryId> reopened_entry_ids;
    std::optional<ConversationId> reopened_conversation_id;
    auto reopened_outcome =
            reopened->commit([&conversation_id, &reopened_conversation_id, &reopened_entry_count, &reopened_entry_ids](
                                     Transaction& transaction) -> support::Expected<void> {
                auto conversation = transaction.conversation(conversation_id);
                if (!conversation) {
                    return std::unexpected(std::move(conversation.error()));
                }
                if ((*conversation).has_value()) {
                    reopened_conversation_id = (*conversation)->id;
                }
                auto scanned = transaction.scan_entries(conversation_id, 10);
                if (!scanned) {
                    return std::unexpected(std::move(scanned.error()));
                }
                reopened_entry_count = scanned->size();
                for (const EntryRecord& record : *scanned) {
                    reopened_entry_ids.push_back(record.id);
                }
                return {};
            });
    REQUIRE(consume(std::move(reopened_outcome)).has_value());

    const auto* frozen_reopened_ids = scenario.array_member("reopenedEntryIds");
    REQUIRE(frozen_reopened_ids != nullptr);
    CHECK(static_cast<double>(reopened_entry_count) == static_cast<double>(frozen_reopened_ids->size()));

    // The frozen artifact names the exact entries the reopened session
    // observes: the committed one and the later one, in the frozen order.
    REQUIRE(reopened_entry_ids.size() == frozen_reopened_ids->size());
    for (std::size_t index = 0; index < reopened_entry_ids.size(); ++index) {
        const auto* frozen_id = frozen_reopened_ids->at(index).get_if<double>();
        REQUIRE(frozen_id != nullptr);
        CHECK(static_cast<double>(reopened_entry_ids[index].value) == *frozen_id);
    }

    // The frozen artifact records the conversation the reopened session
    // observes.
    const auto frozen_reopened_conversation_id = scenario.number_member("reopenedConversationId");
    REQUIRE(frozen_reopened_conversation_id.has_value());
    REQUIRE(reopened_conversation_id.has_value());
    CHECK(static_cast<double>(reopened_conversation_id->value) == *frozen_reopened_conversation_id);
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1063/session-transactions.json
// "a failed commit leaves the storage sequence unchanged and the session
// reopens from what was committed" (the storage-failure sub-case: "a failing
// storage commit ends the Session as failed, publishes nothing and closes its
// backend").
//
// Discriminating case: a storage commit that fails must reject its own
// commit with the STORAGE's own error (the frozen "disk vanished"), publish
// nothing beyond the already-committed batch, settle the Session as failed
// with that same message, and close the backend exactly once. An
// implementation that swallows the storage error, publishes the failed batch
// anyway, settles the Session as closed, or leaves the backend open fails
// this case.
TEST_CASE("a failing storage commit fails the session, publishes nothing and closes its backend",
        "[durable][session][failure][storage][issue1063][spec]") {
    const FrozenScenario scenario = frozen(kN3FailedCommit);

    GatedStorage storage;
    auto session = Session::create(storage);
    std::vector<CommitPublication> publications;
    std::size_t close_notifications = 0;
    REQUIRE(session->subscribe_commits(
            [&publications](const CommitPublication& publication) { publications.push_back(publication); }));
    REQUIRE(session->subscribe_close([&close_notifications] { close_notifications += 1; }));

    // The frozen artifact names the storage failure's exact error name and
    // message.
    const auto* frozen_storage_failure = scenario.object_member("storageFailure");
    REQUIRE(frozen_storage_failure != nullptr);
    const auto* frozen_failure_name = support::json_member(*frozen_storage_failure, "name");
    const auto* frozen_failure_message = support::json_member(*frozen_storage_failure, "message");
    REQUIRE(frozen_failure_name != nullptr);
    REQUIRE(frozen_failure_message != nullptr);
    const std::string expected_failure_message = frozen_failure_message->get<std::string>();

    // The committed conversation is the one publication the frozen artifact
    // counts before the failing commit.
    const ConversationId conversation_id = create_conversation(*session);
    REQUIRE(publications.size() == 1);

    // The storage fails the next commit with its own error.
    storage.fail_next_commit(expected_failure_message);

    auto failing = session->commit([&conversation_id](Transaction& transaction) -> support::Expected<void> {
        auto entry = transaction.append_entry(conversation_id, "audit1063.note", chord::Json{4.0});
        if (!entry) {
            return std::unexpected(std::move(entry.error()));
        }
        return {};
    });
    const Outcome<void> failing_terminal = consume(std::move(failing));

    // The failing commit rejects with the STORAGE's own error, not with a
    // generic Session failure: the frozen artifact records the backend's
    // message.
    REQUIRE_FALSE(failing_terminal.has_value());
    CHECK(failing_terminal.error().message == expected_failure_message);

    // The failed commit published nothing: the frozen artifact records the
    // publication count of the failed session.
    const auto frozen_failure_publications = scenario.number_member("storageFailurePublications");
    REQUIRE(frozen_failure_publications.has_value());
    CHECK(static_cast<double>(publications.size()) == *frozen_failure_publications);

    // The failed session closed its backend exactly once: the frozen artifact
    // records the close-call count.
    const auto frozen_failure_close_calls = scenario.number_member("storageFailureCloseCalls");
    REQUIRE(frozen_failure_close_calls.has_value());
    CHECK(static_cast<double>(storage.close_calls()) == *frozen_failure_close_calls);
    CHECK(close_notifications == 1);

    // The session settled as failed with the backend's own message: the
    // frozen artifact records the terminal reason and the error message.
    const auto* frozen_end = scenario.object_member("sessionEnd");
    REQUIRE(frozen_end != nullptr);
    const auto* frozen_end_reason = support::json_member(*frozen_end, "reason");
    const auto* frozen_end_message = support::json_member(*frozen_end, "errorMessage");
    REQUIRE(frozen_end_reason != nullptr);
    REQUIRE(frozen_end_message != nullptr);
    CHECK(frozen_end_reason->get<std::string>() == "failed");
    CHECK(frozen_end_message->get<std::string>() == expected_failure_message);

    CHECK(session->failed());
    CHECK(session->closed().reason == SessionEndReason::Failed);
    CHECK(session->closed().failed);
    REQUIRE(session->closed().error_message.has_value());
    CHECK(*session->closed().error_message == expected_failure_message);
    REQUIRE(session->failure_message().has_value());
    CHECK(*session->failure_message() == expected_failure_message);

    // The frozen artifact records the rejected storage failure's error name.
    CHECK(frozen_failure_name->get<std::string>() == "Error");
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
    REQUIRE(run_commit(*session, [&definition, &conversation_id](Transaction& transaction) -> support::Expected<void> {
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
    auto failure = session->commit(
            [&conversation_id, &definition, &minted_entry_id](Transaction& transaction) -> support::Expected<void> {
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
    auto scan = session->commit(
            [&conversation_id, &entries_after_failure](Transaction& transaction) -> support::Expected<void> {
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
    auto next =
            session->commit([&conversation_id, &next_entry_id](Transaction& transaction) -> support::Expected<void> {
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
    REQUIRE(run_commit(*session, [&conversation_id, &entry_id](Transaction& transaction) -> support::Expected<void> {
        auto entry = transaction.append_entry(conversation_id, "audit1063.note", chord::Json{9.0});
        if (!entry) {
            return std::unexpected(std::move(entry.error()));
        }
        entry_id = entry->id;
        return {};
    }).has_value());
    REQUIRE(entry_id.has_value());

    REQUIRE(run_commit(*session, [&definition, &conversation_id](Transaction& transaction) -> support::Expected<void> {
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
    std::optional<ConversationId> reopened_conversation;
    std::size_t reopened_entry_count = 0;
    auto reopened_outcome = reopened->commit(
            [&conversation_id, &entry_id, &reopened_conversation, &reopened_entry, &reopened_entry_count](
                    Transaction& transaction) -> support::Expected<void> {
                auto conversation = transaction.conversation(conversation_id);
                if (!conversation) {
                    return std::unexpected(std::move(conversation.error()));
                }
                if ((*conversation).has_value()) {
                    reopened_conversation = (*conversation)->id;
                }
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
    REQUIRE(reopened_conversation.has_value());

    // The frozen artifact records the conversation the reopened session
    // observes: the same conversation the first session created.
    const auto frozen_reopened_conversation_id = scenario.number_member("reopenedConversationId");
    REQUIRE(frozen_reopened_conversation_id.has_value());
    CHECK(static_cast<double>(reopened_conversation->value) == *frozen_reopened_conversation_id);
    CHECK(reopened_conversation->value == conversation_id.value);

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
    auto reopened_next = reopened->commit(
            [&conversation_id, &reopened_next_entry_id](Transaction& transaction) -> support::Expected<void> {
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
        auto outcome = session->commit(
                [&conversation_id, round, &entry_id](Transaction& transaction) -> support::Expected<void> {
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
