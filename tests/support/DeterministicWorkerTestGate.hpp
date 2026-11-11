#pragma once

// Test-only synchronization seam for the deterministic Remote worker's lifetime
// regression (#1232). The test-only `cch_tests_codemode_worker_variant` target
// force-includes this header and defines `CCH_REMOTE_WORKER_TEST_HOOKS`; the
// production `cch_codemode` archive is compiled without either, so no gate
// symbol enters it and production source never includes this header
// (CODING_STANDARDS.md sections 11.5 and 11.8).
//
// The gate is a counted, latched release gate: every notification stores a
// permit in a counting semaphore, so a release that arrives before its waiter
// arms is consumed by the next wait and cannot hang a case. The existing
// `tests::ReleaseGate` is bound to the thread that pumps its Asio executor;
// this gate spans the deterministic worker thread, the serving thread and the
// test thread, so the standard library's thread-safe semaphore is the
// equivalent support facility. Fields are atomics because the test reads them
// while a production thread writes them; the semaphore orders the wake-up, not
// the data.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <semaphore>
#include <thread>

namespace cch::codemode::detail {

/// The one observation surface a lifetime test installs. The worker publishes
/// `owners_while_parked`, `worker_thread` and `parked` before waiting on
/// `release`; the serve frame publishes `join_thread`, `join_started` and
/// `join_finished`; the case's serve-completion witness publishes
/// `serve_completed` and `serve_completed_ok`. Every publication releases one
/// `changed` permit.
struct DeterministicWorkerTestGate {
    std::atomic<std::size_t> owners_while_parked{0};
    std::atomic<std::thread::id> worker_thread{};
    std::atomic<bool> parked{false};

    std::atomic<std::thread::id> join_thread{};
    std::atomic<bool> join_started{false};
    std::atomic<bool> join_finished{false};

    std::atomic<bool> serve_completed{false};
    std::atomic<bool> serve_completed_ok{false};

    std::counting_semaphore<> changed{0};
    std::binary_semaphore release{0};
};

inline std::atomic<std::shared_ptr<DeterministicWorkerTestGate>> deterministic_worker_test_gate;

/// Parks the deterministic worker after `drive()` returned, before its closure
/// releases `state`. Inert without a gate. The wait is bounded so a case that
/// abandons the schedule cannot strand the worker forever; the bound is a
/// liveness backstop, never a pass condition.
template <typename State> inline void park_deterministic_worker(const std::shared_ptr<State>& state) {
    auto gate = deterministic_worker_test_gate.load();
    if (!gate) return;
    gate->owners_while_parked.store(state.use_count());
    gate->worker_thread.store(std::this_thread::get_id());
    gate->parked.store(true);
    gate->changed.release();
    (void)gate->release.try_acquire_for(std::chrono::seconds{2});
}

/// Records that the serve frame is about to join the worker, and from which
/// thread. Inert without a gate.
inline void record_deterministic_worker_join_started() {
    auto gate = deterministic_worker_test_gate.load();
    if (!gate) return;
    gate->join_thread.store(std::this_thread::get_id());
    gate->join_started.store(true);
    gate->changed.release();
}

/// Records that the serve frame's join returned, i.e. the worker thread ended.
/// Inert without a gate.
inline void record_deterministic_worker_join_finished() {
    auto gate = deterministic_worker_test_gate.load();
    if (!gate) return;
    gate->join_finished.store(true);
    gate->changed.release();
}

} // namespace cch::codemode::detail
