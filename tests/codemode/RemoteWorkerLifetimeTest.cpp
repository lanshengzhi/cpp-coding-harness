// Ticket #1232 (CODING_STANDARDS.md §9.13): the deterministic Remote worker's
// thread handle must not live in the shared state that the worker's own closure
// keeps alive.
//
// The recorded defect is a self-join: `RemoteDeterministicServe` owned the
// handle, the worker's closure owned the state, so the closure's release
// destroyed the state on the worker thread and joined that same thread
// (`std::system_error: Resource deadlock avoided`, exit 134). This case
// reproduces that ownership shape instead of racing it:
//
//   1. the test hook parks the worker after `drive()` returned, before its
//      closure releases the state;
//   2. the serve coroutine has taken its terminal message and is returning;
//   3. the case records whether the serve frame joins the worker, from which
//      thread, whether the worker's closure was still the state's only owner,
//      and whether the serve's own `AsyncResult` completes only afterwards;
//   4. the gate is released and every retirement witness is read back before
//      the first assertion (CODING_STANDARDS.md §11.10: a failing assertion in
//      this `-fno-exceptions` TU runs no destructor, so nothing may be held).
//
// Under the recorded defect step 3 finds no join and step 4 aborts the process
// on the worker thread. The gate is inert without this case.

#include <cch/codemode/Remote.hpp>

#include "codemode/RemoteWorkerTestHook.hpp"
#include "support/AsyncResultBridge.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace cch;
using namespace std::chrono_literals;

namespace {

using Gate = codemode::detail::DeterministicWorkerTestGate;

/// The liveness backstop every wait in this case carries. It bounds a stuck
/// schedule; it is never the evidence that the schedule was correct.
constexpr auto kWaitBound = 20s;

/// Installs this case's gate and clears it again, so no later case in this
/// process can observe a stale latch.
struct GateInstall {
    std::shared_ptr<Gate> gate = std::make_shared<Gate>();
    GateInstall() { codemode::detail::deterministic_worker_test_gate.store(gate); }
    ~GateInstall() { codemode::detail::deterministic_worker_test_gate.store(nullptr); }
};

/// Waits until `done` holds on the gate, or the bound elapses. A timeout is
/// reported through the recorded latches, never as a pass.
template <typename Predicate> bool wait_for_gate(const std::shared_ptr<Gate>& gate, Predicate done) {
    std::unique_lock lock(gate->mutex);
    return gate->changed.wait_for(lock, kWaitBound, [&] { return done(*gate); });
}

/// Everything this case asserts on, recorded before the first assertion.
struct RetirementObservation {
    bool worker_parked{false};
    bool join_started{false};
    bool join_thread_differs_from_worker{false};
    std::size_t owners_while_parked{0};
    bool join_finished{false};
    bool serve_completed{false};
    bool serve_completed_ok{false};
    bool serve_completed_before_release{false};
    bool serve_completed_after_join{false};
};

/// The frozen deterministic start every remote case shares; `started_at` selects
/// the deterministic serve, `return 2` needs no caller answer.
[[nodiscard]] codemode::CodemodeRemoteRequest deterministic_request() {
    codemode::CodemodeRemoteRequest request;
    request.data.code = "return 2";
    request.data.started_at = 1768471200000;
    request.data.seed = 1;
    return request;
}

/// The caller's end: no call is issued, so every batch (the terminal one here)
/// is answered without a result. The terminal reply is not read by the serve.
[[nodiscard]] support::AsyncResult<codemode::CodemodeRemoteReplyVariant> answer_without_result(
        std::vector<codemode::CodemodeRemoteMessageVariant>) {
    return support::AsyncResult<codemode::CodemodeRemoteReplyVariant>{
            support::Expected<codemode::CodemodeRemoteReplyVariant>{codemode::CodemodeRemoteNullReply{}}};
}

} // namespace

TEST_CASE("Remote deterministic worker is joined by the serve frame, never by the worker",
        "[codemode][remote][deterministic][issue1232remote]") {
    GateInstall install;
    const auto gate = install.gate;
    RetirementObservation observation;

    codemode::CodemodeServeOptions options;
    options.wasm_path = std::filesystem::path{CCH_SOURCE_DIR} / "fixtures" / "codemode" / "quickjs" / "quickjs.wasm";
    auto served = codemode::serve_codemode_remote(deterministic_request(), answer_without_result, std::move(options));

    // Drive the serve on its own context thread: the fixed shape blocks that
    // thread in the frame's join until this case releases the parked worker.
    boost::asio::io_context context;
    boost::asio::co_spawn(
            context,
            [operation = std::move(served), gate]() mutable -> boost::asio::awaitable<void> {
                auto outcome = co_await support::detail::await_async_result(std::move(operation));
                {
                    std::lock_guard lock(gate->mutex);
                    gate->serve_completed = true;
                    gate->serve_completed_ok = outcome.has_value();
                }
                gate->changed.notify_all();
            },
            boost::asio::detached);
    std::thread driver([&context] { context.run(); });

    // 1. The worker reached the gate: `drive()` returned and its closure still
    //    holds the state.
    observation.worker_parked = wait_for_gate(gate, [](const Gate& g) { return g.parked; });
    // 2. Either the serve frame joins that parked worker (fixed shape) or the
    //    serve completes with no join at all (the recorded defect). This is the
    //    transition the case exists for, so the wait ends on the observation.
    (void)wait_for_gate(gate, [](const Gate& g) { return g.join_started || g.serve_completed; });
    {
        std::lock_guard lock(gate->mutex);
        observation.join_started = gate->join_started;
        observation.join_thread_differs_from_worker = gate->join_started && gate->join_thread != gate->worker_thread;
        observation.owners_while_parked = gate->state.use_count();
        observation.serve_completed_before_release = gate->serve_completed;
    }
    // 3. Release the gate. The worker cannot retire while it is held, and no
    //    assertion may run before step 4 has retired every thread this case owns.
    {
        std::lock_guard lock(gate->mutex);
        gate->release = true;
        gate->changed.notify_all();
    }
    // 4. Read the retirement back, then let the driving context finish.
    observation.join_finished = wait_for_gate(gate, [](const Gate& g) { return g.join_finished; });
    (void)wait_for_gate(gate, [](const Gate& g) { return g.serve_completed; });
    driver.join();
    {
        std::lock_guard lock(gate->mutex);
        observation.serve_completed = gate->serve_completed;
        observation.serve_completed_ok = gate->serve_completed_ok;
        observation.serve_completed_after_join = gate->serve_completed && gate->join_finished;
    }

    CHECK(observation.worker_parked);
    // The frame owns the handle: the parked worker's closure and the serve
    // frame's own `served` are both owners, so the closure's later release is
    // not the state's last use and cannot destroy the state on the worker.
    CHECK(observation.join_started);
    CHECK(observation.join_thread_differs_from_worker);
    CHECK(observation.owners_while_parked >= 2);
    CHECK(observation.join_finished);
    // The serve's own result is later than the worker's exit: the frame joins
    // before it returns, so the internal `AsyncResult` completes afterwards.
    CHECK_FALSE(observation.serve_completed_before_release);
    CHECK(observation.serve_completed);
    CHECK(observation.serve_completed_after_join);
    CHECK(observation.serve_completed_ok);
}
