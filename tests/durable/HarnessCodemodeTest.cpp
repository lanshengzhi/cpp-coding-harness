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
#include <filesystem>
#include <fstream>
#include <mutex>
#include <regex>
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

/// The nested calls one abort left behind: the call name and terminal outcome of
/// every `pi.tool` task whose input is a nested call (frozen pi `NestedCallDoc`
/// history, read through the task records). The scan runs on the Session line, so
/// it cannot race the Harness's own storage access.
std::vector<support::JsonValue> nested_task_records(Harness& harness) {
    auto rows = std::make_shared<std::vector<support::JsonValue>>();
    auto read = consume(harness.session().read_committed([rows](Storage& storage) -> support::Expected<chord::Json> {
        auto tasks = storage.scan_tasks(1024);
        if (!tasks) return std::unexpected(tasks.error());
        for (const auto& task : *tasks) {
            if (task.kind != "pi.tool") continue;
            const auto input = task.input.to_support();
            if (input.at("kind").get_string() != "nested") continue;
            rows->push_back(support::JsonValue{support::JsonValue::object_t{{"key", input.at("key")},
                    {"name", input.at("call").at("name")},
                    {"ownerPresent", task.owner.has_value()},
                    {"outcome",
                            task.state.outcome.has_value() ? support::JsonValue{task.state.outcome->status}
                                                           : support::JsonValue{}}}});
        }
        std::ranges::sort(*rows, [](const support::JsonValue& left, const support::JsonValue& right) {
            return left.at("key").get_string() < right.at("key").get_string();
        });
        return chord::Json{chord::Json::Object{}};
    }));
    if (!read.has_value()) return {};
    return *rows;
}

namespace {
void install_codemode(Registry& registry, std::shared_ptr<Environment> environment = nullptr) {
    auto sandbox = codemode::CodemodeSandbox::create(codemode::default_codemode_guest_wasm_path());
    REQUIRE(sandbox.has_value());
    REQUIRE(registry.install(codemode_extension(std::move(*sandbox), std::move(environment))).has_value());
}
} // namespace

void install(Registry& registry) {
    install_codemode(registry);
    Extension extension{.name = "arithmetic"};
    extension.tools.push_back(ToolRegistration{.declaration = ai::Tool{.name = "double", .description = "Double n"},
            .execute = [](support::JsonValue args, std::stop_token) -> support::AsyncResult<std::vector<ai::Content>> {
                return support::AsyncResult<std::vector<ai::Content>>{
                        support::Expected<std::vector<ai::Content>>{std::vector<ai::Content>{
                                ai::text_content(std::to_string(static_cast<int>(args.at("n").get_number()) * 2))}}};
            }});
    REQUIRE(registry.install(std::move(extension)).has_value());
}

/// A real temporary directory whose writes can be failed or observed: a failed
/// spill/image keeps a truthful label instead of naming a file that does not
/// exist, and a cancelled run must never reach the writer at all.
class OutputSpyEnvironment final : public Environment {
public:
    OutputSpyEnvironment(std::shared_ptr<Environment> inner, bool fail_writes)
        : inner_(std::move(inner)), fail_writes_(fail_writes) {}
    support::Expected<std::string> read(std::string_view path, std::stop_token stop) override {
        return inner_->read(path, stop);
    }
    support::Expected<std::string> read(
            std::string_view path, std::uintmax_t offset, std::size_t length, std::stop_token stop) override {
        return inner_->read(path, offset, length, stop);
    }
    support::ExpectedVoid write(std::string_view path, std::string_view content, std::stop_token stop) override {
        writes.emplace_back(path);
        if (fail_writes_)
            return std::unexpected(support::make_error(support::ErrorCode::Workspace, "injected output failure"));
        return inner_->write(path, content, stop);
    }
    support::ExpectedVoid append(std::string_view path, std::string_view content, std::stop_token stop) override {
        return inner_->append(path, content, stop);
    }
    support::Expected<FileInfo> stat(std::string_view path, std::stop_token stop) override {
        return inner_->stat(path, stop);
    }
    support::Expected<std::unique_ptr<DirectoryReader>> open_directory(
            std::string_view path, std::stop_token stop) override {
        return inner_->open_directory(path, stop);
    }
    support::Expected<FileListing> list_files(
            std::string_view path, ListOptions options, std::stop_token stop) override {
        return inner_->list_files(path, std::move(options), stop);
    }
    support::Expected<std::string> create_temp_dir(std::string_view prefix, std::stop_token stop) override {
        auto created = inner_->create_temp_dir(prefix, stop);
        if (created) created_dirs.push_back(*created);
        return created;
    }
    support::Expected<ShellResult> exec(const ShellCommand& command,
            std::chrono::milliseconds timeout,
            OutputSink output,
            std::stop_token stop,
            ShellOutputWindow window) override {
        return inner_->exec(command, timeout, std::move(output), stop, window);
    }
    support::Expected<std::unique_ptr<FileWatch>> watch(
            std::string_view path, WatchChangeSink on_change, WatchOptions options) override {
        return inner_->watch(path, std::move(on_change), std::move(options));
    }

    std::vector<std::string> writes;
    std::vector<std::string> created_dirs;

private:
    std::shared_ptr<Environment> inner_;
    bool fail_writes_{};
};

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
    // The parent's abort reports one harness error and no nested details (frozen
    // pi `ToolTask.abort`); the nested calls' own durable records carry the history.
    CHECK(result_text(result) == "<harness>\n[error] Tool codemode was aborted\n</harness>");
    const auto nested = nested_task_records(*harness);
    REQUIRE(nested.size() == 2);
    CHECK(nested.at(0).at("name").get_string() == "double");
    CHECK(nested.at(0).at("ownerPresent").get_boolean());
    CHECK(nested.at(0).at("outcome").get_string() == "completed");
    CHECK(nested.at(1).at("name").get_string() == "pause");
    CHECK(nested.at(1).at("outcome").get_string() == "aborted");
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
    CHECK(calls.at(0).at("error").get_string() == "<harness>\n[error] nested boom\n</harness>");
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
    // The completed nested call keeps its own terminal record even though the
    // parent's abort decided the call's outcome.
    CHECK(result.at("isError").get_boolean());
    CHECK(result_text(result) == "<harness>\n[error] Tool codemode was aborted\n</harness>");
    const auto nested = nested_task_records(*harness);
    REQUIRE(nested.size() == 1);
    CHECK(nested.front().at("name").get_string() == "late");
    CHECK(nested.front().at("outcome").get_string() == "completed");
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

namespace {
using Json = support::JsonValue;
using Object = Json::object_t;
using Array = Json::array_t;

const Json* field(const Json& value, std::string_view key) {
    const auto* object = value.get_if<Json::object_t>();
    if (object == nullptr) return nullptr;
    const auto found = object->find(std::string{key});
    return found == object->end() ? nullptr : &found->second;
}

/// The capture records every run of 100 or more newlines as `<newlines:N>`, so
/// the line-budget scenarios stay readable without losing their exact runs.
std::string collapse_newline_runs(std::string text) {
    std::string collapsed;
    collapsed.reserve(text.size());
    for (std::size_t index = 0; index < text.size();) {
        if (text[index] != '\n') {
            collapsed.push_back(text[index++]);
            continue;
        }
        std::size_t end = index;
        while (end < text.size() && text[end] == '\n')
            ++end;
        if (end - index >= 100)
            collapsed += std::format("<newlines:{}>", end - index);
        else
            collapsed.append(text, index, end - index);
        index = end;
    }
    return collapsed;
}
} // namespace

TEST_CASE("Durable codemode output matches frozen budgets spill and saved images", "[durable][codemode][issue1172]") {
    constexpr std::string_view kArtifact = "fixtures/pi-v1.3.0/audit-1172/codemode-output.json";
    const std::array<std::string, 11> names{
            "token budget truncates output and writes the complete text",
            "text exactly at the token budget is not truncated",
            "a script image is saved through the execution environment",
            "no execution environment keeps the frozen unsupported labels",
            "a failed script keeps partial output and spills it",
            "unicode truncation keeps whole BMP characters",
            "ordered text console images and values keep production order",
            "one character over the token budget truncates",
            "the line budget exactly at its limit is not truncated",
            "one line over the line budget truncates and spills",
            "a token budget larger than the text still clamps the tail to the whole line window",
    };
    const auto norm = [](std::string text, std::string_view directory) {
        text = std::regex_replace(text, std::regex{"Wall time [\\d.]+ seconds"}, "Wall time <seconds>");
        if (!directory.empty()) {
            std::size_t position = 0;
            while ((position = text.find(directory, position)) != std::string::npos) {
                text.replace(position, directory.size(), "<output-dir>");
                position += std::string_view{"<output-dir>"}.size();
            }
        }
        return collapse_newline_runs(std::move(text));
    };
    for (const auto& name : names) {
        DYNAMIC_SECTION(name) {
            const auto frozen = FrozenScenario::load(kArtifact, name);
            REQUIRE(frozen.has_value());
            const auto& inputs = frozen->inputs();
            REQUIRE(inputs.at("source").holds<std::string>());
            std::string source = inputs.at("source").get_string();
            if (const auto* max_tokens = field(inputs, "maxTokens")) {
                source = std::format("// @options: {{\"max_output_tokens\": {}}}\n{}",
                        static_cast<std::int64_t>(max_tokens->get_number()),
                        source);
            }
            const bool env = field(inputs, "environment") != nullptr && field(inputs, "environment")->get_boolean();
            tests::TempWorkspace workspace;
            MemoryStorage storage;
            Registry registry;
            std::shared_ptr<Environment> environment;
            if (env) environment = local_environment(workspace.path().string());
            install_codemode(registry, environment);
            auto harness = open(storage, registry, {source});
            submit(*harness);
            const auto result = last_result(*harness);
            REQUIRE(consume(harness->close()).has_value());
            const auto* details = field(result, "details");
            const auto* full = details == nullptr ? nullptr : field(*details, "fullOutputPath");
            std::string directory;
            if (full != nullptr && full->holds<std::string>())
                directory = std::filesystem::path{full->get_string()}.parent_path().string();
            std::string raw_text;
            for (const auto& block : result.at("content").get_array())
                if (block.at("type").get_string() == "text") raw_text += block.at("text").get_string();
            std::vector<std::string> image_paths;
            const std::regex image_path_pattern{R"(\[Image saved to (.+?) \()"};
            for (std::sregex_iterator it{raw_text.begin(), raw_text.end(), image_path_pattern}, end; it != end; ++it)
                image_paths.push_back((*it)[1].str());
            if (directory.empty() && !image_paths.empty())
                directory = std::filesystem::path{image_paths.front()}.parent_path().string();

            Array content;
            std::string text;
            for (const auto& block : result.at("content").get_array()) {
                if (block.at("type").get_string() == "text") {
                    const auto normalized = norm(block.at("text").get_string(), directory);
                    text += normalized;
                    content.push_back(Json{Object{{"type", "text"}, {"text", normalized}}});
                } else {
                    content.push_back(Json{
                            Object{{"type", "image"}, {"mimeType", block.at("mimeType")}, {"data", block.at("data")}}});
                }
            }
            Array calls;
            if (const auto* rows = details == nullptr ? nullptr : field(*details, "calls"); rows != nullptr) {
                for (const auto& call : rows->get_array()) {
                    Object row{{"id", call.at("id")},
                            {"name", call.at("name")},
                            {"args", call.at("args")},
                            {"status", call.at("status")},
                            {"durationPresent", field(call, "durationMs") != nullptr}};
                    calls.push_back(Json{std::move(row)});
                }
            }
            Json observed_full{nullptr};
            if (full != nullptr && full->holds<std::string>())
                observed_full = Json{norm(full->get_string(), directory)};
            const auto observed = Json{Object{{"isError", result.at("isError")},
                    {"content", Json{std::move(content)}},
                    {"text", text},
                    {"details", Json{Object{{"calls", Json{std::move(calls)}}, {"fullOutputPath", observed_full}}}}}};

            const auto& expected = frozen->expected();
            CHECK(observed.at("isError").get_boolean() == expected.at("isError").get_boolean());
            CHECK(support::canonical_json(observed.at("content")).value() ==
                    support::canonical_json(expected.at("content")).value());
            CHECK(observed.at("text").get_string() == expected.at("text").get_string());
            CHECK(support::canonical_json(observed.at("details").at("calls")).value() ==
                    support::canonical_json(expected.at("details").at("calls")).value());
            CHECK(support::canonical_json(observed.at("details").at("fullOutputPath")).value() ==
                    support::canonical_json(expected.at("details").at("fullOutputPath")).value());

            const auto* spill = field(expected, "spill");
            if (spill == nullptr || spill->holds<std::nullptr_t>()) {
                CHECK(full == nullptr);
            } else if (full != nullptr && full->holds<std::string>()) {
                REQUIRE(environment != nullptr);
                const auto bytes = environment->read(full->get_string(), {});
                REQUIRE(bytes.has_value());
                CHECK(bytes->size() == static_cast<std::size_t>(spill->at("bytes").get_number()));
                CHECK(norm(*bytes, directory) == spill->at("text").get_string());
            } else {
                FAIL("the frozen spill has no output path");
            }

            const auto* images = field(expected, "images");
            REQUIRE(images != nullptr);
            CHECK(image_paths.size() == images->get_array().size());
            for (std::size_t index = 0; index < std::min(image_paths.size(), images->get_array().size()); ++index) {
                REQUIRE(environment != nullptr);
                const auto bytes = environment->read(image_paths[index], {});
                REQUIRE(bytes.has_value());
                const auto& image = images->get_array()[index];
                CHECK(std::filesystem::path{image_paths[index]}.filename().string() == image.at("name").get_string());
                CHECK(bytes->size() == static_cast<std::size_t>(image.at("bytes").get_number()));
            }
            REQUIRE(consume(harness->close()).has_value());
            if (!directory.empty()) {
                std::error_code error;
                std::filesystem::remove_all(directory, error);
            }
        }
    }
}

TEST_CASE("Failed codemode output writes keep truthful labels", "[durable][codemode][issue1172]") {
    tests::TempWorkspace workspace;
    MemoryStorage storage;
    Registry registry;
    auto environment = std::make_shared<OutputSpyEnvironment>(local_environment(workspace.path().string()), true);
    install_codemode(registry, environment);
    const std::string source =
            "// @options: {\"max_output_tokens\": 20}\n"
            "text('x'.repeat(120)); image('data:image/png;base64,"
            "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+a8kUAAAAASUVORK5CYII=');";
    auto harness = open(storage, registry, {source});
    submit(*harness);
    const auto result = last_result(*harness);
    REQUIRE(consume(harness->close()).has_value());
    std::string text;
    for (const auto& block : result.at("content").get_array())
        if (block.at("type").get_string() == "text") text += block.at("text").get_string();
    CHECK_FALSE(result.at("isError").get_boolean());
    CHECK(text.find("[Could not save the full output: injected output failure]") != std::string::npos);
    CHECK(text.find("[Image (image/png, 68 bytes) could not be saved: injected output failure]") != std::string::npos);
    CHECK(text.find("[Full output:") == std::string::npos);
    CHECK(text.find("[Image saved to ") == std::string::npos);
    CHECK(field(result, "details") != nullptr);
    CHECK(field(*field(result, "details"), "fullOutputPath") == nullptr);
    bool kept_image = false;
    for (const auto& block : result.at("content").get_array())
        kept_image |= block.at("type").get_string() == "image";
    CHECK(kept_image);
    // One directory, one attempted spill and one attempted image: no retry and
    // no path that claims a file the environment refused.
    REQUIRE(environment->created_dirs.size() == 1);
    CHECK(environment->writes.size() == 2);
    CHECK_FALSE(std::filesystem::exists(std::filesystem::path{environment->created_dirs.front()} / "output.txt"));
    std::error_code error;
    std::filesystem::remove_all(environment->created_dirs.front(), error);
}

TEST_CASE("Cancelled codemode output publishes no file references", "[durable][codemode][issue1172]") {
    constexpr std::string_view kAbortArtifact = "fixtures/pi-v1.3.0/audit-1170/codemode-nested.json";
    tests::TempWorkspace workspace;
    MemoryStorage storage;
    Registry registry;
    auto environment = std::make_shared<OutputSpyEnvironment>(local_environment(workspace.path().string()), false);
    install_codemode(registry, environment);
    std::atomic<bool> first{true};
    std::binary_semaphore reached{0};
    Extension pause{.name = "pause"};
    pause.tools.push_back(paused_tool(first, reached));
    REQUIRE(registry.install(std::move(pause)).has_value());
    auto harness = open(storage,
            registry,
            {"// @options: {\"max_output_tokens\": 20}\ntext('x'.repeat(120)); await tools.pause();"});
    auto root = consume(harness->root());
    REQUIRE(root.has_value());
    auto input =
            consume(harness->submit(root.value(), SubmissionDraft{.body = InputSubmissionDraft{.content = "start"}}));
    REQUIRE(input.has_value());
    // Liveness backstop only: the shard's frozen-comparison case runs a real
    // WASM sandbox in parallel with this one, so sandbox start can take longer
    // than a second-scale prompt bound.
    REQUIRE(reached.try_acquire_for(std::chrono::seconds{30}));
    auto aborted = consume(harness->abort_conversation(root.value()));
    REQUIRE(aborted.has_value());
    const auto result = last_result(*harness);
    REQUIRE(consume(harness->close()).has_value());

    std::string text;
    for (const auto& block : result.at("content").get_array())
        if (block.at("type").get_string() == "text") text += block.at("text").get_string();

    // The aborted call reports the frozen pi harness result, and no spill or
    // image file is written at all, so no reference can be published.
    const auto frozen =
            FrozenScenario::load(kAbortArtifact, "cancelling the parent codemode call aborts its nested work");
    REQUIRE(frozen.has_value());
    const auto& expected = frozen->expected();
    CHECK(result.at("isError").get_boolean() == expected.at("isError").get_boolean());
    CHECK(text == expected.at("output").get_string());
    CHECK(environment->created_dirs.empty());
    CHECK(environment->writes.empty());
}

} // namespace cch::tests::durable
