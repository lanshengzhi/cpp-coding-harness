#include <cch/ai/Content.hpp>
#include <cch/ai/Models.hpp>
#include <cch/durable/Harness.hpp>
#include <cch/durable/JsonlStorage.hpp>
#include <cch/durable/SqliteStorage.hpp>
#include "durable/FrozenExpectation.hpp"
#include "durable/GatedStorage.hpp"
#include "durable/Outcome.hpp"
#include "support/TempWorkspace.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <semaphore>
#include <string>
#include <vector>

namespace cch::tests::durable {
using namespace cch::durable;

namespace {

const chord::Json& entry_model(const EntryRecord& entry) {
    const auto* envelope = entry.data.as_object();
    REQUIRE(envelope != nullptr);
    const auto found = envelope->find("model");
    REQUIRE(found != envelope->end());
    const auto* messages = found->second.as_array();
    REQUIRE(messages != nullptr);
    REQUIRE(messages->size() == 1);
    return messages->front();
}

chord::Json transcript(const std::vector<EntryRecord>& entries) {
    chord::Json::Array result;
    for (const auto& entry : entries) {
        const auto* message = entry_model(entry).as_object();
        REQUIRE(message != nullptr);
        if (entry.kind == "pi.system") {
            chord::Json::Array sections;
            if (const auto found = message->find("sections"); found != message->end()) {
                for (const auto& [key, text] : *found->second.as_object()) {
                    (void)text;
                    sections.emplace_back(key);
                }
            }
            chord::Json::Array tools;
            if (const auto found = message->find("toolsAdded"); found != message->end()) {
                for (const auto& tool : *found->second.as_array()) tools.push_back(tool.as_object()->at("name"));
            }
            result.emplace_back(chord::Json::Array{entry.kind, chord::Json{std::move(sections)}, chord::Json{std::move(tools)}});
            continue;
        }
        std::string text;
        const auto& content = message->at("content");
        if (content.as_string()) text = *content.as_string();
        else for (const auto& block : *content.as_array()) {
            if (*block.as_object()->at("type").as_string() == "text") {
                if (!text.empty()) text += "|";
                text += *block.as_object()->at("text").as_string();
            }
        }
        if (entry.kind == "pi.tool-result") {
            result.emplace_back(chord::Json::Array{entry.kind, message->at("toolName"), message->at("toolCallId"),
                    message->at("isError"), chord::Json{message->contains("durationMs")}, text});
        } else result.emplace_back(chord::Json::Array{entry.kind, text});
    }
    return chord::Json{std::move(result)};
}

ai::ModelStream echo_response(int turn) {
    return ai::ModelStream{ai::ModelStreamProducer{
            [turn](ai::AssistantEventSink sink, ai::ModelStreamCompletion completion) noexcept {
                auto message = ai::assistant_text_message("finished");
                if (turn == 0) {
                    message.content = {ai::ToolCallContent{.id = "call-1", .name = "echo",
                            .arguments = support::JsonValue{support::JsonValue::object_t{{"text", "hello"}}}}};
                    message.stop_reason = ai::AssistantStopReason::ToolUse;
                }
                auto delivered = sink(ai::AssistantDoneEvent{.reason = message.stop_reason, .message = message});
                if (!delivered) completion(std::unexpected(delivered.error()));
                else completion(std::move(message));
            }}};
}

SubmissionDraft text_input(std::string text, std::optional<WhenBusy> when_busy = std::nullopt) {
    return SubmissionDraft{
            .body = InputSubmissionDraft{.content = chord::Json{std::move(text)}, .when_busy = when_busy}};
}

} // namespace

TEST_CASE("Harness runs an admitted prompt through a real injected tool and settles committed entries",
        "[durable][workflow][issue1079]") {
    const auto expected = FrozenScenario::load("fixtures/pi-v1.3.0/audit-1056/workflow.json",
            "a scripted model drives admission, one real injected tool, committed entries and final settlement");
    REQUIRE(expected.has_value());

    Registry registry;
    std::atomic_int executions{0};
    Extension extension{.name = "audit-tools"};
    extension.tools.push_back(ToolRegistration{
            .declaration = ai::Tool{.name = "echo", .description = "Echo the text argument back"},
            .execute = [&executions](support::JsonValue arguments, std::stop_token) {
                ++executions;
                return support::AsyncResult<std::vector<ai::Content>>{
                        std::vector<ai::Content>{ai::text_content("echo:" + arguments.at("text").get_string())}};
            }});
    extension.sections.push_back(PromptSection{.key = "audit-note", .text = "workflow under audit"});
    REQUIRE(registry.install(std::move(extension)).has_value());

    std::atomic_int requests{0};
    std::atomic_bool offered_echo{true};
    HarnessOptions options{.registry = &registry};
    options.stream = [&requests, &offered_echo](ai::Model, ai::AiContext context, ai::SimpleStreamOptions) {
        const int turn = requests++;
        if (context.tools.size() != 1 || context.tools.front().name != "echo") offered_echo = false;
        return echo_response(turn);
    };
    MemoryStorage storage;
    auto opened = consume(Harness::open(storage, std::move(options)));
    REQUIRE(opened.has_value());
    auto harness = std::move(opened.value());
    auto root = consume(harness->root());
    REQUIRE(root.has_value());
    auto admitted = consume(harness->submit(root.value(), text_input("run the echo tool")));
    REQUIRE(admitted.has_value());
    auto settled = consume(admitted.value().wait());
    REQUIRE(settled.has_value());
    CHECK(settled.value().id == admitted.value().id);
    CHECK(settled.value().conversation_id == root.value());
    CHECK(settled.value().status == "done");
    CHECK(executions == 1);
    CHECK(requests == 2);
    CHECK(offered_echo);
    auto entries = storage.scan_entries(root.value(), 100);
    REQUIRE(entries.has_value());
    std::ranges::reverse(*entries);
    REQUIRE(entries->size() == 5);
    const auto* expected_entries = expected->array_member("entries");
    REQUIRE(expected_entries != nullptr);
    for (std::size_t i = 0; i < entries->size(); ++i) {
        CHECK(entries->at(i).kind == expected_entries->at(i).get_array().at(0).get_string());
    }
    CHECK(json_equals(transcript(*entries), *expected->member("entries")));
    CHECK(settled.value().entry == entries->front().id);
    CHECK(settled.value().answer == entries->back().id);
    CHECK(entries->at(3).data.as_object()->contains("byTaskId"));
    const auto duration = entries->at(3).data.to_support().at("model").get_array().at(0).at("durationMs");
    CHECK(duration.holds<double>());
    CHECK(duration.get_number() >= 0);
    CHECK(entries->at(3).data.to_support().at("model").get_array().at(0).at("content").get_array().at(0).at("text").get_string() == "echo:hello");
    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("Harness waits for the real tool before requesting an answer and settling",
        "[durable][workflow][issue1079]") {
    const auto expected = FrozenScenario::load("fixtures/pi-v1.3.0/audit-1079/held-tool.json",
            "a held real tool prevents the next model request and submission settlement");
    REQUIRE(expected.has_value());
    const auto* before = expected->object_member("before");
    const auto* after = expected->object_member("after");
    REQUIRE(before != nullptr);
    REQUIRE(after != nullptr);

    // ReleaseGate is confined to one Asio executor. These standard counted
    // semaphores provide the same latched-release contract across the Harness
    // execution thread and this caller, without a timer or a lost wake-up.
    std::binary_semaphore entered{0};
    std::binary_semaphore release{0};
    std::atomic_int executions{0};
    std::atomic_int requests{0};
    Registry registry;
    Extension extension{.name = "held-tool"};
    extension.tools.push_back(ToolRegistration{.declaration = ai::Tool{.name = "echo", .description = "Echo text"},
            .execute = [&entered, &release, &executions](support::JsonValue args, std::stop_token) {
                ++executions;
                entered.release();
                release.acquire();
                return support::AsyncResult<std::vector<ai::Content>>{std::vector<ai::Content>{
                        ai::text_content("echo:" + args.at("text").get_string())}};
            }});
    REQUIRE(registry.install(std::move(extension)).has_value());
    HarnessOptions options{.registry = &registry};
    options.stream = [&requests](ai::Model, ai::AiContext, ai::SimpleStreamOptions) {
        return echo_response(requests++);
    };
    MemoryStorage storage;
    auto opened = consume(Harness::open(storage, std::move(options)));
    REQUIRE(opened.has_value());
    auto harness = std::move(opened.value());
    auto root = consume(harness->root());
    REQUIRE(root.has_value());
    std::vector<std::string> published_entries;
    bool terminal_has_final = false;
    REQUIRE(harness->session().subscribe_commits([&storage, &published_entries, &terminal_has_final](const CommitPublication& publication) {
        bool terminal = false;
        bool final_entry = false;
        for (const auto& change : publication.changes) {
            const auto* table = std::get_if<TableCommitChange>(&change);
            if (table == nullptr) continue;
            if (table->entry_id) {
                const auto entry = storage.entry(*table->entry_id);
                if (entry && entry->has_value()) {
                    published_entries.push_back(entry->value().entry.kind);
                    final_entry = entry->value().entry.kind == "pi.assistant";
                }
            }
            if (table->task && table->task->state.status == "terminal") terminal = true;
        }
        if (terminal) terminal_has_final = final_entry;
    }).has_value());
    auto admitted = consume(harness->submit(root.value(), text_input("run the echo tool")));
    REQUIRE(admitted.has_value());
    std::mutex mutex;
    std::condition_variable done;
    std::optional<support::Expected<SettledSubmission>> settlement;
    admitted.value().wait().start([&mutex, &done, &settlement](auto result) noexcept {
        std::lock_guard lock(mutex);
        settlement.emplace(std::move(result));
        done.notify_one();
    });
    const bool started = entered.try_acquire_for(std::chrono::seconds(3));
    CHECK(started);
    if (started) {
        auto entries = storage.scan_entries(root.value(), 100);
        CHECK(entries.has_value());
        if (entries) {
            std::ranges::reverse(*entries);
            CHECK(json_equals(transcript(*entries), before->at("entries")));
        }
        {
            std::lock_guard lock(mutex);
            CHECK(settlement.has_value() == before->at("settled").get_boolean());
        }
        CHECK(executions == before->at("executions").get_number());
        CHECK(requests == before->at("requests").get_number());
        const auto busy = consume(harness->submit(root.value(), text_input("must not append", WhenBusy::Reject)));
        CHECK_FALSE(busy.has_value());
    }
    release.release();
    {
        std::unique_lock lock(mutex);
        const bool completed = done.wait_for(lock, std::chrono::seconds(3), [&settlement] { return settlement.has_value(); });
        REQUIRE(completed);
        REQUIRE(settlement->has_value());
        CHECK(settlement->value().status == after->at("status").get_string());
    }
    CHECK(executions == after->at("executions").get_number());
    CHECK(requests == after->at("requests").get_number());
    auto entries = storage.scan_entries(root.value(), 100);
    REQUIRE(entries.has_value());
    std::ranges::reverse(*entries);
    CHECK(json_equals(transcript(*entries), after->at("entries")));
    REQUIRE(consume(harness->close()).has_value());
    CHECK(published_entries == std::vector<std::string>{"pi.user", "pi.system", "pi.assistant", "pi.tool-result", "pi.assistant"});
    CHECK(terminal_has_final);
}

TEST_CASE("Harness creates its root atomically and resolves late registry installs and settings on request",
        "[durable][workflow][issue1079]") {
    const auto frozen_root = FrozenScenario::load("fixtures/pi-v1.3.0/audit-1056/conversations.json",
            "the root conversation is created lazily in one commit with its five built-in documents");
    const auto transition = FrozenScenario::load("fixtures/pi-v1.3.0/audit-1056/workflow.json",
            "a registry install after conversation creation changes the next request's tools and prompt sections");
    REQUIRE(frozen_root.has_value());
    REQUIRE(transition.has_value());
    Registry registry;
    Extension first{.name = "first-extension"};
    first.tools.push_back(ToolRegistration{.declaration = ai::Tool{.name = "first"},
            .execute = [](support::JsonValue, std::stop_token) {
                return support::AsyncResult<std::vector<ai::Content>>{std::vector<ai::Content>{ai::text_content("first ran")}};
            }});
    REQUIRE(registry.install(std::move(first)).has_value());
    MemoryStorage storage;
    HarnessOptions options{.registry = &registry};
    options.settings.stream.temperature = 0.25;
    bool settings_applied = false;
    std::vector<std::string> offered;
    std::optional<std::string> prompt;
    options.stream = [&settings_applied, &offered, &prompt](ai::Model model, ai::AiContext context, ai::SimpleStreamOptions stream) {
        settings_applied = stream.temperature == 0.25 && stream.reasoning == ai::ThinkingLevel::High && model.id == "test-model";
        for (const auto& tool : context.tools) offered.push_back(tool.name);
        prompt = context.system_prompt;
        return echo_response(1);
    };
    auto opened = consume(Harness::open(storage, std::move(options)));
    REQUIRE(opened.has_value());
    auto harness = std::move(opened.value());
    int root_commits = 0;
    std::vector<std::string> root_changes;
    REQUIRE(harness->session().subscribe_commits([&root_commits, &root_changes](const CommitPublication& publication) {
        ++root_commits;
        for (const auto& change : publication.changes) {
            if (const auto* document = std::get_if<DocumentCommitChange>(&change)) root_changes.push_back("document:" + document->record.kind);
            else if (const auto* table = std::get_if<TableCommitChange>(&change))
                root_changes.push_back(table->type);
            else if (const auto* sub = std::get_if<SubmissionCommitChange>(&change))
                root_changes.push_back("submission:" + std::string{sub->type});
        }
    }).has_value());
    auto root = consume(harness->root(RootOptions{.model = ai::Model{.id = "test-model"}, .thinking_level = ai::ThinkingLevel::High}));
    REQUIRE(root.has_value());
    auto same = consume(harness->root(RootOptions{.thinking_level = ai::ThinkingLevel::Low}));
    REQUIRE(same.has_value());
    CHECK(root.value() == same.value());
    CHECK(root.value().value == *frozen_root->number_member("rootId"));
    CHECK(root_commits == *frozen_root->number_member("commits"));
    std::ranges::sort(root_changes);
    chord::Json::Array observed_changes;
    for (const auto& change : root_changes) observed_changes.emplace_back(change);
    CHECK(json_equals(chord::Json{observed_changes}, *frozen_root->member("changes")));
    auto agent_doc = storage.find_document(
            "pi.agent:v1", DocumentAddress{.conversation_id = root.value()}, DocumentPoint::current());
    REQUIRE(agent_doc.has_value());
    REQUIRE(agent_doc->has_value());
    auto agent = storage.document(agent_doc->value().id, DocumentPoint::current());
    REQUIRE(agent.has_value());
    REQUIRE(agent->has_value());
    CHECK(agent->value().value.as_object()->at("thinkingLevel") == chord::Json{"high"});

    Extension second{.name = "second-extension"};
    second.tools.push_back(ToolRegistration{.declaration = ai::Tool{.name = "second"},
            .execute = [](support::JsonValue, std::stop_token) {
                return support::AsyncResult<std::vector<ai::Content>>{std::vector<ai::Content>{ai::text_content("second ran")}};
            }});
    second.sections.push_back(PromptSection{.key = "late-section", .text = "registered after creation"});
    REQUIRE(registry.install(std::move(second)).has_value());
    auto admitted = consume(harness->submit(root.value(), text_input("what tools do you have")));
    REQUIRE(admitted.has_value());
    REQUIRE(consume(admitted.value().wait()).has_value());
    auto entries = storage.scan_entries(root.value(), 100);
    REQUIRE(entries.has_value());
    std::ranges::reverse(*entries);
    REQUIRE(entries->size() == 3);
    const auto projected = transcript(*entries);
    const auto& system = projected.as_array()->at(1);
    CHECK(json_equals(system.as_array()->at(1), *transition->member("sectionsAdded")));
    CHECK(json_equals(system.as_array()->at(2), *transition->member("toolsAdded")));
    REQUIRE(consume(harness->close()).has_value());
    CHECK(settings_applied);
    chord::Json::Array tool_names;
    for (const auto& name : offered) tool_names.emplace_back(name);
    CHECK(json_equals(chord::Json{std::move(tool_names)}, *transition->member("afterInstall")));
    CHECK(prompt == "<late-section>\nregistered after creation\n</late-section>");
}

TEST_CASE("Harness receipt reports a final storage failure instead of hanging or reporting success",
        "[durable][workflow][issue1079]") {
    cch::tests::GatedStorage storage;
    HarnessOptions options;
    options.stream = [&storage](ai::Model, ai::AiContext, ai::SimpleStreamOptions) {
        storage.fail_next_commit("disk vanished");
        return echo_response(1);
    };
    auto opened = consume(Harness::open(storage, std::move(options)));
    REQUIRE(opened.has_value());
    auto harness = std::move(opened.value());
    auto root = consume(harness->root());
    REQUIRE(root.has_value());
    auto admitted = consume(harness->submit(root.value(), text_input("do not lose the answer")));
    REQUIRE(admitted.has_value());
    auto receipt = consume(admitted.value().wait());
    CHECK_FALSE(receipt.has_value());
    if (!receipt.has_value()) CHECK(receipt.error().message == "Session failed after a storage error; close and reopen it");
    CHECK(harness->session().failed());
    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("Workflow entries persist model messages and byTaskId through JSONL and SQLite storage",
        "[durable][workflow][issue1079]") {
    TempWorkspace workspace;
    const std::filesystem::path base = workspace.path() / "workflow";

    Registry registry;
    std::atomic_int executions{0};
    Extension extension{.name = "roundtrip-tools"};
    extension.tools.push_back(ToolRegistration{
            .declaration = ai::Tool{.name = "echo", .description = "Echo the text argument back"},
            .execute = [&executions](support::JsonValue arguments, std::stop_token) {
                ++executions;
                return support::AsyncResult<std::vector<ai::Content>>{
                        std::vector<ai::Content>{ai::text_content("echo:" + arguments.at("text").get_string())}};
            }});
    REQUIRE(registry.install(std::move(extension)).has_value());
    const std::vector<std::string> backends = {"jsonl", "sqlite"};
    for (const auto& backend : backends) {
        std::atomic_int backend_requests{0};
        INFO("backend: " + backend);
        // Each backend needs its own subdirectory: SQLite derives schema table
        // names from the directory, and JSONL stores several files per storage.
        const std::filesystem::path file = base / backend;
        const auto open_harness = [&registry, &backend_requests](Storage& storage) {
            HarnessOptions harness_options{.registry = &registry};
            harness_options.stream = [&backend_requests](ai::Model, ai::AiContext, ai::SimpleStreamOptions) {
                return echo_response(backend_requests++);
            };
            return Harness::open(storage, std::move(harness_options));
        };
        std::unique_ptr<Storage> storage;
        if (backend == "jsonl") {
            auto opened_storage = JsonlStorage::open(file);
            REQUIRE(opened_storage.has_value());
            storage = std::move(opened_storage.value());
        } else {
            auto opened_storage = SqliteStorage::open(file);
            REQUIRE(opened_storage.has_value());
            storage = std::move(opened_storage.value());
        }
        auto opened = consume(open_harness(*storage));
        REQUIRE(opened.has_value());
        auto harness = std::move(opened.value());
        auto root = consume(harness->root());
        REQUIRE(root.has_value());
        auto admitted = consume(harness->submit(root.value(), text_input("run the echo tool")));
        REQUIRE(admitted.has_value());
        auto settled = consume(admitted.value().wait());
        REQUIRE(settled.has_value());
        CHECK_FALSE(harness->session().failed());
        auto entries = storage->scan_entries(root.value(), 100);
        REQUIRE(entries.has_value());
        std::ranges::reverse(*entries);
        REQUIRE(entries->size() == 5);
        // pi.user carries the user model message; pi.tool-result carries the
        // tool result and the owning task id, exactly as frozen pi persists them.
        CHECK(entries->front().data.as_object()->at("model").as_array()->size() == 1);
        CHECK(entries->at(3).data.as_object()->contains("byTaskId"));
        REQUIRE(consume(harness->close()).has_value());
        REQUIRE(storage->close().has_value());

        // Reopen the serialized file and verify the same records survive the
        // durable restart, including the model payload and byTaskId.
        std::unique_ptr<Storage> reopened;
        if (backend == "jsonl") {
            auto opened_storage = JsonlStorage::open(file);
            REQUIRE(opened_storage.has_value());
            reopened = std::move(opened_storage.value());
        } else {
            auto opened_storage = SqliteStorage::open(file);
            REQUIRE(opened_storage.has_value());
            reopened = std::move(opened_storage.value());
        }
        auto restored = reopened->scan_entries(root.value(), 100);
        REQUIRE(restored.has_value());
        std::ranges::reverse(*restored);
        REQUIRE(restored->size() == 5);
        CHECK(restored->front().data.as_object()->at("model").as_array()->size() == 1);
        CHECK(restored->at(3).data.as_object()->contains("byTaskId"));
        CHECK(restored->at(3).by_task_id.has_value());
        CHECK(restored->at(3).model.size() == 1);
        const auto* final_model = restored->back().data.as_object()->at("model").as_array()->front().as_object();
        REQUIRE(final_model != nullptr);
        CHECK(*final_model->at("role").as_string() == "assistant");
        REQUIRE(reopened->close().has_value());
    }
}

} // namespace cch::tests::durable
