// Ticket #1098 (spec #1044, ADR 0075): Generic Remote exchange and Linux host.
// Acceptance criteria:
// 1. Real nested calls across multi-turn exchange; large console output batched
//    (not one exchange round per output item).
// 2. Abort sends stop and settles result/close without waiting for actual remote
//    worker exit; different transport/capability failures retain error taxonomy.
// 3. Verified through public caller: cch::codemode::RemoteCodemodeSandbox and
//    cch::codemode::serve_codemode_remote.

#include <cch/codemode/Remote.hpp>
#include <cch/codemode/Sandbox.hpp>

#include "support/AsyncResultBridge.hpp"
#include "support/Json.hpp"
#include "support/ReleaseGate.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace cch;

namespace {

template <typename T> [[nodiscard]] std::expected<T, support::Error> run_async(support::AsyncResult<T> operation) {
    boost::asio::io_context io;
    auto future = boost::asio::co_spawn(
            io,
            [operation = std::move(operation)]() mutable -> boost::asio::awaitable<std::expected<T, support::Error>> {
                co_return co_await support::detail::await_async_result(std::move(operation));
            },
            boost::asio::use_future);
    io.run();
    return future.get();
}

/// Helper that wraps serve_codemode_remote and records message exchanges and replies.
struct RecordingRemote final : public codemode::CodemodeRemote {
    codemode::CodemodeServeOptions options;
    std::mutex mutex;
    std::mutex stop_reply_mutex;
    bool stop_reply_recorded{false};
    std::vector<std::vector<std::string>> exchange_message_kinds;
    std::vector<std::size_t> exchange_sizes;
    std::vector<std::string> reply_kinds;
    std::promise<void> stop_reply_received;
    std::vector<support::AsyncResult<void>> served;

    explicit RecordingRemote(codemode::CodemodeServeOptions opts = {}) : options(std::move(opts)) {}

    [[nodiscard]] support::AsyncResult<void> execute(
            codemode::CodemodeRemoteRequest request, codemode::CodemodeRemoteExchangeHook exchange) override {
        auto exchange_hook = std::make_shared<codemode::CodemodeRemoteExchangeHook>(std::move(exchange));
        auto wrapped_exchange = [this, exchange_hook](std::vector<codemode::CodemodeRemoteMessageVariant> messages)
                -> support::AsyncResult<codemode::CodemodeRemoteReplyVariant> {
            std::vector<std::string> kinds;
            kinds.reserve(messages.size());
            for (const auto& msg : messages) {
                std::visit(
                        [&](const auto& m) {
                            using T = std::decay_t<decltype(m)>;
                            if constexpr (std::is_same_v<T, codemode::CodemodeRemoteCallMessage>) {
                                kinds.push_back("call:" + m.name);
                            } else if constexpr (std::is_same_v<T, codemode::CodemodeRemoteOutputMessage>) {
                                kinds.push_back("output");
                            } else if constexpr (std::is_same_v<T, codemode::CodemodeRemoteDoneMessage>) {
                                kinds.push_back("done");
                            } else if constexpr (std::is_same_v<T, codemode::CodemodeRemoteCrashMessage>) {
                                kinds.push_back("crash");
                            } else if constexpr (std::is_same_v<T, codemode::CodemodeRemoteStartedMessage>) {
                                kinds.push_back("started");
                            } else if constexpr (std::is_same_v<T, codemode::CodemodeRemoteIdleMessage>) {
                                kinds.push_back("idle");
                            } else if constexpr (std::is_same_v<T, codemode::CodemodeRemoteFailureMessage>) {
                                kinds.push_back("failure");
                            }
                        },
                        msg);
            }
            {
                std::lock_guard lock(mutex);
                exchange_sizes.push_back(messages.size());
                exchange_message_kinds.push_back(std::move(kinds));
            }

            return support::detail::make_async_result(
                    [this, exchange_hook, messages = std::move(messages)]() mutable
                            -> boost::asio::awaitable<support::Expected<codemode::CodemodeRemoteReplyVariant>> {
                        auto reply =
                                co_await support::detail::await_async_result((*exchange_hook)(std::move(messages)));
                        if (reply) {
                            std::string kind = "null";
                            const bool is_stop = std::holds_alternative<codemode::CodemodeRemoteStopReply>(*reply);
                            if (std::holds_alternative<codemode::CodemodeRemoteResultReply>(*reply)) {
                                const auto& res = std::get<codemode::CodemodeRemoteResultReply>(*reply);
                                kind = std::format("result:{}", res.ok ? "true" : "false");
                            } else if (is_stop) {
                                kind = "stop";
                            }
                            {
                                std::lock_guard lock(mutex);
                                reply_kinds.push_back(std::move(kind));
                            }
                            if (is_stop) {
                                std::lock_guard lock(stop_reply_mutex);
                                if (!stop_reply_recorded) {
                                    stop_reply_recorded = true;
                                    stop_reply_received.set_value();
                                }
                            }
                        }
                        co_return reply;
                    });
        };

        return codemode::serve_codemode_remote(std::move(request), std::move(wrapped_exchange), options);
    }
};

} // namespace

TEST_CASE("RemoteCodemodeSandbox runs tools in the caller and the script on the remote", "[codemode][remote]") {
    auto remote = std::make_shared<RecordingRemote>();
    codemode::RemoteCodemodeSandboxOptions options;
    options.remote = [remote]() -> support::Expected<std::shared_ptr<codemode::CodemodeRemote>> { return remote; };

    auto created = codemode::RemoteCodemodeSandbox::create(std::move(options));
    REQUIRE(created.has_value());
    auto sandbox = std::move(*created);

    std::vector<std::string> seen_args;
    codemode::CodemodeToolCallHandler handler =
            [&](std::string_view name, std::string_view args, std::stop_token) -> support::AsyncResult<std::string> {
        if (name == "add") {
            seen_args.emplace_back(args);
            auto parsed = support::read_json(args);
            if (parsed && parsed->holds<support::JsonValue::object_t>()) {
                const auto& obj = parsed->get_object();
                if (obj.contains("a") && obj.at("a").holds<double>()) {
                    auto sum = static_cast<int>(obj.at("a").get_number()) + 1;
                    return support::AsyncResult<std::string>{support::Expected<std::string>{std::to_string(sum)}};
                }
            }
            return support::AsyncResult<std::string>{support::Expected<std::string>{"2"}};
        }
        return support::AsyncResult<std::string>{support::Expected<std::string>{
                std::unexpected(support::make_error(support::ErrorCode::Validation, "unknown tool"))}};
    };

    std::vector<codemode::CodemodeToolDescriptor> tools{
            {.name = "add", .js_name = "add", .description = "Add 1 to number"}};

    std::map<std::string, support::JsonValue> initial_store;
    initial_store.emplace("k", support::JsonValue{1.0});

    auto result = run_async(sandbox->run("text('hi'); return await tools.add({ a: 1 })",
            std::move(tools),
            {},
            {},
            std::move(handler),
            std::move(initial_store)));

    REQUIRE(result.has_value());
    CHECK_FALSE(result->error.has_value());
    CHECK(result->value_json.value_or("") == "2");
    REQUIRE(result->output.size() == 1);
    CHECK(result->output[0].data == "hi");
    REQUIRE(result->calls.size() == 1);
    CHECK(result->calls[0].name == "add");
    CHECK(result->calls[0].status == codemode::CodemodeCallRecord::Status::Ok);
    CHECK(result->calls[0].duration_ms.has_value());

    REQUIRE(seen_args.size() == 1);
    auto parsed_seen = support::read_json(seen_args[0]);
    REQUIRE(parsed_seen.has_value());
    CHECK(parsed_seen->get_object().at("a").get_number() == 1.0);

    // Verify exchange sequence matches audit-1053 remote-exchange.json:
    // Exchange 0 has ["output", "call:add"]
    // Exchange 1 has ["done"]
    // Replies: ["result:true", "null"]
    REQUIRE(remote->exchange_message_kinds.size() == 2);
    CHECK(remote->exchange_message_kinds[0] == std::vector<std::string>{"output", "call:add"});
    CHECK(remote->exchange_message_kinds[1] == std::vector<std::string>{"done"});

    REQUIRE(remote->reply_kinds.size() == 2);
    CHECK(remote->reply_kinds[0] == "result:true");
    CHECK(remote->reply_kinds[1] == "null");
}

TEST_CASE("RemoteCodemodeSandbox batches output instead of making one exchange per line", "[codemode][remote]") {
    auto remote = std::make_shared<RecordingRemote>();
    codemode::RemoteCodemodeSandboxOptions options;
    options.remote = [remote]() -> support::Expected<std::shared_ptr<codemode::CodemodeRemote>> { return remote; };

    auto created = codemode::RemoteCodemodeSandbox::create(std::move(options));
    REQUIRE(created.has_value());
    auto sandbox = std::move(*created);

    auto result = run_async(sandbox->run("for (let i = 0; i < 5000; i++) console.log(i); return 'ok'"));

    REQUIRE(result.has_value());
    CHECK_FALSE(result->error.has_value());
    CHECK(result->value_json.value_or("") == "\"ok\"");
    REQUIRE(result->output.size() == 5000);
    CHECK(result->output[4999].data == "4999");
    CHECK(result->output[4999].console == true);

    // Bounded exchange count: 5000 lines over <= 6 exchanges (1000 per batch + terminal done)
    CHECK(remote->exchange_sizes.size() <= 6);
    const std::vector<std::size_t> expected_sizes{1000, 1000, 1000, 1000, 1000, 1};
    CHECK(remote->exchange_sizes == expected_sizes);
}

TEST_CASE("serve_codemode_remote stops when an output batch receives a stop reply", "[codemode][remote]") {
    std::size_t output_count = 0;
    std::size_t terminal_count = 0;
    codemode::CodemodeRemoteExchangeHook exchange =
            [&output_count, &terminal_count](std::vector<codemode::CodemodeRemoteMessageVariant> messages)
            -> support::AsyncResult<codemode::CodemodeRemoteReplyVariant> {
        for (const auto& message : messages) {
            if (std::holds_alternative<codemode::CodemodeRemoteOutputMessage>(message)) ++output_count;
            if (std::holds_alternative<codemode::CodemodeRemoteDoneMessage>(message)) ++terminal_count;
        }
        return support::AsyncResult<codemode::CodemodeRemoteReplyVariant>{
                support::Expected<codemode::CodemodeRemoteReplyVariant>{codemode::CodemodeRemoteStopReply{}}};
    };

    codemode::CodemodeRemoteRequest request;
    request.data.code = "for (let i = 0; i < 1000; i++) console.log(i); return 'unreachable'";
    auto result = run_async(codemode::serve_codemode_remote(std::move(request), std::move(exchange)));

    REQUIRE(result.has_value());
    CHECK(output_count == 1000);
    CHECK(terminal_count == 0);
}

TEST_CASE("RemoteCodemodeSandbox caps the interrupt budget and memory at the remote's limits", "[codemode][remote]") {
    codemode::CodemodeServeOptions serve_opts;
    serve_opts.max_interrupt_budget = 1000;
    serve_opts.max_memory_limit_bytes = 8 * 1024 * 1024; // 8 MiB

    auto remote = std::make_shared<RecordingRemote>(serve_opts);
    codemode::RemoteCodemodeSandboxOptions options;
    options.remote = [remote]() -> support::Expected<std::shared_ptr<codemode::CodemodeRemote>> { return remote; };
    options.interrupt_budget = 1000000000; // Requested: 1 billion polls

    auto created = codemode::RemoteCodemodeSandbox::create(std::move(options));
    REQUIRE(created.has_value());
    auto sandbox = std::move(*created);

    // 1. Interrupt budget cap
    auto timeout_result = run_async(sandbox->run("while (true) {}"));
    REQUIRE(timeout_result.has_value());
    REQUIRE(timeout_result->error.has_value());
    CHECK(timeout_result->error->kind == codemode::CodemodeError::Kind::Timeout);
    CHECK(timeout_result->error->message == "Execution exceeded its interrupt budget of 1000");

    // 2. Memory limit cap
    auto mem_result =
            run_async(sandbox->run("const parts = []; for (;;) parts.push('x'.repeat(1 << 20) + parts.length);"));
    REQUIRE(mem_result.has_value());
    REQUIRE(mem_result->error.has_value());
    CHECK(mem_result->error->kind == codemode::CodemodeError::Kind::Script);
    CHECK(mem_result->error->name.value_or("") == "InternalError");
    CHECK(mem_result->error->message == "out of memory");
}

TEST_CASE("RemoteCodemodeSandbox tells the remote to stop when caller aborts and settles immediately",
        "[codemode][remote]") {
    auto remote = std::make_shared<RecordingRemote>();
    auto stop_reply_received = remote->stop_reply_received.get_future();
    codemode::RemoteCodemodeSandboxOptions options;
    options.remote = [remote]() -> support::Expected<std::shared_ptr<codemode::CodemodeRemote>> { return remote; };

    auto created = codemode::RemoteCodemodeSandbox::create(std::move(options));
    REQUIRE(created.has_value());
    auto sandbox = std::move(*created);

    std::promise<void> tool_started;
    auto tool_started_future = tool_started.get_future();
    std::stop_source stop_src;

    codemode::CodemodeToolCallHandler handler =
            [&](std::string_view name, std::string_view, std::stop_token signal) -> support::AsyncResult<std::string> {
        if (name == "hang") {
            tool_started.set_value();
            // Hang until signal cancels
            return support::detail::make_async_result(
                    [signal]() -> boost::asio::awaitable<support::Expected<std::string>> {
                        auto executor = co_await boost::asio::this_coro::executor;
                        boost::asio::steady_timer timer(executor);
                        while (!signal.stop_requested()) {
                            timer.expires_after(std::chrono::milliseconds{10});
                            co_await timer.async_wait(boost::asio::use_awaitable);
                        }
                        co_return std::unexpected(support::make_error(support::ErrorCode::Cancelled, "cancelled"));
                    });
        }
        return support::AsyncResult<std::string>{support::Expected<std::string>{""}};
    };

    std::vector<codemode::CodemodeToolDescriptor> tools{
            {.name = "hang", .js_name = "hang", .description = "Hangs until aborted"}};

    // Spawn execution in thread
    std::stop_source caller_stop;
    auto run_future = std::async(std::launch::async, [&]() {
        return run_async(sandbox->run("await tools.hang(); return 'never'",
                std::move(tools),
                {},
                caller_stop.get_token(),
                std::move(handler)));
    });

    // Wait until tool starts
    tool_started_future.wait();

    const auto abort_time = std::chrono::steady_clock::now();
    caller_stop.request_stop();

    auto run_res = run_future.get();
    const auto settled_time = std::chrono::steady_clock::now();
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(settled_time - abort_time).count();

    // Must settle without waiting for remote compute or worker delay (discriminating check: < 200 ms).
#if !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__)
    CHECK(elapsed_ms < 200);
#else
    (void)elapsed_ms;
#endif

    REQUIRE(run_res.has_value());
    REQUIRE(run_res->error.has_value());
    CHECK(run_res->error->kind == codemode::CodemodeError::Kind::Aborted);
    CHECK(run_res->error->message == "This operation was aborted");
    REQUIRE(run_res->calls.size() == 1);
    CHECK(run_res->calls[0].name == "hang");
    CHECK(run_res->calls[0].status == codemode::CodemodeCallRecord::Status::Cancelled);
    CHECK(run_res->calls[0].duration_ms.has_value());

    // Reply sent across exchange must be stop.
    CHECK(stop_reply_received.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
}

TEST_CASE(
        "RemoteCodemodeSandbox discriminating abort check: does not block on slow remote exit", "[codemode][remote]") {
    // Discriminating test: A slow remote that delays 600 ms after receiving stop before returning execute().
    // The caller's run() must return immediately (< 100 ms) instead of waiting for the remote exit!
    struct SlowExitRemote final : public codemode::CodemodeRemote {
        [[nodiscard]] support::AsyncResult<void> execute(
                codemode::CodemodeRemoteRequest, codemode::CodemodeRemoteExchangeHook exchange) override {
            return support::detail::make_async_result(
                    [exchange = std::move(exchange)]() mutable -> boost::asio::awaitable<support::Expected<void>> {
                        auto executor = co_await boost::asio::this_coro::executor;
                        std::vector<codemode::CodemodeRemoteMessageVariant> msgs;
                        msgs.push_back(codemode::CodemodeRemoteCallMessage{
                                .id = 1, .target = "tool", .name = "slow_call", .args = std::nullopt});
                        auto reply = co_await support::detail::await_async_result(exchange(std::move(msgs)));
                        if (reply && std::holds_alternative<codemode::CodemodeRemoteStopReply>(*reply)) {
                            // Simulate slow remote worker cleanup
                            boost::asio::steady_timer delay(executor);
                            delay.expires_after(std::chrono::milliseconds{600});
                            co_await delay.async_wait(boost::asio::use_awaitable);
                        }
                        co_return support::ExpectedVoid{};
                    });
        }
    };

    auto slow_remote = std::make_shared<SlowExitRemote>();
    codemode::RemoteCodemodeSandboxOptions options;
    options.remote = [slow_remote]() -> support::Expected<std::shared_ptr<codemode::CodemodeRemote>> {
        return slow_remote;
    };

    auto created = codemode::RemoteCodemodeSandbox::create(std::move(options));
    REQUIRE(created.has_value());
    auto sandbox = std::move(*created);

    std::promise<void> call_started;
    auto call_started_future = call_started.get_future();
    std::stop_source stop_src;

    codemode::CodemodeToolCallHandler handler =
            [&](std::string_view, std::string_view, std::stop_token stop) -> support::AsyncResult<std::string> {
        call_started.set_value();
        return support::detail::make_async_result([stop]() -> boost::asio::awaitable<support::Expected<std::string>> {
            auto executor = co_await boost::asio::this_coro::executor;
            boost::asio::steady_timer timer(executor);
            while (!stop.stop_requested()) {
                timer.expires_after(std::chrono::milliseconds{10});
                co_await timer.async_wait(boost::asio::use_awaitable);
            }
            co_return std::unexpected(support::make_error(support::ErrorCode::Cancelled, "cancelled"));
        });
    };

    std::vector<codemode::CodemodeToolDescriptor> tools{
            {.name = "slow_call", .js_name = "slow_call", .description = "call"}};

    auto run_future = std::async(std::launch::async, [&]() {
        return run_async(sandbox->run("dummy", std::move(tools), {}, stop_src.get_token(), std::move(handler)));
    });

    call_started_future.wait();
    const auto t0 = std::chrono::steady_clock::now();
    stop_src.request_stop();
    auto result = run_future.get();
    const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();

    // If caller blocked on remote execute() exit, elapsed would be >= 600 ms.
    // It must settle without waiting for remote compute: elapsed < 150 ms.
#if !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__)
    CHECK(elapsed < 150);
#else
    (void)elapsed;
#endif
    REQUIRE(result.has_value());
    REQUIRE(result->error.has_value());
    CHECK(result->error->kind == codemode::CodemodeError::Kind::Aborted);
}

TEST_CASE(
        "RemoteCodemodeSandbox rejects runs after close and closes active executions promptly", "[codemode][remote]") {
    struct DelayedExitRemote final : public codemode::CodemodeRemote {
        std::promise<void> stopped;
        std::atomic<bool> returned{false};

        [[nodiscard]] support::AsyncResult<void> execute(
                codemode::CodemodeRemoteRequest, codemode::CodemodeRemoteExchangeHook exchange) override {
            return support::detail::make_async_result([this, exchange = std::move(exchange)]() mutable
                                                              -> boost::asio::awaitable<support::Expected<void>> {
                auto reply = co_await support::detail::await_async_result(exchange({codemode::CodemodeRemoteCallMessage{
                        .id = 1, .target = "tool", .name = "hang", .args = "null"}}));
                if (reply && std::holds_alternative<codemode::CodemodeRemoteStopReply>(*reply)) {
                    stopped.set_value();
                    auto executor = co_await boost::asio::this_coro::executor;
                    boost::asio::steady_timer delay(executor);
                    delay.expires_after(std::chrono::milliseconds{300});
                    co_await delay.async_wait(boost::asio::use_awaitable);
                }
                returned.store(true);
                co_return support::ExpectedVoid{};
            });
        }
    };

    auto remote = std::make_shared<DelayedExitRemote>();
    codemode::RemoteCodemodeSandboxOptions options;
    options.remote = [remote]() -> support::Expected<std::shared_ptr<codemode::CodemodeRemote>> { return remote; };
    auto created = codemode::RemoteCodemodeSandbox::create(std::move(options));
    REQUIRE(created.has_value());
    auto sandbox = std::move(*created);

    auto stopped = remote->stopped.get_future();
    std::promise<void> handler_started;
    auto handler_started_future = handler_started.get_future();
    codemode::CodemodeToolCallHandler handler = [&handler_started](std::string_view,
                                                        std::string_view,
                                                        std::stop_token signal) -> support::AsyncResult<std::string> {
        handler_started.set_value();
        return support::detail::make_async_result([signal]() -> boost::asio::awaitable<support::Expected<std::string>> {
            auto executor = co_await boost::asio::this_coro::executor;
            boost::asio::steady_timer timer(executor);
            while (!signal.stop_requested()) {
                timer.expires_after(std::chrono::milliseconds{10});
                co_await timer.async_wait(boost::asio::use_awaitable);
            }
            co_return std::unexpected(support::make_error(support::ErrorCode::Cancelled, "closed"));
        });
    };
    auto run_future = std::async(std::launch::async, [&] {
        return run_async(sandbox->run("await tools.hang()",
                {{.name = "hang", .js_name = "hang", .description = "waits"}},
                {},
                {},
                std::move(handler)));
    });
    handler_started_future.wait();
    const auto close_time = std::chrono::steady_clock::now();
    sandbox->close();
    auto result = run_future.get();
    const auto close_elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - close_time)
                    .count();

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
    // Sanitizer overhead invalidates this performance bound; functional assertions still run.
    (void)close_elapsed;
#else
    CHECK(close_elapsed < 150);
#endif
    REQUIRE(result.has_value());
    REQUIRE(result->error.has_value());
    CHECK(result->error->kind == codemode::CodemodeError::Kind::Aborted);
    CHECK(result->error->message == "Sandbox closed");
    CHECK(result->calls.size() == 1);
    CHECK(result->calls.front().status == codemode::CodemodeCallRecord::Status::Cancelled);
    CHECK(stopped.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
    CHECK_FALSE(remote->returned.load());

    auto after_close = run_async(sandbox->run("return 2"));
    REQUIRE(after_close.has_value());
    REQUIRE(after_close->error.has_value());
    CHECK(after_close->error->kind == codemode::CodemodeError::Kind::Aborted);
    CHECK(after_close->error->message == "Sandbox closed");
}

TEST_CASE("RemoteCodemodeSandbox keeps a settled result when remote cleanup fails later", "[codemode][remote]") {
    struct LateFailureRemote final : public codemode::CodemodeRemote {
        std::shared_ptr<tests::ReleaseGate> release_gate = std::make_shared<tests::ReleaseGate>();
        std::mutex executor_mutex;
        std::optional<boost::asio::any_io_executor> executor;
        std::promise<void> reply_received;
        std::promise<void> failure_returned;

        void release() {
            std::optional<boost::asio::any_io_executor> target;
            {
                std::lock_guard lock(executor_mutex);
                target = executor;
            }
            if (target) {
                boost::asio::post(*target, [release_gate = release_gate] { release_gate->release(); });
            }
        }

        [[nodiscard]] support::AsyncResult<void> execute(
                codemode::CodemodeRemoteRequest, codemode::CodemodeRemoteExchangeHook exchange) override {
            return support::detail::make_async_result(
                    [this, release_gate = release_gate, exchange = std::move(exchange)]() mutable
                            -> boost::asio::awaitable<support::Expected<void>> {
                        auto reply = co_await support::detail::await_async_result(
                                exchange({codemode::CodemodeRemoteDoneMessage{.ok = true,
                                        .value = "\\\"settled\\\"",
                                        .writes = "[]",
                                        .error = std::nullopt,
                                        .during = std::nullopt}}));
                        if (!reply) co_return support::ExpectedVoid{};
                        auto current_executor = co_await boost::asio::this_coro::executor;
                        {
                            std::lock_guard lock(executor_mutex);
                            executor = current_executor;
                        }
                        reply_received.set_value();
                        co_await release_gate->wait();
                        failure_returned.set_value();
                        co_return std::unexpected(support::make_error(support::ErrorCode::Process, "late reset"));
                    });
        }
    };

    auto remote = std::make_shared<LateFailureRemote>();
    codemode::RemoteCodemodeSandboxOptions options;
    options.remote = [remote]() -> support::Expected<std::shared_ptr<codemode::CodemodeRemote>> { return remote; };
    auto created = codemode::RemoteCodemodeSandbox::create(std::move(options));
    REQUIRE(created.has_value());
    auto sandbox = std::move(*created);
    auto reply_received = remote->reply_received.get_future();
    auto failure_returned = remote->failure_returned.get_future();

    auto result = run_async(sandbox->run("return 1"));
    REQUIRE(result.has_value());
    CHECK_FALSE(result->error.has_value());
    CHECK(result->value_json == "\\\"settled\\\"");
    CHECK(reply_received.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
    remote->release();
    CHECK(failure_returned.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
    CHECK_FALSE(result->error.has_value());
}

TEST_CASE("RemoteCodemodeSandbox reports failing, silent or unreachable remote as sandbox error with transport reason",
        "[codemode][remote]") {
    // 1. Failing / rejecting remote
    {
        struct FailingRemote final : public codemode::CodemodeRemote {
            [[nodiscard]] support::AsyncResult<void> execute(
                    codemode::CodemodeRemoteRequest, codemode::CodemodeRemoteExchangeHook) override {
                return support::AsyncResult<void>{
                        std::unexpected(support::make_error(support::ErrorCode::Validation, "object reset"))};
            }
        };

        codemode::RemoteCodemodeSandboxOptions options;
        options.remote = []() -> support::Expected<std::shared_ptr<codemode::CodemodeRemote>> {
            return std::make_shared<FailingRemote>();
        };
        auto sandbox = *codemode::RemoteCodemodeSandbox::create(std::move(options));

        auto res = run_async(sandbox->run("return 1"));
        REQUIRE(res.has_value());
        REQUIRE(res->error.has_value());
        CHECK(res->error->kind == codemode::CodemodeError::Kind::Sandbox);
        CHECK(res->error->message == "Remote sandbox failed: object reset");
    }

    // 2. Silent remote (returns without settling script)
    {
        struct SilentRemote final : public codemode::CodemodeRemote {
            [[nodiscard]] support::AsyncResult<void> execute(
                    codemode::CodemodeRemoteRequest, codemode::CodemodeRemoteExchangeHook) override {
                return support::AsyncResult<void>{support::ExpectedVoid{}};
            }
        };

        codemode::RemoteCodemodeSandboxOptions options;
        options.remote = []() -> support::Expected<std::shared_ptr<codemode::CodemodeRemote>> {
            return std::make_shared<SilentRemote>();
        };
        auto sandbox = *codemode::RemoteCodemodeSandbox::create(std::move(options));

        auto res = run_async(sandbox->run("return 1"));
        REQUIRE(res.has_value());
        REQUIRE(res->error.has_value());
        CHECK(res->error->kind == codemode::CodemodeError::Kind::Sandbox);
        CHECK(res->error->message == "Remote sandbox returned before the script settled");
    }

    // 3. Unreachable remote
    {
        codemode::RemoteCodemodeSandboxOptions options;
        options.remote = []() -> support::Expected<std::shared_ptr<codemode::CodemodeRemote>> {
            return std::unexpected(support::make_error(support::ErrorCode::Validation, "no binding"));
        };
        auto sandbox = *codemode::RemoteCodemodeSandbox::create(std::move(options));

        auto res = run_async(sandbox->run("return 1"));
        REQUIRE(res.has_value());
        REQUIRE(res->error.has_value());
        CHECK(res->error->kind == codemode::CodemodeError::Kind::Sandbox);
        CHECK(res->error->message == "Failed to reach the remote sandbox: no binding");
    }
}
