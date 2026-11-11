// Ticket #1232 (CODING_STANDARDS.md §9.13): the deterministic Remote worker's
// thread handle must not live in the shared state that the worker's own closure
// keeps alive, and destroying the serve frame must stop and join an unfinished
// worker instead of leaking or detaching it.
//
// The recorded defect is a self-join: `RemoteDeterministicServe` owned the
// handle, the worker's closure owned the state, so the closure's release
// destroyed the state on the worker thread and joined that same thread
// (`std::system_error: Resource deadlock avoided`, exit 134).
//
// Case 1 reproduces that ownership shape instead of racing it:
//
//   1. the test gate parks the worker after `drive()` returned, before its
//      closure releases the state;
//   2. the serve coroutine has taken its terminal message and is returning;
//   3. the case records whether the serve frame joins the worker, from which
//      thread, whether the state is still owned after the worker's thread ended,
//      and whether the serve's own `AsyncResult` completes only afterwards;
//   4. the gate is released and every retirement witness is read back before
//      the first assertion (CODING_STANDARDS.md §11.10: a failing assertion in
//      this `-fno-exceptions` TU runs no destructor, so nothing may be held).
//
// Under the recorded defect step 3 finds no join and step 4 aborts the process
// on the worker thread.
//
// Case 2 covers the abandonment path ADR 0040 names: the serve frame is
// destroyed while the run is unfinished (the caller never answers the call the
// worker reported). Destroying the frame must request stop on the run and join
// the worker, from the destroying thread, before the frame is gone.
//
// Case 3 closes the cross-repair dependency between that frame join and the
// run's stop notification: a run that is already parked in the delivery loop's
// `cv.wait` for a caller answer can only be woken by the run's `stop_notify`
// callback, and an unpaired callback can miss the wake. The frame's join is
// then unbounded, so the two repairs are only sufficient together.

#include <cch/codemode/Remote.hpp>

#include "support/AsyncResultBridge.hpp"
#include "support/DeterministicWorkerTestGate.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <semaphore>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

using namespace cch;
using namespace std::chrono_literals;

namespace {

using Gate = codemode::detail::DeterministicWorkerTestGate;
using ParkPoint = codemode::detail::WorkerParkPoint;

/// The liveness backstop every wait in these cases carries. The bounded waits
/// cover a wasm load, the whole deterministic drive, and the frame's request
/// stop-and-join; the bound is never the evidence that a schedule was correct.
constexpr auto kWaitBound = 5s;

/// Upper bound on one abandoned frame's destruction, measured. The delivered
/// path returns in milliseconds; the bound states that the flow is bounded well
/// below the case's 60 s CTest timeout.
constexpr auto kBoundedAbandon = 10s;

/// Installs this case's gate and clears it again, so no later case in this
/// process can observe a stale latch.
struct GateInstall {
    std::shared_ptr<Gate> gate = std::make_shared<Gate>();
    GateInstall() { codemode::detail::deterministic_worker_test_gate.store(gate); }
    ~GateInstall() { codemode::detail::deterministic_worker_test_gate.store(nullptr); }
};

/// Waits until `done` holds, waking on `changed`, or `kWaitBound` elapses. The
/// counting semaphore is latched: a notification that arrives before this wait
/// is consumed immediately and the predicate is re-checked, so there is no
/// arm/release ordering race. A timeout is reported through the recorded
/// observations, never as a pass.
template <typename Predicate> bool wait_until(std::counting_semaphore<>& changed, Predicate done) {
    const auto deadline = std::chrono::steady_clock::now() + kWaitBound;
    while (!done()) {
        const auto remaining = deadline - std::chrono::steady_clock::now();
        if (remaining <= std::chrono::steady_clock::duration::zero()) return done();
        if (!changed.try_acquire_for(remaining)) return done();
    }
    return true;
}

template <typename Predicate> bool wait_for_gate(const std::shared_ptr<Gate>& gate, Predicate done) {
    return wait_until(gate->changed, [&] { return done(*gate); });
}

/// The frozen deterministic start every remote case shares; `started_at` selects
/// the deterministic serve.
[[nodiscard]] codemode::CodemodeRemoteRequest deterministic_request(std::string code) {
    codemode::CodemodeRemoteRequest request;
    request.data.code = std::move(code);
    request.data.started_at = 1768471200000;
    request.data.seed = 1;
    return request;
}

[[nodiscard]] std::filesystem::path guest_wasm() {
    return std::filesystem::path{CCH_SOURCE_DIR} / "fixtures" / "codemode" / "quickjs" / "quickjs.wasm";
}

/// The caller's end for case 1: no call is issued, so every batch (the terminal
/// one here) is answered without a result. The terminal reply is not read by the
/// serve.
[[nodiscard]] support::AsyncResult<codemode::CodemodeRemoteReplyVariant> answer_without_result(
        std::vector<codemode::CodemodeRemoteMessageVariant>) {
    return support::AsyncResult<codemode::CodemodeRemoteReplyVariant>{
            support::Expected<codemode::CodemodeRemoteReplyVariant>{codemode::CodemodeRemoteNullReply{}}};
}

/// Case 2's caller: it records that the run reported a call, then never answers.
/// The pending reply is a producer that drops its completion, so only the run's
/// stop token can end `drive()`.
struct CallerWitness {
    std::atomic<bool> call_reported{false};
    std::counting_semaphore<> changed{0};
};

[[nodiscard]] support::AsyncResult<codemode::CodemodeRemoteReplyVariant> never_answer(
        std::shared_ptr<CallerWitness> caller, std::vector<codemode::CodemodeRemoteMessageVariant> messages) {
    for (auto& message : messages) {
        if (std::holds_alternative<codemode::CodemodeRemoteCallMessage>(message)) {
            caller->call_reported.store(true);
            caller->changed.release();
        }
    }
    return support::AsyncResult<codemode::CodemodeRemoteReplyVariant>{
            support::AsyncProducer<codemode::CodemodeRemoteReplyVariant, support::Error>{
                    [](support::AsyncCompletion<codemode::CodemodeRemoteReplyVariant, support::Error>) noexcept {}}};
}

/// Case 1's observations, recorded before the first assertion.
struct JoinObservation {
    bool worker_parked{false};
    bool join_started{false};
    bool join_thread_is_serve_thread{false};
    bool join_thread_differs_from_worker{false};
    bool join_finished{false};
    bool state_alive_after_join{false};
    std::size_t owners_after_join{0};
    bool serve_completed{false};
    bool serve_completed_ok{false};
    bool serve_completed_before_release{false};
    bool serve_completed_after_join{false};
};

/// Case 2's observations, recorded before the first assertion.
struct AbandonObservation {
    bool worker_parked_in_drive{false};
    bool call_reported{false};
    bool stop_requested_before_release{false};
    bool stop_requested_after_release{false};
    bool frame_destroyed{false};
    bool join_started{false};
    bool join_thread_is_destroying_thread{false};
    bool join_thread_differs_from_worker{false};
    bool join_finished{false};
    bool drive_returned{false};
    bool state_alive_after_join{false};
    bool state_released_after_destroy{false};
    bool serve_completed{false};
    std::chrono::steady_clock::duration abandon_elapsed{};
};

/// One context, its serve coroutine, and the thread that pumps it. The serve's
/// own `AsyncResult` completion is written to the gate, so both cases read the
/// same completion witness.
[[nodiscard]] std::thread spawn_serve(boost::asio::io_context& context,
        support::AsyncResult<void> operation,
        std::shared_ptr<Gate> gate,
        std::atomic<std::thread::id>& serve_thread) {
    boost::asio::co_spawn(
            context,
            [operation = std::move(operation), gate]() mutable -> boost::asio::awaitable<void> {
                auto outcome = co_await support::detail::await_async_result(std::move(operation));
                gate->serve_completed_ok.store(outcome.has_value());
                gate->serve_completed.store(true);
                gate->changed.release();
            },
            boost::asio::detached);
    // std::thread, not std::jthread: io_context::run() does not observe a stop
    // token, and every case stops the context, joins this thread and releases
    // its gate before any assertion can fail.
    return std::thread([&context, &serve_thread] {
        serve_thread.store(std::this_thread::get_id());
        context.run();
    });
}

} // namespace

TEST_CASE("Remote deterministic worker is joined by the serve frame, never by the worker",
        "[codemode][remote][deterministic][issue1232]") {
    GateInstall install;
    const auto gate = install.gate;
    JoinObservation observation;

    codemode::CodemodeServeOptions options;
    options.wasm_path = guest_wasm();
    auto served = codemode::serve_codemode_remote(deterministic_request("return 2"), answer_without_result, options);

    // Drive the serve on its own context thread: the fixed shape blocks that
    // thread in the frame's join until this case releases the parked worker.
    boost::asio::io_context context;
    std::atomic<std::thread::id> serve_thread{};
    std::thread driver = spawn_serve(context, std::move(served), gate, serve_thread);

    // 1. The worker reached the gate: `drive()` returned and its closure still
    //    holds the state.
    observation.worker_parked = wait_for_gate(gate, [](const Gate& g) { return g.parked.load(); });
    // 2. Either the serve frame joins that parked worker (fixed shape) or the
    //    serve completes with no join at all (the recorded defect). This is the
    //    transition the case exists for, so the wait ends on the observation.
    (void)wait_for_gate(gate, [](const Gate& g) { return g.join_started.load() || g.serve_completed.load(); });
    observation.join_started = gate->join_started.load();
    observation.join_thread_differs_from_worker =
            observation.join_started && gate->join_thread.load() != gate->worker_thread.load();
    observation.join_thread_is_serve_thread =
            observation.join_started && gate->join_thread.load() == serve_thread.load();
    observation.serve_completed_before_release = gate->serve_completed.load();
    // 3. Release the gate. The worker cannot retire while it is held; the
    //    release is unconditional so even a timed-out wait reaches the join
    //    below instead of leaving the driver blocked forever.
    gate->release.release();
    // 4. Read the retirement back, then let the driving context finish. No
    //    assertion runs before the release and the join. The join and the
    //    serve's completion are one wait so the case never adds a fourth
    //    timeout budget on the failure path.
    observation.join_finished =
            wait_for_gate(gate, [](const Gate& g) { return g.join_finished.load() && g.serve_completed.load(); });
    driver.join();
    observation.serve_completed = gate->serve_completed.load();
    observation.serve_completed_ok = gate->serve_completed_ok.load();
    observation.serve_completed_after_join = observation.serve_completed && observation.join_finished;
    // The join witness is what makes the ownership claim causal: when the join
    // returned, the worker's thread had ended, so the state could only be alive
    // because the frame — executing that destructor — still owned it.
    observation.state_alive_after_join = gate->state_alive_after_join.load();
    observation.owners_after_join = gate->owners_after_join.load();

    CHECK(observation.worker_parked);
    CHECK(observation.join_started);
    CHECK(observation.join_thread_differs_from_worker);
    // The frame joins from the thread that runs the serve coroutine, never from
    // the worker: a self-join is the recorded defect.
    CHECK(observation.join_thread_is_serve_thread);
    CHECK(observation.join_finished);
    // After the worker's thread ended, exactly one strong owner remained and it
    // is the frame's own `served`; the parked closure's later release is
    // therefore not the state's last use.
    CHECK(observation.state_alive_after_join);
    CHECK(observation.owners_after_join == 1);
    // The serve's own result is later than the worker's exit: the frame joins
    // before it returns, so the internal `AsyncResult` completes afterwards.
    CHECK_FALSE(observation.serve_completed_before_release);
    CHECK(observation.serve_completed);
    CHECK(observation.serve_completed_after_join);
    CHECK(observation.serve_completed_ok);
}

TEST_CASE("Remote deterministic worker is stopped and joined when an abandoned serve frame is destroyed",
        "[codemode][remote][deterministic][issue1232]") {
    GateInstall install;
    const auto gate = install.gate;
    gate->park_point.store(ParkPoint::inside_drive);
    AbandonObservation observation;
    const auto destroying_thread = std::this_thread::get_id();

    // The run reports a caller call and then waits for an answer this case never
    // sends, so `drive()` cannot return on its own.
    auto request = deterministic_request("const value = await tools.wait(1); return value;");
    request.data.tools = {
            {.name = "wait", .js_name = "wait", .description = "Wait for the caller's answer."},
    };
    codemode::CodemodeServeOptions options;
    options.wasm_path = guest_wasm();
    auto caller = std::make_shared<CallerWitness>();
    auto context = std::make_unique<boost::asio::io_context>();
    std::atomic<std::thread::id> serve_thread{};
    std::thread driver = spawn_serve(*context,
            codemode::serve_codemode_remote(
                    request,
                    [caller](std::vector<codemode::CodemodeRemoteMessageVariant> messages) {
                        return never_answer(caller, std::move(messages));
                    },
                    options),
            gate,
            serve_thread);

    // 1. The worker reached the caller's call inside `drive()` and parked there.
    observation.worker_parked_in_drive = wait_for_gate(gate, [](const Gate& g) { return g.parked_in_drive.load(); });
    observation.call_reported = wait_until(caller->changed, [&] { return caller->call_reported.load(); });
    observation.stop_requested_before_release = gate->stop_requested_before_park_release.load();

    // 2. The releaser is the only thread that can release the in-drive park once
    //    this thread is inside the frame's destructor. It waits for the frame's
    //    join to start, so the stop request it is evidence for has already been
    //    made; a missed signal still releases the park, and the case then reads
    //    the missing join from the recorded observations instead of hanging.
    std::thread releaser([gate] {
        (void)wait_for_gate(gate, [](const Gate& g) { return g.join_started.load() || g.serve_completed.load(); });
        gate->release_in_drive.release();
    });

    // 3. Abandon the frame: stop the context, let its run return, then destroy
    //    the context. Destroying it destroys the suspended serve frame, whose
    //    scope owner requests stop on the unfinished run and joins the worker.
    const auto abandon_started = std::chrono::steady_clock::now();
    context->stop();
    driver.join();
    context.reset();
    observation.abandon_elapsed = std::chrono::steady_clock::now() - abandon_started;
    observation.frame_destroyed = true;
    releaser.join();

    // 4. Read everything back. No assertion runs while the worker, the driver or
    //    the releaser is live.
    observation.stop_requested_after_release = gate->stop_requested_after_park_release.load();
    observation.join_started = gate->join_started.load();
    observation.join_thread_differs_from_worker =
            observation.join_started && gate->join_thread.load() != gate->worker_thread.load();
    observation.join_thread_is_destroying_thread =
            observation.join_started && gate->join_thread.load() == destroying_thread;
    observation.join_finished = gate->join_finished.load();
    observation.drive_returned = gate->drive_returned.load();
    observation.state_alive_after_join = gate->state_alive_after_join.load();
    observation.state_released_after_destroy = gate->state.expired();
    observation.serve_completed = gate->serve_completed.load();

    CHECK(observation.worker_parked_in_drive);
    CHECK(observation.call_reported);
    // The request did not exist while the worker sat in `drive()`.
    CHECK_FALSE(observation.stop_requested_before_release);
    CHECK(observation.frame_destroyed);
    // The frame destroyed its scope owner (no detach) and joined the worker from
    // the destroying thread, not from the worker.
    CHECK(observation.join_started);
    CHECK(observation.join_thread_differs_from_worker);
    CHECK(observation.join_thread_is_destroying_thread);
    CHECK(observation.join_finished);
    // The worker left `drive()` while the frame was being destroyed, and the only
    // thing that could have ended the run is the stop request the destruction
    // made: the caller never answered.
    CHECK(observation.stop_requested_after_release);
    CHECK(observation.drive_returned);
    // The state outlived the worker's thread and was released when the frame was
    // destroyed, so the frame owned it across the whole join.
    CHECK(observation.state_alive_after_join);
    CHECK(observation.state_released_after_destroy);
    // An abandoned frame delivers no terminal outcome to its consumer.
    CHECK_FALSE(observation.serve_completed);
    // The whole abandonment is bounded well below the 60 s CTest timeout.
    CHECK(observation.abandon_elapsed < kBoundedAbandon);
}

/// Case 3's observations, recorded before the first assertion.
struct DeliveryWaitObservation {
    bool loop_top_parked{false};
    bool call_reported{false};
    bool frame_destroyed{false};
    bool stop_request_pending{false};
    bool delivery_wait_parked{false};
    bool delivery_wait_released{false};
    bool stop_notify_entered{false};
    bool stop_notify_mutex_acquired{false};
    bool stop_notify_notified{false};
    bool frame_stop_requested{false};
    bool join_started{false};
    bool join_thread_is_destroying_thread{false};
    bool join_thread_differs_from_worker{false};
    bool join_finished{false};
    bool drive_returned{false};
    bool state_alive_after_join{false};
    bool state_released_after_destroy{false};
    bool serve_completed{false};
    std::uint64_t stop_request_pending_seq{0};
    std::uint64_t delivery_wait_parked_seq{0};
    std::uint64_t delivery_wait_released_seq{0};
    std::uint64_t stop_notify_entered_seq{0};
    std::uint64_t stop_notify_mutex_acquired_seq{0};
    std::uint64_t stop_notify_notified_seq{0};
    std::uint64_t frame_stop_requested_seq{0};
    std::uint64_t drive_returned_seq{0};
    std::uint64_t join_started_seq{0};
    std::uint64_t join_finished_seq{0};
    std::chrono::steady_clock::duration abandon_elapsed{};
};

// Ticket #1232 cross-repair case: the frame-local worker requests stop and
// joins on destruction, but a run already parked in the delivery loop's
// `cv.wait` for a caller answer can only be woken by the run's `stop_notify`
// callback. The parks below order three threads into that interleaving; they do
// not invent a state. The run sits between the delivery loop's top and its wait,
// and the serve frame sits between the `finished()` check and the
// `request_stop()` that follow it. Those are two separate `shared.mutex`
// acquisitions, so the run can enter the wait between them on its own; the
// request then lands inside `cv.wait`'s predicate, which holds the mutex from
// the predicate's evaluation until the waiter is enqueued.
//
// With the callback paired to `shared.mutex` it cannot run inside that hold, so
// its notification is delivered after the waiter is enqueued. Without the
// pairing it notifies into the window, the wake is lost, the run never settles
// and the frame's join never returns: the recorded red is a CTest timeout at
// the case's 30 s bound, with the process still alive on its two threads.
TEST_CASE("An abandoned serve frame stops and joins a deterministic run parked in the caller-answer wait",
        "[codemode][remote][deterministic][issue1232][issue1232wake]") {
    GateInstall install;
    const auto gate = install.gate;
    gate->park_point.store(ParkPoint::delivery_wait);
    DeliveryWaitObservation observation;
    const auto destroying_thread = std::this_thread::get_id();

    // The run reports a caller call and then waits for an answer this case never
    // sends, so `drive()` cannot settle on its own and the run's stop token is
    // the only thing that can end it.
    auto request = deterministic_request("const value = await tools.wait(1); return value;");
    request.data.tools = {
            {.name = "wait", .js_name = "wait", .description = "Wait for the caller's answer."},
    };
    codemode::CodemodeServeOptions options;
    options.wasm_path = guest_wasm();
    auto caller = std::make_shared<CallerWitness>();
    auto context = std::make_unique<boost::asio::io_context>();
    std::atomic<std::thread::id> serve_thread{};
    std::thread driver = spawn_serve(*context,
            codemode::serve_codemode_remote(
                    request,
                    [caller](std::vector<codemode::CodemodeRemoteMessageVariant> messages) {
                        return never_answer(caller, std::move(messages));
                    },
                    options),
            gate,
            serve_thread);

    // 1. The run issued the call and reached the delivery loop's top.
    observation.loop_top_parked = wait_for_gate(gate, [](const Gate& g) { return g.loop_top_parked.load(); });
    observation.call_reported = wait_until(caller->changed, [&] { return caller->call_reported.load(); });

    // 2. The sequencer walks the frame's destruction into the wait window and
    //    then releases each park in turn. Every wait is bounded, so a missing
    //    signal reaches the assertions instead of hanging here; a genuinely
    //    unbounded join is left to the case's CTest timeout, which is the
    //    recorded defect.
    std::thread sequencer([gate] {
        (void)wait_for_gate(gate, [](const Gate& g) { return g.stop_request_pending.load(); });
        gate->release_loop_top.release();
        (void)wait_for_gate(gate, [](const Gate& g) { return g.delivery_wait_parked.load(); });
        gate->release_stop_request.release();
        (void)wait_for_gate(gate, [](const Gate& g) { return g.stop_notify_entered.load(); });
        gate->release_delivery_wait.release();
    });

    // 3. Abandon the frame: stop the context, let its run return, then destroy
    //    the context. Destroying it destroys the suspended serve frame, whose
    //    scope owner requests stop on the unfinished run and joins the worker
    //    from this thread.
    const auto abandon_started = std::chrono::steady_clock::now();
    context->stop();
    driver.join();
    context.reset();
    observation.abandon_elapsed = std::chrono::steady_clock::now() - abandon_started;
    observation.frame_destroyed = true;
    sequencer.join();

    // 4. Read everything back. No assertion runs while the worker, the driver or
    //    the sequencer is live.
    observation.stop_request_pending = gate->stop_request_pending.load();
    observation.delivery_wait_parked = gate->delivery_wait_parked.load();
    observation.delivery_wait_released = gate->delivery_wait_left_park.load();
    observation.stop_notify_entered = gate->stop_notify_entered.load();
    observation.stop_notify_mutex_acquired = gate->stop_notify_mutex_acquired.load();
    observation.stop_notify_notified = gate->stop_notify_notified.load();
    observation.frame_stop_requested = gate->frame_stop_requested.load();
    observation.join_started = gate->join_started.load();
    observation.join_thread_differs_from_worker =
            observation.join_started && gate->join_thread.load() != gate->worker_thread.load();
    observation.join_thread_is_destroying_thread =
            observation.join_started && gate->join_thread.load() == destroying_thread;
    observation.join_finished = gate->join_finished.load();
    observation.drive_returned = gate->drive_returned.load();
    observation.state_alive_after_join = gate->state_alive_after_join.load();
    observation.state_released_after_destroy = gate->state.expired();
    observation.serve_completed = gate->serve_completed.load();
    observation.stop_request_pending_seq = gate->stop_request_pending_seq.load();
    observation.delivery_wait_parked_seq = gate->delivery_wait_parked_seq.load();
    observation.delivery_wait_released_seq = gate->delivery_wait_released_seq.load();
    observation.stop_notify_entered_seq = gate->stop_notify_entered_seq.load();
    observation.stop_notify_mutex_acquired_seq = gate->stop_notify_mutex_acquired_seq.load();
    observation.stop_notify_notified_seq = gate->stop_notify_notified_seq.load();
    observation.frame_stop_requested_seq = gate->frame_stop_requested_seq.load();
    observation.drive_returned_seq = gate->drive_returned_seq.load();
    observation.join_started_seq = gate->join_started_seq.load();
    observation.join_finished_seq = gate->join_finished_seq.load();

    INFO("frame destruction took "
            << std::chrono::duration_cast<std::chrono::microseconds>(observation.abandon_elapsed).count() << " us");

    CHECK(observation.loop_top_parked);
    CHECK(observation.call_reported);
    CHECK(observation.frame_destroyed);
    // (a) The frame's scope owner requested stop on the run it had just found
    //     unfinished, and the run was parked in the caller-answer wait by then.
    CHECK(observation.stop_request_pending);
    CHECK(observation.frame_stop_requested);
    CHECK(observation.frame_stop_requested_seq > observation.stop_request_pending_seq);
    CHECK(observation.delivery_wait_parked);
    CHECK(observation.delivery_wait_released);
    CHECK(observation.stop_request_pending_seq < observation.delivery_wait_released_seq);
    // (b) The wake went through Deterministic.cpp's `stop_notify` callback, and
    //     the callback could acquire `shared.mutex` only after the wait
    //     predicate released it: that pairing is what carries the wake to an
    //     enqueued waiter instead of notifying into the window.
    CHECK(observation.stop_notify_entered);
    CHECK(observation.stop_notify_entered_seq > observation.delivery_wait_parked_seq);
    CHECK(observation.stop_notify_entered_seq < observation.delivery_wait_released_seq);
    CHECK(observation.stop_notify_mutex_acquired);
    CHECK(observation.stop_notify_mutex_acquired_seq > observation.delivery_wait_released_seq);
    CHECK(observation.stop_notify_notified);
    CHECK(observation.stop_notify_notified_seq > observation.stop_notify_mutex_acquired_seq);
    // The worker left `drive()` after that notification, and nothing else could
    // have woken it: the caller never answered.
    CHECK(observation.drive_returned);
    CHECK(observation.drive_returned_seq > observation.stop_notify_notified_seq);
    // (c) The frame destroyed its scope owner (no detach) and joined the worker
    //     from the destroying thread, never from the worker.
    CHECK(observation.join_started);
    CHECK(observation.join_thread_differs_from_worker);
    CHECK(observation.join_thread_is_destroying_thread);
    CHECK(observation.join_finished);
    CHECK(observation.join_finished_seq > observation.drive_returned_seq);
    // The state outlived the worker's thread and was released when the frame was
    // destroyed, so the frame owned it across the whole join.
    CHECK(observation.state_alive_after_join);
    CHECK(observation.state_released_after_destroy);
    // An abandoned frame delivers no terminal outcome to its consumer.
    CHECK_FALSE(observation.serve_completed);
    // (d) The whole abandonment is bounded far below the case's 30 s timeout.
    CHECK(observation.abandon_elapsed < kBoundedAbandon);
}
