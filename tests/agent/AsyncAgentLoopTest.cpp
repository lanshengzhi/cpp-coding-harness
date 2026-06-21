#include "../../third_party/catch2/catch_test_macros.hpp"

#include "../../src/util/ExpectedMacros.hpp"

#include "../../include/cch/agent/AgentLoop.hpp"
#include "../../include/cch/ai/Content.hpp"
#include "../../include/cch/util/Error.hpp"
#include "util/Json.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <chrono>
#include <deque>
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

class FakeStreamingClient final : public ai::StreamingChatClient {
public:
    boost::asio::awaitable<util::Expected<ai::AssistantMessage>> stream(
        const ai::StreamChatRequest& request,
        ai::AssistantEventSink sink) override {
        requests.push_back(request);
        if (failure) {
            co_return std::unexpected(*failure);
        }
        if (responses.empty()) {
            co_return ai::assistant_text_message("default fake response");
        }
        auto response = responses.front();
        responses.pop_front();
        for (std::size_t index = 0; index < response.content.size(); ++index) {
            const auto& block = response.content[index];
            if (const auto* text = std::get_if<ai::TextContent>(&block)) {
                CCH_TRY_VOID(sink(ai::TextDeltaEvent{index, text->text, response}));
            } else if (const auto* thinking = std::get_if<ai::ThinkingContent>(&block)) {
                CCH_TRY_VOID(sink(ai::ThinkingDeltaEvent{index, thinking->thinking, response}));
            } else if (const auto* call = std::get_if<ai::ToolCallContent>(&block)) {
                CCH_TRY_VOID(sink(ai::ToolCallStartEvent{index, response}));
                CCH_TRY_VOID(sink(ai::ToolCallDeltaEvent{index, call->raw_arguments, response}));
                CCH_TRY_VOID(sink(ai::ToolCallEndEvent{index, *call, response}));
            }
        }
        co_return response;
    }

    std::deque<ai::AssistantMessage> responses;
    std::optional<util::Error> failure;
    std::vector<ai::StreamChatRequest> requests;
};

class FakeTool final : public agent::AsyncAgentTool {
public:
    explicit FakeTool(ai::Tool definition) : definition_(std::move(definition)) {}

    const ai::Tool& definition() const override { return definition_; }

    boost::asio::awaitable<util::Expected<agent::AsyncToolExecutionResult>> execute(
        agent::ToolInvocation invocation) override {
        invocations.push_back(invocation);
        co_return agent::AsyncToolExecutionResult{std::vector<ai::Content>{ai::text_content("tool says ok")}, std::nullopt, false};
    }

    ai::Tool definition_;
    std::vector<agent::ToolInvocation> invocations;
};

struct RunResult {
    util::Expected<agent::AsyncAgentRunResult> result;
    std::vector<agent::AgentLifecycleEvent> events;
};

RunResult run_loop(agent::AsyncAgentLoop& loop, std::string prompt) {
    boost::asio::io_context io;
    std::optional<util::Expected<agent::AsyncAgentRunResult>> result;
    std::vector<agent::AgentLifecycleEvent> events;

    boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<void> {
            result = co_await loop.run(
                std::move(prompt),
                [&](const agent::AgentLifecycleEvent& event) {
                    events.push_back(event);
                    return util::ExpectedVoid{};
                });
            co_return;
        },
        boost::asio::detached);

    io.run();
    REQUIRE(result.has_value());
    return RunResult{std::move(*result), std::move(events)};
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

ai::AssistantMessage tool_call_response(std::string raw_arguments = R"({"path":"README.md"})") {
    auto args = util::read_json<util::JsonValue>(raw_arguments);
    ai::AssistantMessage message;
    message.stop_reason = ai::AssistantStopReason::ToolUse;
    ai::ToolCallContent call;
    call.id = "call-1";
    call.name = "read_file";
    call.raw_arguments = raw_arguments;
    if (args) {
        call.arguments = *args;
    } else {
        call.arguments_valid = false;
        call.argument_error = args.error().detail;
    }
    message.content.emplace_back(std::move(call));
    return message;
}

} // namespace

TEST_CASE("async tool registry owns tools and returns deterministic definitions", "[agent][u6]") {
    static_assert(!std::is_copy_constructible_v<agent::AsyncToolRegistry>);
    static_assert(std::is_move_constructible_v<agent::AsyncToolRegistry>);

    auto zed = std::make_unique<FakeTool>(ai::Tool{"zed", "Zed tool", ai::JsonSchema::object()});
    auto alpha = std::make_unique<FakeTool>(ai::Tool{"alpha", "Alpha tool", ai::JsonSchema::object()});
    auto* zed_ptr = zed.get();
    auto* alpha_ptr = alpha.get();

    agent::AsyncToolRegistry registry;
    REQUIRE(registry.add(std::move(zed)));
    REQUIRE(registry.add(std::move(alpha)));

    CHECK(registry.find("zed") == zed_ptr);
    CHECK(registry.find("alpha") == alpha_ptr);
    CHECK(registry.find("missing") == nullptr);

    const auto definitions = registry.definitions();
    REQUIRE(definitions.size() == 2);
    CHECK(definitions[0].name == "alpha");
    CHECK(definitions[1].name == "zed");
}

TEST_CASE("async agent loop emits deterministic lifecycle events for text", "[agent][async][u5]") {
    FakeStreamingClient client;
    client.responses.push_back(ai::assistant_text_message("hello user"));
    agent::AsyncToolRegistry registry;
    agent::AsyncAgentLoop loop(client, std::move(registry), agent::AsyncAgentOptions{3, "gpt-test"});

    auto run = run_loop(loop, "hi");

    REQUIRE(run.result);
    CHECK(run.result->turns == 1);
    CHECK(run.result->stop_reason == ai::AssistantStopReason::Stop);
    REQUIRE(run.result->context.messages.size() == 2);
    CHECK(count_events<agent::AgentStartEvent>(run.events) == 1);
    CHECK(count_events<agent::TurnStartEvent>(run.events) == 1);
    CHECK(count_events<agent::MessageStartEvent>(run.events) == 1);
    CHECK(count_events<agent::MessageUpdateEvent>(run.events) == 1);
    CHECK(count_events<agent::MessageEndEvent>(run.events) == 1);
    CHECK(count_events<agent::TurnEndEvent>(run.events) == 1);
    CHECK(count_events<agent::AgentEndEvent>(run.events) == 1);
}

TEST_CASE("async agent loop forwards thinking and tool-call stream lifecycle events", "[agent][async][u5]") {
    FakeStreamingClient client;
    ai::AssistantMessage message;
    message.stop_reason = ai::AssistantStopReason::ToolUse;
    message.content.emplace_back(ai::thinking_content("SECRET_THOUGHT"));
    message.content.emplace_back(ai::tool_call_content("call-1", "read_file", R"({"path":"README.md"})"));
    client.responses.push_back(std::move(message));
    client.responses.push_back(ai::assistant_text_message("done"));

    agent::AsyncToolRegistry registry;
    REQUIRE(registry.add(std::make_unique<FakeTool>(ai::Tool{
        "read_file",
        "Read a workspace file",
        ai::JsonSchema::object({{"path", ai::JsonSchema::string("file path")}}, {"path"}),
    })));
    agent::AsyncAgentLoop loop(client, std::move(registry), agent::AsyncAgentOptions{4, "gpt-test"});

    auto run = run_loop(loop, "read");

    REQUIRE(run.result);
    CHECK(count_events<agent::ThinkingUpdateEvent>(run.events) == 1);
    CHECK(count_events<agent::ToolCallStreamStartEvent>(run.events) == 1);
    CHECK(count_events<agent::ToolCallStreamUpdateEvent>(run.events) == 1);
    CHECK(count_events<agent::ToolCallStreamEndEvent>(run.events) == 1);
    CHECK(run.result->state.model == "gpt-test");
    CHECK(run.result->state.pending_tool_call_ids.empty());
    CHECK(run.result->state.active_tool_names.empty());
    CHECK(run.result->state.messages.size() == run.result->context.messages.size());
}

TEST_CASE("async agent loop executes tool calls and continues with tool result context", "[agent][async][u5][ae2]") {
    FakeStreamingClient client;
    client.responses.push_back(tool_call_response());
    client.responses.push_back(ai::assistant_text_message("done"));

    auto tool = std::make_unique<FakeTool>(ai::Tool{
        "read_file",
        "Read a workspace file",
        ai::JsonSchema::object({{"path", ai::JsonSchema::string("file path")}}, {"path"}),
    });
    auto* tool_ptr = tool.get();
    agent::AsyncToolRegistry registry;
    REQUIRE(registry.add(std::move(tool)));
    agent::AsyncAgentLoop loop(client, std::move(registry), agent::AsyncAgentOptions{4, "gpt-test"});

    auto run = run_loop(loop, "read");

    REQUIRE(run.result);
    CHECK(run.result->turns == 2);
    REQUIRE(tool_ptr->invocations.size() == 1);
    CHECK(tool_ptr->invocations[0].call_id == "call-1");
    CHECK(tool_ptr->invocations[0].name == "read_file");
    CHECK(tool_ptr->invocations[0].arguments.get<util::JsonValue::object_t>().at("path").get_string() == "README.md");
    REQUIRE(client.requests.size() == 2);
    REQUIRE(client.requests[1].context.messages.size() == 3);
    REQUIRE(std::holds_alternative<ai::ToolResultMessage>(client.requests[1].context.messages.back()));
    CHECK(count_events<agent::ToolCallStreamStartEvent>(run.events) == 1);
    CHECK(count_events<agent::ToolCallStreamEndEvent>(run.events) == 1);
    CHECK(count_events<agent::ToolExecutionStartEvent>(run.events) == 1);
    CHECK(count_events<agent::ToolExecutionEndEvent>(run.events) == 1);
    CHECK(run.result->state.pending_tool_call_ids.empty());
    CHECK(run.result->state.active_tool_names.empty());
}

TEST_CASE("async agent loop turns malformed tool arguments into error tool results", "[agent][async][u5]") {
    FakeStreamingClient client;
    client.responses.push_back(tool_call_response("not-json"));
    client.responses.push_back(ai::assistant_text_message("saw error"));
    agent::AsyncToolRegistry registry;
    REQUIRE(registry.add(std::make_unique<FakeTool>(ai::Tool{"read_file", "Read", ai::JsonSchema::object()})));
    agent::AsyncAgentLoop loop(client, std::move(registry), agent::AsyncAgentOptions{4, "gpt-test"});

    auto run = run_loop(loop, "read");

    REQUIRE(run.result);
    REQUIRE(client.requests.size() == 2);
    REQUIRE(std::holds_alternative<ai::ToolResultMessage>(client.requests[1].context.messages.back()));
    const auto& result = std::get<ai::ToolResultMessage>(client.requests[1].context.messages.back());
    CHECK(result.is_error);
    CHECK(count_events<agent::ToolExecutionEndEvent>(run.events) == 1);
}

TEST_CASE("async agent loop reports max turn exhaustion", "[agent][async][u5]") {
    FakeStreamingClient client;
    client.responses.push_back(tool_call_response());
    agent::AsyncToolRegistry registry;
    REQUIRE(registry.add(std::make_unique<FakeTool>(ai::Tool{"read_file", "Read", ai::JsonSchema::object()})));
    agent::AsyncAgentLoop loop(client, std::move(registry), agent::AsyncAgentOptions{1, "gpt-test"});

    auto run = run_loop(loop, "read");

    REQUIRE_FALSE(run.result);
    CHECK(run.result.error().message == "max turns exceeded");
    CHECK(count_events<agent::AgentEndEvent>(run.events) == 1);
}

TEST_CASE("beforeToolCall hook can block a tool call", "[agent][async][u7]") {
    FakeStreamingClient client;
    client.responses.push_back(tool_call_response());
    client.responses.push_back(ai::assistant_text_message("done"));

    auto tool = std::make_unique<FakeTool>(ai::Tool{"read_file", "Read", ai::JsonSchema::object()});
    auto* tool_ptr = tool.get();
    agent::AsyncToolRegistry registry;
    REQUIRE(registry.add(std::move(tool)));

    agent::AsyncAgentOptions options{4, "gpt-test"};
    options.before_tool_call = [](const agent::BeforeToolCallContext&) -> util::Expected<agent::BeforeToolCallResult> {
        return agent::BeforeToolCallResult{true, "blocked by policy"};
    };

    agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));
    auto run = run_loop(loop, "read");

    REQUIRE(run.result);
    CHECK(tool_ptr->invocations.empty());
    CHECK(count_events<agent::ToolExecutionStartEvent>(run.events) == 1);
    CHECK(count_events<agent::ToolExecutionEndEvent>(run.events) == 1);

    const agent::ToolExecutionEndEvent* end_event = nullptr;
    for (const auto& event : run.events) {
        if (const auto* candidate = std::get_if<agent::ToolExecutionEndEvent>(&event)) {
            end_event = candidate;
        }
    }
    REQUIRE(end_event);
    CHECK(end_event->is_error);

    REQUIRE(client.requests.size() == 2);
    REQUIRE(std::holds_alternative<ai::ToolResultMessage>(client.requests[1].context.messages.back()));
    const auto& result = std::get<ai::ToolResultMessage>(client.requests[1].context.messages.back());
    CHECK(result.is_error);
    CHECK(ai::text_from_content(result.content) == "blocked by policy");
}

TEST_CASE("beforeToolCall hook passes context and skips execution on block", "[agent][async][u7]") {
    FakeStreamingClient client;
    client.responses.push_back(tool_call_response());
    client.responses.push_back(ai::assistant_text_message("done"));

    auto tool = std::make_unique<FakeTool>(ai::Tool{"read_file", "Read", ai::JsonSchema::object()});
    auto* tool_ptr = tool.get();
    agent::AsyncToolRegistry registry;
    REQUIRE(registry.add(std::move(tool)));

    agent::AsyncAgentOptions options{4, "gpt-test"};
    options.before_tool_call = [](const agent::BeforeToolCallContext& ctx) -> util::Expected<agent::BeforeToolCallResult> {
        REQUIRE(ctx.tool_call.name == "read_file");
        REQUIRE(ctx.args.get<util::JsonValue::object_t>().at("path").get_string() == "README.md");
        REQUIRE(ctx.context.model == "gpt-test");
        REQUIRE(!ctx.context.messages.empty());
        return agent::BeforeToolCallResult{false, std::nullopt};
    };

    agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));
    auto run = run_loop(loop, "read");

    REQUIRE(run.result);
    CHECK(tool_ptr->invocations.size() == 1);
}

TEST_CASE("beforeToolCall hook failure aborts the run", "[agent][async][u7]") {
    FakeStreamingClient client;
    client.responses.push_back(tool_call_response());
    agent::AsyncToolRegistry registry;
    REQUIRE(registry.add(std::make_unique<FakeTool>(ai::Tool{"read_file", "Read", ai::JsonSchema::object()})));

    agent::AsyncAgentOptions options{4, "gpt-test"};
    options.before_tool_call = [](const agent::BeforeToolCallContext&) -> util::Expected<agent::BeforeToolCallResult> {
        return std::unexpected(util::make_error(util::ErrorCode::Tool, "policy rejected"));
    };

    agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));
    auto run = run_loop(loop, "read");

    REQUIRE_FALSE(run.result);
    CHECK(run.result.error().code == util::ErrorCode::Tool);
    CHECK(run.result.error().message == "policy rejected");

    const auto* end_event = std::get_if<agent::AgentEndEvent>(&run.events.back());
    REQUIRE(end_event);
    CHECK_FALSE(end_event->success);
}

TEST_CASE("beforeToolCall hook exception becomes a tool error", "[agent][async][u7]") {
    FakeStreamingClient client;
    client.responses.push_back(tool_call_response());
    agent::AsyncToolRegistry registry;
    REQUIRE(registry.add(std::make_unique<FakeTool>(ai::Tool{"read_file", "Read", ai::JsonSchema::object()})));

    agent::AsyncAgentOptions options{4, "gpt-test"};
    options.before_tool_call = [](const agent::BeforeToolCallContext&) -> util::Expected<agent::BeforeToolCallResult> {
        throw std::runtime_error("boom");
    };

    agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));
    auto run = run_loop(loop, "read");

    REQUIRE_FALSE(run.result);
    CHECK(run.result.error().code == util::ErrorCode::Tool);
    CHECK(run.result.error().detail.find("boom") != std::string::npos);
}

TEST_CASE("afterToolCall hook overrides tool result content", "[agent][async][u7]") {
    FakeStreamingClient client;
    client.responses.push_back(tool_call_response());
    client.responses.push_back(ai::assistant_text_message("done"));
    agent::AsyncToolRegistry registry;
    REQUIRE(registry.add(std::make_unique<FakeTool>(ai::Tool{"read_file", "Read", ai::JsonSchema::object()})));

    agent::AsyncAgentOptions options{4, "gpt-test"};
    options.after_tool_call = [](const agent::AfterToolCallContext&) -> util::Expected<agent::AfterToolCallResult> {
        return agent::AfterToolCallResult{
            std::vector<ai::Content>{ai::text_content("overridden")}, std::nullopt, std::nullopt, std::nullopt};
    };

    agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));
    auto run = run_loop(loop, "read");

    REQUIRE(run.result);
    REQUIRE(client.requests.size() == 2);
    REQUIRE(std::holds_alternative<ai::ToolResultMessage>(client.requests[1].context.messages.back()));
    const auto& result = std::get<ai::ToolResultMessage>(client.requests[1].context.messages.back());
    CHECK(ai::text_from_content(result.content) == "overridden");
}

TEST_CASE("afterToolCall hook overrides error flag", "[agent][async][u7]") {
    FakeStreamingClient client;
    client.responses.push_back(tool_call_response());
    client.responses.push_back(ai::assistant_text_message("done"));
    agent::AsyncToolRegistry registry;
    REQUIRE(registry.add(std::make_unique<FakeTool>(ai::Tool{"read_file", "Read", ai::JsonSchema::object()})));

    agent::AsyncAgentOptions options{4, "gpt-test"};
    options.after_tool_call = [](const agent::AfterToolCallContext&) -> util::Expected<agent::AfterToolCallResult> {
        return agent::AfterToolCallResult{std::nullopt, std::nullopt, true, std::nullopt};
    };

    agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));
    auto run = run_loop(loop, "read");

    REQUIRE(run.result);
    REQUIRE(client.requests.size() == 2);
    REQUIRE(std::holds_alternative<ai::ToolResultMessage>(client.requests[1].context.messages.back()));
    const auto& result = std::get<ai::ToolResultMessage>(client.requests[1].context.messages.back());
    CHECK(result.is_error);
}

namespace {

ai::AssistantMessage two_tool_call_response() {
    ai::AssistantMessage message;
    message.stop_reason = ai::AssistantStopReason::ToolUse;
    message.content.emplace_back(ai::tool_call_content("call-1", "alpha", R"({"x":1})"));
    message.content.emplace_back(ai::tool_call_content("call-2", "beta", R"({"y":2})"));
    return message;
}

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

class DelayedFakeTool final : public agent::AsyncAgentTool {
public:
    DelayedFakeTool(
        ai::Tool definition,
        std::chrono::milliseconds delay,
        std::string result_text = "tool says ok")
        : definition_(std::move(definition)), delay_(delay), result_text_(std::move(result_text)) {}

    const ai::Tool& definition() const override { return definition_; }

    std::optional<ai::ToolExecutionMode> execution_mode() const override {
        return ai::ToolExecutionMode::Parallel;
    }

    boost::asio::awaitable<util::Expected<agent::AsyncToolExecutionResult>> execute(
        agent::ToolInvocation invocation) override {
        auto timer = boost::asio::steady_timer(co_await boost::asio::this_coro::executor, delay_);
        co_await timer.async_wait(boost::asio::use_awaitable);
        invocations.push_back(invocation);
        co_return agent::AsyncToolExecutionResult{
            std::vector<ai::Content>{ai::text_content(result_text_)}, std::nullopt, false};
    }

    ai::Tool definition_;
    std::chrono::milliseconds delay_;
    std::string result_text_;
    std::vector<agent::ToolInvocation> invocations;
};

class FailingFakeTool final : public agent::AsyncAgentTool {
public:
    explicit FailingFakeTool(ai::Tool definition) : definition_(std::move(definition)) {}

    const ai::Tool& definition() const override { return definition_; }

    std::optional<ai::ToolExecutionMode> execution_mode() const override {
        return ai::ToolExecutionMode::Parallel;
    }

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
    ProbedFakeTool(
        ai::Tool definition,
        std::optional<ai::ToolExecutionMode> mode,
        ConcurrencyProbe& probe)
        : definition_(std::move(definition)), mode_(mode), probe_(probe) {}

    const ai::Tool& definition() const override { return definition_; }

    std::optional<ai::ToolExecutionMode> execution_mode() const override { return mode_; }

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
    std::optional<ai::ToolExecutionMode> mode_;
    ConcurrencyProbe& probe_;
    std::vector<agent::ToolInvocation> invocations;
};

RunResult run_loop_on_pool(agent::AsyncAgentLoop& loop, std::string prompt) {
    boost::asio::thread_pool pool{4};
    std::optional<util::Expected<agent::AsyncAgentRunResult>> result;
    std::vector<agent::AgentLifecycleEvent> events;
    std::mutex events_mutex;

    boost::asio::co_spawn(
        pool,
        [&]() -> boost::asio::awaitable<void> {
            result = co_await loop.run(
                std::move(prompt),
                [&](const agent::AgentLifecycleEvent& event) {
                    std::lock_guard lock(events_mutex);
                    events.push_back(event);
                    return util::ExpectedVoid{};
                });
            co_return;
        },
        boost::asio::detached);

    pool.join();
    REQUIRE(result.has_value());
    return RunResult{std::move(*result), std::move(events)};
}

} // namespace

TEST_CASE("afterToolCall terminate hint stops the run when all calls agree", "[agent][async][u7]") {
    FakeStreamingClient client;
    client.responses.push_back(tool_call_response());
    agent::AsyncToolRegistry registry;
    REQUIRE(registry.add(std::make_unique<FakeTool>(ai::Tool{"read_file", "Read", ai::JsonSchema::object()})));

    agent::AsyncAgentOptions options{4, "gpt-test"};
    options.after_tool_call = [](const agent::AfterToolCallContext&) -> util::Expected<agent::AfterToolCallResult> {
        return agent::AfterToolCallResult{std::nullopt, std::nullopt, std::nullopt, true};
    };

    agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));
    auto run = run_loop(loop, "read");

    REQUIRE(run.result);
    CHECK(run.result->turns == 1);
    CHECK(run.result->stop_reason == ai::AssistantStopReason::ToolUse);
    CHECK(count_events<agent::AgentEndEvent>(run.events) == 1);
    const auto* end_event = std::get_if<agent::AgentEndEvent>(&run.events.back());
    REQUIRE(end_event);
    CHECK(end_event->success);
    CHECK(end_event->reason == ai::stop_reason_to_string(ai::AssistantStopReason::ToolUse));
}

TEST_CASE("terminate batch continues when one call declines", "[agent][async][u7]") {
    FakeStreamingClient client;
    client.responses.push_back(two_tool_call_response());
    client.responses.push_back(ai::assistant_text_message("done"));

    agent::AsyncToolRegistry registry;
    REQUIRE(registry.add(std::make_unique<FakeTool>(ai::Tool{"alpha", "Alpha", ai::JsonSchema::object()})));
    REQUIRE(registry.add(std::make_unique<FakeTool>(ai::Tool{"beta", "Beta", ai::JsonSchema::object()})));

    agent::AsyncAgentOptions options{4, "gpt-test"};
    options.after_tool_call = [](const agent::AfterToolCallContext& ctx) -> util::Expected<agent::AfterToolCallResult> {
        if (ctx.tool_call.name == "alpha") {
            return agent::AfterToolCallResult{std::nullopt, std::nullopt, std::nullopt, true};
        }
        return agent::AfterToolCallResult{std::nullopt, std::nullopt, std::nullopt, false};
    };

    agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));
    auto run = run_loop(loop, "read");

    REQUIRE(run.result);
    CHECK(run.result->turns == 2);
    CHECK(run.result->stop_reason == ai::AssistantStopReason::Stop);
}

TEST_CASE("blocked call prevents terminate batch", "[agent][async][u7]") {
    FakeStreamingClient client;
    client.responses.push_back(two_tool_call_response());
    client.responses.push_back(ai::assistant_text_message("done"));

    agent::AsyncToolRegistry registry;
    REQUIRE(registry.add(std::make_unique<FakeTool>(ai::Tool{"alpha", "Alpha", ai::JsonSchema::object()})));
    REQUIRE(registry.add(std::make_unique<FakeTool>(ai::Tool{"beta", "Beta", ai::JsonSchema::object()})));

    agent::AsyncAgentOptions options{4, "gpt-test"};
    options.before_tool_call = [](const agent::BeforeToolCallContext& ctx) -> util::Expected<agent::BeforeToolCallResult> {
        if (ctx.tool_call.name == "alpha") {
            return agent::BeforeToolCallResult{true, "no alpha"};
        }
        return agent::BeforeToolCallResult{false, std::nullopt};
    };
    options.after_tool_call = [](const agent::AfterToolCallContext&) -> util::Expected<agent::AfterToolCallResult> {
        return agent::AfterToolCallResult{std::nullopt, std::nullopt, std::nullopt, true};
    };

    agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));
    auto run = run_loop(loop, "read");

    REQUIRE(run.result);
    CHECK(run.result->turns == 2);
}

TEST_CASE("tool execution error prevents terminate batch", "[agent][async][u7]") {
    FakeStreamingClient client;
    client.responses.push_back(two_tool_call_response());
    client.responses.push_back(ai::assistant_text_message("done"));

    agent::AsyncToolRegistry registry;
    REQUIRE(registry.add(std::make_unique<FakeTool>(ai::Tool{"alpha", "Alpha", ai::JsonSchema::object()})));
    REQUIRE(registry.add(std::make_unique<FakeTool>(ai::Tool{"beta", "Beta", ai::JsonSchema::object()})));

    agent::AsyncAgentOptions options{4, "gpt-test"};
    options.after_tool_call = [](const agent::AfterToolCallContext& ctx) -> util::Expected<agent::AfterToolCallResult> {
        if (ctx.tool_call.name == "alpha") {
            return agent::AfterToolCallResult{std::nullopt, std::nullopt, true, true};
        }
        return agent::AfterToolCallResult{std::nullopt, std::nullopt, std::nullopt, true};
    };

    agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));
    auto run = run_loop(loop, "read");

    REQUIRE(run.result);
    CHECK(run.result->turns == 2);
}

TEST_CASE("afterToolCall hook failure aborts the run", "[agent][async][u7]") {
    FakeStreamingClient client;
    client.responses.push_back(tool_call_response());
    agent::AsyncToolRegistry registry;
    REQUIRE(registry.add(std::make_unique<FakeTool>(ai::Tool{"read_file", "Read", ai::JsonSchema::object()})));

    agent::AsyncAgentOptions options{4, "gpt-test"};
    options.after_tool_call = [](const agent::AfterToolCallContext&) -> util::Expected<agent::AfterToolCallResult> {
        return std::unexpected(util::make_error(util::ErrorCode::Tool, "post-processor failed"));
    };

    agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));
    auto run = run_loop(loop, "read");

    REQUIRE_FALSE(run.result);
    CHECK(run.result.error().code == util::ErrorCode::Tool);
    CHECK(run.result.error().message == "post-processor failed");
}

TEST_CASE("afterToolCall hook exception becomes a tool error", "[agent][async][u7]") {
    FakeStreamingClient client;
    client.responses.push_back(tool_call_response());
    agent::AsyncToolRegistry registry;
    REQUIRE(registry.add(std::make_unique<FakeTool>(ai::Tool{"read_file", "Read", ai::JsonSchema::object()})));

    agent::AsyncAgentOptions options{4, "gpt-test"};
    options.after_tool_call = [](const agent::AfterToolCallContext&) -> util::Expected<agent::AfterToolCallResult> {
        throw std::runtime_error("after boom");
    };

    agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));
    auto run = run_loop(loop, "read");

    REQUIRE_FALSE(run.result);
    CHECK(run.result.error().code == util::ErrorCode::Tool);
    CHECK(run.result.error().detail.find("after boom") != std::string::npos);
}

TEST_CASE("AsyncAgentOptions hooks are move-only", "[agent][async][u7]") {
    static_assert(!std::is_copy_constructible_v<agent::AsyncAgentOptions>);
    static_assert(!std::is_copy_assignable_v<agent::AsyncAgentOptions>);
    static_assert(std::is_move_constructible_v<agent::AsyncAgentOptions>);
}

TEST_CASE("transformContext hook prunes old messages from LLM request", "[agent][async][u8]") {
    FakeStreamingClient client;
    client.responses.push_back(ai::assistant_text_message("ok"));
    agent::AsyncToolRegistry registry;

    agent::AsyncAgentOptions options{4, "gpt-test"};
    options.transform_context = [](const std::vector<ai::MessageVariant>& messages)
        -> util::Expected<std::vector<ai::MessageVariant>> {
        if (messages.size() <= 1) {
            return messages;
        }
        return std::vector<ai::MessageVariant>{messages.back()};
    };

    agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));
    auto run = run_loop(loop, "hi");

    REQUIRE(run.result);
    REQUIRE(client.requests.size() == 1);
    REQUIRE(client.requests[0].context.messages.size() == 1);
    REQUIRE(std::holds_alternative<ai::UserMessage>(client.requests[0].context.messages[0]));
    REQUIRE(run.result->context.messages.size() == 2);
}

TEST_CASE("convertToLlm hook filters non-LLM messages", "[agent][async][u8]") {
    FakeStreamingClient client;
    client.responses.push_back(ai::assistant_text_message("ok"));
    agent::AsyncToolRegistry registry;

    agent::AsyncAgentOptions options{4, "gpt-test"};
    options.convert_to_llm = [](const std::vector<ai::MessageVariant>& messages)
        -> util::Expected<std::vector<ai::MessageVariant>> {
        std::vector<ai::MessageVariant> result;
        for (const auto& message : messages) {
            if (std::holds_alternative<ai::UserMessage>(message) ||
                std::holds_alternative<ai::AssistantMessage>(message) ||
                std::holds_alternative<ai::ToolResultMessage>(message)) {
                result.push_back(message);
            }
        }
        return result;
    };

    agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));
    auto run = run_loop(loop, "hi");

    REQUIRE(run.result);
    REQUIRE(client.requests.size() == 1);
    REQUIRE(client.requests[0].context.messages.size() == 1);
}

TEST_CASE("convertToLlm returning empty aborts with validation error", "[agent][async][u8]") {
    FakeStreamingClient client;
    client.responses.push_back(ai::assistant_text_message("ok"));
    agent::AsyncToolRegistry registry;

    agent::AsyncAgentOptions options{4, "gpt-test"};
    options.convert_to_llm = [](const std::vector<ai::MessageVariant>&)
        -> util::Expected<std::vector<ai::MessageVariant>> { return std::vector<ai::MessageVariant>{}; };

    agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));
    auto run = run_loop(loop, "hi");

    REQUIRE_FALSE(run.result);
    CHECK(run.result.error().code == util::ErrorCode::Validation);
    const auto* end_event = std::get_if<agent::AgentEndEvent>(&run.events.back());
    REQUIRE(end_event);
    CHECK_FALSE(end_event->success);
}

TEST_CASE("transformContext hook error aborts the run", "[agent][async][u8]") {
    FakeStreamingClient client;
    client.responses.push_back(ai::assistant_text_message("ok"));
    agent::AsyncToolRegistry registry;

    agent::AsyncAgentOptions options{4, "gpt-test"};
    options.transform_context = [](const std::vector<ai::MessageVariant>&)
        -> util::Expected<std::vector<ai::MessageVariant>> {
        return std::unexpected(util::make_error(util::ErrorCode::Tool, "context transform failed"));
    };

    agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));
    auto run = run_loop(loop, "hi");

    REQUIRE_FALSE(run.result);
    CHECK(run.result.error().code == util::ErrorCode::Tool);
    CHECK(run.result.error().message == "context transform failed");
}

TEST_CASE("convertToLlm hook error aborts the run", "[agent][async][u8]") {
    FakeStreamingClient client;
    client.responses.push_back(ai::assistant_text_message("ok"));
    agent::AsyncToolRegistry registry;

    agent::AsyncAgentOptions options{4, "gpt-test"};
    options.convert_to_llm = [](const std::vector<ai::MessageVariant>&)
        -> util::Expected<std::vector<ai::MessageVariant>> {
        return std::unexpected(util::make_error(util::ErrorCode::Tool, "conversion failed"));
    };

    agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));
    auto run = run_loop(loop, "hi");

    REQUIRE_FALSE(run.result);
    CHECK(run.result.error().code == util::ErrorCode::Tool);
    CHECK(run.result.error().message == "conversion failed");
}

TEST_CASE("transformContext and convertToLlm exceptions abort cleanly", "[agent][async][u8]") {
    {
        FakeStreamingClient client;
        client.responses.push_back(ai::assistant_text_message("ok"));
        agent::AsyncToolRegistry registry;

        agent::AsyncAgentOptions options{4, "gpt-test"};
        options.transform_context = [](const std::vector<ai::MessageVariant>&)
            -> util::Expected<std::vector<ai::MessageVariant>> {
            throw std::runtime_error("transform boom");
        };

        agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));
        auto run = run_loop(loop, "hi");

        REQUIRE_FALSE(run.result);
        CHECK(run.result.error().code == util::ErrorCode::Tool);
        CHECK(run.result.error().message == "transformContext hook failed");
        CHECK(run.result.error().detail.find("transform boom") != std::string::npos);
    }

    {
        FakeStreamingClient client;
        client.responses.push_back(ai::assistant_text_message("ok"));
        agent::AsyncToolRegistry registry;

        agent::AsyncAgentOptions options{4, "gpt-test"};
        options.convert_to_llm = [](const std::vector<ai::MessageVariant>&)
            -> util::Expected<std::vector<ai::MessageVariant>> {
            throw std::runtime_error("convert boom");
        };

        agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));
        auto run = run_loop(loop, "hi");

        REQUIRE_FALSE(run.result);
        CHECK(run.result.error().code == util::ErrorCode::Tool);
        CHECK(run.result.error().message == "convertToLlm hook failed");
        CHECK(run.result.error().detail.find("convert boom") != std::string::npos);
    }
}

TEST_CASE("steering message is injected before next LLM request", "[agent][async][u8]") {
    FakeStreamingClient client;
    client.responses.push_back(tool_call_response());
    client.responses.push_back(ai::assistant_text_message("done"));

    agent::AsyncToolRegistry registry;
    REQUIRE(registry.add(std::make_unique<FakeTool>(ai::Tool{"read_file", "Read", ai::JsonSchema::object()})));

    bool steering_returned = false;
    agent::AsyncAgentOptions options{4, "gpt-test"};
    options.get_steering_messages = [&]() -> util::Expected<std::vector<ai::MessageVariant>> {
        if (steering_returned) {
            return std::vector<ai::MessageVariant>{};
        }
        steering_returned = true;
        return std::vector<ai::MessageVariant>{ai::user_text_message("steer")};
    };

    agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));
    auto run = run_loop(loop, "read");

    REQUIRE(run.result);
    REQUIRE(client.requests.size() == 2);
    REQUIRE(client.requests[1].context.messages.size() == 4);
    REQUIRE(std::holds_alternative<ai::UserMessage>(client.requests[1].context.messages[1]));
    CHECK(ai::text_from_content(std::get<ai::UserMessage>(client.requests[1].context.messages[1]).content) == "steer");

    std::size_t queued_start_events = 0;
    std::size_t queued_end_events = 0;
    for (const auto& event : run.events) {
        if (const auto* start = std::get_if<agent::QueuedMessageStartEvent>(&event)) {
            if (std::holds_alternative<ai::UserMessage>(start->message)) {
                ++queued_start_events;
            }
        } else if (const auto* end = std::get_if<agent::QueuedMessageEndEvent>(&event)) {
            if (std::holds_alternative<ai::UserMessage>(end->message)) {
                ++queued_end_events;
            }
        }
    }
    CHECK(queued_start_events == 1);
    CHECK(queued_end_events == 1);
}

TEST_CASE("follow-up message triggers an additional turn", "[agent][async][u8]") {
    FakeStreamingClient client;
    client.responses.push_back(ai::assistant_text_message("first"));
    client.responses.push_back(ai::assistant_text_message("second"));

    bool follow_up_returned = false;
    agent::AsyncAgentOptions options{4, "gpt-test"};
    options.get_follow_up_messages = [&]() -> util::Expected<std::vector<ai::MessageVariant>> {
        if (follow_up_returned) {
            return std::vector<ai::MessageVariant>{};
        }
        follow_up_returned = true;
        return std::vector<ai::MessageVariant>{ai::user_text_message("follow up")};
    };

    agent::AsyncToolRegistry registry;
    agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));
    auto run = run_loop(loop, "hi");

    REQUIRE(run.result);
    CHECK(run.result->turns == 2);
    REQUIRE(client.requests.size() == 2);
    REQUIRE(client.requests[1].context.messages.size() == 3);
    REQUIRE(std::holds_alternative<ai::UserMessage>(client.requests[1].context.messages[2]));
    CHECK(ai::text_from_content(std::get<ai::UserMessage>(client.requests[1].context.messages[2]).content) == "follow up");
}

TEST_CASE("follow-up empty ends after the final response", "[agent][async][u8]") {
    FakeStreamingClient client;
    client.responses.push_back(ai::assistant_text_message("done"));

    int follow_up_calls = 0;
    agent::AsyncAgentOptions options{4, "gpt-test"};
    options.get_follow_up_messages = [&]() -> util::Expected<std::vector<ai::MessageVariant>> {
        ++follow_up_calls;
        return std::vector<ai::MessageVariant>{};
    };

    agent::AsyncToolRegistry registry;
    agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));
    auto run = run_loop(loop, "hi");

    REQUIRE(run.result);
    CHECK(run.result->turns == 1);
    CHECK(follow_up_calls == 1);
    REQUIRE(client.requests.size() == 1);
}

TEST_CASE("steering and follow-up callback failures abort the run", "[agent][async][u8]") {
    {
        FakeStreamingClient client;
        client.responses.push_back(ai::assistant_text_message("ok"));
        agent::AsyncToolRegistry registry;

        agent::AsyncAgentOptions options{4, "gpt-test"};
        options.get_steering_messages = []() -> util::Expected<std::vector<ai::MessageVariant>> {
            return std::unexpected(util::make_error(util::ErrorCode::Tool, "steering failed"));
        };

        agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));
        auto run = run_loop(loop, "hi");

        REQUIRE_FALSE(run.result);
        CHECK(run.result.error().message == "steering failed");
    }

    {
        FakeStreamingClient client;
        client.responses.push_back(ai::assistant_text_message("done"));
        agent::AsyncToolRegistry registry;

        agent::AsyncAgentOptions options{4, "gpt-test"};
        options.get_follow_up_messages = []() -> util::Expected<std::vector<ai::MessageVariant>> {
            throw std::runtime_error("follow boom");
        };

        agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));
        auto run = run_loop(loop, "hi");

        REQUIRE_FALSE(run.result);
        CHECK(run.result.error().message == "getFollowUpMessages hook failed");
        CHECK(run.result.error().detail.find("follow boom") != std::string::npos);
    }
}

TEST_CASE("prepareNextTurn sees queued steering messages", "[agent][async][u8]") {
    FakeStreamingClient client;
    client.responses.push_back(ai::assistant_text_message("first"));
    client.responses.push_back(ai::assistant_text_message("second"));

    agent::AsyncToolRegistry registry;

    int steering_calls = 0;
    std::vector<ai::MessageVariant> observed_queued;
    agent::AsyncAgentOptions options{4, "gpt-test"};
    options.get_steering_messages = [&]() -> util::Expected<std::vector<ai::MessageVariant>> {
        ++steering_calls;
        if (steering_calls == 2) {
            return std::vector<ai::MessageVariant>{ai::user_text_message("steer")};
        }
        return std::vector<ai::MessageVariant>{};
    };
    options.prepare_next_turn = [&](const agent::PrepareNextTurnContext& context)
        -> util::Expected<std::optional<agent::AgentLoopTurnUpdate>> {
        if (!context.queued_messages.empty()) {
            observed_queued = context.queued_messages;
        }
        return std::nullopt;
    };

    agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));
    auto run = run_loop(loop, "hi");

    REQUIRE(run.result);
    REQUIRE(observed_queued.size() == 1);
    REQUIRE(std::holds_alternative<ai::UserMessage>(observed_queued[0]));
    CHECK(ai::text_from_content(std::get<ai::UserMessage>(observed_queued[0]).content) == "steer");
}

TEST_CASE("prepareNextTurn model swap changes next request model", "[agent][async][u8]") {
    FakeStreamingClient client;
    client.responses.push_back(tool_call_response());
    client.responses.push_back(ai::assistant_text_message("second"));

    agent::AsyncToolRegistry registry;
    REQUIRE(registry.add(std::make_unique<FakeTool>(ai::Tool{"read_file", "Read", ai::JsonSchema::object()})));

    agent::AsyncAgentOptions options{4, "gpt-test"};
    options.prepare_next_turn = [](const agent::PrepareNextTurnContext&)
        -> util::Expected<std::optional<agent::AgentLoopTurnUpdate>> {
        return agent::AgentLoopTurnUpdate{std::nullopt, std::string{"gpt-swapped"}, std::nullopt};
    };
    options.validate_turn_update = [](const agent::AgentLoopTurnUpdate& update) -> util::ExpectedVoid {
        if (update.model && *update.model == "gpt-swapped") {
            return {};
        }
        return std::unexpected(util::make_error(util::ErrorCode::Validation, "unknown model"));
    };

    agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));
    auto run = run_loop(loop, "read");

    REQUIRE(run.result);
    REQUIRE(client.requests.size() == 2);
    CHECK(client.requests[1].model == "gpt-swapped");
    CHECK(run.result->state.model == "gpt-swapped");
}

TEST_CASE("prepareNextTurn model update without validator is rejected", "[agent][async][u8]") {
    FakeStreamingClient client;
    client.responses.push_back(tool_call_response());

    agent::AsyncToolRegistry registry;
    REQUIRE(registry.add(std::make_unique<FakeTool>(ai::Tool{"read_file", "Read", ai::JsonSchema::object()})));

    agent::AsyncAgentOptions options{4, "gpt-test"};
    options.prepare_next_turn = [](const agent::PrepareNextTurnContext&)
        -> util::Expected<std::optional<agent::AgentLoopTurnUpdate>> {
        return agent::AgentLoopTurnUpdate{std::nullopt, std::string{"gpt-swapped"}, std::nullopt};
    };

    agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));
    auto run = run_loop(loop, "read");

    REQUIRE_FALSE(run.result);
    CHECK(run.result.error().code == util::ErrorCode::Validation);
    CHECK(run.result.error().message == "model update requires validation");
}

TEST_CASE("prepareNextTurn thinking level is validated", "[agent][async][u8]") {
    FakeStreamingClient client;
    client.responses.push_back(ai::assistant_text_message("first"));

    agent::AsyncToolRegistry registry;

    agent::AsyncAgentOptions options{4, "gpt-test"};
    options.prepare_next_turn = [](const agent::PrepareNextTurnContext&)
        -> util::Expected<std::optional<agent::AgentLoopTurnUpdate>> {
        return agent::AgentLoopTurnUpdate{std::nullopt, std::nullopt, std::string{"invalid"}};
    };

    agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));
    auto run = run_loop(loop, "hi");

    REQUIRE_FALSE(run.result);
    CHECK(run.result.error().code == util::ErrorCode::Validation);
}

TEST_CASE("prepareNextTurn rejected update does not persist partial model changes", "[agent][async][u8]") {
    FakeStreamingClient client;
    client.responses.push_back(tool_call_response());
    client.responses.push_back(ai::assistant_text_message("second run"));

    agent::AsyncToolRegistry registry;
    REQUIRE(registry.add(std::make_unique<FakeTool>(ai::Tool{"read_file", "Read", ai::JsonSchema::object()})));

    int prepare_calls = 0;
    agent::AsyncAgentOptions options{4, "gpt-test"};
    options.prepare_next_turn = [&](const agent::PrepareNextTurnContext&)
        -> util::Expected<std::optional<agent::AgentLoopTurnUpdate>> {
        ++prepare_calls;
        if (prepare_calls == 1) {
            return agent::AgentLoopTurnUpdate{std::nullopt, std::string{"gpt-swapped"}, std::string{"invalid"}};
        }
        return std::nullopt;
    };
    options.validate_turn_update = [](const agent::AgentLoopTurnUpdate&) -> util::ExpectedVoid { return {}; };

    agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));
    auto first = run_loop(loop, "read");
    REQUIRE_FALSE(first.result);
    CHECK(first.result.error().code == util::ErrorCode::Validation);

    auto second = run_loop(loop, "hi again");
    REQUIRE(second.result);
    REQUIRE(client.requests.size() == 2);
    CHECK(client.requests[1].model == "gpt-test");
    CHECK(second.result->state.model == "gpt-test");
}

TEST_CASE("prepareNextTurn append_messages appends to transcript", "[agent][async][u8]") {
    FakeStreamingClient client;
    client.responses.push_back(ai::assistant_text_message("first"));

    agent::AsyncToolRegistry registry;

    agent::AsyncAgentOptions options{4, "gpt-test"};
    options.prepare_next_turn = [](const agent::PrepareNextTurnContext&)
        -> util::Expected<std::optional<agent::AgentLoopTurnUpdate>> {
        return agent::AgentLoopTurnUpdate{
            std::vector<ai::MessageVariant>{ai::user_text_message("appended")},
            std::nullopt,
            std::nullopt};
    };

    agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));
    auto run = run_loop(loop, "hi");

    REQUIRE(run.result);
    REQUIRE(run.result->context.messages.size() == 3);
    REQUIRE(std::holds_alternative<ai::UserMessage>(run.result->context.messages.back()));
    CHECK(ai::text_from_content(std::get<ai::UserMessage>(run.result->context.messages.back()).content) == "appended");
}

TEST_CASE("prepareNextTurn no update leaves model and thinking level unchanged", "[agent][async][u8]") {
    FakeStreamingClient client;
    client.responses.push_back(ai::assistant_text_message("first"));

    agent::AsyncToolRegistry registry;

    agent::AsyncAgentOptions options{4, "gpt-test"};
    options.prepare_next_turn = [](const agent::PrepareNextTurnContext&)
        -> util::Expected<std::optional<agent::AgentLoopTurnUpdate>> { return std::nullopt; };

    agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));
    auto run = run_loop(loop, "hi");

    REQUIRE(run.result);
    CHECK(run.result->state.model == "gpt-test");
    CHECK(run.result->state.thinking_level.empty());
}

TEST_CASE("prepareNextTurn valid thinking level is preserved in state", "[agent][async][u8]") {
    FakeStreamingClient client;
    client.responses.push_back(ai::assistant_text_message("first"));

    agent::AsyncToolRegistry registry;

    agent::AsyncAgentOptions options{4, "gpt-test"};
    options.prepare_next_turn = [](const agent::PrepareNextTurnContext&)
        -> util::Expected<std::optional<agent::AgentLoopTurnUpdate>> {
        return agent::AgentLoopTurnUpdate{std::nullopt, std::nullopt, std::string{"high"}};
    };

    agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));
    auto run = run_loop(loop, "hi");

    REQUIRE(run.result);
    CHECK(run.result->state.thinking_level == "high");
}

TEST_CASE("prepareNextTurn model validation hook can reject unknown models", "[agent][async][u8]") {
    FakeStreamingClient client;
    client.responses.push_back(tool_call_response());
    client.responses.push_back(ai::assistant_text_message("second"));

    agent::AsyncToolRegistry registry;
    REQUIRE(registry.add(std::make_unique<FakeTool>(ai::Tool{"read_file", "Read", ai::JsonSchema::object()})));

    agent::AsyncAgentOptions options{4, "gpt-test"};
    options.prepare_next_turn = [](const agent::PrepareNextTurnContext&)
        -> util::Expected<std::optional<agent::AgentLoopTurnUpdate>> {
        return agent::AgentLoopTurnUpdate{std::nullopt, std::string{"missing-model"}, std::nullopt};
    };
    options.validate_turn_update = [](const agent::AgentLoopTurnUpdate& update) -> util::ExpectedVoid {
        if (update.model && *update.model != "gpt-test") {
            return std::unexpected(util::make_error(util::ErrorCode::Validation, "unknown model", *update.model));
        }
        return {};
    };

    agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));
    auto run = run_loop(loop, "read");

    REQUIRE_FALSE(run.result);
    CHECK(run.result.error().code == util::ErrorCode::Validation);
    CHECK(run.result.error().message == "unknown model");
}

TEST_CASE("prepareNextTurn and turn-update validation exceptions abort cleanly", "[agent][async][u8]") {
    {
        FakeStreamingClient client;
        client.responses.push_back(ai::assistant_text_message("first"));
        agent::AsyncToolRegistry registry;

        agent::AsyncAgentOptions options{4, "gpt-test"};
        options.prepare_next_turn = [](const agent::PrepareNextTurnContext&)
            -> util::Expected<std::optional<agent::AgentLoopTurnUpdate>> {
            throw std::runtime_error("prepare boom");
        };

        agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));
        auto run = run_loop(loop, "hi");

        REQUIRE_FALSE(run.result);
        CHECK(run.result.error().message == "prepareNextTurn hook failed");
        CHECK(run.result.error().detail.find("prepare boom") != std::string::npos);
    }

    {
        FakeStreamingClient client;
        client.responses.push_back(ai::assistant_text_message("first"));
        agent::AsyncToolRegistry registry;

        agent::AsyncAgentOptions options{4, "gpt-test"};
        options.prepare_next_turn = [](const agent::PrepareNextTurnContext&)
            -> util::Expected<std::optional<agent::AgentLoopTurnUpdate>> {
            return agent::AgentLoopTurnUpdate{std::nullopt, std::string{"gpt-next"}, std::nullopt};
        };
        options.validate_turn_update = [](const agent::AgentLoopTurnUpdate&) -> util::ExpectedVoid {
            throw std::runtime_error("validator boom");
        };

        agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));
        auto run = run_loop(loop, "hi");

        REQUIRE_FALSE(run.result);
        CHECK(run.result.error().message == "validateTurnUpdate hook failed");
        CHECK(run.result.error().detail.find("validator boom") != std::string::npos);
    }
}

TEST_CASE("tool execution mode defaults to sequential", "[agent][async][u8]") {
    agent::AsyncAgentOptions options;
    CHECK(options.tool_execution_mode == ai::ToolExecutionMode::Sequential);
}

TEST_CASE("per-tool sequential override forces sequential execution", "[agent][async][u8]") {
    FakeStreamingClient client;
    client.responses.push_back(two_tool_call_response());
    client.responses.push_back(ai::assistant_text_message("done"));

    ConcurrencyProbe probe;
    agent::AsyncToolRegistry registry;
    REQUIRE(registry.add(std::make_unique<ProbedFakeTool>(
        ai::Tool{"alpha", "Alpha", ai::JsonSchema::object()},
        ai::ToolExecutionMode::Parallel,
        probe)));
    REQUIRE(registry.add(std::make_unique<ProbedFakeTool>(
        ai::Tool{"beta", "Beta", ai::JsonSchema::object()},
        ai::ToolExecutionMode::Sequential,
        probe)));

    agent::AsyncAgentOptions options{4, "gpt-test"};
    options.tool_execution_mode = ai::ToolExecutionMode::Parallel;

    agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));
    auto run = run_loop_on_pool(loop, "read");

    REQUIRE(run.result);
    REQUIRE(client.requests.size() == 2);
    CHECK(probe.max_active.load() == 1);
}

TEST_CASE("parallel tool execution runs both tools and preserves source order in transcript", "[agent][async][u8]") {
    FakeStreamingClient client;
    client.responses.push_back(two_tool_call_response());
    client.responses.push_back(ai::assistant_text_message("done"));

    agent::AsyncToolRegistry registry;
    auto alpha = std::make_unique<ConfigurableFakeTool>(
        ai::Tool{"alpha", "Alpha", ai::JsonSchema::object()},
        ai::ToolExecutionMode::Parallel,
        "alpha result");
    auto beta = std::make_unique<ConfigurableFakeTool>(
        ai::Tool{"beta", "Beta", ai::JsonSchema::object()},
        ai::ToolExecutionMode::Parallel,
        "beta result");
    auto* alpha_ptr = alpha.get();
    auto* beta_ptr = beta.get();
    REQUIRE(registry.add(std::move(alpha)));
    REQUIRE(registry.add(std::move(beta)));

    agent::AsyncAgentOptions options{4, "gpt-test"};
    options.tool_execution_mode = ai::ToolExecutionMode::Parallel;

    agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));
    auto run = run_loop_on_pool(loop, "read");

    REQUIRE(run.result);
    CHECK(alpha_ptr->invocations.size() == 1);
    CHECK(beta_ptr->invocations.size() == 1);

    REQUIRE(client.requests.size() == 2);
    const auto& second_request = client.requests[1];
    REQUIRE(second_request.context.messages.size() == 4);
    REQUIRE(std::holds_alternative<ai::ToolResultMessage>(second_request.context.messages[2]));
    REQUIRE(std::holds_alternative<ai::ToolResultMessage>(second_request.context.messages[3]));
    CHECK(std::get<ai::ToolResultMessage>(second_request.context.messages[2]).tool_name == "alpha");
    CHECK(std::get<ai::ToolResultMessage>(second_request.context.messages[3]).tool_name == "beta");
}

TEST_CASE("run-mode sequential override forces sequential execution", "[agent][async][u8]") {
    FakeStreamingClient client;
    client.responses.push_back(two_tool_call_response());
    client.responses.push_back(ai::assistant_text_message("done"));

    ConcurrencyProbe probe;
    agent::AsyncToolRegistry registry;
    REQUIRE(registry.add(std::make_unique<ProbedFakeTool>(
        ai::Tool{"alpha", "Alpha", ai::JsonSchema::object()},
        ai::ToolExecutionMode::Parallel,
        probe)));
    REQUIRE(registry.add(std::make_unique<ProbedFakeTool>(
        ai::Tool{"beta", "Beta", ai::JsonSchema::object()},
        ai::ToolExecutionMode::Parallel,
        probe)));

    agent::AsyncAgentOptions options{4, "gpt-test"};
    options.tool_execution_mode = ai::ToolExecutionMode::Sequential;

    agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));
    auto run = run_loop_on_pool(loop, "read");

    REQUIRE(run.result);
    REQUIRE(client.requests.size() == 2);
    CHECK(probe.max_active.load() == 1);
}

TEST_CASE("max_parallel_tools cap falls back to sequential execution", "[agent][async][u8]") {
    FakeStreamingClient client;
    client.responses.push_back(two_tool_call_response());
    client.responses.push_back(ai::assistant_text_message("done"));

    ConcurrencyProbe probe;
    agent::AsyncToolRegistry registry;
    REQUIRE(registry.add(std::make_unique<ProbedFakeTool>(
        ai::Tool{"alpha", "Alpha", ai::JsonSchema::object()},
        ai::ToolExecutionMode::Parallel,
        probe)));
    REQUIRE(registry.add(std::make_unique<ProbedFakeTool>(
        ai::Tool{"beta", "Beta", ai::JsonSchema::object()},
        ai::ToolExecutionMode::Parallel,
        probe)));

    agent::AsyncAgentOptions options{4, "gpt-test"};
    options.tool_execution_mode = ai::ToolExecutionMode::Parallel;
    options.max_parallel_tools = 1;

    agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));
    auto run = run_loop_on_pool(loop, "read");

    REQUIRE(run.result);
    REQUIRE(client.requests.size() == 2);
    CHECK(probe.max_active.load() == 1);
}

TEST_CASE("parallel tool execution handles beforeToolCall blocks before execution", "[agent][async][u8]") {
    FakeStreamingClient client;
    client.responses.push_back(two_tool_call_response());
    client.responses.push_back(ai::assistant_text_message("done"));

    agent::AsyncToolRegistry registry;
    REQUIRE(registry.add(std::make_unique<ConfigurableFakeTool>(
        ai::Tool{"alpha", "Alpha", ai::JsonSchema::object()},
        ai::ToolExecutionMode::Parallel,
        "alpha result")));
    REQUIRE(registry.add(std::make_unique<ConfigurableFakeTool>(
        ai::Tool{"beta", "Beta", ai::JsonSchema::object()},
        ai::ToolExecutionMode::Parallel,
        "beta result")));

    agent::AsyncAgentOptions options{4, "gpt-test"};
    options.tool_execution_mode = ai::ToolExecutionMode::Parallel;
    options.before_tool_call = [](const agent::BeforeToolCallContext& ctx) -> util::Expected<agent::BeforeToolCallResult> {
        if (ctx.tool_call.name == "alpha") {
            return agent::BeforeToolCallResult{true, "blocked alpha"};
        }
        return agent::BeforeToolCallResult{};
    };

    agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));
    auto run = run_loop_on_pool(loop, "read");

    REQUIRE(run.result);
    REQUIRE(client.requests.size() == 2);
    const auto& second_request = client.requests[1];
    REQUIRE(std::holds_alternative<ai::ToolResultMessage>(second_request.context.messages[2]));
    REQUIRE(std::holds_alternative<ai::ToolResultMessage>(second_request.context.messages[3]));
    const auto& alpha = std::get<ai::ToolResultMessage>(second_request.context.messages[2]);
    const auto& beta = std::get<ai::ToolResultMessage>(second_request.context.messages[3]);
    CHECK(alpha.is_error);
    CHECK(ai::text_from_content(alpha.content) == "blocked alpha");
    CHECK_FALSE(beta.is_error);
}

TEST_CASE("parallel tool execution aborts on beforeToolCall failure before workers start", "[agent][async][u8]") {
    FakeStreamingClient client;
    client.responses.push_back(two_tool_call_response());

    ConcurrencyProbe probe;
    agent::AsyncToolRegistry registry;
    REQUIRE(registry.add(std::make_unique<ProbedFakeTool>(
        ai::Tool{"alpha", "Alpha", ai::JsonSchema::object()},
        ai::ToolExecutionMode::Parallel,
        probe)));
    REQUIRE(registry.add(std::make_unique<ProbedFakeTool>(
        ai::Tool{"beta", "Beta", ai::JsonSchema::object()},
        ai::ToolExecutionMode::Parallel,
        probe)));

    agent::AsyncAgentOptions options{4, "gpt-test"};
    options.tool_execution_mode = ai::ToolExecutionMode::Parallel;
    options.before_tool_call = [](const agent::BeforeToolCallContext&) -> util::Expected<agent::BeforeToolCallResult> {
        return std::unexpected(util::make_error(util::ErrorCode::Tool, "preflight failed"));
    };

    agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));
    auto run = run_loop_on_pool(loop, "read");

    REQUIRE_FALSE(run.result);
    CHECK(run.result.error().message == "preflight failed");
    CHECK(probe.max_active.load() == 0);
}

TEST_CASE("parallel tool execution preserves success when one tool fails", "[agent][async][u8]") {
    FakeStreamingClient client;
    client.responses.push_back(two_tool_call_response());
    client.responses.push_back(ai::assistant_text_message("done"));

    agent::AsyncToolRegistry registry;
    REQUIRE(registry.add(std::make_unique<FailingFakeTool>(
        ai::Tool{"alpha", "Alpha", ai::JsonSchema::object()})));
    REQUIRE(registry.add(std::make_unique<ConfigurableFakeTool>(
        ai::Tool{"beta", "Beta", ai::JsonSchema::object()},
        ai::ToolExecutionMode::Parallel,
        "beta result")));

    agent::AsyncAgentOptions options{4, "gpt-test"};
    options.tool_execution_mode = ai::ToolExecutionMode::Parallel;

    agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));
    auto run = run_loop_on_pool(loop, "read");

    REQUIRE(run.result);
    REQUIRE(client.requests.size() == 2);
    const auto& second_request = client.requests[1];
    REQUIRE(std::holds_alternative<ai::ToolResultMessage>(second_request.context.messages[2]));
    REQUIRE(std::holds_alternative<ai::ToolResultMessage>(second_request.context.messages[3]));
    CHECK(std::get<ai::ToolResultMessage>(second_request.context.messages[2]).is_error);
    CHECK_FALSE(std::get<ai::ToolResultMessage>(second_request.context.messages[3]).is_error);
}

TEST_CASE("parallel tool execution emits end events in completion order", "[agent][async][u8]") {
    FakeStreamingClient client;
    client.responses.push_back(two_tool_call_response());
    client.responses.push_back(ai::assistant_text_message("done"));

    agent::AsyncToolRegistry registry;
    auto alpha = std::make_unique<DelayedFakeTool>(
        ai::Tool{"alpha", "Alpha", ai::JsonSchema::object()},
        std::chrono::milliseconds{100},
        "alpha result");
    auto beta = std::make_unique<DelayedFakeTool>(
        ai::Tool{"beta", "Beta", ai::JsonSchema::object()},
        std::chrono::milliseconds{10},
        "beta result");
    REQUIRE(registry.add(std::move(alpha)));
    REQUIRE(registry.add(std::move(beta)));

    agent::AsyncAgentOptions options{4, "gpt-test"};
    options.tool_execution_mode = ai::ToolExecutionMode::Parallel;

    agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));
    auto run = run_loop_on_pool(loop, "read");

    REQUIRE(run.result);

    std::vector<std::string> end_order;
    for (const auto& event : run.events) {
        if (const auto* end = std::get_if<agent::ToolExecutionEndEvent>(&event)) {
            end_order.push_back(end->tool_name);
        }
    }
    REQUIRE(end_order.size() == 2);
    CHECK(end_order[0] == "beta");
    CHECK(end_order[1] == "alpha");
}

TEST_CASE("parallel tool execution preserves hook failure as an agent error", "[agent][async][u8]") {
    FakeStreamingClient client;
    client.responses.push_back(two_tool_call_response());

    agent::AsyncToolRegistry registry;
    REQUIRE(registry.add(std::make_unique<ConfigurableFakeTool>(
        ai::Tool{"alpha", "Alpha", ai::JsonSchema::object()},
        ai::ToolExecutionMode::Parallel,
        "alpha result")));
    REQUIRE(registry.add(std::make_unique<ConfigurableFakeTool>(
        ai::Tool{"beta", "Beta", ai::JsonSchema::object()},
        ai::ToolExecutionMode::Parallel,
        "beta result")));

    agent::AsyncAgentOptions options{4, "gpt-test"};
    options.tool_execution_mode = ai::ToolExecutionMode::Parallel;
    options.after_tool_call = [](const agent::AfterToolCallContext& ctx) -> util::Expected<agent::AfterToolCallResult> {
        if (ctx.tool_call.name == "alpha") {
            return std::unexpected(util::make_error(util::ErrorCode::Tool, "post-processor failed"));
        }
        return agent::AfterToolCallResult{};
    };

    agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));
    auto run = run_loop_on_pool(loop, "read");

    REQUIRE_FALSE(run.result);
    CHECK(run.result.error().code == util::ErrorCode::Tool);
    CHECK(run.result.error().message == "post-processor failed");
}

TEST_CASE("parallel tool execution returns event sink failures", "[agent][async][u8]") {
    FakeStreamingClient client;
    client.responses.push_back(two_tool_call_response());

    agent::AsyncToolRegistry registry;
    REQUIRE(registry.add(std::make_unique<ConfigurableFakeTool>(
        ai::Tool{"alpha", "Alpha", ai::JsonSchema::object()},
        ai::ToolExecutionMode::Parallel,
        "alpha result")));
    REQUIRE(registry.add(std::make_unique<ConfigurableFakeTool>(
        ai::Tool{"beta", "Beta", ai::JsonSchema::object()},
        ai::ToolExecutionMode::Parallel,
        "beta result")));

    agent::AsyncAgentOptions options{4, "gpt-test"};
    options.tool_execution_mode = ai::ToolExecutionMode::Parallel;
    agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));

    boost::asio::thread_pool pool{4};
    std::optional<util::Expected<agent::AsyncAgentRunResult>> result;
    boost::asio::co_spawn(
        pool,
        [&]() -> boost::asio::awaitable<void> {
            result = co_await loop.run(
                "read",
                [](const agent::AgentLifecycleEvent& event) -> util::ExpectedVoid {
                    if (std::holds_alternative<agent::ToolExecutionEndEvent>(event)) {
                        throw std::runtime_error("sink boom");
                    }
                    return {};
                });
            co_return;
        },
        boost::asio::detached);
    pool.join();

    REQUIRE(result.has_value());
    REQUIRE_FALSE(*result);
    CHECK(result->error().code == util::ErrorCode::Tool);
    CHECK(result->error().message == "agent event sink failed");
    CHECK(result->error().detail.find("sink boom") != std::string::npos);
}

TEST_CASE("ToolExecutionMode is usable from cch::ai without agent headers", "[agent][async][u8]") {
    static_assert(std::is_enum_v<ai::ToolExecutionMode>);
    CHECK(ai::ToolExecutionMode::Sequential != ai::ToolExecutionMode::Parallel);
}
