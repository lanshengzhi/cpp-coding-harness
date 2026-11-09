#define CCH_REMOTE_RETIREMENT_TESTING
#include "codemode/RemoteRetirement.hpp"

#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/use_future.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <functional>
#include <future>
#include <optional>
#include <semaphore>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <utility>

using namespace std::chrono_literals;
using cch::codemode::detail::RemoteOperation;
using cch::codemode::detail::RemoteRetirement;

TEST_CASE("Remote retirement waits for an executing stop callback", "[codemode][retirement][issue1232]") {
    RemoteRetirement operation;
    auto participant = operation.admit();
    REQUIRE(participant);
    auto observer = operation.observe();
    std::binary_semaphore entered(0), release(0);
    auto callback = participant->on_stop([&] {
        entered.release();
        (void)release.try_acquire_for(2s);
    });
    std::thread stopper([&] { operation.request_stop(); });
    const bool entered_in_time = entered.try_acquire_for(2s);
    participant->retire_self();
    const bool prematurely_ready = observer.ready();
    release.release();
    stopper.join();
    callback.reset();
    CHECK(entered_in_time);
    CHECK_FALSE(prematurely_ready);
    CHECK(observer.ready());
}

TEST_CASE("Remote cooperative retirement remains independent of the result", "[codemode][retirement][issue1232]") {
    // A legacy result has no retirement capability, even when it is ready.
    std::promise<int> legacy_result;
    RemoteOperation<std::future<int>> legacy{legacy_result.get_future(), std::nullopt};
    legacy_result.set_value(42);
    REQUIRE(legacy.result.get() == 42);
    REQUIRE(!legacy.retirement);

    RemoteRetirement operation;
    auto observer = operation.observe();
    auto participant = operation.admit();
    REQUIRE(participant);
    std::promise<int> result;
    RemoteOperation<std::future<int>> exchange{result.get_future(), observer};
    result.set_value(17);
    REQUIRE(exchange.result.get() == 17);
    REQUIRE(!exchange.retirement->ready());
    auto child = participant->admit_child();
    REQUIRE(child);
    operation.close_admission();
    REQUIRE(!operation.admit());
    operation.request_stop();
    operation.request_stop();
    REQUIRE(participant->stop_requested());
    REQUIRE(child->stop_requested());
    REQUIRE(!observer.ready());
    participant->retire_self();
    REQUIRE(!participant->admit_child());
    REQUIRE(!observer.ready());
    std::thread foreign([child = std::move(*child)]() mutable { child.retire_self(); });
    foreign.join();
    REQUIRE(observer.wait_for(100ms));

    // Dropping an unretired participant never invents a successful witness.
    RemoteRetirement::Observer pending = [] {
        RemoteRetirement stalled;
        auto witness = stalled.observe();
        auto abandoned = stalled.admit();
        REQUIRE(abandoned);
        stalled.close_admission();
        return witness;
    }();
    REQUIRE(!pending.wait_for(10ms));

    // Stop callbacks may synchronously re-enter admission without deadlock.
    RemoteRetirement reentrant;
    auto existing = reentrant.admit();
    REQUIRE(existing);
    bool called = false;
    bool rejected = false;
    auto callback = existing->on_stop([&] {
        called = true;
        rejected = !reentrant.admit();
    });
    reentrant.request_stop();
    REQUIRE(called);
    REQUIRE(rejected);
    existing->retire_self();
    callback.reset();
    REQUIRE(reentrant.observe().ready());

    // Both outcomes of a racing admission/stop are valid; an admitted
    // participant must receive stop and keep the witness pending until exit.
    for (int i = 0; i < 200; ++i) {
        RemoteRetirement raced;
        std::optional<RemoteRetirement::Participant> admitted;
        std::thread starter([&] {
            auto participant = raced.admit();
            if (participant) admitted.emplace(std::move(*participant));
        });
        raced.request_stop();
        starter.join();
        if (admitted) {
            REQUIRE(admitted->stop_requested());
            REQUIRE(!raced.observe().ready());
            admitted->retire_self();
        }
        REQUIRE(raced.observe().ready());
    }
}

TEST_CASE("Remote retirement covers immediate callbacks and children admitted after closure",
        "[codemode][retirement][issue1232]") {
    RemoteRetirement operation;
    auto parent = operation.admit();
    REQUIRE(parent);
    operation.request_stop();
    auto child = parent->admit_child();
    REQUIRE(child);
    auto observer = operation.observe();
    std::binary_semaphore entered(0), release(0);
    std::thread worker([child = std::move(*child), &entered, &release]() mutable {
        auto callback = child.on_stop([&] {
            entered.release();
            (void)release.try_acquire_for(2s);
        });
        callback.reset();
        child.retire_self();
    });
    const bool entered_in_time = entered.try_acquire_for(2s);
    parent->retire_self();
    const bool premature = observer.ready();
    release.release();
    worker.join();
    CHECK(entered_in_time);
    CHECK_FALSE(premature);
    CHECK(observer.ready());
}

TEST_CASE("Remote retirement waits for callback capture destruction", "[codemode][retirement][issue1232]") {
    struct Tail {
        std::binary_semaphore& entered;
        std::binary_semaphore& release;
        ~Tail() {
            entered.release();
            (void)release.try_acquire_for(2s);
        }
    };
    RemoteRetirement operation;
    auto participant = operation.admit();
    REQUIRE(participant);
    auto observer = operation.observe();
    std::binary_semaphore entered(0), release(0);
    auto resource = std::make_unique<Tail>(Tail{entered, release});
    auto callback = participant->on_stop([resource = std::move(resource)] {});
    operation.close_admission();
    participant->retire_self();
    std::thread destroyer([callback = std::move(callback)]() mutable { callback.reset(); });
    const bool entered_in_time = entered.try_acquire_for(2s);
    const bool premature = observer.ready();
    release.release();
    destroyer.join();
    CHECK(entered_in_time);
    CHECK_FALSE(premature);
    CHECK(observer.ready());
}

TEST_CASE("Remote retirement wakes multiple asynchronous observations", "[codemode][retirement][issue1232]") {
    RemoteRetirement operation;
    auto participant = operation.admit();
    REQUIRE(participant);
    auto observer = operation.observe();
    auto first = observer.async_wait();
    auto second = operation.observe().async_wait();
    {
        auto discarded = observer.async_wait();
    }
    operation.close_admission();
    const auto before = first.wait_for(0ms);
    std::promise<std::future_status> result;
    auto received = result.get_future();
    std::thread waiter([wait = std::move(first), &result] { result.set_value(wait.wait_for(2s)); });
    participant->retire_self();
    const auto received_status = received.wait_for(3s);
    const auto woken = received_status == std::future_status::ready ? received.get() : std::future_status::timeout;
    waiter.join();
    CHECK(before == std::future_status::timeout);
    CHECK(woken == std::future_status::ready);
    CHECK(second.wait_for(0ms) == std::future_status::ready);
}

TEST_CASE("Remote retirement retains a self-unregistered running callback", "[codemode][retirement][issue1232]") {
    RemoteRetirement operation;
    auto participant = operation.admit();
    REQUIRE(participant);
    auto observer = operation.observe();
    std::binary_semaphore entered(0), release(0);
    std::function<void()> unregister;
    auto callback = participant->on_stop([&] {
        unregister();
        participant->retire_self();
        entered.release();
        (void)release.try_acquire_for(2s);
    });
    unregister = [&] { callback.reset(); };
    std::thread stopper([&] { operation.request_stop(); });
    bool entered_in_time = entered.try_acquire_for(2s);
    bool pending = !observer.ready();
    release.release();
    stopper.join();
    CHECK(entered_in_time);
    CHECK(pending);
    CHECK(observer.ready());
}

TEST_CASE("Remote retirement retains an immediate registration callback", "[codemode][retirement][issue1232]") {
    RemoteRetirement operation;
    auto participant = operation.admit();
    REQUIRE(participant);
    auto observer = operation.observe();
    operation.request_stop();
    std::binary_semaphore entered(0), release(0);
    std::thread registrar([&] {
        auto callback = participant->on_stop([&] {
            participant->retire_self();
            entered.release();
            (void)release.try_acquire_for(2s);
        });
    });
    bool entered_in_time = entered.try_acquire_for(2s);
    bool pending = !observer.ready();
    release.release();
    registrar.join();
    CHECK(entered_in_time);
    CHECK(pending);
    CHECK(observer.ready());
}

TEST_CASE("Remote retirement retains callback construction during reentrant retirement",
        "[codemode][retirement][issue1232]") {
    struct Reentrant {
        RemoteRetirement::Participant* participant;
        RemoteRetirement::Observer* observer;
        bool* pending;
        Reentrant(RemoteRetirement::Participant* p, RemoteRetirement::Observer* o, bool* value)
            : participant(p), observer(o), pending(value) {}
        Reentrant(const Reentrant& other) : Reentrant(other.participant, other.observer, other.pending) {
            participant->retire_self();
            *pending = *pending && !observer->ready();
        }
        Reentrant(Reentrant&& other) : Reentrant(other.participant, other.observer, other.pending) {
            participant->retire_self();
            *pending = *pending && !observer->ready();
        }
        void operator()() const noexcept {}
    };
    RemoteRetirement operation;
    auto participant = operation.admit();
    REQUIRE(participant);
    auto observer = operation.observe();
    operation.close_admission();
    bool pending = true;
    Reentrant function(&*participant, &observer, &pending);
    auto callback = participant->on_stop(function);
    const bool pending_with_callback = !observer.ready();
    callback.reset();
    CHECK(pending);
    CHECK(pending_with_callback);
    CHECK(observer.ready());
}

TEST_CASE("Remote retirement suspends on a single Asio executor", "[codemode][retirement][issue1232]") {
    RemoteRetirement operation;
    auto participant = operation.admit();
    REQUIRE(participant);
    auto observer = operation.observe();
    boost::asio::io_context context;
    auto completed = boost::asio::co_spawn(
            context, RemoteRetirement::Observer::async_wait_on_executor(observer), boost::asio::use_future);
    boost::asio::post(context, [&] {
        operation.close_admission();
        participant->retire_self();
    });
    context.run_for(2s);
    const bool ready = completed.wait_for(0ms) == std::future_status::ready;
    const bool succeeded = ready && completed.get();
    context.stop();
    CHECK(ready);
    CHECK(succeeded);
}

TEST_CASE("Remote retirement posts an immediate notification to a waiting executor",
        "[codemode][retirement][issue1232]") {
    RemoteRetirement operation;
    auto participant = operation.admit();
    REQUIRE(participant);
    boost::asio::io_context context;
    auto result = boost::asio::co_spawn(
            context, RemoteRetirement::Observer::async_wait_on_executor(operation.observe()), boost::asio::use_future);
    (void)context.poll();
    operation.close_admission();
    participant->retire_self();
    (void)context.poll();
    const bool ready = result.wait_for(0ms) == std::future_status::ready;
    const bool success = ready && result.get();
    context.stop();
    CHECK(ready);
    CHECK(success);
}

TEST_CASE("Remote retirement posts to multiple executor observers from another thread",
        "[codemode][retirement][issue1232]") {
    RemoteRetirement operation;
    auto participant = operation.admit();
    REQUIRE(participant);
    boost::asio::io_context context;
    auto observer = operation.observe();
    auto first = boost::asio::co_spawn(
            context, RemoteRetirement::Observer::async_wait_on_executor(observer), boost::asio::use_future);
    auto second = boost::asio::co_spawn(
            context, RemoteRetirement::Observer::async_wait_on_executor(observer), boost::asio::use_future);
    (void)context.poll();
    std::jthread retire([&] {
        operation.close_admission();
        participant->retire_self();
    });
    retire.join();
    (void)context.poll();
    const bool first_ready = first.wait_for(0ms) == std::future_status::ready;
    const bool second_ready = second.wait_for(0ms) == std::future_status::ready;
    const bool first_success = first_ready && first.get();
    const bool second_success = second_ready && second.get();
    CHECK(first_success);
    CHECK(second_success);
}

TEST_CASE("Remote retirement cancellation is not a retirement witness", "[codemode][retirement][issue1232]") {
    RemoteRetirement operation;
    auto participant = operation.admit();
    REQUIRE(participant);
    boost::asio::io_context context;
    boost::asio::cancellation_signal signal;
    auto result = boost::asio::co_spawn(context,
            RemoteRetirement::Observer::async_wait_on_executor(operation.observe()),
            boost::asio::bind_cancellation_slot(signal.slot(), boost::asio::use_future));
    (void)context.poll();
    signal.emit(boost::asio::cancellation_type::terminal);
    (void)context.poll();
    const bool completed = result.wait_for(0ms) == std::future_status::ready;
    const bool witnessed = completed && result.get();
    operation.close_admission();
    participant->retire_self();
    context.stop();
    CHECK(completed);
    CHECK_FALSE(witnessed);
}

TEST_CASE(
        "Remote retirement retains a notification while the executor is stopped", "[codemode][retirement][issue1232]") {
    RemoteRetirement operation;
    auto participant = operation.admit();
    REQUIRE(participant);
    boost::asio::io_context context;
    auto result = boost::asio::co_spawn(
            context, RemoteRetirement::Observer::async_wait_on_executor(operation.observe()), boost::asio::use_future);
    (void)context.poll();
    context.stop();
    operation.close_admission();
    participant->retire_self();
    const bool before_restart = result.wait_for(0ms) == std::future_status::ready;
    context.restart();
    (void)context.poll();
    const bool after_restart = result.wait_for(0ms) == std::future_status::ready;
    const bool succeeded = after_restart && result.get();
    CHECK_FALSE(before_restart);
    CHECK(succeeded);
}

TEST_CASE("Remote retirement tolerates a destroyed waiting executor", "[codemode][retirement][issue1232]") {
    RemoteRetirement operation;
    auto participant = operation.admit();
    REQUIRE(participant);
    {
        boost::asio::io_context context;
        auto result = boost::asio::co_spawn(context,
                RemoteRetirement::Observer::async_wait_on_executor(operation.observe()),
                boost::asio::use_future);
        (void)context.poll();
        context.stop();
    }
    operation.close_admission();
    participant->retire_self();
    CHECK(operation.observe().ready());
}

TEST_CASE("Remote retirement completes an observer registered after publication", "[codemode][retirement][issue1232]") {
    static int late_posts = 0;
    late_posts = 0;
    RemoteRetirement operation(+[] { ++late_posts; });
    auto participant = operation.admit();
    REQUIRE(participant);
    operation.close_admission();
    participant->retire_self();
    REQUIRE(operation.observe().ready());
    boost::asio::io_context context;
    auto result = boost::asio::co_spawn(
            context, RemoteRetirement::Observer::async_wait_on_executor(operation.observe()), boost::asio::use_future);
    (void)context.poll();
    const bool ready = result.wait_for(0ms) == std::future_status::ready;
    const bool succeeded = ready && result.get();
    context.stop();
    CHECK(ready);
    CHECK(succeeded);
    // A late registration reports the visible publication; it needs no post.
    CHECK(late_posts == 0);
}

TEST_CASE("Remote retirement notification may reenter observer", "[codemode][retirement][issue1232]") {
    static std::function<void()> hook;
    RemoteRetirement operation(+[] { hook(); });
    auto participant = operation.admit();
    REQUIRE(participant);
    auto observer = operation.observe();
    hook = [&] { (void)observer.delivery_failures(); };
    boost::asio::io_context context;
    auto result = boost::asio::co_spawn(
            context, RemoteRetirement::Observer::async_wait_on_executor(observer), boost::asio::use_future);
    (void)context.poll();
    operation.close_admission();
    participant->retire_self();
    (void)context.poll();
    REQUIRE(result.wait_for(0ms) == std::future_status::ready);
    CHECK(result.get());
    hook = {};
}

#if defined(__cpp_exceptions)
TEST_CASE("Remote retirement reports a failed post and continues notifying", "[codemode][retirement][issue1232]") {
    static int posts = 0;
    posts = 0;
    RemoteRetirement operation(+[] {
        if (++posts == 1) throw std::runtime_error("injected post failure");
    });
    auto participant = operation.admit();
    REQUIRE(participant);
    auto observer = operation.observe();
    boost::asio::io_context context;
    boost::asio::cancellation_signal cancellation;
    auto first = boost::asio::co_spawn(context,
            RemoteRetirement::Observer::async_wait_on_executor(observer),
            boost::asio::bind_cancellation_slot(cancellation.slot(), boost::asio::use_future));
    auto second = boost::asio::co_spawn(
            context, RemoteRetirement::Observer::async_wait_on_executor(observer), boost::asio::use_future);
    (void)context.poll();
    auto callback = participant->on_stop([] {});
    operation.close_admission();
    participant->retire_self();
    callback.reset(); // Publication occurs in the lease destructor.
    (void)context.poll();
    const bool first_pending = first.wait_for(0ms) != std::future_status::ready;
    const bool second_ready = second.wait_for(0ms) == std::future_status::ready;
    const bool second_succeeded = second_ready && second.get();
    const auto failures = observer.delivery_failures();
    cancellation.emit(boost::asio::cancellation_type::terminal);
    (void)context.poll();
    const bool first_cancelled = first.wait_for(0ms) == std::future_status::ready && !first.get();
    CHECK(observer.ready());
    CHECK(first_pending);
    CHECK(second_succeeded);
    CHECK(failures == 1);
    CHECK(first_cancelled);
}

TEST_CASE("Remote retirement rolls back a throwing callback copy", "[codemode][retirement][issue1232]") {
    struct ThrowingCopy {
        ThrowingCopy() = default;
        ThrowingCopy(const ThrowingCopy&) { throw std::runtime_error("copy"); }
        ThrowingCopy(ThrowingCopy&&) = default;
        void operator()() const noexcept {}
    };
    RemoteRetirement operation;
    auto participant = operation.admit();
    REQUIRE(participant);
    ThrowingCopy function;
    bool threw = false;
    try {
        (void)participant->on_stop(function);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    operation.close_admission();
    participant->retire_self();
    CHECK(threw);
    CHECK(operation.observe().ready());
}
TEST_CASE("Remote retirement rolls back each throwing callback move", "[codemode][retirement][issue1232]") {
    struct ThrowingMove {
        int* moves;
        int fail_at;
        ThrowingMove(int& count, int fail) : moves(&count), fail_at(fail) {}
        ThrowingMove(const ThrowingMove&) = delete;
        ThrowingMove(ThrowingMove&& other) : moves(other.moves), fail_at(other.fail_at) {
            if (++*moves == fail_at) throw std::runtime_error("move");
        }
        void operator()() const noexcept {}
    };
    for (bool stopped : {false, true}) {
        for (int failure = 1; failure <= 2; ++failure) {
            RemoteRetirement operation;
            auto participant = operation.admit();
            REQUIRE(participant);
            if (stopped) operation.request_stop();
            int moves = 0;
            bool threw = false;
            try {
                (void)participant->on_stop(ThrowingMove(moves, failure));
            } catch (const std::runtime_error&) {
                threw = true;
            }
            operation.close_admission();
            participant->retire_self();
            CHECK(threw);
            CHECK(operation.observe().ready());
        }
    }
}
#endif

TEST_CASE("Remote retirement publishes callback writes before waking observers", "[codemode][retirement][issue1232]") {
    RemoteRetirement operation;
    auto participant = operation.admit();
    REQUIRE(participant);
    auto wait = operation.observe().async_wait();
    int published = 0;
    auto callback = participant->on_stop([&] { published = 42; });
    std::thread worker([&] {
        operation.request_stop();
        callback.reset();
        participant->retire_self();
    });
    const auto status = wait.wait_for(2s);
    // Reading the non-atomic value is safe only after the ready synchronization.
    const int observed = status == std::future_status::ready ? published : 0;
    worker.join();
    CHECK(status == std::future_status::ready);
    CHECK(observed == 42);
}
