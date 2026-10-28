// Process-crash comparison with audit-1055: the external effect is replayed,
// while its stable key makes the application itself idempotent.
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
    CHECK(calls_after_crash == 1);
    CHECK(applied_after_crash == 1);
    CHECK(recovery > 0);
    CHECK(recovered.exited);
    CHECK(WIFEXITED(recovered.status));
    CHECK(WEXITSTATUS(recovered.status) == 0);
    CHECK(line_count(workspace.path() / "calls") == 2);
    CHECK(line_count(workspace.path() / "applied") == 1);
}
