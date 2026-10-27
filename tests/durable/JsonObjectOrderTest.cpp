#include <cch/ai/Models.hpp>
#include <cch/durable/Harness.hpp>
#include <cch/durable/JsonlStorage.hpp>
#include <cch/durable/Session.hpp>
#include <cch/durable/SqliteStorage.hpp>
#include <cch/durable/Storage.hpp>
#include <cch/support/JsonValue.hpp>

#include "support/Json.hpp"

#include "durable/Outcome.hpp"
#include "support/TempWorkspace.hpp"

#include <catch2/catch_test_macros.hpp>

#include <fstream>

#include <iterator>
#include <memory>
#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::tests::durable {
using namespace cch::durable;

namespace {

/// The `pi.system` entry data the runtime writes: `{"model":[message]}`, where
/// the message carries `sections` as an object whose member order is the frozen
/// `SystemMessage.sections` order (#1149).
[[nodiscard]] chord::Json system_entry_data(chord::Json::Object sections) {
    chord::Json::Object message{{"role", "system"}, {"content", ""}, {"timestamp", chord::Json(0.0)}};
    message.emplace("sections", chord::Json{std::move(sections)});
    return chord::Json{chord::Json::Object{{"model", chord::Json::Array{chord::Json{std::move(message)}}}}};
}

/// The section names of the first `pi.system` entry, in stored order.
[[nodiscard]] std::string stored_section_order(const std::vector<EntryRecord>& entries) {
    for (const auto& entry : entries) {
        if (entry.kind != "pi.system") continue;
        const auto* envelope = entry.data.as_object();
        REQUIRE(envelope != nullptr);
        const auto model = envelope->find("model");
        REQUIRE(model != envelope->end());
        const auto* messages = model->second.as_array();
        REQUIRE(messages != nullptr);
        REQUIRE(!messages->empty());
        const auto* message = messages->front().as_object();
        REQUIRE(message != nullptr);
        const auto sections = message->find("sections");
        REQUIRE(sections != message->end());
        const auto* fields = sections->second.as_object();
        REQUIRE(fields != nullptr);
        std::string order;
        for (const auto& [name, text] : *fields) {
            (void)text;
            order += name;
        }
        return order;
    }
    return {};
}

[[nodiscard]] std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

} // namespace

TEST_CASE("JSONL keeps pi.system.sections in written order across a reopen", "[durable][jsonl][issue1149][spec]") {
    TempWorkspace workspace;
    ConversationId conversation{};
    {
        auto opened = JsonlStorage::open(workspace.path());
        REQUIRE(opened.has_value());
        std::unique_ptr<JsonlStorage> storage = std::move(*opened);
        auto session = Session::create(*storage);
        REQUIRE(consume(session->commit([&conversation](Transaction& transaction) -> support::Expected<void> {
            auto created = transaction.create_conversation();
            if (!created) return std::unexpected(created.error());
            conversation = created->id;
            // Deliberately not in dictionary order: `b` then `a` is the
            // frozen order the round must preserve.
            auto entry = transaction.append_entry(conversation,
                    "pi.system",
                    system_entry_data(chord::Json::Object{
                            {"b", chord::Json{"2"}},
                            {"a", chord::Json{"1"}},
                    }));
            if (!entry) return std::unexpected(entry.error());
            return {};
        })).has_value());
        REQUIRE(consume(session->close()).has_value());
    }

    // The bytes on disk carry the member order, so a sorted writer is visible
    // here as well as in the reopened order.
    const std::string written = read_text_file(workspace.path() / "main.jsonl");
    const auto sections_at = written.find("\"sections\"");
    REQUIRE(sections_at != std::string::npos);
    const auto b_at = written.find("\"b\"", sections_at);
    const auto a_at = written.find("\"a\"", sections_at);
    REQUIRE(b_at != std::string::npos);
    REQUIRE(a_at != std::string::npos);
    CHECK(b_at < a_at);

    auto reopened = JsonlStorage::open(workspace.path());
    REQUIRE(reopened.has_value());
    std::unique_ptr<JsonlStorage> storage = std::move(*reopened);
    auto entries = storage->scan_entries(conversation, 100);
    REQUIRE(entries.has_value());
    CHECK(stored_section_order(*entries) == "ba");
}

TEST_CASE("SQLite keeps pi.system.sections in written order across a reopen", "[durable][sqlite][issue1149][spec]") {
    TempWorkspace workspace;
    ConversationId conversation{};
    const auto path = workspace.path() / "storage.sqlite";
    {
        auto opened = SqliteStorage::open(path);
        REQUIRE(opened.has_value());
        std::unique_ptr<SqliteStorage> storage = std::move(*opened);
        auto session = Session::create(*storage);
        REQUIRE(consume(session->commit([&conversation](Transaction& transaction) -> support::Expected<void> {
            auto created = transaction.create_conversation();
            if (!created) return std::unexpected(created.error());
            conversation = created->id;
            auto entry = transaction.append_entry(conversation,
                    "pi.system",
                    system_entry_data(chord::Json::Object{
                            {"b", chord::Json{"2"}},
                            {"a", chord::Json{"1"}},
                    }));
            if (!entry) return std::unexpected(entry.error());
            return {};
        })).has_value());
        REQUIRE(consume(session->close()).has_value());
    }

    auto reopened = SqliteStorage::open(path);
    REQUIRE(reopened.has_value());
    std::unique_ptr<SqliteStorage> storage = std::move(*reopened);
    auto entries = storage->scan_entries(conversation, 100);
    REQUIRE(entries.has_value());
    CHECK(stored_section_order(*entries) == "ba");
}

TEST_CASE("write_json preserves member order while canonical_json sorts it", "[support][json][issue1149][spec]") {
    // The discriminating pair: the same value renders differently, so a
    // comparison that needs stable bytes must name `canonical_json` explicitly.
    const support::JsonValue value = support::JsonValue{support::JsonValue::object_t{
            {"b", support::JsonValue{support::JsonValue::object_t{{"z", 1.0}, {"y", 2.0}}}},
            {"a", support::JsonValue{true}},
    }};

    const auto ordered = support::write_json(value);
    REQUIRE(ordered);
    CHECK(*ordered == R"({"b":{"z":1,"y":2},"a":true})");

    const auto canonical = support::canonical_json(value);
    REQUIRE(canonical);
    CHECK(*canonical == R"({"a":true,"b":{"y":2,"z":1}})");
    CHECK(*ordered != *canonical);

    // Canonical text is the same for two values that differ only in order.
    const support::JsonValue reordered = support::JsonValue{support::JsonValue::object_t{
            {"a", support::JsonValue{true}},
            {"b", support::JsonValue{support::JsonValue::object_t{{"y", 2.0}, {"z", 1.0}}}},
    }};
    const auto reordered_canonical = support::canonical_json(reordered);
    REQUIRE(reordered_canonical);
    CHECK(*reordered_canonical == *canonical);
    const auto reordered_ordered = support::write_json(reordered);
    REQUIRE(reordered_ordered);
    CHECK(*reordered_ordered != *ordered);
}

TEST_CASE("the runtime stores a non-alphabetical pi.system.sections order", "[durable][workflow][issue1149][spec]") {
    // The runtime builds `sections` from `SystemMessage.sections` in the offered
    // order. The frozen planner's `replaySections` reads that order back, so it
    // has to survive the write. The keys are deliberately not in dictionary
    // order: a sorted writer reports `ab` here.
    Registry registry;
    Extension ordered{.name = "ordered-sections"};
    ordered.sections.push_back(PromptSection{.key = "b", .text = "2", .tag = false});
    ordered.sections.push_back(PromptSection{.key = "a", .text = "1", .tag = false});
    REQUIRE(registry.install(std::move(ordered)).has_value());

    // A real JSONL store, so the sections object is serialized and parsed back
    // rather than kept as live `chord::Json` in memory.
    TempWorkspace workspace;
    auto store = JsonlStorage::open(workspace.path());
    REQUIRE(store.has_value());
    std::unique_ptr<JsonlStorage> storage_owner = std::move(*store);
    JsonlStorage& storage = *storage_owner;
    HarnessOptions options{.registry = &registry};
    options.settings.compaction.enabled = false;
    options.stream = [](ai::Model, ai::AiContext, ai::SimpleStreamOptions) {
        return ai::ModelStream{
                ai::ModelStreamProducer{[](ai::AssistantEventSink sink, ai::ModelStreamCompletion done) noexcept {
                    auto answer = ai::assistant_text_message("finished");
                    auto result = sink(ai::AssistantDoneEvent{.reason = answer.stop_reason, .message = answer});
                    if (!result)
                        done(std::unexpected(result.error()));
                    else
                        done(std::move(answer));
                }}};
    };
    auto opened = consume(Harness::open(storage, std::move(options)));
    REQUIRE(opened.has_value());
    auto harness = std::move(opened.value());
    auto root = consume(harness->root(RootOptions{.model = ai::Model{.id = "model", .provider = "test"}}));
    REQUIRE(root.has_value());

    auto admitted = consume(harness->submit(
            root.value(), SubmissionDraft{.body = InputSubmissionDraft{.content = chord::Json{"first"}}}));
    REQUIRE(admitted.has_value());
    REQUIRE(consume(admitted.value().wait()).has_value());

    const ConversationId conversation = root.value();
    REQUIRE(consume(harness->close()).has_value());

    // Reopen: a live store answers from the record it just appended, so only a
    // fresh parse of the committed bytes proves the round-trip.
    auto reopened = JsonlStorage::open(workspace.path());
    REQUIRE(reopened.has_value());
    std::unique_ptr<JsonlStorage> reopened_storage = std::move(*reopened);
    auto entries = reopened_storage->scan_entries(conversation, 100);
    REQUIRE(entries.has_value());
    CHECK(stored_section_order(*entries) == "ba");
}

TEST_CASE("reordering sections past a head marker keeps the offered order", "[durable][workflow][issue1149][spec]") {
    // Frozen `planSystemEntries` decides between a minimal patch, a full
    // remove/re-add reorder, and nothing by comparing the replayed section order
    // with the desired one. That comparison needs the stored order, so this
    // covers the post-head baseline branch (the branch this runtime implements):
    // the baseline carries every desired section in the offered order.
    //
    // Measured while writing this: with `a,b` then `b,a` and no head marker, the
    // second round writes no `pi.system` entry at all (the planner compares
    // section names, not order), so the frozen three-entry remove/re-add
    // sequence is not reachable yet. That gap is the branch 2/3 work #1149
    // unblocks, not part of this ticket.
    Registry registry;
    Extension ordered{.name = "ordered-sections"};
    ordered.sections.push_back(PromptSection{.key = "b", .text = "2", .tag = false});
    ordered.sections.push_back(PromptSection{.key = "a", .text = "1", .tag = false});
    REQUIRE(registry.install(std::move(ordered)).has_value());

    // A real JSONL store, so the sections object is serialized and parsed back
    // rather than kept as live `chord::Json` in memory.
    TempWorkspace workspace;
    auto store = JsonlStorage::open(workspace.path());
    REQUIRE(store.has_value());
    std::unique_ptr<JsonlStorage> storage_owner = std::move(*store);
    JsonlStorage& storage = *storage_owner;
    HarnessOptions options{.registry = &registry};
    options.settings.compaction.enabled = false;
    options.stream = [](ai::Model, ai::AiContext, ai::SimpleStreamOptions) {
        return ai::ModelStream{
                ai::ModelStreamProducer{[](ai::AssistantEventSink sink, ai::ModelStreamCompletion done) noexcept {
                    auto answer = ai::assistant_text_message("finished");
                    auto result = sink(ai::AssistantDoneEvent{.reason = answer.stop_reason, .message = answer});
                    if (!result)
                        done(std::unexpected(result.error()));
                    else
                        done(std::move(answer));
                }}};
    };
    auto opened = consume(Harness::open(storage, std::move(options)));
    REQUIRE(opened.has_value());
    auto harness = std::move(opened.value());
    auto root = consume(harness->root(RootOptions{.model = ai::Model{.id = "model", .provider = "test"}}));
    REQUIRE(root.has_value());

    // An old `pi.system` entry and a head marker make the next preparation take
    // the post-head baseline branch.
    EntryId marker{};
    REQUIRE(consume(harness->commit([&](Transaction& tx) -> support::Expected<void> {
        auto old = tx.append_entry(root.value(),
                "pi.system",
                system_entry_data(chord::Json::Object{{"a", chord::Json{"1"}}, {"b", chord::Json{"2"}}}),
                std::nullopt,
                std::vector<chord::Json>{chord::Json{chord::Json::Object{{"role", "system"}, {"content", "old"}}}});
        if (!old) return std::unexpected(old.error());
        auto head = tx.append_entry(root.value(),
                "pi.compaction",
                chord::Json{chord::Json::Object{}},
                EntryHeadVariant{old->id},
                std::vector<chord::Json>{chord::Json{chord::Json::Object{{"role", "user"}, {"content", "summary"}}}});
        if (!head) return std::unexpected(head.error());
        marker = head->id;
        return {};
    })).has_value());

    auto admitted = consume(harness->submit(
            root.value(), SubmissionDraft{.body = InputSubmissionDraft{.content = chord::Json{"continue"}}}));
    REQUIRE(admitted.has_value());
    REQUIRE(consume(admitted.value().wait()).has_value());

    const ConversationId conversation = root.value();
    REQUIRE(consume(harness->close()).has_value());

    auto reopened = JsonlStorage::open(workspace.path());
    REQUIRE(reopened.has_value());
    std::unique_ptr<JsonlStorage> reopened_storage = std::move(*reopened);
    auto entries = reopened_storage->scan_entries(conversation, 100);
    REQUIRE(entries.has_value());
    const auto baseline = std::ranges::find_if(*entries,
            [&marker](const auto& entry) { return entry.kind == "pi.system" && entry.id.value > marker.value; });
    REQUIRE(baseline != entries->end());
    CHECK(stored_section_order(std::vector<EntryRecord>{*baseline}) == "ba");
    // The baseline omits the superseded entry rather than rewriting it.
    REQUIRE(baseline->edits.size() == 1);
    CHECK(baseline->edits[0].action == "omit");
}

} // namespace cch::tests::durable
