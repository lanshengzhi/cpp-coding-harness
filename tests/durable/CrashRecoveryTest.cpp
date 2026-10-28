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

#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace cch;
using namespace cch::durable;
using namespace cch::tests::durable;

namespace {

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
            return NextTaskState::terminal(TaskOutcome{.status = "completed", .result = chord::Json(70.0)});
        });
    });
    return std::move(*builder.build());
}

int run_effect(const std::filesystem::path& directory, int ready_fd, bool first) {
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
        if (!record.has_value() || !record.value() || record.value()->state.status != "pending") return 16;
    }
    if (!harness->resume()) return 17;
    auto outcome = consume(harness->wait_for_task(id));
    if (!outcome.has_value() || outcome.value().state.status != "terminal" || !outcome.value().state.outcome ||
            outcome.value().state.outcome->status != "completed" || !outcome.value().state.outcome->result ||
            !outcome.value().state.outcome->result->as_number() ||
            *outcome.value().state.outcome->result->as_number() != 70.0)
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
                    while (!runtime.stop_token().stop_requested())
                        std::this_thread::yield();
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
        if (abort_case && (record.value()->state.status != "pending" || !record.value()->abort_requested)) return 38;
        if (!abort_case && record.value()->state.status != "terminal") return 39;
    }
    if (!abort_case) {
        auto root = consume(harness->root());
        if (!root.has_value()) return 47;
        SubmissionDraft draft{.request_id = "terminal-request",
                .body = WriteSubmissionDraft{.entry = WriteEntry{.kind = "test.terminal"}}};
        auto submission = consume(harness->submit(root.value(), std::move(draft)));
        if (!submission.has_value()) return 48;
        auto records = (*storage)->scan_submissions(100);
        if (!records || records->size() != 1 || records->front().id != submission.value().id ||
                records->front().status != "done")
            return 49;
    }
    if (!harness->resume()) return 40;
    if (first && abort_case) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        std::error_code error;
        while (!std::filesystem::exists(directory / "events", error) && !error &&
                std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (error || !std::filesystem::exists(directory / "events", error) || error) return 50;
        if (!consume(harness->abort_task(id)).has_value()) return 41;
    }
    auto outcome = consume(harness->wait_for_task(id));
    if (!outcome.has_value() || !outcome.value().state.outcome) return 42;
    const auto& receipt = *outcome.value().state.outcome;
    if (receipt.status != (abort_case ? "aborted" : "completed")) return 43;
    if (abort_case && receipt.reason != "recovered") return 44;
    if (!abort_case && (!receipt.result || !receipt.result->as_string() || *receipt.result->as_string() != "once"))
        return 45;
    if (first) signal_boundary(ready_fd);
    if (!consume(harness->close()).has_value()) return 46;
    return 0;
}

SubmissionDraft crash_input() {
    return SubmissionDraft{
            .request_id = "crash-1", .body = InputSubmissionDraft{.content = chord::Json("crash input")}};
}

int run_generation(const std::filesystem::path& directory, int ready_fd, bool first, bool third = false) {
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
    auto submission = consume(harness->submit(root.value(), crash_input()));
    if (!submission.has_value()) return 55;
    if (first) {
        if (!harness->resume()) return 56;
        // The request handler signals after admission and before model completion.
        auto settled = consume(submission.value().wait());
        (void)settled;
        return 57;
    }
    auto all = (*storage)->scan_submissions(100);
    if (!all || all->size() != 1 || all->front().id != submission.value().id ||
            all->front().status != (third ? "done" : "placed"))
        return 58;
    auto tasks = (*storage)->scan_tasks(100);
    if (!tasks || tasks->size() != 1 || tasks->front().kind != "pi.generation" ||
            tasks->front().state.status != (third ? "terminal" : "pending"))
        return 59;
    if (!harness->resume()) return 60;
    auto settled = consume(submission.value().wait());
    if (!settled.has_value() || settled.value().status != "done" || !settled.value().answer) return 61;
    auto entries = (*storage)->scan_entries(root.value(), 100);
    if (!entries) return 62;
    int users = 0;
    int answers = 0;
    for (const auto& entry : *entries) {
        if (entry.kind == "pi.user") ++users;
        if (entry.kind == "pi.assistant") ++answers;
    }
    if (users != 1 || answers != 1) return 63;
    if (!consume(harness->close()).has_value()) return 64;
    return 0;
}

struct ChildResult {
    bool exited{false};
    int status{0};
};

ChildResult reap_bounded(pid_t child) {
    ChildResult result;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto waited = ::waitpid(child, &result.status, WNOHANG);
        if (waited == child) {
            result.exited = true;
            return result;
        }
        if (waited < 0) return result;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    (void)::kill(child, SIGKILL);
    result.exited = ::waitpid(child, &result.status, 0) == child;
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
    const pid_t worker = ::fork();
    REQUIRE(worker >= 0);
    if (worker == 0) {
        reader.reset();
        _exit(run_effect(workspace.path(), writer.get(), true));
    }
    writer.reset();
    pollfd ready{.fd = reader.get(), .events = POLLIN, .revents = 0};
    const bool signalled = ::poll(&ready, 1, 10000) == 1 && (ready.revents & POLLIN) != 0;
    char marker = 0;
    const bool got_marker = signalled && ::read(reader.get(), &marker, 1) == 1 && marker == 'R';
    const bool killed = ::kill(worker, SIGKILL) == 0;
    const auto killed_result = reap_bounded(worker);
    const auto calls_after_crash = line_count(workspace.path() / "calls");
    const auto applied_after_crash = line_count(workspace.path() / "applied");
    pid_t recovery = -1;
    ChildResult recovered;
    if (got_marker && killed_result.exited) {
        recovery = ::fork();
        if (recovery == 0) _exit(run_effect(workspace.path(), -1, false));
        if (recovery > 0) recovered = reap_bounded(recovery);
    }
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
    CHECK(line_count(workspace.path() / "calls") == expected.at("effectAfterResume").at("calls").get_number());
    CHECK(line_count(workspace.path() / "applied") == expected.at("effectAfterResume").at("applied").get_number());
}

TEST_CASE("killed lifecycle workers retain terminal outcomes and abort marks", "[durable][issue1084]") {
    for (const bool abort_case : {false, true}) {
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
        const pid_t worker = ::fork();
        REQUIRE(worker >= 0);
        if (worker == 0) {
            reader.reset();
            _exit(run_lifecycle(workspace.path(), writer.get(), true, abort_case));
        }
        writer.reset();
        pollfd ready{.fd = reader.get(), .events = POLLIN, .revents = 0};
        const bool signalled = ::poll(&ready, 1, 10000) == 1 && (ready.revents & POLLIN) != 0;
        char marker = 0;
        const bool got_marker = signalled && ::read(reader.get(), &marker, 1) == 1 && marker == 'R';
        const bool killed = ::kill(worker, SIGKILL) == 0;
        const auto killed_result = reap_bounded(worker);
        const auto before = line_count(workspace.path() / "events");
        pid_t recovery = -1;
        ChildResult recovered;
        if (got_marker && killed_result.exited) {
            recovery = ::fork();
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
        CHECK(before == expected.at("workerLog").get_array().size());
        CHECK(recovery > 0);
        CHECK(recovered.exited);
        CHECK(WIFEXITED(recovered.status));
        CHECK(WEXITSTATUS(recovered.status) == 0);
        CHECK(line_count(workspace.path() / "events") ==
                before + (abort_case ? expected.at("reopenedLog").get_array().size()
                                     : expected.at("rerunLog").get_array().size()));
    }
}

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
    const pid_t worker = ::fork();
    REQUIRE(worker >= 0);
    if (worker == 0) {
        reader.reset();
        _exit(run_generation(workspace.path(), writer.get(), true));
    }
    writer.reset();
    pollfd ready{.fd = reader.get(), .events = POLLIN, .revents = 0};
    const bool signalled = ::poll(&ready, 1, 10000) == 1 && (ready.revents & POLLIN) != 0;
    char marker = 0;
    const bool got_marker = signalled && ::read(reader.get(), &marker, 1) == 1 && marker == 'R';
    const bool killed = ::kill(worker, SIGKILL) == 0;
    const auto killed_result = reap_bounded(worker);
    pid_t recovery = -1;
    ChildResult recovered;
    if (got_marker && killed_result.exited) {
        recovery = ::fork();
        if (recovery == 0) _exit(run_generation(workspace.path(), -1, false));
        if (recovery > 0) recovered = reap_bounded(recovery);
    }
    pid_t third = -1;
    ChildResult third_result;
    if (recovered.exited && WIFEXITED(recovered.status) && WEXITSTATUS(recovered.status) == 0) {
        third = ::fork();
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
    CHECK(frozen->expected().at("duplicateSameId").get_boolean());
    CHECK(line_count(workspace.path() / "requests") == 2);
}
