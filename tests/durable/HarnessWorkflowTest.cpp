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
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace cch::durable::testing {
std::shared_ptr<Environment> local_environment_before_rename(std::string cwd, std::move_only_function<void()> hook);
} // namespace cch::durable::testing

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
            std::string_view path, FileChangeSink callback, bool recursive) override {
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

TEST_CASE("Durable writes remain anchored when paths are swapped", "[durable][environment][issue1160]") {
    namespace fs = std::filesystem;
    tests::TempWorkspace workspace;
    auto root = workspace.path();
    fs::create_directories(root / "safe");
    fs::create_directories(root / "outside");
    workspace.write("safe/file", "original");
    workspace.write("outside/file", "protected");
    SECTION("parent replaced with symlink") {
        auto env = cch::durable::testing::local_environment_before_rename(root.string(), [&] {
            fs::rename(root / "safe", root / "moved");
            fs::create_directory_symlink(root / "outside", root / "safe");
        });
        REQUIRE(env->write("safe/file", "new", {}));
        CHECK(workspace.read("outside/file") == "protected");
        CHECK(workspace.read("moved/file") == "new");
    }
    SECTION("final component replaced with symlink") {
        auto env = cch::durable::testing::local_environment_before_rename(root.string(), [&] {
            fs::remove(root / "safe/file");
            fs::create_symlink(root / "outside/file", root / "safe/file");
        });
        REQUIRE(env->write("safe/file", "new", {}));
        CHECK(workspace.read("outside/file") == "protected");
        CHECK(workspace.read("safe/file") == "new");
        CHECK_FALSE(fs::is_symlink(root / "safe/file"));
    }
    SECTION("parent and final symlinks are rejected before writing") {
        fs::create_directory_symlink(root / "outside", root / "link");
        fs::create_symlink(root / "outside/file", root / "safe/link");
        auto env = local_environment(root.string());
        CHECK_FALSE(env->write("link/file", "new", {}));
        CHECK_FALSE(env->append("link/file", "new", {}));
        CHECK_FALSE(env->write("safe/link", "new", {}));
        CHECK_FALSE(env->append("safe/link", "new", {}));
        CHECK(workspace.read("outside/file") == "protected");
    }
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
    std::string streamed;
    auto normal = env->exec(std::vector<std::string>{"/bin/sh", "-c", "printf out; printf err >&2; exit 7"},
            0ms,
            // The sink owns move-only state; streamed outlives this synchronous call.
            [chunks = std::make_unique<int>(0), &streamed](std::string_view text, bool) {
                ++*chunks;
                streamed += text;
            },
            {});
    REQUIRE(normal);
    CHECK(normal->exit_code == 7);
    CHECK(normal->standard_output == "out");
    CHECK(normal->standard_error == "err");
    CHECK(streamed == normal->output);
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
    // close() joins delivery before these borrowed locals leave scope.
    auto watch = env->watch("watched.txt", [count = std::make_unique<int>(0), &calls, &changed] {
        calls = ++*count;
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

TEST_CASE("compaction tool text follows frozen UTF-16 limits through the model request",
        "[durable][compaction][issue1083][review1044]") {
    const auto repeat = [](std::string_view unit, std::size_t count) {
        std::string text;
        for (std::size_t index = 0; index < count; ++index)
            text += unit;
        return text;
    };
    struct Example {
        std::string input;
        std::string expected;
    };
    // Observed from frozen serializeConversation, encoded as UTF-8 like Node's
    // Buffer.from: a sliced high surrogate becomes U+FFFD. Counts are UTF-16
    // units, so one emoji occupies two and a CJK character occupies one.
    const std::vector<Example> examples{
            {.input = std::string(2000, 'a'), .expected = std::string(2000, 'a')},
            {.input = std::string(2001, 'a'),
                    .expected = std::string(2000, 'a') + "\n\n[... 1 more characters truncated]"},
            {.input = repeat("中", 2000), .expected = repeat("中", 2000)},
            {.input = repeat("中", 2001), .expected = repeat("中", 2000) + "\n\n[... 1 more characters truncated]"},
            {.input = repeat("🙂", 1000), .expected = repeat("🙂", 1000)},
            {.input = repeat("🙂", 1001), .expected = repeat("🙂", 1000) + "\n\n[... 2 more characters truncated]"},
            {.input = std::string(1999, 'a') + "🙂",
                    .expected = std::string(1999, 'a') + "�\n\n[... 1 more characters truncated]"},
    };
    for (const auto& example : examples) {
        INFO(example.input.size());
        MemoryStorage storage;
        std::string prompt;
        HarnessOptions options;
        options.settings.compaction.keep_recent_tokens = 1;
        // wait_for_task below observes settlement after this task's model call,
        // keeping prompt alive and synchronizing its write before inspection.
        options.stream = [&prompt](ai::Model, ai::AiContext context, ai::SimpleStreamOptions) {
            for (const auto& message : context.messages) {
                if (const auto* user = std::get_if<ai::UserMessage>(&message)) {
                    if (const auto* text = std::get_if<std::string>(&user->content))
                        prompt += *text;
                    else
                        for (const auto& block : std::get<std::vector<ai::Content>>(user->content))
                            if (const auto* text = std::get_if<ai::TextContent>(&block)) prompt += text->text;
                }
            }
            return ai::ModelStream{
                    ai::ModelStreamProducer{[](ai::AssistantEventSink sink, ai::ModelStreamCompletion done) noexcept {
                        auto answer = ai::assistant_text_message("summary");
                        auto delivered = sink(ai::AssistantDoneEvent{.reason = answer.stop_reason, .message = answer});
                        if (!delivered)
                            done(std::unexpected(delivered.error()));
                        else
                            done(std::move(answer));
                    }}};
        };
        auto opened = consume(Harness::open(storage, std::move(options)));
        REQUIRE(opened.has_value());
        auto harness = std::move(opened.value());
        auto root = consume(harness->root(RootOptions{.model = ai::Model{.id = "model", .provider = "test"}}));
        REQUIRE(root.has_value());
        auto appended = consume(harness->session().commit(
                [id = root.value(), text = example.input](Transaction& tx) -> support::ExpectedVoid {
                    using Json = chord::Json;
                    const auto history = tx.append_entry(id,
                            "test.history",
                            Json{Json::Object{}},
                            {},
                            {
                                    Json{Json::Object{{"role", "assistant"},
                                            {"content",
                                                    Json::Array{Json{Json::Object{{"type", "toolCall"},
                                                            {"id", "call"},
                                                            {"name", "echo"},
                                                            {"arguments", Json::Object{}}}}}}}},
                                    Json{Json::Object{{"role", "toolResult"},
                                            {"toolCallId", "call"},
                                            {"toolName", "echo"},
                                            {"content", text}}},
                            });
                    if (!history) return std::unexpected(history.error());
                    const auto recent = tx.append_entry(id,
                            "test.message",
                            Json{Json::Object{}},
                            {},
                            {
                                    Json{Json::Object{{"role", "user"}, {"content", "recent history"}}},
                            });
                    if (!recent) return std::unexpected(recent.error());
                    return {};
                }));
        REQUIRE(appended.has_value());
        REQUIRE(harness->resume().has_value());
        auto task = harness->compact(root.value());
        REQUIRE(task.has_value());
        auto settled = consume(harness->wait_for_task(*task));
        const auto closed = consume(harness->close());
        REQUIRE(settled.has_value());
        CHECK(closed.has_value());
        CHECK(prompt.starts_with("<conversation>\n[Assistant tool calls]: echo()\n\n[Tool result]: " +
                                 example.expected + "\n</conversation>"));
    }
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
    REQUIRE(consume(harness->session().commit([&](Transaction& tx) -> support::Expected<void> {
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
    REQUIRE(consume(harness->session().commit([&](Transaction& tx) -> support::Expected<void> {
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
    REQUIRE(consume(harness->session().commit([&](Transaction& tx) -> support::Expected<void> {
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
    REQUIRE(consume(harness->session().commit([&](Transaction& tx) -> support::Expected<void> {
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
    REQUIRE(consume(harness->session().commit([&](Transaction& tx) -> support::Expected<void> {
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
    REQUIRE(consume(harness->session().commit([&](Transaction& tx) -> support::Expected<void> {
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
        auto result = consume(harness->session().commit([&](Transaction& tx) -> support::Expected<void> {
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
        auto committed = consume(harness->session().commit([&](Transaction& tx) -> support::Expected<void> {
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
        auto result = consume(harness->session().commit([&](Transaction& tx) -> support::Expected<void> {
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
        auto result = consume(harness->session().commit([&](Transaction& tx) -> support::Expected<void> {
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
    REQUIRE(consume(harness->session().commit([&](Transaction& tx) -> support::Expected<void> {
        auto marker = tx.append_entry(root_id, "issue1082.marker", chord::Json{chord::Json::Object{}});
        if (!marker) return std::unexpected(marker.error());
        selected = marker->id;
        auto doc = tx.doc(*doc_definition, DocumentAddress{.conversation_id = root_id});
        if (!doc) return std::unexpected(doc.error());
        return doc->set(chord::Path{chord::path_key("value")}, chord::Json{"at-selected"});
    })).has_value());
    REQUIRE(selected.has_value());
    REQUIRE(consume(harness->session().commit([&](Transaction& tx) -> support::Expected<void> {
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
    auto final_messages = harness->session().commit([&replayed](Transaction& tx) -> support::Expected<void> {
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

namespace {

constexpr std::string_view kRegistryHooksArtifact = "fixtures/pi-v1.3.0/audit-1087/registry-hooks.json";

/// One scripted model stream that delivers `message` and completes.
ai::ModelStream scripted_reply(ai::AssistantMessage message) {
    return ai::ModelStream{ai::ModelStreamProducer{
            [message = std::move(message)](ai::AssistantEventSink sink, ai::ModelStreamCompletion done) noexcept {
                auto delivered = sink(ai::AssistantDoneEvent{.reason = message.stop_reason, .message = message});
                if (!delivered)
                    done(std::unexpected(delivered.error()));
                else
                    done(message);
            }}};
}

/// One scripted model response that calls `name` with `arguments`.
ai::AssistantMessage call_message(std::string name, support::JsonValue arguments, std::string id = "call-1") {
    auto message = ai::assistant_text_message("");
    message.content = {
            ai::ToolCallContent{.id = std::move(id), .name = std::move(name), .arguments = std::move(arguments)}};
    message.stop_reason = ai::AssistantStopReason::ToolUse;
    return message;
}

/// The `text` member of a hook's `call.arguments` object.
std::string argument_text(const chord::Json& call) {
    const auto* fields = call.as_object();
    REQUIRE(fields != nullptr);
    const auto arguments = fields->find("arguments");
    REQUIRE(arguments != fields->end());
    const auto* text = arguments->second.as_object()->at("text").as_string();
    REQUIRE(text != nullptr);
    return *text;
}

/// The text blocks of an assistant message as one `|`-joined string.
std::string message_text(const chord::Json& message) {
    const auto* fields = message.as_object();
    REQUIRE(fields != nullptr);
    const auto content = fields->find("content");
    REQUIRE(content != fields->end());
    if (const auto* text = content->second.as_string()) return *text;
    std::string joined;
    for (const auto& block : *content->second.as_array()) {
        const auto* block_fields = block.as_object();
        REQUIRE(block_fields != nullptr);
        if (const auto* text = block_fields->at("text").as_string()) {
            if (!joined.empty()) joined += "|";
            joined += *text;
        }
    }
    return joined;
}

/// The `echo` tool of the frozen `audit-1087` scenarios, recording what it executed.
ToolRegistration echo_tool(std::vector<std::string>* executed) {
    return ToolRegistration{.declaration = ai::Tool{.name = "echo", .description = "Echo the text argument back"},
            .execute = [executed](support::JsonValue arguments, std::stop_token) {
                const std::string text = arguments.at("text").get_string();
                if (executed != nullptr) executed->push_back(text);
                return support::AsyncResult<std::vector<ai::Content>>{
                        std::vector<ai::Content>{ai::text_content("echo:" + text)}};
            }};
}

/// Install one extension that registers a single `pi.tool` hook.
void install_tool_hook(Registry& registry, std::string name, std::string hook_name, TaskHook handler) {
    Extension extension{.name = std::move(name)};
    extension.hooks.push_back(tool_hook(std::move(hook_name), std::move(handler)));
    REQUIRE(registry.install(std::move(extension)).has_value());
}

/// The conversation entries of `conversation`, oldest first.
std::vector<EntryRecord> entries_oldest_first(Storage& storage, ConversationId conversation) {
    auto scanned = storage.scan_entries(conversation, 1000);
    REQUIRE(scanned.has_value());
    std::ranges::reverse(*scanned);
    return *scanned;
}

} // namespace

TEST_CASE("Workflow onYield continues the admitted run under a successor generation task",
        "[durable][workflow][issue1087]") {
    const auto frozen = FrozenScenario::load(kRegistryHooksArtifact,
            "a generation onYield continuation hands the admitted run to a successor generation task");
    REQUIRE(frozen.has_value());
    Registry registry;
    std::vector<std::string> yields;
    std::vector<std::string> second_users;
    std::atomic_int calls{0};
    std::binary_semaphore first_requested{0};
    std::binary_semaphore release_first{0};
    std::binary_semaphore second_requested{0};
    std::binary_semaphore release_second{0};
    Extension hooks{.name = "audit-hooks"};
    hooks.hooks.push_back(task_hook("pi.generation", "onYield", [&yields](const chord::Json& answer, TaskRuntime&) {
        const std::string text = message_text(answer);
        yields.push_back(text);
        // Exactly one continuation, decided by the first answer (frozen
        // `onYield` returning `{ continue }`).
        if (text != "first answer") return support::AsyncResult<chord::Json>{chord::Json{}};
        return support::AsyncResult<chord::Json>{chord::Json{chord::Json::Object{{"continue", chord::Json{"go on"}}}}};
    }));
    REQUIRE(registry.install(std::move(hooks)).has_value());
    MemoryStorage storage;
    HarnessOptions options{.registry = &registry};
    options.stream = [&](ai::Model, ai::AiContext context, ai::SimpleStreamOptions) {
        const int turn = calls.fetch_add(1);
        if (turn == 0) {
            first_requested.release();
            return ai::ModelStream{ai::ModelStreamProducer{
                    [&release_first](ai::AssistantEventSink sink, ai::ModelStreamCompletion done) noexcept {
                        if (!release_first.try_acquire_for(std::chrono::seconds{10})) {
                            done(std::unexpected(
                                    support::make_error(support::ErrorCode::Cancelled, "first request timed out")));
                            return;
                        }
                        auto answer = ai::assistant_text_message("first answer");
                        auto delivered = sink(ai::AssistantDoneEvent{.reason = answer.stop_reason, .message = answer});
                        if (!delivered)
                            done(std::unexpected(delivered.error()));
                        else
                            done(std::move(answer));
                    }}};
        }
        for (const auto& message : context.messages) {
            if (const auto* user = std::get_if<ai::UserMessage>(&message))
                second_users.push_back(ai::text_from_user_message(*user));
        }
        second_requested.release();
        return ai::ModelStream{ai::ModelStreamProducer{[&release_second](ai::AssistantEventSink sink,
                                                               ai::ModelStreamCompletion done) noexcept {
            if (!release_second.try_acquire_for(std::chrono::seconds{10})) {
                done(std::unexpected(support::make_error(support::ErrorCode::Cancelled, "second request timed out")));
                return;
            }
            auto answer = ai::assistant_text_message("second answer");
            auto delivered = sink(ai::AssistantDoneEvent{.reason = answer.stop_reason, .message = answer});
            if (!delivered)
                done(std::unexpected(delivered.error()));
            else
                done(std::move(answer));
        }}};
    };
    auto opened = consume(Harness::open(storage, std::move(options)));
    REQUIRE(opened.has_value());
    auto harness = std::move(opened.value());
    auto root = consume(harness->root(RootOptions{.model = ai::Model{.id = "model", .provider = "test"}}));
    REQUIRE(root.has_value());
    auto admitted = consume(harness->submit(root.value(), text_input("start")));
    REQUIRE(admitted.has_value());
    auto submission = std::move(admitted.value());
    const auto await_request = [&](std::binary_semaphore& requested, std::string_view label) {
        if (requested.try_acquire_for(std::chrono::seconds{5})) return;
        // Release any pending stream before failing, so Harness destruction can drain.
        if (label == "first request") release_first.release();
        release_second.release();
        auto inspection = consume(harness->inspect());
        std::string diagnostic = "Timed out waiting for " + std::string{label};
        if (inspection.has_value()) {
            for (const auto& task : inspection.value().tasks) {
                diagnostic += " task " + std::to_string(task.record.id.value) + " kind " + task.record.kind +
                              " record " + task.record.state.status + " scheduler " + task.state.kind;
            }
        }
        auto live = storage.find_document(
                "pi.live:v1", DocumentAddress{.conversation_id = root.value()}, DocumentPoint::current());
        if (live && live->has_value()) {
            auto value = storage.document((**live).id, DocumentPoint::current());
            if (value && value->has_value()) {
                const auto* fields = (**value).value.as_object();
                if (fields != nullptr && fields->contains("run")) {
                    const auto* run = fields->at("run").as_object();
                    if (run != nullptr && run->contains("task") && run->at("task").as_number()) {
                        diagnostic += " live.run.task " + std::to_string(*run->at("task").as_number());
                    }
                }
            }
        }
        FAIL(diagnostic << "; model request was not reached (check early-settlement mutation when applicable)");
    };
    await_request(first_requested, "first request");

    // The first request runs one live generation task.
    auto first_inspection = consume(harness->inspect());
    REQUIRE(first_inspection.has_value());
    CHECK(first_inspection.value().scheduling == "running");
    REQUIRE(first_inspection.value().tasks.size() == 1);
    const TaskInspection& first_task = first_inspection.value().tasks.front();
    CHECK(first_task.record.kind == "pi.generation");
    CHECK(first_task.state.kind == "running");
    const TaskId first_task_id = first_task.record.id;

    release_first.release();
    await_request(second_requested, "successor request");
    // The continuation keeps the run alive under a successor task: one live
    // generation task, and it is not the task that answered.
    auto second_inspection = consume(harness->inspect());
    REQUIRE(second_inspection.has_value());
    REQUIRE(second_inspection.value().tasks.size() == 1);
    const TaskInspection& second_task = second_inspection.value().tasks.front();
    CHECK(second_task.record.kind == "pi.generation");
    CHECK(second_task.state.kind == "running");
    const TaskId second_task_id = second_task.record.id;
    CHECK(second_task_id.value != first_task_id.value);

    const auto first_terminal = consume(harness->get_task(first_task_id));
    REQUIRE(first_terminal.has_value());
    REQUIRE(first_terminal.value().has_value());
    const TaskRecord& first_record = *first_terminal.value();
    CHECK(first_record.state.status == "terminal");
    REQUIRE(first_record.state.outcome.has_value());
    CHECK(first_record.state.outcome->status == "completed");
    REQUIRE(first_record.state.outcome->result.has_value());
    const auto* first_result = first_record.state.outcome->result->as_object();
    REQUIRE(first_result != nullptr);
    // The terminal outcome is the frozen `{ entryId }` shape, not an answer.
    REQUIRE(first_result->size() == 1);
    REQUIRE(first_result->contains("entryId"));
    CHECK_FALSE(first_result->contains("answer"));

    release_second.release();
    auto settled = consume(submission.wait());
    REQUIRE(settled.has_value());
    CHECK(settled.value().status == "done");
    REQUIRE(consume(harness->wait_for_idle()).has_value());
    auto idle_inspection = consume(harness->inspect());
    REQUIRE(idle_inspection.has_value());
    CHECK(idle_inspection.value().tasks.empty());

    const auto entries = entries_oldest_first(storage, root.value());
    const auto assistants = [&entries] {
        std::vector<const EntryRecord*> found;
        for (const auto& entry : entries) {
            if (entry.kind == "pi.assistant") found.push_back(&entry);
        }
        return found;
    }();
    REQUIRE(assistants.size() == 2);
    // Exactly one settlement over the whole continued run, pointing at the
    // first user entry and the SECOND assistant answer.
    REQUIRE(settled.value().entry.has_value());
    REQUIRE(settled.value().answer.has_value());
    CHECK(settled.value().entry->value == entries.front().id.value);
    CHECK(settled.value().answer->value == assistants.back()->id.value);
    CHECK(settled.value().answer->value != assistants.front()->id.value);
    CHECK(*first_result->at("entryId").as_number() == static_cast<double>(assistants.front()->id.value));

    const auto second_terminal = consume(harness->get_task(second_task_id));
    REQUIRE(second_terminal.has_value());
    REQUIRE(second_terminal.value().has_value());
    const TaskRecord& second_record = *second_terminal.value();
    REQUIRE(second_record.state.outcome.has_value());
    CHECK(second_record.state.outcome->status == "completed");
    REQUIRE(second_record.state.outcome->result.has_value());
    const auto* second_result = second_record.state.outcome->result->as_object();
    REQUIRE(second_result != nullptr);
    REQUIRE(second_result->size() == 1);
    REQUIRE(second_result->contains("entryId"));
    CHECK(*second_result->at("entryId").as_number() == static_cast<double>(assistants.back()->id.value));

    // The continuation appends a plain user entry and never admits a second
    // submission: one record, done, at the first user entry.
    auto submissions = storage.scan_submissions(100);
    REQUIRE(submissions.has_value());
    REQUIRE(submissions->size() == 1);
    CHECK(submissions->front().type == "input");
    CHECK(submissions->front().status == "done");
    REQUIRE(submissions->front().entry.has_value());
    CHECK(submissions->front().entry->value == entries.front().id.value);

    const auto* expected_entries = frozen->array_member("entries");
    REQUIRE(expected_entries != nullptr);
    CHECK(json_equals(transcript(entries), *expected_entries));
    const auto* expected_users = frozen->array_member("secondRequestUsers");
    REQUIRE(expected_users != nullptr);
    chord::Json::Array users;
    for (const auto& text : second_users)
        users.emplace_back(text);
    CHECK(json_equals(chord::Json{std::move(users)}, *expected_users));
    const auto expected_calls = frozen->number_member("callCount");
    REQUIRE(expected_calls.has_value());
    CHECK(calls.load() == static_cast<int>(*expected_calls));
    chord::Json::Array yields_json;
    for (const auto& text : yields)
        yields_json.emplace_back(text);
    const auto* expected_yields = frozen->array_member("yields");
    REQUIRE(expected_yields != nullptr);
    CHECK(json_equals(chord::Json{std::move(yields_json)}, *expected_yields));
    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("Workflow afterTool replaces the committed tool result", "[durable][workflow][issue1087]") {
    const auto frozen =
            FrozenScenario::load(kRegistryHooksArtifact, "an afterTool hook replaces the committed tool result");
    REQUIRE(frozen.has_value());
    Registry registry;
    std::vector<std::string> executed;
    std::vector<chord::Json> observed;
    Extension tools{.name = "audit-tools"};
    tools.tools.push_back(echo_tool(&executed));
    REQUIRE(registry.install(std::move(tools)).has_value());
    install_tool_hook(registry, "audit-tool", "afterTool", [&observed](const chord::Json& input, TaskRuntime&) {
        const auto* fields = input.as_object();
        REQUIRE(fields != nullptr);
        const auto call = fields->find("call");
        const auto result = fields->find("result");
        REQUIRE(call != fields->end());
        REQUIRE(result != fields->end());
        const auto* result_fields = result->second.as_object();
        REQUIRE(result_fields != nullptr);
        std::string text;
        for (const auto& block : *result_fields->at("output").as_array()) {
            if (const auto* value = block.as_object()->at("text").as_string()) text += *value;
        }
        observed.emplace_back(chord::Json{chord::Json::Object{{"phase", "afterTool"},
                {"id", call->second.as_object()->at("id")},
                {"arguments", call->second.as_object()->at("arguments")},
                {"text", text},
                {"isError", result_fields->at("isError")}}});
        return support::AsyncResult<chord::Json>{
                chord::Json{chord::Json::Object{{"output",
                                                        chord::Json{chord::Json::Array{chord::Json{chord::Json::Object{
                                                                {"type", "text"}, {"text", "replaced"}}}}}},
                        {"isError", false}}}};
    });
    MemoryStorage storage;
    HarnessOptions options{.registry = &registry};
    std::atomic_int calls{0};
    options.stream = [&calls](ai::Model, ai::AiContext, ai::SimpleStreamOptions) {
        if (calls.fetch_add(1) == 0)
            return scripted_reply(
                    call_message("echo", support::JsonValue{support::JsonValue::object_t{{"text", "original"}}}));
        return scripted_reply(ai::assistant_text_message("done"));
    };
    auto opened = consume(Harness::open(storage, std::move(options)));
    REQUIRE(opened.has_value());
    auto harness = std::move(opened.value());
    auto root = consume(harness->root(RootOptions{.model = ai::Model{.id = "model", .provider = "test"}}));
    REQUIRE(root.has_value());
    auto admitted = consume(harness->submit(root.value(), text_input("echo")));
    REQUIRE(admitted.has_value());
    auto settled = consume(admitted.value().wait());
    REQUIRE(settled.has_value());
    CHECK(settled.value().status == "done");
    const auto entries = entries_oldest_first(storage, root.value());
    const auto results = [&entries] {
        std::vector<const EntryRecord*> found;
        for (const auto& entry : entries) {
            if (entry.kind == "pi.tool-result") found.push_back(&entry);
        }
        return found;
    }();
    REQUIRE(results.size() == 1);
    const chord::Json& model = entry_model(*results.front());
    const auto* result_fields = model.as_object();
    REQUIRE(result_fields != nullptr);
    const auto result_text = [&result_fields] {
        std::string text;
        for (const auto& block : *result_fields->at("content").as_array()) {
            const auto* fields = block.as_object();
            REQUIRE(fields != nullptr);
            if (const auto* value = fields->at("text").as_string()) text += *value;
        }
        return text;
    }();
    // The committed result is the hook's replacement, not the tool's output.
    CHECK(result_text == *frozen->string_member("resultText"));
    CHECK(*result_fields->at("isError").as_boolean() == *frozen->member("isError")->get_if<bool>());
    const auto expected_duration = frozen->member("durationPresent");
    REQUIRE(expected_duration != nullptr);
    CHECK(result_fields->contains("durationMs") == *expected_duration->get_if<bool>());
    CHECK(executed.size() == 1);
    CHECK(executed.front() == "original");
    const auto* expected_hooks = frozen->array_member("hooks");
    REQUIRE(expected_hooks != nullptr);
    CHECK(json_equals(chord::Json{chord::Json::Array{observed.begin(), observed.end()}}, *expected_hooks));
    const auto* expected_entries = frozen->array_member("entries");
    REQUIRE(expected_entries != nullptr);
    CHECK(json_equals(transcript(entries), *expected_entries));
    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("Workflow beforeTool hooks run in extension order and the last rewrite executes",
        "[durable][workflow][issue1087]") {
    const auto frozen = FrozenScenario::load(
            kRegistryHooksArtifact, "beforeTool hooks run in extension order and the last rewrite executes");
    REQUIRE(frozen.has_value());
    Registry registry;
    std::vector<std::string> executed;
    std::vector<chord::Json> observed;
    Extension tools{.name = "audit-tools"};
    tools.tools.push_back(echo_tool(&executed));
    REQUIRE(registry.install(std::move(tools)).has_value());
    const auto rewrite = [&registry, &observed](std::string name, std::string text) {
        install_tool_hook(registry,
                name,
                "beforeTool",
                [&observed, extension = name, text](const chord::Json& call, TaskRuntime&) {
                    const auto* fields = call.as_object();
                    REQUIRE(fields != nullptr);
                    observed.emplace_back(chord::Json{chord::Json::Object{{"extension", extension},
                            {"id", fields->at("id")},
                            {"arguments", fields->at("arguments")}}});
                    return support::AsyncResult<chord::Json>{chord::Json{
                            chord::Json::Object{{"arguments", chord::Json{chord::Json::Object{{"text", text}}}}}}};
                });
    };
    rewrite("first-hook", "first");
    rewrite("second-hook", "second");
    MemoryStorage storage;
    HarnessOptions options{.registry = &registry};
    std::atomic_int calls{0};
    options.stream = [&calls](ai::Model, ai::AiContext, ai::SimpleStreamOptions) {
        if (calls.fetch_add(1) == 0)
            return scripted_reply(
                    call_message("echo", support::JsonValue{support::JsonValue::object_t{{"text", "original"}}}));
        return scripted_reply(ai::assistant_text_message("done"));
    };
    auto opened = consume(Harness::open(storage, std::move(options)));
    REQUIRE(opened.has_value());
    auto harness = std::move(opened.value());
    auto root = consume(harness->root(RootOptions{.model = ai::Model{.id = "model", .provider = "test"}}));
    REQUIRE(root.has_value());
    auto admitted = consume(harness->submit(root.value(), text_input("echo")));
    REQUIRE(admitted.has_value());
    REQUIRE(consume(admitted.value().wait()).has_value());
    const auto entries = entries_oldest_first(storage, root.value());
    chord::Json::Array executed_json;
    for (const auto& text : executed)
        executed_json.emplace_back(text);
    const auto* expected_executions = frozen->array_member("executions");
    REQUIRE(expected_executions != nullptr);
    CHECK(json_equals(chord::Json{std::move(executed_json)}, *expected_executions));
    const auto* expected_hooks = frozen->array_member("hooks");
    REQUIRE(expected_hooks != nullptr);
    CHECK(json_equals(chord::Json{chord::Json::Array{observed.begin(), observed.end()}}, *expected_hooks));
    // The second hook sees the first hook's rewrite, and its own rewrite is
    // the one that executes.
    REQUIRE(observed.size() == 2);
    CHECK(argument_text(observed.front()) == "original");
    CHECK(argument_text(observed.back()) == "first");
    const auto* expected_entries = frozen->array_member("entries");
    REQUIRE(expected_entries != nullptr);
    CHECK(json_equals(transcript(entries), *expected_entries));
    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("Workflow throwing beforeTool blocks the call with the frozen harness error",
        "[durable][workflow][issue1087]") {
    const auto frozen = FrozenScenario::load(
            kRegistryHooksArtifact, "a throwing beforeTool blocks the call and the run still settles");
    REQUIRE(frozen.has_value());
    Registry registry;
    std::vector<std::string> executed;
    std::vector<chord::Json> observed;
    Extension tools{.name = "audit-tools"};
    tools.tools.push_back(echo_tool(&executed));
    REQUIRE(registry.install(std::move(tools)).has_value());
    install_tool_hook(registry,
            "blocking-hook",
            "beforeTool",
            [&observed](const chord::Json& call, TaskRuntime&) -> support::AsyncResult<chord::Json> {
                observed.emplace_back(chord::Json{chord::Json::Object{{"extension", "blocking-hook"},
                        {"id", call.as_object()->at("id")},
                        {"arguments", call.as_object()->at("arguments")}}});
                return support::AsyncResult<chord::Json>{
                        std::unexpected(support::make_error(support::ErrorCode::Validation, "denied by policy"))};
            });
    install_tool_hook(registry, "later-hook", "beforeTool", [&observed](const chord::Json& call, TaskRuntime&) {
        observed.emplace_back(chord::Json{chord::Json::Object{{"extension", "later-hook"},
                {"id", call.as_object()->at("id")},
                {"arguments", call.as_object()->at("arguments")}}});
        return support::AsyncResult<chord::Json>{
                chord::Json{chord::Json::Object{{"arguments", chord::Json{chord::Json::Object{{"text", "later"}}}}}}};
    });
    MemoryStorage storage;
    HarnessOptions options{.registry = &registry};
    std::atomic_int calls{0};
    options.stream = [&calls](ai::Model, ai::AiContext, ai::SimpleStreamOptions) {
        if (calls.fetch_add(1) == 0)
            return scripted_reply(
                    call_message("echo", support::JsonValue{support::JsonValue::object_t{{"text", "original"}}}));
        return scripted_reply(ai::assistant_text_message("done"));
    };
    auto opened = consume(Harness::open(storage, std::move(options)));
    REQUIRE(opened.has_value());
    auto harness = std::move(opened.value());
    auto root = consume(harness->root(RootOptions{.model = ai::Model{.id = "model", .provider = "test"}}));
    REQUIRE(root.has_value());
    auto admitted = consume(harness->submit(root.value(), text_input("echo")));
    REQUIRE(admitted.has_value());
    auto settled = consume(admitted.value().wait());
    REQUIRE(settled.has_value());
    // A blocked call feeds an error result back to the model, so the run
    // still settles.
    CHECK(settled.value().status == *frozen->string_member("status"));
    CHECK(executed.empty());
    const auto entries = entries_oldest_first(storage, root.value());
    const auto* result = [&entries]() -> const chord::Json* {
        for (const auto& entry : entries) {
            if (entry.kind == "pi.tool-result") return &entry_model(entry);
        }
        return nullptr;
    }();
    REQUIRE(result != nullptr);
    const auto* fields = result->as_object();
    REQUIRE(fields != nullptr);
    // The committed text is byte-for-byte the frozen artifact's own string.
    const std::string expected_text = *frozen->string_member("resultText");
    REQUIRE(expected_text.find("Tool call blocked: denied by policy") != std::string::npos);
    std::string text;
    for (const auto& block : *fields->at("content").as_array()) {
        if (const auto* value = block.as_object()->at("text").as_string()) text += *value;
    }
    CHECK(text == expected_text);
    CHECK(*fields->at("isError").as_boolean() == *frozen->member("isError")->get_if<bool>());
    const auto expected_duration = frozen->member("durationPresent");
    REQUIRE(expected_duration != nullptr);
    // A call that never executed carries no duration.
    CHECK(fields->contains("durationMs") == *expected_duration->get_if<bool>());
    CHECK_FALSE(fields->contains("durationMs"));
    // The later hook never ran.
    const auto* expected_hooks = frozen->array_member("hooks");
    REQUIRE(expected_hooks != nullptr);
    CHECK(json_equals(chord::Json{chord::Json::Array{observed.begin(), observed.end()}}, *expected_hooks));
    REQUIRE(observed.size() == 1);
    CHECK(argument_text(observed.front()) == "original");
    const auto* expected_entries = frozen->array_member("entries");
    REQUIRE(expected_entries != nullptr);
    CHECK(json_equals(transcript(entries), *expected_entries));
    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("Workflow afterTool observes the model call while the rewritten arguments execute",
        "[durable][workflow][issue1087]") {
    const auto frozen = FrozenScenario::load(
            kRegistryHooksArtifact, "afterTool observes the model's call while the rewritten arguments execute");
    REQUIRE(frozen.has_value());
    Registry registry;
    std::vector<std::string> executed;
    std::vector<chord::Json> observed;
    Extension tools{.name = "audit-tools"};
    tools.tools.push_back(echo_tool(&executed));
    REQUIRE(registry.install(std::move(tools)).has_value());
    install_tool_hook(registry, "rewrite-hook", "beforeTool", [](const chord::Json&, TaskRuntime&) {
        return support::AsyncResult<chord::Json>{chord::Json{
                chord::Json::Object{{"arguments", chord::Json{chord::Json::Object{{"text", "rewritten"}}}}}}};
    });
    install_tool_hook(registry, "observe-hook", "afterTool", [&observed](const chord::Json& input, TaskRuntime&) {
        const auto* fields = input.as_object();
        REQUIRE(fields != nullptr);
        const auto* result = fields->at("result").as_object();
        REQUIRE(result != nullptr);
        std::string text;
        for (const auto& block : *result->at("output").as_array()) {
            if (const auto* value = block.as_object()->at("text").as_string()) text += *value;
        }
        observed.emplace_back(chord::Json{chord::Json::Object{{"phase", "afterTool"},
                {"id", fields->at("call").as_object()->at("id")},
                {"arguments", fields->at("call").as_object()->at("arguments")},
                {"text", text}}});
        // Observing only: an absent decision keeps the result.
        return support::AsyncResult<chord::Json>{chord::Json{}};
    });
    MemoryStorage storage;
    HarnessOptions options{.registry = &registry};
    std::atomic_int calls{0};
    options.stream = [&calls](ai::Model, ai::AiContext, ai::SimpleStreamOptions) {
        if (calls.fetch_add(1) == 0)
            return scripted_reply(
                    call_message("echo", support::JsonValue{support::JsonValue::object_t{{"text", "original"}}}));
        return scripted_reply(ai::assistant_text_message("done"));
    };
    auto opened = consume(Harness::open(storage, std::move(options)));
    REQUIRE(opened.has_value());
    auto harness = std::move(opened.value());
    auto root = consume(harness->root(RootOptions{.model = ai::Model{.id = "model", .provider = "test"}}));
    REQUIRE(root.has_value());
    auto admitted = consume(harness->submit(root.value(), text_input("echo")));
    REQUIRE(admitted.has_value());
    auto settled = consume(admitted.value().wait());
    REQUIRE(settled.has_value());
    CHECK(settled.value().status == *frozen->string_member("status"));
    // The rewrite executed, and the hook still saw the model's call.
    const auto* expected_executed = frozen->array_member("executed");
    REQUIRE(expected_executed != nullptr);
    chord::Json::Array executed_json;
    for (const auto& text : executed)
        executed_json.emplace_back(text);
    CHECK(json_equals(chord::Json{std::move(executed_json)}, *expected_executed));
    const auto* expected_observed = frozen->array_member("observed");
    REQUIRE(expected_observed != nullptr);
    CHECK(json_equals(chord::Json{chord::Json::Array{observed.begin(), observed.end()}}, *expected_observed));
    REQUIRE(observed.size() == 1);
    CHECK(argument_text(observed.front()) == "original");
    CHECK(executed.size() == 1);
    CHECK(executed.front() == "rewritten");
    const auto entries = entries_oldest_first(storage, root.value());
    const auto* result = [&entries]() -> const chord::Json* {
        for (const auto& entry : entries) {
            if (entry.kind == "pi.tool-result") return &entry_model(entry);
        }
        return nullptr;
    }();
    REQUIRE(result != nullptr);
    std::string text;
    for (const auto& block : *result->as_object()->at("content").as_array()) {
        if (const auto* value = block.as_object()->at("text").as_string()) text += *value;
    }
    CHECK(text == *frozen->string_member("resultText"));
    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("Workflow extension replacement and uninstall change the next request's tool hooks",
        "[durable][workflow][issue1087]") {
    const auto frozen = FrozenScenario::load(
            kRegistryHooksArtifact, "replacing and uninstalling an extension changes the next request's tool hooks");
    REQUIRE(frozen.has_value());
    Registry registry;
    std::vector<chord::Json> observed;
    std::atomic_int calls{0};
    std::vector<chord::Json> results;
    MemoryStorage storage;
    Extension tools{.name = "audit-tools"};
    tools.tools.push_back(echo_tool(nullptr));
    REQUIRE(registry.install(std::move(tools)).has_value());
    const auto hooked = [&observed](std::string name, std::string text) {
        Extension extension{.name = name};
        extension.hooks.push_back(
                tool_hook("beforeTool", [&observed, name, text](const chord::Json& call, TaskRuntime&) {
                    const auto* fields = call.as_object();
                    REQUIRE(fields != nullptr);
                    observed.emplace_back(chord::Json{
                            chord::Json::Object{{"request", chord::Json{static_cast<double>(observed.size() + 1)}},
                                    {"extension", name},
                                    {"arguments", fields->at("arguments")}}});
                    return support::AsyncResult<chord::Json>{chord::Json{
                            chord::Json::Object{{"arguments", chord::Json{chord::Json::Object{{"text", text}}}}}}};
                }));
        return extension;
    };
    REQUIRE(registry.install(hooked("hook-set", "one")).has_value());
    HarnessOptions options{.registry = &registry};
    options.stream = [&calls](ai::Model, ai::AiContext, ai::SimpleStreamOptions) {
        if (calls.fetch_add(1) % 2 == 0)
            return scripted_reply(
                    call_message("echo", support::JsonValue{support::JsonValue::object_t{{"text", "original"}}}));
        return scripted_reply(ai::assistant_text_message("done"));
    };
    auto opened = consume(Harness::open(storage, std::move(options)));
    REQUIRE(opened.has_value());
    auto harness = std::move(opened.value());
    auto root = consume(harness->root(RootOptions{.model = ai::Model{.id = "model", .provider = "test"}}));
    REQUIRE(root.has_value());
    const auto run = [&](std::string label) {
        auto admitted = consume(harness->submit(root.value(), text_input(std::move(label))));
        REQUIRE(admitted.has_value());
        auto settled = consume(admitted.value().wait());
        REQUIRE(settled.has_value());
        const auto entries = entries_oldest_first(storage, root.value());
        const auto* result = [&entries]() -> const chord::Json* {
            for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
                if (it->kind == "pi.tool-result") return &entry_model(*it);
            }
            return nullptr;
        }();
        REQUIRE(result != nullptr);
        std::string text;
        for (const auto& block : *result->as_object()->at("content").as_array()) {
            if (const auto* value = block.as_object()->at("text").as_string()) text += *value;
        }
        return std::pair{settled.value().status, text};
    };
    const auto before_replace = run("first");
    REQUIRE(registry.install(hooked("hook-set", "two")).has_value());
    const auto after_replace = run("second");
    registry.uninstall("hook-set");
    const auto after_uninstall = run("third");
    const auto* expected_before = frozen->object_member("beforeReplace");
    const auto* expected_replace = frozen->object_member("afterReplace");
    const auto* expected_uninstall = frozen->object_member("afterUninstall");
    REQUIRE(expected_before != nullptr);
    REQUIRE(expected_replace != nullptr);
    REQUIRE(expected_uninstall != nullptr);
    const auto expected_text = [](const support::JsonValue::object_t& object) {
        const auto found = object.find("resultText");
        REQUIRE(found != object.end());
        const auto* text = found->second.get_if<std::string>();
        REQUIRE(text != nullptr);
        return *text;
    };
    // The next request after a replace sees the new rewrite; after an
    // uninstall it sees none.
    CHECK(before_replace.first == "done");
    CHECK(before_replace.second == expected_text(*expected_before));
    CHECK(after_replace.first == "done");
    CHECK(after_replace.second == expected_text(*expected_replace));
    CHECK(after_uninstall.first == "done");
    CHECK(after_uninstall.second == expected_text(*expected_uninstall));
    const auto* expected_observations = frozen->array_member("observations");
    REQUIRE(expected_observations != nullptr);
    // Hooks observed only the two requests that had a hook installed.
    CHECK(json_equals(chord::Json{chord::Json::Array{observed.begin(), observed.end()}}, *expected_observations));
    REQUIRE(observed.size() == 2);
    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE(
        "Workflow generation outcome keeps entryId through JSONL and SQLite reopen", "[durable][workflow][issue1087]") {
    TempWorkspace workspace;
    const std::filesystem::path base = workspace.path() / "generation-outcome";
    for (const std::string backend : {"jsonl", "sqlite"}) {
        const std::filesystem::path file = base / backend;
        std::filesystem::create_directories(file);
        const std::filesystem::path path = file / (backend == "jsonl" ? "state.jsonl" : "state.sqlite");
        auto storage_result = [&]() -> std::expected<std::unique_ptr<Storage>, support::Error> {
            if (backend == "jsonl") {
                auto opened_storage = JsonlStorage::open(path);
                if (!opened_storage) return std::unexpected(opened_storage.error());
                return std::unique_ptr<Storage>{std::move(*opened_storage)};
            }
            auto opened_storage = SqliteStorage::open(path);
            if (!opened_storage) return std::unexpected(opened_storage.error());
            return std::unique_ptr<Storage>{std::move(*opened_storage)};
        }();
        REQUIRE(storage_result.has_value());
        auto storage = std::move(*storage_result);
        HarnessOptions options;
        options.stream = [](ai::Model, ai::AiContext, ai::SimpleStreamOptions) {
            return scripted_reply(ai::assistant_text_message("answer"));
        };
        auto opened = consume(Harness::open(*storage, std::move(options)));
        REQUIRE(opened.has_value());
        auto harness = std::move(opened.value());
        auto root = consume(harness->root(RootOptions{.model = ai::Model{.id = "model", .provider = "test"}}));
        REQUIRE(root.has_value());
        auto admitted = consume(harness->submit(root.value(), text_input("ask")));
        REQUIRE(admitted.has_value());
        REQUIRE(consume(admitted.value().wait()).has_value());
        auto tasks = storage->scan_tasks(100);
        REQUIRE(tasks.has_value());
        const auto generation = std::ranges::find_if(*tasks,
                [](const TaskRecord& task) { return task.kind == "pi.generation" && task.state.status == "terminal"; });
        REQUIRE(generation != tasks->end());
        const TaskId generation_id = generation->id;
        REQUIRE(generation->state.outcome.has_value());
        REQUIRE(generation->state.outcome->result.has_value());
        REQUIRE(consume(harness->close()).has_value());
        REQUIRE(storage->close().has_value());

        // Reopen the same bytes: the terminal outcome still names the answer
        // entry under `entryId`, with no `answer` key.
        auto reopened_storage = [&]() -> std::expected<std::unique_ptr<Storage>, support::Error> {
            if (backend == "jsonl") {
                auto opened_storage = JsonlStorage::open(path);
                if (!opened_storage) return std::unexpected(opened_storage.error());
                return std::unique_ptr<Storage>{std::move(*opened_storage)};
            }
            auto opened_storage = SqliteStorage::open(path);
            if (!opened_storage) return std::unexpected(opened_storage.error());
            return std::unique_ptr<Storage>{std::move(*opened_storage)};
        }();
        REQUIRE(reopened_storage.has_value());
        auto reopened = std::move(*reopened_storage);
        auto restored = reopened->task(generation_id);
        REQUIRE(restored.has_value());
        REQUIRE(restored->has_value());
        REQUIRE((*restored)->state.outcome.has_value());
        REQUIRE((*restored)->state.outcome->result.has_value());
        const auto* result = (*restored)->state.outcome->result->as_object();
        REQUIRE(result != nullptr);
        REQUIRE(result->contains("entryId"));
        CHECK_FALSE(result->contains("answer"));
        const auto entries = entries_oldest_first(*reopened, root.value());
        const auto assistant =
                std::ranges::find_if(entries, [](const EntryRecord& entry) { return entry.kind == "pi.assistant"; });
        REQUIRE(assistant != entries.end());
        CHECK(*result->at("entryId").as_number() == static_cast<double>(assistant->id.value));
        HarnessOptions reopen_options;
        reopen_options.stream = [](ai::Model, ai::AiContext, ai::SimpleStreamOptions) {
            return scripted_reply(ai::assistant_text_message("answer"));
        };
        auto reopened_harness = consume(Harness::open(*reopened, std::move(reopen_options)));
        REQUIRE(reopened_harness.has_value());
        auto harness_again = std::move(reopened_harness.value());
        // Both public projections agree after the reopen.
        auto projection = consume(harness_again->get_task(generation_id));
        REQUIRE(projection.has_value());
        REQUIRE(projection.value().has_value());
        const TaskRecord& projected_record = *projection.value();
        REQUIRE(projected_record.state.outcome.has_value());
        REQUIRE(projected_record.state.outcome->result.has_value());
        CHECK(*projected_record.state.outcome->result == *(*restored)->state.outcome->result);
        const auto* projected = projected_record.state.outcome->result->as_object();
        REQUIRE(projected != nullptr);
        CHECK(projected->contains("entryId"));
        auto inspection = consume(harness_again->inspect());
        REQUIRE(inspection.has_value());
        CHECK(inspection.value().tasks.empty());
        REQUIRE(consume(harness_again->close()).has_value());
        REQUIRE(reopened->close().has_value());
        if (backend == "jsonl") {
            // The persisted line carries the frozen key, not the old one.
            std::ifstream stream(path / "main.jsonl");
            std::stringstream buffer;
            buffer << stream.rdbuf();
            const std::string text = buffer.str();
            CHECK(text.find("\"entryId\"") != std::string::npos);
        }
    }
}

namespace {

// ── issue1163 helpers: public-seam observations of the live generation ─────
// ── lifecycle and the usage ledger ─────────────────────────────────────────

/// The committed pi.live value of one conversation, when the record exists.
std::optional<chord::Json> live_document_value(Storage& storage, ConversationId root) {
    auto doc = storage.find_document("pi.live:v1", DocumentAddress{.conversation_id = root}, DocumentPoint::current());
    if (!doc || !doc->has_value()) return std::nullopt;
    auto value = storage.document((**doc).id, DocumentPoint::current());
    if (!value || !value->has_value()) return std::nullopt;
    return (**value).value;
}

/// The committed generation attempt of one conversation, when published.
std::optional<std::int64_t> live_generation_attempt(Storage& storage, ConversationId root) {
    auto live = live_document_value(storage, root);
    if (!live) return std::nullopt;
    const auto* object = live->as_object();
    if (object == nullptr) return std::nullopt;
    const auto generation = object->find("generation");
    if (generation == object->end()) return std::nullopt;
    const auto* attempt = generation->second.as_object();
    if (attempt == nullptr) return std::nullopt;
    const auto* number = attempt->at("attempt").as_number();
    if (number == nullptr) return std::nullopt;
    return static_cast<std::int64_t>(*number);
}

/// One committed `pi.usage` models bucket counter for `test/model`.
std::optional<double> usage_counter(Storage& storage, ConversationId root, std::string_view counter) {
    auto doc = storage.find_document("pi.usage:v1", DocumentAddress{.conversation_id = root}, DocumentPoint::current());
    if (!doc || !doc->has_value()) return std::nullopt;
    auto value = storage.document((**doc).id, DocumentPoint::current());
    if (!value || !value->has_value()) return std::nullopt;
    const auto* object = (**value).value.as_object();
    if (object == nullptr) return std::nullopt;
    const auto models = object->find("models");
    if (models == object->end()) return std::nullopt;
    const auto* bucket = models->second.as_object();
    if (bucket == nullptr) return std::nullopt;
    const auto model = bucket->find("test/model");
    if (model == bucket->end()) return std::nullopt;
    const auto* usage = model->second.as_object();
    if (usage == nullptr) return std::nullopt;
    const auto found = usage->find(std::string{counter});
    if (found == usage->end()) return std::nullopt;
    const auto* number = found->second.as_number();
    if (number == nullptr) return std::nullopt;
    return *number;
}

} // namespace

TEST_CASE("generation publishes the attempt before the model stream and usage with the final answer",
        "[durable][workflow][issue1163]") {
    MemoryStorage storage;
    std::optional<ConversationId> root_id{};
    std::binary_semaphore entered{0};
    std::binary_semaphore release{0};
    std::mutex observed_mutex;
    std::vector<std::optional<std::int64_t>> observed_attempts;
    HarnessOptions options;
    options.stream = [&](ai::Model, ai::AiContext, ai::SimpleStreamOptions) {
        return ai::ModelStream{ai::ModelStreamProducer{
                [&](ai::AssistantEventSink sink, ai::ModelStreamCompletion done) mutable noexcept {
                    {
                        std::lock_guard lock(observed_mutex);
                        // The publication commit has settled before the stream
                        // factory runs; this observation cannot race it.
                        observed_attempts.push_back(
                                root_id ? live_generation_attempt(storage, *root_id) : std::optional<std::int64_t>{});
                    }
                    entered.release();
                    if (!release.try_acquire_for(std::chrono::seconds{10})) {
                        done(std::unexpected(support::make_error(support::ErrorCode::Cancelled, "held too long")));
                        return;
                    }
                    auto answer = ai::assistant_text_message("finished");
                    answer.provider = "test";
                    answer.model = "model";
                    answer.usage.input = 7;
                    answer.usage.output = 2;
                    answer.usage.cache_read = 1;
                    answer.usage.total_tokens = 10;
                    auto delivered = sink(ai::AssistantDoneEvent{.reason = answer.stop_reason, .message = answer});
                    if (!delivered)
                        done(std::unexpected(delivered.error()));
                    else
                        done(std::move(answer));
                }}};
    };
    auto opened = consume(Harness::open(storage, std::move(options)));
    REQUIRE(opened.has_value());
    auto harness = std::move(opened.value());
    auto root = consume(harness->root(RootOptions{.model = ai::Model{.id = "model", .provider = "test"}}));
    REQUIRE(root.has_value());
    root_id = root.value();
    auto definition = detail::builtin_document("pi.live:v1", DocumentHistory::Latest, DocumentFork::Initial, [] {
        return chord::Json{chord::Json::Object{}};
    });
    auto acquired = consume(harness->session().watch_doc(definition, DocumentAddress{.conversation_id = root.value()}));
    REQUIRE(acquired.has_value());
    REQUIRE(acquired.value().has_value());
    auto& watch = *acquired.value();
    const chord::Json initial = watch.value();
    std::mutex frames_mutex;
    std::vector<DocumentFrame> frames;
    std::atomic_int atomic_answer_commits{0};
    REQUIRE(watch.start([&](DocumentFrame frame) {
        std::lock_guard lock(frames_mutex);
        frames.push_back(std::move(frame));
    }));
    REQUIRE(harness->session().subscribe_commits([&](const CommitPublication& publication) {
        bool answer = false;
        bool usage = false;
        for (const auto& change : publication.changes) {
            if (const auto* table = std::get_if<TableCommitChange>(&change)) {
                if (table->type == "entry" && table->conversation_id == root.value()) answer = true;
            }
            if (const auto* doc = std::get_if<DocumentCommitChange>(&change)) {
                if (doc->record.kind == "pi.usage:v1" && doc->conversation_id == root.value()) usage = true;
            }
        }
        if (answer && usage) ++atomic_answer_commits;
    }));
    auto admitted = consume(harness->submit(root.value(), text_input("hi")));
    REQUIRE(admitted.has_value());
    REQUIRE(entered.try_acquire_for(std::chrono::seconds{5}));
    {
        std::lock_guard lock(observed_mutex);
        REQUIRE(observed_attempts.size() == 1);
        REQUIRE(observed_attempts.front().has_value());
        CHECK(*observed_attempts.front() == 1);
    }
    // The run is still live mid-stream: the attempt stays published.
    auto mid_attempt = live_generation_attempt(storage, root.value());
    REQUIRE(mid_attempt.has_value());
    CHECK(*mid_attempt == 1);
    release.release();
    auto settled = consume(admitted.value().wait());
    REQUIRE(settled.has_value());
    CHECK(settled.value().status == "done");
    // The final answer cleared the live document entirely.
    auto live = live_document_value(storage, root.value());
    REQUIRE(live.has_value());
    CHECK(live->as_object()->find("generation") == live->as_object()->end());
    CHECK(live->as_object()->find("run") == live->as_object()->end());
    CHECK(usage_counter(storage, root.value(), "input") == 7);
    CHECK(usage_counter(storage, root.value(), "output") == 2);
    CHECK(usage_counter(storage, root.value(), "cacheRead") == 1);
    CHECK(usage_counter(storage, root.value(), "totalTokens") == 10);
    CHECK(atomic_answer_commits == 1);
    {
        std::lock_guard lock(frames_mutex);
        REQUIRE(frames.size() == 3);
        chord::Json replay = initial;
        for (const auto& frame : frames) {
            CHECK_FALSE(frame.ops.empty());
            auto applied = chord::apply_immutable(replay, frame.ops);
            REQUIRE(applied.has_value());
            replay = std::move(*applied);
            CHECK(replay == frame.value);
        }
        CHECK(replay == *live);
        const auto* generation = frames[1].value.as_object()->at("generation").as_object();
        REQUIRE(generation != nullptr);
        CHECK(*generation->at("attempt").as_number() == 1);
    }
    CHECK(watch.stop() == WatchEndReason::Stopped);
    auto entries = storage.scan_entries(root.value(), 100);
    REQUIRE(entries.has_value());
    REQUIRE(entries->size() == 2);
    const auto* answer = entry_model(entries->front()).as_object();
    REQUIRE(answer != nullptr);
    REQUIRE(answer->contains("usage"));
    CHECK(*answer->at("usage").as_object()->at("input").as_number() == 7);
    CHECK(*answer->at("stopReason").as_string() == "stop");
    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("generation accumulates usage across a committed retry", "[durable][workflow][issue1163]") {
    MemoryStorage storage;
    std::optional<ConversationId> root_id{};
    std::mutex observed_mutex;
    std::vector<std::optional<std::int64_t>> observed_attempts;
    std::atomic_int requests{0};
    HarnessOptions options;
    options.settings.compaction.enabled = false;
    options.settings.retry.max_retries = 1;
    options.settings.retry.base_delay_ms = 1;
    options.stream = [&](ai::Model, ai::AiContext, ai::SimpleStreamOptions) {
        return ai::ModelStream{ai::ModelStreamProducer{[&, turn = ++requests](ai::AssistantEventSink sink,
                                                               ai::ModelStreamCompletion done) mutable noexcept {
            {
                std::lock_guard lock(observed_mutex);
                observed_attempts.push_back(
                        root_id ? live_generation_attempt(storage, *root_id) : std::optional<std::int64_t>{});
            }
            auto answer = ai::assistant_text_message(turn == 1 ? "" : "finished");
            answer.provider = "test";
            answer.model = "model";
            if (turn == 1) {
                answer.stop_reason = ai::AssistantStopReason::Error;
                answer.error_message = "overloaded";
                answer.usage.input = 3;
                answer.usage.output = 1;
                answer.usage.total_tokens = 4;
                auto result = sink(ai::AssistantErrorEvent{.reason = answer.stop_reason,
                        .error = answer,
                        .inference_failure = ai::InferenceFailure{.kind = ai::InferenceFailureKind::RateLimited}});
                if (!result) {
                    done(std::unexpected(result.error()));
                    return;
                }
            } else {
                answer.usage.input = 7;
                answer.usage.output = 2;
                answer.usage.cache_read = 1;
                answer.usage.total_tokens = 10;
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
    root_id = root.value();
    auto admitted = consume(harness->submit(root.value(), text_input("hi")));
    REQUIRE(admitted.has_value());
    auto settled = consume(admitted.value().wait());
    REQUIRE(settled.has_value());
    CHECK(settled.value().status == "done");
    CHECK(requests == 2);
    // The retry publishes its own attempt before the second stream.
    {
        std::lock_guard lock(observed_mutex);
        REQUIRE(observed_attempts.size() == 2);
        REQUIRE(observed_attempts[0].has_value());
        REQUIRE(observed_attempts[1].has_value());
        CHECK(*observed_attempts[0] == 1);
        CHECK(*observed_attempts[1] == 2);
    }
    // One ledger holds both attempts' usage.
    CHECK(usage_counter(storage, root.value(), "input") == 10);
    CHECK(usage_counter(storage, root.value(), "output") == 3);
    CHECK(usage_counter(storage, root.value(), "cacheRead") == 1);
    CHECK(usage_counter(storage, root.value(), "totalTokens") == 14);
    auto live = live_document_value(storage, root.value());
    REQUIRE(live.has_value());
    CHECK(live->as_object()->empty());
    auto entries = storage.scan_entries(root.value(), 100);
    REQUIRE(entries.has_value());
    REQUIRE(std::ranges::count_if(*entries, [](const auto& entry) { return entry.kind == "pi.assistant"; }) == 2);
    for (const auto& entry : *entries) {
        if (entry.kind != "pi.assistant") continue;
        CHECK(entry_model(entry).as_object()->contains("usage"));
    }
    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE(
        "generation clears the attempt for a tool round and accumulates its usage", "[durable][workflow][issue1163]") {
    MemoryStorage storage;
    std::optional<ConversationId> root_id{};
    Registry registry;
    std::binary_semaphore tool_entered{0};
    std::binary_semaphore tool_release{0};
    Extension extension{.name = "audit-tools"};
    extension.tools.push_back(ToolRegistration{
            .declaration = ai::Tool{.name = "echo", .description = "Echo the text argument back"},
            .execute = [&](support::JsonValue arguments, std::stop_token) {
                tool_entered.release();
                if (!tool_release.try_acquire_for(std::chrono::seconds{10}))
                    return support::AsyncResult<std::vector<ai::Content>>{
                            std::unexpected(support::make_error(support::ErrorCode::Cancelled, "held too long"))};
                return support::AsyncResult<std::vector<ai::Content>>{
                        std::vector<ai::Content>{ai::text_content("echo:" + arguments.at("text").get_string())}};
            }});
    REQUIRE(registry.install(std::move(extension)).has_value());
    std::mutex observed_mutex;
    std::vector<std::optional<std::int64_t>> observed_attempts;
    std::atomic_int requests{0};
    HarnessOptions options{.registry = &registry};
    options.stream = [&](ai::Model, ai::AiContext, ai::SimpleStreamOptions) {
        return ai::ModelStream{ai::ModelStreamProducer{
                [&, turn = ++requests](ai::AssistantEventSink sink, ai::ModelStreamCompletion done) mutable noexcept {
                    {
                        std::lock_guard lock(observed_mutex);
                        observed_attempts.push_back(
                                root_id ? live_generation_attempt(storage, *root_id) : std::optional<std::int64_t>{});
                    }
                    auto answer = ai::assistant_text_message("finished");
                    answer.provider = "test";
                    answer.model = "model";
                    if (turn == 1) {
                        answer.content = {ai::ToolCallContent{.id = "call-1",
                                .name = "echo",
                                .arguments = support::JsonValue{support::JsonValue::object_t{{"text", "hello"}}}}};
                        answer.stop_reason = ai::AssistantStopReason::ToolUse;
                        answer.usage.input = 4;
                        answer.usage.output = 1;
                        answer.usage.total_tokens = 5;
                    } else {
                        answer.usage.input = 6;
                        answer.usage.output = 3;
                        answer.usage.cache_write = 2;
                        answer.usage.total_tokens = 11;
                    }
                    auto delivered = sink(ai::AssistantDoneEvent{.reason = answer.stop_reason, .message = answer});
                    if (!delivered)
                        done(std::unexpected(delivered.error()));
                    else
                        done(std::move(answer));
                }}};
    };
    auto opened = consume(Harness::open(storage, std::move(options)));
    REQUIRE(opened.has_value());
    auto harness = std::move(opened.value());
    auto root = consume(harness->root(RootOptions{.model = ai::Model{.id = "model", .provider = "test"}}));
    REQUIRE(root.has_value());
    root_id = root.value();
    auto admitted = consume(harness->submit(root.value(), text_input("run the echo tool")));
    REQUIRE(admitted.has_value());
    // The tool answer commit clears the published attempt while the run
    // continues: observed while the tool itself is blocked.
    REQUIRE(tool_entered.try_acquire_for(std::chrono::seconds{5}));
    {
        auto live = live_document_value(storage, root.value());
        REQUIRE(live.has_value());
        const auto* object = live->as_object();
        CHECK(object->contains("run"));
        CHECK(object->find("generation") == object->end());
    }
    tool_release.release();
    auto settled = consume(admitted.value().wait());
    REQUIRE(settled.has_value());
    CHECK(settled.value().status == "done");
    CHECK(requests == 2);
    // The successor request republished the attempt before its stream.
    {
        std::lock_guard lock(observed_mutex);
        REQUIRE(observed_attempts.size() == 2);
        REQUIRE(observed_attempts[0].has_value());
        REQUIRE(observed_attempts[1].has_value());
        CHECK(*observed_attempts[0] == 1);
        CHECK(*observed_attempts[1] == 1);
    }
    CHECK(usage_counter(storage, root.value(), "input") == 10);
    CHECK(usage_counter(storage, root.value(), "output") == 4);
    CHECK(usage_counter(storage, root.value(), "cacheWrite") == 2);
    CHECK(usage_counter(storage, root.value(), "totalTokens") == 16);
    auto live = live_document_value(storage, root.value());
    REQUIRE(live.has_value());
    CHECK(live->as_object()->empty());
    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("a provider-aborted response settles as a model error with its entry and usage",
        "[durable][workflow][issue1163]") {
    MemoryStorage storage;
    HarnessOptions options;
    options.settings.compaction.enabled = false;
    options.stream = [](ai::Model, ai::AiContext, ai::SimpleStreamOptions) {
        return ai::ModelStream{
                ai::ModelStreamProducer{[](ai::AssistantEventSink sink, ai::ModelStreamCompletion done) noexcept {
                    auto answer = ai::assistant_text_message("partial");
                    answer.provider = "test";
                    answer.model = "model";
                    answer.usage.input = 5;
                    answer.usage.output = 1;
                    answer.usage.total_tokens = 6;
                    answer.stop_reason = ai::AssistantStopReason::Aborted;
                    answer.error_message = "provider aborted";
                    auto delivered = sink(ai::AssistantDoneEvent{.reason = answer.stop_reason, .message = answer});
                    if (!delivered)
                        done(std::unexpected(delivered.error()));
                    else
                        done(std::move(answer));
                }}};
    };
    auto opened = consume(Harness::open(storage, std::move(options)));
    REQUIRE(opened.has_value());
    auto harness = std::move(opened.value());
    auto root = consume(harness->root(RootOptions{.model = ai::Model{.id = "model", .provider = "test"}}));
    REQUIRE(root.has_value());
    auto admitted = consume(harness->submit(root.value(), text_input("hi")));
    REQUIRE(admitted.has_value());
    auto settled = consume(admitted.value().wait());
    REQUIRE(settled.has_value());
    CHECK(settled.value().status == "unanswered");
    REQUIRE(settled.value().reason.has_value());
    CHECK(*settled.value().reason == "model_error");
    auto entries = storage.scan_entries(root.value(), 100);
    REQUIRE(entries.has_value());
    REQUIRE(entries->size() == 2);
    const auto* aborted = entry_model(entries->front()).as_object();
    REQUIRE(aborted != nullptr);
    CHECK(*aborted->at("stopReason").as_string() == "aborted");
    REQUIRE(aborted->contains("usage"));
    CHECK(*aborted->at("usage").as_object()->at("input").as_number() == 5);
    CHECK(usage_counter(storage, root.value(), "input") == 5);
    auto live = live_document_value(storage, root.value());
    REQUIRE(live.has_value());
    CHECK(live->as_object()->empty());
    auto tasks = storage.scan_tasks(100);
    REQUIRE(tasks.has_value());
    const auto task = std::ranges::find_if(*tasks, [&](const auto& record) {
        return record.kind == "pi.generation" && record.conversation_id == root.value();
    });
    REQUIRE(task != tasks->end());
    REQUIRE(task->state.outcome.has_value());
    CHECK(task->state.outcome->status == "failed");
    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("aborting a run clears the published attempt without recording an answer", "[durable][workflow][issue1163]") {
    MemoryStorage storage;
    std::binary_semaphore entered{0};
    std::mutex stop_mutex;
    std::condition_variable stop_cv;
    bool released = false;
    HarnessOptions options;
    options.stream = [&](ai::Model, ai::AiContext, ai::SimpleStreamOptions s) {
        return ai::ModelStream{ai::ModelStreamProducer{
                [&, stop = s.stop_token](ai::AssistantEventSink sink, ai::ModelStreamCompletion done) mutable noexcept {
                    entered.release();
                    std::stop_callback cb(stop, [&]() {
                        {
                            std::lock_guard lock(stop_mutex);
                            released = true;
                        }
                        stop_cv.notify_all();
                    });
                    std::unique_lock lock(stop_mutex);
                    const bool awakened = stop_cv.wait_for(
                            lock, std::chrono::seconds{3}, [&] { return released || stop.stop_requested(); });
                    if (!awakened || stop.stop_requested()) {
                        done(std::unexpected(support::make_error(support::ErrorCode::Cancelled, "aborted")));
                        return;
                    }
                    auto answer = ai::assistant_text_message("finished");
                    answer.provider = "test";
                    answer.model = "model";
                    answer.usage.input = 7;
                    answer.usage.total_tokens = 7;
                    auto delivered = sink(ai::AssistantDoneEvent{.reason = answer.stop_reason, .message = answer});
                    if (!delivered)
                        done(std::unexpected(delivered.error()));
                    else
                        done(std::move(answer));
                }}};
    };
    auto opened = consume(Harness::open(storage, std::move(options)));
    REQUIRE(opened.has_value());
    auto harness = std::move(opened.value());
    auto root = consume(harness->root(RootOptions{.model = ai::Model{.id = "model", .provider = "test"}}));
    REQUIRE(root.has_value());
    auto admitted = consume(harness->submit(root.value(), text_input("hi")));
    REQUIRE(admitted.has_value());
    REQUIRE(entered.try_acquire_for(std::chrono::seconds{5}));
    // The attempt is published while the stream runs.
    auto mid_attempt = live_generation_attempt(storage, root.value());
    REQUIRE(mid_attempt.has_value());
    CHECK(*mid_attempt == 1);
    const auto live_definition =
            detail::builtin_document("pi.live:v1", DocumentHistory::Latest, DocumentFork::Initial, [] {
                return chord::Json{chord::Json::Object{}};
            });
    REQUIRE(consume(harness->session().commit([&](Transaction& tx) -> support::Expected<void> {
        auto live = tx.doc(live_definition, DocumentAddress{.conversation_id = root.value()});
        if (!live) return std::unexpected(live.error());
        auto tools = live->set(chord::Path{chord::path_key("tools")}, chord::Json{chord::Json::Array{}});
        if (!tools) return std::unexpected(tools.error());
        return live->set(chord::Path{chord::path_key("nestedTools")}, chord::Json{chord::Json::Array{}});
    })).has_value());
    auto seeded = live_document_value(storage, root.value());
    REQUIRE(seeded.has_value());
    CHECK(seeded->as_object()->contains("tools"));
    CHECK(seeded->as_object()->contains("nestedTools"));
    REQUIRE(consume(harness->abort_conversation(root.value())).has_value());
    auto settled = consume(admitted.value().wait());
    REQUIRE(settled.has_value());
    CHECK(settled.value().status == "unanswered");
    REQUIRE(settled.value().reason.has_value());
    CHECK(*settled.value().reason == "aborted");
    auto live = live_document_value(storage, root.value());
    REQUIRE(live.has_value());
    CHECK_FALSE(live->as_object()->contains("generation"));
    CHECK_FALSE(live->as_object()->contains("tools"));
    CHECK_FALSE(live->as_object()->contains("nestedTools"));
    CHECK(live->as_object()->empty());
    auto entries = storage.scan_entries(root.value(), 100);
    REQUIRE(entries.has_value());
    CHECK(std::ranges::none_of(*entries, [](const auto& entry) { return entry.kind == "pi.assistant"; }));
    CHECK_FALSE(usage_counter(storage, root.value(), "input").has_value());
    auto tasks = storage.scan_tasks(100);
    REQUIRE(tasks.has_value());
    const auto task = std::ranges::find_if(*tasks, [&](const auto& record) {
        return record.kind == "pi.generation" && record.conversation_id == root.value();
    });
    REQUIRE(task != tasks->end());
    REQUIRE(task->state.outcome.has_value());
    CHECK(task->state.outcome->status == "aborted");
    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("a committed partial converts to an aborted entry with usage in the attempt publication",
        "[durable][workflow][issue1163]") {
    MemoryStorage storage;
    // The same record the root seeds pi.live:v1 with; the test stages one
    // committed partial through the public commit seam, the state an
    // interrupted attempt of a reopened host could leave behind.
    DocumentDefinition live_definition =
            detail::builtin_document("pi.live:v1", DocumentHistory::Latest, DocumentFork::Initial, [] {
                return chord::Json{chord::Json::Object{}};
            });
    chord::Json partial{chord::Json::Object{{"role", chord::Json{"assistant"}},
            {"content",
                    chord::Json{chord::Json::Array{chord::Json{
                            chord::Json::Object{{"type", chord::Json{"text"}}, {"text", chord::Json{"partial"}}}}}}},
            {"provider", chord::Json{"test"}},
            {"model", chord::Json{"model"}},
            {"usage",
                    chord::Json{chord::Json::Object{{"input", chord::Json{5.0}},
                            {"output", chord::Json{1.0}},
                            {"cacheRead", chord::Json{0.0}},
                            {"cacheWrite", chord::Json{0.0}},
                            {"totalTokens", chord::Json{6.0}},
                            {"cost",
                                    chord::Json{chord::Json::Object{{"input", chord::Json{0.0}},
                                            {"output", chord::Json{0.0}},
                                            {"cacheRead", chord::Json{0.0}},
                                            {"cacheWrite", chord::Json{0.0}},
                                            {"total", chord::Json{0.0}}}}}}}},
            {"stopReason", chord::Json{"length"}},
            {"timestamp", chord::Json{1.0}}}};
    std::optional<ConversationId> root_id{};
    std::mutex observed_mutex;
    std::vector<std::size_t> observed_user_entries;
    HarnessOptions options;
    options.stream = [&](ai::Model, ai::AiContext, ai::SimpleStreamOptions) {
        return ai::ModelStream{ai::ModelStreamProducer{
                [&](ai::AssistantEventSink sink, ai::ModelStreamCompletion done) mutable noexcept {
                    {
                        std::lock_guard lock(observed_mutex);
                        // The partial converted in the publication commit,
                        // before this stream ran.
                        if (root_id) {
                            auto entries = storage.scan_entries(*root_id, 100);
                            observed_user_entries.push_back(entries ? entries->size() : 0);
                        } else {
                            observed_user_entries.push_back(0);
                        }
                    }
                    auto answer = ai::assistant_text_message("finished");
                    answer.provider = "test";
                    answer.model = "model";
                    answer.usage.input = 7;
                    answer.usage.total_tokens = 7;
                    auto delivered = sink(ai::AssistantDoneEvent{.reason = answer.stop_reason, .message = answer});
                    if (!delivered)
                        done(std::unexpected(delivered.error()));
                    else
                        done(std::move(answer));
                }}};
    };
    auto opened = consume(Harness::open(storage, std::move(options)));
    REQUIRE(opened.has_value());
    auto harness = std::move(opened.value());
    auto root = consume(harness->root(RootOptions{.model = ai::Model{.id = "model", .provider = "test"}}));
    REQUIRE(root.has_value());
    root_id = root.value();
    REQUIRE(consume(harness->session().commit([&](Transaction& tx) -> support::Expected<void> {
        auto live = tx.doc(live_definition, DocumentAddress{.conversation_id = root.value()});
        if (!live) return std::unexpected(live.error());
        auto set = live->set(chord::Path{chord::path_key("generation")},
                chord::Json{chord::Json::Object{{"attempt", chord::Json{1}}, {"message", partial}}});
        if (!set) return std::unexpected(set.error());
        return {};
    })).has_value());
    auto admitted = consume(harness->submit(root.value(), text_input("hi")));
    REQUIRE(admitted.has_value());
    auto settled = consume(admitted.value().wait());
    REQUIRE(settled.has_value());
    CHECK(settled.value().status == "done");
    {
        std::lock_guard lock(observed_mutex);
        REQUIRE(observed_user_entries.size() == 1);
        // user + converted partial, published before the stream invocation.
        CHECK(observed_user_entries.front() == 2);
    }
    auto entries = storage.scan_entries(root.value(), 100);
    REQUIRE(entries.has_value());
    REQUIRE(entries->size() == 3);
    const auto* converted = entry_model((*entries)[1]).as_object();
    REQUIRE(converted != nullptr);
    CHECK(*converted->at("stopReason").as_string() == "aborted");
    REQUIRE(converted->contains("usage"));
    CHECK(*converted->at("usage").as_object()->at("input").as_number() == 5);
    // Both the converted partial and the final answer are in the ledger.
    CHECK(usage_counter(storage, root.value(), "input") == 12);
    CHECK(usage_counter(storage, root.value(), "totalTokens") == 13);
    auto live = live_document_value(storage, root.value());
    REQUIRE(live.has_value());
    CHECK(live->as_object()->empty());
    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("a failed attempt publication commit never invokes the model stream", "[durable][workflow][issue1163]") {
    GatedStorage storage;
    std::atomic_int requests{0};
    HarnessOptions options;
    options.stream = [&](ai::Model, ai::AiContext, ai::SimpleStreamOptions) {
        ++requests;
        return ai::ModelStream{
                ai::ModelStreamProducer{[](ai::AssistantEventSink sink, ai::ModelStreamCompletion done) noexcept {
                    auto answer = ai::assistant_text_message("finished");
                    auto delivered = sink(ai::AssistantDoneEvent{.reason = answer.stop_reason, .message = answer});
                    if (!delivered)
                        done(std::unexpected(delivered.error()));
                    else
                        done(std::move(answer));
                }}};
    };
    auto opened = consume(Harness::open(storage, std::move(options)));
    REQUIRE(opened.has_value());
    auto harness = std::move(opened.value());
    auto root = consume(harness->root(RootOptions{.model = ai::Model{.id = "model", .provider = "test"}}));
    REQUIRE(root.has_value());
    auto live_record = storage.find_document(
            "pi.live:v1", DocumentAddress{.conversation_id = root.value()}, DocumentPoint::current());
    REQUIRE(live_record.has_value());
    REQUIRE(live_record->has_value());
    const auto live_id = (**live_record).id;
    std::atomic_bool armed{false};
    REQUIRE(harness->session().subscribe_commits([&](const CommitPublication& publication) {
        for (const auto& change : publication.changes) {
            const auto* table = std::get_if<TableCommitChange>(&change);
            if (!table || !table->task || table->task->kind != "pi.generation" || !table->task->state.checkpoint)
                continue;
            const auto* checkpoint = table->task->state.checkpoint->as_object();
            if (!checkpoint) continue;
            const auto phase = checkpoint->find("phase");
            if (phase != checkpoint->end() && phase->second.as_string() && *phase->second.as_string() == "request" &&
                    !armed.exchange(true)) {
                storage.fail_next_commit("disk vanished");
            }
        }
    }));
    auto admitted = consume(harness->submit(root.value(), text_input("hi")));
    REQUIRE(admitted.has_value());
    auto receipt = consume(admitted.value().wait());
    CHECK_FALSE(receipt.has_value());
    CHECK(armed);
    // Inspect the rejected batch itself: an unrelated scheduler commit is not
    // evidence that request publication failed.
    const auto& rejected = storage.failed_writes();
    REQUIRE(std::ranges::any_of(rejected, [&](const StorageWrite& write) {
        return write.type == "document.change" && write.document_id == live_id;
    }));
    CHECK(requests == 0);
    CHECK_FALSE(live_generation_attempt(storage, root.value()).has_value());
    CHECK(harness->session().failed());
    REQUIRE(consume(harness->close()).has_value());
}

} // namespace cch::tests::durable
