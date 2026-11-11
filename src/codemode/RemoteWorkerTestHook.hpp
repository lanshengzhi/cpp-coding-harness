#pragma once

// Test-only synchronization seam for the deterministic Remote worker's lifetime
// regression (#1232). Compiled into the codemode test shard only, through
// `tests/codemode/RemoteWithTestHooks.cpp`; the production archive never
// carries these symbols because the only inclusion sites are behind
// `CCH_REMOTE_WORKER_TEST_HOOKS`.
//
// Two latches, both inert until a test installs a gate:
//   * the worker parks after `drive()` returns, before its closure releases the
//     shared state, so a test can retire every other owner first (the schedule
//     that makes the recorded self-join deterministic);
//   * the serve coroutine frame records which thread joins the worker and when
//     that join returned, so a test can tell a frame-owned join from none.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

namespace cch::codemode::detail {

/// The one observation surface a lifetime test installs. Production-side hooks
/// only ever set: the parked worker publishes `state`/`worker_thread`/`parked`
/// and waits for `release`; the serve frame publishes `join_*`. The test reads
/// and releases. A test that records its own completion witness writes the
/// `serve_*` fields, so every wait in one case can share one condition.
struct DeterministicWorkerTestGate {
    std::mutex mutex;
    std::condition_variable changed;
    /// The shared state the parked worker's closure holds; owner counting only.
    std::weak_ptr<const void> state;
    bool parked{false};
    /// The test's release; also tells a completion witness whether the worker
    /// was still held when the serve coroutine returned.
    bool release{false};
    bool join_started{false};
    bool join_finished{false};
    std::thread::id worker_thread;
    std::thread::id join_thread;
    bool serve_completed{false};
    bool serve_completed_ok{false};
};

inline std::atomic<std::shared_ptr<DeterministicWorkerTestGate>> deterministic_worker_test_gate;

/// Parks the deterministic worker after `drive()` returned, before its closure
/// releases `state`. Inert without a gate. The wait is bounded so a test that
/// abandons the schedule cannot strand the worker forever; the bound is a
/// liveness backstop, never a pass condition. Deduced on the state's own type
/// so the call itself creates no extra strong owner while a test counts them.
template <typename State> inline void park_deterministic_worker(const std::shared_ptr<State>& state) {
    auto gate = deterministic_worker_test_gate.load();
    if (!gate) return;
    std::unique_lock lock(gate->mutex);
    gate->state = state;
    gate->worker_thread = std::this_thread::get_id();
    gate->parked = true;
    gate->changed.notify_all();
    (void)gate->changed.wait_for(lock, std::chrono::seconds{20}, [&gate] { return gate->release; });
}

/// Records that the serve frame is about to join the worker, and from which
/// thread. Inert without a gate.
inline void record_deterministic_worker_join_started() {
    auto gate = deterministic_worker_test_gate.load();
    if (!gate) return;
    {
        std::lock_guard lock(gate->mutex);
        gate->join_thread = std::this_thread::get_id();
        gate->join_started = true;
    }
    gate->changed.notify_all();
}

/// Records that the serve frame's join returned, i.e. the worker thread ended.
/// Inert without a gate.
inline void record_deterministic_worker_join_finished() {
    auto gate = deterministic_worker_test_gate.load();
    if (!gate) return;
    {
        std::lock_guard lock(gate->mutex);
        gate->join_finished = true;
    }
    gate->changed.notify_all();
}

} // namespace cch::codemode::detail
