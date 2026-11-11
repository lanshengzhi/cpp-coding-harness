#pragma once

// Test-only synchronization seam for the deterministic Remote worker's lifetime
// regressions (#1232). The test-only `cch_tests_codemode_worker_variant` target
// force-includes this header and defines `CCH_REMOTE_WORKER_TEST_HOOKS`; the
// production `cch_codemode` archive is compiled without either, so no gate
// symbol enters it and production source never includes this header
// (CODING_STANDARDS.md sections 11.5 and 11.8).
//
// The gate's notifications are counted, latched permits: every publication
// stores a permit in a counting semaphore, so a notification that arrives before
// its waiter arms is consumed by the next wait and cannot hang a case. The
// existing `tests::ReleaseGate` is bound to the thread that pumps its Asio
// executor; this gate spans the deterministic worker thread, the serving thread
// and the test thread, so the standard library's thread-safe semaphores are the
// equivalent support facility. Fields are atomics because the case reads them
// while a production thread writes them; the semaphore orders the wake-up, not
// the data.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <semaphore>
#include <stop_token>
#include <thread>

namespace cch::codemode::detail {

/// Where the worker parks for a lifetime case. One gate is installed
/// process-wide, so a case selects the single park point its schedule needs and
/// the other park point stays inert.
enum class WorkerParkPoint {
    /// After `drive()` returned, before the worker's closure releases `state`.
    /// The run is settled; the case observes the frame's join and that the
    /// closure's release is not the state's last use.
    after_drive,
    /// Inside `drive()`, in the caller's `call` host callback, before the run
    /// parks on an answer the case never sends. The case destroys the serve
    /// frame with the run unfinished.
    inside_drive,
};

/// The one observation surface a lifetime case installs. Each producer publishes
/// its fields and then releases one `changed` permit.
struct DeterministicWorkerTestGate {
    std::atomic<WorkerParkPoint> park_point{WorkerParkPoint::after_drive};

    // Worker side.
    std::atomic<std::thread::id> worker_thread{};
    std::atomic<bool> parked{false};
    std::atomic<bool> parked_in_drive{false};
    std::atomic<bool> left_in_drive_park{false};
    std::atomic<bool> stop_requested_before_park_release{false};
    std::atomic<bool> stop_requested_after_park_release{false};
    std::atomic<bool> drive_returned{false};

    /// Non-owning view of the state the worker's closure shares, recorded when
    /// the worker starts. A lifetime case uses it to prove which owner kept the
    /// state alive across the join without relying on a bare count.
    std::weak_ptr<void> state;

    // Frame side: the scope owner that destroys first in the serve frame.
    std::atomic<std::thread::id> join_thread{};
    std::atomic<bool> join_started{false};
    std::atomic<bool> join_finished{false};
    std::atomic<bool> state_alive_after_join{false};
    /// Strong owners left once the join returned, excluding this gate's own
    /// temporary lock.
    std::atomic<std::size_t> owners_after_join{0};

    // Case side: the serve's own `AsyncResult`.
    std::atomic<bool> serve_completed{false};
    std::atomic<bool> serve_completed_ok{false};

    std::counting_semaphore<> changed{0};
    /// Release for the after-drive park.
    std::binary_semaphore release{0};
    /// Release for the inside-drive park; a separate permit so the two park
    /// points cannot consume each other's release.
    std::binary_semaphore release_in_drive{0};
};

inline std::atomic<std::shared_ptr<DeterministicWorkerTestGate>> deterministic_worker_test_gate;

/// Liveness backstop for a park whose release never arrives. It is never a pass
/// condition: each case releases its park, and a missing release shows up in the
/// recorded observations instead of being read as success.
inline constexpr auto kWorkerParkBound = std::chrono::seconds{2};

/// The gate a case installed, or null. Inert without a gate.
[[nodiscard]] inline std::shared_ptr<DeterministicWorkerTestGate> current_deterministic_worker_gate() {
    return deterministic_worker_test_gate.load();
}

/// Records the worker thread and a non-owning view of the state its closure
/// shares. Inert without a gate.
template <typename State> inline void record_deterministic_worker_started(const std::shared_ptr<State>& state) {
    auto gate = current_deterministic_worker_gate();
    if (!gate) return;
    gate->worker_thread.store(std::this_thread::get_id());
    gate->state = state;
}

/// Records that `drive()` returned, i.e. the run settled and the worker is about
/// to release its closure. Inert without a gate.
inline void record_deterministic_worker_drive_returned() {
    auto gate = current_deterministic_worker_gate();
    if (!gate) return;
    gate->drive_returned.store(true);
    gate->changed.release();
}

/// Parks the worker after `drive()` returned, before its closure releases
/// `state`. Parks only at the after-drive point. Inert without a gate.
inline void park_deterministic_worker() {
    auto gate = current_deterministic_worker_gate();
    if (!gate || gate->park_point.load() != WorkerParkPoint::after_drive) return;
    gate->parked.store(true);
    gate->changed.release();
    (void)gate->release.try_acquire_for(kWorkerParkBound);
}

/// Parks the worker inside `drive()`, in the caller's `call` host callback,
/// before the run parks on an answer the case never sends. The stop token is
/// sampled before and after the park, so the case can tell a request made by the
/// frame's destruction from one the caller made. Parks only at the inside-drive
/// point. Inert without a gate.
inline void park_deterministic_worker_in_drive(std::stop_token token) {
    auto gate = current_deterministic_worker_gate();
    if (!gate || gate->park_point.load() != WorkerParkPoint::inside_drive) return;
    gate->stop_requested_before_park_release.store(token.stop_requested());
    gate->parked_in_drive.store(true);
    gate->changed.release();
    (void)gate->release_in_drive.try_acquire_for(kWorkerParkBound);
    gate->left_in_drive_park.store(true);
    gate->stop_requested_after_park_release.store(token.stop_requested());
    gate->changed.release();
}

/// Records that the serve frame is about to join the worker, and from which
/// thread. Inert without a gate.
inline void record_deterministic_worker_join_started() {
    auto gate = current_deterministic_worker_gate();
    if (!gate) return;
    gate->join_thread.store(std::this_thread::get_id());
    gate->join_started.store(true);
    gate->changed.release();
}

/// Records that the frame's join returned, and whether the state survived the
/// worker's exit. At this point the worker's thread has ended, so a live state
/// means the frame — which is executing this destructor — still owns it. Inert
/// without a gate.
inline void record_deterministic_worker_join_finished() {
    auto gate = current_deterministic_worker_gate();
    if (!gate) return;
    const auto remaining = gate->state.lock();
    gate->state_alive_after_join.store(remaining != nullptr);
    gate->owners_after_join.store(remaining == nullptr ? 0 : remaining.use_count() - 1);
    gate->join_finished.store(true);
    gate->changed.release();
}

} // namespace cch::codemode::detail
