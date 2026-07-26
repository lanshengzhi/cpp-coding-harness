#include "../../third_party/catch2/catch_test_macros.hpp"

#include "agent/ToolCallExecutor.hpp"

#include "support/ToolArgumentCompatibilityFixture.hpp"
#include "support/ToolArgumentContracts.hpp"
#include "util/Json.hpp"
#include <cch/agent/AgentContext.hpp>
#include <cch/agent/ToolRegistry.hpp>
#include <cch/ai/Content.hpp>
#include <cch/ai/Context.hpp>
#include <cch/ai/Message.hpp>
#include <cch/ai/Tool.hpp>
#include <cch/util/Error.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
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
            .content = std::vector<ai::Content>{ai::text_content(result_text_)},
            .details = std::nullopt,
            .is_error = false};
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

    [[nodiscard]] std::vector<agent::ToolInvocation> invocations() const {
        std::lock_guard lock(invocations_mutex_);
        return invocations_;
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

util::JsonValue fixture_json(std::string_view json) {
    auto parsed = util::read_json<util::JsonValue>(json);
    REQUIRE(parsed);
    return std::move(*parsed);
}

void check_format_fixture(
    std::string_view format,
    std::optional<std::string_view> valid,
    std::string_view invalid) {
    agent::AsyncToolRegistry registry;
    auto tool = std::make_unique<RecordingTool>(ai::Tool{
        "format-fixture",
        "Format fixture",
        util::JsonValue::object_t{
            {"type", "string"},
            {"format", std::string(format)},
        }});
    auto* tool_ptr = tool.get();
    REQUIRE(registry.add(std::move(tool)));

    std::vector<ai::ToolCallContent> calls;
    if (valid) {
        auto valid_json = util::write_json(util::JsonValue{std::string(*valid)});
        REQUIRE(valid_json);
        calls.push_back(make_call("call-valid", "format-fixture", *valid_json));
    }
    auto invalid_json = util::write_json(util::JsonValue{std::string(invalid)});
    REQUIRE(invalid_json);
    calls.push_back(make_call("call-invalid", "format-fixture", *invalid_json));

    agent::ToolCallExecutor executor{registry, agent::ToolCallExecutorOptions{}};
    auto assistant = assistant_with_calls(std::move(calls));
    auto run = run_executor(executor, assistant);

    REQUIRE(run.result);
    REQUIRE(run.result->results.size() == (valid ? 2 : 1));
    if (valid) CHECK_FALSE(run.result->results[0].is_error);
    CHECK(run.result->results.back().is_error);
    CHECK(tool_ptr->invocation_count() == (valid ? 1 : 0));
}

void check_json_format_fixture(const tests::JsonFormatFixture& fixture) {
    agent::AsyncToolRegistry registry;
    auto tool = std::make_unique<RecordingTool>(ai::Tool{
        "json-format-fixture",
        "JSON format fixture",
        util::JsonValue::object_t{
            {"type", "string"},
            {"format", std::string(fixture.format)},
        }});
    auto* tool_ptr = tool.get();
    REQUIRE(registry.add(std::move(tool)));

    agent::ToolCallExecutor executor{registry, agent::ToolCallExecutorOptions{}};
    auto assistant = assistant_with_calls({make_call(
        "call-format",
        "json-format-fixture",
        std::string(fixture.raw_json))});
    auto run = run_executor(executor, assistant);

    REQUIRE(run.result);
    REQUIRE(run.result->results.size() == 1);
    CHECK(run.result->results[0].is_error == !fixture.accepted);
    CHECK(tool_ptr->invocation_count() == (fixture.accepted ? 1 : 0));
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
    agent::BeforeToolCallHook before_hook =
        agent::adapt_sync_before_tool_call(
            [&](const agent::BeforeToolCallContext& context)
        -> util::Expected<agent::BeforeToolCallResult> {
        hook_arguments = context.args;
        hook_raw_arguments = context.tool_call.raw_arguments;
        return agent::BeforeToolCallResult{};
    });
    agent::ToolCallExecutorOptions options;
    options.before_tool_call = &before_hook;
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
    agent::BeforeToolCallHook before_hook =
        agent::adapt_sync_before_tool_call(
            [&](const agent::BeforeToolCallContext& context)
        -> util::Expected<agent::BeforeToolCallResult> {
        hook_arguments = context.args;
        return agent::BeforeToolCallResult{};
    });
    agent::ToolCallExecutorOptions options;
    options.before_tool_call = &before_hook;
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
    "sequential execution recursively prepares schema and tuple array items",
    "[agent][tool-executor][tool-arguments][compatibility-fixture]") {
    agent::AsyncToolRegistry registry;
    auto tool = std::make_unique<RecordingTool>(ai::Tool{
        "collections",
        "Collections",
        fixture_json(tests::kRecursiveCollectionContract)});
    auto* tool_ptr = tool.get();
    REQUIRE(registry.add(std::move(tool)));

    agent::ToolCallExecutor executor{registry, agent::ToolCallExecutorOptions{}};
    auto assistant = assistant_with_calls({make_call(
        "call-1",
        "collections",
        std::string(tests::kRecursiveCollectionArguments))});

    auto run = run_executor(executor, assistant);

    REQUIRE(run.result);
    REQUIRE_FALSE(run.result->results[0].is_error);
    const auto invocation = tool_ptr->first_invocation();
    REQUIRE(invocation);
    auto prepared = util::write_json(invocation->arguments);
    REQUIRE(prepared);
    CHECK(*prepared == tests::kRecursiveCollectionExpected);
}

TEST_CASE(
    "sequential execution enforces numeric string object and array constraints",
    "[agent][tool-executor][tool-arguments][compatibility-fixture]") {
    agent::AsyncToolRegistry registry;
    auto tool = std::make_unique<RecordingTool>(ai::Tool{
        "bounded",
        "Bounded",
        fixture_json(tests::kBoundedContract)});
    auto* tool_ptr = tool.get();
    REQUIRE(registry.add(std::move(tool)));

    agent::ToolCallExecutor executor{registry, agent::ToolCallExecutorOptions{}};
    auto assistant = assistant_with_calls({
        make_call("call-valid", "bounded", std::string(tests::kBoundedValidArguments)),
        make_call("call-invalid", "bounded", std::string(tests::kBoundedInvalidArguments)),
        make_call("call-upper", "bounded", std::string(tests::kBoundedUpperInvalidArguments)),
        make_call("call-lower", "bounded", std::string(tests::kBoundedLowerInvalidArguments)),
    });

    auto run = run_executor(executor, assistant);

    REQUIRE(run.result);
    REQUIRE(run.result->results.size() == 4);
    CHECK_FALSE(run.result->results[0].is_error);
    CHECK(run.result->results[1].is_error);
    CHECK(run.result->results[2].is_error);
    CHECK(run.result->results[3].is_error);
    CHECK(tool_ptr->invocation_count() == 1);
    const auto diagnostic = ai::text_from_content(run.result->results[1].content);
    CHECK(diagnostic.find("/quantity") != std::string::npos);
    CHECK(diagnostic.find("/label") != std::string::npos);
    CHECK(diagnostic.find("/metadata") != std::string::npos);
    CHECK(diagnostic.find("/values") != std::string::npos);
    const auto upper_diagnostic = ai::text_from_content(run.result->results[2].content);
    CHECK(upper_diagnostic.find("/quantity") != std::string::npos);
    CHECK(upper_diagnostic.find("/label") != std::string::npos);
    CHECK(upper_diagnostic.find("/metadata") != std::string::npos);
    CHECK(upper_diagnostic.find("/values") != std::string::npos);
    CHECK(ai::text_from_content(run.result->results[3].content).find("/quantity") != std::string::npos);
}

TEST_CASE(
    "recorded TypeBox boundary behavior remains executable",
    "[agent][tool-executor][tool-arguments][compatibility-fixture]") {
    agent::AsyncToolRegistry registry;
    auto tool = std::make_unique<RecordingTool>(ai::Tool{
        "baseline-boundaries",
        "Baseline boundaries",
        fixture_json(tests::kTypeBoxBoundaryContract)});
    auto* tool_ptr = tool.get();
    REQUIRE(registry.add(std::move(tool)));

    agent::ToolCallExecutor executor{registry, agent::ToolCallExecutorOptions{}};
    auto assistant = assistant_with_calls({make_call(
        "call-boundaries",
        "baseline-boundaries",
        std::string(tests::kTypeBoxBoundaryArguments))});

    auto run = run_executor(executor, assistant);

    REQUIRE(run.result);
    CHECK_FALSE(run.result->results[0].is_error);
    CHECK(tool_ptr->invocation_count() == 1);
}

TEST_CASE(
    "recorded TypeBox Unicode string-bound fast paths remain executable",
    "[agent][tool-executor][tool-arguments][compatibility-fixture]") {
    agent::AsyncToolRegistry registry;
    auto max_one_tool = std::make_unique<RecordingTool>(ai::Tool{
        "max-one",
        "Maximum one TypeBox string unit",
        util::JsonValue::object_t{{"type", "string"}, {"maxLength", 1}}});
    auto min_two_tool = std::make_unique<RecordingTool>(ai::Tool{
        "min-two",
        "Minimum two TypeBox string units",
        util::JsonValue::object_t{{"type", "string"}, {"minLength", 2}}});
    auto* max_one_ptr = max_one_tool.get();
    auto* min_two_ptr = min_two_tool.get();
    REQUIRE(registry.add(std::move(max_one_tool)));
    REQUIRE(registry.add(std::move(min_two_tool)));

    std::vector<ai::ToolCallContent> calls;
    for (std::size_t index = 0; index < tests::kStringBoundFixtures.size(); ++index) {
        const auto& fixture = tests::kStringBoundFixtures[index];
        calls.push_back(make_call(
            "call-max-" + std::to_string(index),
            "max-one",
            std::string(fixture.raw_json)));
        calls.push_back(make_call(
            "call-min-" + std::to_string(index),
            "min-two",
            std::string(fixture.raw_json)));
    }

    agent::ToolCallExecutor executor{registry, agent::ToolCallExecutorOptions{}};
    auto assistant = assistant_with_calls(std::move(calls));
    auto run = run_executor(executor, assistant);

    REQUIRE(run.result);
    REQUIRE(run.result->results.size() == tests::kStringBoundFixtures.size() * 2);
    std::size_t expected_max_invocations = 0;
    std::size_t expected_min_invocations = 0;
    for (std::size_t index = 0; index < tests::kStringBoundFixtures.size(); ++index) {
        const auto& fixture = tests::kStringBoundFixtures[index];
        CHECK(run.result->results[index * 2].is_error ==
              !fixture.accepted_by_max_length_one);
        CHECK(run.result->results[index * 2 + 1].is_error ==
              !fixture.accepted_by_min_length_two);
        expected_max_invocations += fixture.accepted_by_max_length_one ? 1 : 0;
        expected_min_invocations += fixture.accepted_by_min_length_two ? 1 : 0;
    }
    CHECK(max_one_ptr->invocation_count() == expected_max_invocations);
    CHECK(min_two_ptr->invocation_count() == expected_min_invocations);
}

TEST_CASE(
    "array item fixtures enforce boolean schemas additional items and patterns",
    "[agent][tool-executor][tool-arguments][compatibility-fixture]") {
    agent::AsyncToolRegistry registry;
    auto denied_items = std::make_unique<RecordingTool>(ai::Tool{
        "denied-items",
        "Denied items",
        util::JsonValue::object_t{{"type", "array"}, {"items", false}}});
    auto tuple_items = std::make_unique<RecordingTool>(ai::Tool{
        "tuple-items",
        "Tuple items",
        util::JsonValue::object_t{
            {"type", "array"},
            {"items", util::JsonValue::array_t{
                util::JsonValue::object_t{{"type", "string"}, {"pattern", "^[a-z]+$"}},
            }},
            {"additionalItems", util::JsonValue::object_t{{"type", "integer"}}},
        }});
    auto code_point_pattern = std::make_unique<RecordingTool>(ai::Tool{
        "code-point-pattern",
        "Code point pattern",
        util::JsonValue::object_t{{"type", "string"}, {"pattern", "^.$"}}});
    auto four_point_pattern = std::make_unique<RecordingTool>(ai::Tool{
        "four-point-pattern",
        "Four point pattern",
        util::JsonValue::object_t{{"type", "string"}, {"pattern", "^....$"}}});
    auto unsupported_pattern = std::make_unique<RecordingTool>(ai::Tool{
        "unsupported-pattern",
        "Unsupported pattern",
        util::JsonValue::object_t{{"type", "string"}, {"pattern", R"(^\p{Emoji}$)"}}});
    auto* denied_ptr = denied_items.get();
    auto* tuple_ptr = tuple_items.get();
    auto* code_point_ptr = code_point_pattern.get();
    auto* four_point_ptr = four_point_pattern.get();
    auto* unsupported_ptr = unsupported_pattern.get();
    REQUIRE(registry.add(std::move(denied_items)));
    REQUIRE(registry.add(std::move(tuple_items)));
    REQUIRE(registry.add(std::move(code_point_pattern)));
    REQUIRE(registry.add(std::move(four_point_pattern)));
    REQUIRE(registry.add(std::move(unsupported_pattern)));

    agent::ToolCallExecutor executor{registry, agent::ToolCallExecutorOptions{}};
    auto assistant = assistant_with_calls({
        make_call("call-denied", "denied-items", R"([1])"),
        make_call("call-valid", "tuple-items", R"(["name",2])"),
        make_call("call-pattern", "tuple-items", R"(["NAME",2])"),
        make_call("call-additional", "tuple-items", R"(["name","2"])"),
        make_call("call-code-point", "code-point-pattern", R"("😀")"),
        make_call("call-byte-regression", "four-point-pattern", R"("😀")"),
        make_call("call-four-points", "four-point-pattern", R"("abcd")"),
        make_call("call-unsupported-pattern", "unsupported-pattern", R"("😀")"),
    });

    auto run = run_executor(executor, assistant);

    REQUIRE(run.result);
    REQUIRE(run.result->results.size() == 8);
    CHECK(run.result->results[0].is_error);
    CHECK_FALSE(run.result->results[1].is_error);
    CHECK(run.result->results[2].is_error);
    CHECK(run.result->results[3].is_error);
    CHECK_FALSE(run.result->results[4].is_error);
    CHECK(run.result->results[5].is_error);
    CHECK_FALSE(run.result->results[6].is_error);
    CHECK(run.result->results[7].is_error);
    CHECK(denied_ptr->invocation_count() == 0);
    CHECK(tuple_ptr->invocation_count() == 1);
    CHECK(code_point_ptr->invocation_count() == 1);
    CHECK(four_point_ptr->invocation_count() == 1);
    CHECK(unsupported_ptr->invocation_count() == 0);
}

TEST_CASE(
    "composition coercion selects only satisfying branches and validates the final contract",
    "[agent][tool-executor][tool-arguments][compatibility-fixture]") {
    agent::AsyncToolRegistry registry;
    auto composed_tool = std::make_unique<RecordingTool>(ai::Tool{
        "composed",
        "Composed",
        fixture_json(tests::kCompositionContract)});
    auto branch_tool = std::make_unique<RecordingTool>(ai::Tool{
        "branch",
        "Branch",
        util::JsonValue::object_t{{"anyOf", util::JsonValue::array_t{
            util::JsonValue::object_t{{"type", "integer"}, {"minimum", 2}},
            util::JsonValue::object_t{{"type", "string"}, {"const", "fallback"}},
        }}}});
    auto ambiguous_tool = std::make_unique<RecordingTool>(ai::Tool{
        "ambiguous",
        "Ambiguous",
        util::JsonValue::object_t{{"oneOf", util::JsonValue::array_t{
            util::JsonValue::object_t{{"type", "number"}},
            util::JsonValue::object_t{{"type", "integer"}},
        }}}});
    auto* composed_ptr = composed_tool.get();
    auto* branch_ptr = branch_tool.get();
    auto* ambiguous_ptr = ambiguous_tool.get();
    REQUIRE(registry.add(std::move(composed_tool)));
    REQUIRE(registry.add(std::move(branch_tool)));
    REQUIRE(registry.add(std::move(ambiguous_tool)));

    agent::ToolCallExecutor executor{registry, agent::ToolCallExecutorOptions{}};
    auto assistant = assistant_with_calls({
        make_call("call-valid", "composed", std::string(tests::kCompositionArguments)),
        make_call("call-no-branch", "branch", R"("1")"),
        make_call("call-ambiguous", "ambiguous", "3"),
    });

    auto run = run_executor(executor, assistant);

    REQUIRE(run.result);
    REQUIRE(run.result->results.size() == 3);
    CHECK_FALSE(run.result->results[0].is_error);
    CHECK(run.result->results[1].is_error);
    CHECK(run.result->results[2].is_error);
    const auto invocation = composed_ptr->first_invocation();
    REQUIRE(invocation);
    auto prepared = util::write_json(invocation->arguments);
    REQUIRE(prepared);
    CHECK(*prepared == tests::kCompositionExpected);
    CHECK(branch_ptr->invocation_count() == 0);
    CHECK(ambiguous_ptr->invocation_count() == 0);
}

TEST_CASE(
    "recorded baseline formats reject invalid strings while unknown formats remain annotations",
    "[agent][tool-executor][tool-arguments][compatibility-fixture]") {
    for (const auto& fixture : tests::kRecognizedFormatFixtures) {
        check_format_fixture(fixture.format, fixture.valid, fixture.invalid);
    }
    for (const auto& fixture : tests::kRejectedFormatRegressionFixtures) {
        check_format_fixture(fixture.format, std::nullopt, fixture.value);
    }

    agent::AsyncToolRegistry registry;
    auto annotation_tool = std::make_unique<RecordingTool>(ai::Tool{
        "annotation-format",
        "Annotation format",
        util::JsonValue::object_t{
            {"type", "string"},
            {"format", "project-local-identifier"},
        }});
    auto* annotation_ptr = annotation_tool.get();
    REQUIRE(registry.add(std::move(annotation_tool)));
    agent::ToolCallExecutor executor{registry, agent::ToolCallExecutorOptions{}};
    auto assistant = assistant_with_calls({make_call(
        "call-annotation",
        "annotation-format",
        R"("not-project-shaped")")});

    auto run = run_executor(executor, assistant);

    REQUIRE(run.result);
    CHECK_FALSE(run.result->results[0].is_error);
    CHECK(annotation_ptr->invocation_count() == 1);
}

TEST_CASE(
    "recorded TypeBox IDN separator behavior remains executable",
    "[agent][tool-executor][tool-arguments][compatibility-fixture]") {
    for (const auto& fixture : tests::kIdnSeparatorFixtures) {
        check_json_format_fixture(fixture);
    }
}

TEST_CASE(
    "unsupported dialect vocabulary reference and executable constructs fail closed",
    "[agent][tool-executor][tool-arguments][compatibility-fixture]") {
    agent::AsyncToolRegistry registry;
    std::vector<RecordingTool*> rejected_tools;
    auto add_rejected = [&](std::string name, util::JsonValue contract) {
        auto tool = std::make_unique<RecordingTool>(
            ai::Tool{name, "Rejected fixture", std::move(contract)});
        rejected_tools.push_back(tool.get());
        REQUIRE(registry.add(std::move(tool)));
    };
    add_rejected("dialect", util::JsonValue::object_t{
        {"$schema", "https://example.test/unsupported-schema"},
        {"type", "object"},
    });
    add_rejected("vocabulary", util::JsonValue::object_t{
        {"$vocabulary", util::JsonValue::object_t{{"https://example.test/vocab", true}}},
        {"type", "object"},
    });
    add_rejected("reference", util::JsonValue::object_t{{"$ref", "#/$defs/missing"}});
    add_rejected("contains", util::JsonValue::object_t{
        {"type", "array"},
        {"contains", util::JsonValue::object_t{{"type", "string"}}},
    });
    add_rejected("dialect-items", util::JsonValue::object_t{
        {"$schema", "https://json-schema.org/draft/2020-12/schema"},
        {"type", "array"},
        {"items", util::JsonValue::array_t{util::JsonValue::object_t{{"type", "string"}}}},
    });
    add_rejected("mandatory-format", util::JsonValue::object_t{
        {"$vocabulary", util::JsonValue::object_t{
            {"https://json-schema.org/draft/2020-12/vocab/format-assertion", true},
        }},
        {"type", "string"},
        {"format", "project-local-identifier"},
    });

    auto annotation_tool = std::make_unique<RecordingTool>(ai::Tool{
        "annotations",
        "Annotations",
        util::JsonValue::object_t{
            {"$schema", "http://json-schema.org/draft-07/schema#"},
            {"$vocabulary", util::JsonValue::object_t{{"https://example.test/optional", false}}},
            {"type", "number"},
            {"minimum", 2},
            {"format", "number-format-is-annotation"},
            {"x-contract", util::JsonValue::object_t{{"minimum", 100}}},
        }});
    auto* annotation_ptr = annotation_tool.get();
    REQUIRE(registry.add(std::move(annotation_tool)));

    agent::ToolCallExecutor executor{registry, agent::ToolCallExecutorOptions{}};
    auto assistant = assistant_with_calls({
        make_call("call-dialect", "dialect", R"({})"),
        make_call("call-vocabulary", "vocabulary", R"({})"),
        make_call("call-reference", "reference", R"({})"),
        make_call("call-contains", "contains", R"([])"),
        make_call("call-dialect-items", "dialect-items", R"([])"),
        make_call("call-format", "mandatory-format", R"("value")"),
        make_call("call-annotations", "annotations", "2"),
    });

    auto run = run_executor(executor, assistant);

    REQUIRE(run.result);
    REQUIRE(run.result->results.size() == 7);
    for (std::size_t index = 0; index < rejected_tools.size(); ++index) {
        CHECK(run.result->results[index].is_error);
        CHECK(rejected_tools[index]->invocation_count() == 0);
    }
    CHECK_FALSE(run.result->results[6].is_error);
    CHECK(annotation_ptr->invocation_count() == 1);
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
    agent::BeforeToolCallHook before_hook =
        agent::adapt_sync_before_tool_call(
            [&](const agent::BeforeToolCallContext&)
        -> util::Expected<agent::BeforeToolCallResult> {
        ++before_calls;
        return agent::BeforeToolCallResult{};
    });
    agent::ToolCallExecutorOptions options;
    options.before_tool_call = &before_hook;
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
        {"contains", util::JsonValue::object_t{{"type", "string"}}},
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
    agent::BeforeToolCallHook before_hook =
        agent::adapt_sync_before_tool_call(
            [&](const agent::BeforeToolCallContext& context)
        -> util::Expected<agent::BeforeToolCallResult> {
        CHECK(tool_execution_started);
        CHECK(context.tool_call.name == "valid");
        CHECK(context.args.holds<bool>());
        CHECK_FALSE(context.args.get_boolean());
        ++before_calls;
        return agent::BeforeToolCallResult{};
    });
    agent::ToolCallExecutorOptions options;
    options.before_tool_call = &before_hook;
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
    CHECK(ai::text_from_content(run.result->results[4].content).find("contains") != std::string::npos);
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
    "malformed argument diagnostics redact before truncation and preserve peer calls",
    "[agent][tool-executor][tool-arguments][issue33]") {
    const std::string secret = "sk-FAKEBOUNDARYCREDENTIAL123456789";
    std::string parser_prefix = "1:4060: syntax_error\n   ";
    parser_prefix.push_back(static_cast<char>(0xff));
    parser_prefix += " invalid-byte ";
    constexpr std::string_view truncation_suffix = " [diagnostic truncated]";
    constexpr std::size_t diagnostic_keep = 4096 - truncation_suffix.size();
    const std::string diagnostic_prefix =
        "Tool Argument Contract preparation failed at root for tool \"malformed\": "
        "arguments are malformed JSON (parser detail: " + parser_prefix;
    REQUIRE(diagnostic_prefix.size() + 5 < diagnostic_keep);
    const std::string parser_detail =
        parser_prefix +
        std::string(diagnostic_keep - diagnostic_prefix.size() - 5, 'x') +
        secret + " BROKEN\n   ^";

    agent::AsyncToolRegistry registry;
    auto malformed_tool = std::make_unique<RecordingTool>(
        ai::Tool{"malformed", "Malformed", test::empty_object_tool_argument_contract()});
    auto valid_tool = std::make_unique<RecordingTool>(
        ai::Tool{"valid", "Valid", util::JsonValue{true}});
    auto* malformed_ptr = malformed_tool.get();
    auto* valid_ptr = valid_tool.get();
    REQUIRE(registry.add(std::move(malformed_tool)));
    REQUIRE(registry.add(std::move(valid_tool)));

    int before_calls = 0;
    agent::BeforeToolCallHook before_hook =
        agent::adapt_sync_before_tool_call(
            [&](const agent::BeforeToolCallContext& context)
        -> util::Expected<agent::BeforeToolCallResult> {
        ++before_calls;
        CHECK(context.tool_call.name == "valid");
        return agent::BeforeToolCallResult{};
    });
    agent::ToolCallExecutorOptions options;
    options.before_tool_call = &before_hook;
    agent::ToolCallExecutor executor{registry, std::move(options)};

    auto malformed = make_call("call-malformed", "malformed", "not-json");
    malformed.argument_error = parser_detail;
    auto assistant = assistant_with_calls({
        std::move(malformed),
        make_call("call-valid", "valid", "false"),
    });

    auto run = run_executor(executor, assistant);

    REQUIRE(run.result);
    REQUIRE(run.result->results.size() == 2);
    CHECK(run.result->results[0].is_error);
    CHECK_FALSE(run.result->results[1].is_error);
    const auto diagnostic = ai::text_from_content(run.result->results[0].content);
    CHECK(diagnostic.size() <= 4096);
    CHECK(diagnostic.find("malformed") != std::string::npos);
    CHECK(diagnostic.find("arguments are malformed JSON") != std::string::npos);
    CHECK(diagnostic.find("[REDACTED]") != std::string::npos);
    CHECK(diagnostic.find("\xef\xbf\xbd") != std::string::npos);
    CHECK(diagnostic.find(secret) == std::string::npos);
    CHECK(diagnostic.find("sk-") == std::string::npos);

    std::optional<std::string> end_diagnostic;
    std::optional<std::string> message_diagnostic;
    for (const auto& event : run.events) {
        if (const auto* end = std::get_if<agent::ToolExecutionEndEvent>(&event);
            end != nullptr && end->tool_call_id == "call-malformed") {
            end_diagnostic = ai::text_from_content(end->result.content);
        }
        if (const auto* end = std::get_if<agent::MessageEndEvent>(&event);
            end != nullptr && std::holds_alternative<ai::ToolResultMessage>(end->message)) {
            const auto& result = std::get<ai::ToolResultMessage>(end->message);
            if (result.tool_call_id == "call-malformed") {
                message_diagnostic = ai::text_from_content(result.content);
            }
        }
    }
    REQUIRE(end_diagnostic.has_value());
    REQUIRE(message_diagnostic.has_value());
    CHECK(*end_diagnostic == diagnostic);
    CHECK(*message_diagnostic == diagnostic);

    auto encoded = util::write_json(util::JsonValue{diagnostic});
    REQUIRE(encoded);
    auto decoded = util::read_json<util::JsonValue>(*encoded);
    REQUIRE(decoded);
    CHECK(decoded->get_string() == diagnostic);

    CHECK(before_calls == 1);
    CHECK(malformed_ptr->invocation_count() == 0);
    CHECK(valid_ptr->invocation_count() == 1);
}

TEST_CASE(
    "schema validation diagnostics keep credential-like locations actionable",
    "[agent][tool-executor][tool-arguments][issue33]") {
    const auto contract = util::JsonValue::object_t{
        {"type", "object"},
        {"properties", util::JsonValue::object_t{
            {"api_key", util::JsonValue::object_t{{"type", "string"}}},
        }},
        {"required", util::JsonValue::array_t{"api_key"}},
        {"additionalProperties", false},
    };

    agent::AsyncToolRegistry registry;
    REQUIRE(registry.add(std::make_unique<RecordingTool>(
        ai::Tool{"configure", "Configure", contract})));
    agent::ToolCallExecutor executor{registry, agent::ToolCallExecutorOptions{}};
    auto assistant = assistant_with_calls({
        make_call("call-invalid", "configure", R"({"api_key":{}})"),
    });

    auto run = run_executor(executor, assistant);

    REQUIRE(run.result);
    REQUIRE(run.result->results.size() == 1);
    const auto diagnostic = ai::text_from_content(run.result->results[0].content);
    CHECK(diagnostic.find(
        "/api_key: value does not match an allowed JSON type") != std::string::npos);
    CHECK(diagnostic.find("[REDACTED]") == std::string::npos);
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

TEST_CASE(
    "sequential and bounded parallel batches share argument preparation semantics",
    "[agent][tool-executor][tool-arguments][issue27]") {
    const auto strict_contract = test::integer_value_tool_argument_contract();
    auto assistant = assistant_with_calls({
        make_call("call-invalid", "work", R"({"value":"not-an-integer"})"),
        make_call("call-1", "work", R"({"value":"1"})"),
        make_call("call-2", "work", R"({"value":"2"})"),
        make_call("call-3", "work", R"({"value":"3"})"),
    });

    struct ExecutionSnapshot {
        std::vector<bool> result_errors;
        std::vector<std::string> result_text;
        std::vector<std::string> before_hook_ids;
        std::vector<std::string> before_hook_arguments;
        std::vector<std::string> result_message_ids;
        std::vector<double> invocation_values;
        std::size_t execution_starts{};
        std::size_t execution_ends{};
        std::size_t message_starts{};
        std::size_t message_ends{};
        int max_active{};
    };

    auto execute_policy = [&](agent::ToolExecutionPolicy policy) {
        ConcurrencyProbe probe;
        agent::AsyncToolRegistry registry;
        auto tool = std::make_unique<RecordingTool>(
            ai::Tool{"work", "Work", strict_contract},
            agent::ToolConcurrency::ParallelSafe,
            "work result",
            std::chrono::milliseconds{30},
            &probe);
        auto* tool_ptr = tool.get();
        REQUIRE(registry.add(std::move(tool)));

        ExecutionSnapshot snapshot;
        agent::BeforeToolCallHook before_hook =
            agent::adapt_sync_before_tool_call(
                [&](const agent::BeforeToolCallContext& context)
            -> util::Expected<agent::BeforeToolCallResult> {
            snapshot.before_hook_ids.push_back(context.tool_call.id);
            auto encoded = util::write_json(context.args);
            REQUIRE(encoded);
            snapshot.before_hook_arguments.push_back(std::move(*encoded));
            return agent::BeforeToolCallResult{};
        });
        agent::ToolCallExecutorOptions options;
        options.execution = std::move(policy);
        options.before_tool_call = &before_hook;
        agent::ToolCallExecutor executor{registry, std::move(options)};
        auto run = run_executor(executor, assistant);

        REQUIRE(run.result);
        for (const auto& result : run.result->results) {
            snapshot.result_errors.push_back(result.is_error);
            snapshot.result_text.push_back(ai::text_from_content(result.content));
        }
        for (const auto& event : run.events) {
            if (const auto* end = std::get_if<agent::MessageEndEvent>(&event)) {
                if (const auto* result = std::get_if<ai::ToolResultMessage>(&end->message)) {
                    snapshot.result_message_ids.push_back(result->tool_call_id);
                }
            }
        }
        for (const auto& invocation : tool_ptr->invocations()) {
            snapshot.invocation_values.push_back(invocation.arguments.at("value").get_number());
        }
        std::sort(snapshot.invocation_values.begin(), snapshot.invocation_values.end());
        snapshot.execution_starts = count_events<agent::ToolExecutionStartEvent>(run.events);
        snapshot.execution_ends = count_events<agent::ToolExecutionEndEvent>(run.events);
        snapshot.message_starts = count_events<agent::MessageStartEvent>(run.events);
        snapshot.message_ends = count_events<agent::MessageEndEvent>(run.events);
        snapshot.max_active = probe.max_active.load();
        return snapshot;
    };

    const auto sequential = execute_policy(agent::SequentialToolExecution{});
    const auto parallel = execute_policy(agent::BoundedParallelToolExecution{2});

    CHECK(parallel.result_errors == sequential.result_errors);
    CHECK(parallel.result_text == sequential.result_text);
    CHECK(parallel.before_hook_ids == sequential.before_hook_ids);
    CHECK(parallel.before_hook_arguments == sequential.before_hook_arguments);
    REQUIRE((parallel.before_hook_ids ==
             std::vector<std::string>{"call-1", "call-2", "call-3"}));
    REQUIRE((parallel.before_hook_arguments ==
             std::vector<std::string>{R"({"value":1})", R"({"value":2})", R"({"value":3})"}));
    REQUIRE((parallel.result_message_ids ==
             std::vector<std::string>{"call-invalid", "call-1", "call-2", "call-3"}));
    CHECK(parallel.result_message_ids == sequential.result_message_ids);
    REQUIRE((parallel.invocation_values == std::vector<double>{1, 2, 3}));
    CHECK(parallel.invocation_values == sequential.invocation_values);
    CHECK(parallel.execution_starts == 4);
    CHECK(parallel.execution_ends == 4);
    CHECK(parallel.message_starts == 4);
    CHECK(parallel.message_ends == 4);
    CHECK(sequential.execution_starts == 4);
    CHECK(sequential.execution_ends == 4);
    CHECK(sequential.message_starts == 4);
    CHECK(sequential.message_ends == 4);
    CHECK(sequential.max_active == 1);
    CHECK(parallel.max_active == 2);
}

TEST_CASE(
    "bounded parallel preparation completes immediate failures before the next source call",
    "[agent][tool-executor][tool-arguments][issue32]") {
    ConcurrencyProbe probe;
    agent::AsyncToolRegistry registry;

    auto add_tool = [&](std::string name,
                        util::JsonValue contract,
                        agent::ToolConcurrency concurrency = agent::ToolConcurrency::Exclusive,
                        std::string result = "ok",
                        std::chrono::milliseconds delay = std::chrono::milliseconds{}) {
        auto tool = std::make_unique<RecordingTool>(
            ai::Tool{name, name, std::move(contract)},
            concurrency,
            std::move(result),
            delay,
            &probe);
        auto* tool_ptr = tool.get();
        REQUIRE(registry.add(std::move(tool)));
        return tool_ptr;
    };

    auto* malformed = add_tool(
        "malformed", test::empty_object_tool_argument_contract());
    auto* invalid = add_tool(
        "invalid", test::integer_value_tool_argument_contract());
    auto* unsupported = add_tool(
        "unsupported",
        util::JsonValue::object_t{
            {"type", "array"},
            {"contains", util::JsonValue::object_t{{"type", "string"}}},
        });
    auto* blocked = add_tool(
        "blocked", test::empty_object_tool_argument_contract());
    auto* slow = add_tool(
        "slow",
        test::empty_object_tool_argument_contract(),
        agent::ToolConcurrency::ParallelSafe,
        "slow result",
        std::chrono::milliseconds{80});
    auto* fast = add_tool(
        "fast",
        test::empty_object_tool_argument_contract(),
        agent::ToolConcurrency::ParallelSafe,
        "fast result",
        std::chrono::milliseconds{10});

    std::vector<std::string> hook_ids;
    agent::BeforeToolCallHook before_hook =
        agent::adapt_sync_before_tool_call(
            [&](const agent::BeforeToolCallContext& context)
        -> util::Expected<agent::BeforeToolCallResult> {
        hook_ids.push_back(context.tool_call.id);
        if (context.tool_call.id == "call-blocked") {
            return agent::BeforeToolCallResult{true, "blocked by policy"};
        }
        return agent::BeforeToolCallResult{};
    });
    agent::ToolCallExecutorOptions options;
    options.execution = agent::BoundedParallelToolExecution{2};
    options.before_tool_call = &before_hook;
    agent::ToolCallExecutor executor{registry, std::move(options)};
    auto assistant = assistant_with_calls({
        make_call("call-unknown", "missing"),
        make_call("call-malformed", "malformed", "not-json"),
        make_call("call-invalid", "invalid", R"({"value":"no"})"),
        make_call("call-unsupported", "unsupported", R"([])"),
        make_call("call-blocked", "blocked"),
        make_call("call-slow", "slow"),
        make_call("call-fast", "fast"),
    });

    auto run = run_executor(executor, assistant);

    REQUIRE(run.result);
    REQUIRE(run.result->results.size() == 7);
    REQUIRE((hook_ids ==
             std::vector<std::string>{"call-blocked", "call-slow", "call-fast"}));
    CHECK(malformed->invocation_count() == 0);
    CHECK(invalid->invocation_count() == 0);
    CHECK(unsupported->invocation_count() == 0);
    CHECK(blocked->invocation_count() == 0);
    CHECK(slow->invocation_count() == 1);
    CHECK(fast->invocation_count() == 1);
    CHECK(probe.max_active.load() == 2);

    std::vector<std::string> sequence;
    for (const auto& event : run.events) {
        if (const auto* start = std::get_if<agent::ToolExecutionStartEvent>(&event)) {
            sequence.push_back("start:" + start->tool_call_id);
        } else if (const auto* end = std::get_if<agent::ToolExecutionEndEvent>(&event)) {
            sequence.push_back("end:" + end->tool_call_id);
        } else if (const auto* start = std::get_if<agent::MessageStartEvent>(&event)) {
            const auto* result = std::get_if<ai::ToolResultMessage>(&start->message);
            REQUIRE(result != nullptr);
            sequence.push_back("message-start:" + result->tool_call_id);
        } else if (const auto* end = std::get_if<agent::MessageEndEvent>(&event)) {
            const auto* result = std::get_if<ai::ToolResultMessage>(&end->message);
            REQUIRE(result != nullptr);
            sequence.push_back("message-end:" + result->tool_call_id);
        }
    }

    REQUIRE((sequence == std::vector<std::string>{
        "start:call-unknown",
        "end:call-unknown",
        "start:call-malformed",
        "end:call-malformed",
        "start:call-invalid",
        "end:call-invalid",
        "start:call-unsupported",
        "end:call-unsupported",
        "start:call-blocked",
        "end:call-blocked",
        "start:call-slow",
        "start:call-fast",
        "end:call-fast",
        "end:call-slow",
        "message-start:call-unknown",
        "message-end:call-unknown",
        "message-start:call-malformed",
        "message-end:call-malformed",
        "message-start:call-invalid",
        "message-end:call-invalid",
        "message-start:call-unsupported",
        "message-end:call-unsupported",
        "message-start:call-blocked",
        "message-end:call-blocked",
        "message-start:call-slow",
        "message-end:call-slow",
        "message-start:call-fast",
        "message-end:call-fast",
    }));
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

TEST_CASE(
    "bounded parallel execution serializes suspended hooks and lifecycle callbacks",
    "[agent][tool-executor][issue82]") {
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

    std::atomic<int> active_hooks{0};
    std::atomic<int> max_active_hooks{0};
    auto enter_hook = [&] {
        const int current = ++active_hooks;
        int observed = max_active_hooks.load();
        while (current > observed &&
               !max_active_hooks.compare_exchange_weak(observed, current)) {}
    };

    std::atomic<int> active_lifecycle_callbacks{0};
    std::atomic<int> max_active_lifecycle_callbacks{0};
    auto enter_lifecycle_callback = [&] {
        const int current = ++active_lifecycle_callbacks;
        int observed = max_active_lifecycle_callbacks.load();
        while (current > observed &&
               !max_active_lifecycle_callbacks.compare_exchange_weak(observed, current)) {}
        std::this_thread::sleep_for(std::chrono::milliseconds{15});
        --active_lifecycle_callbacks;
    };

    agent::AfterToolCallHook after_hook = [&](agent::AfterToolCallContext, std::stop_token)
        -> boost::asio::awaitable<util::Expected<agent::AfterToolCallResult>> {
        enter_hook();
        auto timer = boost::asio::steady_timer(
            co_await boost::asio::this_coro::executor,
            std::chrono::milliseconds{15});
        co_await timer.async_wait(boost::asio::use_awaitable);
        --active_hooks;
        co_return agent::AfterToolCallResult{};
    };
    agent::ToolCallExecutorOptions options;
    options.execution = agent::BoundedParallelToolExecution{2};
    options.after_tool_call = &after_hook;
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
                enter_lifecycle_callback();
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
    CHECK(max_active_hooks.load() == 1);
    CHECK(max_active_lifecycle_callbacks.load() == 1);
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

    agent::BeforeToolCallHook before_hook =
        agent::adapt_sync_before_tool_call(
            [](const agent::BeforeToolCallContext&)
        -> util::Expected<agent::BeforeToolCallResult> {
        return agent::BeforeToolCallResult{true, "blocked"};
    });
    agent::ToolCallExecutorOptions options;
    options.before_tool_call = &before_hook;
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

    agent::AfterToolCallHook after_hook =
        agent::adapt_sync_after_tool_call(
            [](const agent::AfterToolCallContext&)
        -> util::Expected<agent::AfterToolCallResult> {
        return agent::AfterToolCallResult{
            std::vector<ai::Content>{ai::text_content("overridden")},
            std::nullopt,
            std::nullopt,
            true};
    });
    agent::ToolCallExecutorOptions options;
    options.after_tool_call = &after_hook;
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
