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
#include <ranges>
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

TEST_CASE("Harness rebuilds conversation context across reopen", "[durable][workflow][issue1082]") {
    TempWorkspace workspace;
    const std::filesystem::path file = workspace.path() / "reopen";
    auto storage = JsonlStorage::open(file);
    REQUIRE(storage.has_value());
    const auto open_harness = [](Storage& backend,
                                      std::atomic_int& requests,
                                      std::atomic_size_t& observed_size,
                                      std::vector<ai::MessageVariant>& observed_messages,
                                      std::mutex& observed_mutex) {
        HarnessOptions options;
        options.stream = [&requests, &observed_size, &observed_messages, &observed_mutex](
                                 ai::Model, ai::AiContext context, ai::SimpleStreamOptions) {
            requests.fetch_add(1);
            observed_size.store(context.messages.size());
            {
                std::lock_guard lock(observed_mutex);
                observed_messages = context.messages;
            }
            return ai::ModelStream{ai::ModelStreamProducer{[](ai::AssistantEventSink sink,
                                                                   ai::ModelStreamCompletion completion) noexcept {
                auto response = ai::assistant_text_message("answer");
                if (auto delivered = sink(ai::AssistantDoneEvent{.reason = response.stop_reason, .message = response});
                        !delivered)
                    completion(std::unexpected(delivered.error()));
                else
                    completion(std::move(response));
            }}};
        };
        return Harness::open(backend, std::move(options));
    };
    std::atomic_int requests{0};
    std::atomic_size_t first_context_size{0};
    std::vector<ai::MessageVariant> first_context;
    std::mutex first_context_mutex;
    auto opened =
            consume(open_harness(*storage.value(), requests, first_context_size, first_context, first_context_mutex));
    REQUIRE(opened.has_value());
    auto harness = std::move(opened.value());
    auto root = consume(harness->root());
    REQUIRE(root.has_value());
    const ConversationId root_id = root.value();
    auto first = consume(harness->submit(root_id, text_input("remember this")));
    REQUIRE(first.has_value());
    REQUIRE(consume(first.value().wait()).has_value());
    CHECK(first_context_size.load() == 1);
    REQUIRE(first_context.size() == 1);
    CHECK(ai::text_from_user_message(std::get<ai::UserMessage>(first_context.front())) == "remember this");
    REQUIRE(consume(harness->close()).has_value());
    REQUIRE(storage.value()->close().has_value());
    storage = JsonlStorage::open(file);
    REQUIRE(storage.has_value());

    std::atomic_int reopened_requests{0};
    std::atomic_size_t reopened_context_size{0};
    std::vector<ai::MessageVariant> reopened_context;
    std::mutex reopened_context_mutex;
    auto reopened = consume(open_harness(
            *storage.value(), reopened_requests, reopened_context_size, reopened_context, reopened_context_mutex));
    REQUIRE(reopened.has_value());
    harness = std::move(reopened.value());
    auto second = consume(harness->submit(root_id, text_input("continue")));
    if (!second.has_value()) UNSCOPED_INFO(second.error().message);
    REQUIRE(second.has_value());
    REQUIRE(consume(second.value().wait()).has_value());
    CHECK(requests.load() == 1);
    CHECK(reopened_requests.load() == 1);
    CHECK(reopened_context_size.load() == 3);
    REQUIRE(reopened_context.size() == 3);
    CHECK(ai::text_from_user_message(std::get<ai::UserMessage>(reopened_context.at(0))) == "remember this");
    CHECK(ai::text_from_user_message(std::get<ai::UserMessage>(reopened_context.at(2))) == "continue");
    REQUIRE(consume(harness->close()).has_value());
    REQUIRE(storage.value()->close().has_value());
}

TEST_CASE("Session preserves edited context and fork views across reopen", "[durable][workflow][issue1082]") {
    TempWorkspace workspace;
    const auto dir = workspace.path() / "context-views";
    auto storage = JsonlStorage::open(dir);
    REQUIRE(storage.has_value());
    auto opened = consume(Harness::open(*storage.value()));
    REQUIRE(opened.has_value());
    auto harness = std::move(opened.value());
    auto root = consume(harness->root());
    REQUIRE(root.has_value());
    const auto root_id = root.value();
    const auto frozen = FrozenScenario::load(
            "fixtures/pi-v1.3.0/audit-1082/contexts.json", "fork isolation and edited head contexts survive reopen");
    REQUIRE(frozen.has_value());
    const auto frozen_messages = [&](std::string_view name) {
        const auto* object = frozen->object_member(name);
        REQUIRE(object != nullptr);
        const auto found = object->find("messages");
        REQUIRE(found != object->end());
        const auto* values = found->second.get_if<support::JsonValue::array_t>();
        REQUIRE(values != nullptr);
        std::vector<chord::Json> messages;
        for (const auto& value : *values)
            messages.emplace_back(
                    chord::Json::Object{{"role", chord::Json{"user"}}, {"content", chord::Json{value.get_string()}}});
        return messages;
    };
    const auto message = [](std::string text) {
        return chord::Json{
                chord::Json::Object{{"role", chord::Json{"user"}}, {"content", chord::Json{std::move(text)}}}};
    };
    auto append = [&](ConversationId conversation,
                          std::string text,
                          std::optional<EntryHeadVariant> head = {},
                          std::vector<ContextEdit> edits = {}) {
        std::optional<EntryId> id;
        auto result = consume(harness->commit([&](Transaction& tx) -> support::Expected<void> {
            auto entry = tx.append_entry(conversation,
                    "test.message",
                    chord::Json{chord::Json::Object{}},
                    head,
                    {message(text)},
                    std::nullopt,
                    edits);
            if (!entry) return std::unexpected(entry.error());
            id = entry->id;
            return {};
        }));
        REQUIRE(result.has_value());
        REQUIRE(id.has_value());
        return *id;
    };
    auto view = [&](ConversationId conversation, std::optional<EntryId> at = {}) {
        std::vector<chord::Json> result;
        auto committed = consume(harness->commit([&](Transaction& tx) -> support::Expected<void> {
            auto messages = tx.context_messages(conversation, at);
            if (!messages) return std::unexpected(messages.error());
            result = std::move(*messages);
            return {};
        }));
        REQUIRE(committed.has_value());
        return result;
    };
    std::map<std::int64_t, EntryId> entry_ids;
    auto check_view = [&](std::string_view name, ConversationId conversation, std::optional<EntryId> at = {}) {
        const auto* expected = frozen->object_member(name);
        REQUIRE(expected != nullptr);
        const auto& ids = expected->at("entries").get_array();
        const auto& contributions = expected->at("contributions").get_array();
        auto result = consume(harness->commit([&](Transaction& tx) -> support::Expected<void> {
            auto actual = tx.context_view(conversation, at);
            if (!actual) return std::unexpected(actual.error());
            REQUIRE(actual->entries.size() == ids.size());
            REQUIRE(actual->contributions.size() == contributions.size());
            for (std::size_t i = 0; i < ids.size(); ++i) {
                const auto frozen_id = static_cast<std::int64_t>(ids[i].get_number());
                REQUIRE(entry_ids.contains(frozen_id));
                CHECK(actual->entries[i].id == entry_ids.at(frozen_id));
                const auto& expected_contribution = contributions[i].get_array();
                REQUIRE(actual->contributions[i].size() == expected_contribution.size());
                for (std::size_t j = 0; j < expected_contribution.size(); ++j)
                    CHECK(actual->contributions[i][j] == message(expected_contribution[j].get_string()));
            }
            return {};
        }));
        REQUIRE(result.has_value());
    };
    auto bind_entry = [&](std::string_view view_name, std::size_t index, EntryId id) {
        const auto* expected = frozen->object_member(view_name);
        REQUIRE(expected != nullptr);
        entry_ids.emplace(static_cast<std::int64_t>(expected->at("entries").get_array().at(index).get_number()), id);
    };
    auto definition = define_document(DocumentDefinitionBuilder{"issue1082.isolation"}.conversation(
                                              DocumentHistory::Rewindable, DocumentFork::AsOf),
            std::move_only_function<chord::Json()>{
                    [] { return chord::Json{chord::Json::Object{{"value", "initial"}}}; }});
    REQUIRE(definition.has_value());
    auto set_document = [&](ConversationId conversation, std::string value) {
        auto result = consume(harness->commit([&](Transaction& tx) -> support::Expected<void> {
            auto doc = tx.doc(*definition, DocumentAddress{.conversation_id = conversation});
            if (!doc) return std::unexpected(doc.error());
            return doc->set(chord::Path{chord::path_key("value")}, chord::Json{value});
        }));
        REQUIRE(result.has_value());
    };
    set_document(root_id, "at fork");
    const auto a = append(root_id, "a");
    const auto b = append(root_id, "b");
    bind_entry("forkBefore", 0, a);
    bind_entry("forkBefore", 1, b);
    auto fork_result = consume(harness->fork_conversation(root_id, b));
    REQUIRE(fork_result.has_value());
    const auto fork_id = fork_result.value().id;
    const auto original_fork = view(fork_id);
    REQUIRE(original_fork == frozen_messages("forkBefore"));
    auto fork_document = storage.value()->find_document(
            "issue1082.isolation", DocumentAddress{.conversation_id = fork_id}, DocumentPoint::current());
    REQUIRE(fork_document.has_value());
    REQUIRE(fork_document->has_value());
    const auto fork_document_id = fork_document->value().id;
    const auto fork_entries_before = storage.value()->scan_entries(fork_id, 100);
    REQUIRE(fork_entries_before.has_value());
    set_document(root_id, "root changed once");
    const auto later_one = append(root_id, "later one");
    bind_entry("rootBeforeForkWrite", 2, later_one);
    set_document(root_id, "root changed twice");
    const auto root_before_fork_tail = append(root_id, "later two");
    bind_entry("rootBeforeForkWrite", 3, root_before_fork_tail);
    check_view("forkAfterRoot", fork_id);
    CHECK(view(fork_id) == original_fork);
    CHECK(view(fork_id) == frozen_messages("forkAfterRoot"));
    const auto fork_entries_after = storage.value()->scan_entries(fork_id, 100);
    REQUIRE(fork_entries_after.has_value());
    REQUIRE(fork_entries_after->size() == fork_entries_before->size());
    for (std::size_t index = 0; index < fork_entries_after->size(); ++index) {
        CHECK(fork_entries_after->at(index).id == fork_entries_before->at(index).id);
        CHECK(fork_entries_after->at(index).model == fork_entries_before->at(index).model);
    }
    auto fork_document_value = storage.value()->document(fork_document_id, DocumentPoint::current());
    REQUIRE(fork_document_value.has_value());
    REQUIRE(fork_document_value->has_value());
    CHECK(fork_document_value->value().value.as_object()->at("value") == chord::Json{"at fork"});
    const auto original_root = view(root_id);
    const auto root_document = storage.value()->find_document(
            "issue1082.isolation", DocumentAddress{.conversation_id = root_id}, DocumentPoint::current());
    REQUIRE(root_document.has_value());
    REQUIRE(root_document->has_value());
    auto read_document = [&](DocumentId id) {
        auto doc = storage.value()->document(id, DocumentPoint::current());
        REQUIRE(doc.has_value());
        REQUIRE(doc->has_value());
        return doc->value().value.as_object()->at("value");
    };
    CHECK(read_document(fork_document_id) == chord::Json{*frozen->string_member("forkDoc")});
    CHECK(read_document(root_document->value().id) == chord::Json{*frozen->string_member("rootAfterRootWrites")});
    set_document(fork_id, "fork changed");
    CHECK(read_document(root_document->value().id) ==
            chord::Json{*frozen->string_member("rootAfterForkDocumentWrite")});
    CHECK(read_document(fork_document_id) == chord::Json{*frozen->string_member("forkAfterForkDocumentWrite")});
    const auto branch = append(fork_id, "branch only");
    bind_entry("forkAfter", 2, branch);
    check_view("rootBeforeForkWrite", root_id);
    check_view("forkAfter", fork_id);
    CHECK(view(root_id) == original_root);
    CHECK(view(root_id) == frozen_messages("rootAfterForkWrite"));
    const auto fork_after = view(fork_id);
    CHECK(fork_after == frozen_messages("forkAfter"));

    const auto omit = append(root_id, "omit marker", {}, {{.target = a, .action = "omit"}});
    bind_entry("omitted", 4, omit);
    check_view("omitted", root_id, omit);
    CHECK(view(root_id, omit) == frozen_messages("omitted"));
    const auto replace = append(
            root_id, "replace marker", {}, {{.target = a, .action = "replace", .messages = {message("replacement")}}});
    bind_entry("replaced", 5, replace);
    check_view("replaced", root_id, replace);
    const auto replaced = view(root_id, replace);
    CHECK(replaced == frozen_messages("replaced"));
    auto historical_fork = consume(harness->fork_conversation(root_id, replace));
    REQUIRE(historical_fork.has_value());
    check_view("forkAtReplace", historical_fork.value().id);
    CHECK(view(historical_fork.value().id) == replaced);
    append(root_id, "old head", EntryHeadVariant{b});
    const auto marker = append(root_id,
            "new head",
            EntryHeadVariant{a},
            {{.target = b, .action = "replace", .messages = {message("edited b")}}});
    bind_entry("headed", 0, marker);
    check_view("headed", root_id);
    const auto headed = view(root_id);
    CHECK(headed == frozen_messages("headed"));
    auto headed_fork = consume(harness->fork_conversation(root_id, marker));
    REQUIRE(headed_fork.has_value());
    check_view("forkAtHead", headed_fork.value().id);
    CHECK(view(headed_fork.value().id) == headed);
    REQUIRE(consume(harness->close()).has_value());
    REQUIRE(consume(harness->session().close()).has_value());
    storage.value().reset();
    storage = JsonlStorage::open(dir);
    REQUIRE(storage.has_value());
    opened = consume(Harness::open(*storage.value()));
    REQUIRE(opened.has_value());
    harness = std::move(opened.value());
    check_view("forkAfter", fork_id);
    check_view("headed", root_id);
    check_view("rootBeforeForkWrite", root_id, root_before_fork_tail);
    check_view("omitted", root_id, omit);
    check_view("replaced", root_id, replace);
    check_view("forkAtReplace", historical_fork.value().id);
    check_view("forkAtHead", headed_fork.value().id);
    CHECK(view(fork_id) == fork_after);
    CHECK(view(root_id) == headed);
    CHECK(view(root_id, b) == original_fork);
    CHECK(view(root_id, root_before_fork_tail) == original_root);
    fork_document_value = storage.value()->document(fork_document_id, DocumentPoint::current());
    REQUIRE(fork_document_value.has_value());
    REQUIRE(fork_document_value->has_value());
    CHECK(fork_document_value->value().value.as_object()->at("value") ==
            chord::Json{*frozen->string_member("forkAfterForkDocumentWrite")});
    CHECK(read_document(root_document->value().id) ==
            chord::Json{*frozen->string_member("rootAfterForkDocumentWrite")});
    CHECK(view(root_id, omit) == frozen_messages("omitted"));
    CHECK(view(root_id, replace) == replaced);
    CHECK(view(historical_fork.value().id) == replaced);
    CHECK(view(headed_fork.value().id) == headed);
    REQUIRE(consume(harness->close()).has_value());
    REQUIRE(consume(harness->session().close()).has_value());
}

TEST_CASE("Harness forks selected ancestry, copies as-of documents and resets with handoff",
        "[durable][workflow][issue1082]") {
    const auto fork_frozen = FrozenScenario::load("fixtures/pi-v1.3.0/audit-1056/conversations.json",
            "a fork copies the transcript and agent state as of its entry and stays independent");
    REQUIRE(fork_frozen.has_value());
    const auto reset_frozen = FrozenScenario::load("fixtures/pi-v1.3.0/audit-1056/conversations.json",
            "reset starts a new context and carries its handoff text as the first user message");
    REQUIRE(reset_frozen.has_value());
    TempWorkspace workspace;
    const std::filesystem::path dir = workspace.path() / "fork-reset";

    std::atomic_int requests{0};
    HarnessOptions options;
    options.now = [] { return std::int64_t{7}; };
    std::mutex contexts_mutex;
    std::vector<std::vector<ai::MessageVariant>> observed_contexts;
    options.stream = [&requests, &contexts_mutex, &observed_contexts](
                             ai::Model, ai::AiContext context, ai::SimpleStreamOptions) {
        const int turn = requests++;
        {
            std::lock_guard lock(contexts_mutex);
            observed_contexts.push_back(context.messages);
        }
        return ai::ModelStream{ai::ModelStreamProducer{[turn](ai::AssistantEventSink sink,
                                                               ai::ModelStreamCompletion completion) noexcept {
            auto response = ai::assistant_text_message(turn == 0 ? "a1" : "a2");
            if (auto delivered = sink(ai::AssistantDoneEvent{.reason = response.stop_reason, .message = response});
                    !delivered)
                completion(std::unexpected(delivered.error()));
            else
                completion(std::move(response));
        }}};
    };
    auto opened_storage = JsonlStorage::open(dir);
    REQUIRE(opened_storage.has_value());
    std::unique_ptr<JsonlStorage> storage = std::move(*opened_storage);
    auto opened = consume(Harness::open(*storage, std::move(options)));
    REQUIRE(opened.has_value());
    auto harness = std::move(opened.value());
    auto root = consume(harness->root());
    REQUIRE(root.has_value());
    const ConversationId root_id = root.value();

    auto first = consume(harness->submit(root_id, text_input("u1")));
    if (!first.has_value()) {
        INFO(first.error().message);
        FAIL("submit failed");
    }
    auto first_receipt = consume(first.value().wait());
    REQUIRE(first_receipt.has_value());
    if (!first_receipt.has_value()) {
        const bool closed = consume(harness->close()).has_value();
        FAIL(first_receipt.error().message + " close=" + std::to_string(closed));
    }
    const auto root_entries = storage->scan_entries(root_id, 100);
    if (!root_entries.has_value()) {
        INFO(root_entries.error().message);
        FAIL("scan failed");
    }
    if (root_entries->size() != 2) FAIL("unexpected entries");
    const EntryId first_turn = root_entries->front().id;

    auto fork = consume(harness->fork_conversation(root_id, first_turn));
    REQUIRE(fork.has_value());
    if (!fork.has_value()) {
        REQUIRE(consume(harness->close()).has_value());
        FAIL(fork.error().message);
    }
    const ConversationId fork_id = fork.value().id;
    CHECK(fork.value().parent == ConversationRecord::Parent{.conversation_id = root_id, .at = first_turn});
    auto inherited = storage->scan_entries(fork_id, 100);
    REQUIRE(inherited.has_value());
    REQUIRE(inherited->size() == 2);
    CHECK(inherited->front().id == first_turn);

    auto later = consume(harness->submit(root_id, text_input("u2")));
    REQUIRE(later.has_value());
    REQUIRE(consume(later.value().wait()).has_value());
    auto branch = consume(harness->submit(fork_id, text_input("branch")));
    REQUIRE(branch.has_value());
    REQUIRE(consume(branch.value().wait()).has_value());
    auto original = storage->scan_entries(root_id, 100);
    auto fork_entries = storage->scan_entries(fork_id, 100);
    REQUIRE(original.has_value());
    REQUIRE(fork_entries.has_value());
    const auto* root_expected = fork_frozen->array_member("rootEntries");
    REQUIRE(root_expected != nullptr);
    std::ranges::reverse(*original);
    {
        std::string kinds;
        for (const auto& entry : *original) {
            if (!kinds.empty()) kinds += ",";
            kinds += entry.kind;
        }
        INFO(kinds);
    }
    CHECK(json_equals(transcript(*original), *root_expected));
    CHECK(std::ranges::any_of(
            *fork_entries, [first_turn](const EntryRecord& entry) { return entry.id == first_turn; }));
    CHECK(std::ranges::any_of(*fork_entries, [](const EntryRecord& entry) {
        return entry.kind == "pi.user" && entry.data.as_object()
                                                          ->at("model")
                                                          .as_array()
                                                          ->front()
                                                          .as_object()
                                                          ->at("content")
                                                          .as_array()
                                                          ->front()
                                                          .as_object()
                                                          ->at("text") == chord::Json{"branch"};
    }));
    const auto* distinct = fork_frozen->member("forksAreDistinct");
    const auto* preserved = fork_frozen->member("rootStillHasTwoTurns");
    REQUIRE(distinct != nullptr);
    REQUIRE(preserved != nullptr);
    CHECK(fork_id != root_id);
    CHECK(original->size() == 4);
    CHECK(fork_entries->size() == 4);

    // The selected entry's commit fixes the source point. The as-of document
    // also proves the fork copied the agent state as of the selected entry,
    // and the retained prompt deltas prove reset does not replay them.
    // is committed with a later source revision before the fork is made.
    auto doc_definition = define_document(
            DocumentDefinitionBuilder{"issue1082.as-of"}.conversation(DocumentHistory::Rewindable, DocumentFork::AsOf),
            std::move_only_function<chord::Json()>{
                    [] { return chord::Json{chord::Json::Object{{"value", chord::Json{"initial"}}}}; }});
    REQUIRE(doc_definition.has_value());
    std::optional<EntryId> selected;
    REQUIRE(consume(harness->commit([&](Transaction& tx) -> support::Expected<void> {
        auto marker = tx.append_entry(root_id, "issue1082.marker", chord::Json{chord::Json::Object{}});
        if (!marker) return std::unexpected(marker.error());
        selected = marker->id;
        auto doc = tx.doc(*doc_definition, DocumentAddress{.conversation_id = root_id});
        if (!doc) return std::unexpected(doc.error());
        return doc->set(chord::Path{chord::path_key("value")}, chord::Json{"at-selected"});
    })).has_value());
    REQUIRE(selected.has_value());
    REQUIRE(consume(harness->commit([&](Transaction& tx) -> support::Expected<void> {
        auto doc = tx.doc(*doc_definition, DocumentAddress{.conversation_id = root_id});
        if (!doc) return std::unexpected(doc.error());
        return doc->set(chord::Path{chord::path_key("value")}, chord::Json{"current"});
    })).has_value());
    auto doc_fork = consume(harness->fork_conversation(root_id, *selected));
    REQUIRE(doc_fork.has_value());
    const ConversationId doc_fork_id = doc_fork.value().id;
    auto fork_agent_doc = storage->find_document(
            "pi.agent:v1", DocumentAddress{.conversation_id = doc_fork_id}, DocumentPoint::current());
    REQUIRE(fork_agent_doc.has_value());
    REQUIRE(fork_agent_doc->has_value());
    auto fork_agent = storage->document(fork_agent_doc->value().id, DocumentPoint::current());
    REQUIRE(fork_agent.has_value());
    REQUIRE(fork_agent->has_value());
    const auto* fork_agent_object = fork_agent->value().value.as_object();
    REQUIRE(fork_agent_object != nullptr);
    CHECK_FALSE(fork_agent_object->contains("thinkingLevel"));
    auto copied = storage->find_document(
            doc_definition->kind(), DocumentAddress{.conversation_id = doc_fork_id}, DocumentPoint::current());
    REQUIRE(copied.has_value());
    REQUIRE(copied->has_value());
    auto copied_value = storage->document((*copied)->id, DocumentPoint::current());
    REQUIRE(copied_value.has_value());
    REQUIRE(copied_value->has_value());
    CHECK((*copied_value)->value.as_object()->at("value") == chord::Json{"at-selected"});
    CHECK((*copied_value)->value.as_object()->at("value") != chord::Json{"current"});

    const auto before_reset = storage->scan_entries(root_id, 100);
    REQUIRE(before_reset.has_value());
    REQUIRE(before_reset->size() == 5);
    REQUIRE(consume(harness->reset(root_id, std::string{"carry this forward"})).has_value());
    auto after_reset = consume(harness->submit(root_id, text_input("u2")));
    REQUIRE(after_reset.has_value());
    REQUIRE(consume(after_reset.value().wait()).has_value());
    // A discriminating reopen at the fork: rebuilding this branch starts from
    // the fork's copied state, not from any later root writes.
    // Release the backend only after the Session close settles, then reopen
    // the same directory with a fresh backend and Harness.
    REQUIRE(consume(harness->close()).has_value());
    REQUIRE(consume(harness->session().close()).has_value());
    storage.reset();
    auto reopened_storage = JsonlStorage::open(dir);
    REQUIRE(reopened_storage.has_value());
    storage = std::move(*reopened_storage);
    std::atomic_int fork_requests{0};
    std::atomic_size_t fork_context_size{0};
    std::vector<ai::MessageVariant> fork_context;
    std::mutex fork_context_mutex;
    {
        HarnessOptions fork_options;
        fork_options.stream = [&fork_requests, &fork_context_size, &fork_context, &fork_context_mutex](
                                      ai::Model, ai::AiContext context, ai::SimpleStreamOptions) {
            fork_requests.fetch_add(1);
            fork_context_size.store(context.messages.size());
            {
                std::lock_guard lock(fork_context_mutex);
                fork_context = context.messages;
            }
            return ai::ModelStream{ai::ModelStreamProducer{[](ai::AssistantEventSink sink,
                                                                   ai::ModelStreamCompletion completion) noexcept {
                auto response = ai::assistant_text_message("fork reply");
                if (auto delivered = sink(ai::AssistantDoneEvent{.reason = response.stop_reason, .message = response});
                        !delivered)
                    completion(std::unexpected(delivered.error()));
                else
                    completion(std::move(response));
            }}};
        };
        auto reopened = consume(Harness::open(*storage, std::move(fork_options)));
        REQUIRE(reopened.has_value());
        harness = std::move(reopened.value());
        auto forked = consume(harness->submit(doc_fork_id, text_input("continue from the fork")));
        if (!forked.has_value()) {
            INFO(std::string{"fork submit failed: "} + forked.error().message);
            FAIL("fork submit failed");
        }
        REQUIRE(forked.has_value());
        REQUIRE(consume(forked.value().wait()).has_value());
        CHECK(fork_requests.load() == 1);
        // The doc fork descends from the root conversation at the selected
        // entry, so reopened replay rebuilds the root ancestry (u1/a1/u2/a2)
        // and appends the new user turn.
        REQUIRE(fork_context.size() == 5);
        CHECK(ai::text_from_user_message(std::get<ai::UserMessage>(fork_context.at(0))) == "u1");
        CHECK(std::get<ai::TextContent>(std::get<ai::AssistantMessage>(fork_context.at(1)).content.front()).text ==
                "a1");
        CHECK(ai::text_from_user_message(std::get<ai::UserMessage>(fork_context.at(2))) == "u2");
        CHECK(std::get<ai::TextContent>(std::get<ai::AssistantMessage>(fork_context.at(3)).content.front()).text ==
                "a2");
        CHECK(ai::text_from_user_message(std::get<ai::UserMessage>(fork_context.at(4))) == "continue from the fork");
    }
    REQUIRE(observed_contexts.size() == 4);
    // The last captured pre-reopen context is the post-reset turn: the
    // handoff user message restarts the context, followed by the new user
    // turn (frozen reset scenario contextTexts minus the system row).
    const auto& reset_context = observed_contexts.back();
    REQUIRE(reset_context.size() == 2);
    CHECK(ai::text_from_user_message(std::get<ai::UserMessage>(reset_context.at(0))) == "carry this forward");
    CHECK(ai::text_from_user_message(std::get<ai::UserMessage>(reset_context.at(1))) == "u2");
    std::vector<chord::Json> replayed;
    auto final_messages = harness->commit([&replayed](Transaction& tx) -> support::Expected<void> {
        auto view = tx.context_view(ConversationId{.value = 1});
        if (!view) return std::unexpected(view.error());
        replayed = std::move(view->messages);
        return {};
    });
    REQUIRE(consume(std::move(final_messages)).has_value());
    CHECK(observed_contexts.at(0).size() == 1);
    CHECK(observed_contexts.at(1).size() == 3);
    CHECK(observed_contexts.at(2).size() == 3);
    const auto* expected_roles = reset_frozen->array_member("contextRoles");
    const auto* expected_texts = reset_frozen->array_member("contextTexts");
    REQUIRE(expected_roles != nullptr);
    REQUIRE(expected_texts != nullptr);
    // The committed baseline is part of the shared context view.
    REQUIRE(expected_roles->size() == replayed.size());
    REQUIRE(expected_texts->size() == replayed.size());
    CHECK(expected_roles->front().get_string() == "system");
    CHECK(expected_texts->front().get_string() == "system");
    for (std::size_t index = 0; index < replayed.size(); ++index) {
        const std::size_t expected_index = index;
        const auto* message = replayed.at(index).as_object();
        REQUIRE(message != nullptr);
        CHECK(message->at("role") == chord::Json{expected_roles->at(expected_index).get_string()});
        std::string text;
        if (const auto content = message->find("content"); content != message->end()) {
            if (const auto* string = content->second.as_string())
                text = *string;
            else if (const auto* blocks = content->second.as_array())
                for (const chord::Json& block : *blocks) {
                    if (const auto* fields = block.as_object(); fields != nullptr) {
                        if (const auto value = fields->find("text");
                                value != fields->end() && value->second.as_string())
                            text += *value->second.as_string();
                    }
                }
        }
        if (expected_index == 0) {
            CHECK(text.empty());
        } else {
            CHECK(chord::Json{text} == chord::Json{expected_texts->at(expected_index).get_string()});
        }
    }
    auto final_entries = storage->scan_entries(root_id, 100);
    REQUIRE(final_entries.has_value());
    const auto reset_entry =
            std::ranges::find_if(*final_entries, [](const EntryRecord& entry) { return entry.kind == "pi.reset"; });
    REQUIRE(reset_entry != final_entries->end());
    CHECK(reset_entry->head == reset_entry->id);
    const auto* reset_spec = reset_frozen->array_member("reset");
    REQUIRE(reset_spec != nullptr);
    CHECK(reset_spec->size() == 1);
    const auto* reset_spec_object = reset_spec->front().get_if<support::JsonValue::object_t>();
    REQUIRE(reset_spec_object != nullptr);
    const auto expected_self_head = reset_spec_object->find("headIsSelf");
    REQUIRE(expected_self_head != reset_spec_object->end());
    const auto* expected_bool = expected_self_head->second.get_if<bool>();
    REQUIRE(expected_bool != nullptr);
    CHECK(*expected_bool);
    CHECK(reset_entry->model.size() == 1);
    const auto* reset_record = reset_entry->model.front().as_object();
    REQUIRE(reset_record != nullptr);
    CHECK(reset_spec->size() == 1);
    const auto reset_role = reset_record->find("role");
    REQUIRE(reset_role != reset_record->end());
    REQUIRE(reset_role->second.as_string() != nullptr);
    CHECK(*reset_role->second.as_string() == "user");
    const auto reset_content = reset_record->find("content");
    REQUIRE(reset_content != reset_record->end());
    const auto* reset_text_value = reset_content->second.as_string();
    REQUIRE(reset_text_value != nullptr);
    CHECK(*reset_text_value == "carry this forward");
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
