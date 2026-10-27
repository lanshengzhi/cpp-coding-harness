// Durable document lifecycle acceptance for issue #1081: incarnation identity
// across retire/rebuild, rewindable history with as-of reads, version
// migration, keyed family isolation, task attribution, checkpoint cuts and
// rollback boundaries, verified against the frozen pi-v1.3.0 artifacts
// (audit-1054 session transactions, audit-1057 document transactions).
//
// Every frozen-anchored assertion reads its expectation from the registered
// artifact through FrozenScenario, so a change in the frozen behavior changes
// the expectation this side must meet.

#include <cch/chord/Delta.hpp>
#include <cch/durable/JsonlStorage.hpp>
#include <cch/durable/Records.hpp>
#include <cch/durable/Session.hpp>
#include <cch/durable/Storage.hpp>
#include <cch/support/JsonAccess.hpp>

#include "durable/FrozenExpectation.hpp"
#include "durable/GatedStorage.hpp"
#include "durable/Outcome.hpp"
#include "support/TempWorkspace.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace cch;
using namespace cch::durable;
using namespace cch::tests::durable;

namespace {

constexpr std::string_view kArtifact1054 = "fixtures/pi-v1.3.0/audit-1054/session-transactions.json";
constexpr std::string_view kArtifact1057 = "fixtures/pi-v1.3.0/audit-1057/document-transaction.json";

/// One frozen expectation, loaded once per case.
[[nodiscard]] FrozenScenario frozen(std::string_view artifact, std::string_view name) {
    auto scenario = FrozenScenario::load(artifact, name);
    REQUIRE(scenario.has_value());
    return *scenario;
}

/// The path of one member of a document value.
[[nodiscard]] chord::Path member_path(std::string key) { return chord::Path{chord::path_key(std::move(key))}; }

/// The document address of a conversation-scoped document.
[[nodiscard]] DocumentAddress conversation_address(ConversationId conversation_id) {
    return DocumentAddress{.conversation_id = conversation_id};
}

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

/// One session-scoped document definition with the frozen initial shape
/// `{value, nested: {count}}`.
[[nodiscard]] DocumentDefinition session_document(std::string_view kind, std::uint32_t version = 1) {
    auto definition = define_document(
            DocumentDefinitionBuilder{std::string{kind}}.version(version), std::move_only_function<chord::Json()>{[] {
                return chord::Json{chord::Json::Object{
                        {"value", chord::Json{0.0}},
                        {"nested", chord::Json{chord::Json::Object{{"count", chord::Json{0.0}}}}},
                }};
            }});
    REQUIRE(definition.has_value());
    return std::move(*definition);
}

/// A session-scoped document definition with a migration callback (frozen pi
/// `CommonDocDefinition.migrate`).
[[nodiscard]] DocumentDefinition migrating_session_document(std::string_view kind,
        std::uint32_t version,
        std::move_only_function<chord::Json(const chord::Json&, std::uint32_t)> migrate) {
    auto definition =
            define_document(DocumentDefinitionBuilder{std::string{kind}}.version(version).migrate(std::move(migrate)),
                    std::move_only_function<chord::Json()>{[] {
                        return chord::Json{chord::Json::Object{
                                {"value", chord::Json{0.0}},
                                {"nested", chord::Json{chord::Json::Object{{"count", chord::Json{0.0}}}}},
                        }};
                    }});
    REQUIRE(definition.has_value());
    return std::move(*definition);
}

/// The single document change of one publication, when there is exactly one.
[[nodiscard]] const DocumentCommitChange* single_document_change(const CommitPublication& publication) {
    const DocumentCommitChange* found = nullptr;
    for (const auto& change : publication.changes) {
        const auto* document = std::get_if<DocumentCommitChange>(&change);
        if (document == nullptr) continue;
        REQUIRE(found == nullptr);
        found = document;
    }
    return found;
}

/// The number of document changes of one publication.
[[nodiscard]] std::size_t document_change_count(const CommitPublication& publication) {
    std::size_t count = 0;
    for (const auto& change : publication.changes) {
        if (std::get_if<DocumentCommitChange>(&change) != nullptr) {
            ++count;
        }
    }
    return count;
}

/// Commit `text` as one new revision of the conversation document, appending
/// one marker entry before it, and return the entry id.
[[nodiscard]] EntryId commit_text_revision(
        Session& session, const DocumentDefinition& definition, ConversationId conversation_id, std::string text) {
    std::optional<EntryId> appended;
    auto outcome = run_commit(session, [&](Transaction& transaction) -> support::Expected<void> {
        auto entry = transaction.append_entry(conversation_id, "audit1054.marker", chord::Json{chord::Json::Object{}});
        if (!entry) {
            return std::unexpected(std::move(entry.error()));
        }
        appended = entry->id;
        auto draft = transaction.doc(definition, conversation_address(conversation_id));
        if (!draft) {
            return std::unexpected(std::move(draft.error()));
        }
        if (auto set = draft->set(member_path("text"), chord::Json{std::move(text)}); !set) {
            return std::unexpected(std::move(set.error()));
        }
        return {};
    });
    REQUIRE(outcome.has_value());
    REQUIRE(appended.has_value());
    return *appended;
}

} // namespace

// Frozen evidence: fixtures/pi-v1.3.0/audit-1057/document-transaction.json
// "retiring a document publishes the canonical root-null batch and closes its
// watcher".
//
// Discriminating case: the retirement publication must carry no version and a
// JSON null value (frozen `Transaction.adopt`), the watch frame must be the
// canonical root-null replacement, and the watch must close with `retired`
// rather than following anything else.
TEST_CASE("retiring a document publishes the canonical root-null batch and closes its watcher",
        "[durable][session][document][retire][watch][issue1081][spec]") {
    const FrozenScenario scenario =
            frozen(kArtifact1057, "retiring a document publishes the canonical root-null batch and closes its watcher");
    MemoryStorage storage;
    auto session = Session::create(storage);
    const auto definition = session_document("1057.retirement");

    std::vector<CommitPublication> publications;
    REQUIRE(session->subscribe_commits(
            [&publications](const CommitPublication& publication) { publications.push_back(publication); }));

    REQUIRE(run_commit(*session, [&definition](Transaction& transaction) -> support::Expected<void> {
        auto draft = transaction.doc(definition, DocumentAddress{});
        if (!draft) {
            return std::unexpected(std::move(draft.error()));
        }
        return draft->set(member_path("value"), chord::Json{1.0});
    }).has_value());

    Outcome<std::optional<DocumentWatch>> watch = consume(session->watch_doc(definition, DocumentAddress{}));
    REQUIRE(watch.has_value());
    REQUIRE(watch.value().has_value());
    std::vector<DocumentFrame> frames;
    REQUIRE(watch.value().value().start([&frames](DocumentFrame frame) { frames.push_back(std::move(frame)); }));

    REQUIRE(run_commit(*session, [&definition](Transaction& transaction) -> support::Expected<void> {
        auto retired = transaction.retire_doc(definition, DocumentAddress{});
        if (!retired) {
            return std::unexpected(std::move(retired.error()));
        }
        return support::Expected<void>{};
    }).has_value());

    // The frozen publication is one document change with no version and no
    // operations.
    const auto* frozen_publications = scenario.array_member("publications");
    REQUIRE(frozen_publications != nullptr);
    REQUIRE(frozen_publications->size() == 1);
    const auto* frozen_retirement = (*frozen_publications)[0].get_if<support::JsonValue::object_t>();
    REQUIRE(frozen_retirement != nullptr);
    const auto* frozen_version = support::json_member(*frozen_retirement, "version");
    const auto* frozen_ops = support::json_member(*frozen_retirement, "ops");
    const auto* frozen_seq = support::json_member(*frozen_retirement, "seq");
    REQUIRE(frozen_version != nullptr);
    REQUIRE(frozen_ops != nullptr);
    REQUIRE(frozen_ops->get_if<support::JsonValue::array_t>() != nullptr);
    REQUIRE(frozen_seq != nullptr);

    REQUIRE(publications.size() == 2);
    const CommitPublication& retirement = publications.back();
    CHECK(retirement.seq == static_cast<std::int64_t>(frozen_seq->get<double>()));
    REQUIRE(document_change_count(retirement) == 1);
    const DocumentCommitChange* change = single_document_change(retirement);
    REQUIRE(change != nullptr);
    CHECK(change->version == std::nullopt);
    CHECK(change->ops.empty());
    CHECK(change->value.as_object() == nullptr);
    CHECK(change->value.as_array() == nullptr);
    CHECK(change->value.as_string() == nullptr);
    CHECK(change->value.as_number() == nullptr);

    // The watch receives the canonical root-null batch exactly once.
    const auto* frozen_frames = scenario.array_member("frames");
    REQUIRE(frozen_frames != nullptr);
    REQUIRE(frozen_frames->size() == 1);
    REQUIRE(frames.size() == 1);
    CHECK(frames.back().value.as_object() == nullptr);
    CHECK(frames.back().value.as_number() == nullptr);
    REQUIRE(chord::is_base(frames.back().ops));
    REQUIRE(!frames.back().ops.empty());
    REQUIRE(chord::is_replace(frames.back().ops.front()));
    const auto* replacement = std::get_if<chord::ReplaceOp>(&frames.back().ops.front());
    REQUIRE(replacement != nullptr);
    CHECK(replacement->value.as_object() == nullptr);
    CHECK(replacement->value.as_number() == nullptr);

    // The watch closed at the retirement and the address has no current
    // incarnation anymore.
    CHECK(watch.value().value().stop() == WatchEndReason::Retired);
    auto found = consume(session->watch_doc(definition, DocumentAddress{}));
    REQUIRE(found.has_value());
    CHECK(!found.value().has_value());
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1054/session-transactions.json
// "rewindable history reads an entry revision and retirement keeps it
// addressable".
//
// Discriminating case: after retirement the current incarnation is gone while
// an as-of read at the entry's commit still materializes the old revision; a
// rebuild then mints a NEW incarnation whose current value is the definition's
// initial value, without disturbing the retired history.
TEST_CASE("rewindable history reads an entry revision and retirement keeps it addressable",
        "[durable][session][document][as-of][retire][lifecycle][issue1081][spec]") {
    const FrozenScenario scenario =
            frozen(kArtifact1054, "rewindable history reads an entry revision and retirement keeps it addressable");
    MemoryStorage storage;
    auto session = Session::create(storage);

    auto rewind_result = define_document(
            DocumentDefinitionBuilder{"audit1054.rewind"}.conversation(DocumentHistory::Rewindable, DocumentFork::AsOf),
            std::move_only_function<chord::Json()>{
                    [] { return chord::Json{chord::Json::Object{{"text", chord::Json{std::string{""}}}}}; }});
    REQUIRE(rewind_result.has_value());
    const auto rewind = std::move(*rewind_result);
    const auto latest = define_document(std::string{"audit1054.latest"}, std::move_only_function<chord::Json()>{[] {
        return chord::Json{chord::Json::Object{{"text", chord::Json{std::string{""}}}}};
    }});

    std::vector<CommitPublication> publications;
    REQUIRE(session->subscribe_commits(
            [&publications](const CommitPublication& publication) { publications.push_back(publication); }));

    const ConversationId conversation_id = create_conversation(*session);
    const DocumentAddress address = conversation_address(conversation_id);

    // Two revisions, each pinned by one appended entry; the entry ids come
    // from the frozen artifact, so the ID namespace must match too: the
    // frozen run created both documents before the second entry, so the
    // second entry mints after both document ids.
    std::optional<EntryId> first_appended;
    REQUIRE(run_commit(*session, [&](Transaction& transaction) -> support::Expected<void> {
        auto entry = transaction.append_entry(conversation_id, "audit1054.marker", chord::Json{chord::Json::Object{}});
        if (!entry) {
            return std::unexpected(std::move(entry.error()));
        }
        first_appended = entry->id;
        for (const auto* definition : {&rewind, &latest}) {
            auto draft = transaction.doc(*definition, address);
            if (!draft) {
                return std::unexpected(std::move(draft.error()));
            }
            if (auto set = draft->set(member_path("text"), chord::Json{std::string{"v1"}}); !set) {
                return std::unexpected(std::move(set.error()));
            }
        }
        return {};
    }).has_value());
    REQUIRE(first_appended.has_value());
    const EntryId first_entry = *first_appended;
    const EntryId second_entry = commit_text_revision(*session, rewind, conversation_id, "v2");
    const auto frozen_first = scenario.number_member("firstEntry");
    const auto frozen_second = scenario.number_member("secondEntry");
    REQUIRE(frozen_first.has_value());
    REQUIRE(frozen_second.has_value());
    CHECK(first_entry.value == static_cast<std::int64_t>(*frozen_first));
    CHECK(second_entry.value == static_cast<std::int64_t>(*frozen_second));

    const auto current_value = [&](const DocumentDefinition& definition) -> support::Expected<chord::Json> {
        auto found = storage.find_document(definition.kind(), address, DocumentPoint::current());
        if (!found) {
            return std::unexpected(std::move(found.error()));
        }
        if (!(*found).has_value()) {
            return std::unexpected(support::make_error(support::ErrorCode::Validation, "document is absent"));
        }
        auto stored = storage.document((*found)->id, DocumentPoint::current());
        if (!stored) {
            return std::unexpected(std::move(stored.error()));
        }
        if (!(*stored).has_value()) {
            return std::unexpected(support::make_error(support::ErrorCode::Validation, "document is absent"));
        }
        return (*stored)->value;
    };

    // The current revision is v2 before retirement.
    const auto* frozen_current = scenario.member("currentBeforeRetire");
    REQUIRE(frozen_current != nullptr);
    auto before = current_value(rewind);
    REQUIRE(before.has_value());
    CHECK(json_equals(*before, *frozen_current));

    // As-of reads at each entry return that entry's revision.
    const auto* frozen_as_of_first = scenario.member("asOfFirst");
    const auto* frozen_as_of_second = scenario.member("asOfSecond");
    REQUIRE(frozen_as_of_first != nullptr);
    REQUIRE(frozen_as_of_second != nullptr);
    Outcome<std::optional<chord::Json>> as_of_first = consume(session->snapshot_as_of(rewind, address, first_entry));
    REQUIRE(as_of_first.has_value());
    REQUIRE(as_of_first.value().has_value());
    CHECK(json_equals(*as_of_first.value(), *frozen_as_of_first));
    Outcome<std::optional<chord::Json>> as_of_second = consume(session->snapshot_as_of(rewind, address, second_entry));
    REQUIRE(as_of_second.has_value());
    REQUIRE(as_of_second.value().has_value());
    CHECK(json_equals(*as_of_second.value(), *frozen_as_of_second));

    // Retirement publishes the canonical null changes for both documents in
    // one commit, matching the frozen publication exactly.
    REQUIRE(run_commit(*session, [&](Transaction& transaction) -> support::Expected<void> {
        auto retired_rewind = transaction.retire_doc(rewind, address);
        if (!retired_rewind) {
            return std::unexpected(std::move(retired_rewind.error()));
        }
        auto retired_latest = transaction.retire_doc(latest, address);
        if (!retired_latest) {
            return std::unexpected(std::move(retired_latest.error()));
        }
        return support::Expected<void>{};
    }).has_value());

    const auto* frozen_publications = scenario.array_member("publications");
    REQUIRE(frozen_publications != nullptr);
    REQUIRE(frozen_publications->size() == 1);
    const auto* frozen_retirement = (*frozen_publications)[0].get_if<support::JsonValue::object_t>();
    REQUIRE(frozen_retirement != nullptr);
    const auto* frozen_changes = support::json_array_member(*frozen_retirement, "changes");
    REQUIRE(frozen_changes != nullptr);
    REQUIRE(frozen_changes->size() == 2);
    const auto* frozen_seq = support::json_member(*frozen_retirement, "seq");
    REQUIRE(frozen_seq != nullptr);
    REQUIRE(publications.size() == 4);
    CHECK(publications.back().seq == static_cast<std::int64_t>(frozen_seq->get<double>()));
    CHECK(document_change_count(publications.back()) == 2);
    for (const auto& change_variant : publications.back().changes) {
        const auto* change = std::get_if<DocumentCommitChange>(&change_variant);
        if (change == nullptr) continue;
        CHECK(change->value.as_object() == nullptr);
        CHECK(change->value.as_number() == nullptr);
        CHECK(change->ops.empty());
        CHECK(change->version == std::nullopt);
        CHECK(change->source == std::nullopt);
    }

    // The current address is empty after retirement, while the retired
    // revision stays readable as of the older entry.
    auto after_retire = storage.find_document(rewind.kind(), address, DocumentPoint::current());
    REQUIRE(after_retire.has_value());
    CHECK(!(*after_retire).has_value());
    const auto* frozen_as_of_after = scenario.member("asOfFirstAfterRetire");
    REQUIRE(frozen_as_of_after != nullptr);
    Outcome<std::optional<chord::Json>> as_of_after = consume(session->snapshot_as_of(rewind, address, first_entry));
    REQUIRE(as_of_after.has_value());
    REQUIRE(as_of_after.value().has_value());
    CHECK(json_equals(*as_of_after.value(), *frozen_as_of_after));

    // Rebuilding the address mints a new incarnation; its current value is the
    // definition's initial value and the as-of history is untouched.
    REQUIRE(run_commit(*session, [&](Transaction& transaction) -> support::Expected<void> {
        auto draft = transaction.doc(rewind, address);
        if (!draft) {
            return std::unexpected(std::move(draft.error()));
        }
        return support::Expected<void>{};
    }).has_value());
    auto rebuilt = storage.find_document(rewind.kind(), address, DocumentPoint::current());
    REQUIRE(rebuilt.has_value());
    REQUIRE((*rebuilt).has_value());
    auto rebuilt_value = storage.document((*rebuilt)->id, DocumentPoint::current());
    REQUIRE(rebuilt_value.has_value());
    REQUIRE((*rebuilt_value).has_value());
    const chord::Json::Object* value_object = (*rebuilt_value)->value.as_object();
    REQUIRE(value_object != nullptr);
    const auto text = value_object->find("text");
    REQUIRE(text != value_object->end());
    REQUIRE(text->second.as_string() != nullptr);
    CHECK(*text->second.as_string() == "");
    Outcome<std::optional<chord::Json>> history_after_rebuild =
            consume(session->snapshot_as_of(rewind, address, first_entry));
    REQUIRE(history_after_rebuild.has_value());
    REQUIRE(history_after_rebuild.value().has_value());
    CHECK(json_equals(*history_after_rebuild.value(), *frozen_as_of_after));
}

// New frozen-aligned case (issue #1081 acceptance): retirement and rebuild in
// ONE transaction must produce the replacement incarnation in the same
// commit, applying the retirement to the old incarnation and the creation to
// the new one (frozen pi `Tx.#latestDocumentByAddress` skip-load reacquire).
TEST_CASE("one transaction retires and rebuilds an address into a replacement incarnation",
        "[durable][session][document][retire][ordering][issue1081][spec]") {
    MemoryStorage storage;
    auto session = Session::create(storage);
    const auto definition = session_document("1081.rebuild");

    std::vector<CommitPublication> publications;
    REQUIRE(session->subscribe_commits(
            [&publications](const CommitPublication& publication) { publications.push_back(publication); }));

    REQUIRE(run_commit(*session, [&definition](Transaction& transaction) -> support::Expected<void> {
        auto draft = transaction.doc(definition, DocumentAddress{});
        if (!draft) {
            return std::unexpected(std::move(draft.error()));
        }
        return draft->set(member_path("value"), chord::Json{1.0});
    }).has_value());
    REQUIRE(publications.size() == 1);
    const DocumentCommitChange* created_change = single_document_change(publications.front());
    REQUIRE(created_change != nullptr);
    const DocumentId first_incarnation = created_change->record.id;

    // Retire and immediately reacquire in one transaction.
    REQUIRE(run_commit(*session, [&definition](Transaction& transaction) -> support::Expected<void> {
        auto retired = transaction.retire_doc(definition, DocumentAddress{});
        if (!retired) {
            return std::unexpected(std::move(retired.error()));
        }
        auto draft = transaction.doc(definition, DocumentAddress{});
        if (!draft) {
            return std::unexpected(std::move(draft.error()));
        }
        return draft->set(member_path("value"), chord::Json{2.0});
    }).has_value());

    // One publication carries both the canonical retirement of the old
    // incarnation and the creation of the new one.
    REQUIRE(publications.size() == 2);
    const CommitPublication& rebuild = publications.back();
    std::optional<DocumentId> retired_incarnation;
    std::optional<DocumentId> created_incarnation;
    for (const auto& change_variant : rebuild.changes) {
        const auto* change = std::get_if<DocumentCommitChange>(&change_variant);
        if (change == nullptr) continue;
        if (change->version.has_value()) {
            REQUIRE(!created_incarnation.has_value());
            created_incarnation = change->record.id;
            const chord::Json::Object* value = change->value.as_object();
            REQUIRE(value != nullptr);
            const auto member = value->find("value");
            REQUIRE(member != value->end());
            REQUIRE(member->second.as_number() != nullptr);
            CHECK(*member->second.as_number() == 2.0);
        } else {
            REQUIRE(!retired_incarnation.has_value());
            retired_incarnation = change->record.id;
            CHECK(change->value.as_object() == nullptr);
            CHECK(change->value.as_number() == nullptr);
        }
    }
    REQUIRE(retired_incarnation.has_value());
    REQUIRE(created_incarnation.has_value());
    CHECK(*retired_incarnation == first_incarnation);
    CHECK(*created_incarnation != first_incarnation);

    // The storage's current incarnation is the replacement with the new value.
    auto found = storage.find_document(definition.kind(), DocumentAddress{}, DocumentPoint::current());
    REQUIRE(found.has_value());
    REQUIRE((*found).has_value());
    CHECK((*found)->id == *created_incarnation);
    auto stored = storage.document(*created_incarnation, DocumentPoint::current());
    REQUIRE(stored.has_value());
    REQUIRE((*stored).has_value());
    const chord::Json::Object* value = (*stored)->value.as_object();
    REQUIRE(value != nullptr);
    const auto member = value->find("value");
    REQUIRE(member != value->end());
    REQUIRE(member->second.as_number() != nullptr);
    CHECK(*member->second.as_number() == 2.0);
    CHECK((*stored)->deltas_since_base == 0);
}

// Discriminating ordering case: a watch bound to the retired incarnation ends
// at the retirement and never receives frames of the replacement incarnation,
// while a fresh watch acquires the replacement (frozen pi
// `session-watches.test.ts`: delivers retirement and does not follow
// recreation).
TEST_CASE("a retired watch ends at retirement and never follows the replacement incarnation",
        "[durable][session][document][watch][retire][ordering][issue1081][spec]") {
    MemoryStorage storage;
    auto session = Session::create(storage);
    const auto definition = session_document("1081.watch-retire");

    REQUIRE(run_commit(*session, [&definition](Transaction& transaction) -> support::Expected<void> {
        auto draft = transaction.doc(definition, DocumentAddress{});
        if (!draft) {
            return std::unexpected(std::move(draft.error()));
        }
        return draft->set(member_path("value"), chord::Json{1.0});
    }).has_value());

    Outcome<std::optional<DocumentWatch>> old_watch = consume(session->watch_doc(definition, DocumentAddress{}));
    REQUIRE(old_watch.has_value());
    REQUIRE(old_watch.value().has_value());
    std::vector<DocumentFrame> old_frames;
    REQUIRE(old_watch.value().value().start(
            [&old_frames](DocumentFrame frame) { old_frames.push_back(std::move(frame)); }));

    // Retire and rebuild in one commit, then change the replacement.
    REQUIRE(run_commit(*session, [&definition](Transaction& transaction) -> support::Expected<void> {
        auto retired = transaction.retire_doc(definition, DocumentAddress{});
        if (!retired) {
            return std::unexpected(std::move(retired.error()));
        }
        auto draft = transaction.doc(definition, DocumentAddress{});
        if (!draft) {
            return std::unexpected(std::move(draft.error()));
        }
        return support::Expected<void>{};
    }).has_value());
    REQUIRE(run_commit(*session, [&definition](Transaction& transaction) -> support::Expected<void> {
        auto draft = transaction.doc(definition, DocumentAddress{});
        if (!draft) {
            return std::unexpected(std::move(draft.error()));
        }
        return draft->set(member_path("value"), chord::Json{3.0});
    }).has_value());

    // The old watch received exactly the root-null retirement frame and ended.
    REQUIRE(old_frames.size() == 1);
    CHECK(old_frames.back().value.as_object() == nullptr);
    CHECK(old_frames.back().value.as_number() == nullptr);
    REQUIRE(chord::is_base(old_frames.back().ops));
    CHECK(old_watch.value().value().stop() == WatchEndReason::Retired);

    // A new watch on the address acquires the replacement incarnation at its
    // latest committed value.
    Outcome<std::optional<DocumentWatch>> new_watch = consume(session->watch_doc(definition, DocumentAddress{}));
    REQUIRE(new_watch.has_value());
    REQUIRE(new_watch.value().has_value());
    const chord::Json::Object* acquired = new_watch.value().value().value().as_object();
    REQUIRE(acquired != nullptr);
    const auto member = acquired->find("value");
    REQUIRE(member != acquired->end());
    REQUIRE(member->second.as_number() != nullptr);
    CHECK(*member->second.as_number() == 3.0);
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1057/document-transaction.json "a
// storage failure publishes nothing and ends the Session without a partial
// commit".
//
// Discriminating case for the document path: the failing commit staged a
// retirement plus a change; neither the publication nor the storage may show
// any effect, the watch receives no frame, and the Session ends failed.
TEST_CASE("a failing storage commit publishes nothing and retires nothing",
        "[durable][session][document][rollback][failure][issue1081][spec]") {
    const FrozenScenario scenario =
            frozen(kArtifact1057, "a storage failure publishes nothing and ends the Session without a partial commit");
    cch::tests::GatedStorage storage;
    auto session = Session::create(storage);
    const auto definition = session_document("1057.storage-failure");

    std::vector<CommitPublication> publications;
    REQUIRE(session->subscribe_commits(
            [&publications](const CommitPublication& publication) { publications.push_back(publication); }));

    // The only storage commit is the creation.
    REQUIRE(run_commit(*session, [&definition](Transaction& transaction) -> support::Expected<void> {
        auto draft = transaction.doc(definition, DocumentAddress{});
        if (!draft) {
            return std::unexpected(std::move(draft.error()));
        }
        return draft->set(member_path("value"), chord::Json{1.0});
    }).has_value());
    REQUIRE(publications.size() == 1);

    Outcome<std::optional<DocumentWatch>> watch = consume(session->watch_doc(definition, DocumentAddress{}));
    REQUIRE(watch.has_value());
    REQUIRE(watch.value().has_value());
    std::vector<DocumentFrame> frames;
    REQUIRE(watch.value().value().start([&frames](DocumentFrame frame) { frames.push_back(std::move(frame)); }));

    // The storage fails the commit that stages a change and a retirement.
    const auto* frozen_failure = scenario.object_member("commitError");
    REQUIRE(frozen_failure != nullptr);
    const auto* frozen_message = support::json_member(*frozen_failure, "message");
    REQUIRE(frozen_message != nullptr);
    storage.fail_next_commit(frozen_message->get<std::string>());
    auto failing = run_commit(*session, [&definition](Transaction& transaction) -> support::Expected<void> {
        auto draft = transaction.doc(definition, DocumentAddress{});
        if (!draft) {
            return std::unexpected(std::move(draft.error()));
        }
        if (auto set = draft->set(member_path("value"), chord::Json{2.0}); !set) {
            return std::unexpected(std::move(set.error()));
        }
        auto retired = transaction.retire_doc(definition, DocumentAddress{});
        if (!retired) {
            return std::unexpected(std::move(retired.error()));
        }
        return support::Expected<void>{};
    });
    REQUIRE(!failing.has_value());
    CHECK(failing.error().message == frozen_message->get<std::string>());

    // Nothing was published and no frame was delivered.
    CHECK(publications.size() == 1);
    CHECK(frames.empty());
    const SessionEnd end = session->closed();
    CHECK(end.reason == SessionEndReason::Failed);
    REQUIRE(end.error_message.has_value());
    CHECK(*end.error_message == frozen_message->get<std::string>());
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1057/document-transaction.json "a
// callback rollback publishes nothing and keeps the committed revision".
//
// Discriminating case: the failed callback staged real document writes; the
// rollback must publish nothing, deliver no frame, and leave the committed
// revision (and the storage commit count) untouched.
TEST_CASE("a document callback rollback publishes nothing and keeps the committed revision",
        "[durable][session][document][rollback][issue1081][spec]") {
    const FrozenScenario scenario =
            frozen(kArtifact1057, "a callback rollback publishes nothing and keeps the committed revision");
    MemoryStorage storage;
    auto session = Session::create(storage);
    const auto definition = session_document("1057.rollback");

    std::vector<CommitPublication> publications;
    REQUIRE(run_commit(*session, [&definition](Transaction& transaction) -> support::Expected<void> {
        auto draft = transaction.doc(definition, DocumentAddress{});
        if (!draft) {
            return std::unexpected(std::move(draft.error()));
        }
        return draft->set(member_path("value"), chord::Json{1.0});
    }).has_value());
    // The frozen publication window starts after the creation.
    REQUIRE(session->subscribe_commits(
            [&publications](const CommitPublication& publication) { publications.push_back(publication); }));
    Outcome<std::optional<DocumentWatch>> watch = consume(session->watch_doc(definition, DocumentAddress{}));
    REQUIRE(watch.has_value());
    REQUIRE(watch.value().has_value());
    std::vector<DocumentFrame> frames;
    REQUIRE(watch.value().value().start([&frames](DocumentFrame frame) { frames.push_back(std::move(frame)); }));
    const auto* frozen_before = scenario.member("beforeRollback");
    REQUIRE(frozen_before != nullptr);
    CHECK(json_equals(watch.value().value().value(), *frozen_before));

    // The failed callback mutates the draft and then refuses the commit.
    const auto* frozen_after_object = scenario.object_member("afterRollback");
    REQUIRE(frozen_after_object != nullptr);
    const auto* frozen_rollback = support::json_member(*frozen_after_object, "rollbackError");
    REQUIRE(frozen_rollback != nullptr);
    const auto* frozen_rollback_object = frozen_rollback->get_if<support::JsonValue::object_t>();
    REQUIRE(frozen_rollback_object != nullptr);
    const auto* frozen_message = support::json_member(*frozen_rollback_object, "message");
    REQUIRE(frozen_message != nullptr);
    const auto* frozen_message_value = frozen_message->get_if<std::string>();
    REQUIRE(frozen_message_value != nullptr);
    const std::string frozen_message_text = *frozen_message_value;
    auto failing = run_commit(
            *session, [&definition, &frozen_message_text](Transaction& transaction) -> support::Expected<void> {
                auto draft = transaction.doc(definition, DocumentAddress{});
                if (!draft) {
                    return std::unexpected(std::move(draft.error()));
                }
                if (auto set = draft->set(member_path("value"), chord::Json{99.0}); !set) {
                    return std::unexpected(std::move(set.error()));
                }
                if (auto set = draft->set(
                            chord::Path{chord::path_key("nested"), chord::path_key("count")}, chord::Json{5.0});
                        !set) {
                    return std::unexpected(std::move(set.error()));
                }
                return std::unexpected(support::make_error(support::ErrorCode::Validation, frozen_message_text));
            });
    REQUIRE(!failing.has_value());
    CHECK(failing.error().message == frozen_message_text);

    // The rollback published nothing and delivered no frame; the watch value
    // and the committed snapshot are the pre-rollback revision.
    const auto* frozen_after = scenario.object_member("afterRollback");
    REQUIRE(frozen_after != nullptr);
    const auto* frozen_publications = support::json_array_member(*frozen_after, "publications");
    const auto* frozen_frames = support::json_array_member(*frozen_after, "frames");
    REQUIRE(frozen_publications != nullptr);
    REQUIRE(frozen_frames != nullptr);
    CHECK(frozen_publications->empty());
    CHECK(publications.empty());
    CHECK(frozen_frames->empty());
    CHECK(frames.empty());
    const auto* frozen_watch_value = support::json_member(*frozen_after, "watchValue");
    REQUIRE(frozen_watch_value != nullptr);
    CHECK(json_equals(watch.value().value().value(), *frozen_watch_value));
    const auto* frozen_snapshot = support::json_member(*frozen_after, "snapshot");
    REQUIRE(frozen_snapshot != nullptr);
    auto found = storage.find_document(definition.kind(), DocumentAddress{}, DocumentPoint::current());
    REQUIRE(found.has_value());
    REQUIRE((*found).has_value());
    auto stored = storage.document((*found)->id, DocumentPoint::current());
    REQUIRE(stored.has_value());
    REQUIRE((*stored).has_value());
    CHECK(json_equals((*stored)->value, *frozen_snapshot));

    // The Session stays usable; the next commit publishes exactly the frozen
    // later revision with its frame.
    REQUIRE(run_commit(*session, [&definition](Transaction& transaction) -> support::Expected<void> {
        auto draft = transaction.doc(definition, DocumentAddress{});
        if (!draft) {
            return std::unexpected(std::move(draft.error()));
        }
        return draft->set(member_path("value"), chord::Json{2.0});
    }).has_value());
    const auto* frozen_later = scenario.object_member("afterLaterCommit");
    REQUIRE(frozen_later != nullptr);
    const auto* frozen_later_publications = support::json_array_member(*frozen_later, "publications");
    const auto* frozen_later_frames = support::json_array_member(*frozen_later, "frameValues");
    const auto* frozen_later_snapshot = support::json_member(*frozen_later, "snapshot");
    REQUIRE(frozen_later_publications != nullptr);
    REQUIRE(frozen_later_frames != nullptr);
    REQUIRE(frozen_later_snapshot != nullptr);
    REQUIRE(frozen_later_publications->size() == 1);
    const auto* frozen_seq_value = (*frozen_later_publications)[0].get_if<double>();
    REQUIRE(frozen_seq_value != nullptr);
    REQUIRE(publications.size() == 1);
    CHECK(publications.back().seq == static_cast<std::int64_t>(*frozen_seq_value));
    REQUIRE(frozen_later_frames->size() == 1);
    REQUIRE(frames.size() == 1);
    CHECK(json_equals(frames.back().value, (*frozen_later_frames)[0]));
    auto latest = storage.find_document(definition.kind(), DocumentAddress{}, DocumentPoint::current());
    REQUIRE(latest.has_value());
    REQUIRE((*latest).has_value());
    auto latest_value = storage.document((*latest)->id, DocumentPoint::current());
    REQUIRE(latest_value.has_value());
    REQUIRE((*latest_value).has_value());
    CHECK(json_equals((*latest_value)->value, *frozen_later_snapshot));
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1057/document-transaction.json "a
// watcher acquired at an older definition receives a root replacement on a
// newer-definition commit".
TEST_CASE("a watcher acquired at an older definition receives a root replacement on a newer-definition commit",
        "[durable][session][document][watch][migration][issue1081][spec]") {
    const FrozenScenario scenario = frozen(kArtifact1057,
            "a watcher acquired at an older definition receives a root replacement on a newer-definition commit");
    MemoryStorage storage;
    auto session = Session::create(storage);
    const auto first = session_document("1057.versioned", 1);
    // The version-2 definition migrates to the frozen committed value; the
    // migration commit itself prepares no operations, so the publication
    // carries the frozen empty ops with version 2.
    const auto second = migrating_session_document("1057.versioned", 2, [](const chord::Json&, std::uint32_t) {
        return chord::Json{chord::Json::Object{{"value", chord::Json{2.0}}, {"extra", chord::Json{true}}}};
    });

    std::vector<CommitPublication> publications;
    REQUIRE(session->subscribe_commits(
            [&publications](const CommitPublication& publication) { publications.push_back(publication); }));
    REQUIRE(run_commit(*session, [&first](Transaction& transaction) -> support::Expected<void> {
        auto draft = transaction.doc(first, DocumentAddress{});
        if (!draft) {
            return std::unexpected(std::move(draft.error()));
        }
        return draft->set(member_path("value"), chord::Json{1.0});
    }).has_value());
    REQUIRE(publications.size() == 1);
    const DocumentCommitChange* created_change = single_document_change(publications.front());
    REQUIRE(created_change != nullptr);
    const DocumentId document_id = created_change->record.id;

    Outcome<std::optional<DocumentWatch>> watch = consume(session->watch_doc(first, DocumentAddress{}));
    REQUIRE(watch.has_value());
    REQUIRE(watch.value().has_value());
    std::vector<DocumentFrame> frames;
    REQUIRE(watch.value().value().start([&frames](DocumentFrame frame) { frames.push_back(std::move(frame)); }));

    // The migration-only commit: acquire with the newer definition, touch
    // nothing, and let the version gap store a base (frozen pi
    // `planDocument`).
    REQUIRE(run_commit(*session, [&second](Transaction& transaction) -> support::Expected<void> {
        auto draft = transaction.doc(second, DocumentAddress{});
        if (!draft) {
            return std::unexpected(std::move(draft.error()));
        }
        return support::Expected<void>{};
    }).has_value());

    const auto* frozen_publication = scenario.object_member("publication");
    REQUIRE(frozen_publication != nullptr);
    const auto* frozen_pub_seq = support::json_member(*frozen_publication, "seq");
    const auto* frozen_pub_version = support::json_member(*frozen_publication, "version");
    const auto* frozen_pub_ops = support::json_member(*frozen_publication, "ops");
    REQUIRE(frozen_pub_seq != nullptr);
    REQUIRE(frozen_pub_version != nullptr);
    REQUIRE(frozen_pub_ops != nullptr);
    const auto* frozen_pub_ops_array = frozen_pub_ops->get_if<support::JsonValue::array_t>();
    REQUIRE(frozen_pub_ops_array != nullptr);
    REQUIRE(publications.size() == 2);
    CHECK(publications.back().seq == static_cast<std::int64_t>(frozen_pub_seq->get<double>()));
    const DocumentCommitChange* change = single_document_change(publications.back());
    REQUIRE(change != nullptr);
    REQUIRE(change->version.has_value());
    CHECK(*change->version == static_cast<std::uint32_t>(frozen_pub_version->get<double>()));
    CHECK(change->ops.size() == frozen_pub_ops_array->size());

    // The frame is the frozen root replacement carrying the committed value.
    const auto* frozen_frame = scenario.object_member("frame");
    REQUIRE(frozen_frame != nullptr);
    const auto* frozen_frame_value = support::json_member(*frozen_frame, "value");
    REQUIRE(frozen_frame_value != nullptr);
    REQUIRE(frames.size() == 1);
    CHECK(json_equals(frames.back().value, *frozen_frame_value));
    REQUIRE(chord::is_base(frames.back().ops));
    REQUIRE(chord::is_replace(frames.back().ops.front()));
    const auto* replacement = std::get_if<chord::ReplaceOp>(&frames.back().ops.front());
    REQUIRE(replacement != nullptr);
    CHECK(json_equals(replacement->value, *frozen_frame_value));

    // The newer snapshot materializes through the migration callback.
    const auto* frozen_snapshot = scenario.member("snapshotV2");
    REQUIRE(frozen_snapshot != nullptr);
    auto stored = storage.document(document_id, DocumentPoint::current());
    REQUIRE(stored.has_value());
    REQUIRE((*stored).has_value());
    CHECK((*stored)->version == 2);
    CHECK(json_equals((*stored)->value, *frozen_snapshot));

    // A structurally identical commit at the same version now delivers the
    // exact (non-replacement) operations to the upgraded observer.
    REQUIRE(run_commit(*session, [&second](Transaction& transaction) -> support::Expected<void> {
        auto draft = transaction.doc(second, DocumentAddress{});
        if (!draft) {
            return std::unexpected(std::move(draft.error()));
        }
        return draft->set(member_path("value"), chord::Json{3.0});
    }).has_value());
    REQUIRE(frames.size() == 2);
    CHECK(!chord::is_base(frames.back().ops));

    // Reading the newer stored version through the older definition is the
    // frozen typed rejection.
    const auto* frozen_rejection = scenario.object_member("olderVersionRead");
    REQUIRE(frozen_rejection != nullptr);
    auto rejected = run_commit(*session, [&first](Transaction& transaction) -> support::Expected<void> {
        auto draft = transaction.doc(first, DocumentAddress{});
        if (!draft) {
            return std::unexpected(std::move(draft.error()));
        }
        return support::Expected<void>{};
    });
    REQUIRE(!rejected.has_value());
    const std::string expected =
            "Document " + std::to_string(document_id.value) + " (1057.versioned) has newer version 2 than 1";
    CHECK(rejected.error().message == expected);
}

// Issue #1081 acceptance: migration versioning is per-definition, and an
// older stored version without a migrate callback is a typed rejection that
// never crosses into the stored content.
TEST_CASE("a definition without a migrate callback rejects an older stored version with a typed error",
        "[durable][session][document][migration][issue1081][spec]") {
    MemoryStorage storage;
    auto session = Session::create(storage);
    const auto first = session_document("1081.migration");
    const auto second = session_document("1081.migration", 2);
    const auto third = session_document("1081.migration", 3);

    std::vector<CommitPublication> publications;
    REQUIRE(session->subscribe_commits(
            [&publications](const CommitPublication& publication) { publications.push_back(publication); }));
    REQUIRE(run_commit(*session, [&first](Transaction& transaction) -> support::Expected<void> {
        auto draft = transaction.doc(first, DocumentAddress{});
        if (!draft) {
            return std::unexpected(std::move(draft.error()));
        }
        return draft->set(member_path("value"), chord::Json{1.0});
    }).has_value());
    REQUIRE(publications.size() == 1);
    const DocumentCommitChange* created_change = single_document_change(publications.front());
    REQUIRE(created_change != nullptr);
    const DocumentId document_id = created_change->record.id;

    // Version 2 without a migrate callback cannot read the version-1 value.
    auto rejected = run_commit(*session, [&second](Transaction& transaction) -> support::Expected<void> {
        auto draft = transaction.doc(second, DocumentAddress{});
        if (!draft) {
            return std::unexpected(std::move(draft.error()));
        }
        return support::Expected<void>{};
    });
    REQUIRE(!rejected.has_value());
    CHECK(rejected.error().message ==
            "Document " + std::to_string(document_id.value) + " (1081.migration) requires migration from version 1");

    // The same version 2 with a migrate callback succeeds; the migration is
    // persisted as a base even though the commit prepared no operations.
    const auto migrated = migrating_session_document("1081.migration", 2, [](const chord::Json& value, std::uint32_t) {
        const chord::Json::Object* object = value.as_object();
        if (object == nullptr) {
            return value;
        }
        chord::Json::Object migrated_object = *object;
        migrated_object["migrated"] = chord::Json{true};
        return chord::Json{std::move(migrated_object)};
    });
    REQUIRE(run_commit(*session, [&migrated](Transaction& transaction) -> support::Expected<void> {
        auto draft = transaction.doc(migrated, DocumentAddress{});
        if (!draft) {
            return std::unexpected(std::move(draft.error()));
        }
        return support::Expected<void>{};
    }).has_value());
    auto stored = storage.document(document_id, DocumentPoint::current());
    REQUIRE(stored.has_value());
    REQUIRE((*stored).has_value());
    CHECK((*stored)->version == 2);
    const chord::Json::Object* value = (*stored)->value.as_object();
    REQUIRE(value != nullptr);
    const auto flag = value->find("migrated");
    REQUIRE(flag != value->end());
    CHECK(flag->second.as_object() == nullptr);
    CHECK(flag->second.as_array() == nullptr);
    CHECK(flag->second.as_string() == nullptr);
    CHECK(flag->second.as_number() == nullptr);

    // Version 3 now requires migration from the persisted version 2.
    auto rejected_again = run_commit(*session, [&third](Transaction& transaction) -> support::Expected<void> {
        auto draft = transaction.doc(third, DocumentAddress{});
        if (!draft) {
            return std::unexpected(std::move(draft.error()));
        }
        return support::Expected<void>{};
    });
    REQUIRE(!rejected_again.has_value());
    CHECK(rejected_again.error().message ==
            "Document " + std::to_string(document_id.value) + " (1081.migration) requires migration from version 2");
}

// Issue #1081 acceptance: task attribution and scoped addressing never cross
// use: a task document of one task is invisible at another task's address, a
// dead task rejects writes with the frozen typed errors, and a mismatched
// address is rejected before any incarnation is touched.
TEST_CASE("task-scoped documents stay attributed to their task and reject dead owners",
        "[durable][session][document][task][scope][issue1081][spec]") {
    MemoryStorage storage;
    auto session = Session::create(storage);
    auto task_definition = define_document(DocumentDefinitionBuilder{"1081.taskdoc"}.task(),
            std::move_only_function<chord::Json()>{
                    [] { return chord::Json{chord::Json::Object{{"note", chord::Json{std::string{""}}}}}; }});
    REQUIRE(task_definition.has_value());
    const auto definition = std::move(*task_definition);
    const auto conversation_definition = define_document(std::string{"1081.convdoc"},
            std::move_only_function<chord::Json()>{
                    [] { return chord::Json{chord::Json::Object{{"note", chord::Json{std::string{""}}}}}; }});

    const ConversationId conversation_id = create_conversation(*session);
    const TaskId task_a{.value = 101};
    const TaskId task_b{.value = 102};
    const TaskId task_terminal{.value = 103};
    REQUIRE(run_commit(*session, [&](Transaction& transaction) -> support::Expected<void> {
        for (const TaskId id : {task_a, task_b}) {
            TaskRecord record{};
            record.id = id;
            record.conversation_id = conversation_id;
            record.kind = "1081.task";
            record.state.status = "running";
            if (auto set = transaction.set_task(record); !set) {
                return std::unexpected(std::move(set.error()));
            }
        }
        TaskRecord terminal{};
        terminal.id = task_terminal;
        terminal.conversation_id = conversation_id;
        terminal.kind = "1081.task";
        terminal.state.status = "terminal";
        return transaction.set_task(terminal);
    }).has_value());

    // Write through task A only.
    REQUIRE(run_commit(*session, [&definition, task_a](Transaction& transaction) -> support::Expected<void> {
        auto draft = transaction.doc(definition, DocumentAddress{.task_id = task_a});
        if (!draft) {
            return std::unexpected(std::move(draft.error()));
        }
        return draft->set(member_path("note"), chord::Json{std::string{"from A"}});
    }).has_value());

    // Task B's member is still the initial value; task A keeps its own.
    const auto read_note = [&](TaskId task) -> support::Expected<std::string> {
        std::optional<std::string> note;
        auto outcome = run_commit(*session, [&](Transaction& transaction) -> support::Expected<void> {
            auto draft = transaction.doc(definition, DocumentAddress{.task_id = task});
            if (!draft) {
                return std::unexpected(std::move(draft.error()));
            }
            auto value = draft->get(member_path("note"));
            if (!value) {
                return std::unexpected(std::move(value.error()));
            }
            if (!(*value).has_value() || (*value)->as_string() == nullptr) {
                return std::unexpected(support::make_error(support::ErrorCode::Validation, "note must be a string"));
            }
            note = *(*value)->as_string();
            return support::Expected<void>{};
        });
        if (!outcome.has_value()) {
            return std::unexpected(std::move(outcome.error()));
        }
        if (!note.has_value()) {
            return std::unexpected(support::make_error(support::ErrorCode::Validation, "note was not read"));
        }
        return *note;
    };
    auto note_a = read_note(task_a);
    REQUIRE(note_a.has_value());
    CHECK(*note_a == "from A");
    auto note_b = read_note(task_b);
    REQUIRE(note_b.has_value());
    CHECK(*note_b == "");

    // The frozen owner validation: unknown and terminal tasks reject writes,
    // and a scope mismatch is rejected before any incarnation is touched.
    const TaskId task_unknown{.value = 404};
    auto unknown = run_commit(*session, [&](Transaction& transaction) -> support::Expected<void> {
        auto draft = transaction.doc(definition, DocumentAddress{.task_id = task_unknown});
        if (!draft) {
            return std::unexpected(std::move(draft.error()));
        }
        return support::Expected<void>{};
    });
    REQUIRE(!unknown.has_value());
    CHECK(unknown.error().message == "Task 404 does not exist");

    auto terminal = run_commit(*session, [&](Transaction& transaction) -> support::Expected<void> {
        auto draft = transaction.doc(definition, DocumentAddress{.task_id = task_terminal});
        if (!draft) {
            return std::unexpected(std::move(draft.error()));
        }
        return support::Expected<void>{};
    });
    REQUIRE(!terminal.has_value());
    CHECK(terminal.error().message == "Task 103 is terminal");

    auto wrong_scope = run_commit(*session, [&](Transaction& transaction) -> support::Expected<void> {
        auto draft = transaction.doc(definition, conversation_address(conversation_id));
        if (!draft) {
            return std::unexpected(std::move(draft.error()));
        }
        return support::Expected<void>{};
    });
    REQUIRE(!wrong_scope.has_value());
    CHECK(wrong_scope.error().message == "Document 1081.taskdoc requires a task address");

    auto wrong_scope_other = run_commit(*session, [&](Transaction& transaction) -> support::Expected<void> {
        auto draft = transaction.doc(conversation_definition, DocumentAddress{.task_id = task_a});
        if (!draft) {
            return std::unexpected(std::move(draft.error()));
        }
        return support::Expected<void>{};
    });
    REQUIRE(!wrong_scope_other.has_value());
    CHECK(wrong_scope_other.error().message == "Document 1081.convdoc requires a conversation address");
}

// Issue #1081 acceptance: keyed families address members independently; the
// first seed initializes a member, later seeds are ignored, and the
// singleton/family key contract is enforced at the address.
TEST_CASE("keyed family members are independent incarnations under one kind",
        "[durable][session][document][family][issue1081][spec]") {
    MemoryStorage storage;
    auto session = Session::create(storage);
    auto family_result = define_document_family(
            DocumentDefinitionBuilder{"1081.family"}.conversation(DocumentHistory::Latest, DocumentFork::Current),
            std::move_only_function<chord::Json(const chord::Json&)>{
                    [](const chord::Json& seed) { return chord::Json{chord::Json::Object{{"owner", seed}}}; }});
    REQUIRE(family_result.has_value());
    const auto family = std::move(*family_result);
    const auto singleton = define_document(std::string{"1081.singleton"}, std::move_only_function<chord::Json()>{[] {
        return chord::Json{chord::Json::Object{{"owner", chord::Json{std::string{""}}}}};
    }});

    const ConversationId conversation_id = create_conversation(*session);
    const DocumentAddress first_address{
            .conversation_id = conversation_id, .task_id = std::nullopt, .key = std::string{"k1"}};
    const DocumentAddress second_address{
            .conversation_id = conversation_id, .task_id = std::nullopt, .key = std::string{"k2"}};

    // Each member initializes from its own first seed.
    const auto read_owner = [&](const DocumentAddress& address,
                                    const chord::Json& seed) -> support::Expected<std::string> {
        std::optional<std::string> owner;
        auto outcome = run_commit(*session, [&](Transaction& transaction) -> support::Expected<void> {
            auto draft = transaction.doc(family, address, seed);
            if (!draft) {
                return std::unexpected(std::move(draft.error()));
            }
            auto value = draft->get(member_path("owner"));
            if (!value) {
                return std::unexpected(std::move(value.error()));
            }
            if (!(*value).has_value() || (*value)->as_string() == nullptr) {
                return std::unexpected(support::make_error(support::ErrorCode::Validation, "owner must be a string"));
            }
            owner = *(*value)->as_string();
            return support::Expected<void>{};
        });
        if (!outcome.has_value()) {
            return std::unexpected(std::move(outcome.error()));
        }
        if (!owner.has_value()) {
            return std::unexpected(support::make_error(support::ErrorCode::Validation, "owner was not read"));
        }
        return *owner;
    };
    auto first = read_owner(first_address, chord::Json{std::string{"alice"}});
    REQUIRE(first.has_value());
    CHECK(*first == "alice");
    auto second = read_owner(second_address, chord::Json{std::string{"bob"}});
    REQUIRE(second.has_value());
    CHECK(*second == "bob");

    // An existing member ignores a later seed.
    auto reseeded = read_owner(first_address, chord::Json{std::string{"carol"}});
    REQUIRE(reseeded.has_value());
    CHECK(*reseeded == "alice");

    // The address contract: a family needs a key, a singleton rejects one.
    auto missing_key = run_commit(*session, [&](Transaction& transaction) -> support::Expected<void> {
        auto draft = transaction.doc(family, conversation_address(conversation_id), chord::Json{std::string{"x"}});
        if (!draft) {
            return std::unexpected(std::move(draft.error()));
        }
        return support::Expected<void>{};
    });
    REQUIRE(!missing_key.has_value());
    CHECK(missing_key.error().message == "Document 1081.family requires a family key");

    auto unexpected_key = run_commit(*session, [&](Transaction& transaction) -> support::Expected<void> {
        auto draft = transaction.doc(singleton, first_address);
        if (!draft) {
            return std::unexpected(std::move(draft.error()));
        }
        return support::Expected<void>{};
    });
    REQUIRE(!unexpected_key.has_value());
    CHECK(unexpected_key.error().message == "Document 1081.singleton requires no family key");

    // The no-seed overload refuses a family definition.
    auto no_seed = run_commit(*session, [&](Transaction& transaction) -> support::Expected<void> {
        auto draft = transaction.doc(family, first_address);
        if (!draft) {
            return std::unexpected(std::move(draft.error()));
        }
        return support::Expected<void>{};
    });
    REQUIRE(!no_seed.has_value());
    CHECK(no_seed.error().message == "Document 1081.family requires a family seed to create a member");
}

// Issue #1081 acceptance: the checkpoint predicate cuts an unbounded delta
// chain with a complete base; after a reopen the incarnation materializes
// from the base and the delta count restarts.
TEST_CASE("a checkpoint predicate cuts the delta chain with a complete base",
        "[durable][session][document][checkpoint][issue1081][spec]") {
    MemoryStorage storage;
    auto session = Session::create(storage);
    auto plain_result = define_document(
            DocumentDefinitionBuilder{"1081.checkpoint"}.conversation(DocumentHistory::Latest, DocumentFork::Current),
            std::move_only_function<chord::Json()>{
                    [] { return chord::Json{chord::Json::Object{{"items", chord::Json{chord::Json::Array{}}}}}; }});
    REQUIRE(plain_result.has_value());
    // A base replaces the delta chain once two deltas have accumulated after
    // the newest base (frozen pi `checkpointWhen` / `info.deltasSinceBase`).
    auto cut_result = define_document(
            DocumentDefinitionBuilder{"1081.checkpoint-cut"}
                    .conversation(DocumentHistory::Latest, DocumentFork::Current)
                    .checkpoint_when([](const chord::Json&, const chord::OpBatch&, std::int64_t deltas_since_base) {
                        return deltas_since_base >= 2;
                    }),
            std::move_only_function<chord::Json()>{
                    [] { return chord::Json{chord::Json::Object{{"items", chord::Json{chord::Json::Array{}}}}}; }});
    REQUIRE(cut_result.has_value());

    const ConversationId conversation_id = create_conversation(*session);
    const DocumentAddress plain_address = conversation_address(conversation_id);
    const DocumentAddress cut_address = conversation_address(conversation_id);

    const auto push_item = [&](const DocumentDefinition& definition, const DocumentAddress& address, std::string item) {
        auto outcome = run_commit(*session, [&](Transaction& transaction) -> support::Expected<void> {
            auto draft = transaction.doc(definition, address);
            if (!draft) {
                return std::unexpected(std::move(draft.error()));
            }
            return draft->splice(member_path("items"), 0, 0, {chord::Json{std::move(item)}});
        });
        if (!outcome.has_value()) {
            UNSCOPED_INFO(outcome.error().message);
        }
        REQUIRE(outcome.has_value());
    };

    // Create both documents (creation is a base), then four changes each.
    // With `deltas_since_base >= 2` the predicate cuts at the fourth change:
    // the loaded chain is two deltas long, so that change stores a new base.
    push_item(*plain_result, plain_address, "a");
    push_item(*cut_result, cut_address, "a");
    for (const std::string item : {"b", "c", "d", "e"}) {
        push_item(*plain_result, plain_address, item);
        push_item(*cut_result, cut_address, item);
    }

    const auto load = [&](const DocumentDefinition& definition,
                              const DocumentAddress& address) -> support::Expected<StoredDocument> {
        auto found = storage.find_document(definition.kind(), address, DocumentPoint::current());
        if (!found) {
            return std::unexpected(std::move(found.error()));
        }
        if (!(*found).has_value()) {
            return std::unexpected(support::make_error(support::ErrorCode::Validation, "document is absent"));
        }
        auto stored = storage.document((*found)->id, DocumentPoint::current());
        if (!stored) {
            return std::unexpected(std::move(stored.error()));
        }
        if (!(*stored).has_value()) {
            return std::unexpected(support::make_error(support::ErrorCode::Validation, "document is absent"));
        }
        return *std::move(*stored);
    };

    const auto plain = load(*plain_result, plain_address);
    REQUIRE(plain.has_value());
    CHECK(plain->deltas_since_base == 4);
    const auto cut = load(*cut_result, cut_address);
    REQUIRE(cut.has_value());
    // The fourth change stored a complete base, so exactly one delta follows
    // it; the values match the unchecked document.
    CHECK(cut->deltas_since_base == 1);
    CHECK(json_equals(cut->value, to_json(plain->value)));

    // The chain after the checkpoint keeps counting normally.
    push_item(*cut_result, cut_address, "f");
    const auto after = load(*cut_result, cut_address);
    REQUIRE(after.has_value());
    CHECK(after->deltas_since_base == 2);
}

// Issue #1081 acceptance: rewindable history and incarnation identity survive
// a storage reopen; the retired incarnation stays as-of addressable while the
// rebuilt one serves current reads.
TEST_CASE("rewindable history and incarnation identity survive a storage reopen",
        "[durable][session][document][as-of][persistence][issue1081][spec]") {
    const cch::tests::TempWorkspace workspace;
    std::optional<EntryId> first_entry;
    std::optional<DocumentId> original_incarnation;
    std::optional<DocumentId> replacement_incarnation;
    auto definition_result = define_document(
            DocumentDefinitionBuilder{"audit1054.rewind"}.conversation(DocumentHistory::Rewindable, DocumentFork::AsOf),
            std::move_only_function<chord::Json()>{
                    [] { return chord::Json{chord::Json::Object{{"text", chord::Json{std::string{""}}}}}; }});
    REQUIRE(definition_result.has_value());
    const auto definition = std::move(*definition_result);

    {
        auto opened = JsonlStorage::open(workspace.path());
        REQUIRE(opened.has_value());
        std::unique_ptr<JsonlStorage> storage = std::move(*opened);
        auto session = Session::create(*storage);
        const ConversationId conversation_id = create_conversation(*session);
        const DocumentAddress address = conversation_address(conversation_id);
        first_entry = commit_text_revision(*session, definition, conversation_id, "v1");
        const EntryId second_entry = commit_text_revision(*session, definition, conversation_id, "v2");
        CHECK(second_entry.value > first_entry->value);
        auto found = storage->find_document(definition.kind(), address, DocumentPoint::current());
        REQUIRE(found.has_value());
        REQUIRE((*found).has_value());
        original_incarnation = (*found)->id;
        REQUIRE(run_commit(*session, [&definition, &address](Transaction& transaction) -> support::Expected<void> {
            auto retired = transaction.retire_doc(definition, address);
            if (!retired) {
                return std::unexpected(std::move(retired.error()));
            }
            auto draft = transaction.doc(definition, address);
            if (!draft) {
                return std::unexpected(std::move(draft.error()));
            }
            return support::Expected<void>{};
        }).has_value());
        found = storage->find_document(definition.kind(), address, DocumentPoint::current());
        REQUIRE(found.has_value());
        REQUIRE((*found).has_value());
        replacement_incarnation = (*found)->id;
        CHECK(*replacement_incarnation != *original_incarnation);
        REQUIRE(consume(session->close()).has_value());
    }

    {
        auto reopened = JsonlStorage::open(workspace.path());
        REQUIRE(reopened.has_value());
        std::unique_ptr<JsonlStorage> storage = std::move(*reopened);
        auto session = Session::create(*storage);
        const ConversationId conversation_id{.value = 2};
        const DocumentAddress address = conversation_address(conversation_id);

        // The replacement serves current reads with the initial value.
        auto found = storage->find_document(definition.kind(), address, DocumentPoint::current());
        REQUIRE(found.has_value());
        REQUIRE((*found).has_value());
        CHECK((*found)->id == *replacement_incarnation);
        auto stored = storage->document(*replacement_incarnation, DocumentPoint::current());
        REQUIRE(stored.has_value());
        REQUIRE((*stored).has_value());
        const chord::Json::Object* value = (*stored)->value.as_object();
        REQUIRE(value != nullptr);
        const auto text = value->find("text");
        REQUIRE(text != value->end());
        REQUIRE(text->second.as_string() != nullptr);
        CHECK(*text->second.as_string() == "");

        // The retired incarnation stays as-of addressable after the reopen.
        Outcome<std::optional<chord::Json>> as_of = consume(session->snapshot_as_of(definition, address, *first_entry));
        REQUIRE(as_of.has_value());
        REQUIRE(as_of.value().has_value());
        const chord::Json::Object* history = as_of.value()->as_object();
        REQUIRE(history != nullptr);
        const auto text_at_entry = history->find("text");
        REQUIRE(text_at_entry != history->end());
        REQUIRE(text_at_entry->second.as_string() != nullptr);
        CHECK(*text_at_entry->second.as_string() == "v1");
    }
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1054/session-transactions.json
// "forking a conversation copies as-of and current documents and leaves initial-only members absent".
//
// Discriminating copy case: the source advances after the selected entry. The
// public Storage copy write must retain that exact source point, so the child
// contains v1 while the parent remains at v2. Copying the current parent value
// or losing the source point would fail this check.
TEST_CASE("a document copy preserves its exact as-of source point",
        "[durable][storage][document][copy][as-of][issue1081][spec]") {
    const FrozenScenario scenario = frozen(kArtifact1054,
            "forking a conversation copies as-of and current documents and leaves initial-only members absent");
    const auto* frozen_changes = scenario.array_member("copyChanges");
    REQUIRE(frozen_changes != nullptr);
    REQUIRE(frozen_changes->size() == 3);
    const auto* frozen_rewind_copy = (*frozen_changes)[1].get_if<support::JsonValue::object_t>();
    REQUIRE(frozen_rewind_copy != nullptr);
    const auto* frozen_source = support::json_member(*frozen_rewind_copy, "source");
    REQUIRE(frozen_source != nullptr);
    const auto* frozen_source_object = frozen_source->get_if<support::JsonValue::object_t>();
    REQUIRE(frozen_source_object != nullptr);
    const auto* frozen_source_point = support::json_member(*frozen_source_object, "at");
    REQUIRE(frozen_source_point != nullptr);
    CHECK(frozen_source_point->get_if<double>() != nullptr);

    MemoryStorage storage;
    auto session = Session::create(storage);
    auto definition_result = define_document(
            DocumentDefinitionBuilder{"audit1054.rewind"}.conversation(DocumentHistory::Rewindable, DocumentFork::AsOf),
            std::move_only_function<chord::Json()>{
                    [] { return chord::Json{chord::Json::Object{{"text", chord::Json{std::string{}}}}}; }});
    REQUIRE(definition_result.has_value());
    const auto definition = std::move(*definition_result);
    const ConversationId parent = create_conversation(*session);
    const DocumentAddress parent_address = conversation_address(parent);

    // Pin the v1 revision to an entry, then advance the same incarnation to v2.
    std::optional<EntryId> first_entry;
    REQUIRE(run_commit(*session, [&](Transaction& transaction) -> support::Expected<void> {
        auto entry = transaction.append_entry(parent, "audit1054.marker", chord::Json{chord::Json::Object{}});
        if (!entry) {
            return std::unexpected(std::move(entry.error()));
        }
        first_entry = entry->id;
        auto draft = transaction.doc(definition, parent_address);
        if (!draft) {
            return std::unexpected(std::move(draft.error()));
        }
        return draft->set(member_path("text"), chord::Json{std::string{"v1"}});
    }).has_value());
    REQUIRE(first_entry.has_value());
    auto parent_record = storage.find_document(definition.kind(), parent_address, DocumentPoint::current());
    REQUIRE(parent_record.has_value());
    REQUIRE((*parent_record).has_value());
    const DocumentId source_id = (*parent_record)->id;
    const auto* frozen_source_id = support::json_member(*frozen_source_object, "id");
    REQUIRE(frozen_source_id != nullptr);
    CHECK(source_id.value == static_cast<std::int64_t>(frozen_source_id->get<double>()));
    auto pinned_entry = storage.entry(*first_entry);
    REQUIRE(pinned_entry.has_value());
    REQUIRE((*pinned_entry).has_value());
    const Seq source_seq = (*pinned_entry)->commit_seq;

    REQUIRE(run_commit(*session, [&](Transaction& transaction) -> support::Expected<void> {
        auto entry = transaction.append_entry(parent, "audit1054.marker", chord::Json{chord::Json::Object{}});
        if (!entry) {
            return std::unexpected(std::move(entry.error()));
        }
        auto draft = transaction.doc(definition, parent_address);
        if (!draft) {
            return std::unexpected(std::move(draft.error()));
        }
        return draft->set(member_path("text"), chord::Json{std::string{"v2"}});
    }).has_value());

    const ConversationId child = create_conversation(*session);
    auto copied_id = storage.mint_id();
    REQUIRE(copied_id.has_value());
    const DocumentId child_id{.value = *copied_id};
    DocumentRecord copied_record{.id = child_id,
            .kind = definition.kind(),
            .conversation_id = child,
            .history = DocumentHistory::Rewindable,
            .fork = DocumentFork::AsOf};
    const std::vector<StorageWrite> writes{StorageWrite{.type = "document.copy",
            .document_record = copied_record,
            .document_copy_source = DocumentCopySource{.id = source_id, .at = DocumentPoint::as_of(source_seq)}}};
    Outcome<Seq> copy_commit = consume(storage.commit(writes));
    REQUIRE(copy_commit.has_value());
    CHECK(static_cast<double>(source_seq) == frozen_source_point->get<double>());

    auto copied = storage.find_document(definition.kind(), conversation_address(child), DocumentPoint::current());
    REQUIRE(copied.has_value());
    REQUIRE((*copied).has_value());
    CHECK((*copied)->id == child_id);
    auto copied_value = storage.document(child_id, DocumentPoint::current());
    REQUIRE(copied_value.has_value());
    REQUIRE((*copied_value).has_value());
    const chord::Json::Object* child_value = (*copied_value)->value.as_object();
    REQUIRE(child_value != nullptr);
    const auto child_text = child_value->find("text");
    REQUIRE(child_text != child_value->end());
    REQUIRE(child_text->second.as_string() != nullptr);
    CHECK(*child_text->second.as_string() == "v1");

    auto parent_value = storage.document(source_id, DocumentPoint::current());
    REQUIRE(parent_value.has_value());
    REQUIRE((*parent_value).has_value());
    const chord::Json::Object* current_parent = (*parent_value)->value.as_object();
    REQUIRE(current_parent != nullptr);
    const auto parent_text = current_parent->find("text");
    REQUIRE(parent_text != current_parent->end());
    REQUIRE(parent_text->second.as_string() != nullptr);
    CHECK(*parent_text->second.as_string() == "v2");
    CHECK(source_id != child_id);
}
