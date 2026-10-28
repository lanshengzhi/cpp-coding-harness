// Process-crash comparison with audit-1055: the external effect is replayed,
// while its stable key makes the application itself idempotent.
#include <cch/ai/Content.hpp>
#include <cch/ai/Models.hpp>
#include <cch/durable/Harness.hpp>
#include <cch/durable/SqliteStorage.hpp>
#include <cch/durable/Task.hpp>

#include "durable/FrozenExpectation.hpp"
#include "durable/Outcome.hpp"
#include "support/TempWorkspace.hpp"
#include "support/UniqueFd.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <poll.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace cch;
using namespace cch::durable;
using namespace cch::tests::durable;

namespace {

pid_t fork_child() {
    const pid_t parent = ::getpid();
    const pid_t child = ::fork();
    if (child == 0 && (::prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 || ::getppid() != parent)) _exit(90);
    return child;
}

TaskDefinition effect_task(const std::filesystem::path& directory, int ready_fd, bool block) {
    TaskDefinitionBuilder builder("audit1055.effect");
    builder.initial([](const chord::Json&) {
        return chord::Json(chord::Json::Object{{"phase", chord::Json("apply")}, {"key", chord::Json("transfer-7")}});
    });
    builder.phase("apply", [directory, ready_fd, block](const TaskRecord&, TaskRuntime& runtime) {
        {
            std::ofstream calls(directory / "calls", std::ios::app);
            calls << "call\n";
        }
        std::error_code error;
        if (!std::filesystem::exists(directory / "applied", error)) {
            if (error) _exit(10);
            std::ofstream applied(directory / "applied", std::ios::app);
            applied << "transfer-7\n";
        }
        if (block) {
            const char ready = 'R';
            if (::write(ready_fd, &ready, 1) != 1) _exit(11);
            for (;;)
                ::pause();
        }
        return runtime.commit([](Transaction&, const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
            return NextTaskState::terminal(TaskOutcome{
                    .status = "completed", .result = chord::Json(chord::Json::Object{{"receipt", chord::Json(70.0)}})});
        });
    });
    return std::move(*builder.build());
}

int run_effect(const std::filesystem::path& directory, int ready_fd, bool first, bool third = false) {
    const auto frozen = FrozenScenario::load("fixtures/pi-v1.3.0/audit-1055/crash-recovery.json",
            "a killed process resumes an intent/effect/outcome task from its checkpoint and applies the effect once");
    if (!frozen) return 21;
    const auto& expected = frozen->expected();
    auto storage = SqliteStorage::open(directory / "store.sqlite");
    if (!storage) return 12;
    std::vector<TaskDefinition> definitions;
    definitions.push_back(effect_task(directory, ready_fd, first));
    auto opened = consume(Harness::open(**storage, HarnessOptions{.tasks = std::move(definitions)}));
    if (!opened.has_value()) return 13;
    auto harness = std::move(opened.value());
    TaskId id{};
    if (first) {
        auto committed = consume(harness->session().commit([&](Transaction& tx) -> support::ExpectedVoid {
            auto created = tx.create_task(*harness->task_definition("audit1055.effect"), chord::Json(nullptr));
            if (!created) return std::unexpected(created.error());
            id = *created;
            return {};
        }));
        if (!committed.has_value()) return 14;
    } else {
        auto tasks = (*storage)->scan_tasks(100);
        if (!tasks || tasks->size() != 1) return 15;
        id = tasks->front().id;
        if (!consume(harness->open_tasks()).has_value()) return 20;
        auto record = consume(harness->get_task(id));
        if (!record.has_value() || !record.value() ||
                record.value()->state.status != (third ? expected.at("terminalAfterThird").get_string()
                                                       : expected.at("afterOpen").at("status").get_string()))
            return 16;
        if (!third &&
                (!record.value()->state.checkpoint ||
                        !json_equals(*record.value()->state.checkpoint, expected.at("afterOpen").at("checkpoint")) ||
                        record.value()->started_at.has_value() !=
                                expected.at("afterOpen").at("startedAtPresent").get_boolean()))
            return 22;
    }
    if (!harness->resume()) return 17;
    auto outcome = consume(harness->wait_for_task(id));
    if (!outcome.has_value() || outcome.value().state.status != expected.at("terminalAfterThird").get_string() ||
            !outcome.value().state.outcome ||
            outcome.value().state.outcome->status != expected.at("outcome").at("status").get_string() ||
            !outcome.value().state.outcome->result ||
            !json_equals(*outcome.value().state.outcome->result, expected.at("outcome").at("result")))
        return 18;
    if (!consume(harness->close()).has_value()) return 19;
    return 0;
}

void append_log(const std::filesystem::path& path, std::string_view text) {
    std::ofstream out(path, std::ios::app);
    out << text << '\n';
}

void signal_boundary(int fd) {
    const char marker = 'R';
    if (::write(fd, &marker, 1) != 1) _exit(31);
    for (;;)
        ::pause();
}

TaskDefinition lifecycle_task(const std::filesystem::path& directory, int ready_fd, bool abort_case, bool first) {
    TaskDefinitionBuilder builder("audit1055.lifecycle");
    builder.initial([](const chord::Json&) { return chord::Json(chord::Json::Object{{"phase", "run"}}); });
    builder.phase(
            "run", [directory, abort_case](const TaskRecord&, TaskRuntime& runtime) -> support::AsyncResult<void> {
                append_log(directory / "events", "run");
                if (abort_case) {
                    std::mutex mutex;
                    std::condition_variable cv;
                    std::stop_callback wake(runtime.stop_token(), [&mutex, &cv] {
                        std::lock_guard lock(mutex);
                        cv.notify_all();
                    });
                    std::unique_lock lock(mutex);
                    if (!cv.wait_for(lock, std::chrono::seconds(5), [&runtime] {
                            return runtime.stop_token().stop_requested();
                        }))
                        _exit(91);
                    return support::AsyncResult<void>{support::ExpectedVoid{}};
                }
                return runtime.commit([](Transaction&,
                                              const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
                    return NextTaskState::terminal(TaskOutcome{.status = "completed", .result = chord::Json("once")});
                });
            });
    builder.abort([directory, ready_fd, first](const TaskRecord&, TaskRuntime& runtime) -> support::AsyncResult<void> {
        append_log(directory / "events", "abort");
        if (first) signal_boundary(ready_fd);
        return runtime.commit([](Transaction&, const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
            return NextTaskState::terminal(TaskOutcome{.status = "aborted", .reason = "recovered"});
        });
    });
    return std::move(*builder.build());
}

int run_lifecycle(const std::filesystem::path& directory, int ready_fd, bool first, bool abort_case) {
    const auto frozen = FrozenScenario::load("fixtures/pi-v1.3.0/audit-1055/crash-recovery.json",
            abort_case
                    ? "an abort mark survives a killed process and only the abort handler runs after reopen"
                    : "a killed process after a terminal commit does not rerun the task or duplicate the submission");
    if (!frozen) return 30;
    const auto& expected = frozen->expected();
    auto storage = SqliteStorage::open(directory / "store.sqlite");
    if (!storage) return 32;
    std::vector<TaskDefinition> definitions;
    definitions.push_back(lifecycle_task(directory, ready_fd, abort_case, first));
    auto opened = consume(Harness::open(**storage, HarnessOptions{.tasks = std::move(definitions)}));
    if (!opened.has_value()) return 33;
    auto harness = std::move(opened.value());
    TaskId id{};
    if (first) {
        auto committed = consume(harness->session().commit([&](Transaction& tx) -> support::ExpectedVoid {
            auto created = tx.create_task(*harness->task_definition("audit1055.lifecycle"), chord::Json(nullptr));
            if (!created) return std::unexpected(created.error());
            id = *created;
            return {};
        }));
        if (!committed.has_value()) return 34;
    } else {
        auto tasks = (*storage)->scan_tasks(100);
        if (!tasks || tasks->size() != 1) return 35;
        id = tasks->front().id;
        if (!consume(harness->open_tasks()).has_value()) return 36;
        auto record = consume(harness->get_task(id));
        if (!record.has_value() || !record.value()) return 37;
        if (abort_case &&
                (record.value()->state.status != expected.at("afterOpen").at("status").get_string() ||
                        record.value()->abort_requested !=
                                expected.at("afterOpen").at("abortRequested").get_boolean() ||
                        !record.value()->state.checkpoint ||
                        !json_equals(*record.value()->state.checkpoint, expected.at("afterOpen").at("checkpoint"))))
            return 38;
        if (!abort_case && record.value()->state.status != expected.at("taskStatus").get_string()) return 39;
    }
    if (!abort_case) {
        auto root = consume(harness->root());
        if (!root.has_value()) return 47;
        SubmissionDraft draft{.request_id = "terminal-request",
                .body = WriteSubmissionDraft{.entry = WriteEntry{.kind = "test.terminal"}}};
        auto submission = consume(harness->submit(root.value(), std::move(draft)));
        if (!submission.has_value()) return 48;
        append_log(directory / "terminal-ids", std::to_string(submission.value().id.value));
        auto records = (*storage)->scan_submissions(100);
        if (!records || records->size() != 1 || records->front().id != submission.value().id ||
                records->front().status != expected.at("submissionStatus").get_string())
            return 49;
    }
    if (!harness->resume()) return 40;
    if (first && abort_case) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        std::error_code error;
        while (!std::filesystem::exists(directory / "events", error) && !error &&
                std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (error) return 50;
        const bool reached = std::filesystem::exists(directory / "events", error);
        if (error || !reached) return 50;
        if (!consume(harness->abort_task(id)).has_value()) return 41;
    }
    auto outcome = consume(harness->wait_for_task(id));
    if (!outcome.has_value() || !outcome.value().state.outcome) return 42;
    const auto& receipt = *outcome.value().state.outcome;
    const auto& frozen_outcome = expected.at("outcome");
    if (receipt.status != frozen_outcome.at("status").get_string()) return 43;
    if (abort_case && (!receipt.reason || *receipt.reason != frozen_outcome.at("reason").get_string())) return 44;
    if (!abort_case && (!receipt.result || !json_equals(*receipt.result, frozen_outcome.at("result")))) return 45;
    if (first) signal_boundary(ready_fd);
    if (!consume(harness->close()).has_value()) return 46;
    return 0;
}

SubmissionDraft crash_input() {
    return SubmissionDraft{
            .request_id = "crash-1", .body = InputSubmissionDraft{.content = chord::Json("crash input")}};
}

int run_generation(const std::filesystem::path& directory, int ready_fd, bool first, bool third = false) {
    const auto frozen = FrozenScenario::load("fixtures/pi-v1.3.0/audit-1055/crash-recovery.json",
            "a killed process leaves an admitted input and its generation task recoverable without a duplicate "
            "submission");
    if (!frozen) return 65;
    const auto& expected = frozen->expected();
    auto storage = SqliteStorage::open(directory / "store.sqlite");
    if (!storage) return 51;
    HarnessOptions options;
    options.stream = [directory, ready_fd, first](ai::Model, ai::AiContext, ai::SimpleStreamOptions) {
        return ai::ModelStream{ai::ModelStreamProducer{
                [directory, ready_fd, first](ai::AssistantEventSink sink, ai::ModelStreamCompletion done) noexcept {
                    append_log(directory / "requests", "request");
                    if (first) signal_boundary(ready_fd);
                    auto answer = ai::assistant_text_message("after reopen");
                    auto sent = sink(ai::AssistantDoneEvent{.reason = answer.stop_reason, .message = answer});
                    if (!sent)
                        done(std::unexpected(sent.error()));
                    else
                        done(std::move(answer));
                }}};
    };
    auto opened = consume(Harness::open(**storage, std::move(options)));
    if (!opened.has_value()) return 52;
    auto harness = std::move(opened.value());
    auto root = consume(harness->root(RootOptions{.model = ai::Model{.id = "model", .provider = "test"}}));
    if (!root.has_value()) return 53;
    if (!first && !consume(harness->open_tasks()).has_value()) return 54;
    if (!first && !third) {
        auto inspection = consume(harness->inspect());
        if (!inspection.has_value() || inspection.value().scheduling != expected.at("schedulingAfterOpen").get_string())
            return 68;
        const auto& frozen_task = expected.at("taskAfterOpen").get_array().front();
        if (inspection.value().tasks.size() != 1) return 69;
        if (inspection.value().tasks.front().state.kind != frozen_task.at("inspection").get_string()) return 70;
    }
    auto submission = consume(harness->submit(root.value(), crash_input()));
    if (!submission.has_value()) return 55;
    append_log(directory / "generation-ids", std::to_string(submission.value().id.value));
    if (first) {
        if (!harness->resume()) return 56;
        // The request handler signals after admission and before model completion.
        auto settled = consume(submission.value().wait());
        (void)settled;
        return 57;
    }
    auto all = (*storage)->scan_submissions(100);
    if (!all || all->size() != 1 || all->front().id != submission.value().id ||
            all->front().status != (third ? expected.at("settledAfterThird").get_string()
                                          : expected.at("submissionAfterOpen").at("status").get_string()) ||
            all->front().request_id != expected.at("submissionAfterOpen").at("requestId").get_string())
        return 58;
    auto tasks = (*storage)->scan_tasks(100);
    if (!tasks || tasks->size() != 1 ||
            tasks->front().kind != expected.at("taskAfterOpen").get_array().front().at("kind").get_string() ||
            tasks->front().state.status !=
                    (third ? "terminal"
                           : expected.at("taskAfterOpen").get_array().front().at("recordStatus").get_string()))
        return 59;
    if (!third) {
        const auto& frozen_task = expected.at("taskAfterOpen").get_array().front();
        if (tasks->front().abort_requested != frozen_task.at("abortRequested").get_boolean()) return 71;
        if (!tasks->front().state.checkpoint || !tasks->front().state.checkpoint->as_object() ||
                !tasks->front().state.checkpoint->as_object()->contains("phase") ||
                !tasks->front().state.checkpoint->as_object()->at("phase").as_string() ||
                *tasks->front().state.checkpoint->as_object()->at("phase").as_string() !=
                        frozen_task.at("checkpointPhase").get_string())
            return 72;
    }
    if (!harness->resume()) return 60;
    auto settled = consume(submission.value().wait());
    if (!settled.has_value() ||
            settled.value().status != (third ? expected.at("settledAfterThird").get_string()
                                             : expected.at("settled").at("status").get_string()) ||
            settled.value().answer.has_value() != expected.at("settled").at("hasAnswer").get_boolean())
        return 61;
    auto view = consume(harness->conversation_view(root.value()));
    if (!view.has_value() || !view.value().as_object()) return 62;
    const auto* entries = view.value().as_object()->at("entries").as_array();
    if (!entries) return 62;
    std::ofstream transcript(directory / (third ? "transcript-third" : "transcript"));
    std::size_t answer_count = 0;
    for (const auto& entry : *entries) {
        const auto* record = entry.as_object();
        if (!record || !record->contains("kind") || !record->at("kind").as_string()) return 63;
        const auto& kind = *record->at("kind").as_string();
        if (kind != "pi.user" && kind != "pi.assistant") continue;
        if (kind == "pi.assistant") ++answer_count;
        const auto* envelope = record->at("data").as_object();
        if (!envelope || !envelope->contains("model")) return 63;
        const auto* messages = envelope->at("model").as_array();
        if (!messages || messages->empty()) return 63;
        const auto* message = messages->front().as_object();
        if (!message || !message->contains("content")) return 63;
        const auto& content = message->at("content");
        std::string text;
        if (content.as_string())
            text = *content.as_string();
        else if (const auto* blocks = content.as_array()) {
            for (const auto& block : *blocks) {
                const auto* object = block.as_object();
                if (object && object->contains("text") && object->at("text").as_string())
                    text += *object->at("text").as_string();
            }
        }
        transcript << kind << ':' << text << '\n';
    }
    transcript.close();
    if (third && static_cast<double>(answer_count) != expected.at("answerCountAfterThird").get_number()) return 66;
    if (!consume(harness->close()).has_value()) return 64;
    return 0;
}

bool receive_boundary(int fd) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    pollfd ready{.fd = fd, .events = POLLIN, .revents = 0};
    while (std::chrono::steady_clock::now() < deadline) {
        const auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        const int result = ::poll(&ready, 1, static_cast<int>(remaining.count()));
        if (result < 0 && errno == EINTR) continue;
        if (result <= 0 || (ready.revents & POLLIN) == 0) return false;
        char marker = 0;
        ssize_t received;
        do {
            received = ::read(fd, &marker, 1);
        } while (received < 0 && errno == EINTR);
        return received == 1 && marker == 'R';
    }
    return false;
}

struct ChildResult {
    bool exited{false};
    int status{0};
};

ChildResult reap_bounded(pid_t child) {
    ChildResult result;
    if (child <= 0) return result;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto waited = ::waitpid(child, &result.status, WNOHANG);
        if (waited == child) {
            result.exited = true;
            return result;
        }
        if (waited < 0 && errno != EINTR) return result;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    (void)::kill(child, SIGKILL);
    const auto reap_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < reap_deadline) {
        const auto waited = ::waitpid(child, &result.status, WNOHANG);
        if (waited == child) {
            result.exited = true;
            return result;
        }
        if (waited < 0 && errno != EINTR) return result;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return result;
}

std::vector<std::string> read_lines(const std::filesystem::path& path) {
    std::ifstream in(path);
    std::vector<std::string> result;
    std::string line;
    while (std::getline(in, line))
        result.push_back(line);
    return result;
}

std::vector<std::string> frozen_lines(const support::JsonValue& value) {
    std::vector<std::string> result;
    for (const auto& item : value.get_array())
        result.push_back(item.get_string());
    return result;
}

std::size_t line_count(const std::filesystem::path& path) {
    std::ifstream in(path);
    std::size_t count = 0;
    std::string line;
    while (std::getline(in, line))
        ++count;
    return count;
}

} // namespace

TEST_CASE("a killed effect worker retries its idempotent application after reopen", "[durable][issue1084]") {
    const auto frozen = FrozenScenario::load("fixtures/pi-v1.3.0/audit-1055/crash-recovery.json",
            "a killed process resumes an intent/effect/outcome task from its checkpoint and applies the effect once");
    REQUIRE(frozen.has_value());
    cch::tests::TempWorkspace workspace;
    int pipe_fds[2];
    REQUIRE(::pipe(pipe_fds) == 0);
    support::UniqueFd reader(pipe_fds[0]);
    support::UniqueFd writer(pipe_fds[1]);
    const pid_t worker = fork_child();
    if (worker == 0) {
        reader.reset();
        _exit(run_effect(workspace.path(), writer.get(), true));
    }
    REQUIRE(worker > 0);
    writer.reset();
    const bool got_marker = receive_boundary(reader.get());
    const bool killed = worker > 0 && ::kill(worker, SIGKILL) == 0;
    const auto killed_result = reap_bounded(worker);
    const auto calls_after_crash = line_count(workspace.path() / "calls");
    const auto applied_after_crash = line_count(workspace.path() / "applied");
    pid_t recovery = -1;
    ChildResult recovered;
    if (got_marker && killed_result.exited) {
        recovery = fork_child();
        if (recovery == 0) _exit(run_effect(workspace.path(), -1, false));
        if (recovery > 0) recovered = reap_bounded(recovery);
    }
    const auto calls_after_resume = line_count(workspace.path() / "calls");
    const auto applied_after_resume = line_count(workspace.path() / "applied");
    pid_t third = -1;
    ChildResult third_result;
    if (recovered.exited && WIFEXITED(recovered.status) && WEXITSTATUS(recovered.status) == 0) {
        third = fork_child();
        if (third == 0) _exit(run_effect(workspace.path(), -1, false, true));
        if (third > 0) third_result = reap_bounded(third);
    }
    CHECK(third > 0);
    CHECK(third_result.exited);
    CHECK(WIFEXITED(third_result.status));
    CHECK(WEXITSTATUS(third_result.status) == 0);
    CHECK(got_marker);
    CHECK(killed);
    CHECK(killed_result.exited);
    CHECK(WIFSIGNALED(killed_result.status));
    CHECK(WTERMSIG(killed_result.status) == SIGKILL);
    const auto& expected = frozen->expected().get_object();
    CHECK(calls_after_crash == expected.at("effectAfterCrash").at("calls").get_number());
    CHECK(applied_after_crash == expected.at("effectAfterCrash").at("applied").get_number());
    CHECK(recovery > 0);
    CHECK(recovered.exited);
    CHECK(WIFEXITED(recovered.status));
    CHECK(WEXITSTATUS(recovered.status) == 0);
    CHECK(calls_after_resume == expected.at("effectAfterResume").at("calls").get_number());
    CHECK(applied_after_resume == expected.at("effectAfterResume").at("applied").get_number());
    CHECK(line_count(workspace.path() / "calls") == expected.at("effectAfterThird").at("calls").get_number());
    CHECK(line_count(workspace.path() / "applied") == expected.at("effectAfterThird").at("applied").get_number());
}

void check_lifecycle_recovery(bool abort_case) {
    const auto frozen = FrozenScenario::load("fixtures/pi-v1.3.0/audit-1055/crash-recovery.json",
            abort_case ? "an abort mark survives a killed process and only the abort handler runs after reopen"
                       : "a killed process after a terminal commit does not rerun the task or duplicate the "
                         "submission");
    REQUIRE(frozen.has_value());
    cch::tests::TempWorkspace workspace;
    int fds[2];
    REQUIRE(::pipe(fds) == 0);
    support::UniqueFd reader(fds[0]);
    support::UniqueFd writer(fds[1]);
    const pid_t worker = fork_child();
    if (worker == 0) {
        reader.reset();
        _exit(run_lifecycle(workspace.path(), writer.get(), true, abort_case));
    }
    REQUIRE(worker > 0);
    writer.reset();
    const bool got_marker = receive_boundary(reader.get());
    const bool killed = worker > 0 && ::kill(worker, SIGKILL) == 0;
    const auto killed_result = reap_bounded(worker);
    const auto before = read_lines(workspace.path() / "events");
    pid_t recovery = -1;
    ChildResult recovered;
    if (got_marker && killed_result.exited) {
        recovery = fork_child();
        if (recovery == 0) _exit(run_lifecycle(workspace.path(), -1, false, abort_case));
        if (recovery > 0) recovered = reap_bounded(recovery);
    }
    INFO("abort case: " << abort_case);
    CHECK(got_marker);
    CHECK(killed);
    CHECK(killed_result.exited);
    CHECK(WIFSIGNALED(killed_result.status));
    CHECK(WTERMSIG(killed_result.status) == SIGKILL);
    const auto& expected = frozen->expected().get_object();
    CHECK(before == frozen_lines(expected.at("workerLog")));
    CHECK(recovery > 0);
    CHECK(recovered.exited);
    CHECK(WIFEXITED(recovered.status));
    CHECK(WEXITSTATUS(recovered.status) == 0);
    auto expected_events = before;
    const auto reopened = frozen_lines(expected.at(abort_case ? "reopenedLog" : "rerunLog"));
    expected_events.insert(expected_events.end(), reopened.begin(), reopened.end());
    CHECK(read_lines(workspace.path() / "events") == expected_events);
    if (!abort_case) {
        const auto ids = read_lines(workspace.path() / "terminal-ids");
        CHECK(ids.size() == 2);
        if (ids.size() == 2) CHECK((ids[0] == ids[1]) == expected.at("duplicateSameId").get_boolean());
    }
}

TEST_CASE("a killed terminal worker does not rerun after reopen", "[durable][issue1084]") {
    check_lifecycle_recovery(false);
}

TEST_CASE("a killed abort worker retains its abort mark", "[durable][issue1084]") { check_lifecycle_recovery(true); }

TEST_CASE("a killed generation worker recovers admission without a duplicate", "[durable][issue1084]") {
    const auto frozen = FrozenScenario::load("fixtures/pi-v1.3.0/audit-1055/crash-recovery.json",
            "a killed process leaves an admitted input and its generation task recoverable without a duplicate "
            "submission");
    REQUIRE(frozen.has_value());
    cch::tests::TempWorkspace workspace;
    int fds[2];
    REQUIRE(::pipe(fds) == 0);
    support::UniqueFd reader(fds[0]);
    support::UniqueFd writer(fds[1]);
    const pid_t worker = fork_child();
    if (worker == 0) {
        reader.reset();
        _exit(run_generation(workspace.path(), writer.get(), true));
    }
    REQUIRE(worker > 0);
    writer.reset();
    const bool got_marker = receive_boundary(reader.get());
    const bool killed = worker > 0 && ::kill(worker, SIGKILL) == 0;
    const auto killed_result = reap_bounded(worker);
    pid_t recovery = -1;
    ChildResult recovered;
    if (got_marker && killed_result.exited) {
        recovery = fork_child();
        if (recovery == 0) _exit(run_generation(workspace.path(), -1, false));
        if (recovery > 0) recovered = reap_bounded(recovery);
    }
    pid_t third = -1;
    ChildResult third_result;
    if (recovered.exited && WIFEXITED(recovered.status) && WEXITSTATUS(recovered.status) == 0) {
        third = fork_child();
        if (third == 0) _exit(run_generation(workspace.path(), -1, false, true));
        if (third > 0) third_result = reap_bounded(third);
    }
    CHECK(third > 0);
    CHECK(third_result.exited);
    CHECK(WIFEXITED(third_result.status));
    CHECK(WEXITSTATUS(third_result.status) == 0);
    CHECK(got_marker);
    CHECK(killed);
    CHECK(killed_result.exited);
    CHECK(WIFSIGNALED(killed_result.status));
    CHECK(WTERMSIG(killed_result.status) == SIGKILL);
    CHECK(recovery > 0);
    CHECK(recovered.exited);
    CHECK(WIFEXITED(recovered.status));
    CHECK(WEXITSTATUS(recovered.status) == 0);
    const auto ids = read_lines(workspace.path() / "generation-ids");
    CHECK(ids.size() == 3);
    if (ids.size() == 3)
        CHECK((ids[0] == ids[1] && ids[1] == ids[2]) == frozen->expected().at("duplicateSameId").get_boolean());
    CHECK(read_lines(workspace.path() / "transcript") == frozen_lines(frozen->expected().at("transcriptAfterSettle")));
    CHECK(read_lines(workspace.path() / "transcript-third") ==
            frozen_lines(frozen->expected().at("transcriptAfterSettle")));
    CHECK(line_count(workspace.path() / "requests") == 2);
}
