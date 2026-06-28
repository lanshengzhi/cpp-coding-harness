#include "../../third_party/catch2/catch_test_macros.hpp"

#include "agent/ToolCallExecutor.hpp"
#include "util/ExpectedMacros.hpp"

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
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace cch;

namespace {

class FakeTool final : public agent::AsyncAgentTool {
public:
    explicit FakeTool(ai::Tool definition, std::string result_text = "tool says ok")
        : definition_(std::move(definition)), result_text_(std::move(result_text)) {}

    const ai::Tool& definition() const override { return definition_; }

    boost::asio::awaitable<util::Expected<agent::AsyncToolExecutionResult>> execute(
        agent::ToolInvocation invocation) override {
        invocations.push_back(invocation);
        co_return agent::AsyncToolExecutionResult{
            std::vector<ai::Content>{ai::text_content(result_text_)}, std::nullopt, false};
    }

    ai::Tool definition_;
    std::string result_text_;
    std::vector<agent::ToolInvocation> invocations;
};

class ConfigurableFakeTool final : public agent::AsyncAgentTool {
public:
    ConfigurableFakeTool(
        ai::Tool definition,
        std::optional<ai::ToolExecutionMode> mode,
        std::string result_text = "tool says ok")
        : definition_(std::move(definition)), mode_(mode), result_text_(std::move(result_text)) {}

    const ai::Tool& definition() const override { return definition_; }

    std::optional<ai::ToolExecutionMode> execution_mode() const override { return mode_; }

    boost::asio::awaitable<util::Expected<agent::AsyncToolExecutionResult>> execute(
        agent::ToolInvocation invocation) override {
        invocations.push_back(invocation);
        co_return agent::AsyncToolExecutionResult{
            std::vector<ai::Content>{ai::text_content(result_text_)}, std::nullopt, false};
    }

    ai::Tool definition_;
    std::optional<ai::ToolExecutionMode> mode_;
    std::string result_text_;
    std::vector<agent::ToolInvocation> invocations;
};

class FailingFakeTool final : public agent::AsyncAgentTool {
public:
    explicit FailingFakeTool(ai::Tool definition) : definition_(std::move(definition)) {}

    const ai::Tool& definition() const override { return definition_; }

    boost::asio::awaitable<util::Expected<agent::AsyncToolExecutionResult>> execute(
        agent::ToolInvocation invocation) override {
        invocations.push_back(invocation);
        co_return std::unexpected(util::make_error(util::ErrorCode::Tool, "tool failed", "boom"));
    }

    ai::Tool definition_;
    std::vector<agent::ToolInvocation> invocations;
};

struct ConcurrencyProbe {
    std::atomic<int> active{0};
    std::atomic<int> max_active{0};
};

class ProbedFakeTool final : public agent::AsyncAgentTool {
public:
    explicit ProbedFakeTool(ai::Tool definition, ConcurrencyProbe& probe)
        : definition_(std::move(definition)), probe_(probe) {}

    const ai::Tool& definition() const override { return definition_; }

    std::optional<ai::ToolExecutionMode> execution_mode() const override {
        return ai::ToolExecutionMode::Parallel;
    }

    boost::asio::awaitable<util::Expected<agent::AsyncToolExecutionResult>> execute(
        agent::ToolInvocation invocation) override {
        invocations.push_back(invocation);
        const int current = ++probe_.active;
        int observed = probe_.max_active.load();
        while (current > observed && !probe_.max_active.compare_exchange_weak(observed, current)) {}
        auto timer = boost::asio::steady_timer(co_await boost::asio::this_coro::executor, std::chrono::milliseconds{30});
        co_await timer.async_wait(boost::asio::use_awaitable);
        --probe_.active;
        co_return agent::AsyncToolExecutionResult{
            std::vector<ai::Content>{ai::text_content(definition_.name + " result")}, std::nullopt, false};
    }

    ai::Tool definition_;
    ConcurrencyProbe& probe_;
    std::vector<agent::ToolInvocation> invocations;
};

struct ExecuteResult {
    util::Expected<agent::ToolCallBatchResult> result;
    std::vector<agent::AgentLifecycleEvent> events;
};

ExecuteResult run_executor(
    agent::ToolCallExecutor& executor,
    const ai::AssistantMessage& assistant_message,
    const std::vector<ai::ToolCallContent>& calls,
    ai::AiContext& context,
    agent::AgentState& state) {
    boost::asio::io_context io;
    std::optional<util::Expected<agent::ToolCallBatchResult>> result;
    std::vector<agent::AgentLifecycleEvent> events;

    agent::AgentEventSink sink{
        [&](const agent::AgentLifecycleEvent& event) {
            events.push_back(event);
            return util::ExpectedVoid{};
        }};

    boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<void> {
            result = co_await executor.execute(1, assistant_message, calls, context, state, sink);
            co_return;
        },
        boost::asio::detached);

    io.run();
    REQUIRE(result.has_value());
    return ExecuteResult{std::move(*result), std::move(events)};
}

ExecuteResult run_executor_on_pool(
    agent::ToolCallExecutor& executor,
    const ai::AssistantMessage& assistant_message,
    const std::vector<ai::ToolCallContent>& calls,
    ai::AiContext& context,
    agent::AgentState& state) {
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
            result = co_await executor.execute(1, assistant_message, calls, context, state, sink);
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

ai::ToolCallContent make_call(std::string id, std::string name, std::string raw_args = R"({"x":1})") {
    auto args = util::read_json<util::JsonValue>(raw_args);
    ai::ToolCallContent call;
    call.id = std::move(id);
    call.name = std::move(name);
    call.raw_arguments = raw_args;
    if (args) {
        call.arguments = *args;
    } else {
        call.arguments_valid = false;
        call.argument_error = args.error().detail;
    }
    return call;
}

ai::AssistantMessage assistant_with_calls(std::vector<ai::ToolCallContent> calls) {
    ai::AssistantMessage message;
    message.stop_reason = ai::AssistantStopReason::ToolUse;
    for (auto& call : calls) {
        message.content.emplace_back(std::move(call));
    }
    return message;
}

} // namespace

TEST_CASE("ToolCallExecutor is not copyable", "[agent][tool-executor]") {
    static_assert(!std::is_copy_constructible_v<agent::ToolCallExecutor>);
    static_assert(std::is_move_constructible_v<agent::ToolCallExecutor>);
}

TEST_CASE("ToolCallExecutor executes a single tool call sequentially", "[agent][tool-executor]") {
    agent::AsyncToolRegistry registry;
    auto tool = std::make_unique<FakeTool>(ai::Tool{"read_file", "Read", ai::JsonSchema::object()});
    auto* tool_ptr = tool.get();
    REQUIRE(registry.add(std::move(tool)));

    agent::ToolCallExecutor executor{registry, agent::ToolCallExecutorOptions{}};

    auto call = make_call("call-1", "read_file");
    auto assistant = assistant_with_calls({call});
    ai::AiContext context;
    agent::AgentState state;

    auto run = run_executor(executor, assistant, {std::move(call)}, context, state);

    REQUIRE(run.result);
    CHECK(run.result->results.size() == 1);
    CHECK_FALSE(run.result->terminate_batch);
    CHECK(tool_ptr->invocations.size() == 1);
    CHECK(tool_ptr->invocations[0].call_id == "call-1");
    CHECK(tool_ptr->invocations[0].name == "read_file");
    CHECK(count_events<agent::ToolExecutionStartEvent>(run.events) == 1);
    CHECK(count_events<agent::ToolExecutionEndEvent>(run.events) == 1);
    CHECK(state.active_tool_names.empty());
    CHECK(state.pending_tool_call_ids.empty());
}

TEST_CASE("ToolCallExecutor executes multiple tool calls sequentially", "[agent][tool-executor]") {
    agent::AsyncToolRegistry registry;
    auto alpha = std::make_unique<FakeTool>(ai::Tool{"alpha", "Alpha", ai::JsonSchema::object()}, "alpha result");
    auto beta = std::make_unique<FakeTool>(ai::Tool{"beta", "Beta", ai::JsonSchema::object()}, "beta result");
    REQUIRE(registry.add(std::move(alpha)));
    REQUIRE(registry.add(std::move(beta)));

    agent::ToolCallExecutor executor{registry, agent::ToolCallExecutorOptions{}};

    auto call1 = make_call("call-1", "alpha");
    auto call2 = make_call("call-2", "beta");
    std::vector<ai::ToolCallContent> calls;
    calls.push_back(call1);
    calls.push_back(call2);
    auto assistant = assistant_with_calls({call1, call2});
    ai::AiContext context;
    agent::AgentState state;

    auto run = run_executor(executor, assistant, calls, context, state);

    REQUIRE(run.result);
    REQUIRE(run.result->results.size() == 2);
    CHECK(run.result->results[0].tool_name == "alpha");
    CHECK(run.result->results[1].tool_name == "beta");
    CHECK(count_events<agent::ToolExecutionStartEvent>(run.events) == 2);
    CHECK(count_events<agent::ToolExecutionEndEvent>(run.events) == 2);
}

TEST_CASE("ToolCallExecutor falls back to sequential when a tool requests it", "[agent][tool-executor]") {
    agent::AsyncToolRegistry registry;
    auto alpha = std::make_unique<ConfigurableFakeTool>(
        ai::Tool{"alpha", "Alpha", ai::JsonSchema::object()},
        ai::ToolExecutionMode::Sequential,
        "alpha result");
    auto beta = std::make_unique<ConfigurableFakeTool>(
        ai::Tool{"beta", "Beta", ai::JsonSchema::object()},
        ai::ToolExecutionMode::Parallel,
        "beta result");
    REQUIRE(registry.add(std::move(alpha)));
    REQUIRE(registry.add(std::move(beta)));

    agent::ToolCallExecutorOptions options;
    options.mode = ai::ToolExecutionMode::Parallel;
    agent::ToolCallExecutor executor{registry, std::move(options)};

    auto call1 = make_call("call-1", "alpha");
    auto call2 = make_call("call-2", "beta");
    std::vector<ai::ToolCallContent> calls;
    calls.push_back(call1);
    calls.push_back(call2);
    auto assistant = assistant_with_calls({call1, call2});
    ai::AiContext context;
    agent::AgentState state;

    auto run = run_executor(executor, assistant, calls, context, state);

    REQUIRE(run.result);
    REQUIRE(run.result->results.size() == 2);
    CHECK(run.result->results[0].tool_name == "alpha");
    CHECK(run.result->results[1].tool_name == "beta");
}

TEST_CASE("ToolCallExecutor runs independent tool calls in parallel", "[agent][tool-executor]") {
    agent::AsyncToolRegistry registry;
    ConcurrencyProbe probe;
    auto alpha = std::make_unique<ProbedFakeTool>(ai::Tool{"alpha", "Alpha", ai::JsonSchema::object()}, probe);
    auto beta = std::make_unique<ProbedFakeTool>(ai::Tool{"beta", "Beta", ai::JsonSchema::object()}, probe);
    REQUIRE(registry.add(std::move(alpha)));
    REQUIRE(registry.add(std::move(beta)));

    agent::ToolCallExecutorOptions options;
    options.mode = ai::ToolExecutionMode::Parallel;
    agent::ToolCallExecutor executor{registry, std::move(options)};

    auto call1 = make_call("call-1", "alpha");
    auto call2 = make_call("call-2", "beta");
    std::vector<ai::ToolCallContent> calls;
    calls.push_back(call1);
    calls.push_back(call2);
    auto assistant = assistant_with_calls({call1, call2});
    ai::AiContext context;
    agent::AgentState state;

    auto run = run_executor_on_pool(executor, assistant, calls, context, state);

    REQUIRE(run.result);
    REQUIRE(run.result->results.size() == 2);
    CHECK(probe.max_active >= 2);
}

TEST_CASE("ToolCallExecutor produces error result for unknown tool", "[agent][tool-executor]") {
    agent::AsyncToolRegistry registry;
    agent::ToolCallExecutor executor{registry, agent::ToolCallExecutorOptions{}};

    auto call = make_call("call-1", "missing");
    auto assistant = assistant_with_calls({call});
    ai::AiContext context;
    agent::AgentState state;

    auto run = run_executor(executor, assistant, {std::move(call)}, context, state);

    REQUIRE(run.result);
    REQUIRE(run.result->results.size() == 1);
    CHECK(run.result->results[0].is_error);
    CHECK(run.result->results[0].tool_name == "missing");
}

TEST_CASE("ToolCallExecutor produces error result for malformed arguments", "[agent][tool-executor]") {
    agent::AsyncToolRegistry registry;
    REQUIRE(registry.add(std::make_unique<FakeTool>(ai::Tool{"read_file", "Read", ai::JsonSchema::object()})));

    agent::ToolCallExecutor executor{registry, agent::ToolCallExecutorOptions{}};

    auto call = make_call("call-1", "read_file", "not-json");
    auto assistant = assistant_with_calls({call});
    ai::AiContext context;
    agent::AgentState state;

    auto run = run_executor(executor, assistant, {std::move(call)}, context, state);

    REQUIRE(run.result);
    REQUIRE(run.result->results.size() == 1);
    CHECK(run.result->results[0].is_error);
    CHECK(count_events<agent::ToolExecutionEndEvent>(run.events) == 1);
}

TEST_CASE("ToolCallExecutor beforeToolCall hook can block", "[agent][tool-executor]") {
    agent::AsyncToolRegistry registry;
    auto tool = std::make_unique<FakeTool>(ai::Tool{"read_file", "Read", ai::JsonSchema::object()});
    auto* tool_ptr = tool.get();
    REQUIRE(registry.add(std::move(tool)));

    agent::ToolCallExecutorOptions options;
    options.before_tool_call = [](const agent::BeforeToolCallContext&) -> util::Expected<agent::BeforeToolCallResult> {
        return agent::BeforeToolCallResult{true, "blocked"};
    };
    agent::ToolCallExecutor executor{registry, std::move(options)};

    auto call = make_call("call-1", "read_file");
    auto assistant = assistant_with_calls({call});
    ai::AiContext context;
    agent::AgentState state;

    auto run = run_executor(executor, assistant, {std::move(call)}, context, state);

    REQUIRE(run.result);
    REQUIRE(run.result->results.size() == 1);
    CHECK(run.result->results[0].is_error);
    CHECK(tool_ptr->invocations.empty());
}

TEST_CASE("ToolCallExecutor afterToolCall hook overrides result", "[agent][tool-executor]") {
    agent::AsyncToolRegistry registry;
    REQUIRE(registry.add(std::make_unique<FakeTool>(ai::Tool{"read_file", "Read", ai::JsonSchema::object()})));

    agent::ToolCallExecutorOptions options;
    options.after_tool_call = [](const agent::AfterToolCallContext&) -> util::Expected<agent::AfterToolCallResult> {
        return agent::AfterToolCallResult{
            std::vector<ai::Content>{ai::text_content("overridden")}, std::nullopt, std::nullopt, std::nullopt};
    };
    agent::ToolCallExecutor executor{registry, std::move(options)};

    auto call = make_call("call-1", "read_file");
    auto assistant = assistant_with_calls({call});
    ai::AiContext context;
    agent::AgentState state;

    auto run = run_executor(executor, assistant, {std::move(call)}, context, state);

    REQUIRE(run.result);
    REQUIRE(run.result->results.size() == 1);
    CHECK(ai::text_from_content(run.result->results[0].content) == "overridden");
}

TEST_CASE("ToolCallExecutor afterToolCall terminate hint stops batch", "[agent][tool-executor]") {
    agent::AsyncToolRegistry registry;
    REQUIRE(registry.add(std::make_unique<FakeTool>(ai::Tool{"read_file", "Read", ai::JsonSchema::object()})));

    agent::ToolCallExecutorOptions options;
    options.after_tool_call = [](const agent::AfterToolCallContext&) -> util::Expected<agent::AfterToolCallResult> {
        return agent::AfterToolCallResult{std::nullopt, std::nullopt, std::nullopt, true};
    };
    agent::ToolCallExecutor executor{registry, std::move(options)};

    auto call = make_call("call-1", "read_file");
    auto assistant = assistant_with_calls({call});
    ai::AiContext context;
    agent::AgentState state;

    auto run = run_executor(executor, assistant, {std::move(call)}, context, state);

    REQUIRE(run.result);
    CHECK(run.result->terminate_batch);
}

TEST_CASE("ToolCallExecutor error result prevents terminate batch", "[agent][tool-executor]") {
    agent::AsyncToolRegistry registry;
    REQUIRE(registry.add(std::make_unique<FailingFakeTool>(ai::Tool{"read_file", "Read", ai::JsonSchema::object()})));

    agent::ToolCallExecutorOptions options;
    options.after_tool_call = [](const agent::AfterToolCallContext&) -> util::Expected<agent::AfterToolCallResult> {
        return agent::AfterToolCallResult{std::nullopt, std::nullopt, std::nullopt, true};
    };
    agent::ToolCallExecutor executor{registry, std::move(options)};

    auto call = make_call("call-1", "read_file");
    auto assistant = assistant_with_calls({call});
    ai::AiContext context;
    agent::AgentState state;

    auto run = run_executor(executor, assistant, {std::move(call)}, context, state);

    REQUIRE(run.result);
    CHECK_FALSE(run.result->terminate_batch);
    CHECK(run.result->results[0].is_error);
}
