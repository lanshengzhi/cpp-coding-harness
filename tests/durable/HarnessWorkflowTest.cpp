#include <cch/ai/Content.hpp>
#include <cch/ai/Models.hpp>
#include <cch/durable/Harness.hpp>
#include <cch/durable/Environment.hpp>
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
#include <thread>
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

class FailingWriteEnvironment final : public Environment {
public:
    explicit FailingWriteEnvironment(std::shared_ptr<Environment> inner) : inner_(std::move(inner)) {}
    support::Expected<std::string> read(std::string_view path, std::stop_token stop) override {
        return inner_->read(path, stop);
    }
    support::ExpectedVoid write(std::string_view, std::string_view, std::stop_token) override {
        ++writes;
        return std::unexpected(support::make_error(support::ErrorCode::Workspace, "injected write failure"));
    }
    support::ExpectedVoid append(std::string_view path, std::string_view content, std::stop_token stop) override {
        return inner_->append(path, content, stop);
    }
    support::Expected<FileInfo> stat(std::string_view path, std::stop_token stop) override {
        return inner_->stat(path, stop);
    }
    support::Expected<std::vector<FileInfo>> list(std::string_view path, std::stop_token stop) override {
        return inner_->list(path, stop);
    }
    support::Expected<ShellResult> exec(const ShellCommand& command,
            std::chrono::milliseconds timeout,
            OutputSink output,
            std::stop_token stop) override {
        return inner_->exec(command, timeout, std::move(output), stop);
    }
    support::Expected<std::unique_ptr<FileWatch>> watch(
            std::string_view path, std::function<void()> callback, bool recursive) override {
        return inner_->watch(path, std::move(callback), recursive);
    }
    std::atomic_int writes{0};

private:
    std::shared_ptr<Environment> inner_;
};

TEST_CASE("Harness coding tools use the injected environment for file writes", "[durable][workflow][issue1089]") {
    tests::TempWorkspace workspace;
    Registry registry;
    auto local = local_environment(workspace.path().string());
    auto failing = std::make_shared<FailingWriteEnvironment>(local);
    bool inject_failure = false;
    SECTION("local write succeeds") {}
    SECTION("injected write fails without a filesystem fallback") { inject_failure = true; }
    REQUIRE(registry.install(coding_tools(inject_failure ? std::shared_ptr<Environment>{failing} : local)).has_value());
    int requests = 0;
    std::atomic_bool declarations_valid{true};
    HarnessOptions options{.registry = &registry};
    options.stream = [&requests, &declarations_valid, inject_failure](
                             ai::Model, ai::AiContext context, ai::SimpleStreamOptions) {
        if (context.tools.size() != 4) declarations_valid = false;
        for (const auto& tool : context.tools) {
            if (tool.description.empty() || !tool.parameters.holds<support::JsonValue::object_t>())
                declarations_valid = false;
        }
        const int turn = requests++;
        return ai::ModelStream{ai::ModelStreamProducer{
                [turn, inject_failure](ai::AssistantEventSink sink, ai::ModelStreamCompletion completion) noexcept {
                    auto message = ai::assistant_text_message("finished");
                    if (turn == 0 || (!inject_failure && turn < 5)) {
                        using Json = support::JsonValue;
                        std::vector<std::string> names{"write", "read", "edit", "read", "bash"};
                        Json::object_t args{{"path", "notes.txt"}};
                        if (turn == 0) args["content"] = "alpha\n";
                        if (turn == 2)
                            args["edits"] = Json::array_t{Json::object_t{{"oldText", "alpha"}, {"newText", "omega"}}};
                        if (turn == 4) args = Json::object_t{{"command", "cat notes.txt"}};
                        message.content = {ai::ToolCallContent{.id = std::to_string(turn),
                                .name = names[static_cast<std::size_t>(turn)],
                                .arguments = Json{std::move(args)}}};
                        message.stop_reason = ai::AssistantStopReason::ToolUse;
                    }
                    auto delivered = sink(ai::AssistantDoneEvent{.reason = message.stop_reason, .message = message});
                    if (!delivered)
                        completion(std::unexpected(delivered.error()));
                    else
                        completion(std::move(message));
                }}};
    };
    MemoryStorage storage;
    auto opened = consume(Harness::open(storage, std::move(options)));
    REQUIRE(opened.has_value());
    auto harness = std::move(opened.value());
    auto root = consume(harness->root());
    REQUIRE(root.has_value());
    auto submission = consume(harness->submit(root.value(), text_input("write")));
    REQUIRE(submission.has_value());
    auto settled = consume(submission.value().wait());
    REQUIRE(settled.has_value());
    CHECK(settled.value().status == "done");
    if (inject_failure) {
        CHECK(failing->writes == 1);
        CHECK_FALSE(std::filesystem::exists(workspace.path() / "notes.txt"));
        auto entries = storage.scan_entries(root.value(), 100);
        REQUIRE(entries);
        auto result = std::ranges::find_if(*entries, [](const auto& entry) { return entry.kind == "pi.tool-result"; });
        REQUIRE(result != entries->end());
        auto model = entry_model(*result).to_support();
        CHECK(model.at("isError").get_boolean());
        CHECK(model.at("content").get_array().at(0).at("text").get_string() == "injected write failure");
    } else {
        CHECK(workspace.read("notes.txt") == "omega\n");
        auto frozen = FrozenScenario::load("fixtures/pi-v1.3.0/audit-1056/environment.json",
                "the built-in read, write, edit and bash tools run over a real injected local environment");
        REQUIRE(frozen);
        auto entries = storage.scan_entries(root.value(), 100);
        REQUIRE(entries);
        std::ranges::reverse(*entries);
        std::vector<std::string> outputs;
        for (const auto& entry : *entries)
            if (entry.kind == "pi.tool-result")
                outputs.push_back(
                        entry_model(entry).to_support().at("content").get_array().at(0).at("text").get_string());
        const auto* tools = frozen->array_member("tools");
        REQUIRE(tools);
        REQUIRE(outputs.size() == tools->size());
        for (std::size_t i = 0; i < outputs.size(); ++i)
            CHECK(outputs[i] == tools->at(i).at("text").get_string());
    }
    CHECK(requests == (inject_failure ? 2 : 6));
    CHECK(declarations_valid);
    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("Durable local environment reports file process and watch outcomes", "[durable][environment][issue1089]") {
    using namespace std::chrono_literals;
    tests::TempWorkspace workspace;
    auto env = local_environment(workspace.path().string());
    REQUIRE(env->write("file.txt", "hel", {}));
    REQUIRE(env->append("file.txt", "lo", {}));
    auto text = env->read("file.txt", {});
    REQUIRE(text);
    CHECK(*text == "hello");
    auto missing = env->read("missing.txt", {});
    REQUIRE_FALSE(missing);
    CHECK(missing.error().detail == "not_found");
    CHECK(missing.error().message ==
            "ENOENT: no such file or directory, open '" + (workspace.path() / "missing.txt").string() + "'");
    auto info = env->stat("file.txt", {});
    REQUIRE(info);
    CHECK(info->size == 5);
    auto listing = env->list(".", {});
    REQUIRE(listing);
    CHECK(listing->size() == 1);
    auto normal =
            env->exec(std::vector<std::string>{"/bin/sh", "-c", "printf out; printf err >&2; exit 7"}, 0ms, {}, {});
    REQUIRE(normal);
    CHECK(normal->exit_code == 7);
    CHECK(normal->standard_output == "out");
    CHECK(normal->standard_error == "err");
    auto timeout = env->exec(std::vector<std::string>{"/bin/sleep", "30"}, 100ms, {}, {});
    REQUIRE_FALSE(timeout);
    CHECK(timeout.error().code == support::ErrorCode::Timeout);
    CHECK(timeout.error().message == "timeout:0.1");
    std::binary_semaphore started{0};
    std::stop_source stop;
    std::optional<support::Expected<ShellResult>> aborted;
    std::jthread worker([&] {
        aborted = env->exec(
                std::vector<std::string>{"/bin/sh", "-c", "printf ready; exec sleep 30"},
                0ms,
                [&](std::string_view, bool) { started.release(); },
                stop.get_token());
    });
    REQUIRE(started.try_acquire_for(5s));
    stop.request_stop();
    worker.join();
    REQUIRE(aborted);
    REQUIRE_FALSE(*aborted);
    CHECK(aborted->error().code == support::ErrorCode::Cancelled);
    auto recursive = env->watch("file.txt", [] {}, true);
    REQUIRE_FALSE(recursive);
    CHECK(recursive.error().code == support::ErrorCode::Validation);
    std::atomic_int calls{0};
    std::counting_semaphore<> changed{0};
    auto watch = env->watch("watched.txt", [&] {
        ++calls;
        changed.release();
    });
    REQUIRE(watch);
    workspace.write("watched.txt", "first");
    REQUIRE(changed.try_acquire_for(5s));
    (*watch)->close();
    while (changed.try_acquire()) {
    }
    const int at_close = calls;
    workspace.write("watched.txt", "second");
    CHECK_FALSE(changed.try_acquire_for(250ms));
    CHECK(calls == at_close);
    std::counting_semaphore<> directory_changed{0};
    auto directory_watch = env->watch(".", [&] { directory_changed.release(); });
    REQUIRE(directory_watch);
    workspace.write("new-child", "content");
    CHECK(directory_changed.try_acquire_for(5s));
    (*directory_watch)->close();
}

TEST_CASE("manual compaction decline makes no summary request or marker", "[durable][workflow][issue1083]") {
    const auto frozen = FrozenScenario::load("fixtures/pi-v1.3.0/audit-1083/compaction.json",
            "manual compaction decline completes without a summary request or marker");
    REQUIRE(frozen.has_value());
    Registry registry;
    MemoryStorage storage;
    std::atomic_int requests{0};
    HarnessOptions options{.registry = &registry};
    options.settings.compaction.keep_recent_tokens = 1;
    options.stream = [&requests](ai::Model, ai::AiContext, ai::SimpleStreamOptions) {
        ++requests;
        return ai::ModelStream{
                ai::ModelStreamProducer{[](ai::AssistantEventSink sink, ai::ModelStreamCompletion done) noexcept {
                    auto answer = ai::assistant_text_message("summary");
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
    const auto* definition = harness->task_definition("pi.compaction");
    REQUIRE(definition != nullptr);
    int hooks = 0;
    Extension extension{.name = "decline-compaction"};
    extension.hooks.push_back(task_hook(*definition, "beforeCompact", [&hooks](const chord::Json&, TaskRuntime&) {
        ++hooks;
        return support::AsyncResult<chord::Json>{chord::Json{chord::Json::Object{{"decline", true}}}};
    }));
    REQUIRE(registry.install(std::move(extension)).has_value());
    auto root = consume(harness->root(RootOptions{.model = ai::Model{.id = "model", .provider = "test"}}));
    REQUIRE(root.has_value());
    REQUIRE(consume(harness->commit([&](Transaction& tx) -> support::Expected<void> {
        for (const auto* text : {"old history", "recent history"}) {
            auto entry = tx.append_entry(root.value(),
                    "test.message",
                    chord::Json{chord::Json::Object{}},
                    {},
                    {chord::Json{chord::Json::Object{{"role", "user"}, {"content", text}}}});
            if (!entry) return std::unexpected(entry.error());
        }
        return {};
    })).has_value());
    REQUIRE(harness->resume().has_value());
    auto task = harness->compact(root.value());
    REQUIRE(task.has_value());
    auto finished = consume(harness->wait_for_task(*task));
    REQUIRE(finished.has_value());
    REQUIRE(finished.value().state.outcome.has_value());
    CHECK(finished.value().state.outcome->status == frozen->string_member("outcome").value());
    CHECK(hooks == frozen->number_member("hookCalls").value());
    CHECK(requests == frozen->number_member("summaryRequests").value());
    auto entries = storage.scan_entries(root.value(), 100);
    REQUIRE(entries.has_value());
    CHECK(std::ranges::count_if(*entries, [](const auto& entry) { return entry.kind == "pi.compaction"; }) ==
            frozen->number_member("markerCount").value());
    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("threshold compaction precedes the protected model request", "[durable][workflow][issue1083]") {
    const auto frozen = FrozenScenario::load("fixtures/pi-v1.3.0/audit-1083/compaction.json",
            "blocking threshold compaction appends before the protected request");
    REQUIRE(frozen.has_value());
    MemoryStorage storage;
    std::vector<std::string> requests;
    std::vector<std::string> first_users;
    std::mutex mutex;
    HarnessOptions options;
    options.settings.compaction.reserve_tokens = 30;
    options.settings.compaction.keep_recent_tokens = 1;
    options.settings.compaction.background_tokens = 0;
    options.stream = [&](ai::Model, ai::AiContext context, ai::SimpleStreamOptions) {
        const auto* system =
                context.messages.empty() ? nullptr : std::get_if<ai::SystemMessage>(&context.messages.front());
        const bool summary = system && system->content.find("context summarization assistant") != std::string::npos;
        {
            std::lock_guard lock(mutex);
            requests.push_back(summary ? "summary" : "agent");
            std::string first_user;
            for (const auto& message : context.messages) {
                const auto* user = std::get_if<ai::UserMessage>(&message);
                if (user == nullptr) continue;
                if (const auto* text = std::get_if<std::string>(&user->content)) {
                    first_user = *text;
                } else {
                    for (const auto& block : std::get<std::vector<ai::Content>>(user->content))
                        if (const auto* text = std::get_if<ai::TextContent>(&block)) first_user += text->text;
                }
                break;
            }
            first_users.push_back(std::move(first_user));
        }
        return ai::ModelStream{ai::ModelStreamProducer{
                [summary](ai::AssistantEventSink sink, ai::ModelStreamCompletion done) noexcept {
                    auto answer = ai::assistant_text_message(summary ? "compressed history" : "finished");
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
    auto root = consume(harness->root(RootOptions{
            .model = ai::Model{.id = "model", .provider = "test", .context_window = 50, .max_tokens = 100}}));
    REQUIRE(root.has_value());
    REQUIRE(consume(harness->commit([&](Transaction& tx) -> support::Expected<void> {
        for (const auto* text : {"old history that is large enough to exceed the model threshold", "recent message"}) {
            auto entry = tx.append_entry(root.value(),
                    "test.message",
                    chord::Json{chord::Json::Object{}},
                    {},
                    {chord::Json{chord::Json::Object{{"role", "user"}, {"content", text}}}});
            if (!entry) return std::unexpected(entry.error());
        }
        return {};
    })).has_value());
    auto admitted = consume(harness->submit(root.value(), text_input("continue the conversation")));
    REQUIRE(admitted.has_value());
    auto settled = consume(admitted.value().wait());
    REQUIRE(settled.has_value());
    CHECK(settled.value().status == "done");
    {
        std::lock_guard lock(mutex);
        const auto* order = frozen->array_member("requestOrder");
        REQUIRE(order != nullptr);
        REQUIRE(order->size() == requests.size());
        for (std::size_t index = 0; index < order->size(); ++index)
            CHECK(order->at(index).get_string() == requests[index]);
        REQUIRE(first_users.size() == requests.size());
        CHECK(first_users.back().find("compressed history") != std::string::npos);
        CHECK(frozen->member("requestLeadsWithSummary") != nullptr);
    }
    auto entries = storage.scan_entries(root.value(), 100);
    REQUIRE(entries.has_value());
    CHECK(std::ranges::count_if(*entries, [](const auto& entry) { return entry.kind == "pi.compaction"; }) ==
            frozen->number_member("markerCount").value());
    const auto* last_kinds = frozen->array_member("lastKinds");
    REQUIRE(last_kinds != nullptr);
    REQUIRE(entries->size() >= last_kinds->size());
    for (std::size_t index = 0; index < last_kinds->size(); ++index)
        CHECK((*entries)[last_kinds->size() - 1 - index].kind == last_kinds->at(index).get_string());
    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("background compaction allows the generation to finish before summary", "[durable][workflow][issue1083]") {
    const auto frozen = FrozenScenario::load(
            "fixtures/pi-v1.3.0/audit-1083/compaction.json", "background threshold compaction never blocks the run");
    REQUIRE(frozen.has_value());
    MemoryStorage storage;
    std::binary_semaphore summary_started{0};
    std::binary_semaphore release_summary{0};
    std::atomic_bool summary_finished{false};
    HarnessOptions options;
    options.settings.compaction.reserve_tokens = 100;
    options.settings.compaction.keep_recent_tokens = 1;
    options.settings.compaction.background_tokens = 900;
    options.stream = [&](ai::Model, ai::AiContext context, ai::SimpleStreamOptions) {
        const auto* system =
                context.messages.empty() ? nullptr : std::get_if<ai::SystemMessage>(&context.messages.front());
        const bool summary = system && system->content.find("context summarization assistant") != std::string::npos;
        return ai::ModelStream{ai::ModelStreamProducer{
                [&, summary](ai::AssistantEventSink sink, ai::ModelStreamCompletion done) noexcept {
                    if (summary) {
                        summary_started.release();
                        release_summary.acquire();
                        summary_finished = true;
                    }
                    auto answer = ai::assistant_text_message(summary ? "summary" : "finished");
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
    auto root = consume(harness->root(RootOptions{
            .model = ai::Model{.id = "model", .provider = "test", .context_window = 1000, .max_tokens = 100}}));
    REQUIRE(root.has_value());
    REQUIRE(consume(harness->commit([&](Transaction& tx) -> support::Expected<void> {
        for (const auto* text : {"old history", "recent history"}) {
            auto entry = tx.append_entry(root.value(),
                    "test.message",
                    chord::Json{chord::Json::Object{}},
                    {},
                    {chord::Json{chord::Json::Object{{"role", "user"}, {"content", text}}}});
            if (!entry) return std::unexpected(entry.error());
        }
        return {};
    })).has_value());
    auto admitted = consume(harness->submit(root.value(), text_input("continue")));
    REQUIRE(admitted.has_value());
    const bool started = summary_started.try_acquire_for(std::chrono::seconds(3));
    if (!started) release_summary.release();
    REQUIRE(started);
    auto tasks = storage.scan_tasks(100);
    REQUIRE(tasks.has_value());
    const auto compaction =
            std::ranges::find_if(*tasks, [](const TaskRecord& task) { return task.kind == "pi.compaction"; });
    REQUIRE(compaction != tasks->end());
    CHECK(compaction->background == frozen->member("background")->get_boolean());
    CHECK_FALSE(compaction->owner.has_value());
    CHECK(*compaction->input.as_object()->at("reason").as_string() == frozen->string_member("reason").value());
    auto settled = consume(admitted.value().wait());
    CHECK(settled.has_value());
    CHECK_FALSE(summary_finished.load());
    release_summary.release();
    auto finished = consume(harness->wait_for_task(compaction->id));
    REQUIRE(finished.has_value());
    REQUIRE(finished.value().state.outcome.has_value());
    CHECK(finished.value().state.outcome->status == frozen->string_member("outcome").value());
    auto entries = storage.scan_entries(root.value(), 100);
    REQUIRE(entries.has_value());
    REQUIRE(!entries->empty());
    CHECK(entries->front().kind == frozen->string_member("lastKind").value());
    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("overflow compacts before retrying the original request", "[durable][workflow][issue1083]") {
    const auto frozen = FrozenScenario::load(
            "fixtures/pi-v1.3.0/audit-1083/compaction.json", "overflow compacts before retrying the original request");
    REQUIRE(frozen.has_value());
    MemoryStorage storage;
    std::vector<std::string> requests;
    std::mutex mutex;
    std::int64_t generation_attempt_after_compaction = 0;
    HarnessOptions options;
    options.settings.compaction.keep_recent_tokens = 1;
    options.stream = [&](ai::Model, ai::AiContext context, ai::SimpleStreamOptions) {
        const auto* system =
                context.messages.empty() ? nullptr : std::get_if<ai::SystemMessage>(&context.messages.front());
        const bool summary = system && system->content.find("context summarization assistant") != std::string::npos;
        std::size_t turn;
        {
            std::lock_guard lock(mutex);
            requests.push_back(summary ? "summary" : "agent");
            turn = requests.size();
        }
        return ai::ModelStream{ai::ModelStreamProducer{[summary, turn](ai::AssistantEventSink sink,
                                                               ai::ModelStreamCompletion done) noexcept {
            auto answer = ai::assistant_text_message(summary ? "summary" : "finished");
            if (turn == 1) {
                answer.stop_reason = ai::AssistantStopReason::Error;
                answer.error_message = "arbitrary diagnostic";
                auto result = sink(ai::AssistantErrorEvent{.reason = answer.stop_reason,
                        .error = answer,
                        .inference_failure = ai::InferenceFailure{.kind = ai::InferenceFailureKind::ContextOverflow}});
                if (!result) {
                    done(std::unexpected(result.error()));
                    return;
                }
            } else {
                auto result = sink(ai::AssistantDoneEvent{.reason = answer.stop_reason, .message = answer});
                if (!result) {
                    done(std::unexpected(result.error()));
                    return;
                }
            }
            done(std::move(answer));
        }}};
    };
    auto opened = consume(Harness::open(storage, std::move(options)));
    REQUIRE(opened.has_value());
    auto harness = std::move(opened.value());
    auto root = consume(
            harness->root(RootOptions{.model = ai::Model{.id = "model", .provider = "test", .max_tokens = 100}}));
    REQUIRE(root.has_value());
    REQUIRE(consume(harness->commit([&](Transaction& tx) -> support::Expected<void> {
        for (const auto* text : {"old history", "recent history"}) {
            auto entry = tx.append_entry(root.value(),
                    "test.message",
                    chord::Json{chord::Json::Object{}},
                    {},
                    {chord::Json{chord::Json::Object{{"role", "user"}, {"content", text}}}});
            if (!entry) return std::unexpected(entry.error());
        }
        return {};
    })).has_value());
    auto admitted = consume(harness->submit(root.value(), text_input("continue")));
    REQUIRE(admitted.has_value());
    auto settled = consume(admitted.value().wait());
    REQUIRE(settled.has_value());
    CHECK(settled.value().status == "done");
    {
        std::lock_guard lock(mutex);
        std::vector<std::string> frozen_order;
        const auto* order = frozen->array_member("requestOrder");
        REQUIRE(order != nullptr);
        for (const auto& value : *order)
            frozen_order.push_back(value.get_string());
        std::vector<std::string> observed;
        for (const auto& request : requests)
            observed.push_back(request == "summary" ? "summary" : "agent");
        CHECK(observed == frozen_order);
    }
    auto tasks = storage.scan_tasks(100);
    REQUIRE(tasks.has_value());
    for (const auto& task : *tasks) {
        if (task.kind != "pi.generation" || task.conversation_id != root.value() || !task.state.checkpoint) continue;
        const auto* checkpoint = task.state.checkpoint->as_object();
        if (checkpoint == nullptr) continue;
        const auto attempt = checkpoint->find("attempt");
        if (attempt != checkpoint->end() && attempt->second.as_number() != nullptr)
            generation_attempt_after_compaction = static_cast<std::int64_t>(*attempt->second.as_number());
    }
    CHECK(generation_attempt_after_compaction ==
            static_cast<std::int64_t>(frozen->number_member("generationAttemptAfterCompaction").value()));
    auto entries = storage.scan_entries(root.value(), 100);
    REQUIRE(entries.has_value());
    CHECK(std::ranges::count_if(*entries, [](const auto& entry) { return entry.kind == "pi.compaction"; }) ==
            frozen->number_member("markerCount").value());
    const auto* last_kinds = frozen->array_member("lastKinds");
    REQUIRE(last_kinds != nullptr);
    REQUIRE(entries->size() >= last_kinds->size());
    for (std::size_t index = 0; index < last_kinds->size(); ++index)
        CHECK((*entries)[last_kinds->size() - 1 - index].kind == last_kinds->at(index).get_string());
    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("compaction retries structured failures with committed backoff and stops at budget",
        "[durable][workflow][issue1083]") {
    const auto frozen = FrozenScenario::load("fixtures/pi-v1.3.0/audit-1083/compaction.json",
            "compaction retry commits the backoff checkpoint and live retry status");
    REQUIRE(frozen.has_value());
    MemoryStorage storage;
    std::atomic_int attempts{0};
    std::binary_semaphore first{0};
    std::binary_semaphore entered{0};
    std::binary_semaphore release{0};
    HarnessOptions options;
    options.now = [] { return std::int64_t{1000}; };
    options.settings.compaction.keep_recent_tokens = 1;
    options.settings.retry.max_retries = 2;
    options.settings.retry.base_delay_ms = 200;
    options.settings.retry.max_agent_delay_ms = 1000;
    options.stream = [&](ai::Model, ai::AiContext, ai::SimpleStreamOptions) {
        return ai::ModelStream{
                ai::ModelStreamProducer{[&](ai::AssistantEventSink sink, ai::ModelStreamCompletion done) noexcept {
                    const int attempt = ++attempts;
                    if (attempt == 1) first.release();
                    if (attempt == 2) {
                        entered.release();
                        release.acquire();
                    }
                    auto answer = ai::assistant_text_message("");
                    answer.provider = "test";
                    answer.model = "model";
                    answer.usage.input = 3;
                    answer.stop_reason = ai::AssistantStopReason::Error;
                    answer.error_message = "transient";
                    auto result = sink(ai::AssistantErrorEvent{.reason = answer.stop_reason,
                            .error = answer,
                            .inference_failure = ai::InferenceFailure{.kind = ai::InferenceFailureKind::RateLimited}});
                    if (!result)
                        done(std::unexpected(result.error()));
                    else
                        done(std::move(answer));
                }}};
    };
    auto opened = consume(Harness::open(storage, std::move(options)));
    REQUIRE(opened.has_value());
    auto harness = std::move(opened.value());
    auto root = consume(
            harness->root(RootOptions{.model = ai::Model{.id = "model", .provider = "test", .max_tokens = 100}}));
    REQUIRE(root.has_value());
    REQUIRE(consume(harness->commit([&](Transaction& tx) -> support::Expected<void> {
        for (const auto* text : {"old history", "recent history"}) {
            auto entry = tx.append_entry(root.value(),
                    "test.message",
                    chord::Json{chord::Json::Object{}},
                    {},
                    {chord::Json{chord::Json::Object{{"role", "user"}, {"content", text}}}});
            if (!entry) return std::unexpected(entry.error());
        }
        return {};
    })).has_value());
    REQUIRE(harness->resume().has_value());
    auto task = harness->compact(root.value());
    REQUIRE(task.has_value());
    REQUIRE(first.try_acquire_for(std::chrono::seconds(3)));
    bool saw_retry = false;
    for (int i = 0; i < 100 && !saw_retry; ++i) {
        auto state = storage.task(*task);
        REQUIRE(state.has_value());
        if (state->has_value() && (**state).state.checkpoint) {
            const auto* object = (**state).state.checkpoint->as_object();
            if (object && object->at("phase").as_string() && *object->at("phase").as_string() == "retry") {
                CHECK(*object->at("phase").as_string() == frozen->string_member("checkpointPhase").value());
                CHECK(*object->at("attempt").as_number() == frozen->number_member("checkpointAttempt").value());
                CHECK(*object->at("until").as_number() == frozen->number_member("checkpointUntil").value());
                saw_retry = true;
            }
        }
        if (!saw_retry) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(saw_retry);
    const bool reached = entered.try_acquire_for(std::chrono::seconds(3));
    if (!reached) release.release();
    REQUIRE(reached);
    auto current = storage.task(*task);
    REQUIRE(current.has_value());
    REQUIRE(current->has_value());
    REQUIRE((**current).state.checkpoint.has_value());
    const auto& checkpoint = *(**current).state.checkpoint->as_object();
    CHECK(*checkpoint.at("phase").as_string() == "summarize");
    CHECK(*checkpoint.at("attempt").as_number() == 2);
    release.release();
    auto finished = consume(harness->wait_for_task(*task));
    REQUIRE(finished.has_value());
    REQUIRE(finished.value().state.outcome.has_value());
    CHECK(finished.value().state.outcome->status == "failed");
    CHECK(attempts == 3);
    auto entries = storage.scan_entries(root.value(), 100);
    REQUIRE(entries.has_value());
    CHECK(std::ranges::none_of(*entries, [](const auto& entry) { return entry.kind == "pi.compaction"; }));
    auto doc = storage.find_document(
            "pi.usage:v1", DocumentAddress{.conversation_id = root.value()}, DocumentPoint::current());
    REQUIRE(doc.has_value());
    REQUIRE(doc->has_value());
    auto usage = storage.document((**doc).id, DocumentPoint::current());
    REQUIRE(usage.has_value());
    REQUIRE(usage->has_value());
    const auto& models = *(**usage).value.as_object()->at("models").as_object();
    CHECK(*models.at("test/model").as_object()->at("input").as_number() == 9);
    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("generation writes a post-head system baseline with paired omissions", "[durable][workflow][issue1083]") {
    MemoryStorage storage;
    Registry registry;
    Extension section{.name = "baseline-section"};
    section.sections.push_back(PromptSection{.key = "preamble", .text = "desired", .tag = false});
    REQUIRE(registry.install(std::move(section)).has_value());
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
    EntryId old_system{};
    EntryId marker{};
    REQUIRE(consume(harness->commit([&](Transaction& tx) -> support::Expected<void> {
        auto old = tx.append_entry(root.value(),
                "pi.system",
                chord::Json{chord::Json::Object{}},
                {},
                {chord::Json{chord::Json::Object{{"role", "system"}, {"content", "old"}}}});
        if (!old) return std::unexpected(old.error());
        old_system = old->id;
        auto kept = tx.append_entry(root.value(),
                "test.message",
                chord::Json{chord::Json::Object{}},
                {},
                {chord::Json{chord::Json::Object{{"role", "user"}, {"content", "kept"}}}});
        if (!kept) return std::unexpected(kept.error());
        auto head = tx.append_entry(root.value(),
                "pi.compaction",
                chord::Json{chord::Json::Object{}},
                EntryHeadVariant{old_system},
                {chord::Json{chord::Json::Object{{"role", "user"}, {"content", "summary"}}}});
        if (!head) return std::unexpected(head.error());
        marker = head->id;
        return {};
    })).has_value());
    auto admitted = consume(harness->submit(root.value(), text_input("continue")));
    REQUIRE(admitted.has_value());
    REQUIRE(consume(admitted.value().wait()).has_value());
    auto entries = storage.scan_entries(root.value(), 100);
    REQUIRE(entries.has_value());
    auto baseline = std::ranges::find_if(
            *entries, [&](const auto& entry) { return entry.kind == "pi.system" && entry.id.value > marker.value; });
    REQUIRE(baseline != entries->end());
    REQUIRE(baseline->edits.size() == 1);
    CHECK(baseline->edits[0].target == old_system);
    CHECK(baseline->edits[0].action == "omit");
    REQUIRE(baseline->model.size() == 1);
    const auto& sections = *baseline->model[0].as_object()->at("sections").as_object();
    CHECK(*sections.at("preamble").as_string() == "desired");
    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("generation retries structured transient failures after committed retry", "[durable][workflow][issue1083]") {
    MemoryStorage storage;
    std::atomic_int requests{0};
    HarnessOptions options;
    options.settings.compaction.enabled = false;
    options.settings.retry.max_retries = 1;
    options.settings.retry.base_delay_ms = 1;
    options.stream = [&](ai::Model, ai::AiContext, ai::SimpleStreamOptions) {
        const int turn = ++requests;
        return ai::ModelStream{ai::ModelStreamProducer{[turn](ai::AssistantEventSink sink,
                                                               ai::ModelStreamCompletion done) noexcept {
            auto answer = ai::assistant_text_message("finished");
            if (turn == 1) {
                answer.stop_reason = ai::AssistantStopReason::Error;
                answer.error_message = "not used for classification";
                auto result = sink(ai::AssistantErrorEvent{.reason = answer.stop_reason,
                        .error = answer,
                        .inference_failure = ai::InferenceFailure{.kind = ai::InferenceFailureKind::RateLimited}});
                if (!result) {
                    done(std::unexpected(result.error()));
                    return;
                }
            } else {
                auto result = sink(ai::AssistantDoneEvent{.reason = answer.stop_reason, .message = answer});
                if (!result) {
                    done(std::unexpected(result.error()));
                    return;
                }
            }
            done(std::move(answer));
        }}};
    };
    auto opened = consume(Harness::open(storage, std::move(options)));
    REQUIRE(opened.has_value());
    auto harness = std::move(opened.value());
    auto root = consume(harness->root(RootOptions{.model = ai::Model{.id = "model", .provider = "test"}}));
    REQUIRE(root.has_value());
    auto admitted = consume(harness->submit(root.value(), text_input("continue")));
    REQUIRE(admitted.has_value());
    auto settled = consume(admitted.value().wait());
    REQUIRE(settled.has_value());
    CHECK(settled.value().status == "done");
    CHECK(requests == 2);
    auto entries = storage.scan_entries(root.value(), 100);
    REQUIRE(entries.has_value());
    CHECK(std::ranges::count_if(*entries, [](const auto& entry) { return entry.kind == "pi.assistant"; }) == 2);
    REQUIRE(consume(harness->close()).has_value());
}

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
