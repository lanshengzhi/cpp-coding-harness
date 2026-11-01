#include <cch/codemode/Sandbox.hpp>
#include <cch/durable/Codemode.hpp>
#include <cch/durable/Harness.hpp>
#include <cch/durable/JsonlStorage.hpp>
#include <cch/durable/SqliteStorage.hpp>
#include <cch/durable/Storage.hpp>
#include "durable/Outcome.hpp"
#include "durable/ToolContent.hpp"
#include "durable/ToolTask.hpp"
#include "support/Json.hpp"
#include "support/TempWorkspace.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace cch::tests::durable {
using namespace cch::durable;

namespace {

using Json = support::JsonValue;
using Object = Json::object_t;
using Array = Json::array_t;

constexpr auto kWait = std::chrono::seconds{30};
constexpr std::string_view kParentKind{"audit1171.parent"};
constexpr std::string_view kSource{
        "const stamp = Date.now() + ':' + Math.random(); await tools.effect({stamp}); "
        "await tools.pauseA(); await tools.pauseB(); store('stamp', stamp); return 'done ' + stamp;"};

const Json* field(const Json& value, std::string_view key) {
    const auto* object = value.get_if<Json::object_t>();
    if (object == nullptr) return nullptr;
    const auto found = object->find(std::string{key});
    return found == object->end() ? nullptr : &found->second;
}

void append_line(const std::filesystem::path& path, std::string line) {
    std::ofstream output{path, std::ios::app};
    output << line << '\n';
}

std::vector<std::string> lines(const std::filesystem::path& path) {
    std::ifstream input{path};
    std::vector<std::string> result;
    std::string line;
    while (std::getline(input, line))
        if (!line.empty()) result.push_back(line);
    return result;
}

ToolRegistration tool_named(std::string name, std::string description) {
    return ToolRegistration{.declaration = ai::Tool{.name = std::move(name), .description = std::move(description)}};
}

ToolRegistration effect_tool(const std::filesystem::path& directory) {
    auto tool = tool_named("effect", "Record one effect");
    tool.replay_safe = false;
    tool.execute = [directory](
                           support::JsonValue args, std::stop_token) -> support::AsyncResult<std::vector<ai::Content>> {
        append_line(directory / "effects.txt", args.at("stamp").get_string());
        return support::AsyncResult<std::vector<ai::Content>>{
                support::Expected<std::vector<ai::Content>>{std::vector<ai::Content>{ai::text_content("effect done")}}};
    };
    return tool;
}

ToolRegistration pause_tool(const std::filesystem::path& directory, std::string name) {
    auto tool = tool_named(name, "Pause until the other runtime continues " + name);
    tool.replay_safe = true;
    tool.execute = [directory, name = std::move(name)](
                           support::JsonValue, std::stop_token stop) -> support::AsyncResult<std::vector<ai::Content>> {
        const auto marker = directory / (name + ".entered");
        std::error_code error;
        if (!std::filesystem::exists(marker, error)) {
            std::ofstream{marker} << "entered\n";
            while (!stop.stop_requested())
                std::this_thread::sleep_for(std::chrono::milliseconds{5});
            return support::AsyncResult<std::vector<ai::Content>>{
                    std::unexpected(support::make_error(support::ErrorCode::Cancelled, name + " interrupted"))};
        }
        return support::AsyncResult<std::vector<ai::Content>>{support::Expected<std::vector<ai::Content>>{
                std::vector<ai::Content>{ai::text_content(name + " resumed")}}};
    };
    return tool;
}

/// The parent task both runtimes register: a waiting task whose one child is a
/// nested `pi.tool` codemode call.
TaskDefinition parent_task() {
    TaskDefinitionBuilder builder{std::string{kParentKind}, 1};
    builder.initial([](const chord::Json&) { return chord::Json{chord::Json::Object{{"phase", "launch"}}}; });
    builder.phase("launch", [](const TaskRecord&, TaskRuntime& runtime) -> support::AsyncResult<void> {
        const auto conversation = runtime.conversation_id();
        auto* task_runtime = &runtime;
        return runtime.commit([conversation, task_runtime](Transaction& tx,
                                      const TaskRecord& task) -> support::Expected<std::optional<NextTaskState>> {
            const Json arguments{Object{{"code", std::string{kSource}}}};
            const Json call =
                    tool_call_json(ai::ToolCallContent{.id = "c1", .name = "codemode", .arguments = arguments},
                            chord::Json::from_support(arguments).value())
                            .to_support();
            Json message{Object{{"role", "assistant"},
                    {"content", Array{call}},
                    {"api", "audit1171"},
                    {"provider", "audit1171"},
                    {"model", "audit1171"},
                    {"stopReason", "toolUse"},
                    {"timestamp", Json{0.0}}}};
            auto entry = tx.append_entry(conversation,
                    "pi.assistant",
                    chord::Json::from_support(Json{Object{{"model", Array{message}},
                                                      {"byTaskId", Json{static_cast<double>(task.id.value)}}}})
                            .value(),
                    std::nullopt,
                    {chord::Json::from_support(message).value()},
                    task.id);
            if (!entry) return std::unexpected(entry.error());
            auto definition = task_runtime->task_definition(kToolTaskKind);
            if (definition == nullptr)
                return std::unexpected(
                        support::make_error(support::ErrorCode::Validation, "The nested tool task is not registered"));
            auto child = tx.create_task(*definition,
                    chord::Json::from_support(Json{Object{{"kind", "model"},
                                                      {"assistant", Json{static_cast<double>(entry->id.value)}},
                                                      {"callId", "c1"}}})
                            .value(),
                    TaskOptions{.owner = task.id});
            if (!child) return std::unexpected(child.error());
            return std::optional<NextTaskState>{
                    NextTaskState::waiting(chord::Json{chord::Json::Object{{"phase", "finish"},
                                                   {"child", chord::Json{static_cast<double>(child->value)}}}},
                            {*child},
                            JoinPolicy::AllSettled)};
        });
    });
    builder.phase("finish", [](const TaskRecord& task, TaskRuntime& runtime) -> support::AsyncResult<void> {
        (void)task;
        return runtime.commit([](Transaction& tx,
                                      const TaskRecord& current) -> support::Expected<std::optional<NextTaskState>> {
            const auto checkpoint =
                    current.state.checkpoint.has_value() ? current.state.checkpoint->to_support() : Json{nullptr};
            const auto* child = field(checkpoint, "child");
            if (child == nullptr || !child->holds<double>())
                return std::unexpected(
                        support::make_error(support::ErrorCode::Validation, "The parent checkpoint has no child task"));
            auto record = tx.task(TaskId{.value = static_cast<std::int64_t>(child->get_number())});
            if (!record) return std::unexpected(record.error());
            if (!record->has_value() || !(*record)->state.outcome.has_value() ||
                    !(*record)->state.outcome->result.has_value())
                return std::optional<NextTaskState>{NextTaskState::terminal(TaskOutcome{.status = "failed",
                        .error = TaskOutcomeError{.message = "The nested codemode call has no result"}})};
            const auto outcome = (*record)->state.outcome->result->to_support();
            const auto* entry_value = field(outcome, "entryId");
            if (entry_value == nullptr)
                return std::optional<NextTaskState>{NextTaskState::terminal(TaskOutcome{.status = "failed",
                        .error = TaskOutcomeError{.message = "The nested tool task has no result entry"}})};
            auto entry = tx.entry(EntryId{.value = static_cast<std::int64_t>(entry_value->get_number())});
            if (!entry) return std::unexpected(entry.error());
            if (!entry->has_value())
                return std::unexpected(
                        support::make_error(support::ErrorCode::Validation, "The nested tool result entry is missing"));
            const auto data = (*entry)->data.to_support();
            const auto* model = field(data, "model");
            if (model == nullptr || model->get_array().empty())
                return std::unexpected(
                        support::make_error(support::ErrorCode::Validation, "The nested tool result has no message"));
            const auto& message = model->get_array().front();
            Object result;
            if (const auto* content = field(message, "content")) result.emplace("structuredOutput", *content);
            if (const auto* is_error = field(message, "isError")) result.emplace("isError", *is_error);
            auto converted = chord::Json::from_support(Json{std::move(result)});
            if (!converted) return std::unexpected(converted.error());
            return std::optional<NextTaskState>{
                    NextTaskState::terminal(TaskOutcome{.status = "completed", .result = std::move(*converted)})};
        });
    });
    auto built = builder.build();
    if (!built) std::terminate(); // fixed test definition
    return std::move(*built);
}

std::unique_ptr<Storage> open_storage(std::string_view backend, const std::filesystem::path& directory) {
    if (backend == "sqlite") {
        auto opened = SqliteStorage::open(directory / "store.sqlite");
        if (!opened.has_value()) INFO(opened.error().message << ": " << opened.error().detail);
        REQUIRE(opened.has_value());
        return std::move(*opened);
    }
    auto opened = JsonlStorage::open(directory);
    if (!opened.has_value()) INFO(opened.error().message << ": " << opened.error().detail);
    REQUIRE(opened.has_value());
    return std::move(*opened);
}

struct Opened {
    Registry registry;
    std::unique_ptr<Storage> storage;
    std::unique_ptr<Harness> harness;
    ConversationId conversation{};
};

Opened open_harness(std::string_view backend, const std::filesystem::path& directory) {
    Opened opened;
    opened.storage = open_storage(backend, directory);
    Extension tools{.name = "audit1171-tools"};
    tools.tools.push_back(effect_tool(directory));
    tools.tools.push_back(pause_tool(directory, "pauseA"));
    tools.tools.push_back(pause_tool(directory, "pauseB"));
    REQUIRE(opened.registry.install(std::move(tools)).has_value());
    auto sandbox = codemode::CodemodeSandbox::create(codemode::default_codemode_guest_wasm_path());
    REQUIRE(sandbox.has_value());
    REQUIRE(opened.registry.install(codemode_extension(std::move(*sandbox))).has_value());
    std::vector<TaskDefinition> tasks;
    tasks.push_back(parent_task());
    auto harness = consume(
            Harness::open(*opened.storage, HarnessOptions{.tasks = std::move(tasks), .registry = &opened.registry}));
    if (!harness.has_value()) INFO(harness.error().message << ": " << harness.error().detail);
    REQUIRE(harness.has_value());
    opened.harness = std::move(harness.value());
    auto root = consume(opened.harness->root());
    REQUIRE(root.has_value());
    opened.conversation = root.value();
    return opened;
}

TaskId create_parent(Harness& harness, ConversationId conversation) {
    TaskId id{};
    auto committed = consume(harness.session().commit([&](Transaction& tx) -> support::ExpectedVoid {
        auto created = tx.create_task(*harness.task_definition(kParentKind),
                chord::Json{chord::Json::Object{}},
                TaskOptions{.conversation_id = conversation});
        if (!created) return std::unexpected(created.error());
        id = *created;
        return {};
    }));
    REQUIRE(committed.has_value());
    return id;
}

bool wait_for_file(const std::filesystem::path& path, std::chrono::milliseconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        std::error_code error;
        if (std::filesystem::exists(path, error)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    return false;
}

/// The frozen pi child. It joins its own process group so one kill reaches the
/// tsx wrapper and the driver.
class PiProcess {
public:
    PiProcess(const std::filesystem::path& directory, std::string_view backend, std::string_view mode) {
        const char* checkout = std::getenv("PI_CHECKOUT");
        if (checkout == nullptr || *checkout == '\0') return;
        const std::filesystem::path root{checkout};
        const std::string tsx = (root / "node_modules/.bin/tsx").string();
        const std::string driver = std::string{CCH_SOURCE_DIR} + "/docs/research/pi-v1.3.0/continue-1171-codemode.mts";
        pid_ = ::fork();
        REQUIRE(pid_ >= 0);
        if (pid_ == 0) {
            (void)::setpgid(0, 0);
            ::setenv("PI_CHECKOUT", checkout, 1);
            ::setenv("TSX_TSCONFIG_PATH", (root / "tsconfig.json").c_str(), 1);
            ::setenv("NODE_OPTIONS", "--conditions=source", 1);
            ::execl(tsx.c_str(),
                    tsx.c_str(),
                    driver.c_str(),
                    directory.c_str(),
                    std::string{backend}.c_str(),
                    std::string{mode}.c_str(),
                    nullptr);
            _exit(127);
        }
        (void)::setpgid(pid_, pid_);
        directory_ = directory;
    }
    PiProcess(const PiProcess&) = delete;
    PiProcess& operator=(const PiProcess&) = delete;
    ~PiProcess() {
        if (pid_ > 0) {
            (void)::kill(-pid_, SIGKILL);
            int status{};
            while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
            }
        }
    }

    [[nodiscard]] bool started() const noexcept { return pid_ > 0; }

    [[nodiscard]] bool wait_for(const std::filesystem::path& marker, std::chrono::milliseconds limit = kWait) {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        while (std::chrono::steady_clock::now() < deadline) {
            int status{};
            const auto waited = ::waitpid(pid_, &status, WNOHANG);
            if (waited == pid_) {
                pid_ = -1;
                return false;
            }
            std::error_code error;
            if (std::filesystem::exists(directory_ / marker, error)) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
        return false;
    }

    void kill() {
        if (pid_ <= 0) return;
        (void)::kill(-pid_, SIGKILL);
        int status{};
        while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
        }
        pid_ = -1;
        REQUIRE(WIFSIGNALED(status));
        CHECK(WTERMSIG(status) == SIGKILL);
    }

    [[nodiscard]] bool wait_exit(std::chrono::milliseconds limit = kWait) {
        if (pid_ <= 0) return false;
        const auto deadline = std::chrono::steady_clock::now() + limit;
        while (std::chrono::steady_clock::now() < deadline) {
            int status{};
            const auto waited = ::waitpid(pid_, &status, WNOHANG);
            if (waited == pid_) {
                pid_ = -1;
                REQUIRE(WIFEXITED(status));
                CHECK(WEXITSTATUS(status) == 0);
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
        return false;
    }

private:
    pid_t pid_{-1};
    std::filesystem::path directory_{};
};

class CppChild {
public:
    CppChild(const std::filesystem::path& directory, std::string_view backend, std::string_view mode) {
        pid_ = ::fork();
        REQUIRE(pid_ >= 0);
        if (pid_ == 0) {
            (void)::setpgid(0, 0);
            ::setenv("CCH_1171_DIR", directory.c_str(), 1);
            ::setenv("CCH_1171_BACKEND", std::string{backend}.c_str(), 1);
            ::setenv("CCH_1171_MODE", std::string{mode}.c_str(), 1);
            ::execl("/proc/self/exe", "cch_tests_durable", "[.][issue1171child]", nullptr);
            _exit(127);
        }
        (void)::setpgid(pid_, pid_);
        directory_ = directory;
    }
    ~CppChild() {
        if (pid_ > 0) {
            (void)::kill(-pid_, SIGKILL);
            int status{};
            while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
            }
        }
    }
    [[nodiscard]] bool wait_for(const std::filesystem::path& marker, std::chrono::milliseconds limit = kWait) {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        while (std::chrono::steady_clock::now() < deadline) {
            int status{};
            if (::waitpid(pid_, &status, WNOHANG) == pid_) {
                pid_ = -1;
                return false;
            }
            std::error_code error;
            if (std::filesystem::exists(directory_ / marker, error)) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
        return false;
    }
    void kill() {
        if (pid_ <= 0) return;
        (void)::kill(-pid_, SIGKILL);
        int status{};
        while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
        }
        pid_ = -1;
        REQUIRE(WIFSIGNALED(status));
        CHECK(WTERMSIG(status) == SIGKILL);
    }
    [[nodiscard]] bool wait_exit(std::chrono::milliseconds limit = kWait) {
        if (pid_ <= 0) return false;
        const auto deadline = std::chrono::steady_clock::now() + limit;
        while (std::chrono::steady_clock::now() < deadline) {
            int status{};
            if (::waitpid(pid_, &status, WNOHANG) == pid_) {
                pid_ = -1;
                REQUIRE(WIFEXITED(status));
                CHECK(WEXITSTATUS(status) == 0);
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
        return false;
    }

private:
    pid_t pid_{-1};
    std::filesystem::path directory_{};
};

std::optional<Json> document_value(Storage& storage, std::string_view kind, DocumentAddress address) {
    auto record = storage.find_document(std::string{kind}, address, DocumentPoint::current());
    if (!record || !record->has_value()) return std::nullopt;
    auto value = storage.document((*record)->id, DocumentPoint::current());
    if (!value || !value->has_value()) return std::nullopt;
    return (*value)->value.to_support();
}

/// A stable view of one interrupted run: the frozen start identity and the
/// issued frontier, not a C++-specific state wrapper.
Json checkpoint(Storage& storage, TaskId parent, std::size_t expected_entries) {
    auto parent_record = storage.task(parent);
    REQUIRE(parent_record.has_value());
    REQUIRE(parent_record->has_value());
    REQUIRE((*parent_record)->state.checkpoint.has_value());
    const auto parent_state = (*parent_record)->state.checkpoint->to_support();
    const auto* child = field(parent_state, "child");
    REQUIRE(child != nullptr);
    const TaskId codemode{.value = static_cast<std::int64_t>(child->get_number())};
    auto run = document_value(storage, "pi.codemode.run", DocumentAddress{.task_id = codemode});
    auto journal = document_value(storage, "pi.codemode.journal", DocumentAddress{.task_id = codemode});
    REQUIRE(run.has_value());
    REQUIRE(journal.has_value());
    const auto* start = field(*run, "start");
    const auto* entries = field(*journal, "entries");
    REQUIRE(start != nullptr);
    REQUIRE(entries != nullptr);
    REQUIRE(entries->get_array().size() >= expected_entries);
    Array ids;
    std::set<std::int64_t> unique;
    for (const auto& entry : entries->get_array()) {
        const auto id = static_cast<std::int64_t>(entry.at("id").get_number());
        ids.push_back(Json{static_cast<double>(id)});
        unique.insert(id);
        CHECK(entry.at("at").get_number() > 0);
    }
    CHECK(unique.size() == entries->get_array().size());
    const auto* first = field(*journal, "firstIssued");
    CHECK(first != nullptr);
    return Json{Object{{"version", start->at("version")},
            {"seedIsNumber", start->at("seed").holds<double>()},
            {"startedAt", start->at("startedAt")},
            {"fingerprintIsString",
                    field(*start, "fingerprint") != nullptr && field(*start, "fingerprint")->holds<std::string>()},
            {"entryIds", Json{std::move(ids)}},
            {"hasFirstIssued", first != nullptr},
            {"codemodeTask", Json{static_cast<double>(codemode.value)}}}};
}

/// Rewrite the committed issued frontier: a continuation that silently ignored
/// a recorded count would repeat or reorder the script's calls.
void tamper_frontier(const std::filesystem::path& directory, std::string_view backend, TaskId parent) {
    auto opened = open_harness(backend, directory);
    auto record = consume(opened.harness->get_task(parent));
    REQUIRE(record.has_value());
    REQUIRE(record.value().has_value());
    REQUIRE(record.value()->state.checkpoint.has_value());
    const auto state = record.value()->state.checkpoint->to_support();
    const auto* child = field(state, "child");
    REQUIRE(child != nullptr);
    const TaskId codemode{.value = static_cast<std::int64_t>(child->get_number())};
    auto journal = define_document(DocumentDefinitionBuilder{"pi.codemode.journal"}.task(),
            [] { return chord::Json{chord::Json::Object{{"entries", chord::Json{chord::Json::Array{}}}}}; });
    REQUIRE(journal.has_value());
    auto committed = consume(opened.harness->session().commit([&](Transaction& tx) -> support::ExpectedVoid {
        auto document = tx.doc(*journal, DocumentAddress{.task_id = codemode});
        if (!document) return std::unexpected(document.error());
        auto value = document->get({});
        if (!value || !value->has_value())
            return std::unexpected(
                    support::make_error(support::ErrorCode::Validation, "The journal could not be read"));
        auto entries = (**value).to_support().at("entries").get_array();
        REQUIRE_FALSE(entries.empty());
        entries.front().get_object().insert_or_assign("issuedAfter", Json{999.0});
        auto stored = chord::Json::from_support(Json{std::move(entries)});
        if (!stored) return std::unexpected(stored.error());
        if (auto written = document->set({chord::path_key("entries")}, *stored); !written)
            return std::unexpected(written.error());
        return {};
    }));
    REQUIRE(committed.has_value());
    REQUIRE(consume(opened.harness->close()).has_value());
}

void verify_result(const std::filesystem::path& directory, std::string_view file = "pi-result.json") {
    const auto effects = lines(directory / "effects.txt");
    REQUIRE(effects.size() == 1);
    const auto stamp = effects.front();
    auto result = support::read_json([&] {
        std::ifstream input{directory / std::string{file}};
        return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    }());
    REQUIRE(result.has_value());
    CHECK(result->at("outcome").get_string() == "completed");
    CHECK_FALSE(result->at("isError").get_boolean());
    const auto* store = field(*result, "store");
    REQUIRE(store != nullptr);
    REQUIRE(field(*store, "stamp") != nullptr);
    CHECK(field(*store, "stamp")->get_string() == stamp);
    bool output_has_stamp = false;
    const auto* output = field(*result, "structuredOutput");
    REQUIRE(output != nullptr);
    for (const auto& block : output->get_array()) {
        if (field(block, "type") != nullptr && block.at("type").get_string() == "text" &&
                block.at("text").get_string().ends_with(stamp))
            output_has_stamp = true;
    }
    CHECK(output_has_stamp);
    CHECK(result->at("effects").get_array().size() == 1);
    CHECK(result->at("effects").get_array().front().get_string() == stamp);
}

std::optional<TaskRecord> wait_terminal(Harness& harness, TaskId id, std::chrono::milliseconds limit = kWait) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        auto record = consume(harness.get_task(id));
        if (record.has_value() && record.value().has_value() && record.value()->state.status == "terminal")
            return record.value();
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    return std::nullopt;
}

Json result_view(Harness& harness, TaskId parent, ConversationId conversation, const std::filesystem::path& directory) {
    auto record = wait_terminal(harness, parent);
    if (!record.has_value() || !record->state.outcome.has_value()) return Json{nullptr};
    Object view{{"outcome", record->state.outcome->status}};
    if (record->state.outcome->result.has_value()) {
        const auto result = record->state.outcome->result->to_support();
        if (const auto* structured = field(result, "structuredOutput")) view.emplace("structuredOutput", *structured);
        if (const auto* is_error = field(result, "isError")) view.emplace("isError", *is_error);
    }
    auto store = consume(
            harness.session().snapshot(codemode_store_document(), DocumentAddress{.conversation_id = conversation}));
    if (store.has_value() && store.value().has_value()) view.emplace("store", store.value()->to_support().at("values"));
    Array effects;
    for (const auto& line : lines(directory / "effects.txt"))
        effects.push_back(Json{line});
    view.emplace("effects", Json{std::move(effects)});
    return Json{std::move(view)};
}

void write_result(const std::filesystem::path& directory, Json result) {
    auto encoded = support::write_json(result);
    REQUIRE(encoded.has_value());
    std::ofstream output{directory / "cpp-result.json"};
    output << *encoded;
}

/// The C++ side of a shared run, spawned as a child so the controlling test can
/// SIGKILL it between committed boundaries exactly as it kills frozen pi.
void run_cpp_child() {
    const char* directory = std::getenv("CCH_1171_DIR");
    const char* backend = std::getenv("CCH_1171_BACKEND");
    const char* mode = std::getenv("CCH_1171_MODE");
    if (directory == nullptr || backend == nullptr || mode == nullptr) _exit(90);
    if (std::string_view{mode} == "create") {
        auto opened = open_harness(backend, directory);
        auto parent = create_parent(*opened.harness, opened.conversation);
        auto encoded = support::write_json(Json{Object{{"parent", Json{static_cast<double>(parent.value)}},
                {"conversation", Json{static_cast<double>(opened.conversation.value)}}}});
        REQUIRE(encoded.has_value());
        std::ofstream output{std::filesystem::path{directory} / "task.json"};
        output << *encoded;
        output.close();
        REQUIRE(opened.harness->resume().has_value());
        for (;;)
            ::pause();
    }
    auto opened = open_harness(backend, directory);
    auto task_json = support::read_json([&] {
        std::ifstream input{std::filesystem::path{directory} / "task.json"};
        return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    }());
    REQUIRE(task_json.has_value());
    const TaskId parent{.value = static_cast<std::int64_t>(task_json->at("parent").get_number())};
    const ConversationId conversation{.value = static_cast<std::int64_t>(task_json->at("conversation").get_number())};
    REQUIRE(opened.harness->resume().has_value());
    if (std::string_view{mode} == "finish") {
        write_result(directory, result_view(*opened.harness, parent, conversation, directory));
        REQUIRE(consume(opened.harness->close()).has_value());
        _exit(0);
    }
    for (;;)
        ::pause();
}

} // namespace

TEST_CASE(
        "pi and C++ continue each other's interrupted codemode run", "[durable][codemode][issue1171][issue1171live]") {
    const char* checkout = std::getenv("PI_CHECKOUT");
    if (checkout == nullptr || *checkout == '\0') SKIP("PI_CHECKOUT is required for frozen pi continuation");
    const bool sqlite = GENERATE(true, false);
    const std::string backend = sqlite ? "sqlite" : "jsonl";
    CAPTURE(backend);

    tests::TempWorkspace workspace;
    const auto directory = workspace.path();
    workspace.write("source.js", std::string{kSource});

    {
        PiProcess pi{directory, backend, "create"};
        REQUIRE(pi.started());
        REQUIRE(pi.wait_for("pauseA.entered"));
        pi.kill();
    }
    auto task_json = support::read_json(workspace.read("task.json"));
    REQUIRE(task_json.has_value());
    const TaskId parent{.value = static_cast<std::int64_t>(task_json->at("parent").get_number())};

    {
        auto storage = open_storage(backend, directory);
        const auto before = checkpoint(*storage, parent, 1);
        CHECK(before.at("version").get_number() == 2);
        CHECK(before.at("seedIsNumber").get_boolean());
        CHECK(before.at("fingerprintIsString").get_boolean());
        CHECK(before.at("hasFirstIssued").get_boolean());
        CHECK(before.at("entryIds").get_array().size() == 1);
        CHECK(before.at("entryIds").get_array().front().get_number() == 1);
    }

    {
        CppChild cpp{directory, backend, "continue"};
        REQUIRE(cpp.wait_for("pauseB.entered"));
        cpp.kill();
    }
    {
        auto storage = open_storage(backend, directory);
        const auto between = checkpoint(*storage, parent, 2);
        CHECK(between.at("entryIds").get_array().size() == 2);
        CHECK(between.at("entryIds").get_array().at(1).get_number() == 2);
    }
    {
        PiProcess pi{directory, backend, "finish"};
        REQUIRE(pi.started());
        REQUIRE(wait_for_file(directory / "pi-result.json", kWait));
        REQUIRE(pi.wait_exit());
    }
    verify_result(directory);
    {
        CppChild cpp{directory, backend, "finish"};
        REQUIRE(wait_for_file(directory / "cpp-result.json", kWait));
        REQUIRE(cpp.wait_exit());
    }
    verify_result(directory, "cpp-result.json");
}

TEST_CASE("C++ creates an interrupted codemode run pi continues", "[durable][codemode][issue1171][issue1171live]") {
    const char* checkout = std::getenv("PI_CHECKOUT");
    if (checkout == nullptr || *checkout == '\0') SKIP("PI_CHECKOUT is required for frozen pi continuation");
    const bool sqlite = GENERATE(true, false);
    const std::string backend = sqlite ? "sqlite" : "jsonl";
    CAPTURE(backend);

    tests::TempWorkspace workspace;
    const auto directory = workspace.path();
    workspace.write("source.js", std::string{kSource});

    {
        CppChild cpp{directory, backend, "create"};
        REQUIRE(cpp.wait_for("pauseA.entered"));
        cpp.kill();
    }
    auto task_json = support::read_json(workspace.read("task.json"));
    REQUIRE(task_json.has_value());
    const TaskId parent{.value = static_cast<std::int64_t>(task_json->at("parent").get_number())};

    {
        auto storage = open_storage(backend, directory);
        const auto before = checkpoint(*storage, parent, 1);
        CHECK(before.at("version").get_number() == 2);
        CHECK(before.at("entryIds").get_array().size() == 1);
    }

    {
        PiProcess pi{directory, backend, "continue"};
        REQUIRE(pi.started());
        REQUIRE(pi.wait_for("pauseB.entered"));
        pi.kill();
    }

    {
        CppChild cpp{directory, backend, "finish"};
        REQUIRE(wait_for_file(directory / "cpp-result.json", kWait));
        REQUIRE(cpp.wait_exit());
    }
    verify_result(directory, "cpp-result.json");
    {
        PiProcess pi{directory, backend, "finish"};
        REQUIRE(pi.started());
        REQUIRE(wait_for_file(directory / "pi-result.json", kWait));
        REQUIRE(pi.wait_exit());
    }
    verify_result(directory);
}

TEST_CASE("a divergent codemode frontier fails explicitly", "[durable][codemode][issue1171][issue1171live]") {
    const char* checkout = std::getenv("PI_CHECKOUT");
    if (checkout == nullptr || *checkout == '\0') SKIP("PI_CHECKOUT is required for frozen pi continuation");
    const bool sqlite = GENERATE(true, false);
    const std::string backend = sqlite ? "sqlite" : "jsonl";
    CAPTURE(backend);

    tests::TempWorkspace workspace;
    const auto directory = workspace.path();
    workspace.write("source.js", std::string{kSource});
    {
        CppChild cpp{directory, backend, "create"};
        REQUIRE(cpp.wait_for("pauseA.entered"));
        cpp.kill();
    }
    auto task_json = support::read_json(workspace.read("task.json"));
    REQUIRE(task_json.has_value());
    const TaskId parent{.value = static_cast<std::int64_t>(task_json->at("parent").get_number())};
    tamper_frontier(directory, backend, parent);
    {
        CppChild cpp{directory, backend, "finish"};
        REQUIRE(wait_for_file(directory / "cpp-result.json", kWait));
        REQUIRE(cpp.wait_exit());
    }
    auto result = support::read_json(workspace.read("cpp-result.json"));
    REQUIRE(result.has_value());
    CHECK(result->at("outcome").get_string() == "completed");
    CHECK(result->at("isError").get_boolean());
    std::string text;
    for (const auto& block : result->at("structuredOutput").get_array())
        if (block.at("type").get_string() == "text") text += block.at("text").get_string();
    CHECK(text.find("different path") != std::string::npos);
    // The completed effect is reattached, never repeated.
    CHECK(lines(directory / "effects.txt").size() == 1);
}

TEST_CASE("codemode continuation child", "[.][issue1171child]") { run_cpp_child(); }

} // namespace cch::tests::durable
