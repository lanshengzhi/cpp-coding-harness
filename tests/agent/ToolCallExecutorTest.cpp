#include "../../third_party/catch2/catch_test_macros.hpp"

#include "agent/ToolCallExecutor.hpp"

#include "../support/ToolArgumentContracts.hpp"
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

    [[nodiscard]] std::optional<agent::ToolInvocation> first_invocation() const {
        std::lock_guard lock(invocations_mutex_);
        if (invocations_.empty()) {
            return std::nullopt;
        }
        return invocations_.front();
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
    const ai::AiContext& context = {},
    bool* tool_execution_started = nullptr) {
    boost::asio::thread_pool pool{4};
    std::optional<util::Expected<agent::ToolCallBatchResult>> result;
    std::vector<agent::AgentLifecycleEvent> events;
    std::mutex events_mutex;

    agent::AgentEventSink sink{
        [&](const agent::AgentLifecycleEvent& event) {
            std::lock_guard lock(events_mutex);
            if (tool_execution_started != nullptr &&
                std::holds_alternative<agent::ToolExecutionStartEvent>(event)) {
                *tool_execution_started = true;
            }
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
    std::string raw_arguments = R"({})") {
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
        ai::Tool{"read_file", "Read", test::empty_object_tool_argument_contract()});
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

TEST_CASE(
    "sequential execution coerces and validates the foundational argument contract before hooks and tools",
    "[agent][tool-executor][tool-arguments]") {
    const util::JsonValue contract = util::JsonValue::object_t{
        {"type", "object"},
        {"properties", util::JsonValue::object_t{
            {"enabled", util::JsonValue::object_t{{"type", "boolean"}}},
            {"binary_number", util::JsonValue::object_t{{"type", "number"}}},
            {"hexadecimal_number", util::JsonValue::object_t{{"type", "number"}}},
            {"count", util::JsonValue::object_t{{"type", "number"}}},
            {"label", util::JsonValue::object_t{{"type", "string"}}},
            {"large_label", util::JsonValue::object_t{{"type", "string"}}},
            {"scientific_label", util::JsonValue::object_t{{"type", "string"}}},
            {"tiny_label", util::JsonValue::object_t{{"type", "string"}}},
            {"spaced_number", util::JsonValue::object_t{{"type", "number"}}},
            {"nothing", util::JsonValue::object_t{{"type", "null"}}},
            {"values", util::JsonValue::object_t{{"type", "array"}}},
            {"operation", util::JsonValue::object_t{
                {"type", "string"},
                {"enum", util::JsonValue::array_t{"read", "write"}},
            }},
            {"metadata", util::JsonValue::object_t{
                {"type", "object"},
                {"additionalProperties", true},
            }},
            {"dynamic", util::JsonValue::object_t{
                {"type", "object"},
                {"additionalProperties", util::JsonValue::object_t{
                    {"type", "object"},
                    {"properties", util::JsonValue::object_t{
                        {"active", util::JsonValue::object_t{{"type", "boolean"}}},
                    }},
                    {"required", util::JsonValue::array_t{"active"}},
                    {"additionalProperties", false},
                }},
            }},
            {"settings", util::JsonValue::object_t{
                {"type", "object"},
                {"properties", util::JsonValue::object_t{
                    {"level", util::JsonValue::object_t{
                        {"type", util::JsonValue::array_t{"integer", "null"}},
                        {"const", 2},
                    }},
                }},
                {"required", util::JsonValue::array_t{"level"}},
                {"additionalProperties", false},
            }},
        }},
        {"required", util::JsonValue::array_t{
            "enabled", "binary_number", "hexadecimal_number", "count", "label", "large_label",
            "scientific_label", "tiny_label", "spaced_number", "nothing", "values", "operation", "metadata", "dynamic", "settings"}},
        {"additionalProperties", util::JsonValue::object_t{{"type", "integer"}}},
    };

    agent::AsyncToolRegistry registry;
    auto tool = std::make_unique<RecordingTool>(ai::Tool{"configure", "Configure", contract});
    auto* tool_ptr = tool.get();
    REQUIRE(registry.add(std::move(tool)));

    std::optional<util::JsonValue> hook_arguments;
    std::optional<std::string> hook_raw_arguments;
    agent::ToolCallExecutorOptions options;
    options.before_tool_call = [&](const agent::BeforeToolCallContext& context)
        -> util::Expected<agent::BeforeToolCallResult> {
        hook_arguments = context.args;
        hook_raw_arguments = context.tool_call.raw_arguments;
        return agent::BeforeToolCallResult{};
    };
    agent::ToolCallExecutor executor{registry, std::move(options)};
    const std::string raw_arguments =
        R"({"enabled":"true","binary_number":"0b10","hexadecimal_number":"0x10","count":false,"label":1000000,"large_label":100000000000000000000,"scientific_label":1e21,"tiny_label":0.000001,"spaced_number":"\u00a042\u00a0","nothing":"","values":[],"operation":"read","metadata":{"free":true},"dynamic":{"feature":{"active":"false"}},"settings":{"level":"2"},"retries":"3"})";
    auto assistant = assistant_with_calls({make_call("call-1", "configure", raw_arguments)});

    auto run = run_executor(executor, assistant);

    REQUIRE(run.result);
    REQUIRE_FALSE(run.result->results[0].is_error);
    REQUIRE(hook_arguments);
    REQUIRE(hook_raw_arguments);
    const auto invocation = tool_ptr->first_invocation();
    REQUIRE(invocation);
    auto hook_json = util::write_json(*hook_arguments);
    auto invocation_json = util::write_json(invocation->arguments);
    REQUIRE(hook_json);
    REQUIRE(invocation_json);
    CHECK(*hook_json ==
          R"({"binary_number":2,"count":0,"dynamic":{"feature":{"active":false}},"enabled":true,"hexadecimal_number":16,"label":"1000000","large_label":"100000000000000000000","metadata":{"free":true},"nothing":null,"operation":"read","retries":3,"scientific_label":"1e+21","settings":{"level":2},"spaced_number":42,"tiny_label":"0.000001","values":[]})");
    CHECK(*invocation_json == *hook_json);
    CHECK(*hook_raw_arguments == raw_arguments);
    CHECK(invocation->raw_arguments == raw_arguments);
    const auto& original_call = std::get<ai::ToolCallContent>(assistant.content[0]);
    CHECK(original_call.raw_arguments == raw_arguments);
    REQUIRE(original_call.arguments);
    const auto& original_arguments = original_call.arguments->get_object();
    CHECK(original_arguments.at("label").holds<double>());
    CHECK(original_arguments.at("label").get_number() == 1000000);
    CHECK(original_arguments.at("settings").at("level").get_string() == "2");
}

TEST_CASE(
    "radix coercion uses the recorded JavaScript number rounding",
    "[agent][tool-executor][tool-arguments]") {
    constexpr double expected = 3.9821406114177461e64;
    const util::JsonValue contract = util::JsonValue::object_t{
        {"type", "number"},
        {"const", expected},
    };
    agent::AsyncToolRegistry registry;
    auto tool = std::make_unique<RecordingTool>(ai::Tool{"radix", "Radix", contract});
    auto* tool_ptr = tool.get();
    REQUIRE(registry.add(std::move(tool)));

    std::optional<util::JsonValue> hook_arguments;
    agent::ToolCallExecutorOptions options;
    options.before_tool_call = [&](const agent::BeforeToolCallContext& context)
        -> util::Expected<agent::BeforeToolCallResult> {
        hook_arguments = context.args;
        return agent::BeforeToolCallResult{};
    };
    agent::ToolCallExecutor executor{registry, std::move(options)};
    auto assistant = assistant_with_calls({make_call(
        "call-1",
        "radix",
        R"("0x60ccebff3e1f1a18ec01839708f0fd8c49fa56d1e8821b44121dc6")")});

    auto run = run_executor(executor, assistant);

    REQUIRE(run.result);
    CHECK_FALSE(run.result->results[0].is_error);
    REQUIRE(hook_arguments);
    CHECK(hook_arguments->get_number() == expected);
    const auto invocation = tool_ptr->first_invocation();
    REQUIRE(invocation);
    CHECK(invocation->arguments.get_number() == expected);
}

TEST_CASE(
    "sequential schema-invalid arguments are isolated before hooks and tools",
    "[agent][tool-executor][tool-arguments]") {
    const util::JsonValue contract = util::JsonValue::object_t{
        {"type", "object"},
        {"description", "annotation remains non-executable"},
        {"x-contract-note", "extension remains non-executable"},
        {"properties", util::JsonValue::object_t{
            {"a.b", util::JsonValue::object_t{{"const", "safe"}}},
            {"a", util::JsonValue::object_t{
                {"type", "object"},
                {"properties", util::JsonValue::object_t{
                    {"b", util::JsonValue::object_t{{"const", "safe"}}},
                }},
            }},
            {"revision", util::JsonValue::object_t{
                {"type", "integer"},
                {"const", 1},
            }},
            {"settings", util::JsonValue::object_t{
                {"type", "object"},
                {"properties", util::JsonValue::object_t{
                    {"mode", util::JsonValue::object_t{
                        {"type", "string"},
                        {"enum", util::JsonValue::array_t{"safe"}},
                    }},
                }},
                {"required", util::JsonValue::array_t{"mode"}},
                {"additionalProperties", false},
            }},
        }},
        {"required", util::JsonValue::array_t{"a.b", "a", "revision", "settings"}},
        {"additionalProperties", false},
    };

    agent::AsyncToolRegistry registry;
    auto tool = std::make_unique<RecordingTool>(ai::Tool{"configure", "Configure", contract});
    auto* tool_ptr = tool.get();
    REQUIRE(registry.add(std::move(tool)));

    int before_calls = 0;
    agent::ToolCallExecutorOptions options;
    options.before_tool_call = [&](const agent::BeforeToolCallContext&)
        -> util::Expected<agent::BeforeToolCallResult> {
        ++before_calls;
        return agent::BeforeToolCallResult{};
    };
    agent::ToolCallExecutor executor{registry, std::move(options)};
    auto assistant = assistant_with_calls({
        make_call(
            "call-1",
            "configure",
            R"({"a.b":"danger","a":{"b":"danger"},"revision":2,"settings":{"mode":"danger"}})"),
    });

    auto run = run_executor(executor, assistant);

    REQUIRE(run.result);
    REQUIRE(run.result->results.size() == 1);
    CHECK(run.result->results[0].is_error);
    const auto diagnostic = ai::text_from_content(run.result->results[0].content);
    CHECK(diagnostic.find("configure") != std::string::npos);
    CHECK(diagnostic.find("/a.b") != std::string::npos);
    CHECK(diagnostic.find("/a/b") != std::string::npos);
    CHECK(diagnostic.find("/revision") != std::string::npos);
    CHECK(diagnostic.find("/settings/mode") != std::string::npos);
    CHECK(before_calls == 0);
    CHECK(tool_ptr->invocation_count() == 0);
    CHECK(count_events<agent::ToolExecutionStartEvent>(run.events) == 1);
    CHECK(count_events<agent::ToolExecutionEndEvent>(run.events) == 1);
    CHECK(count_events<agent::MessageStartEvent>(run.events) == 1);
    CHECK(count_events<agent::MessageEndEvent>(run.events) == 1);
}

TEST_CASE(
    "sequential preparation failures complete their lifecycle without suppressing unrelated calls",
    "[agent][tool-executor][tool-arguments]") {
    const auto strict_contract = util::JsonValue::object_t{
        {"type", "object"},
        {"required", util::JsonValue::array_t{std::string(6000, 'x')}},
        {"additionalProperties", false},
    };
    const auto unsupported_contract = util::JsonValue::object_t{
        {"type", "array"},
        {"items", util::JsonValue::object_t{{"type", "string"}}},
    };
    const auto compile_invalid_contract = util::JsonValue::object_t{
        {"type", "object"},
        {"properties", false},
    };

    agent::AsyncToolRegistry registry;
    auto malformed_tool = std::make_unique<RecordingTool>(
        ai::Tool{"malformed", "Malformed", test::empty_object_tool_argument_contract()});
    auto invalid_tool = std::make_unique<RecordingTool>(
        ai::Tool{"invalid", "Invalid", strict_contract});
    auto denied_tool = std::make_unique<RecordingTool>(
        ai::Tool{"denied", "Denied", util::JsonValue{false}});
    auto unsupported_tool = std::make_unique<RecordingTool>(
        ai::Tool{"unsupported", "Unsupported", unsupported_contract});
    auto compile_invalid_tool = std::make_unique<RecordingTool>(
        ai::Tool{"compile-invalid", "Compile invalid", compile_invalid_contract});
    auto valid_tool = std::make_unique<RecordingTool>(
        ai::Tool{"valid", "Valid", util::JsonValue{true}});
    auto* malformed_ptr = malformed_tool.get();
    auto* invalid_ptr = invalid_tool.get();
    auto* denied_ptr = denied_tool.get();
    auto* unsupported_ptr = unsupported_tool.get();
    auto* compile_invalid_ptr = compile_invalid_tool.get();
    auto* valid_ptr = valid_tool.get();
    REQUIRE(registry.add(std::move(malformed_tool)));
    REQUIRE(registry.add(std::move(invalid_tool)));
    REQUIRE(registry.add(std::move(denied_tool)));
    REQUIRE(registry.add(std::move(unsupported_tool)));
    REQUIRE(registry.add(std::move(compile_invalid_tool)));
    REQUIRE(registry.add(std::move(valid_tool)));

    bool tool_execution_started = false;
    int before_calls = 0;
    agent::ToolCallExecutorOptions options;
    options.before_tool_call = [&](const agent::BeforeToolCallContext& context)
        -> util::Expected<agent::BeforeToolCallResult> {
        CHECK(tool_execution_started);
        CHECK(context.tool_call.name == "valid");
        CHECK(context.args.holds<bool>());
        CHECK_FALSE(context.args.get_boolean());
        ++before_calls;
        return agent::BeforeToolCallResult{};
    };
    agent::ToolCallExecutor executor{registry, std::move(options)};
    auto assistant = assistant_with_calls({
        make_call("call-malformed", "malformed", "not-json"),
        make_call("call-unknown", "missing", R"({})"),
        make_call("call-invalid", "invalid", R"({})"),
        make_call("call-denied", "denied", R"({})"),
        make_call("call-unsupported", "unsupported", R"([])"),
        make_call("call-compile-invalid", "compile-invalid", R"({})"),
        make_call("call-valid", "valid", "false"),
    });

    auto run = run_executor(executor, assistant, {}, &tool_execution_started);

    REQUIRE(run.result);
    REQUIRE(run.result->results.size() == 7);
    for (std::size_t index = 0; index < 6; ++index) {
        CHECK(run.result->results[index].is_error);
        const auto diagnostic = ai::text_from_content(run.result->results[index].content);
        CHECK(diagnostic.size() <= 4096);
        CHECK(diagnostic.find(run.result->results[index].tool_name) != std::string::npos);
    }
    CHECK_FALSE(run.result->results[6].is_error);
    CHECK(ai::text_from_content(run.result->results[2].content).find("invalid") != std::string::npos);
    CHECK(ai::text_from_content(run.result->results[2].content).find("xxxxxxxx") != std::string::npos);
    CHECK(ai::text_from_content(run.result->results[4].content).find("items") != std::string::npos);
    CHECK(ai::text_from_content(run.result->results[5].content).find("properties") != std::string::npos);
    CHECK(before_calls == 1);
    CHECK(malformed_ptr->invocation_count() == 0);
    CHECK(invalid_ptr->invocation_count() == 0);
    CHECK(denied_ptr->invocation_count() == 0);
    CHECK(unsupported_ptr->invocation_count() == 0);
    CHECK(compile_invalid_ptr->invocation_count() == 0);
    CHECK(valid_ptr->invocation_count() == 1);
    CHECK(count_events<agent::ToolExecutionStartEvent>(run.events) == 7);
    CHECK(count_events<agent::ToolExecutionEndEvent>(run.events) == 7);
    CHECK(count_events<agent::MessageStartEvent>(run.events) == 7);
    CHECK(count_events<agent::MessageEndEvent>(run.events) == 7);
    REQUIRE(run.events.size() == 28);
    for (std::size_t call_index = 0; call_index < 7; ++call_index) {
        const auto event_index = call_index * 4;
        CHECK(std::holds_alternative<agent::ToolExecutionStartEvent>(run.events[event_index]));
        CHECK(std::holds_alternative<agent::ToolExecutionEndEvent>(run.events[event_index + 1]));
        CHECK(std::holds_alternative<agent::MessageStartEvent>(run.events[event_index + 2]));
        CHECK(std::holds_alternative<agent::MessageEndEvent>(run.events[event_index + 3]));
    }
}

TEST_CASE(
    "bounded preparation diagnostics preserve UTF-8 and actionable locations",
    "[agent][tool-executor][tool-arguments]") {
    std::string unknown_name = "missing-";
    for (int index = 0; index < 2000; ++index) {
        unknown_name += "\xf0\x9f\x98\x80";
    }

    agent::AsyncToolRegistry registry;
    agent::ToolCallExecutor executor{registry, agent::ToolCallExecutorOptions{}};
    auto assistant = assistant_with_calls({make_call("call-1", unknown_name, R"({})")});

    auto run = run_executor(executor, assistant);

    REQUIRE(run.result);
    REQUIRE(run.result->results.size() == 1);
    CHECK(run.result->results[0].is_error);
    const auto diagnostic = ai::text_from_content(run.result->results[0].content);
    CHECK(diagnostic.size() <= 4096);
    CHECK(diagnostic.find("missing-") != std::string::npos);
    CHECK(diagnostic.find("argument location: root") != std::string::npos);
    auto encoded = util::write_json(util::JsonValue{diagnostic});
    REQUIRE(encoded);
    auto decoded = util::read_json<util::JsonValue>(*encoded);
    REQUIRE(decoded);
    CHECK(decoded->get_string() == diagnostic);
    CHECK(count_events<agent::ToolExecutionEndEvent>(run.events) == 1);
    CHECK(count_events<agent::MessageStartEvent>(run.events) == 1);
    CHECK(count_events<agent::MessageEndEvent>(run.events) == 1);
}

TEST_CASE("exclusive tools force a bounded batch to execute sequentially", "[agent][tool-executor]") {
    ConcurrencyProbe probe;
    agent::AsyncToolRegistry registry;
    REQUIRE(registry.add(std::make_unique<RecordingTool>(
        ai::Tool{"alpha", "Alpha", test::empty_object_tool_argument_contract()},
        agent::ToolConcurrency::ParallelSafe,
        "alpha",
        std::chrono::milliseconds{25},
        &probe)));
    REQUIRE(registry.add(std::make_unique<RecordingTool>(
        ai::Tool{"beta", "Beta", test::empty_object_tool_argument_contract()},
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
        ai::Tool{"lookup", "Lookup", test::empty_object_tool_argument_contract()},
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
        ai::Tool{"lookup", "Lookup", test::empty_object_tool_argument_contract()},
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
        ai::Tool{"lookup", "Lookup", test::empty_object_tool_argument_contract()},
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
        ai::Tool{"alpha", "Alpha", test::empty_object_tool_argument_contract()},
        agent::ToolConcurrency::ParallelSafe,
        "alpha result",
        std::chrono::milliseconds{80})));
    REQUIRE(registry.add(std::make_unique<RecordingTool>(
        ai::Tool{"beta", "Beta", test::empty_object_tool_argument_contract()},
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
        ai::Tool{"alpha", "Alpha", test::empty_object_tool_argument_contract()},
        agent::ToolConcurrency::ParallelSafe,
        "alpha",
        std::chrono::milliseconds{10})));
    REQUIRE(registry.add(std::make_unique<RecordingTool>(
        ai::Tool{"beta", "Beta", test::empty_object_tool_argument_contract()},
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
        ai::Tool{"read_file", "Read", test::empty_object_tool_argument_contract()})));
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
        ai::Tool{"read_file", "Read", test::empty_object_tool_argument_contract()});
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
        ai::Tool{"read_file", "Read", test::empty_object_tool_argument_contract()})));

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
        ai::Tool{"read_file", "Read", test::empty_object_tool_argument_contract()})));
    agent::ToolCallExecutor executor{registry, agent::ToolCallExecutorOptions{}};
    auto assistant = assistant_with_calls({make_call("call-1", "read_file")});

    auto run = run_executor(executor, assistant);

    REQUIRE(run.result);
    CHECK_FALSE(run.result->terminate_batch);
    REQUIRE(run.result->results.size() == 1);
    CHECK(run.result->results[0].is_error);
}
