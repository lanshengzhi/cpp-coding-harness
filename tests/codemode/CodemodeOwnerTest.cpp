// Ticket #1064 (spec #1044, ADR 0075): the reusable `cch_codemode` owner. This
// is a public caller, not an application session: it includes only the codemode
// Owner Interface headers, parses the frozen source/declaration observations,
// and executes a real script against an injected nested tool.
//
// Acceptance discipline: a compiled-but-inert header move would pass an
// existence check while the library still needed an application Session or had
// lost the tool bridge. The script below therefore calls the injected tool at
// runtime, and the failure case proves a rejected nested call reaches the
// script while prior output survives.

#include <cch/codemode/Declarations.hpp>
#include <cch/codemode/Sandbox.hpp>
#include <cch/codemode/Source.hpp>

#include "support/AsyncResultBridge.hpp"
#include "support/Json.hpp"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_message.hpp>

#include <algorithm>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <future>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace cch;

namespace {

[[nodiscard]] std::filesystem::path audit_root() {
    return std::filesystem::path{CCH_SOURCE_DIR} / "fixtures" / "pi-v1.3.0" / "audit-1053";
}

[[nodiscard]] std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

[[nodiscard]] support::JsonValue read_artifact(std::string_view artifact) {
    auto parsed = support::read_json(read_text(audit_root() / artifact));
    REQUIRE(parsed.has_value());
    return std::move(*parsed);
}

[[nodiscard]] const support::JsonValue* find_scenario(const support::JsonValue& artifact, std::string_view name) {
    for (const auto& scenario : artifact.get_object().at("scenarios").get_array()) {
        if (scenario.get_object().at("name").get_string() == name) return &scenario;
    }
    return nullptr;
}

[[nodiscard]] const support::JsonValue& member(const support::JsonValue& object, std::string_view name) {
    const auto& fields = object.get_object();
    const auto found = fields.find(std::string{name});
    REQUIRE(found != fields.end());
    return found->second;
}

void check_options(const codemode::CodemodeSourceOptions& options, const support::JsonValue& expected) {
    const auto& fields = expected.get_object();
    if (const auto found = fields.find("timeoutMs"); found == fields.end()) {
        CHECK_FALSE(options.timeout_ms.has_value());
    } else {
        CHECK(options.timeout_ms.has_value());
        if (options.timeout_ms.has_value()) {
            CHECK(*options.timeout_ms == static_cast<std::int64_t>(found->second.get_number()));
        }
    }
    if (const auto found = fields.find("maxOutputTokens"); found == fields.end()) {
        CHECK_FALSE(options.max_output_tokens.has_value());
    } else {
        CHECK(options.max_output_tokens.has_value());
        if (options.max_output_tokens.has_value()) {
            CHECK(*options.max_output_tokens == static_cast<std::int64_t>(found->second.get_number()));
        }
    }
}

void check_parsed_source(std::string_view input, const support::JsonValue& expected) {
    auto parsed = codemode::parse_codemode_source(input);
    CHECK(parsed.has_value());
    if (!parsed) return;
    CHECK(parsed->code == member(expected, "code").get_string());
    check_options(parsed->options, member(expected, "options"));
}

template <typename T> [[nodiscard]] std::expected<T, support::Error> run_async(support::AsyncResult<T> operation) {
    boost::asio::io_context io;
    auto future = boost::asio::co_spawn(
            io,
            [](support::AsyncResult<T> pending) -> boost::asio::awaitable<std::expected<T, support::Error>> {
                co_return co_await support::detail::await_async_result(std::move(pending));
            }(std::move(operation)),
            boost::asio::use_future);
    io.run();
    return future.get();
}

[[nodiscard]] std::unique_ptr<codemode::CodemodeSandbox> make_sandbox(
        std::vector<codemode::CodemodeGlobal> globals = {}) {
    auto sandbox = codemode::CodemodeSandbox::create(codemode::default_codemode_guest_wasm_path(), std::move(globals));
    REQUIRE(sandbox.has_value());
    return std::move(*sandbox);
}

[[nodiscard]] codemode::CodemodeLimits limits() {
    codemode::CodemodeLimits bounds;
    bounds.timeout = std::chrono::seconds{30};
    return bounds;
}

struct WorkerExitGate {
    std::mutex mutex;
    std::condition_variable condition;
    bool released{false};

    void wait() {
        std::unique_lock lock(mutex);
        condition.wait(lock, [this] { return released; });
    }

    void release() {
        {
            std::lock_guard lock(mutex);
            released = true;
        }
        condition.notify_all();
    }
};

struct WorkerHostProbe {
    std::mutex mutex;
    std::condition_variable condition;
    std::size_t capacity{2};
    std::size_t active{0};
    std::size_t acquire_attempts{0};
    std::size_t acquired{0};
    std::size_t start_attempts{0};
    std::size_t started{0};
    std::size_t task_completed{0};
    std::size_t tool_calls{0};
    std::size_t exit_callbacks{0};
    std::size_t max_active{0};
    bool fail_start{false};
    std::vector<std::shared_ptr<WorkerExitGate>> exit_gates;
    std::vector<std::thread> workers;
    std::atomic<std::size_t> released{0};

    [[nodiscard]] bool wait_for_tool_calls(std::size_t expected) {
        std::unique_lock lock(mutex);
        return condition.wait_for(lock, std::chrono::seconds{5}, [this, expected] { return tool_calls >= expected; });
    }

    [[nodiscard]] bool wait_for_task_completion(std::size_t expected) {
        std::unique_lock lock(mutex);
        return condition.wait_for(
                lock, std::chrono::seconds{5}, [this, expected] { return task_completed >= expected; });
    }

    [[nodiscard]] bool wait_for_exit_callbacks(std::size_t expected) {
        std::unique_lock lock(mutex);
        return condition.wait_for(
                lock, std::chrono::seconds{5}, [this, expected] { return exit_callbacks >= expected; });
    }

    void release_exit(std::size_t index) {
        std::shared_ptr<WorkerExitGate> gate;
        {
            std::lock_guard lock(mutex);
            gate = exit_gates.at(index);
        }
        gate->release();
    }

    void release_all_exits() {
        std::vector<std::shared_ptr<WorkerExitGate>> gates;
        {
            std::lock_guard lock(mutex);
            gates = exit_gates;
        }
        for (const auto& gate : gates)
            gate->release();
    }

    void wait_for_releases(std::size_t expected) {
        std::size_t observed = released.load(std::memory_order_acquire);
        while (observed < expected) {
            released.wait(observed, std::memory_order_acquire);
            observed = released.load(std::memory_order_acquire);
        }
    }

    void join_workers() {
        release_all_exits();
        std::vector<std::thread> joining;
        {
            std::lock_guard lock(mutex);
            joining.swap(workers);
        }
        for (auto& worker : joining) {
            if (worker.joinable()) worker.join();
        }
    }

    ~WorkerHostProbe() { join_workers(); }
};

[[nodiscard]] codemode::CodemodeWorkerHost worker_host(std::shared_ptr<WorkerHostProbe> probe) {
    codemode::CodemodeWorkerHost host;
    host.acquire_worker = [probe]() -> support::Expected<codemode::CodemodeWorkerRelease> {
        {
            std::lock_guard lock(probe->mutex);
            ++probe->acquire_attempts;
            if (probe->active == probe->capacity) {
                return std::unexpected(support::make_error(support::ErrorCode::Busy, "worker capacity exhausted"));
            }
            ++probe->active;
            ++probe->acquired;
            probe->max_active = std::max(probe->max_active, probe->active);
        }
        return codemode::CodemodeWorkerRelease{[probe]() {
            {
                std::lock_guard lock(probe->mutex);
                --probe->active;
            }
            probe->released.fetch_add(1, std::memory_order_release);
            probe->released.notify_all();
        }};
    };
    host.start_worker = [probe](codemode::CodemodeWorkerTask task,
                                codemode::CodemodeWorkerExit exited) -> support::Expected<void> {
        {
            std::lock_guard lock(probe->mutex);
            ++probe->start_attempts;
            if (probe->fail_start) {
                return std::unexpected(
                        support::make_error(support::ErrorCode::Validation, "worker construction failed"));
            }
        }
        auto gate = std::make_shared<WorkerExitGate>();
        std::thread worker([probe, task = std::move(task), exited = std::move(exited), gate]() mutable {
            task();
            {
                std::lock_guard lock(probe->mutex);
                ++probe->task_completed;
            }
            probe->condition.notify_all();
            gate->wait();
            exited();
            {
                std::lock_guard lock(probe->mutex);
                ++probe->exit_callbacks;
            }
            probe->condition.notify_all();
        });
        {
            std::lock_guard lock(probe->mutex);
            ++probe->started;
            probe->exit_gates.push_back(std::move(gate));
            probe->workers.push_back(std::move(worker));
        }
        return {};
    };
    return host;
}

[[nodiscard]] std::unique_ptr<codemode::CodemodeSandbox> make_sandbox(
        codemode::CodemodeWorkerHost host, std::vector<codemode::CodemodeGlobal> globals = {}) {
    auto sandbox = codemode::CodemodeSandbox::create(
            codemode::default_codemode_guest_wasm_path(), std::move(globals), std::move(host));
    REQUIRE(sandbox.has_value());
    return std::move(*sandbox);
}

struct WorkerTestGuard {
    std::shared_ptr<WorkerHostProbe> probe;
    std::vector<std::stop_source*> cancellations;

    ~WorkerTestGuard() {
        for (auto* cancellation : cancellations)
            cancellation->request_stop();
        probe->release_all_exits();
    }
};

struct StopSourceGuard {
    std::stop_source* source;

    ~StopSourceGuard() { source->request_stop(); }
};

[[nodiscard]] codemode::CodemodeToolCallHandler hanging_handler(std::shared_ptr<WorkerHostProbe> probe) {
    return [probe](std::string_view, std::string_view, std::stop_token signal) {
        return support::detail::make_async_result(
                [probe, signal]() -> boost::asio::awaitable<support::Expected<std::string>> {
                    {
                        std::lock_guard lock(probe->mutex);
                        ++probe->tool_calls;
                    }
                    probe->condition.notify_all();
                    auto executor = co_await boost::asio::this_coro::executor;
                    boost::asio::steady_timer timer{executor};
                    while (!signal.stop_requested()) {
                        timer.expires_after(std::chrono::milliseconds{5});
                        co_await timer.async_wait(boost::asio::use_awaitable);
                    }
                    co_return std::unexpected(
                            support::make_error(support::ErrorCode::Cancelled, "nested tool cancelled"));
                });
    };
}

template <typename T>
[[nodiscard]] std::future<std::expected<T, support::Error>> run_async_later(support::AsyncResult<T> operation) {
    return std::async(std::launch::async,
            [operation = std::move(operation)]() mutable { return run_async(std::move(operation)); });
}

} // namespace

TEST_CASE("codemode worker results settle before exit while admission stays bounded",
        "[codemode][worker][issue1096][spec]") {
    auto probe = std::make_shared<WorkerHostProbe>();
    auto sandbox = make_sandbox(worker_host(probe));
    const std::vector<codemode::CodemodeToolDescriptor> tools{
            {.name = "hang", .js_name = "hang", .description = "Wait until cancelled."},
    };
    std::array<std::stop_source, 3> cancellations;
    std::vector<std::future<std::expected<codemode::CodemodeRunResult, support::Error>>> results;
    WorkerTestGuard cleanup{probe, {&cancellations[0], &cancellations[1], &cancellations[2]}};
    const auto launch = [&](std::size_t index) {
        results.push_back(run_async_later(sandbox->run("await tools.hang(); return 'unreachable';",
                tools,
                limits(),
                cancellations[index].get_token(),
                hanging_handler(probe),
                {},
                std::string{"cancelled"})));
    };

    launch(0);
    CHECK(probe->wait_for_tool_calls(1));
    cancellations[0].request_stop();
    CHECK(probe->wait_for_task_completion(1));

    launch(1);
    CHECK(probe->wait_for_tool_calls(2));
    cancellations[1].request_stop();
    CHECK(probe->wait_for_task_completion(2));

    // The third attempt is denied while both cancelled workers are held at the
    // exit gate, even though both scripts have already reached terminal states.
    auto denied = run_async(sandbox->run("return 3", {}, limits()));
    REQUIRE(denied.has_value());
    REQUIRE(denied->error.has_value());
    CHECK(denied->error->kind == codemode::CodemodeError::Kind::Sandbox);
    CHECK(denied->error->message == "worker capacity exhausted");
    {
        std::lock_guard lock(probe->mutex);
        CHECK(probe->acquired == 2);
        CHECK(probe->start_attempts == 2);
        CHECK(probe->started == 2);
        CHECK(probe->active == 2);
        CHECK(probe->max_active == 2);
    }
    const bool first_settled_before_exit = results[0].wait_for(std::chrono::seconds{5}) == std::future_status::ready;
    const bool second_settled_before_exit = results[1].wait_for(std::chrono::seconds{5}) == std::future_status::ready;
    CHECK(first_settled_before_exit);
    CHECK(second_settled_before_exit);
    CHECK(probe->released.load(std::memory_order_acquire) == 0);

    probe->release_exit(0);
    probe->release_exit(1);
    auto first = results[0].get();
    auto second = results[1].get();
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    REQUIRE(first->error.has_value());
    REQUIRE(second->error.has_value());
    CHECK(first->error->kind == codemode::CodemodeError::Kind::Aborted);
    CHECK(second->error->kind == codemode::CodemodeError::Kind::Aborted);
    probe->wait_for_releases(2);

    launch(2);
    CHECK(probe->wait_for_tool_calls(3));
    cancellations[2].request_stop();
    CHECK(probe->wait_for_task_completion(3));
    const bool third_settled_before_exit = results[2].wait_for(std::chrono::seconds{5}) == std::future_status::ready;
    CHECK(third_settled_before_exit);
    CHECK(probe->released.load(std::memory_order_acquire) == 2);
    probe->release_exit(2);
    auto third = results[2].get();
    REQUIRE(third.has_value());
    REQUIRE(third->error.has_value());
    CHECK(third->error->kind == codemode::CodemodeError::Kind::Aborted);
    probe->wait_for_releases(3);
    CHECK(probe->wait_for_exit_callbacks(3));
    {
        std::lock_guard lock(probe->mutex);
        CHECK(probe->acquired == 3);
        CHECK(probe->started == 3);
        CHECK(probe->active == 0);
        CHECK(probe->max_active == 2);
        CHECK(probe->exit_callbacks == 3);
    }
}

TEST_CASE("native codemode workers release admission independently of earlier workers",
        "[codemode][worker][issue1096][spec]") {
    auto sandbox = make_sandbox();
    struct NativeProbe {
        std::mutex mutex;
        std::condition_variable condition;
        bool first_tool_started{false};
        std::atomic<bool> tool_aborted{false};
    };
    auto probe = std::make_shared<NativeProbe>();
    std::stop_source cancellation;
    const std::vector<codemode::CodemodeToolDescriptor> tools{
            {.name = "hang", .js_name = "hang", .description = "Wait until cancelled."},
    };
    codemode::CodemodeToolCallHandler handler = [probe](std::string_view, std::string_view, std::stop_token signal) {
        return support::detail::make_async_result(
                [probe, signal]() -> boost::asio::awaitable<support::Expected<std::string>> {
                    {
                        std::lock_guard lock(probe->mutex);
                        probe->first_tool_started = true;
                    }
                    probe->condition.notify_all();
                    auto executor = co_await boost::asio::this_coro::executor;
                    boost::asio::steady_timer timer{executor};
                    while (!signal.stop_requested()) {
                        timer.expires_after(std::chrono::milliseconds{5});
                        co_await timer.async_wait(boost::asio::use_awaitable);
                    }
                    probe->tool_aborted.store(true);
                    co_return std::unexpected(
                            support::make_error(support::ErrorCode::Cancelled, "nested tool cancelled"));
                });
    };
    auto first_future = run_async_later(sandbox->run(
            "await tools.hang(); return 1", tools, limits(), cancellation.get_token(), std::move(handler)));
    StopSourceGuard cleanup{&cancellation};
    {
        std::unique_lock lock(probe->mutex);
        REQUIRE(probe->condition.wait_for(
                lock, std::chrono::seconds{5}, [&probe] { return probe->first_tool_started; }));
    }

    auto second = run_async(sandbox->run("return 2", {}, limits()));
    REQUIRE(second.has_value());
    CHECK_FALSE(second->error.has_value());
    REQUIRE(second->value_json.has_value());
    CHECK(*second->value_json == "2");

    std::optional<std::expected<codemode::CodemodeRunResult, support::Error>> third;
    const auto retry_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    do {
        third.emplace(run_async(sandbox->run("return 3", {}, limits())));
        if (!third->has_value() || !(**third).error.has_value() ||
                (**third).error->message != "worker capacity exhausted") {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    } while (std::chrono::steady_clock::now() < retry_deadline);
    REQUIRE(third.has_value());
    REQUIRE(third->has_value());
    CHECK_FALSE((**third).error.has_value());
    REQUIRE((**third).value_json.has_value());
    CHECK(*(**third).value_json == "3");

    cancellation.request_stop();
    CHECK(first_future.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
    CHECK(probe->tool_aborted.load());
    auto first = first_future.get();
    REQUIRE(first.has_value());
    REQUIRE(first->error.has_value());
    CHECK(first->error->kind == codemode::CodemodeError::Kind::Aborted);
}

TEST_CASE("codemode worker admission follows wasm load and releases a failed start once",
        "[codemode][worker][issue1096][spec]") {
    auto load_probe = std::make_shared<WorkerHostProbe>();
    auto missing = codemode::CodemodeSandbox::create(
            codemode::default_codemode_guest_wasm_path() / "missing.wasm", {}, worker_host(load_probe));
    CHECK_FALSE(missing.has_value());
    {
        std::lock_guard lock(load_probe->mutex);
        CHECK(load_probe->acquire_attempts == 0);
        CHECK(load_probe->acquired == 0);
        CHECK(load_probe->start_attempts == 0);
    }

    auto start_probe = std::make_shared<WorkerHostProbe>();
    start_probe->fail_start = true;
    auto sandbox = make_sandbox(worker_host(start_probe));
    auto failed_start = run_async(sandbox->run("return 1", {}, limits()));
    REQUIRE(failed_start.has_value());
    REQUIRE(failed_start->error.has_value());
    CHECK(failed_start->error->kind == codemode::CodemodeError::Kind::Sandbox);
    CHECK(failed_start->error->message == "worker construction failed");
    CHECK(start_probe->released.load(std::memory_order_acquire) == 1);
    {
        std::lock_guard lock(start_probe->mutex);
        CHECK(start_probe->acquire_attempts == 1);
        CHECK(start_probe->acquired == 1);
        CHECK(start_probe->start_attempts == 1);
        CHECK(start_probe->started == 0);
        CHECK(start_probe->active == 0);
    }
}

TEST_CASE("closing codemode settles the run before its worker exit releases admission",
        "[codemode][worker][issue1096][spec]") {
    auto probe = std::make_shared<WorkerHostProbe>();
    auto sandbox = make_sandbox(worker_host(probe));
    const std::vector<codemode::CodemodeToolDescriptor> tools{
            {.name = "hang", .js_name = "hang", .description = "Wait until cancelled."},
    };
    std::stop_source cancellation;
    std::vector<std::future<std::expected<codemode::CodemodeRunResult, support::Error>>> results;
    WorkerTestGuard cleanup{probe, {&cancellation}};
    results.push_back(run_async_later(sandbox->run("await tools.hang(); return 'unreachable';",
            tools,
            limits(),
            cancellation.get_token(),
            hanging_handler(probe))));
    CHECK(probe->wait_for_tool_calls(1));

    sandbox->close();
    CHECK(probe->wait_for_task_completion(1));
    const bool settled_before_exit = results[0].wait_for(std::chrono::seconds{5}) == std::future_status::ready;
    CHECK(settled_before_exit);
    CHECK(probe->released.load(std::memory_order_acquire) == 0);
    auto after_close = run_async(sandbox->run("return 2", {}, limits()));
    REQUIRE(after_close.has_value());
    REQUIRE(after_close->error.has_value());
    CHECK(after_close->error->message == "Sandbox closed");
    {
        std::lock_guard lock(probe->mutex);
        CHECK(probe->acquired == 1);
        CHECK(probe->start_attempts == 1);
    }

    probe->release_exit(0);
    auto closed_run = results[0].get();
    REQUIRE(closed_run.has_value());
    REQUIRE(closed_run->error.has_value());
    CHECK(closed_run->error->kind == codemode::CodemodeError::Kind::Aborted);
    CHECK(closed_run->error->message == "Sandbox closed");
    probe->wait_for_releases(1);
}

TEST_CASE("the public codemode source parser keeps only a first-line options command",
        "[codemode][source][issue1064][spec]") {
    const auto artifact = read_artifact("source-options.json");

    // The frozen capture's accepted forms: plain code, first-line options,
    // leading whitespace/CRLF, and option-like text that is not the first line.
    const std::string_view accepted[] = {
            "source keeps plain JavaScript and a leading ordinary comment unchanged",
            "first-line options are parsed and replaced by an empty line to preserve line numbers",
            "options accept leading whitespace, CRLF, an empty object and a zero token budget",
            "only the first line is an options line",
    };
    for (const auto name : accepted) {
        const auto* scenario = find_scenario(artifact, name);
        REQUIRE(scenario != nullptr);
        INFO("scenario: " + std::string{name});
        const auto& captured = member(*scenario, "inputs");
        const auto& expected = member(*scenario, "expected");
        if (const auto found = captured.get_object().find("input"); found != captured.get_object().end()) {
            INFO("input: " + found->second.get_string());
            check_parsed_source(found->second.get_string(), expected);
            continue;
        }
        const auto& input_values = member(captured, "inputs").get_array();
        const auto& expected_values = expected.get_array();
        REQUIRE(input_values.size() == expected_values.size());
        for (std::size_t i = 0; i < input_values.size(); ++i) {
            INFO("input[" << i << "]: " << input_values[i].get_string());
            check_parsed_source(input_values[i].get_string(), expected_values[i]);
        }
    }

    const auto* constants = find_scenario(artifact, "the public options prefix and grammar constants are fixed");
    REQUIRE(constants != nullptr);
    const auto& exported = member(*constants, "expected");
    CHECK(codemode::kCodemodeOptionsPrefix == member(exported, "prefix").get_string());
    CHECK(codemode::kCodemodeSourceGrammar == member(exported, "grammar").get_string());
}

TEST_CASE("the public codemode source parser rejects a misplaced options line with its line number",
        "[codemode][source][issue1100][spec]") {
    // Frozen pi v1.3.0 (`source-options.json`, "a misplaced options line is
    // rejected with its line number"): a `// @options:` line anywhere past line
    // 1 is a typed error naming the line — including after a valid options line.
    const auto artifact = read_artifact("source-options.json");
    const auto* scenario = find_scenario(artifact, "a misplaced options line is rejected with its line number");
    REQUIRE(scenario != nullptr);
    const auto& inputs = member(member(*scenario, "inputs"), "inputs").get_array();
    const auto& expected = member(*scenario, "expected").get_array();
    REQUIRE(inputs.size() == expected.size());
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        INFO("input[" << i << "]: " << inputs[i].get_string());
        auto parsed = codemode::parse_codemode_source(inputs[i].get_string());
        CHECK_FALSE(parsed.has_value());
        if (parsed) continue;
        CHECK(parsed.error().code == support::ErrorCode::Validation);
        CHECK(parsed.error().message == member(expected[i], "message").get_string());
    }
}

TEST_CASE("the public codemode source parser rejects invalid options and empty input with typed errors",
        "[codemode][source][issue1100][spec]") {
    // Frozen pi v1.3.0 (`source-options.json`, "invalid options and empty
    // input are rejected with typed errors"): every case is a Validation error
    // with the captured message — never a silent fallback to "no options".
    const auto artifact = read_artifact("source-options.json");
    const auto* scenario = find_scenario(artifact, "invalid options and empty input are rejected with typed errors");
    REQUIRE(scenario != nullptr);
    const auto& inputs = member(member(*scenario, "inputs"), "inputs").get_array();
    const auto& expected = member(*scenario, "expected").get_array();
    REQUIRE(inputs.size() == expected.size());
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        const auto& input = member(expected[i], "input").get_string();
        INFO("input[" << i << "]: " << input);
        auto parsed = codemode::parse_codemode_source(input);
        CHECK_FALSE(parsed.has_value());
        if (parsed) continue;
        CHECK(parsed.error().code == support::ErrorCode::Validation);
        CHECK(parsed.error().message == member(member(expected[i], "error"), "message").get_string());
    }
}

TEST_CASE("the public codemode declaration preserves the relocated tool identifiers",
        "[codemode][declarations][issue1064][spec]") {
    const auto artifact = read_artifact("declarations.json");
    const auto* scenario =
            find_scenario(artifact, "tool identifiers replace invalid characters and preserve MCP names");
    REQUIRE(scenario != nullptr);

    const auto& names = member(member(*scenario, "inputs"), "names").get_array();
    const auto& expected = member(*scenario, "expected").get_array();
    REQUIRE(names.size() == expected.size());
    // The frozen identifier capture's last case (`naïve`) exercises a
    // multi-byte character: one invalid code point becomes one `_`.
    for (std::size_t i = 0; i < names.size(); ++i) {
        INFO("name: " << names[i].get_string());
        CHECK(codemode::to_codemode_identifier(names[i].get_string()) == expected[i].get_string());
    }
}

TEST_CASE("the public codemode declaration renders frozen schema types", "[codemode][declarations][issue1064][spec]") {
    const auto artifact = read_artifact("declarations.json");
    const auto* scenario =
            find_scenario(artifact, "schema primitives, literals, unions and references render as TypeScript types");
    REQUIRE(scenario != nullptr);

    const auto& schemas = member(member(*scenario, "inputs"), "schemas").get_array();
    const auto& expected = member(*scenario, "expected").get_array();
    REQUIRE(schemas.size() == expected.size());
    for (std::size_t i = 0; i < schemas.size(); ++i) {
        CHECK(codemode::schema_to_type(schemas[i], 16'000) == expected[i].get_string());
    }
}

TEST_CASE("the public codemode declaration covers the frozen schema renderings",
        "[codemode][declarations][issue1100][spec]") {
    // Every frozen declarations scenario rendered through schema_to_type: one-
    // line property sorting and quoting, doc comments, constraint tags,
    // optional-null branches, draft-04 exclusive bounds, escaped terminators,
    // tag dropping under the character budget, and $ref resolution.
    const auto artifact = read_artifact("declarations.json");
    const auto schemas_scenario = [&](std::string_view name) -> const support::JsonValue* {
        return find_scenario(artifact, name);
    };

    for (const auto name : {
                 "object properties are sorted on one line with quoted non-identifier keys",
         }) {
        const auto* scenario = schemas_scenario(name);
        REQUIRE(scenario != nullptr);
        INFO("scenario: " + std::string{name});
        const auto& schemas = member(member(*scenario, "inputs"), "schemas").get_array();
        const auto& expected = member(*scenario, "expected").get_array();
        REQUIRE(schemas.size() == expected.size());
        for (std::size_t i = 0; i < schemas.size(); ++i) {
            CHECK(codemode::schema_to_type(schemas[i], codemode::kDefaultInputSchemaMaxChars) ==
                    expected[i].get_string());
        }
    }

    // Single-schema scenarios whose expected value is the rendered object.
    for (const auto name : {
                 "property descriptions become comment lines",
                 "constraint tags render integer bounds, defaults, patterns and deprecated markers",
                 "optional non-null branches keep their constraint tags",
                 "draft-04 boolean exclusive bounds become exclusive tags",
         }) {
        const auto* scenario = schemas_scenario(name);
        REQUIRE(scenario != nullptr);
        INFO("scenario: " + std::string{name});
        const auto& schema = member(member(*scenario, "inputs"), "schema");
        CHECK(codemode::schema_to_type(schema, codemode::kDefaultInputSchemaMaxChars) ==
                member(*scenario, "expected").get_string());
    }

    {
        const auto* scenario = schemas_scenario(
                "long tags are dropped, line terminators are escaped, and line breaks stay in comments");
        REQUIRE(scenario != nullptr);
        const auto& schemas = member(member(*scenario, "inputs"), "schemas").get_array();
        const auto& expected = member(*scenario, "expected").get_array();
        REQUIRE(schemas.size() == expected.size());
        for (std::size_t i = 0; i < schemas.size(); ++i) {
            CHECK(codemode::schema_to_type(schemas[i], codemode::kDefaultInputSchemaMaxChars) ==
                    expected[i].get_string());
        }
    }

    {
        const auto* scenario = schemas_scenario("types over the character budget drop tags, then become unknown");
        REQUIRE(scenario != nullptr);
        const auto& schema = member(member(*scenario, "inputs"), "schema");
        const auto& budgets = member(member(*scenario, "inputs"), "budgets").get_array();
        const auto& expected = member(*scenario, "expected").get_array();
        REQUIRE(budgets.size() == expected.size());
        for (std::size_t i = 0; i < budgets.size(); ++i) {
            const std::size_t budget = budgets[i].holds<support::JsonValue::null_t>()
                                               ? std::size_t{0}
                                               : static_cast<std::size_t>(budgets[i].get_number());
            CHECK(codemode::schema_to_type(schema, budget) == expected[i].get_string());
        }
    }

    {
        const auto* scenario =
                schemas_scenario("local references resolve and recursive or remote references render unknown");
        REQUIRE(scenario != nullptr);
        const auto& schema = member(member(*scenario, "inputs"), "schema");
        CHECK(codemode::schema_to_type(schema, codemode::kDefaultInputSchemaMaxChars) ==
                member(*scenario, "expected").get_string());
    }
}

TEST_CASE("the public codemode declaration renders frozen tool signatures samples and blocks",
        "[codemode][declarations][issue1100][spec]") {
    const auto artifact = read_artifact("declarations.json");
    const auto* scenario =
            find_scenario(artifact, "tool signatures and samples use the output schema and input budget");
    REQUIRE(scenario != nullptr);
    const auto& captured = member(*scenario, "inputs");
    ai::Tool tool;
    tool.name = member(member(captured, "tool"), "name").get_string();
    tool.description = member(member(captured, "tool"), "description").get_string();
    tool.parameters = member(member(captured, "tool"), "inputSchema");
    tool.output_schema = member(member(captured, "tool"), "outputSchema");
    const auto& expected = member(*scenario, "expected");

    CHECK(codemode::render_tool_declaration(tool) == member(expected, "signature").get_string());
    CHECK(codemode::render_tool_sample(tool) == member(expected, "sample").get_string());
    CHECK(codemode::render_tool_declaration(tool, 10) == member(expected, "budgeted").get_string());

    // A tool without an output schema promises `unknown`.
    auto unknown = tool;
    unknown.output_schema.reset();
    const std::string unknown_declaration = codemode::render_tool_declaration(unknown);
    CHECK(unknown_declaration.ends_with(": Promise<unknown>;"));
    CHECK(member(expected, "unknownOutput").get_string() == "unknown");
    // `output` is the output schema's own rendering.
    CHECK(codemode::schema_to_type(*tool.output_schema, codemode::kDefaultInputSchemaMaxChars) ==
            "{ results: Array<string>; }");
    CHECK(codemode::schema_to_type(support::JsonValue{support::JsonValue::object_t{{"type", "object"}}},
                  codemode::kDefaultInputSchemaMaxChars) == member(expected, "output").get_string());
}

TEST_CASE("the public codemode declaration groups tools globals and namespaces with doc comments",
        "[codemode][declarations][issue1100][spec]") {
    const auto artifact = read_artifact("declarations.json");
    const auto* scenario =
            find_scenario(artifact, "renderDeclarations groups tools, globals and namespaces with doc comments");
    REQUIRE(scenario != nullptr);
    const auto& captured = member(*scenario, "inputs");
    std::vector<ai::Tool> tools;
    for (const auto& entry : member(captured, "tools").get_array()) {
        ai::Tool tool;
        tool.name = member(entry, "name").get_string();
        tool.description = member(entry, "description").get_string();
        tool.parameters = member(entry, "inputSchema");
        tool.output_schema = member(entry, "outputSchema");
        tools.push_back(std::move(tool));
    }
    std::vector<codemode::CodemodeGlobalDeclaration> globals;
    for (const auto& entry : member(captured, "globals").get_array()) {
        codemode::CodemodeGlobalDeclaration global;
        global.name = member(entry, "name").get_string();
        if (const auto found = entry.get_object().find("description"); found != entry.get_object().end()) {
            global.description = found->second.get_string();
        }
        global.signature = member(entry, "signature").get_string();
        globals.push_back(std::move(global));
    }
    CHECK(codemode::render_declarations(tools, globals) == member(*scenario, "expected").get_string());
}

TEST_CASE("the public codemode limits are the frozen public constants", "[codemode][limits][issue1100][spec]") {
    const auto artifact = read_artifact("declarations.json");
    const auto* scenario = find_scenario(artifact, "default and stored limits are public constants");
    REQUIRE(scenario != nullptr);
    const auto& expected = member(*scenario, "expected");
    CHECK(codemode::kDefaultInputSchemaMaxChars ==
            static_cast<std::size_t>(member(expected, "inputSchemaMaxChars").get_number()));
    CHECK(codemode::kMaxOutputChars == static_cast<std::size_t>(member(expected, "outputChars").get_number()));
    CHECK(codemode::kMaxOutputItems == static_cast<std::size_t>(member(expected, "outputItems").get_number()));
    CHECK(codemode::kMaxStoreTotalChars == static_cast<std::size_t>(member(expected, "storeTotalChars").get_number()));
    CHECK(codemode::kMaxStoreValueChars == static_cast<std::size_t>(member(expected, "storeValueChars").get_number()));
    CHECK(codemode::kDefaultInterruptBudget ==
            static_cast<std::size_t>(member(expected, "interruptBudget").get_number()));
}

TEST_CASE("a public codemode caller executes a script against an injected nested tool",
        "[codemode][sandbox][issue1064][spec]") {
    auto sandbox = make_sandbox();
    std::vector<std::string> calls;
    const std::vector<codemode::CodemodeToolDescriptor> tools{
            {.name = "echo", .js_name = "echo", .description = "Echo the argument."},
    };
    codemode::CodemodeToolCallHandler handler = [&calls](std::string_view name,
                                                        std::string_view arguments,
                                                        std::stop_token) {
        calls.push_back(std::format("{}:{}", name, arguments));
        return support::AsyncResult<std::string>{support::Expected<std::string>{std::string{R"({"value":"pong"})"}}};
    };

    auto result = run_async(sandbox->run("text('before');\n"
                                         "const echoed = await tools.echo({ x: 1 });\n"
                                         "text('after:' + echoed.value);\n"
                                         "return echoed;",
            tools,
            limits(),
            {},
            std::move(handler)));

    REQUIRE(result.has_value());
    CHECK_FALSE(result->error.has_value());
    REQUIRE(result->value_json.has_value());
    CHECK(*result->value_json == R"({"value":"pong"})");
    REQUIRE(result->output.size() == 2);
    CHECK(result->output[0].kind == codemode::CodemodeOutputItem::Kind::Text);
    CHECK(result->output[0].data == "before");
    CHECK(result->output[1].data == "after:pong");
    REQUIRE(calls.size() == 1);
    CHECK(calls.front() == R"(echo:{"x":1})");
}

TEST_CASE("a public codemode caller keeps prior output when an injected tool rejects",
        "[codemode][sandbox][issue1064][spec]") {
    auto sandbox = make_sandbox();
    const std::vector<codemode::CodemodeToolDescriptor> tools{
            {.name = "fail", .js_name = "fail", .description = "Fail the call."},
    };
    codemode::CodemodeToolCallHandler handler = [](std::string_view, std::string_view, std::stop_token) {
        return support::AsyncResult<std::string>{support::Expected<std::string>{
                std::unexpected(support::make_error(support::ErrorCode::Validation, "nested tool rejected the call"))}};
    };

    auto result = run_async(sandbox->run(
            "text('before');\nawait tools.fail({});\ntext('after');", tools, limits(), {}, std::move(handler)));

    REQUIRE(result.has_value());
    REQUIRE(result->error.has_value());
    CHECK(result->error->kind == codemode::CodemodeError::Kind::Script);
    CHECK(result->error->message.find("nested tool rejected the call") != std::string::npos);
    REQUIRE(result->output.size() == 1);
    CHECK(result->output[0].data == "before");
}

TEST_CASE("a public codemode caller executes discovery-returned tools with namespace-consistent names",
        "[codemode][discovery][globals][issue1091][spec]") {
    // #1091: the script discovers a tool through a spread global, then executes
    // the returned raw name through the tools surface. The raw name and its
    // normalized identifier must reach the same host call — an empty catalog
    // or a name-mangled call would not be discovery.
    auto sandbox = make_sandbox({
            {.name = "searchTools", .spread = true},
            {.name = "describeTool", .spread = true},
    });
    std::vector<std::string> calls;
    const std::vector<codemode::CodemodeToolDescriptor> tools{
            {.name = "counter.add", .js_name = "counter_add", .description = "Add two numbers."},
    };
    codemode::CodemodeToolCallHandler handler = [&calls](std::string_view name,
                                                        std::string_view arguments,
                                                        std::stop_token) {
        calls.push_back(std::format("{}:{}", name, arguments));
        if (name == "searchTools") {
            return support::AsyncResult<std::string>{support::Expected<std::string>{
                    std::string{R"([{"name":"counter.add","description":"Add two numbers."}])"}}};
        }
        if (name == "describeTool") {
            return support::AsyncResult<std::string>{support::Expected<std::string>{std::string{
                    R"({"name":"counter.add","declaration":"declare const tools: { counter_add(args: {a: number, b: number}): Promise<unknown> };"})"}}};
        }
        // Sum the arguments so the raw-name call and the normalized-name call
        // prove they each executed with their own argument.
        double sum = 0.0;
        if (auto args = support::read_json(arguments)) {
            if (const auto* fields = args->get_if<support::JsonValue::object_t>()) {
                for (const char* key : {"a", "b"}) {
                    if (const auto it = fields->find(key); it != fields->end()) {
                        if (const auto* number = it->second.get_if<double>()) sum += *number;
                    }
                }
            }
        }
        return support::AsyncResult<std::string>{
                support::Expected<std::string>{std::format("{{\"value\":{:.0f}}}", sum)}};
    };

    auto result = run_async(sandbox->run("const found = await searchTools('add');\n"
                                         "const target = found[0].name;\n"
                                         "const described = await describeTool(target);\n"
                                         "const viaRaw = await tools[target]({ a: 2, b: 3 });\n"
                                         "const viaJs = await tools.counter_add({ a: 1, b: 1 });\n"
                                         "text(described.declaration);\n"
                                         "return [target, viaRaw.value, viaJs.value];",
            tools,
            limits(),
            {},
            std::move(handler)));

    REQUIRE(result.has_value());
    CHECK_FALSE(result->error.has_value());
    REQUIRE(result->value_json.has_value());
    CHECK(*result->value_json == R"(["counter.add",5,2])");
    REQUIRE(result->output.size() == 1);
    CHECK(result->output[0].data ==
            "declare const tools: { counter_add(args: {a: number, b: number}): Promise<unknown> };");
    // namespace/name consistency: discovery, the raw-name call, and the
    // normalized-identifier call all observe the registered name.
    REQUIRE(calls.size() == 4);
    CHECK(calls[0] == R"(searchTools:["add"])");
    CHECK(calls[1] == R"(describeTool:["counter.add"])");
    CHECK(calls[2] == R"(counter.add:{"a":2,"b":3})");
    CHECK(calls[3] == R"(counter.add:{"a":1,"b":1})");
}

TEST_CASE("a public codemode caller cannot execute a discovery name the host does not provide",
        "[codemode][discovery][issue1091][spec]") {
    // Acceptance discriminating case (#1091 "an empty catalog does not count"):
    // discovery must reflect the actually-callable set. A catalog name the
    // host did not register must not be executable, so a discovery surface
    // that advertises tools the callable set lacks is observable to the script.
    auto sandbox = make_sandbox({
            {.name = "searchTools", .spread = true},
    });
    const std::vector<codemode::CodemodeToolDescriptor> tools{
            {.name = "counter.add", .js_name = "counter_add", .description = "Add two numbers."},
    };
    codemode::CodemodeToolCallHandler handler = [](std::string_view name, std::string_view, std::stop_token) {
        if (name == "searchTools") {
            // The double claims a tool the host never registered.
            return support::AsyncResult<std::string>{support::Expected<std::string>{
                    std::string{R"([{"name":"ghost.tool","description":"Not registered."}])"}}};
        }
        return support::AsyncResult<std::string>{support::Expected<std::string>{std::string{"null"}}};
    };

    auto result = run_async(sandbox->run("const found = await searchTools('ghost');\n"
                                         "try {\n"
                                         "  await tools[found[0].name]({});\n"
                                         "  return 'executed';\n"
                                         "} catch (error) {\n"
                                         "  return [error.name, error.message];\n"
                                         "}",
            tools,
            limits(),
            {},
            std::move(handler)));

    REQUIRE(result.has_value());
    CHECK_FALSE(result->error.has_value());
    REQUIRE(result->value_json.has_value());
    CHECK(*result->value_json ==
            R"(["TypeError","tools.ghost.tool does not exist. Available: counter_add. ALL_TOOLS lists every tool; searchTools(query) finds tools by topic. Check for a member with \"ghost.tool\" in tools."])");
}

TEST_CASE("a public codemode caller collects mixed output in order with the console marker and image MIME",
        "[codemode][output][issue1092][spec]") {
    // Frozen pi v1.3.0 (`fixtures/pi-v1.3.0/audit-1053/script-execution.json`,
    // "a script returns a JSON value after top-level await and collects output
    // in order"): `text()` and `console.*` both produce `type: "text"` items,
    // console items carry `console: true`, and `image()` passes the base64 data
    // and detected MIME through. The data URI is the same 1x1 PNG pi used.
    auto sandbox = make_sandbox();

    auto result = run_async(sandbox->run("text('plain'); console.log('log', 2); console.error('err'); "
                                         "image('data:image/png;base64,"
                                         "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAFgwJ/"
                                         "lYQ5WQAAAABJRU5ErkJggg=='); "
                                         "return { a: 1 }",
            {},
            limits(),
            {},
            nullptr));

    REQUIRE(result.has_value());
    CHECK_FALSE(result->error.has_value());
    REQUIRE(result->value_json.has_value());
    CHECK(*result->value_json == R"({"a":1})");
    const auto& output = result->output;
    REQUIRE(output.size() == 4);
    CHECK(output[0].kind == codemode::CodemodeOutputItem::Kind::Text);
    CHECK(output[0].data == "plain");
    CHECK_FALSE(output[0].console);
    CHECK(output[1].kind == codemode::CodemodeOutputItem::Kind::Text);
    CHECK(output[1].data == "log 2");
    CHECK(output[1].console);
    CHECK(output[2].data == "err");
    CHECK(output[2].console);
    CHECK(output[3].kind == codemode::CodemodeOutputItem::Kind::Image);
    CHECK(output[3].data ==
            "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAFgwJ/lYQ5WQAAAABJRU5ErkJggg==");
    CHECK(output[3].mime_type == "image/png");
    CHECK_FALSE(output[3].console);
}

TEST_CASE("a public codemode caller reports a script error with its name message stack and prior output",
        "[codemode][errors][issue1092][spec]") {
    // Frozen pi v1.3.0 (`script-execution.json`, "a script failure keeps prior
    // output and reports the script error with its line"): kind `script`, the
    // error's name/message, the guest stack with the script's line, and the
    // output emitted before the throw.
    auto sandbox = make_sandbox();

    auto result = run_async(sandbox->run("text(\"before\");\nthrow new Error(\"boom\")", {}, limits(), {}, nullptr));

    REQUIRE(result.has_value());
    REQUIRE(result->error.has_value());
    CHECK(result->error->kind == codemode::CodemodeError::Kind::Script);
    CHECK(result->error->name == "Error");
    CHECK(result->error->message == "boom");
    REQUIRE(result->error->stack.has_value());
    CHECK(*result->error->stack == "Error: boom\n    at <anonymous> (codemode.js:2:11)");
    REQUIRE(result->output.size() == 1);
    CHECK(result->output[0].data == "before");
    CHECK_FALSE(result->value_json.has_value());
    CHECK(result->calls.empty());
    CHECK_FALSE(result->store_writes.has_value());
}

TEST_CASE("a public codemode caller reports non-Error throws BigInt returns and syntax errors as script errors",
        "[codemode][errors][issue1092][spec]") {
    // Frozen pi v1.3.0 (`script-execution.json`, "non-Error throws, syntax
    // errors and non-serializable returns are script errors"): a non-Error
    // throw carries only `message`; a non-serializable return surfaces the
    // guest stringify TypeError with its stack; a syntax error surfaces with
    // the guest's line report.
    auto sandbox = make_sandbox();

    auto plain = run_async(sandbox->run("throw \"plain\"", {}, limits(), {}, nullptr));
    REQUIRE(plain.has_value());
    REQUIRE(plain->error.has_value());
    CHECK(plain->error->kind == codemode::CodemodeError::Kind::Script);
    CHECK(plain->error->message == "plain");
    CHECK_FALSE(plain->error->name.has_value());
    CHECK_FALSE(plain->error->stack.has_value());
    CHECK(plain->output.empty());

    auto bigint = run_async(sandbox->run("return 1n", {}, limits(), {}, nullptr));
    REQUIRE(bigint.has_value());
    REQUIRE(bigint->error.has_value());
    CHECK(bigint->error->name == "TypeError");
    CHECK(bigint->error->message == "BigInt are forbidden in JSON.stringify");
    REQUIRE(bigint->error->stack.has_value());
    CHECK(bigint->error->stack->starts_with("TypeError: BigInt are forbidden in JSON.stringify"));

    auto syntax = run_async(sandbox->run("text(", {}, limits(), {}, nullptr));
    REQUIRE(syntax.has_value());
    REQUIRE(syntax->error.has_value());
    CHECK(syntax->error->name == "SyntaxError");
    CHECK(syntax->error->message == "unexpected token in expression: '}'");
    REQUIRE(syntax->error->stack.has_value());
    CHECK(syntax->error->stack->starts_with("SyntaxError: unexpected token in expression: '}'"));
}

TEST_CASE("a public codemode caller treats exit() as success with prior output and the write set",
        "[codemode][output][issue1092][spec]") {
    // Frozen pi v1.3.0 (`script-execution.json`, "exit() succeeds with prior
    // output and store writes and stops later output"): `exit()` ends the run
    // as success, keeps the output emitted before it, stops later output, and
    // reports the pending store writes.
    auto sandbox = make_sandbox();

    auto result = run_async(sandbox->run(
            "text(\"before\"); store(\"a\", {x: 1}); store(\"b\", 2); store(\"b\", undefined); exit(); text(\"after\")",
            {},
            limits(),
            {},
            nullptr));

    REQUIRE(result.has_value());
    CHECK_FALSE(result->error.has_value());
    CHECK_FALSE(result->value_json.has_value());
    REQUIRE(result->output.size() == 1);
    CHECK(result->output[0].data == "before");
    REQUIRE(result->store_writes.has_value());
    REQUIRE(result->store_writes->set.size() == 1);
    const auto* a_value = result->store_writes->set.at("a").get_if<support::JsonValue::object_t>();
    REQUIRE(a_value != nullptr);
    const auto x = a_value->find("x");
    REQUIRE(x != a_value->end());
    const auto* x_value = x->second.get_if<double>();
    REQUIRE(x_value != nullptr);
    CHECK(*x_value == 1.0);
    REQUIRE(result->store_writes->deleted.size() == 1);
    CHECK(result->store_writes->deleted[0] == "b");
    CHECK(result->calls.empty());
}

TEST_CASE("a public codemode caller reports guest memory exhaustion as an out-of-memory script error",
        "[codemode][limits][issue1092][spec]") {
    // Frozen pi v1.3.0 (`script-execution.json`, "a memory-limited VM reports
    // out of memory as a script error", upstream portable.test.ts "caps the
    // interrupt budget and memory at the remote's limits"): with the guest
    // memory capped, an allocation past the cap fails the run as a script
    // error named InternalError with message "out of memory".
    auto sandbox = make_sandbox();
    auto memory_limits = limits();
    memory_limits.memory_limit_bytes = 8u * 1024u * 1024u;

    // The allocation pattern is the frozen upstream case (pi
    // packages/codemode/test/portable.test.ts:93-105): each iteration builds a
    // unique 1 MiB string, so QuickJS cannot intern or rope its way out of
    // actually allocating; the 8 MiB cap exhausts within a handful of rounds.
    auto result =
            run_async(sandbox->run("const parts = [];\nfor (;;) parts.push(\"x\".repeat(1 << 20) + parts.length);",
                    {},
                    memory_limits,
                    {},
                    nullptr));

    REQUIRE(result.has_value());
    REQUIRE(result->error.has_value());
    CHECK(result->error->kind == codemode::CodemodeError::Kind::Script);
    CHECK(result->error->name == "InternalError");
    CHECK(result->error->message == "out of memory");
    CHECK(result->output.empty());
    CHECK(result->calls.empty());
}

TEST_CASE("a public codemode caller fails the run when the guest hits its output item limit",
        "[codemode][limits][issue1092][spec]") {
    // The frozen prelude enforces pi's output limits (16 MiB of characters or
    // 100000 text/image/console items) guest-side: past the limit the script
    // fails with the frozen RangeError, and the host reports it as a script
    // error. One over-limit text() is the cheap discriminating case; the
    // 100000-item variant differs only in which clause trips first.
    auto sandbox = make_sandbox();

    auto result =
            run_async(sandbox->run("text('x'.repeat(16777217)); return 'unreachable';", {}, limits(), {}, nullptr));

    REQUIRE(result.has_value());
    REQUIRE(result->error.has_value());
    CHECK(result->error->kind == codemode::CodemodeError::Kind::Script);
    CHECK(result->error->name == "RangeError");
    CHECK(result->error->message ==
            "script output exceeded the limit of 16777216 characters or 100000 text(), image(), and console calls. "
            "Print a summary instead, or write large data to a file with a tool.");
    CHECK_FALSE(result->value_json.has_value());
}

TEST_CASE("a public codemode caller fails the run when the guest hits its store value limit",
        "[codemode][limits][issue1092][spec]") {
    // The frozen prelude enforces pi's store limits guest-side (262144 JSON
    // characters per value, 1048576 total): past a limit the script fails with
    // the frozen RangeError and the host reports it as a script error.
    auto sandbox = make_sandbox();

    auto result = run_async(
            sandbox->run("store('big', 'x'.repeat(262145)); return 'unreachable';", {}, limits(), {}, nullptr));

    REQUIRE(result.has_value());
    REQUIRE(result->error.has_value());
    CHECK(result->error->kind == codemode::CodemodeError::Kind::Script);
    CHECK(result->error->name == "RangeError");
    CHECK(result->error->message.starts_with(
            "store(\"big\") value has 262147 characters of JSON, more than the limit of 262144."));
    CHECK_FALSE(result->value_json.has_value());
    CHECK_FALSE(result->store_writes.has_value());
}

TEST_CASE("a public codemode caller returns its value and text output from a successful budgeted run",
        "[codemode][inline][issue1097][spec]") {
    // Frozen pi v1.3.0 (`inline-interrupt.json`, "a successful inline execution
    // returns its value and text output"): a budgeted execution that finishes
    // reports ok with the value, the text output, no calls, and a present (but
    // empty) write set.
    auto sandbox = make_sandbox();
    auto budget_limits = limits();
    budget_limits.interrupt_budget = codemode::kDefaultInterruptBudget;

    auto result = run_async(sandbox->run("text('inline'); return 7", {}, budget_limits, {}, nullptr));

    REQUIRE(result.has_value());
    CHECK_FALSE(result->error.has_value());
    REQUIRE(result->value_json.has_value());
    CHECK(*result->value_json == "7");
    REQUIRE(result->output.size() == 1);
    CHECK(result->output[0].data == "inline");
    CHECK(result->calls.empty());
    REQUIRE(result->store_writes.has_value());
    CHECK(result->store_writes->set.empty());
    CHECK(result->store_writes->deleted.empty());
}

TEST_CASE("a public codemode caller stops pure computation with the interrupt budget, not the deadline",
        "[codemode][inline][issue1097][spec]") {
    // Frozen pi v1.3.0 (`inline-interrupt.json`, "an inline script that
    // computes without awaiting is stopped by its interrupt budget, not the
    // deadline"): both a tight loop and a tight await-loop exceed the budget
    // (polls counted across the whole execution) and fail as a timeout with
    // the frozen budget message — even with an earlier deadline pending.
    //
    // The budget is scaled to this harness's pinned guest (#874 quickjs.wasi
    // 3.2): it polls its interrupt handler about two orders of magnitude less
    // often than pi v1.3.0's guest, so frozen pi's 1000-poll capture would take
    // over 20 s here. The frozen contract under test is that polls are counted
    // across the whole execution and reported with this exact message shape.
    auto sandbox = make_sandbox();
    auto budget_limits = limits();
    budget_limits.timeout = std::chrono::seconds{30};
    budget_limits.interrupt_budget = 20;

    for (const char* script : {"while (true) {}", "while (true) await null"}) {
        auto result = run_async(sandbox->run(script, {}, budget_limits, {}, nullptr));
        REQUIRE(result.has_value());
        REQUIRE(result->error.has_value());
        CHECK(result->error->kind == codemode::CodemodeError::Kind::Timeout);
        CHECK(result->error->message == "Execution exceeded its interrupt budget of 20");
    }
}

TEST_CASE("a public codemode caller counts the interrupt budget across tool waits",
        "[codemode][inline][issue1097][spec]") {
    // Frozen pi v1.3.0 (`inline-interrupt.json`, "the interrupt budget counts
    // across awaits on tools"): a loop that alternates compute and awaited
    // tool calls keeps spending the same budget; the run fails with the
    // budget message after the calls that fit. (The frozen capture's exact
    // call count depends on pi's poll rate; the contract here is the error
    // plus completed settled calls.)
    auto sandbox = make_sandbox();
    auto budget_limits = limits();
    budget_limits.timeout = std::chrono::seconds{30};
    budget_limits.interrupt_budget = 20;
    const std::vector<codemode::CodemodeToolDescriptor> tools{
            {.name = "tick", .js_name = "tick", .description = "Count one tick."},
    };
    codemode::CodemodeToolCallHandler handler = [](std::string_view, std::string_view, std::stop_token) {
        return support::AsyncResult<std::string>{support::Expected<std::string>{std::string{"null"}}};
    };

    auto result =
            run_async(sandbox->run("let n = 0;\nfor (;;) { for (let i = 0; i < 100; i++) n += i; await tools.tick(); }",
                    tools,
                    budget_limits,
                    {},
                    std::move(handler)));

    REQUIRE(result.has_value());
    REQUIRE(result->error.has_value());
    CHECK(result->error->kind == codemode::CodemodeError::Kind::Timeout);
    CHECK(result->error->message == "Execution exceeded its interrupt budget of 20");
    CHECK(result->calls.size() >= 2);
    // Every settled call is `ok`; the call in flight when the budget ran out is
    // reported cancelled by the finish sweep.
    const auto settled_calls = result->calls.size() - 1;
    CHECK(std::ranges::all_of(result->calls.begin(),
            result->calls.begin() + static_cast<std::ptrdiff_t>(settled_calls),
            [](const codemode::CodemodeCallRecord& call) {
                return call.status == codemode::CodemodeCallRecord::Status::Ok;
            }));
}

TEST_CASE("a public codemode caller reports the deadline when a tool waits past it and keeps partial output",
        "[codemode][inline][issue1097][spec]") {
    // Frozen pi v1.3.0 (`inline-interrupt.json`, "an inline deadline while a
    // tool waits reports the deadline, not the interrupt budget"): the run
    // fails with the deadline message, the output emitted before the call is
    // kept, and the in-flight call is cancelled with a duration.
    auto sandbox = make_sandbox();
    auto deadline_limits = limits();
    deadline_limits.timeout = std::chrono::milliseconds{40};
    const std::vector<codemode::CodemodeToolDescriptor> tools{
            {.name = "hang", .js_name = "hang", .description = "Hang until cancelled."},
    };
    std::atomic<bool> tool_aborted{false};
    codemode::CodemodeToolCallHandler handler = [&tool_aborted](std::string_view,
                                                        std::string_view,
                                                        std::stop_token signal) -> support::AsyncResult<std::string> {
        return support::detail::make_async_result(
                [&tool_aborted, signal]() -> boost::asio::awaitable<support::Expected<std::string>> {
                    auto executor = co_await boost::asio::this_coro::executor;
                    boost::asio::steady_timer timer{executor};
                    while (!signal.stop_requested()) {
                        timer.expires_after(std::chrono::milliseconds{5});
                        co_await timer.async_wait(boost::asio::use_awaitable);
                    }
                    tool_aborted.store(true);
                    co_return std::unexpected(support::make_error(support::ErrorCode::Cancelled, "aborted"));
                });
    };

    auto result = run_async(sandbox->run("text('partial'); await tools.hang(); return 'unreachable';",
            tools,
            deadline_limits,
            {},
            std::move(handler)));

    REQUIRE(result.has_value());
    REQUIRE(result->error.has_value());
    CHECK(result->error->kind == codemode::CodemodeError::Kind::Timeout);
    CHECK(result->error->message == "Execution timed out after 40 ms");
    REQUIRE(result->output.size() == 1);
    CHECK(result->output[0].data == "partial");
    REQUIRE(result->calls.size() == 1);
    CHECK(result->calls[0].status == codemode::CodemodeCallRecord::Status::Cancelled);
    CHECK(result->calls[0].duration_ms.has_value());
    CHECK(tool_aborted.load());
}

TEST_CASE("a public codemode caller aborting while a tool waits keeps partial output and cancels the call",
        "[codemode][inline][issue1097][spec]") {
    // Frozen pi v1.3.0 (`inline-interrupt.json`, "caller abort during an
    // inline tool wait aborts the tool and keeps partial output"): the run
    // settles aborted with the reason's message, the output before the call is
    // kept, and the in-flight call is cancelled with a duration.
    auto sandbox = make_sandbox();
    const std::vector<codemode::CodemodeToolDescriptor> tools{
            {.name = "hang", .js_name = "hang", .description = "Hang until cancelled."},
    };
    std::atomic<bool> tool_aborted{false};
    std::stop_source cancel;
    codemode::CodemodeToolCallHandler handler = [&tool_aborted, &cancel](std::string_view,
                                                        std::string_view,
                                                        std::stop_token signal) -> support::AsyncResult<std::string> {
        return support::detail::make_async_result(
                [&tool_aborted, &cancel, signal]() -> boost::asio::awaitable<support::Expected<std::string>> {
                    // The caller aborts while the tool is in flight.
                    cancel.request_stop();
                    auto executor = co_await boost::asio::this_coro::executor;
                    boost::asio::steady_timer timer{executor};
                    while (!signal.stop_requested()) {
                        timer.expires_after(std::chrono::milliseconds{5});
                        co_await timer.async_wait(boost::asio::use_awaitable);
                    }
                    tool_aborted.store(true);
                    co_return std::unexpected(support::make_error(support::ErrorCode::Cancelled, "aborted"));
                });
    };

    auto result = run_async(sandbox->run("text('partial'); await tools.hang(); return 'unreachable';",
            tools,
            limits(),
            cancel.get_token(),
            std::move(handler),
            {},
            std::string{"cancelled"}));

    REQUIRE(result.has_value());
    REQUIRE(result->error.has_value());
    CHECK(result->error->kind == codemode::CodemodeError::Kind::Aborted);
    CHECK(result->error->message == "cancelled");
    REQUIRE(result->output.size() == 1);
    CHECK(result->output[0].data == "partial");
    REQUIRE(result->calls.size() == 1);
    CHECK(result->calls[0].status == codemode::CodemodeCallRecord::Status::Cancelled);
    CHECK(result->calls[0].duration_ms.has_value());
    CHECK(tool_aborted.load());
    CHECK_FALSE(result->value_json.has_value());
}

TEST_CASE("a public codemode caller reports a pre-aborted run with the abort reason and never starts it",
        "[codemode][inline][issue1097][spec]") {
    // Frozen pi v1.3.0 (`inline-interrupt.json`, "an inline execution with an
    // already-aborted signal never creates a VM"): the run settles as aborted
    // with the reason's message, with no output and no calls.
    auto sandbox = make_sandbox();
    std::stop_source cancel;
    cancel.request_stop();

    auto result = run_async(sandbox->run(
            "text('unreachable'); return 1", {}, limits(), cancel.get_token(), nullptr, {}, std::string{"early"}));

    REQUIRE(result.has_value());
    REQUIRE(result->error.has_value());
    CHECK(result->error->kind == codemode::CodemodeError::Kind::Aborted);
    CHECK(result->error->message == "early");
    CHECK(result->output.empty());
    CHECK(result->calls.empty());
    CHECK_FALSE(result->value_json.has_value());
}

TEST_CASE("a public codemode caller reads a store snapshot across calls and drops failed writes",
        "[codemode][store][issue1091][spec]") {
    auto sandbox = make_sandbox();
    std::map<std::string, support::JsonValue> snapshot;
    snapshot.emplace("seed", support::JsonValue{7});
    snapshot.emplace("stale", support::JsonValue{1});

    auto first = run_async(sandbox->run("store(\"counter\", 41);\n"
                                        "store(\"stale\", undefined);\n"
                                        "return load(\"seed\");",
            {},
            limits(),
            {},
            {},
            snapshot));

    REQUIRE(first.has_value());
    CHECK_FALSE(first->error.has_value());
    REQUIRE(first->value_json.has_value());
    CHECK(*first->value_json == "7");
    // Frozen rule: a successful run reports exactly its writes — one update
    // and one delete; the untouched snapshot key is not a write.
    REQUIRE(first->store_writes.has_value());
    REQUIRE(first->store_writes->set.size() == 1);
    const auto counter = first->store_writes->set.find("counter");
    REQUIRE(counter != first->store_writes->set.end());
    const auto* counter_value = counter->second.get_if<double>();
    REQUIRE(counter_value != nullptr);
    CHECK(*counter_value == 41.0);
    CHECK(first->store_writes->deleted == std::vector<std::string>{"stale"});

    // Cross-call snapshot: the caller merges the reported writes into the
    // next run's snapshot, so load() sees the update and the delete.
    for (const auto& key : first->store_writes->deleted)
        snapshot.erase(key);
    for (auto& [key, value] : first->store_writes->set)
        snapshot[key] = value;

    auto second = run_async(
            sandbox->run("return [load(\"counter\"), load(\"stale\")];", {}, limits(), {}, {}, std::move(snapshot)));

    REQUIRE(second.has_value());
    CHECK_FALSE(second->error.has_value());
    REQUIRE(second->value_json.has_value());
    CHECK(*second->value_json == "[41,null]");

    // Frozen rule: only successful executions report writes.
    auto failed = run_async(sandbox->run("store(\"lost\", 1);\nthrow new Error(\"x\");", {}, limits()));
    REQUIRE(failed.has_value());
    REQUIRE(failed->error.has_value());
    CHECK(failed->error->kind == codemode::CodemodeError::Kind::Script);
    CHECK_FALSE(failed->store_writes.has_value());
}

TEST_CASE("a public codemode caller records an unawaited call as cancelled and aborts it",
        "[codemode][calls][issue1091][spec]") {
    // #1091 frozen rule: a call the script never awaited is aborted when the
    // script returns, and its record is cancelled with a duration.
    auto sandbox = make_sandbox();
    bool tool_aborted = false;
    const std::vector<codemode::CodemodeToolDescriptor> tools{
            {.name = "hang", .js_name = "hang", .description = "Hang until cancelled."},
    };
    codemode::CodemodeToolCallHandler handler = [&tool_aborted](
                                                        std::string_view, std::string_view, std::stop_token signal) {
        return support::detail::make_async_result(
                [&tool_aborted, signal]() mutable -> boost::asio::awaitable<support::Expected<std::string>> {
                    auto executor = co_await boost::asio::this_coro::executor;
                    boost::asio::steady_timer timer(executor);
                    while (!signal.stop_requested()) {
                        timer.expires_after(std::chrono::milliseconds{5});
                        const auto [error] =
                                co_await timer.async_wait(boost::asio::as_tuple(boost::asio::use_awaitable));
                        (void)error;
                    }
                    tool_aborted = true;
                    co_return std::unexpected(support::make_error(support::ErrorCode::Validation, "tool aborted"));
                });
    };

    auto result = run_async(sandbox->run("tools.hang();\nreturn \"done\";", tools, limits(), {}, std::move(handler)));

    REQUIRE(result.has_value());
    CHECK_FALSE(result->error.has_value());
    REQUIRE(result->value_json.has_value());
    CHECK(*result->value_json == "\"done\"");
    REQUIRE(result->calls.size() == 1);
    CHECK(result->calls[0].name == "hang");
    CHECK(result->calls[0].status == codemode::CodemodeCallRecord::Status::Cancelled);
    CHECK(result->calls[0].duration_ms.has_value());
    // The frozen scenario asserts the host call observed the abort.
    CHECK(tool_aborted);
}

TEST_CASE("a public codemode caller records settled calls with ok and error statuses",
        "[codemode][calls][issue1091][spec]") {
    auto sandbox = make_sandbox();
    const std::vector<codemode::CodemodeToolDescriptor> tools{
            {.name = "ok", .js_name = "ok", .description = "Succeed."},
            {.name = "fail", .js_name = "fail", .description = "Reject the call."},
    };
    codemode::CodemodeToolCallHandler handler = [](std::string_view name, std::string_view, std::stop_token) {
        if (name == "ok") {
            return support::AsyncResult<std::string>{support::Expected<std::string>{std::string{"1"}}};
        }
        return support::AsyncResult<std::string>{support::Expected<std::string>{
                std::unexpected(support::make_error(support::ErrorCode::Validation, "boom"))}};
    };

    auto result = run_async(sandbox->run("const one = await tools.ok();\n"
                                         "let caught = \"\";\n"
                                         "try { await tools.fail(); } catch (error) { caught = error.message; }\n"
                                         "return [one, caught];",
            tools,
            limits(),
            {},
            std::move(handler)));

    REQUIRE(result.has_value());
    CHECK_FALSE(result->error.has_value());
    REQUIRE(result->value_json.has_value());
    CHECK(*result->value_json == R"([1,"boom"])");
    REQUIRE(result->calls.size() == 2);
    CHECK(result->calls[0].name == "ok");
    CHECK(result->calls[0].status == codemode::CodemodeCallRecord::Status::Ok);
    CHECK(result->calls[0].duration_ms.has_value());
    CHECK(result->calls[1].name == "fail");
    CHECK(result->calls[1].status == codemode::CodemodeCallRecord::Status::Error);
    CHECK(result->calls[1].duration_ms.has_value());
}

TEST_CASE("a public codemode caller binds a dotted global into its frozen namespace and spreads arguments",
        "[codemode][globals][issue1091][spec]") {
    // pi `globalsJson`: a dotted global binds as a member of a frozen namespace
    // object; with `spread` the guest passes the whole argument array instead of
    // the first argument.
    auto sandbox = make_sandbox({
            {.name = "models.classify", .spread = true},
    });
    std::atomic<bool> called{false};
    codemode::CodemodeToolCallHandler handler = [&called](std::string_view name,
                                                        std::string_view arguments,
                                                        std::stop_token) {
        called.store(true);
        CHECK(name == "models.classify");
        CHECK(arguments == R"([{"input":"x"}])");
        return support::AsyncResult<std::string>{support::Expected<std::string>{std::string{R"("classified")"}}};
    };

    auto result = run_async(
            sandbox->run("return await models.classify({ input: 'x' });", {}, limits(), {}, std::move(handler)));

    REQUIRE(result.has_value());
    CHECK_FALSE(result->error.has_value());
    CHECK(called.load());
    REQUIRE(result->value_json.has_value());
    CHECK(*result->value_json == R"("classified")");
}

TEST_CASE("a public codemode caller rejects invalid globals at construction with the frozen errors",
        "[codemode][globals][issue1091][spec]") {
    // #1091 frozen observations (audit-1053 script-execution.json): identifier
    // shape, one reserved name, duplicates, and namespace conflicts are
    // construction errors with these exact messages.
    const std::pair<std::vector<codemode::CodemodeGlobal>, std::string> cases[] = {
            {{codemode::CodemodeGlobal{.name = "tools"}}, "Invalid global name \"tools\""},
            {{codemode::CodemodeGlobal{.name = "a-b"}}, "Invalid global name \"a-b\""},
            {{codemode::CodemodeGlobal{.name = "x"}, codemode::CodemodeGlobal{.name = "x"}},
                    "Global \"x\" is already registered"},
            {{codemode::CodemodeGlobal{.name = "ns"}, codemode::CodemodeGlobal{.name = "ns.m"}},
                    "Global \"ns\" conflicts with the namespace \"ns\""},
    };
    for (const auto& [globals, expected] : cases) {
        auto created = codemode::CodemodeSandbox::create(codemode::default_codemode_guest_wasm_path(), globals);
        CHECK_FALSE(created.has_value());
        if (!created) {
            CHECK(created.error().message == expected);
        }
    }
}

TEST_CASE("an aborted public codemode caller does not start the script", "[codemode][sandbox][issue1064][spec]") {
    auto sandbox = make_sandbox();
    std::stop_source stop;
    stop.request_stop();

    auto result = run_async(sandbox->run("text('should not run'); return 1;", {}, limits(), stop.get_token()));

    REQUIRE(result.has_value());
    REQUIRE(result->error.has_value());
    CHECK(result->error->kind == codemode::CodemodeError::Kind::Aborted);
    CHECK(result->output.empty());
}

namespace {

/// One deterministic run's recorded observations, exactly what pi's
/// `startRecording` in `fixtures/pi-v1.3.0/audit-1053/scenarios.mts` collects:
/// the calls in issue order, the idles, the event order, and the fingerprint.
class ConcurrentCalls {
public:
    void push_back(const codemode::CodemodeHostCall& call) {
        std::lock_guard lock(mutex_);
        calls_.push_back(call);
    }

    [[nodiscard]] std::size_t size() const {
        std::lock_guard lock(mutex_);
        return calls_.size();
    }

    [[nodiscard]] bool empty() const { return size() == 0; }

    [[nodiscard]] codemode::CodemodeHostCall operator[](std::size_t index) const {
        std::lock_guard lock(mutex_);
        return calls_[index];
    }

private:
    mutable std::mutex mutex_;
    std::vector<codemode::CodemodeHostCall> calls_;
};

struct DeterministicRecording {
    ConcurrentCalls calls;
    std::vector<std::pair<std::int64_t, std::int64_t>> idles;
    std::vector<std::string> events;
    std::string fingerprint;
};

/// The audit's `startRecording` answer: echo the call's argument array back.
[[nodiscard]] codemode::CodemodeOutcome echo_outcome(const codemode::CodemodeHostCall& call) {
    codemode::CodemodeOutcome outcome;
    outcome.ok = true;
    outcome.value_json = call.arguments_json.value_or(std::string{"null"});
    return outcome;
}

} // namespace

TEST_CASE("a public codemode caller replays a deterministic run from the same start seed and answers",
        "[codemode][deterministic][issue1093][spec]") {
    // Frozen pi v1.3.0 (`deterministic-replay.json`, "the same script, start
    // time, seed and answers make the same calls and output"): two runs of the
    // same script with the same start, seed and answers produce identical
    // calls, value, and output, and the virtual clock and seeded randomness are
    // the frozen values.
    constexpr std::int64_t kStartedAt = 1768471200000;
    const std::string script =
            "const values = [Date.now(), performance.now(), Math.random(), Math.random(), new Date().toISOString()]; "
            "const first = await tools.echo(values); "
            "text(JSON.stringify([Date.now(), performance.now(), Math.random()])); "
            "return await tools.echo(first.concat(Math.random()));";

    auto sandbox = make_sandbox();
    const std::vector<codemode::CodemodeToolDescriptor> tools{
            {.name = "echo", .js_name = "echo", .description = "Echo the argument."},
            {.name = "wait", .js_name = "wait", .description = "Wait for the host."},
    };

    auto record = [&](DeterministicRecording& recording) -> codemode::CodemodeSandbox::Run {
        codemode::CodemodeRunOptions options;
        options.started_at = kStartedAt;
        options.seed = 1;
        options.tools = tools;
        options.timeout = std::chrono::seconds{20};
        options.callbacks.on_started = [&recording](std::string_view fingerprint) {
            recording.fingerprint = std::string{fingerprint};
            recording.events.push_back("started");
        };
        options.callbacks.on_call = [&recording](const codemode::CodemodeHostCall& call) {
            recording.calls.push_back(call);
            recording.events.push_back(std::format("call {}", call.id));
        };
        options.callbacks.on_idle = [&recording](std::int64_t seq, std::int64_t issued) {
            recording.idles.emplace_back(seq, issued);
            recording.events.push_back(std::format("idle {}", seq));
        };
        auto started = sandbox->start(script, std::move(options));
        REQUIRE(started.has_value());
        return std::move(*started);
    };

    DeterministicRecording one;
    DeterministicRecording two;
    auto first = record(one);
    auto second = record(two);

    // The caller answers every call with the audit's echo answer, numbered
    // from 1 and stamped with `startedAt + 1000 * seq`.
    auto answer_all = [&](codemode::CodemodeSandbox::Run& run, DeterministicRecording& recording) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{20};
        std::int64_t seq = 0;
        while (seq < 2 && std::chrono::steady_clock::now() < deadline) {
            // Wait for the next call, then answer the oldest unanswered one.
            if (recording.calls.size() <= static_cast<std::size_t>(seq)) {
                std::this_thread::sleep_for(std::chrono::milliseconds{2});
                continue;
            }
            const auto call = recording.calls[static_cast<std::size_t>(seq)];
            ++seq;
            auto rejected = run.deliver(seq, static_cast<double>(kStartedAt + 1000 * seq), call.id, echo_outcome(call));
            REQUIRE_FALSE(rejected.has_value());
        }
    };
    answer_all(first, one);
    answer_all(second, two);

    auto first_result = run_async(first.result());
    auto second_result = run_async(second.result());
    REQUIRE(first_result.has_value());
    REQUIRE(second_result.has_value());
    REQUIRE_FALSE(first_result->error.has_value());

    // The frozen calls: the virtual clock reads `startedAt`, `performance.now()`
    // starts at 0, and the two `Math.random()` draws are the seed-1 values.
    REQUIRE(one.calls.size() == 2);
    CHECK(one.calls[0].id == 1);
    CHECK(one.calls[0].name == "echo");
    CHECK(one.calls[0].arguments_json ==
            std::format("[{},0,0.34227450605335275,0.4620546081390098,\"2026-01-15T10:00:00.000Z\"]", kStartedAt));
    REQUIRE(one.calls[1].id == 2);
    CHECK(one.calls[1].arguments_json ==
            std::format("[{},0,0.34227450605335275,0.4620546081390098,\"2026-01-15T10:00:00.000Z\","
                        "0.056110785474501323]",
                    kStartedAt));

    // The frozen output: the clock advanced to the first answer's `now`, and
    // the third draw continues the same sequence.
    REQUIRE(first_result->output.size() == 1);
    CHECK(first_result->output[0].data == "[1768471201000,1000,0.5807179129204683]");

    // Replay equality: the second run makes the same calls with the same
    // arguments and produces the same output.
    REQUIRE(two.calls.size() == one.calls.size());
    for (std::size_t index = 0; index < one.calls.size(); ++index) {
        CHECK(two.calls[index].id == one.calls[index].id);
        CHECK(two.calls[index].name == one.calls[index].name);
        CHECK(two.calls[index].arguments_json == one.calls[index].arguments_json);
    }
    CHECK(second_result->output.size() == first_result->output.size());
    CHECK(second_result->output[0].data == first_result->output[0].data);
    CHECK(second_result->value_json == first_result->value_json);

    // The frozen idles and `during`.
    REQUIRE(one.idles.size() == 2);
    CHECK(one.idles[0] == std::pair<std::int64_t, std::int64_t>{0, 1});
    CHECK(one.idles[1] == std::pair<std::int64_t, std::int64_t>{1, 2});
    REQUIRE(first_result->during.has_value());
    CHECK(first_result->during->seq == 2);
    CHECK(first_result->during->issued == 2);

    // `finished` is set with the outcome.
    CHECK(first.finished());
}

TEST_CASE("a public codemode caller applies reversed answers by result number not arrival order",
        "[codemode][deterministic][issue1093][spec]") {
    // Frozen pi v1.3.0 (`deterministic-replay.json`, "results are applied in
    // seq order even when delivered in reverse order"): seq 2 arrives first and
    // waits for seq 1, so the script observes `["b", "a"]`.
    auto sandbox = make_sandbox();
    const std::string script = "const order = []; "
                               "await Promise.all([tools.wait(\"a\").then(() => order.push(\"a\")), "
                               "tools.wait(\"b\").then(() => order.push(\"b\"))]); return order;";

    DeterministicRecording recording;
    codemode::CodemodeRunOptions options;
    options.started_at = 1768471200000;
    options.seed = 1;
    options.tools = {
            {.name = "wait", .js_name = "wait", .description = "Wait for the host."},
    };
    options.timeout = std::chrono::seconds{20};
    options.callbacks.on_call = [&recording](const codemode::CodemodeHostCall& call) {
        recording.calls.push_back(call);
        recording.events.push_back(std::format("call {}", call.id));
    };
    options.callbacks.on_idle = [&recording](std::int64_t seq, std::int64_t issued) {
        recording.idles.emplace_back(seq, issued);
        recording.events.push_back(std::format("idle {}", seq));
    };
    auto started = sandbox->start(script, std::move(options));
    REQUIRE(started.has_value());
    auto run = std::move(*started);

    // Wait for both calls, then answer seq 2 before seq 1.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{20};
    while (recording.calls.size() < 2 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    REQUIRE(recording.calls.size() == 2);
    const auto a = recording.calls[0];
    const auto b = recording.calls[1];
    CHECK_FALSE(run.deliver(2,
                           1768471202000.0,
                           a.id,
                           codemode::CodemodeOutcome{
                                   .ok = true, .value_json = "null", .message = {}, .properties_json = std::nullopt})
                    .has_value());
    CHECK_FALSE(run.deliver(1,
                           1768471201000.0,
                           b.id,
                           codemode::CodemodeOutcome{
                                   .ok = true, .value_json = "null", .message = {}, .properties_json = std::nullopt})
                    .has_value());

    auto result = run_async(run.result());
    REQUIRE(result.has_value());
    REQUIRE_FALSE(result->error.has_value());
    CHECK(result->value_json == "[\"b\",\"a\"]");

    // The frozen idles: the script's first run with both calls issued, then
    // result 1 with both still issued.
    REQUIRE(recording.idles.size() == 2);
    CHECK(recording.idles[0] == std::pair<std::int64_t, std::int64_t>{0, 2});
    CHECK(recording.idles[1] == std::pair<std::int64_t, std::int64_t>{1, 2});
}

TEST_CASE("a public codemode caller rejects a reused result number a non-finite clock and an unknown call "
          "deterministically",
        "[codemode][deterministic][issue1093][spec]") {
    // Frozen pi v1.3.0 (`deterministic-replay.json`, "deliver rejects a wrong
    // call, a reused result number and a non-finite clock without settling the
    // run"): each refusal carries pi's exact message, and a later valid answer
    // still settles the run.
    auto sandbox = make_sandbox();
    DeterministicRecording recording;
    codemode::CodemodeRunOptions options;
    options.started_at = 1768471200000;
    options.seed = 1;
    options.tools = {
            {.name = "wait", .js_name = "wait", .description = "Wait for the host."},
    };
    options.timeout = std::chrono::seconds{20};
    options.callbacks.on_call = [&recording](
                                        const codemode::CodemodeHostCall& call) { recording.calls.push_back(call); };
    auto started = sandbox->start("return await Promise.all([tools.wait(1), tools.wait(2)])", std::move(options));
    REQUIRE(started.has_value());
    auto run = std::move(*started);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{20};
    while (recording.calls.size() < 2 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    REQUIRE(recording.calls.size() == 2);
    const std::array<std::int64_t, 2> call_ids{recording.calls[0].id, recording.calls[1].id};

    // A non-finite clock is refused before anything is recorded.
    const auto nan = std::numeric_limits<double>::quiet_NaN();
    auto clock = run.deliver(1,
            nan,
            call_ids[0],
            codemode::CodemodeOutcome{
                    .ok = true, .value_json = "null", .message = {}, .properties_json = std::nullopt});
    REQUIRE(clock.has_value());
    CHECK(clock->reason == codemode::CodemodeDeliveryRejection::Reason::Clock);
    CHECK(clock->message == "The clock value of result 1 is not a finite number");
    CHECK_FALSE(run.finished());

    // A valid first answer uses result 1 for call 1. Reusing it while call 2
    // remains pending is refused without settling the run.
    auto first = run.deliver(1,
            1768471200000.0,
            call_ids[0],
            codemode::CodemodeOutcome{
                    .ok = true, .value_json = "null", .message = {}, .properties_json = std::nullopt});
    CHECK_FALSE(first.has_value());
    auto duplicate = run.deliver(1,
            1768471200000.0,
            call_ids[1],
            codemode::CodemodeOutcome{
                    .ok = true, .value_json = "null", .message = {}, .properties_json = std::nullopt});
    REQUIRE(duplicate.has_value());
    CHECK(duplicate->reason == codemode::CodemodeDeliveryRejection::Reason::ResultNumber);
    CHECK(duplicate->message == "Result number 1 is not a positive integer used once");
    CHECK_FALSE(run.finished());

    // An id no call is waiting on is refused, and does not consume result 2.
    auto unknown = run.deliver(2,
            1768471200000.0,
            99,
            codemode::CodemodeOutcome{
                    .ok = true, .value_json = "null", .message = {}, .properties_json = std::nullopt});
    REQUIRE(unknown.has_value());
    CHECK(unknown->reason == codemode::CodemodeDeliveryRejection::Reason::UnknownCall);
    CHECK(unknown->message == "Call 99 is not waiting for a result");
    CHECK_FALSE(run.finished());

    // The valid seq 2 answer then settles it: refusals leave calls available.
    CHECK_FALSE(
            run.deliver(2,
                       1768471200000.0,
                       call_ids[1],
                       codemode::CodemodeOutcome{
                               .ok = true, .value_json = "\"fine\"", .message = {}, .properties_json = std::nullopt})
                    .has_value());
    auto result = run_async(run.result());
    REQUIRE(result.has_value());
    REQUIRE_FALSE(result->error.has_value());
    CHECK(result->value_json == "[null,\"fine\"]");
    CHECK(run.finished());

    // The finished guard precedes sequence validation, matching pi.
    CHECK_FALSE(
            run.deliver(1,
                       1768471200000.0,
                       call_ids[0],
                       codemode::CodemodeOutcome{
                               .ok = true, .value_json = "\"again\"", .message = {}, .properties_json = std::nullopt})
                    .has_value());
}

TEST_CASE("a public codemode caller gets the fingerprint before the first call and a reused result number is refused",
        "[codemode][deterministic][issue1093][spec]") {
    // Frozen pi v1.3.0 (`deterministic-replay.json`, "the VM fingerprint
    // arrives before calls, idles report issued calls, and a different heap
    // changes the fingerprint"): `started` precedes `call 1`, idles report the
    // calls issued after each result, and the fingerprint names the engine, the
    // prelude, the heap, and the budget.
    auto sandbox = make_sandbox();

    DeterministicRecording recording;
    codemode::CodemodeRunOptions options;
    options.started_at = 1768471200000;
    options.seed = 1;
    options.tools = {
            {.name = "echo", .js_name = "echo", .description = "Echo the argument."},
    };
    options.memory_limit_bytes = 64ull * 1024 * 1024;
    options.timeout = std::chrono::seconds{20};
    options.callbacks.on_started = [&recording](std::string_view fingerprint) {
        recording.fingerprint = std::string{fingerprint};
        recording.events.push_back("started");
    };
    options.callbacks.on_call = [&recording](const codemode::CodemodeHostCall& call) {
        recording.calls.push_back(call);
        recording.events.push_back(std::format("call {}", call.id));
    };
    options.callbacks.on_idle = [&recording](std::int64_t seq, std::int64_t issued) {
        recording.idles.emplace_back(seq, issued);
        recording.events.push_back(std::format("idle {}", seq));
    };

    auto started = sandbox->start(
            "await tools.echo(1); await Promise.all([tools.echo(2), tools.echo(3)]);", std::move(options));
    REQUIRE(started.has_value());
    auto run = std::move(*started);

    // Answer each call as it arrives, numbered in issue order.
    std::int64_t seq = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{20};
    while (seq < 3 && std::chrono::steady_clock::now() < deadline) {
        if (recording.calls.size() <= static_cast<std::size_t>(seq)) {
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
            continue;
        }
        const auto& call = recording.calls[static_cast<std::size_t>(seq)];
        ++seq;
        CHECK_FALSE(run.deliver(seq, 1768471200000.0 + static_cast<double>(seq) * 1000.0, call.id, echo_outcome(call))
                        .has_value());
    }
    auto result = run_async(run.result());
    REQUIRE(result.has_value());
    REQUIRE_FALSE(result->error.has_value());

    // `started` precedes the first call, and every call is numbered in issue order.
    REQUIRE(recording.events.size() >= 3);
    CHECK(recording.events[0] == "started");
    CHECK(recording.events[1] == "call 1");
    CHECK(recording.events[2] == "idle 0");
    REQUIRE(recording.calls.size() == 3);
    CHECK(recording.calls[0].id == 1);
    CHECK(recording.calls[1].id == 2);
    CHECK(recording.calls[2].id == 3);

    // The frozen idles: the first run issued one call, then result 1 and
    // result 2 each report all three issued calls.
    REQUIRE(recording.idles.size() == 3);
    CHECK(recording.idles[0] == std::pair<std::int64_t, std::int64_t>{0, 1});
    CHECK(recording.idles[1] == std::pair<std::int64_t, std::int64_t>{1, 3});
    CHECK(recording.idles[2] == std::pair<std::int64_t, std::int64_t>{2, 3});

    // The fingerprint names the VM, the prelude, the heap and the budget.
    CHECK(recording.fingerprint.starts_with("quickjs-wasi "));
    CHECK(recording.fingerprint.find(", quickjs ") != std::string::npos);
    CHECK(recording.fingerprint.find(", prelude ") != std::string::npos);
    CHECK(recording.fingerprint.ends_with(", heap 67108864, budget none"));

    // The same VM options keep the fingerprint; a different heap changes it.
    DeterministicRecording same;
    DeterministicRecording other;
    auto record_fingerprint = [](codemode::CodemodeSandbox& target,
                                      DeterministicRecording& into,
                                      std::uint64_t heap) -> std::unique_ptr<codemode::CodemodeSandbox::Run> {
        codemode::CodemodeRunOptions run_options;
        run_options.started_at = 1768471200000;
        run_options.seed = 1;
        run_options.memory_limit_bytes = heap;
        run_options.timeout = std::chrono::seconds{20};
        run_options.callbacks.on_started = [&into](std::string_view fingerprint) {
            into.fingerprint = std::string{fingerprint};
        };
        auto pending = target.start("return 1", std::move(run_options));
        REQUIRE(pending.has_value());
        return std::make_unique<codemode::CodemodeSandbox::Run>(std::move(*pending));
    };
    auto same_run = record_fingerprint(*sandbox, same, 64ull * 1024 * 1024);
    auto other_sandbox = make_sandbox();
    auto other_run = record_fingerprint(*other_sandbox, other, 32ull * 1024 * 1024);
    auto same_result = run_async(same_run->result());
    auto other_result = run_async(other_run->result());
    REQUIRE(same_result.has_value());
    REQUIRE(other_result.has_value());
    CHECK(same.fingerprint == recording.fingerprint);
    CHECK(other.fingerprint != recording.fingerprint);
    CHECK(other.fingerprint.ends_with(", heap 33554432, budget none"));
}

TEST_CASE("a public codemode caller gets illegal start options refused before any VM exists",
        "[codemode][deterministic][issue1093][spec]") {
    // Frozen pi v1.3.0 (`deterministic-replay.json`, "start rejects invalid
    // start times, seeds and duplicate script names"): each rejected option set
    // reports pi's exact message.
    auto sandbox = make_sandbox();

    auto rejected = [&](codemode::CodemodeRunOptions options) {
        auto started = sandbox->start("", std::move(options));
        REQUIRE_FALSE(started.has_value());
        return started.error().message;
    };

    codemode::CodemodeRunOptions negative_time;
    negative_time.started_at = -1;
    CHECK(rejected(std::move(negative_time)) ==
            "startedAt must be a non-negative number of milliseconds since the epoch");

    codemode::CodemodeRunOptions bad_seed;
    bad_seed.seed = 0x1'0000'0000ull;
    CHECK(rejected(std::move(bad_seed)) == "seed must be a 32-bit unsigned integer");

    codemode::CodemodeRunOptions duplicate_names;
    duplicate_names.tools = {
            {.name = "x", .js_name = "x", .description = {}},
            {.name = "x", .js_name = "x", .description = {}},
    };
    CHECK(rejected(std::move(duplicate_names)) == "Tool names must be unique");

    // Two different tool names whose derived script names collide: `a-b` and
    // `a_b` both become `a_b` (`toCodemodeIdentifier`).
    codemode::CodemodeRunOptions duplicate_script_names;
    duplicate_script_names.tools = {
            {.name = "a-b", .js_name = {}, .description = {}},
            {.name = "a_b", .js_name = {}, .description = {}},
    };
    CHECK(rejected(std::move(duplicate_script_names)) == "Tool script names must be unique");
}

TEST_CASE("a public codemode caller sees a seed mismatch change randomness without changing the start time",
        "[codemode][deterministic][issue1093][spec]") {
    // Frozen pi v1.3.0 (`deterministic-replay.json`, "a changed seed is a replay
    // mismatch and changes randomness without changing the start time"): the
    // same seed reproduces the draw, a different seed does not, and the clock
    // reads the same `startedAt` either way. Seed 0 is distinct from seed 1,
    // which the frozen capture pins to a different first draw.
    auto sandbox = make_sandbox();
    auto draw = [&](std::uint64_t seed) {
        codemode::CodemodeRunOptions options;
        options.started_at = 1768471200000;
        options.seed = seed;
        options.timeout = std::chrono::seconds{20};
        auto started = sandbox->start("return [Date.now(), Math.random()]", std::move(options));
        REQUIRE(started.has_value());
        auto run = std::move(*started);
        auto result = run_async(run.result());
        REQUIRE(result.has_value());
        REQUIRE_FALSE(result->error.has_value());
        return *result->value_json;
    };

    const std::string seed_one = draw(1);
    const std::string seed_one_again = draw(1);
    const std::string seed_two = draw(2);
    const std::string seed_zero = draw(0);

    CHECK(seed_one == seed_one_again);
    CHECK(seed_one != seed_two);
    CHECK(seed_zero != seed_one);
    CHECK(seed_one == "[1768471200000,0.34227450605335275]");
    CHECK(seed_zero == "[1768471200000,0.6711372530266764]");

    // Replay mismatch is a rejection observation, separate from showing the
    // changed random output: callers carry the recorded seed alongside the
    // current inputs, and a mismatch refuses before the VM is created.
    codemode::CodemodeRunOptions mismatch;
    mismatch.started_at = 1768471200000;
    mismatch.seed = 2;
    mismatch.expected_seed = 1;
    auto rejected_seed = sandbox->start("return 1", std::move(mismatch));
    REQUIRE_FALSE(rejected_seed.has_value());
    CHECK(rejected_seed.error().message == "Deterministic replay seed mismatch: expected 1, got 2");

    // Fingerprint mismatch is checked after the VM reports the fingerprint but
    // before the script runs; no call is exposed and the deterministic error is
    // part of the normal run result.
    std::string actual_fingerprint;
    codemode::CodemodeRunOptions capture;
    capture.started_at = 1768471200000;
    capture.seed = 1;
    capture.callbacks.on_started = [&actual_fingerprint](
                                           std::string_view value) { actual_fingerprint = std::string{value}; };
    auto capture_started = sandbox->start("return 1", std::move(capture));
    REQUIRE(capture_started.has_value());
    auto capture_run = std::move(*capture_started);
    auto capture_result = run_async(capture_run.result());
    REQUIRE(capture_result.has_value());
    REQUIRE_FALSE(actual_fingerprint.empty());

    std::vector<codemode::CodemodeHostCall> unexpected_calls;
    codemode::CodemodeRunOptions fingerprint_mismatch;
    fingerprint_mismatch.started_at = 1768471200000;
    fingerprint_mismatch.seed = 1;
    fingerprint_mismatch.expected_fingerprint = "different VM";
    fingerprint_mismatch.tools = {
            {.name = "echo", .js_name = "echo", .description = "Echo the argument."},
    };
    fingerprint_mismatch.callbacks.on_call = [&unexpected_calls](const codemode::CodemodeHostCall& call) {
        unexpected_calls.push_back(call);
    };
    auto mismatch_started = sandbox->start("await tools.echo(1); return 2", std::move(fingerprint_mismatch));
    REQUIRE(mismatch_started.has_value());
    auto mismatch_run = std::move(*mismatch_started);
    auto mismatch_result = run_async(mismatch_run.result());
    REQUIRE(mismatch_result.has_value());
    REQUIRE(mismatch_result->error.has_value());
    CHECK(mismatch_result->error->kind == codemode::CodemodeError::Kind::Sandbox);
    CHECK(mismatch_result->error->message.find("Deterministic replay fingerprint mismatch: expected 'different VM'") ==
            0);
    CHECK(unexpected_calls.empty());
}

TEST_CASE("a public codemode caller gets during for the result the VM was processing when the script ended",
        "[codemode][deterministic][issue1093][spec]") {
    // Frozen pi v1.3.0 (`deterministic-replay.json`, "during reports the result
    // the VM was processing when the script ended"): `return 1` reports
    // `{seq: 0, issued: 0}`, a two-call script `{seq: 2, issued: 2}`, and a
    // failing three-call script `{seq: 3, issued: 3}`.
    auto sandbox = make_sandbox();
    const std::vector<codemode::CodemodeToolDescriptor> tools{
            {.name = "echo", .js_name = "echo", .description = "Echo the argument."},
    };

    struct Case {
        std::string script;
        bool ok;
        std::int64_t seq;
        std::int64_t issued;
    };
    const std::vector<Case> cases{
            {"return 1", true, 0, 0},
            {"await tools.echo(1); return await tools.echo(2)", true, 2, 2},
            {"await tools.echo(1); await Promise.all([tools.echo(2), tools.echo(3)]); throw new Error('x')",
                    false,
                    3,
                    3},
    };

    for (const auto& test : cases) {
        ConcurrentCalls calls;
        codemode::CodemodeRunOptions options;
        options.started_at = 1768471200000;
        options.seed = 1;
        options.tools = tools;
        options.timeout = std::chrono::seconds{20};
        options.callbacks.on_call = [&calls](const codemode::CodemodeHostCall& call) { calls.push_back(call); };
        auto started = sandbox->start(test.script, std::move(options));
        REQUIRE(started.has_value());
        auto run = std::move(*started);

        std::int64_t seq = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{20};
        while (std::chrono::steady_clock::now() < deadline) {
            if (calls.size() <= static_cast<std::size_t>(seq)) {
                if (run.finished()) break;
                std::this_thread::sleep_for(std::chrono::milliseconds{2});
                continue;
            }
            const auto& call = calls[static_cast<std::size_t>(seq)];
            ++seq;
            CHECK_FALSE(
                    run.deliver(seq, 1768471200000.0 + static_cast<double>(seq) * 1000.0, call.id, echo_outcome(call))
                            .has_value());
        }
        auto result = run_async(run.result());
        REQUIRE(result.has_value());
        INFO("script: " << test.script);
        CHECK(result->error.has_value() == !test.ok);
        REQUIRE(result->during.has_value());
        CHECK(result->during->seq == test.seq);
        CHECK(result->during->issued == test.issued);
    }
}

TEST_CASE("a public codemode caller numbers globals with tools and gets rejection properties on the script Error",
        "[codemode][deterministic][issue1093][spec]") {
    // Frozen pi v1.3.0 (`deterministic-replay.json`, "globals count as issued
    // calls and rejection properties reach the script Error"): a global takes
    // call id 1, the tool takes id 2, the idles report both, the tool record is
    // `error`, and the rejection's message reaches the script.
    auto sandbox = make_sandbox();
    ConcurrentCalls calls;
    std::vector<std::pair<std::int64_t, std::int64_t>> idles;
    codemode::CodemodeRunOptions options;
    options.started_at = 1768471200000;
    options.seed = 1;
    options.tools = {
            {.name = "echo", .js_name = "echo", .description = "Echo the argument."},
    };
    options.globals = {
            {.name = "search", .spread = true},
    };
    options.timeout = std::chrono::seconds{20};
    options.callbacks.on_call = [&calls](const codemode::CodemodeHostCall& call) { calls.push_back(call); };
    options.callbacks.on_idle = [&idles](std::int64_t seq, std::int64_t issued) { idles.emplace_back(seq, issued); };
    auto started =
            sandbox->start("const found = await search(\"x\", 2); "
                           "try { await tools.echo(\"fail\"); } catch (error) { text(error.message); } return found;",
                    std::move(options));
    REQUIRE(started.has_value());
    auto run = std::move(*started);

    // Answer the global first with its value, then reject the tool call.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{20};
    std::int64_t seq = 0;
    while (seq < 2 && std::chrono::steady_clock::now() < deadline) {
        if (calls.size() <= static_cast<std::size_t>(seq)) {
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
            continue;
        }
        const auto& call = calls[static_cast<std::size_t>(seq)];
        ++seq;
        if (call.target == codemode::CodemodeHostCall::Target::Global) {
            CHECK_FALSE(run.deliver(seq,
                                   1768471200000.0 + static_cast<double>(seq) * 1000.0,
                                   call.id,
                                   codemode::CodemodeOutcome{.ok = true,
                                           .value_json = "[\"found\"]",
                                           .message = {},
                                           .properties_json = std::nullopt})
                            .has_value());
        } else {
            CHECK_FALSE(
                    run.deliver(seq,
                               1768471200000.0 + static_cast<double>(seq) * 1000.0,
                               call.id,
                               codemode::CodemodeOutcome{
                                       .ok = false, .value_json = {}, .message = "no", .properties_json = std::nullopt})
                            .has_value());
        }
    }
    auto result = run_async(run.result());
    REQUIRE(result.has_value());
    REQUIRE_FALSE(result->error.has_value());

    // The frozen call numbering: the global is id 1 and the tool id 2.
    REQUIRE(calls.size() == 2);
    CHECK(calls[0].id == 1);
    CHECK(calls[0].target == codemode::CodemodeHostCall::Target::Global);
    CHECK(calls[0].name == "search");
    CHECK(calls[0].arguments_json == "[\"x\",2]");
    CHECK(calls[1].id == 2);
    CHECK(calls[1].target == codemode::CodemodeHostCall::Target::Tool);
    CHECK(calls[1].name == "echo");
    CHECK(calls[1].arguments_json == "\"fail\"");

    // The idles report both issued calls, and only the tool has a record.
    REQUIRE(idles.size() >= 2);
    CHECK(idles[0] == std::pair<std::int64_t, std::int64_t>{0, 1});
    CHECK(idles[1] == std::pair<std::int64_t, std::int64_t>{1, 2});
    REQUIRE(result->calls.size() == 1);
    CHECK(result->calls[0].name == "echo");
    CHECK(result->calls[0].status == codemode::CodemodeCallRecord::Status::Error);
    CHECK(result->calls[0].duration_ms.has_value());

    // The rejection message reached the script and the global's value is the result.
    REQUIRE(result->output.size() == 1);
    CHECK(result->output[0].data == "no");
    CHECK(result->value_json == "[\"found\"]");
}

TEST_CASE("a public codemode caller gets an uncatchable abort and a cancelled record in a deterministic run",
        "[codemode][deterministic][issue1093][spec]") {
    // Frozen pi v1.3.0 (`deterministic-replay.json`, "abort in a deterministic
    // run is an uncatchable aborted error and finished becomes true"): the
    // script's own `catch` cannot swallow the abort, the output it wrote is
    // dropped because the run never reached it, and the pending call keeps a
    // `cancelled` record with a duration.
    auto sandbox = make_sandbox();
    ConcurrentCalls calls;
    std::stop_source cancel;
    codemode::CodemodeRunOptions options;
    options.started_at = 1768471200000;
    options.seed = 1;
    options.tools = {
            {.name = "wait", .js_name = "wait", .description = "Wait for the host."},
    };
    options.timeout = std::chrono::seconds{20};
    options.stop_token = cancel.get_token();
    options.abort_message = std::string{"diverged"};
    options.callbacks.on_call = [&calls](const codemode::CodemodeHostCall& call) { calls.push_back(call); };
    auto started = sandbox->start(
            "try { await tools.wait(1); } catch { text(\"caught\"); } text(\"after\");", std::move(options));
    REQUIRE(started.has_value());
    auto run = std::move(*started);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{20};
    while (calls.empty() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    REQUIRE(calls.size() == 1);
    cancel.request_stop();

    auto result = run_async(run.result());
    REQUIRE(result.has_value());
    REQUIRE(result->error.has_value());
    CHECK(result->error->kind == codemode::CodemodeError::Kind::Aborted);
    CHECK(result->error->message == "diverged");
    CHECK(result->output.empty());
    REQUIRE(result->calls.size() == 1);
    CHECK(result->calls[0].name == "wait");
    CHECK(result->calls[0].status == codemode::CodemodeCallRecord::Status::Cancelled);
    CHECK(result->calls[0].duration_ms.has_value());
    CHECK(run.finished());
}

TEST_CASE("a public codemode caller keeps a call waiting when its answer cannot be serialized",
        "[codemode][deterministic][issue1093][spec]") {
    // Frozen pi v1.3.0 (`deterministic-replay.json`, "a nonserializable answer
    // leaves the call waiting and a later valid answer settles it"): pi's
    // `JSON.stringify` throws before anything is recorded, so result 1 stays
    // available and the later answer settles the run.
    auto sandbox = make_sandbox();
    ConcurrentCalls calls;
    codemode::CodemodeRunOptions options;
    options.started_at = 1768471200000;
    options.seed = 1;
    options.tools = {
            {.name = "wait", .js_name = "wait", .description = "Wait for the host."},
    };
    options.timeout = std::chrono::seconds{20};
    options.callbacks.on_call = [&calls](const codemode::CodemodeHostCall& call) { calls.push_back(call); };
    auto started = sandbox->start("return await tools.wait(1)", std::move(options));
    REQUIRE(started.has_value());
    auto run = std::move(*started);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{20};
    while (calls.empty() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    REQUIRE(calls.size() == 1);

    // A value the host cannot express as JSON. pi reaches this through
    // `JSON.stringify` throwing on a BigInt; this API receives the JSON text
    // already, so the same refusal is text that is not JSON — recorded nowhere,
    // exactly as pi records nothing.
    auto bad = run.deliver(1,
            1768471200000.0,
            calls[0].id,
            codemode::CodemodeOutcome{
                    .ok = true, .value_json = "{not json}", .message = {}, .properties_json = std::nullopt});
    REQUIRE(bad.has_value());
    CHECK(bad->reason == codemode::CodemodeDeliveryRejection::Reason::Serialization);
    CHECK(bad->message == "The value of result 1 is not JSON");
    CHECK_FALSE(run.finished());

    // The same result number is still free, and the valid answer settles it.
    CHECK_FALSE(
            run.deliver(1,
                       1768471200000.0,
                       calls[0].id,
                       codemode::CodemodeOutcome{
                               .ok = true, .value_json = "\"fine\"", .message = {}, .properties_json = std::nullopt})
                    .has_value());
    auto result = run_async(run.result());
    REQUIRE(result.has_value());
    REQUIRE_FALSE(result->error.has_value());
    CHECK(result->value_json == "\"fine\"");
}

TEST_CASE("a public codemode caller gets UTC and no WeakRef in a deterministic run",
        "[codemode][deterministic][issue1093][spec]") {
    // Frozen pi v1.3.0 (`deterministic-replay.json`, "deterministic runs use a
    // virtual UTC clock and seeded randomness without WeakRef"): the clock
    // reads `startedAt`, `performance.now()` is 0, `getHours()` on the epoch is
    // 0 in UTC, the timezone offset is 0, and `WeakRef`/`FinalizationRegistry`
    // do not exist.
    auto sandbox = make_sandbox();
    codemode::CodemodeRunOptions options;
    options.started_at = 1768471200000;
    options.seed = 1;
    options.timeout = std::chrono::seconds{20};
    auto started = sandbox->start("return [Date.now(), performance.now(), Math.random(), new Date(0).getHours(), "
                                  "new Date(0).getTimezoneOffset(), typeof WeakRef, typeof FinalizationRegistry]",
            std::move(options));
    REQUIRE(started.has_value());
    auto run = std::move(*started);
    auto result = run_async(run.result());
    REQUIRE(result.has_value());
    REQUIRE_FALSE(result->error.has_value());
    CHECK(result->value_json == "[1768471200000,0,0.34227450605335275,0,0,\"undefined\",\"undefined\"]");
}
