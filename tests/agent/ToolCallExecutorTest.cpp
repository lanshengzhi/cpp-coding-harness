#include "../../third_party/catch2/catch_test_macros.hpp"

#include "agent/ToolCallExecutor.hpp"

#include "../../include/cch/agent/AgentContext.hpp"
#include "../../include/cch/agent/ToolRegistry.hpp"
#include "../../include/cch/ai/Content.hpp"
#include "../../include/cch/ai/Context.hpp"
#include "../../include/cch/ai/Message.hpp"
#include "../../include/cch/ai/Tool.hpp"
#include "../../include/cch/util/Error.hpp"
#include "util/Json.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

using namespace cch;

namespace {

struct ConcurrencyProbe {
    std::atomic<int> active{0};
    std::atomic<int> max_active{0};
};

class RecordingTool final : public agent::AsyncAgentTool {
public:
    RecordingTool(
        ai::Tool definition,
        agent::ToolConcurrency concurrency = agent::ToolConcurrency::Exclusive,
        std::string result_text = "tool says ok",
        std::chrono::milliseconds delay = {},
        ConcurrencyProbe* probe = nullptr)
        : definition_(std::move(definition)),
          concurrency_(concurrency),
          result_text_(std::move(result_text)),
          delay_(delay),
          probe_(probe) {}

    const ai::Tool& definition() const override { return definition_; }

    agent::ToolConcurrency concurrency() const noexcept override {
        return concurrency_;
    }

    boost::asio::awaitable<util::Expected<agent::AsyncToolExecutionResult>> execute(
        agent::ToolInvocation invocation) override {
        {
            std::lock_guard lock(invocations_mutex_);
            invocations_.push_back(std::move(invocation));
        }

        if (probe_ != nullptr) {
            const int current = ++probe_->active;
            int observed = probe_->max_active.load();
            while (current > observed &&
                   !probe_->max_active.compare_exchange_weak(observed, current)) {}
        }

        if (delay_.count() > 0) {
            auto timer = boost::asio::steady_timer(
                co_await boost::asio::this_coro::executor,
                delay_);
            co_await timer.async_wait(boost::asio::use_awaitable);
        }

        if (probe_ != nullptr) {
            --probe_->active;
        }

        co_return agent::AsyncToolExecutionResult{
            std::vector<ai::Content>{ai::text_content(result_text_)},
            std::nullopt,
            false};
    }

    [[nodiscard]] std::size_t invocation_count() const {
        std::lock_guard lock(invocations_mutex_);
        return invocations_.size();
    }

private:
    ai::Tool definition_;
    agent::ToolConcurrency concurrency_;
    std::string result_text_;
    std::chrono::milliseconds delay_;
    ConcurrencyProbe* probe_{};
    mutable std::mutex invocations_mutex_;
    std::vector<agent::ToolInvocation> invocations_;
};

class FailingTool final : public agent::AsyncAgentTool {
public:
    explicit FailingTool(ai::Tool definition) : definition_(std::move(definition)) {}

    const ai::Tool& definition() const override { return definition_; }

    boost::asio::awaitable<util::Expected<agent::AsyncToolExecutionResult>> execute(
        agent::ToolInvocation) override {
        co_return std::unexpected(util::make_error(
            util::ErrorCode::Tool,
            "tool failed",
            "boom"));
    }

private:
    ai::Tool definition_;
};

struct ExecuteResult {
    util::Expected<agent::ToolCallBatchResult> result;
    std::vector<agent::AgentLifecycleEvent> events;
};

ExecuteResult run_executor(
    agent::ToolCallExecutor& executor,
    const ai::AssistantMessage& assistant_message,
    const ai::AiContext& context = {}) {
    boost::asio::thread_pool pool{4};
    std::optional<util::Expected<agent::ToolCallBatchResult>> result;
    std::vector<agent::AgentLifecycleEvent> events;
    std::mutex events_mutex;

    agent::AgentEventSink sink{
        [&](const agent::AgentLifecycleEvent& event) {
            std::lock_guard lock(events_mutex);
            events.push_back(event);
            return util::ExpectedVoid{};
        }};

    boost::asio::co_spawn(
        pool,
        [&]() -> boost::asio::awaitable<void> {
            result = co_await executor.execute(
                agent::ToolCallBatchRequest{assistant_message, context},
                sink);
            co_return;
        },
        boost::asio::detached);

    pool.join();
    REQUIRE(result.has_value());
    return ExecuteResult{std::move(*result), std::move(events)};
}

template <typename T>
std::size_t count_events(const std::vector<agent::AgentLifecycleEvent>& events) {
    std::size_t count = 0;
    for (const auto& event : events) {
        if (std::holds_alternative<T>(event)) {
            ++count;
        }
    }
    return count;
}

ai::ToolCallContent make_call(
    std::string id,
    std::string name,
    std::string raw_arguments = R"({"x":1})") {
    auto arguments = util::read_json<util::JsonValue>(raw_arguments);
    ai::ToolCallContent call;
    call.id = std::move(id);
    call.name = std::move(name);
    call.raw_arguments = raw_arguments;
    if (arguments) {
        call.arguments = *arguments;
    } else {
        call.arguments_valid = false;
        call.argument_error = arguments.error().detail;
    }
    return call;
}

ai::AssistantMessage assistant_with_calls(
    std::vector<ai::ToolCallContent> calls) {
    ai::AssistantMessage message;
    message.stop_reason = ai::AssistantStopReason::ToolUse;
    for (auto& call : calls) {
        message.content.emplace_back(std::move(call));
    }
    return message;
}

} // namespace

TEST_CASE("ToolCallExecutor is a move-only private adapter", "[agent][tool-executor]") {
    static_assert(!std::is_copy_constructible_v<agent::ToolCallExecutor>);
    static_assert(std::is_move_constructible_v<agent::ToolCallExecutor>);
}

TEST_CASE("ToolCallExecutor defaults to source-order sequential execution", "[agent][tool-executor]") {
    agent::AsyncToolRegistry registry;
    auto tool = std::make_unique<RecordingTool>(
        ai::Tool{"read_file", "Read", ai::JsonSchema::object()});
    auto* tool_ptr = tool.get();
    REQUIRE(registry.add(std::move(tool)));

    agent::ToolCallExecutor executor{registry, agent::ToolCallExecutorOptions{}};
    auto assistant = assistant_with_calls({make_call("call-1", "read_file")});

    auto run = run_executor(executor, assistant);

    REQUIRE(run.result);
    REQUIRE(run.result->results.size() == 1);
    CHECK_FALSE(run.result->terminate_batch);
    CHECK(tool_ptr->invocation_count() == 1);
    CHECK(count_events<agent::ToolExecutionStartEvent>(run.events) == 1);
    CHECK(count_events<agent::ToolExecutionEndEvent>(run.events) == 1);
    CHECK(count_events<agent::MessageStartEvent>(run.events) == 1);
    CHECK(count_events<agent::MessageEndEvent>(run.events) == 1);
}

TEST_CASE("exclusive tools force a bounded batch to execute sequentially", "[agent][tool-executor]") {
    ConcurrencyProbe probe;
    agent::AsyncToolRegistry registry;
    REQUIRE(registry.add(std::make_unique<RecordingTool>(
        ai::Tool{"alpha", "Alpha", ai::JsonSchema::object()},
        agent::ToolConcurrency::ParallelSafe,
        "alpha",
        std::chrono::milliseconds{25},
        &probe)));
    REQUIRE(registry.add(std::make_unique<RecordingTool>(
        ai::Tool{"beta", "Beta", ai::JsonSchema::object()},
        agent::ToolConcurrency::Exclusive,
        "beta",
        std::chrono::milliseconds{25},
        &probe)));

    agent::ToolCallExecutorOptions options;
    options.execution = agent::BoundedParallelToolExecution{2};
    agent::ToolCallExecutor executor{registry, std::move(options)};
    auto assistant = assistant_with_calls({
        make_call("call-1", "alpha"),
        make_call("call-2", "beta"),
    });

    auto run = run_executor(executor, assistant);

    REQUIRE(run.result);
    CHECK(probe.max_active.load() == 1);
}

TEST_CASE("bounded parallel execution enforces max_in_flight", "[agent][tool-executor]") {
    ConcurrencyProbe probe;
    agent::AsyncToolRegistry registry;
    REQUIRE(registry.add(std::make_unique<RecordingTool>(
        ai::Tool{"lookup", "Lookup", ai::JsonSchema::object()},
        agent::ToolConcurrency::ParallelSafe,
        "ok",
        std::chrono::milliseconds{30},
        &probe)));

    agent::ToolCallExecutorOptions options;
    options.execution = agent::BoundedParallelToolExecution{2};
    agent::ToolCallExecutor executor{registry, std::move(options)};
    auto assistant = assistant_with_calls({
        make_call("call-1", "lookup"),
        make_call("call-2", "lookup"),
        make_call("call-3", "lookup"),
        make_call("call-4", "lookup"),
    });

    auto run = run_executor(executor, assistant);

    REQUIRE(run.result);
    REQUIRE(run.result->results.size() == 4);
    CHECK(probe.max_active.load() == 2);
}

TEST_CASE("bounded parallel execution accepts a limit of one", "[agent][tool-executor]") {
    ConcurrencyProbe probe;
    agent::AsyncToolRegistry registry;
    REQUIRE(registry.add(std::make_unique<RecordingTool>(
        ai::Tool{"lookup", "Lookup", ai::JsonSchema::object()},
        agent::ToolConcurrency::ParallelSafe,
        "ok",
        std::chrono::milliseconds{20},
        &probe)));

    agent::ToolCallExecutorOptions options;
    options.execution = agent::BoundedParallelToolExecution{1};
    agent::ToolCallExecutor executor{registry, std::move(options)};
    auto assistant = assistant_with_calls({
        make_call("call-1", "lookup"),
        make_call("call-2", "lookup"),
    });

    auto run = run_executor(executor, assistant);

    REQUIRE(run.result);
    CHECK(probe.max_active.load() == 1);
}

TEST_CASE("bounded parallel execution rejects zero before events or tools", "[agent][tool-executor]") {
    agent::AsyncToolRegistry registry;
    auto tool = std::make_unique<RecordingTool>(
        ai::Tool{"lookup", "Lookup", ai::JsonSchema::object()},
        agent::ToolConcurrency::ParallelSafe);
    auto* tool_ptr = tool.get();
    REQUIRE(registry.add(std::move(tool)));

    agent::ToolCallExecutorOptions options;
    options.execution = agent::BoundedParallelToolExecution{0};
    agent::ToolCallExecutor executor{registry, std::move(options)};
    auto assistant = assistant_with_calls({make_call("call-1", "lookup")});

    auto run = run_executor(executor, assistant);

    REQUIRE_FALSE(run.result);
    CHECK(run.result.error().code == util::ErrorCode::Validation);
    CHECK(run.events.empty());
    CHECK(tool_ptr->invocation_count() == 0);
}

TEST_CASE("bounded parallel execution preserves source-order results", "[agent][tool-executor]") {
    agent::AsyncToolRegistry registry;
    REQUIRE(registry.add(std::make_unique<RecordingTool>(
        ai::Tool{"alpha", "Alpha", ai::JsonSchema::object()},
        agent::ToolConcurrency::ParallelSafe,
        "alpha result",
        std::chrono::milliseconds{80})));
    REQUIRE(registry.add(std::make_unique<RecordingTool>(
        ai::Tool{"beta", "Beta", ai::JsonSchema::object()},
        agent::ToolConcurrency::ParallelSafe,
        "beta result",
        std::chrono::milliseconds{10})));

    agent::ToolCallExecutorOptions options;
    options.execution = agent::BoundedParallelToolExecution{2};
    agent::ToolCallExecutor executor{registry, std::move(options)};
    auto assistant = assistant_with_calls({
        make_call("call-1", "alpha"),
        make_call("call-2", "beta"),
    });

    auto run = run_executor(executor, assistant);

    REQUIRE(run.result);
    REQUIRE(run.result->results.size() == 2);
    CHECK(run.result->results[0].tool_name == "alpha");
    CHECK(run.result->results[1].tool_name == "beta");

    std::vector<std::string> end_order;
    for (const auto& event : run.events) {
        if (const auto* end = std::get_if<agent::ToolExecutionEndEvent>(&event)) {
            end_order.push_back(end->tool_name);
        }
    }
    REQUIRE(end_order.size() == 2);
    CHECK(end_order[0] == "beta");
    CHECK(end_order[1] == "alpha");

    std::vector<std::string> message_order;
    for (const auto& event : run.events) {
        if (const auto* end = std::get_if<agent::MessageEndEvent>(&event)) {
            if (const auto* result = std::get_if<ai::ToolResultMessage>(&end->message)) {
                message_order.push_back(result->tool_name);
            }
        }
    }
    REQUIRE(message_order.size() == 2);
    CHECK(message_order[0] == "alpha");
    CHECK(message_order[1] == "beta");
}

TEST_CASE("bounded parallel execution serializes hooks and lifecycle callbacks", "[agent][tool-executor]") {
    agent::AsyncToolRegistry registry;
    REQUIRE(registry.add(std::make_unique<RecordingTool>(
        ai::Tool{"alpha", "Alpha", ai::JsonSchema::object()},
        agent::ToolConcurrency::ParallelSafe,
        "alpha",
        std::chrono::milliseconds{10})));
    REQUIRE(registry.add(std::make_unique<RecordingTool>(
        ai::Tool{"beta", "Beta", ai::JsonSchema::object()},
        agent::ToolConcurrency::ParallelSafe,
        "beta",
        std::chrono::milliseconds{10})));

    std::atomic<int> active_callbacks{0};
    std::atomic<int> max_callbacks{0};
    auto enter_callback = [&] {
        const int current = ++active_callbacks;
        int observed = max_callbacks.load();
        while (current > observed &&
               !max_callbacks.compare_exchange_weak(observed, current)) {}
        std::this_thread::sleep_for(std::chrono::milliseconds{15});
        --active_callbacks;
    };

    agent::ToolCallExecutorOptions options;
    options.execution = agent::BoundedParallelToolExecution{2};
    options.after_tool_call = [&](const agent::AfterToolCallContext&)
        -> util::Expected<agent::AfterToolCallResult> {
        enter_callback();
        return agent::AfterToolCallResult{};
    };
    agent::ToolCallExecutor executor{registry, std::move(options)};
    auto assistant = assistant_with_calls({
        make_call("call-1", "alpha"),
        make_call("call-2", "beta"),
    });
    ai::AiContext context;

    boost::asio::thread_pool pool{4};
    std::optional<util::Expected<agent::ToolCallBatchResult>> result;
    agent::AgentEventSink sink{
        [&](const agent::AgentLifecycleEvent& event) -> util::ExpectedVoid {
            if (std::holds_alternative<agent::ToolExecutionEndEvent>(event)) {
                enter_callback();
            }
            return {};
        }};
    boost::asio::co_spawn(
        pool,
        [&]() -> boost::asio::awaitable<void> {
            result = co_await executor.execute(
                agent::ToolCallBatchRequest{assistant, context},
                sink);
            co_return;
        },
        boost::asio::detached);
    pool.join();

    REQUIRE(result.has_value());
    REQUIRE(*result);
    CHECK(max_callbacks.load() == 1);
}

TEST_CASE("ToolCallExecutor maps lookup and argument failures to results", "[agent][tool-executor]") {
    agent::AsyncToolRegistry registry;
    REQUIRE(registry.add(std::make_unique<RecordingTool>(
        ai::Tool{"read_file", "Read", ai::JsonSchema::object()})));
    agent::ToolCallExecutor executor{registry, agent::ToolCallExecutorOptions{}};
    auto assistant = assistant_with_calls({
        make_call("call-1", "missing"),
        make_call("call-2", "read_file", "not-json"),
    });

    auto run = run_executor(executor, assistant);

    REQUIRE(run.result);
    REQUIRE(run.result->results.size() == 2);
    CHECK(run.result->results[0].is_error);
    CHECK(run.result->results[1].is_error);
    CHECK(count_events<agent::ToolExecutionEndEvent>(run.events) == 2);
    CHECK(count_events<agent::MessageStartEvent>(run.events) == 2);
    CHECK(count_events<agent::MessageEndEvent>(run.events) == 2);
}

TEST_CASE("ToolCallExecutor keeps hook policy behind its interface", "[agent][tool-executor]") {
    agent::AsyncToolRegistry registry;
    auto tool = std::make_unique<RecordingTool>(
        ai::Tool{"read_file", "Read", ai::JsonSchema::object()});
    auto* tool_ptr = tool.get();
    REQUIRE(registry.add(std::move(tool)));

    agent::ToolCallExecutorOptions options;
    options.before_tool_call = [](const agent::BeforeToolCallContext&)
        -> util::Expected<agent::BeforeToolCallResult> {
        return agent::BeforeToolCallResult{true, "blocked"};
    };
    agent::ToolCallExecutor executor{registry, std::move(options)};
    auto assistant = assistant_with_calls({make_call("call-1", "read_file")});

    auto run = run_executor(executor, assistant);

    REQUIRE(run.result);
    REQUIRE(run.result->results.size() == 1);
    CHECK(run.result->results[0].is_error);
    CHECK(ai::text_from_content(run.result->results[0].content) == "blocked");
    CHECK(tool_ptr->invocation_count() == 0);
    CHECK(count_events<agent::MessageStartEvent>(run.events) == 1);
    CHECK(count_events<agent::MessageEndEvent>(run.events) == 1);
}

TEST_CASE("ToolCallExecutor applies after-hook overrides and termination", "[agent][tool-executor]") {
    agent::AsyncToolRegistry registry;
    REQUIRE(registry.add(std::make_unique<RecordingTool>(
        ai::Tool{"read_file", "Read", ai::JsonSchema::object()})));

    agent::ToolCallExecutorOptions options;
    options.after_tool_call = [](const agent::AfterToolCallContext&)
        -> util::Expected<agent::AfterToolCallResult> {
        return agent::AfterToolCallResult{
            std::vector<ai::Content>{ai::text_content("overridden")},
            std::nullopt,
            std::nullopt,
            true};
    };
    agent::ToolCallExecutor executor{registry, std::move(options)};
    auto assistant = assistant_with_calls({make_call("call-1", "read_file")});

    auto run = run_executor(executor, assistant);

    REQUIRE(run.result);
    REQUIRE(run.result->results.size() == 1);
    CHECK(ai::text_from_content(run.result->results[0].content) == "overridden");
    CHECK(run.result->terminate_batch);
    CHECK(count_events<agent::MessageStartEvent>(run.events) == 1);
    CHECK(count_events<agent::MessageEndEvent>(run.events) == 1);
}

TEST_CASE("tool errors prevent batch termination", "[agent][tool-executor]") {
    agent::AsyncToolRegistry registry;
    REQUIRE(registry.add(std::make_unique<FailingTool>(
        ai::Tool{"read_file", "Read", ai::JsonSchema::object()})));
    agent::ToolCallExecutor executor{registry, agent::ToolCallExecutorOptions{}};
    auto assistant = assistant_with_calls({make_call("call-1", "read_file")});

    auto run = run_executor(executor, assistant);

    REQUIRE(run.result);
    CHECK_FALSE(run.result->terminate_batch);
    REQUIRE(run.result->results.size() == 1);
    CHECK(run.result->results[0].is_error);
}
