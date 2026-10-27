#include <cch/durable/JsonlStorage.hpp>
#include <cch/durable/Session.hpp>
#include <cch/durable/Storage.hpp>
#include <cch/support/JsonAccess.hpp>

#include "durable/FrozenExpectation.hpp"
#include "durable/Outcome.hpp"
#include "support/Json.hpp"
#include "support/TempWorkspace.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
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

/// The frozen JSONL artifact of the `audit-1054` family.
constexpr std::string_view kJsonlArtifact = "fixtures/pi-v1.3.0/audit-1054/jsonl-store.json";

constexpr std::string_view kOrderScenario = "a JSONL commit appends every sidecar before the main marker";
constexpr std::string_view kRecoveryScenario =
        "JSONL recovery truncates a torn main marker and an unconfirmed sidecar tail";
constexpr std::string_view kMissingScenario = "JSONL refuses a main marker whose confirmed sidecar record is missing";
constexpr std::string_view kPoisonScenario =
        "a JSONL main-marker I/O failure poisons the instance while reopen recovers the published prefix";
constexpr std::string_view kCheckpointScenario =
        "a JSONL checkpoint replaces a current-only sidecar and retirement removes it";

/// The document definition of the captured scenarios: `{text, revision}`.
[[nodiscard]] DocumentDefinition latest_document() {
    return define_document(std::string{"audit1054.latest"}, std::move_only_function<chord::Json()>{[] {
        return chord::Json{chord::Json::Object{{"text", chord::Json{std::string{""}}}, {"revision", chord::Json{0}}}};
    }});
}

[[nodiscard]] chord::Path member_path(std::string key) { return chord::Path{chord::path_key(std::move(key))}; }

[[nodiscard]] Outcome<void> run_commit(
        Session& session, std::move_only_function<support::Expected<void>(Transaction&)> change) {
    return consume(session.commit(std::move(change)));
}

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
    if (!outcome.has_value()) {
        FAIL(outcome.error().message);
    }
    REQUIRE(outcome.has_value());
    REQUIRE(created.has_value());
    return *created;
}

[[nodiscard]] std::vector<support::JsonValue> json_lines(std::string_view text) {
    std::vector<support::JsonValue> lines;
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t end = text.find('\n', start);
        REQUIRE(end != std::string_view::npos);
        auto parsed = support::read_json(text.substr(start, end - start));
        REQUIRE(parsed.has_value());
        lines.push_back(std::move(*parsed));
        start = end + 1;
    }
    return lines;
}

[[nodiscard]] std::string json_text(const support::JsonValue& value) {
    auto text = support::write_json(value);
    REQUIRE(text.has_value());
    return *text;
}

/// Canonical comparison text for the frozen `audit-1054` records. The frozen
/// artifacts are compared by value, not by their member order, so the
/// comparison normalizes explicitly instead of relying on a container order
/// (#1149).
[[nodiscard]] std::string canonical_text(const support::JsonValue& value) {
    auto text = support::canonical_json(value);
    REQUIRE(text.has_value());
    return *text;
}

[[nodiscard]] support::JsonValue json_object(
        std::initializer_list<std::pair<std::string, support::JsonValue>> members) {
    support::JsonValue::object_t object;
    for (const auto& [key, value] : members) {
        object.emplace(key, value);
    }
    return support::JsonValue{std::move(object)};
}

void require_files_match(const TempWorkspace& workspace, const support::JsonValue::array_t& expected_files) {
    std::vector<std::string> actual_names;
    for (const auto& entry : std::filesystem::directory_iterator(workspace.path())) {
        const std::string name = entry.path().filename().string();
        if (name.ends_with(".jsonl")) {
            actual_names.push_back(name);
        }
    }
    std::vector<std::string> expected_names;
    for (const support::JsonValue& file_value : expected_files) {
        const auto* file = file_value.get_if<support::JsonValue::object_t>();
        REQUIRE(file != nullptr);
        const auto name = support::json_string_member(*file, "name");
        const auto* lines = support::json_array_member(*file, "lines");
        REQUIRE(name.has_value());
        REQUIRE(lines != nullptr);
        expected_names.emplace_back(*name);

        const auto actual_lines = json_lines(workspace.read(std::string{*name}));
        REQUIRE(actual_lines.size() == lines->size());
        for (std::size_t index = 0; index < lines->size(); ++index) {
            CHECK(canonical_text(actual_lines[index]) == canonical_text((*lines)[index]));
        }
    }
    std::sort(expected_names.begin(), expected_names.end());
    std::sort(actual_names.begin(), actual_names.end());
    CHECK(actual_names == expected_names);
}

void append_text(const std::filesystem::path& path, std::string_view text) {
    std::ofstream stream(path, std::ios::binary | std::ios::app);
    stream << text;
    REQUIRE(stream.good());
}

} // namespace

// Frozen evidence: fixtures/pi-v1.3.0/audit-1054/jsonl-store.json
// "a JSONL commit appends every sidecar before the main marker".
//
// Discriminating case: the marker carries the sidecar's sequence and ordinal,
// and the sidecar carries the base revision the marker names. A backend that
// writes only a marker, or that names no matching sidecar record, fails the
// decoded comparison even though the files exist.
TEST_CASE("a JSONL store commits, closes and reopens the same records through the public storage contract",
        "[durable][jsonl][commit][reopen][issue1074][spec]") {
    const auto loaded = FrozenScenario::load(kJsonlArtifact, kOrderScenario);
    REQUIRE(loaded.has_value());
    const FrozenScenario& scenario = *loaded;

    TempWorkspace workspace;
    auto opened = JsonlStorage::open(workspace.path());
    REQUIRE(opened.has_value());
    std::unique_ptr<JsonlStorage> storage = std::move(*opened);
    auto session = Session::create(*storage);

    const ConversationId conversation_id = create_conversation(*session);
    CHECK(conversation_id.value == 2);
    const auto definition = latest_document();
    std::optional<EntryId> entry_id;
    std::optional<DocumentId> document_id;
    auto commit = run_commit(*session,
            [&conversation_id, &definition, &entry_id, &document_id](
                    Transaction& transaction) -> support::Expected<void> {
                auto entry = transaction.append_entry(
                        conversation_id, "audit1054.note", chord::Json{chord::Json::Object{{"n", chord::Json{1}}}});
                if (!entry) {
                    return std::unexpected(std::move(entry.error()));
                }
                entry_id = entry->id;
                auto draft = transaction.doc(definition, DocumentAddress{.conversation_id = conversation_id});
                if (!draft) {
                    return std::unexpected(std::move(draft.error()));
                }
                if (auto set = draft->set(member_path("text"), chord::Json{std::string{"ordered"}}); !set) {
                    return std::unexpected(std::move(set.error()));
                }
                if (auto set = draft->set(member_path("revision"), chord::Json{1.0}); !set) {
                    return std::unexpected(std::move(set.error()));
                }
                return {};
            });
    REQUIRE(commit.has_value());
    REQUIRE(entry_id.has_value());
    REQUIRE(entry_id->value == 3);

    // The decoded marker and sidecar record are the frozen ones. Field order
    // is not part of a JSON value; the frozen artifact's own objects are the
    // comparison source.
    const auto main_lines = json_lines(workspace.read("main.jsonl"));
    REQUIRE(main_lines.size() == 2);
    const auto* main_object = main_lines.back().get_if<support::JsonValue::object_t>();
    REQUIRE(main_object != nullptr);
    const auto* writes = support::json_array_member(*main_object, "writes");
    REQUIRE(writes != nullptr);
    REQUIRE(writes->size() == 2);
    const auto* document_create = (*writes)[1].get_if<support::JsonValue::object_t>();
    REQUIRE(document_create != nullptr);
    const auto* record = support::json_object_member(*document_create, "record");
    REQUIRE(record != nullptr);
    const auto frozen_document_id = support::json_integer_member(*record, "id");
    REQUIRE(frozen_document_id.has_value());
    document_id = DocumentId{.value = *frozen_document_id};
    CHECK(*document_id == DocumentId{.value = 4});

    const auto* frozen_marker = scenario.object_member("mainMarker");
    REQUIRE(frozen_marker != nullptr);
    CHECK(canonical_text(main_lines.back()) == canonical_text(support::JsonValue{*frozen_marker}));

    const auto sidecar_lines = json_lines(workspace.read("doc-4.jsonl"));
    REQUIRE(sidecar_lines.size() == 1);
    const auto* frozen_sidecar = scenario.object_member("sidecarRecord");
    REQUIRE(frozen_sidecar != nullptr);
    CHECK(canonical_text(sidecar_lines.back()) == canonical_text(support::JsonValue{*frozen_sidecar}));

    // Close and reopen through the same public seam: the records and the ID
    // namespace continue rather than restarting.
    REQUIRE(consume(session->close()).has_value());
    auto reopened_storage = JsonlStorage::open(workspace.path());
    REQUIRE(reopened_storage.has_value());
    auto reopened = Session::create(**reopened_storage);

    std::optional<ConversationRecord> reopened_conversation;
    std::optional<EntryRecord> reopened_entry;
    REQUIRE(run_commit(*reopened,
            [&conversation_id, &entry_id, &reopened_conversation, &reopened_entry](
                    Transaction& transaction) -> support::Expected<void> {
                auto conversation = transaction.conversation(conversation_id);
                if (!conversation) {
                    return std::unexpected(std::move(conversation.error()));
                }
                if ((*conversation).has_value()) {
                    reopened_conversation = **conversation;
                }
                auto entry = transaction.entry(*entry_id);
                if (!entry) {
                    return std::unexpected(std::move(entry.error()));
                }
                if ((*entry).has_value()) {
                    reopened_entry = **entry;
                }
                return {};
            }).has_value());
    REQUIRE(reopened_conversation.has_value());
    CHECK(reopened_conversation->id == conversation_id);
    REQUIRE(reopened_entry.has_value());
    CHECK(reopened_entry->id == *entry_id);
    CHECK(reopened_entry->kind == "audit1054.note");

    auto snapshot = consume(reopened->snapshot(definition, DocumentAddress{.conversation_id = conversation_id}));
    REQUIRE(snapshot.has_value());
    REQUIRE(snapshot.value().has_value());
    CHECK(*snapshot.value() == chord::Json{chord::Json::Object{
                                       {"text", chord::Json{std::string{"ordered"}}}, {"revision", chord::Json{1.0}}}});

    std::optional<EntryId> continued_entry;
    REQUIRE(run_commit(
            *reopened, [&conversation_id, &continued_entry](Transaction& transaction) -> support::Expected<void> {
                auto entry = transaction.append_entry(conversation_id, "audit1054.note", chord::Json{2.0});
                if (!entry) {
                    return std::unexpected(std::move(entry.error()));
                }
                continued_entry = entry->id;
                return {};
            }).has_value());
    REQUIRE(continued_entry.has_value());
    CHECK(continued_entry->value > document_id->value);
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1054/jsonl-store.json
// "JSONL recovery truncates a torn main marker and an unconfirmed sidecar tail".
//
// Discriminating case: the unconfirmed sidecar record is complete and names
// sequence 4, while the last confirmed marker is sequence 2. Recovery must
// compare those sequences, drop the tail, and restore both files to their
// pre-crash bytes. A backend that adopts the complete sidecar tail, or keeps
// the torn marker, fails the file comparison and the snapshot.
TEST_CASE("JSONL recovery truncates a torn main marker and an unconfirmed sidecar tail",
        "[durable][jsonl][recovery][torn][issue1074][spec]") {
    const auto loaded = FrozenScenario::load(kJsonlArtifact, kRecoveryScenario);
    REQUIRE(loaded.has_value());
    const FrozenScenario& scenario = *loaded;

    TempWorkspace workspace;
    {
        auto opened = JsonlStorage::open(workspace.path());
        REQUIRE(opened.has_value());
        auto storage = std::move(*opened);
        auto session = Session::create(*storage);
        const ConversationId conversation_id = create_conversation(*session);
        const auto definition = latest_document();
        REQUIRE(run_commit(
                *session, [&conversation_id, &definition](Transaction& transaction) -> support::Expected<void> {
                    auto draft = transaction.doc(definition, DocumentAddress{.conversation_id = conversation_id});
                    if (!draft) {
                        return std::unexpected(std::move(draft.error()));
                    }
                    return draft->set(member_path("text"), chord::Json{std::string{"kept"}});
                }).has_value());
        REQUIRE(consume(session->close()).has_value());
    }

    const std::string main_before = workspace.read("main.jsonl");
    const std::string sidecar_before = workspace.read("doc-3.jsonl");
    const auto confirmed_main = json_lines(main_before);
    const auto confirmed_sidecar = json_lines(sidecar_before);
    REQUIRE(confirmed_main.size() == 2);
    REQUIRE(confirmed_sidecar.size() == 1);
    const auto confirmed_main_seq =
            support::json_integer_member(confirmed_main.back().get<support::JsonValue::object_t>(), "seq");
    const auto confirmed_sidecar_seq =
            support::json_integer_member(confirmed_sidecar.back().get<support::JsonValue::object_t>(), "seq");
    REQUIRE(confirmed_main_seq.has_value());
    REQUIRE(confirmed_sidecar_seq.has_value());
    CHECK(*confirmed_main_seq == 2);
    CHECK(*confirmed_sidecar_seq == 2);

    // The captured crash shape: a torn main marker plus a complete but
    // unconfirmed sidecar record.
    append_text(workspace.path() / "main.jsonl", R"({"format":1,"type":"commit","seq":3,"writes":[)");
    const support::JsonValue unconfirmed = json_object({
            {"format", support::JsonValue{1}},
            {"type", support::JsonValue{"record"}},
            {"seq", support::JsonValue{4}},
            {"ordinal", support::JsonValue{0}},
            {"payload",
                    json_object({
                            {"type", support::JsonValue{"document"}},
                            {"id", support::JsonValue{3}},
                            {"content",
                                    json_object({
                                            {"version", support::JsonValue{1}},
                                            {"kind", support::JsonValue{"delta"}},
                                            {"ops",
                                                    support::JsonValue{support::JsonValue::array_t{support::JsonValue{
                                                            support::JsonValue::array_t{support::JsonValue{"s"},
                                                                    support::JsonValue{support::JsonValue::array_t{
                                                                            support::JsonValue{"text"}}},
                                                                    support::JsonValue{"unconfirmed"}}}}}},
                                    })},
                    })},
    });
    append_text(workspace.path() / "doc-3.jsonl", json_text(unconfirmed) + "\n");
    CHECK(support::json_integer_member(unconfirmed.get<support::JsonValue::object_t>(), "seq").value_or(0) >
            *confirmed_main_seq);

    auto reopened_storage = JsonlStorage::open(workspace.path());
    REQUIRE(reopened_storage.has_value());
    auto reopened = Session::create(**reopened_storage);
    const auto definition = latest_document();
    auto snapshot = consume(reopened->snapshot(definition, DocumentAddress{.conversation_id = ConversationId{2}}));
    REQUIRE(snapshot.has_value());
    const auto* frozen_recovered = scenario.member("recovered");
    REQUIRE(frozen_recovered != nullptr);
    CHECK(json_equals(*snapshot.value(), *frozen_recovered));

    // Recovery restores both files byte-for-byte to their pre-crash content.
    CHECK(workspace.read("main.jsonl") == main_before);
    CHECK(workspace.read("doc-3.jsonl") == sidecar_before);
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1054/jsonl-store.json
// "JSONL refuses a main marker whose confirmed sidecar record is missing".
//
// Discriminating case: a marker at sequence 2 names doc-3.jsonl at ordinal 0,
// and that confirmed record is deleted. A permissive reader that drops the
// document passes a happy reopen; the frozen corruption message separates it.
TEST_CASE("JSONL refuses a main marker whose confirmed sidecar record is missing",
        "[durable][jsonl][recovery][corruption][issue1074][spec]") {
    const auto loaded = FrozenScenario::load(kJsonlArtifact, kMissingScenario);
    REQUIRE(loaded.has_value());
    const FrozenScenario& scenario = *loaded;
    const auto frozen_sidecar = scenario.string_member("missingSidecar");
    REQUIRE(frozen_sidecar.has_value());
    const auto* frozen_open = scenario.object_member("open");
    REQUIRE(frozen_open != nullptr);
    const auto* frozen_message = support::json_member(*frozen_open, "message");
    REQUIRE(frozen_message != nullptr);
    const std::string expected_message = frozen_message->get<std::string>();
    CHECK(expected_message == "Missing confirmed sidecar record doc-3.jsonl at sequence 2");

    TempWorkspace workspace;
    {
        auto opened = JsonlStorage::open(workspace.path());
        REQUIRE(opened.has_value());
        auto storage = std::move(*opened);
        auto session = Session::create(*storage);
        const ConversationId conversation_id = create_conversation(*session);
        const auto definition = latest_document();
        REQUIRE(run_commit(
                *session, [&conversation_id, &definition](Transaction& transaction) -> support::Expected<void> {
                    auto draft = transaction.doc(definition, DocumentAddress{.conversation_id = conversation_id});
                    if (!draft) {
                        return std::unexpected(std::move(draft.error()));
                    }
                    return draft->set(member_path("text"), chord::Json{std::string{"x"}});
                }).has_value());
        REQUIRE(consume(session->close()).has_value());
    }

    std::filesystem::remove(workspace.path() / *frozen_sidecar);
    auto reopened = JsonlStorage::open(workspace.path());
    REQUIRE_FALSE(reopened.has_value());
    CHECK(reopened.error().message == expected_message);
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1054/jsonl-store.json
// "a JSONL main-marker I/O failure poisons the instance while reopen recovers
// the published prefix".
//
// Discriminating case: the commit fails at the public file path, then the
// original instance must reject both writes and reads while only a reopen
// recovers the published prefix. A backend that retries in place, stays
// usable, or replays the failed batch fails one of those observations.
//
// This models an injected append failure, not SIGKILL or power loss: no fsync
// or crash-consistency guarantee is claimed from it.
TEST_CASE("a JSONL main-marker I/O failure poisons the store while reopen recovers the published prefix",
        "[durable][jsonl][poison][recovery][issue1074][spec]") {
    const auto loaded = FrozenScenario::load(kJsonlArtifact, kPoisonScenario);
    REQUIRE(loaded.has_value());
    const FrozenScenario& scenario = *loaded;
    const auto* frozen_failure = scenario.object_member("failure");
    REQUIRE(frozen_failure != nullptr);
    const auto* frozen_failure_message = support::json_member(*frozen_failure, "message");
    REQUIRE(frozen_failure_message != nullptr);
    const std::string expected_poison = frozen_failure_message->get<std::string>();
    CHECK(expected_poison == "JSONL storage is poisoned and must be reopened");

    TempWorkspace workspace;
    auto opened = JsonlStorage::open(workspace.path());
    REQUIRE(opened.has_value());
    std::unique_ptr<JsonlStorage> storage = std::move(*opened);
    auto session = Session::create(*storage);
    const ConversationId conversation_id = create_conversation(*session);
    const std::string main_before = workspace.read("main.jsonl");

    // Inject the failure at the physical main-marker path: the append targets
    // a directory, so the real append fails after the batch was validated.
    const auto main_path = workspace.path() / "main.jsonl";
    REQUIRE(std::filesystem::remove(main_path));
    REQUIRE(std::filesystem::create_directory(main_path));

    auto failing = run_commit(*session, [&conversation_id](Transaction& transaction) -> support::Expected<void> {
        auto entry = transaction.append_entry(conversation_id, "audit1054.note", chord::Json{1.0});
        if (!entry) {
            return std::unexpected(std::move(entry.error()));
        }
        return {};
    });
    REQUIRE_FALSE(failing.has_value());
    CHECK(failing.error().message == expected_poison);

    const auto* frozen_read = scenario.object_member("readAfterFailure");
    REQUIRE(frozen_read != nullptr);
    const auto* frozen_read_message = support::json_member(*frozen_read, "message");
    REQUIRE(frozen_read_message != nullptr);
    const std::string expected_read = frozen_read_message->get<std::string>();
    CHECK(expected_read == expected_poison);

    auto read_after_failure = storage->conversation(conversation_id);
    REQUIRE_FALSE(read_after_failure.has_value());
    CHECK(read_after_failure.error().message == expected_read);

    // The original instance cannot write again, even through a fresh public
    // Storage call that never passes through the failed Session.
    const StorageWrite forbidden{.type = "entry",
            .entry = EntryRecord{.id = EntryId{.value = 4},
                    .conversation_id = conversation_id,
                    .kind = "audit1054.note",
                    .data = chord::Json{2.0}}};
    const std::vector<StorageWrite> forbidden_batch{forbidden};
    const Outcome<Seq> forbidden_outcome = consume(storage->commit(forbidden_batch));
    REQUIRE_FALSE(forbidden_outcome.has_value());
    CHECK(forbidden_outcome.error().message == expected_poison);

    const auto* frozen_end = scenario.object_member("sessionEnd");
    REQUIRE(frozen_end != nullptr);
    const auto* frozen_reason = support::json_member(*frozen_end, "reason");
    const auto* frozen_error = support::json_member(*frozen_end, "errorMessage");
    REQUIRE(frozen_reason != nullptr);
    REQUIRE(frozen_error != nullptr);
    CHECK(frozen_reason->get<std::string>() == "failed");
    CHECK(frozen_error->get<std::string>() == expected_poison);
    CHECK(session->failed());
    CHECK(session->closed().reason == SessionEndReason::Failed);
    REQUIRE(session->closed().error_message.has_value());
    CHECK(*session->closed().error_message == expected_poison);

    // Restore the physical path and reopen: only the published prefix returns.
    REQUIRE(std::filesystem::remove_all(main_path) == 1);
    workspace.write("main.jsonl", main_before);
    auto reopened_storage = JsonlStorage::open(workspace.path());
    REQUIRE(reopened_storage.has_value());
    auto reopened = Session::create(**reopened_storage);
    const auto* frozen_entries = scenario.array_member("entriesAfterRecovery");
    REQUIRE(frozen_entries != nullptr);
    std::size_t recovered_entries = 0;
    REQUIRE(run_commit(
            *reopened, [&conversation_id, &recovered_entries](Transaction& transaction) -> support::Expected<void> {
                auto conversation = transaction.conversation(conversation_id);
                if (!conversation) {
                    return std::unexpected(std::move(conversation.error()));
                }
                CHECK((*conversation).has_value());
                auto entries = transaction.scan_entries(conversation_id, 10);
                if (!entries) {
                    return std::unexpected(std::move(entries.error()));
                }
                recovered_entries = entries->size();
                return {};
            }).has_value());
    CHECK(recovered_entries == frozen_entries->size());
    CHECK(recovered_entries == 0);
}

// Frozen obligation: "a sidecar record is confirmed only by a later marker that
// names it" and "the marker must be the last write".
//
// Discriminating case: injecting the main-marker failure after the sidecar was
// written leaves a complete but unconfirmed sidecar record. A backend that
// writes the marker first leaves no sidecar at all here; a backend that adopts
// an unconfirmed tail resurrects the document on reopen.
TEST_CASE("a JSONL marker failure leaves the sidecar unconfirmed and reopen discards it",
        "[durable][jsonl][poison][sidecar][ordering][issue1074][spec]") {
    TempWorkspace workspace;
    auto opened = JsonlStorage::open(workspace.path());
    REQUIRE(opened.has_value());
    std::unique_ptr<JsonlStorage> storage = std::move(*opened);
    auto session = Session::create(*storage);
    const ConversationId conversation_id = create_conversation(*session);
    const std::string main_before = workspace.read("main.jsonl");

    const auto main_path = workspace.path() / "main.jsonl";
    REQUIRE(std::filesystem::remove(main_path));
    REQUIRE(std::filesystem::create_directory(main_path));

    const auto definition = latest_document();
    auto failing =
            run_commit(*session, [&conversation_id, &definition](Transaction& transaction) -> support::Expected<void> {
                auto draft = transaction.doc(definition, DocumentAddress{.conversation_id = conversation_id});
                if (!draft) {
                    return std::unexpected(std::move(draft.error()));
                }
                return draft->set(member_path("text"), chord::Json{std::string{"unconfirmed"}});
            });
    REQUIRE_FALSE(failing.has_value());
    CHECK(failing.error().message == "JSONL storage is poisoned and must be reopened");

    // The sidecar was appended before the marker failed: this is the ordering
    // the recovery evidence relies on.
    REQUIRE(std::filesystem::exists(workspace.path() / "doc-3.jsonl"));
    const auto unconfirmed_lines = json_lines(workspace.read("doc-3.jsonl"));
    REQUIRE(unconfirmed_lines.size() == 1);
    const auto* unconfirmed_object = unconfirmed_lines.front().get_if<support::JsonValue::object_t>();
    REQUIRE(unconfirmed_object != nullptr);
    const auto unconfirmed_seq = support::json_integer_member(*unconfirmed_object, "seq");
    REQUIRE(unconfirmed_seq.has_value());
    CHECK(*unconfirmed_seq == 2);

    REQUIRE(std::filesystem::remove_all(main_path) == 1);
    workspace.write("main.jsonl", main_before);
    auto reopened_storage = JsonlStorage::open(workspace.path());
    REQUIRE(reopened_storage.has_value());
    auto reopened = Session::create(**reopened_storage);
    auto snapshot = consume(reopened->snapshot(definition, DocumentAddress{.conversation_id = conversation_id}));
    REQUIRE(snapshot.has_value());
    CHECK_FALSE(snapshot.value().has_value());
    CHECK_FALSE(std::filesystem::exists(workspace.path() / "doc-3.jsonl"));
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1054/jsonl-store.json
// "a JSONL checkpoint replaces a current-only sidecar and retirement removes it".
//
// Discriminating case: after the checkpoint the sidecar must contain only the
// new base at sequence 4, not the create base plus the delta; after retirement
// the sidecar must be gone. A backend that appends the checkpoint beside the
// old records, or leaves an empty file behind, fails the file-set comparison.
TEST_CASE("a JSONL checkpoint replaces a current-only sidecar and retirement removes it",
        "[durable][jsonl][checkpoint][retire][issue1074][spec]") {
    const auto loaded = FrozenScenario::load(kJsonlArtifact, kCheckpointScenario);
    REQUIRE(loaded.has_value());
    const FrozenScenario& scenario = *loaded;
    const auto* after_first_base = scenario.array_member("afterFirstBase");
    const auto* after_delta = scenario.array_member("afterDelta");
    const auto* after_checkpoint = scenario.array_member("afterCheckpoint");
    const auto* after_retire = scenario.array_member("afterRetire");
    REQUIRE(after_first_base != nullptr);
    REQUIRE(after_delta != nullptr);
    REQUIRE(after_checkpoint != nullptr);
    REQUIRE(after_retire != nullptr);

    TempWorkspace workspace;
    auto opened = JsonlStorage::open(workspace.path());
    REQUIRE(opened.has_value());
    std::unique_ptr<JsonlStorage> storage = std::move(*opened);
    auto session = Session::create(*storage);
    const ConversationId conversation_id = create_conversation(*session);
    const auto definition = latest_document();
    REQUIRE(run_commit(*session, [&conversation_id, &definition](Transaction& transaction) -> support::Expected<void> {
        auto draft = transaction.doc(definition, DocumentAddress{.conversation_id = conversation_id});
        if (!draft) {
            return std::unexpected(std::move(draft.error()));
        }
        return draft->set(member_path("text"), chord::Json{std::string{"one"}});
    }).has_value());
    require_files_match(workspace, *after_first_base);

    REQUIRE(run_commit(*session, [&conversation_id, &definition](Transaction& transaction) -> support::Expected<void> {
        auto draft = transaction.doc(definition, DocumentAddress{.conversation_id = conversation_id});
        if (!draft) {
            return std::unexpected(std::move(draft.error()));
        }
        return draft->set(member_path("text"), chord::Json{std::string{"two"}});
    }).has_value());
    require_files_match(workspace, *after_delta);

    // A checkpoint is a document.change whose committed content is a base
    // snapshot rather than an operation batch. It replaces the current-only
    // sidecar instead of appending beside the create base and delta.
    //
    // Session in this slice does not expose checkpointWhen or retire_doc (they
    // belong to the later durable document tasks), so Storage::commit is the
    // highest public caller available for these two operations.
    const StorageWrite checkpoint{.type = "document.change",
            .document_id = DocumentId{.value = 3},
            .document_value = chord::Json{chord::Json::Object{
                    {"text", chord::Json{std::string{"checkpoint"}}}, {"revision", chord::Json{0}}}}};
    const std::vector<StorageWrite> checkpoint_batch{checkpoint};
    REQUIRE(consume(storage->commit(checkpoint_batch)).has_value());
    require_files_match(workspace, *after_checkpoint);

    auto checkpoint_storage = JsonlStorage::open(workspace.path());
    REQUIRE(checkpoint_storage.has_value());
    auto checkpoint_session = Session::create(**checkpoint_storage);
    auto snapshot =
            consume(checkpoint_session->snapshot(definition, DocumentAddress{.conversation_id = conversation_id}));
    REQUIRE(snapshot.has_value());
    REQUIRE(snapshot.value().has_value());
    CHECK(*snapshot.value() == chord::Json{chord::Json::Object{{"text", chord::Json{std::string{"checkpoint"}}},
                                       {"revision", chord::Json{0}}}});

    const StorageWrite retire{.type = "document.retire", .document_id = DocumentId{.value = 3}};
    const std::vector<StorageWrite> retire_batch{retire};
    REQUIRE(consume(storage->commit(retire_batch)).has_value());
    require_files_match(workspace, *after_retire);
    CHECK_FALSE(std::filesystem::exists(workspace.path() / "doc-3.jsonl"));

    auto retired_storage = JsonlStorage::open(workspace.path());
    REQUIRE(retired_storage.has_value());
    auto retired_session = Session::create(**retired_storage);
    auto retired = consume(retired_session->snapshot(definition, DocumentAddress{.conversation_id = conversation_id}));
    REQUIRE(retired.has_value());
    CHECK_FALSE(retired.value().has_value());
}
