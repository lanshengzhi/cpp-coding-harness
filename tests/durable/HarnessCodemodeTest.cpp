#include <cch/durable/Codemode.hpp>
#include <cch/durable/Environment.hpp>
#include <cch/durable/Harness.hpp>
#include <cch/durable/SqliteStorage.hpp>
#include <cch/durable/JsonlStorage.hpp>
#include "durable/FrozenExpectation.hpp"
#include "support/Json.hpp"
#include "support/TempWorkspace.hpp"
#include "support/UniqueFd.hpp"
#include <cch/durable/Storage.hpp>
#include "durable/Outcome.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <fstream>
#include <mutex>
#include <semaphore>
#include <memory>
#include <string>
#include <vector>

#include <poll.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace cch::tests::durable {
using namespace cch::durable;

namespace {

ai::ModelStreamFactory scripts(std::vector<std::string> sources, std::string tool_name = "codemode") {
    return [sources = std::move(sources), tool_name = std::move(tool_name), turn = std::size_t{0}](
                   ai::Model, ai::AiContext, ai::SimpleStreamOptions) mutable {
        auto message = ai::assistant_text_message("finished");
        if (turn % 2 == 0 && turn / 2 < sources.size()) {
            message.content = {ai::ToolCallContent{.id = "script-" + std::to_string(turn / 2),
                    .name = tool_name,
                    .arguments = support::JsonValue{support::JsonValue::object_t{{"code", sources[turn / 2]}}}}};
            message.stop_reason = ai::AssistantStopReason::ToolUse;
        }
        ++turn;
        return ai::ModelStream{ai::ModelStreamProducer{[message = std::move(message)](ai::AssistantEventSink sink,
                                                               ai::ModelStreamCompletion completion) mutable noexcept {
            auto delivered = sink(ai::AssistantDoneEvent{.reason = message.stop_reason, .message = message});
            if (!delivered)
                completion(std::unexpected(delivered.error()));
            else
                completion(std::move(message));
        }}};
    };
}

std::unique_ptr<Harness> open(
        Storage& storage, Registry& registry, std::vector<std::string> sources, std::string tool_name = "codemode") {
    auto opened = consume(Harness::open(storage,
            HarnessOptions{.registry = &registry, .stream = scripts(std::move(sources), std::move(tool_name))}));
    REQUIRE(opened.has_value());
    return std::move(opened.value());
}

EntryId submit(Harness& harness, std::optional<ConversationId> conversation = std::nullopt) {
    auto root = consume(harness.root());
    REQUIRE(root.has_value());
    auto submitted = consume(harness.submit(
            conversation.value_or(root.value()), SubmissionDraft{.body = InputSubmissionDraft{.content = "run"}}));
    REQUIRE(submitted.has_value());
    auto settled = consume(submitted.value().wait());
    REQUIRE(settled.has_value());
    CHECK(settled.value().status == "done");
    REQUIRE(settled.value().answer.has_value());
    return *settled.value().answer;
}

std::string result_text(const support::JsonValue& result) {
    std::string text;
    for (const auto& item : result.at("content").get_array())
        if (item.at("type").get_string() == "text") text += item.at("text").get_string();
    return text;
}

std::string normalized_text(const support::JsonValue& result) {
    auto text = result_text(result);
    const auto start = text.find("Wall time ");
    REQUIRE(start != std::string::npos);
    const auto end = text.find('\n', start);
    REQUIRE(end != std::string::npos);
    text.replace(start, end - start, "Wall time <seconds>");
    return text;
}

support::JsonValue last_result(Harness& harness, ConversationId conversation = ConversationId{.value = 1}) {
    auto view = consume(harness.conversation_view(conversation));
    REQUIRE(view.has_value());
    // Public committed view, including the model-visible result and its details.
    const auto json = view.value().to_support();
    const auto& entries = json.at("entries").get_array();
    const auto found = std::find_if(entries.rbegin(), entries.rend(), [](const auto& entry) {
        return entry.at("kind").get_string() == "pi.tool-result";
    });
    INFO(support::write_json(json).value());
    REQUIRE(found != entries.rend());
    return found->at("data").at("model").get_array().front();
}

void install(Registry& registry) {
    auto sandbox = codemode::CodemodeSandbox::create(codemode::default_codemode_guest_wasm_path());
    REQUIRE(sandbox.has_value());
    REQUIRE(registry.install(codemode_extension(std::move(*sandbox))).has_value());
    Extension extension{.name = "arithmetic"};
    extension.tools.push_back(ToolRegistration{.declaration = ai::Tool{.name = "double", .description = "Double n"},
            .execute = [](support::JsonValue args, std::stop_token) -> support::AsyncResult<std::vector<ai::Content>> {
                return support::AsyncResult<std::vector<ai::Content>>{
                        support::Expected<std::vector<ai::Content>>{std::vector<ai::Content>{
                                ai::text_content(std::to_string(static_cast<int>(args.at("n").get_number()) * 2))}}};
            }});
    REQUIRE(registry.install(std::move(extension)).has_value());
}

ToolRegistration paused_tool(std::atomic<bool>& first, std::binary_semaphore& reached) {
    return ToolRegistration{.declaration = ai::Tool{.name = "pause"},
            .execute = [&first, &reached](support::JsonValue,
                               std::stop_token stop) -> support::AsyncResult<std::vector<ai::Content>> {
                if (first) {
                    reached.release();
                    std::mutex mutex;
                    std::condition_variable_any ready;
                    std::unique_lock lock(mutex);
                    ready.wait(lock, stop, [] { return false; });
                    return support::AsyncResult<std::vector<ai::Content>>{std::unexpected(
                            support::make_error(support::ErrorCode::Cancelled, "paused tool cancelled"))};
                }
                return support::AsyncResult<std::vector<ai::Content>>{support::Expected<std::vector<ai::Content>>{
                        std::vector<ai::Content>{ai::text_content("resumed")}}};
            },
            .replay_safe = true};
}

support::Expected<std::unique_ptr<Storage>> persistent_storage(const std::filesystem::path& path, bool sqlite) {
    if (sqlite) {
        auto storage = SqliteStorage::open(path / "store.sqlite");
        if (!storage) return std::unexpected(storage.error());
        return std::unique_ptr<Storage>{std::move(*storage)};
    }
    auto storage = JsonlStorage::open(path / "jsonl");
    if (!storage) return std::unexpected(storage.error());
    return std::unique_ptr<Storage>{std::move(*storage)};
}

Extension crash_tools(const std::filesystem::path& directory, int ready_fd, bool first, bool replay_safe) {
    Extension extension{.name = "crash-tools"};
    extension.tools.push_back(ToolRegistration{.declaration = ai::Tool{.name = "effect"},
            .execute = [directory](support::JsonValue args,
                               std::stop_token) -> support::AsyncResult<std::vector<ai::Content>> {
                std::ofstream effect(directory / "effects", std::ios::app);
                effect << args.at("stamp").get_string() << '\n';
                effect.close();
                if (!effect)
                    return support::AsyncResult<std::vector<ai::Content>>{std::unexpected(
                            support::make_error(support::ErrorCode::Tool, "Cannot record test effect"))};
                return support::AsyncResult<std::vector<ai::Content>>{support::Expected<std::vector<ai::Content>>{
                        std::vector<ai::Content>{ai::text_content("effect done")}}};
            }});
    extension.tools.push_back(ToolRegistration{.declaration = ai::Tool{.name = "pause"},
            .execute = [ready_fd, first](
                               support::JsonValue, std::stop_token) -> support::AsyncResult<std::vector<ai::Content>> {
                if (first) {
                    const char marker = 'R';
                    if (::write(ready_fd, &marker, 1) != 1) _exit(91);
                    for (;;)
                        ::pause();
                }
                return support::AsyncResult<std::vector<ai::Content>>{support::Expected<std::vector<ai::Content>>{
                        std::vector<ai::Content>{ai::text_content("resumed")}}};
            },
            .replay_safe = replay_safe});
    return extension;
}

struct ChildGuard {
    pid_t pid{-1};
    ~ChildGuard() {
        if (pid > 0) {
            (void)::kill(pid, SIGKILL);
            int status{};
            while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
            }
        }
    }
};

} // namespace

TEST_CASE("Durable codemode calls a real nested tool and retains its store across turns",
        "[durable][codemode][issue1094]") {
    MemoryStorage storage;
    Registry registry;
    install(registry);
    auto harness = open(storage,
            registry,
            {"const doubled = await tools.double({ n: 21 }); store(\"answer\", doubled); return doubled;",
                    "return load(\"answer\");"});
    submit(*harness);
    auto first = last_result(*harness);
    CHECK_FALSE(first.at("isError").get_boolean());
    CHECK(result_text(first).ends_with("Output:\n42"));
    submit(*harness);
    auto second = last_result(*harness);
    CHECK_FALSE(second.at("isError").get_boolean());
    CHECK(result_text(second).ends_with("Output:\n42"));
    const auto frozen = FrozenScenario::load("fixtures/pi-v1.3.0/audit-1056/codemode-toolsearch.json",
            "code mode runs a script that calls a real nested tool and keeps its store across turns");
    REQUIRE(frozen.has_value());
    CHECK(normalized_text(first) == frozen->expected().at("calls").get_array().at(0).at("text").get_string());
    CHECK(normalized_text(second) == frozen->expected().at("calls").get_array().at(1).at("text").get_string());
    const auto store = consume(harness->session().snapshot(
            codemode_store_document(), DocumentAddress{.conversation_id = ConversationId{.value = 1}}));
    REQUIRE(store.has_value());
    REQUIRE(store.value().has_value());
    CHECK(json_equals(*store.value(), frozen->expected().at("store")));
    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("Durable codemode preserves successful nested history and discards failed store writes",
        "[durable][codemode][issue1094]") {
    MemoryStorage storage;
    Registry registry;
    install(registry);
    auto harness = open(storage,
            registry,
            {"store('answer', 'kept'); return 'initial';",
                    "await tools.double({ n: 2 }); store('answer', 'lost'); throw new Error('boom');",
                    "return load('answer');"});
    submit(*harness);
    submit(*harness);
    const auto result = last_result(*harness);
    CHECK(result.at("isError").get_boolean());
    const auto text = result_text(result);
    CHECK(text.find("Error: boom") != std::string::npos);
    CHECK(text.ends_with("Tool calls made before the failure (they are not undone): double (ok)"));
    const auto& rows = result.at("details").at("calls").get_array();
    REQUIRE(rows.size() == 1);
    CHECK(rows.front().at("id").get_number() == 1);
    CHECK(rows.front().at("name").get_string() == "double");
    CHECK(rows.front().at("args").get_string() == "{\"n\":2}");
    CHECK(rows.front().at("status").get_string() == "ok");
    submit(*harness);
    CHECK(result_text(last_result(*harness)).ends_with("Output:\nkept"));
    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("Durable codemode resumes recorded nested answers after reopening SQLite", "[durable][codemode][issue1094]") {
    tests::TempWorkspace workspace;
    const auto path = workspace.path() / "codemode.sqlite";
    auto storage = SqliteStorage::open(path);
    REQUIRE(storage.has_value());
    Registry registry;
    install(registry);
    std::binary_semaphore reached{0};
    std::atomic<bool> first{true};
    std::atomic<int> effects{0};
    std::string stamp;
    Extension extension{.name = "replay-tools"};
    extension.tools.push_back(ToolRegistration{.declaration = ai::Tool{.name = "effect"},
            .execute = [&effects, &stamp](support::JsonValue args,
                               std::stop_token) -> support::AsyncResult<std::vector<ai::Content>> {
                ++effects;
                stamp = args.at("stamp").get_string();
                return support::AsyncResult<std::vector<ai::Content>>{support::Expected<std::vector<ai::Content>>{
                        std::vector<ai::Content>{ai::text_content("effect done")}}};
            }});
    extension.tools.push_back(paused_tool(first, reached));
    REQUIRE(registry.install(std::move(extension)).has_value());
    auto harness = open(**storage,
            registry,
            {"const stamp = Date.now() + ':' + Math.random(); await tools.effect({stamp}); await tools.pause(); "
             "store('stamp', stamp); return stamp;"});
    auto root = consume(harness->root());
    REQUIRE(root.has_value());
    auto input =
            consume(harness->submit(root.value(), SubmissionDraft{.body = InputSubmissionDraft{.content = "start"}}));
    REQUIRE(input.has_value());
    REQUIRE(reached.try_acquire_for(std::chrono::seconds{5}));
    REQUIRE(consume(harness->close()).has_value());
    harness.reset();
    REQUIRE((*storage)->close().has_value());
    storage->reset();
    first = false;
    storage = SqliteStorage::open(path);
    REQUIRE(storage.has_value());
    harness = open(**storage, registry, {});
    REQUIRE(harness->resume().has_value());
    REQUIRE(consume(harness->wait_for_idle(root.value())).has_value());
    CHECK(effects == 1);
    const auto result = last_result(*harness);
    CHECK_FALSE(result.at("isError").get_boolean());
    CHECK(result_text(result).ends_with(stamp));
    CHECK(result.at("details").at("calls").get_array().size() == 2);
    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE(
        "Cancelling durable codemode retains completed and cancelled nested calls", "[durable][codemode][issue1094]") {
    MemoryStorage storage;
    std::binary_semaphore reached{0};
    std::atomic<bool> first{true};
    Registry registry;
    install(registry);
    Extension extension{.name = "held-tool"};
    extension.tools.push_back(paused_tool(first, reached));
    REQUIRE(registry.install(std::move(extension)).has_value());
    auto harness = open(storage, registry, {"await tools.double({ n: 2 }); store('lost', true); await tools.pause();"});
    auto root = consume(harness->root());
    REQUIRE(root.has_value());
    auto input =
            consume(harness->submit(root.value(), SubmissionDraft{.body = InputSubmissionDraft{.content = "start"}}));
    REQUIRE(input.has_value());
    REQUIRE(reached.try_acquire_for(std::chrono::seconds{5}));
    REQUIRE(consume(harness->abort_conversation(root.value())).has_value());
    const auto result = last_result(*harness);
    CHECK(result.at("isError").get_boolean());
    const auto& rows = result.at("details").at("calls").get_array();
    REQUIRE(rows.size() == 2);
    CHECK(rows.at(0).at("status").get_string() == "ok");
    CHECK(rows.at(1).at("status").get_string() == "cancelled");
    CHECK(result_text(result).find("double (ok), pause (cancelled)") != std::string::npos);
    const auto store = consume(
            harness->session().snapshot(codemode_store_document(), DocumentAddress{.conversation_id = root.value()}));
    REQUIRE(store.has_value());
    REQUIRE(store.value().has_value());
    CHECK(store.value()->to_support().at("values").get_object().empty());
    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("Durable codemode returns the frozen mixed output through Harness", "[durable][codemode][issue1094]") {
    const auto frozen = FrozenScenario::load("fixtures/pi-v1.3.0/audit-1094/codemode.json",
            "durable codemode returns ordered text image console and value output");
    REQUIRE(frozen.has_value());
    MemoryStorage storage;
    Registry registry;
    install(registry);
    auto harness = open(storage,
            registry,
            {frozen_artifact("fixtures/pi-v1.3.0/audit-1094/codemode.json")
                            .at("scenarios")
                            .get_array()
                            .front()
                            .at("inputs")
                            .at("source")
                            .get_string()});
    submit(*harness);
    auto result = last_result(*harness);
    auto& header = result.at("content").get_array().front().at("text").get_string();
    const auto start = header.find("Wall time ");
    REQUIRE(start != std::string::npos);
    const auto end = header.find('\n', start);
    REQUIRE(end != std::string::npos);
    header.replace(start, end - start, "Wall time <seconds>");
    CHECK(result.at("isError").get_boolean() == frozen->expected().at("isError").get_boolean());
    CHECK(support::canonical_json(result.at("content")).value() ==
            support::canonical_json(frozen->expected().at("content")).value());
    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("Durable codemode runs real write edit read and bash tools", "[durable][codemode][issue1094]") {
    tests::TempWorkspace workspace;
    MemoryStorage storage;
    Registry registry;
    install(registry);
    REQUIRE(registry.install(coding_tools(local_environment(workspace.path().string()))).has_value());
    auto harness = open(storage,
            registry,
            {"await tools.write({path:'note', content:'alpha'}); await tools.edit({path:'note', "
             "edits:[{oldText:'alpha',newText:'beta'}]}); const value = await tools.read({path:'note'}); const shell = "
             "await tools.bash({command:'printf shell'}); return value + ':' + shell;"});
    submit(*harness);
    const auto result = last_result(*harness);
    CHECK_FALSE(result.at("isError").get_boolean());
    CHECK(result_text(result).ends_with("Output:\nbeta:shell"));
    CHECK(result.at("details").at("calls").get_array().size() == 4);
    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("Durable codemode replays committed answers after SIGKILL on SQLite and JSONL",
        "[durable][codemode][issue1094]") {
    const bool sqlite = GENERATE(true, false);
    const bool replay_safe = GENERATE(true, false);
    CAPTURE(sqlite, replay_safe);
    const auto frozen = FrozenScenario::load("fixtures/pi-v1.3.0/audit-1094/codemode.json",
            replay_safe ? "durable codemode reopens with the same clock seed and recorded nested answer"
                        : "durable codemode refuses to repeat an unsafe interrupted nested call");
    REQUIRE(frozen.has_value());
    tests::TempWorkspace workspace;
    int descriptors[2]{};
    REQUIRE(::pipe(descriptors) == 0);
    support::UniqueFd read_fd{descriptors[0]};
    support::UniqueFd write_fd{descriptors[1]};
    const auto parent = ::getpid();
    const auto pid = ::fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        if (::prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 || ::getppid() != parent) _exit(90);
        read_fd.reset();
        auto storage = persistent_storage(workspace.path(), sqlite);
        if (!storage) _exit(92);
        Registry registry;
        install(registry);
        if (!registry.install(crash_tools(workspace.path(), write_fd.get(), true, replay_safe))) _exit(93);
        auto harness = open(**storage,
                registry,
                {"const stamp = Date.now() + ':' + Math.random(); await tools.effect({stamp}); await tools.pause(); "
                 "store('stamp', stamp); return stamp;"});
        submit(*harness);
        _exit(94);
    }
    ChildGuard child{pid};
    write_fd.reset();
    pollfd ready{.fd = read_fd.get(), .events = POLLIN, .revents = 0};
    REQUIRE(::poll(&ready, 1, 10000) == 1);
    char marker{};
    REQUIRE(::read(read_fd.get(), &marker, 1) == 1);
    REQUIRE(marker == 'R');
    REQUIRE(::kill(pid, SIGKILL) == 0);
    int status{};
    REQUIRE(::waitpid(pid, &status, 0) == pid);
    child.pid = -1;
    CHECK(WIFSIGNALED(status));
    CHECK(WTERMSIG(status) == SIGKILL);
    auto storage = persistent_storage(workspace.path(), sqlite);
    REQUIRE(storage.has_value());
    Registry registry;
    install(registry);
    REQUIRE(registry.install(crash_tools(workspace.path(), -1, false, replay_safe)).has_value());
    auto harness = open(**storage, registry, {});
    REQUIRE(harness->resume().has_value());
    REQUIRE(consume(harness->wait_for_idle(ConversationId{.value = 1})).has_value());
    const auto result = last_result(*harness);
    const auto environment = local_environment(workspace.path().string());
    const auto effects = environment->read("effects", {});
    REQUIRE(effects.has_value());
    const auto stamp = effects->substr(0, effects->find('\n'));
    const auto store = consume(harness->session().snapshot(
            codemode_store_document(), DocumentAddress{.conversation_id = ConversationId{.value = 1}}));
    REQUIRE(store.has_value());
    REQUIRE(store.value().has_value());
    support::JsonValue::array_t statuses;
    for (const auto& call : result.at("details").at("calls").get_array())
        statuses.push_back(call.at("status"));
    const support::JsonValue actual{
            support::JsonValue::object_t{{"effectCalls", static_cast<double>(std::ranges::count(*effects, '\n'))},
                    {"isError", result.at("isError")},
                    {"outputMatchesOriginalStamp", result_text(result).ends_with(stamp)},
                    {"storeMatchesOriginalStamp",
                            [&store, &stamp] {
                                const auto values = store.value()->to_support().at("values").get_object();
                                const auto found = values.find("stamp");
                                return found != values.end() && found->second.get_string() == stamp;
                            }()},
                    {"statuses", support::JsonValue{std::move(statuses)}}}};
    CHECK(support::canonical_json(actual).value() == support::canonical_json(frozen->expected()).value());
    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("Durable codemode returns the frozen failure text and nested history", "[durable][codemode][issue1094]") {
    const auto frozen = FrozenScenario::load("fixtures/pi-v1.3.0/audit-1056/codemode-toolsearch.json",
            "a failed code mode script reports its error and the nested calls already made");
    REQUIRE(frozen.has_value());
    MemoryStorage storage;
    Registry registry;
    install(registry);
    auto harness = open(storage, registry, {"await tools.double({ n: 2 }); throw new Error(\"boom\");"});
    submit(*harness);
    const auto result = last_result(*harness);
    CHECK(normalized_text(result) == frozen->expected().at("text").get_string());
    CHECK(result.at("isError").get_boolean() == frozen->expected().at("isError").get_boolean());
    auto calls = result.at("details").at("calls");
    for (auto& row : calls.get_array()) {
        const bool duration = row.get_object().erase("durationMs") == 1;
        row.get_object().emplace("durationPresent", duration);
    }
    CHECK(support::canonical_json(calls).value() ==
            support::canonical_json(frozen->expected().at("details").at("calls")).value());
    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("Durable codemode store forks as of the selected answer and supports deletion",
        "[durable][codemode][issue1094]") {
    MemoryStorage storage;
    Registry registry;
    install(registry);
    auto harness = open(storage,
            registry,
            {"store('answer', 'one');",
                    "store('answer', 'two');",
                    "return load('answer');",
                    "store('answer', undefined);"});
    const auto at = submit(*harness);
    submit(*harness);
    auto forked = consume(harness->fork_conversation(ConversationId{.value = 1}, at));
    REQUIRE(forked.has_value());
    submit(*harness, forked.value().id);
    CHECK(result_text(last_result(*harness, forked.value().id)).ends_with("Output:\none"));
    submit(*harness, forked.value().id);
    const auto branch = consume(harness->session().snapshot(
            codemode_store_document(), DocumentAddress{.conversation_id = forked.value().id}));
    const auto root = consume(harness->session().snapshot(
            codemode_store_document(), DocumentAddress{.conversation_id = ConversationId{.value = 1}}));
    REQUIRE(branch.has_value());
    REQUIRE(branch.value().has_value());
    REQUIRE(root.has_value());
    REQUIRE(root.value().has_value());
    CHECK(branch.value()->to_support().at("values").get_object().empty());
    CHECK(root.value()->to_support().at("values").at("answer").get_string() == "two");
    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("Invalid durable codemode source settles as a tool error", "[durable][codemode][issue1094]") {
    MemoryStorage storage;
    Registry registry;
    install(registry);
    auto harness = open(storage, registry, {"// @options: {\"unknown\": true}\nreturn 1;"});
    submit(*harness);
    CHECK(last_result(*harness).at("isError").get_boolean());
    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("Durable codemode catches a nested failure and retains its error row", "[durable][codemode][issue1094]") {
    MemoryStorage storage;
    Registry registry;
    install(registry);
    Extension extension{.name = "failing-tool"};
    extension.tools.push_back(ToolRegistration{.declaration = ai::Tool{.name = "fail"},
            .execute = [](support::JsonValue, std::stop_token) -> support::AsyncResult<std::vector<ai::Content>> {
                return support::AsyncResult<std::vector<ai::Content>>{
                        std::unexpected(support::make_error(support::ErrorCode::Tool, "nested boom"))};
            }});
    REQUIRE(registry.install(std::move(extension)).has_value());
    auto harness = open(storage,
            registry,
            {"try { await tools.fail({}); } catch(e) { text(e.message); } return await tools.double({n:3});"});
    submit(*harness);
    const auto result = last_result(*harness);
    CHECK_FALSE(result.at("isError").get_boolean());
    const auto text = result_text(result);
    CHECK(text.find("nested boom") != std::string::npos);
    CHECK(text.ends_with("6"));
    const auto& calls = result.at("details").at("calls").get_array();
    REQUIRE(calls.size() == 2);
    CHECK(calls.at(0).at("status").get_string() == "error");
    CHECK(calls.at(0).at("error").get_string() == "nested boom");
    CHECK(calls.at(1).at("status").get_string() == "ok");
    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("Durable codemode nested tools use the request tool hooks", "[durable][codemode][issue1094]") {
    MemoryStorage storage;
    Registry registry;
    install(registry);
    Extension hooks{.name = "nested-hooks"};
    hooks.hooks.push_back(
            tool_hook("beforeTool", [](const chord::Json& input, TaskRuntime&) -> support::AsyncResult<chord::Json> {
                const auto json = input.to_support();
                chord::Json result;
                if (json.at("name").get_string() == "double")
                    result = chord::Json{
                            chord::Json::Object{{"arguments", chord::Json{chord::Json::Object{{"n", 21.0}}}}}};
                return support::AsyncResult<chord::Json>{support::Expected<chord::Json>{std::move(result)}};
            }));
    REQUIRE(registry.install(std::move(hooks)).has_value());
    auto harness = open(storage, registry, {"return await tools.double({n:2});"});
    submit(*harness);
    const auto result = last_result(*harness);
    CHECK(result_text(result).ends_with("Output:\n42"));
    CHECK(result.at("details").at("calls").get_array().front().at("args").get_string() == "{\"n\":2}");
    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("A nested result completed during durable abort retains its successful history",
        "[durable][codemode][issue1094]") {
    using Completion = support::AsyncCompletion<std::vector<ai::Content>, support::Error>;
    struct Pending {
        std::mutex mutex;
        std::optional<Completion> complete;
        std::optional<std::stop_callback<std::move_only_function<void()>>> stop;
        std::binary_semaphore entered{0};
        std::binary_semaphore cancelled{0};
    };
    auto pending = std::make_shared<Pending>();
    MemoryStorage storage;
    Registry registry;
    install(registry);
    Extension extension{.name = "late-result"};
    extension.tools.push_back(ToolRegistration{.declaration = ai::Tool{.name = "late"},
            .execute = [pending](support::JsonValue,
                               std::stop_token stop) -> support::AsyncResult<std::vector<ai::Content>> {
                return support::AsyncResult<std::vector<ai::Content>>{
                        support::AsyncProducer<std::vector<ai::Content>, support::Error>{
                                [pending, stop](Completion complete) noexcept {
                                    {
                                        std::lock_guard lock(pending->mutex);
                                        pending->complete.emplace(std::move(complete));
                                        // The tool registration and test own pending until completion.
                                        pending->stop.emplace(
                                                stop, [value = pending.get()] { value->cancelled.release(); });
                                    }
                                    pending->entered.release();
                                }}};
            }});
    REQUIRE(registry.install(std::move(extension)).has_value());
    auto harness = open(storage, registry, {"const value = await tools.late(); store('lost', value);"});
    auto root = consume(harness->root());
    REQUIRE(root.has_value());
    auto input =
            consume(harness->submit(root.value(), SubmissionDraft{.body = InputSubmissionDraft{.content = "start"}}));
    REQUIRE(input.has_value());
    REQUIRE(pending->entered.try_acquire_for(std::chrono::seconds{5}));
    std::binary_semaphore aborted{0};
    std::optional<support::ExpectedVoid> abort_result;
    // The semaphore wait retains these callback locals through completion.
    harness->abort_conversation(root.value()).start([&aborted, &abort_result](support::ExpectedVoid result) noexcept {
        abort_result.emplace(std::move(result));
        aborted.release();
    });
    REQUIRE(pending->cancelled.try_acquire_for(std::chrono::seconds{5}));
    std::optional<Completion> completion;
    {
        std::lock_guard lock(pending->mutex);
        completion = std::move(pending->complete);
    }
    REQUIRE(completion.has_value());
    (*completion)(std::vector<ai::Content>{ai::text_content("completed")});
    REQUIRE(aborted.try_acquire_for(std::chrono::seconds{5}));
    REQUIRE(abort_result.has_value());
    CHECK(abort_result->has_value());
    const auto result = last_result(*harness);
    const auto& calls = result.at("details").at("calls").get_array();
    REQUIRE(calls.size() == 1);
    CHECK(calls.front().at("status").get_string() == "ok");
    CHECK(result_text(result).find("late (ok)") != std::string::npos);
    const auto store = consume(
            harness->session().snapshot(codemode_store_document(), DocumentAddress{.conversation_id = root.value()}));
    REQUIRE(store.has_value());
    REQUIRE(store.value().has_value());
    CHECK(store.value()->to_support().at("values").get_object().empty());
    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("Durable recovery does not re-execute an unsafe ordinary tool", "[durable][codemode][issue1094]") {
    tests::TempWorkspace workspace;
    const auto path = workspace.path() / "ordinary.sqlite";
    auto storage = SqliteStorage::open(path);
    REQUIRE(storage.has_value());
    std::binary_semaphore reached{0};
    std::atomic<bool> first{true};
    std::atomic<int> effects{0};
    Registry registry;
    auto pause = paused_tool(first, reached);
    pause.replay_safe = false;
    auto original = std::move(pause.execute);
    pause.execute = [original = std::move(original), &effects](support::JsonValue args, std::stop_token stop) mutable {
        ++effects;
        return original(std::move(args), stop);
    };
    Extension extension{.name = "ordinary"};
    extension.tools.push_back(std::move(pause));
    REQUIRE(registry.install(std::move(extension)).has_value());
    auto harness = open(**storage, registry, {"ignored"}, "pause");
    auto root = consume(harness->root());
    REQUIRE(root.has_value());
    auto input =
            consume(harness->submit(root.value(), SubmissionDraft{.body = InputSubmissionDraft{.content = "start"}}));
    REQUIRE(input.has_value());
    REQUIRE(reached.try_acquire_for(std::chrono::seconds{5}));
    REQUIRE(consume(harness->close()).has_value());
    harness.reset();
    REQUIRE((*storage)->close().has_value());
    storage->reset();
    first = false;
    storage = SqliteStorage::open(path);
    REQUIRE(storage.has_value());
    harness = open(**storage, registry, {});
    REQUIRE(harness->resume().has_value());
    REQUIRE(consume(harness->wait_for_idle(root.value())).has_value());
    CHECK(effects == 1);
    REQUIRE(consume(harness->close()).has_value());
}

} // namespace cch::tests::durable
