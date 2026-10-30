// [issue1167] Reproducible RED for the -fno-exceptions Catch2 failure behavior
// (ADR 0042), preserved as evidence that a scope guard alone does not satisfy
// the issue's safety property.
//
// Mechanism: durable test TUs build with -fno-exceptions, so a Catch2 REQUIRE
// failure unwinds through the test frame WITHOUT running local destructors
// (CODING_STANDARDS.md §11.10). A test that REQUIRE-fails while its Harness
// owns live threads leaks them: the scheduler worker and every execution
// thread keep running against captures that lived in the abandoned frame.
//
// Evaluated and REJECTED: a `LiveHarness` scope class whose Catch2
// `testCaseEnded` listener closes still-registered scopes. The listener runs
// after the frame is abandoned, and its teardown recurses below that frame on
// the same stack, so a still-running worker or phase can read overwritten
// stack objects (recorded cores: the worker faulted inside
// `schedule_step -> reconcile -> MemoryStorage::scan_conversations` on
// corrupted frame bytes). Cleanup at `testCaseEnded` therefore does not
// establish "a failed assertion leaves no live threads or mutexes"; see
// docs/research/pi-v1.3.0/1167-thread-leak-guard/NOTES.md.
//
// This pair stays hidden (`[.]`) so no CTest discovery spec runs it. It runs
// only when a spec names its tag explicitly:
//
//     cch_tests_durable '[issue1167]'
//
// Expected result TODAY (RED), with the canonical invocation that pins the
// order:
//
//     cch_tests_durable '[issue1167]' --order decl
//
// the probe case reports its deliberate assertion failure, and the victim case
// fails because the leaked worker is still live. Catch2 randomizes the run
// order by default, so the victim reports an explicit ordering failure instead
// of passing vacuously when it happens to run first. When a sound guard lands
// (assert-level release-then-assert plus heap-owned thread-visible state, a
// follow-up slice), the victim turns green and only the deliberate failure
// remains.
#include <cch/durable/Harness.hpp>
#include <cch/durable/Storage.hpp>
#include <cch/durable/Task.hpp>

#include "durable/Outcome.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace cch;
using namespace cch::durable;
using namespace cch::tests::durable;

namespace {

[[nodiscard]] long thread_count_from_proc() {
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.starts_with("Threads:")) return std::stol(line.substr(line.find(':') + 1));
    }
    return -1;
}

// Captured before main: every harness thread in the suite is transient, so a
// later count above the startup baseline names a leak.
const long g_threads_at_startup = thread_count_from_proc();

// Set by the probe immediately before its deliberate failure; the victim needs
// it to tell "the probe ran and leaked" from "this case ran first".
std::atomic_bool g_probe_failed_with_live_harness{false};

[[nodiscard]] bool wait_for(const std::atomic_bool& flag, std::chrono::seconds budget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (!flag.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return flag.load();
}

} // namespace

// Deliberate RED, pinned as the failure injection: the assertion below is
// WRONG ON PURPOSE and must never be "fixed". It fails while the Harness is
// still open and its phase thread is live, the exact shape that crashed the
// suite under #1166 (9/12 full-suite runs SIGSEGV'd).
TEST_CASE("a failing assertion with a live harness phase leaves the harness open", "[.][issue1167][cleanup-probe]") {
    // Heap-owned so the threads that outlive the failure keep valid state;
    // the unique_ptrs are intentionally leaked through the failing REQUIRE.
    // Frame-owned state would be overwritten by later stack use while the
    // leaked threads still read it.
    auto storage = std::make_unique<MemoryStorage>();
    auto events = std::make_shared<std::vector<std::string>>();
    auto phase_ready = std::make_shared<std::atomic_bool>(false);
    auto builder = TaskDefinitionBuilder("audit1167.probe", 1);
    builder.initial([](const chord::Json&) { return chord::Json(chord::Json::Object{{"phase", "run"}}); });
    builder.phase("run", [events, phase_ready](const TaskRecord&, TaskRuntime& runtime) -> support::AsyncResult<void> {
        events->push_back("phase-ran");
        phase_ready->store(true);
        // Stays live well past the failing assertion. Stop-aware and bounded:
        // this is the shape a real phase should have, and it still leaks
        // because nothing requests the stop on the failure path.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!runtime.stop_token().stop_requested() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return support::AsyncResult<void>{support::ExpectedVoid{}};
    });
    auto definition = builder.build();
    REQUIRE(definition.has_value());
    std::vector<TaskDefinition> definitions;
    definitions.push_back(std::move(*definition));

    auto opened = consume(Harness::open(*storage, HarnessOptions{.tasks = std::move(definitions)}));
    REQUIRE(opened.has_value());
    auto harness_owner = std::move(opened.value());
    Harness* harness = harness_owner.get();
    REQUIRE(consume(harness->session().commit([&](Transaction& tx) -> support::ExpectedVoid {
        auto task = tx.create_task(*harness->task_definition("audit1167.probe"), chord::Json(nullptr));
        if (!task) return std::unexpected(task.error());
        return {};
    })).has_value());
    REQUIRE(harness->resume().has_value());
    REQUIRE(wait_for(*phase_ready, std::chrono::seconds(5)));

    // Deliberate RED: this expected sequence is wrong on purpose. The failure
    // unwinds while `harness_owner` still owns the open Harness and its live
    // phase, so the frame's destructors never run and the threads leak.
    g_probe_failed_with_live_harness.store(true, std::memory_order_release);
    REQUIRE(*events == std::vector<std::string>{"phase-ran", "deliberate-red"});

    // Unreachable on the RED path; the explicit close documents the intended
    // success-path shape.
    events->push_back("after-assert");
    REQUIRE(consume(harness->close()).has_value());
}

// Runs immediately after the probe when the spec names [issue1167]. The probe
// leaked its Harness through the failing REQUIRE; this case checks the
// property the issue asks for — no live threads after a failed assertion.
// The bounded wait gives any transient thread time to drain, so a pass here
// means the count genuinely returned to baseline.
TEST_CASE("the case after a failed harness assertion sees no leaked threads", "[.][issue1167][cleanup-victim]") {
    if (!g_probe_failed_with_live_harness.load(std::memory_order_acquire)) {
        FAIL("run this pair with --order decl: the probe case must fail first, "
             "otherwise this check passes without evidence");
    }
    REQUIRE(g_threads_at_startup > 0);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (thread_count_from_proc() != g_threads_at_startup && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    // RED evidence today: the probe's scheduler worker (and its phase) are
    // still live. This is the assertion that turns green once a sound guard
    // lands; the probe's deliberate failure stays either way.
    REQUIRE(thread_count_from_proc() == g_threads_at_startup);
}
