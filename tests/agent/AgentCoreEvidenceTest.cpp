// pi-agent-core fixture evidence for #351 (T02): the committed goldens under
#include "support/AsyncResultBridge.hpp"
// fixtures/pi-agent-core/ compared byte-for-byte against the C++ Agent's
// observable behavior. Three evidence bundles:
//
//  1. streamSimple option-forwarding goldens (recording fake ModelRuntime)
//     proving the per-turn option set matches pi's harness consumer
//     (agent-harness.ts createStreamFn);
//  2. loop lifecycle ordering goldens covering agent_start → turn_start →
//     message_start/update/end → tool_execution_* → turn_end → agent_end,
//     error/aborted terminals, steer/follow-up drain points,
//     prepare-next-turn, and continuation;
//  3. the six-category terminal matrix lives in ModelRuntimeSeamTest (each
//     category produces exactly one terminal event plus a final
//     AssistantMessage); this file pins the option and lifecycle goldens.
//
// Sanitization follows the fixtures/pi-ai/ rules: no live credentials, all
// fixture strings are distinguishable dummy values.

#include <cch/agent/Agent.hpp>
#include <cch/ai/Content.hpp>
#include "agent/ToolCallExecutor.hpp"
#include "ai/SimpleOptions.hpp"
#include "support/FakeModelStream.hpp"
#include "support/FakeTool.hpp"
#include "support/ModelFixture.hpp"
#include "support/ToolArgumentContracts.hpp"
#include "support/ExpectedMacros.hpp"
#include "support/Json.hpp"

#include <catch2/catch_test_macros.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using namespace cch;

namespace {

[[nodiscard]] std::string read_fixture_text(std::string_view name) {
    const std::string path = std::string{CCH_SOURCE_DIR} +
                             "/fixtures/pi-agent-core/" +
                             std::string{name};
    std::ifstream input(path, std::ios::binary);
    return std::string{
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{}};
}

void expect_json_equal(
    const support::JsonValue& actual,
    std::string_view fixture_name) {
    auto serialized = support::write_json(actual);
    REQUIRE(serialized);
    const auto expected = read_fixture_text(fixture_name);
    if (*serialized != expected) {
        std::cerr << "\n[AgentCoreEvidenceTest] fixture mismatch: "
                  << fixture_name << "\n--- expected ---\n"
                  << expected << "\n--- actual ---\n"
                  << *serialized << "\n--- end ---\n";
    }
    CHECK(*serialized == expected);
}

[[nodiscard]] std::string thinking_level_name(ai::ThinkingLevel level) {
    switch (level) {
    case ai::ThinkingLevel::Minimal:
        return "minimal";
    case ai::ThinkingLevel::Low:
        return "low";
    case ai::ThinkingLevel::Medium:
        return "medium";
    case ai::ThinkingLevel::High:
        return "high";
    case ai::ThinkingLevel::XHigh:
        return "xhigh";
    case ai::ThinkingLevel::Max:
        return "max";
    }
    return {};
}

[[nodiscard]] std::string cache_retention_name(ai::CacheRetention retention) {
    switch (retention) {
    case ai::CacheRetention::None:
        return "none";
    case ai::CacheRetention::Short:
        return "short";
    case ai::CacheRetention::Long:
        return "long";
    }
    return {};
}

/// The canonical per-turn option record, named exactly like pi's
/// harness-consumer option set (agent-harness.ts createStreamFn): reasoning,
/// sessionId, cacheRetention (resolved, matching what the provider dispatch
/// receives), timeoutMs, maxRetries, maxRetryDelayMs, headers, signal.
[[nodiscard]] support::JsonValue options_to_json(
    const ai::SimpleStreamOptions& options,
    const ai::Model& model) {
    support::JsonValue object{support::JsonValue::object_t{}};
    auto& o = object.get_object();

    support::JsonValue model_object{support::JsonValue::object_t{}};
    model_object.get_object().emplace("id", support::JsonValue(model.id));
    model_object.get_object().emplace(
        "provider", support::JsonValue(model.provider));
    model_object.get_object().emplace("api", support::JsonValue(model.api));
    o.emplace("model", std::move(model_object));

    if (options.reasoning) {
        o.emplace(
            "reasoning",
            support::JsonValue(thinking_level_name(*options.reasoning)));
    } else {
        o.emplace("reasoning", support::JsonValue(nullptr));
    }
    if (options.session_id) {
        o.emplace("sessionId", support::JsonValue(*options.session_id));
    } else {
        o.emplace("sessionId", support::JsonValue(nullptr));
    }
    o.emplace(
        "cacheRetention",
        support::JsonValue(cache_retention_name(
            ai::detail::resolve_cache_retention(
                options.cache_retention, options.env))));
    if (options.timeout_ms) {
        o.emplace(
            "timeoutMs",
            support::JsonValue(static_cast<int>(*options.timeout_ms)));
    } else {
        o.emplace("timeoutMs", support::JsonValue(nullptr));
    }
    o.emplace("maxRetries", support::JsonValue(static_cast<int>(options.max_retries)));
    if (options.max_retry_delay_ms) {
        o.emplace(
            "maxRetryDelayMs",
            support::JsonValue(static_cast<int>(*options.max_retry_delay_ms)));
    } else {
        o.emplace("maxRetryDelayMs", support::JsonValue(nullptr));
    }
    support::JsonValue headers{support::JsonValue::object_t{}};
    for (const auto& [name, value] : options.headers) {
        if (value) {
            headers.get_object().emplace(name, support::JsonValue(*value));
        } else {
            headers.get_object().emplace(name, support::JsonValue(nullptr));
        }
    }
    o.emplace("headers", std::move(headers));
    o.emplace(
        "signal",
        support::JsonValue(options.stop_token != std::stop_token{}));
    return object;
}

[[nodiscard]] std::string message_role(const ai::MessageVariant& message) {
    if (std::holds_alternative<ai::UserMessage>(message)) {
        return "user";
    }
    if (std::holds_alternative<ai::AssistantMessage>(message)) {
        return "assistant";
    }
    if (std::holds_alternative<ai::ToolResultMessage>(message)) {
        return "tool_result";
    }
    return "other";
}

[[nodiscard]] std::string stream_event_name(
    const ai::AssistantStreamEvent& event) {
    if (std::holds_alternative<ai::AssistantStartEvent>(event)) {
        return "start";
    }
    if (std::holds_alternative<ai::TextStartEvent>(event)) {
        return "text_start";
    }
    if (std::holds_alternative<ai::TextDeltaEvent>(event)) {
        return "text_delta";
    }
    if (std::holds_alternative<ai::TextEndEvent>(event)) {
        return "text_end";
    }
    if (std::holds_alternative<ai::ThinkingStartEvent>(event)) {
        return "thinking_start";
    }
    if (std::holds_alternative<ai::ThinkingDeltaEvent>(event)) {
        return "thinking_delta";
    }
    if (std::holds_alternative<ai::ThinkingEndEvent>(event)) {
        return "thinking_end";
    }
    if (std::holds_alternative<ai::ToolCallStartEvent>(event)) {
        return "toolcall_start";
    }
    if (std::holds_alternative<ai::ToolCallDeltaEvent>(event)) {
        return "toolcall_delta";
    }
    if (std::holds_alternative<ai::ToolCallEndEvent>(event)) {
        return "toolcall_end";
    }
    if (std::holds_alternative<ai::AssistantDoneEvent>(event)) {
        return "done";
    }
    if (std::holds_alternative<ai::AssistantErrorEvent>(event)) {
        return "error";
    }
    return "unknown";
}

/// One canonical lifecycle event record. Turn numbers are derived by counting
/// turn_start events, matching the loop's per-turn counter.
struct LifecycleSerializer {
    int turn{0};

    [[nodiscard]] support::JsonValue event_to_json(
        const agent::AgentLifecycleEvent& event) {
        support::JsonValue object{support::JsonValue::object_t{}};
        auto& o = object.get_object();
        if (std::holds_alternative<agent::AgentStartEvent>(event)) {
            o.emplace("type", support::JsonValue("agent_start"));
        } else if (const auto* end =
                       std::get_if<agent::AgentEndEvent>(&event)) {
            o.emplace("type", support::JsonValue("agent_end"));
            o.emplace(
                "messageCount",
                support::JsonValue(static_cast<int>(end->messages.size())));
        } else if (std::holds_alternative<agent::TurnStartEvent>(event)) {
            ++turn;
            o.emplace("type", support::JsonValue("turn_start"));
            o.emplace("turn", support::JsonValue(turn));
        } else if (const auto* end =
                       std::get_if<agent::TurnEndEvent>(&event)) {
            o.emplace("type", support::JsonValue("turn_end"));
            const auto* assistant =
                std::get_if<ai::AssistantMessage>(&end->message);
            REQUIRE(assistant != nullptr);
            o.emplace(
                "stopReason",
                support::JsonValue(ai::stop_reason_to_string(
                    assistant->stop_reason)));
            o.emplace(
                "toolResults",
                support::JsonValue(static_cast<int>(end->tool_results.size())));
        } else if (const auto* start =
                       std::get_if<agent::MessageStartEvent>(&event)) {
            o.emplace("type", support::JsonValue("message_start"));
            o.emplace("role", support::JsonValue(message_role(start->message)));
        } else if (const auto* update =
                       std::get_if<agent::MessageUpdateEvent>(&event)) {
            o.emplace("type", support::JsonValue("message_update"));
            o.emplace(
                "event",
                support::JsonValue(stream_event_name(update->assistant_event)));
        } else if (const auto* end =
                       std::get_if<agent::MessageEndEvent>(&event)) {
            o.emplace("type", support::JsonValue("message_end"));
            o.emplace("role", support::JsonValue(message_role(end->message)));
        } else if (const auto* start =
                       std::get_if<agent::ToolExecutionStartEvent>(&event)) {
            o.emplace("type", support::JsonValue("tool_execution_start"));
            o.emplace("toolCallId", support::JsonValue(start->tool_call_id));
            o.emplace("toolName", support::JsonValue(start->tool_name));
        } else if (const auto* update =
                       std::get_if<agent::ToolExecutionUpdateEvent>(&event)) {
            o.emplace("type", support::JsonValue("tool_execution_update"));
            o.emplace("toolCallId", support::JsonValue(update->tool_call_id));
            o.emplace("toolName", support::JsonValue(update->tool_name));
        } else if (const auto* end =
                       std::get_if<agent::ToolExecutionEndEvent>(&event)) {
            o.emplace("type", support::JsonValue("tool_execution_end"));
            o.emplace("toolCallId", support::JsonValue(end->tool_call_id));
            o.emplace("toolName", support::JsonValue(end->tool_name));
            o.emplace("isError", support::JsonValue(end->is_error));
        }
        return object;
    }
};

support::ExpectedVoid run_prompt(
    agent::Agent& subject,
    std::string prompt) {
    boost::asio::io_context io;
    std::optional<support::ExpectedVoid> result;
    boost::asio::co_spawn(
            io,
            [&]() -> boost::asio::awaitable<void> {
                result = co_await support::detail::await_async_result(subject.prompt(std::move(prompt)));
                co_return;
            },
            boost::asio::detached);
    io.run();
    if (!result) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Unknown,
            "agent prompt coroutine did not complete"));
    }
    return std::move(*result);
}

struct AgentRun {
    support::ExpectedVoid result;
    std::vector<agent::AgentLifecycleEvent> events;
    agent::AgentState state;
};

AgentRun run_agent(agent::Agent& subject, std::string prompt) {
    boost::asio::io_context io;
    std::optional<support::ExpectedVoid> result;
    std::vector<agent::AgentLifecycleEvent> events;
    auto subscribed = subject.subscribe([&events](const agent::AgentLifecycleEvent& event) {
        events.push_back(event);
        return support::ExpectedVoid{};
    });
    REQUIRE(subscribed);
    auto subscription = std::move(*subscribed);

    boost::asio::co_spawn(
            io,
            [&]() -> boost::asio::awaitable<void> {
                result = co_await support::detail::await_async_result(subject.prompt(std::move(prompt)));
                co_return;
            },
            boost::asio::detached);
    io.run();
    REQUIRE(result.has_value());
    return AgentRun{
            .result = std::move(*result),
            .events = std::move(events),
            .state = subject.state(),
    };
}

[[nodiscard]] ai::AssistantStopReason final_stop_reason(const std::vector<agent::AgentLifecycleEvent>& events) {
    for (auto it = events.rbegin(); it != events.rend(); ++it) {
        if (const auto* turn_end = std::get_if<agent::TurnEndEvent>(&*it)) {
            return std::get<ai::AssistantMessage>(turn_end->message).stop_reason;
        }
    }
    return ai::AssistantStopReason::Stop;
}

template <typename T> std::size_t count_events(const std::vector<agent::AgentLifecycleEvent>& events) {
    std::size_t count = 0;
    for (const auto& event : events) {
        if (std::holds_alternative<T>(event)) {
            ++count;
        }
    }
    return count;
}

[[nodiscard]] support::JsonValue lifecycle_events_to_json(
    const std::vector<agent::AgentLifecycleEvent>& events) {
    LifecycleSerializer serializer;
    support::JsonValue array{support::JsonValue::array_t{}};
    for (const auto& event : events) {
        array.get_array().push_back(serializer.event_to_json(event));
    }
    return array;
}

[[nodiscard]] support::JsonValue stream_calls_to_json(
    const std::vector<tests::RecordedStreamSimpleCall>& calls) {
    support::JsonValue array{support::JsonValue::array_t{}};
    for (std::size_t index = 0; index < calls.size(); ++index) {
        const auto& call = calls[index];
        auto object = options_to_json(call.options, call.model);
        object.get_object().emplace(
            "turn", support::JsonValue(static_cast<int>(index + 1)));
        array.get_array().push_back(std::move(object));
    }
    return array;
}

[[nodiscard]] agent::Tool make_read_tool() {
    return tests::make_fake_tool(
        ai::Tool{"read", "Read a file", test::permissive_object_tool_argument_contract()},
        agent::ToolConcurrency::Exclusive,
        [](agent::ToolInvocation, std::stop_token, agent::ToolUpdateSink)
            -> boost::asio::awaitable<support::Expected<agent::AsyncToolExecutionResult>> {
            agent::AsyncToolExecutionResult result;
            result.content.push_back(ai::text_content("tool says ok"));
            co_return result;
        });
}

[[nodiscard]] ai::AssistantMessage read_tool_call_message() {
    auto args = support::read_json(R"({"path":"README.md"})");
    REQUIRE(args.has_value());
    ai::AssistantMessage message;
    message.stop_reason = ai::AssistantStopReason::ToolUse;
    message.content.push_back(ai::tool_call_content(
        "call-1", "read", R"({"path":"README.md"})", *args));
    return message;
}

} // namespace

TEST_CASE(
    "streamSimple option-forwarding golden matches pi's harness consumer (full set)",
    "[agent][fixture][issue351]") {
    auto runtime = std::make_shared<tests::FakeModelStream>();
    runtime->responses.push_back(ai::assistant_text_message("hello user"));

    agent::AsyncAgentOptions options;
    options.max_turns = 3;
    // Full-map reasoning model: the requested "high" survives creation-time
    // clamping, so the golden's reasoning field proves option forwarding rather
    // than clamp fallback (#352).
    options.model = tests::make_full_thinking_model("gpt-test");
    options.session_id = "session-golden";
    options.cache_retention = ai::CacheRetention::Short;
    options.timeout_ms = 60000;
    options.max_retries = 2;
    options.max_retry_delay_ms = 120000;
    options.headers = {
        {std::string{"x-golden"}, std::string{"value"}},
    };
    agent::Agent subject(
        runtime->factory(),
        agent::ToolRegistry{},
        std::move(options),
        agent::AgentInitialState{
            .messages = {},
            .thinking_level = "high",
        });


    REQUIRE(run_prompt(subject, "hi"));

    REQUIRE(runtime->calls.size() == 1);
    const auto& call = runtime->calls[0];
    expect_json_equal(
        options_to_json(call.options, call.model),
        "stream-simple-options.json");
}

TEST_CASE(
    "streamSimple option-forwarding golden matches pi's harness consumer (default turn)",
    "[agent][fixture][issue351]") {
    auto runtime = std::make_shared<tests::FakeModelStream>();
    runtime->responses.push_back(ai::assistant_text_message("hello user"));

    // Every knob at its default: the unset level requested pi's
    // DEFAULT_THINKING_LEVEL ("medium") and was clamped at creation to the
    // non-reasoning model's only supported level ("off", no reasoning), no
    // session id, no timeout/retries/headers. Only the active prompt signal is
    // forwarded, and cacheRetention resolves to the pi "short" default.
    agent::AsyncAgentOptions options;
    options.max_turns = 3;
    options.model = tests::make_model("gpt-test");
    agent::Agent subject(runtime->factory(), agent::ToolRegistry{}, std::move(options));

    REQUIRE(run_prompt(subject, "hi"));

    REQUIRE(runtime->calls.size() == 1);
    const auto& call = runtime->calls[0];
    expect_json_equal(
        options_to_json(call.options, call.model),
        "stream-simple-options-default.json");
}

TEST_CASE(
    "loop lifecycle ordering golden covers the pi turn lifecycle end to end",
    "[agent][fixture][issue351]") {
    auto runtime = std::make_shared<tests::FakeModelStream>();
    runtime->responses.push_back(read_tool_call_message());
    runtime->responses.push_back(ai::assistant_text_message("after tool"));
    runtime->responses.push_back(ai::assistant_text_message("after follow-up"));

    agent::ToolRegistry tools;
    REQUIRE(tools.add(make_read_tool()));

    int prepare_calls = 0;
    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    // Full-map reasoning model so the requested "medium" (and the "high"
    // prepare-next-turn flip) survives clamping; the golden proves per-turn
    // reasoning forwarding on the wire, not clamp fallback (#352).
    options.model = tests::make_full_thinking_model("gpt-test");
    // prepare-next-turn runs after turn_end: its first update flips the
    // thinking level from medium to high, so the recorded per-turn reasoning
    // for calls 2/3 proves the update landed before the next stream call.
    options.prepare_next_turn = [&prepare_calls](const agent::PrepareNextTurnContext&)
            -> support::AsyncResult<std::optional<agent::AgentLoopTurnUpdate>> {
        ++prepare_calls;
        if (prepare_calls == 1) {
            return support::AsyncResult<std::optional<agent::AgentLoopTurnUpdate>>{
                    support::Expected<std::optional<agent::AgentLoopTurnUpdate>>{agent::AgentLoopTurnUpdate{
                            .thinking_level = std::string{"high"},
                    }}};
        }
        return support::AsyncResult<std::optional<agent::AgentLoopTurnUpdate>>{
                support::Expected<std::optional<agent::AgentLoopTurnUpdate>>{std::nullopt}};
    };

    agent::Agent subject(
        runtime->factory(),
        std::move(tools),
        std::move(options),
        agent::AgentInitialState{
            .messages = {},
            .thinking_level = "medium",
        });


    // A follow-up queued up front drains only after the last tool-call turn
    // completes; a steering message steered from the turn_end(1) observer
    // drains at the end of turn 1 and injects before the turn-2 request.
    REQUIRE(subject.follow_up(ai::user_text_message("follow me")));

    std::vector<agent::AgentLifecycleEvent> events;
    bool steered = false;
    auto subscription =
        subject.subscribe(
            [&](const agent::AgentLifecycleEvent& event)
                -> support::ExpectedVoid {
                events.push_back(event);
                if (!steered &&
                    std::holds_alternative<agent::TurnEndEvent>(event)) {
                    steered = true;
                    return subject.steer(ai::user_text_message("steer me"));
                }
                return support::ExpectedVoid{};
            });
    REQUIRE(subscription);
    REQUIRE(run_prompt(subject, "read the file"));

    REQUIRE(steered);
    REQUIRE(runtime->calls.size() == 3);

    support::JsonValue golden{support::JsonValue::object_t{}};
    golden.get_object().emplace("events", lifecycle_events_to_json(events));
    golden.get_object().emplace(
        "streamCalls", stream_calls_to_json(runtime->calls));
    expect_json_equal(golden, "loop-lifecycle.json");
}

TEST_CASE(
    "loop lifecycle ordering golden covers the error terminal",
    "[agent][fixture][terminal][issue351]") {
    auto runtime = std::make_shared<tests::FakeModelStream>();
    auto terminal = ai::assistant_text_message("");
    terminal.stop_reason = ai::AssistantStopReason::Error;
    terminal.error_message = "provider request failed";
    runtime->responses.push_back(std::move(terminal));

    agent::AsyncAgentOptions options;
    options.max_turns = 3;
    options.model = tests::make_model("gpt-test");
    agent::Agent subject(runtime->factory(), agent::ToolRegistry{}, std::move(options));

    auto run = run_agent(subject, "hi");

    CHECK(run.result.has_value());
    CHECK(final_stop_reason(run.events) == ai::AssistantStopReason::Error);
    CHECK(runtime->terminal_events == 1);
    expect_json_equal(
        lifecycle_events_to_json(run.events), "loop-terminal-error.json");
}

TEST_CASE(
    "loop lifecycle ordering golden covers the aborted terminal",
    "[agent][fixture][terminal][abort][issue351]") {
    auto runtime = std::make_shared<tests::FakeModelStream>();
    runtime->responses.push_back(ai::assistant_text_message("unused"));

    agent::AsyncAgentOptions options;
    options.max_turns = 3;
    options.model = tests::make_model("gpt-test");
    agent::Agent subject(runtime->factory(), agent::ToolRegistry{}, std::move(options));

    boost::asio::io_context io;
    std::optional<support::ExpectedVoid> result;
    std::vector<agent::AgentLifecycleEvent> events;
    auto subscribed = subject.subscribe([&events](const agent::AgentLifecycleEvent& event) {
        events.push_back(event);
        return support::ExpectedVoid{};
    });
    REQUIRE(subscribed);
    auto subscription = std::move(*subscribed);
    boost::asio::co_spawn(
            io,
            [&]() -> boost::asio::awaitable<void> {
                result = co_await support::detail::await_async_result(subject.prompt("cancel me"));
                co_return;
            },
            boost::asio::detached);
    while (runtime->calls.empty()) {
        REQUIRE(io.run_one() == 1);
    }
    subject.abort();
    io.run();

    REQUIRE(result.has_value());
    REQUIRE(*result);
    CHECK(final_stop_reason(events) == ai::AssistantStopReason::Aborted);
    CHECK(runtime->terminal_events == 1);
    expect_json_equal(lifecycle_events_to_json(events), "loop-terminal-aborted.json");
}

TEST_CASE(
    "thinking-level clamp golden covers creation and model-switch clamping",
    "[agent][fixture][issue352]") {
    auto runtime = std::make_shared<tests::FakeModelStream>();
    runtime->responses.push_back(ai::assistant_text_message("first reply"));
    runtime->responses.push_back(ai::assistant_text_message("second reply"));

    // Partial map (off/low/high/xhigh mapped, "max" absent): the supported
    // set is off..xhigh, so the requested "max" clamps to "xhigh" at
    // creation. The prepare-next-turn hook then switches to a non-reasoning
    // model, which must re-clamp "xhigh" to "off" so turn 2 forwards no
    // reasoning — an unsupported level never reaches the wire (#352).
    const ai::ThinkingLevelMap partial_map{
        {ai::ModelThinkingLevel::Off, "off"},
        {ai::ModelThinkingLevel::Low, "low"},
        {ai::ModelThinkingLevel::High, "high"},
        {ai::ModelThinkingLevel::XHigh, "xhigh"},
    };
    agent::AsyncAgentOptions options;
    options.max_turns = 3;
    options.model = tests::make_reasoning_model("gpt-partial", partial_map);
    options.prepare_next_turn = [](const agent::PrepareNextTurnContext&)
            -> support::AsyncResult<std::optional<agent::AgentLoopTurnUpdate>> {
        return support::AsyncResult<std::optional<agent::AgentLoopTurnUpdate>>{
                support::Expected<std::optional<agent::AgentLoopTurnUpdate>>{agent::AgentLoopTurnUpdate{
                        .model = tests::make_model("gpt-basic"),
                }}};
    };
    options.validate_turn_update = [](const agent::AgentLoopTurnUpdate&) -> support::AsyncResult<void> {
        return support::AsyncResult<void>{support::ExpectedVoid{}};
    };

    agent::Agent subject(
        runtime->factory(),
        agent::ToolRegistry{},
        std::move(options),
        agent::AgentInitialState{
            .messages = {},
            .thinking_level = "max",
        });


    REQUIRE(run_prompt(subject, "think hard"));
    REQUIRE(run_prompt(subject, "switch model"));

    REQUIRE(runtime->calls.size() == 2);
    expect_json_equal(
        stream_calls_to_json(runtime->calls),
        "thinking-level-clamp.json");
    // The wire-level facts the golden pins, asserted directly as well:
    CHECK(runtime->calls[0].options.reasoning == ai::ThinkingLevel::XHigh);
    CHECK(runtime->calls[1].options.reasoning == std::nullopt);
    CHECK(subject.state().thinking_level == "off");
}

namespace {

/// A deterministic fake tool whose result carries content plus optional
/// details, so the committed tool-result shape golden covers both.
[[nodiscard]] agent::Tool make_details_tool(
    std::string name,
    std::string result_text,
    support::JsonValue contract) {
    return tests::make_fake_tool(
        ai::Tool{std::move(name), "Fake tool", std::move(contract)},
        agent::ToolConcurrency::Exclusive,
        [result_text = std::move(result_text)](
            agent::ToolInvocation, std::stop_token, agent::ToolUpdateSink)
            -> boost::asio::awaitable<support::Expected<agent::AsyncToolExecutionResult>> {
            agent::AsyncToolExecutionResult result;
            result.content.push_back(ai::text_content(result_text));
            result.details = support::JsonValue::object_t{
                {"note", "golden-details"},
            };
            co_return result;
        });
}

/// Serialize one tool result into pi's `ToolResultMessage` wire shape:
/// role, toolCallId, toolName, content, details (omitted when absent),
/// isError, timestamp.
[[nodiscard]] support::JsonValue tool_result_to_json(
    const ai::ToolResultMessage& result) {
    support::JsonValue object{support::JsonValue::object_t{}};
    auto& o = object.get_object();
    o.emplace("role", support::JsonValue("toolResult"));
    o.emplace("toolCallId", support::JsonValue(result.tool_call_id));
    o.emplace("toolName", support::JsonValue(result.tool_name));
    support::JsonValue content{support::JsonValue::array_t{}};
    for (const auto& block : result.content) {
        if (const auto* text = std::get_if<ai::TextContent>(&block)) {
            support::JsonValue text_block{support::JsonValue::object_t{}};
            text_block.get_object().emplace("type", support::JsonValue("text"));
            text_block.get_object().emplace("text", support::JsonValue(text->text));
            content.get_array().push_back(std::move(text_block));
        }
    }
    o.emplace("content", std::move(content));
    if (result.details) {
        o.emplace("details", *result.details);
    }
    o.emplace("isError", support::JsonValue(result.is_error));
    o.emplace("timestamp", support::JsonValue(static_cast<int>(result.timestamp)));
    return object;
}

} // namespace

TEST_CASE(
    "tool-result shape golden groups is_error exactly as pi's toolResult message",
    "[agent][fixture][tool-result][issue354]") {
    // One assistant message carrying three tool calls: a success, a call whose
    // arguments fail JSON Schema validation, and a sibling success. The batch
    // result groups each call's ToolResultMessage with is_error, and the
    // failing call never takes down its siblings (ADR 0008).
    const auto contract = support::JsonValue::object_t{
        {"type", "object"},
        {"properties", support::JsonValue::object_t{
            {"path", support::JsonValue::object_t{{"type", "string"}}},
        }},
        {"required", support::JsonValue::array_t{"path"}},
        {"additionalProperties", false},
    };

    agent::ToolRegistry tools;
    REQUIRE(tools.add(make_details_tool(
        "read", "tool says ok", contract)));
    REQUIRE(tools.add(make_details_tool(
        "write", "wrote 5 bytes", contract)));

    agent::ToolCallExecutor executor{tools, agent::ToolCallExecutorOptions{}};
    auto calls = {
        ai::tool_call_content("call-ok", "read", R"({"path":"a.txt"})",
            *support::read_json(R"({"path":"a.txt"})")),
        ai::tool_call_content("call-bad", "read", R"({"path":{}})",
            *support::read_json(R"({"path":{}})")),
        ai::tool_call_content("call-sibling", "write", R"({"path":"b.txt"})",
            *support::read_json(R"({"path":"b.txt"})")),
    };
    ai::AssistantMessage assistant;
    assistant.stop_reason = ai::AssistantStopReason::ToolUse;
    for (const auto& call : calls) {
        assistant.content.emplace_back(call);
    }

    boost::asio::io_context io;
    std::optional<support::Expected<agent::ToolCallBatchResult>> result;
    boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<void> {
            agent::AgentEventSink sink{
                [](const agent::AgentLifecycleEvent&) { return support::ExpectedVoid{}; }};
            result = co_await executor.execute(
                agent::ToolCallBatchRequest{assistant, {}},
                sink);
            co_return;
        },
        boost::asio::detached);
    io.run();
    REQUIRE(result.has_value());
    REQUIRE(result->has_value());
    const auto& batch = **result;
    REQUIRE(batch.results.size() == 3);

    support::JsonValue golden{support::JsonValue::object_t{}};
    support::JsonValue results{support::JsonValue::array_t{}};
    for (const auto& tool_result : batch.results) {
        results.get_array().push_back(tool_result_to_json(tool_result));
    }
    golden.get_object().emplace("toolResults", std::move(results));
    expect_json_equal(golden, "tool-result-shape.json");

    // The committed golden also pins the isolation fact: the middle call is
    // an error while both siblings completed normally.
    CHECK(batch.results[0].is_error == false);
    CHECK(batch.results[1].is_error == true);
    CHECK(batch.results[2].is_error == false);
}

namespace {

/// Deterministic scheduled fake tool: declares pi-style concurrency and sleeps
/// on the loop executor so parallel/sequential interleavings are reproducible.
[[nodiscard]] agent::Tool make_delayed_evidence_tool(
    std::string name,
    agent::ToolConcurrency concurrency,
    std::chrono::milliseconds delay = {},
    std::string result_text = "tool result") {
    return tests::make_fake_tool(
        ai::Tool{
            std::move(name),
            "Scheduled fake tool",
            test::permissive_object_tool_argument_contract()},
        concurrency,
        [delay, result_text = std::move(result_text)](
            agent::ToolInvocation, std::stop_token, agent::ToolUpdateSink)
            -> boost::asio::awaitable<support::Expected<agent::AsyncToolExecutionResult>> {
            if (delay.count() > 0) {
                auto timer = boost::asio::steady_timer(
                    co_await boost::asio::this_coro::executor,
                    delay);
                co_await timer.async_wait(boost::asio::use_awaitable);
            }
            co_return agent::AsyncToolExecutionResult{
                .content = std::vector<ai::Content>{ai::text_content(result_text)},
                .details = std::nullopt,
                .is_error = false};
        });
}

/// Serializer for the tool-scheduling golden: richer than the lifecycle
/// serializer because ordering evidence needs per-call ids and argument
/// payloads, which the shared lifecycle golden intentionally omits.
struct SchedulingSerializer {
    int turn{0};

    [[nodiscard]] support::JsonValue event_to_json(
        const agent::AgentLifecycleEvent& event) {
        support::JsonValue object{support::JsonValue::object_t{}};
        auto& o = object.get_object();
        if (std::holds_alternative<agent::AgentStartEvent>(event)) {
            o.emplace("type", support::JsonValue("agent_start"));
        } else if (const auto* end =
                       std::get_if<agent::AgentEndEvent>(&event)) {
            o.emplace("type", support::JsonValue("agent_end"));
            o.emplace(
                "messageCount",
                support::JsonValue(static_cast<int>(end->messages.size())));
        } else if (std::holds_alternative<agent::TurnStartEvent>(event)) {
            ++turn;
            o.emplace("type", support::JsonValue("turn_start"));
            o.emplace("turn", support::JsonValue(turn));
        } else if (const auto* end =
                       std::get_if<agent::TurnEndEvent>(&event)) {
            o.emplace("type", support::JsonValue("turn_end"));
            const auto* assistant =
                std::get_if<ai::AssistantMessage>(&end->message);
            REQUIRE(assistant != nullptr);
            o.emplace(
                "stopReason",
                support::JsonValue(ai::stop_reason_to_string(
                    assistant->stop_reason)));
            o.emplace(
                "toolResults",
                support::JsonValue(static_cast<int>(end->tool_results.size())));
        } else if (const auto* start =
                       std::get_if<agent::MessageStartEvent>(&event)) {
            o.emplace("type", support::JsonValue("message_start"));
            o.emplace("role", support::JsonValue(message_role(start->message)));
            if (const auto* result =
                    std::get_if<ai::ToolResultMessage>(&start->message)) {
                o.emplace("toolCallId", support::JsonValue(result->tool_call_id));
            }
        } else if (const auto* update =
                       std::get_if<agent::MessageUpdateEvent>(&event)) {
            o.emplace("type", support::JsonValue("message_update"));
            o.emplace(
                "event",
                support::JsonValue(stream_event_name(update->assistant_event)));
        } else if (const auto* end =
                       std::get_if<agent::MessageEndEvent>(&event)) {
            o.emplace("type", support::JsonValue("message_end"));
            o.emplace("role", support::JsonValue(message_role(end->message)));
            if (const auto* result =
                    std::get_if<ai::ToolResultMessage>(&end->message)) {
                o.emplace("toolCallId", support::JsonValue(result->tool_call_id));
            }
        } else if (const auto* start =
                       std::get_if<agent::ToolExecutionStartEvent>(&event)) {
            o.emplace("type", support::JsonValue("tool_execution_start"));
            o.emplace("toolCallId", support::JsonValue(start->tool_call_id));
            o.emplace("toolName", support::JsonValue(start->tool_name));
            o.emplace("args", start->args);
        } else if (const auto* end =
                       std::get_if<agent::ToolExecutionEndEvent>(&event)) {
            o.emplace("type", support::JsonValue("tool_execution_end"));
            o.emplace("toolCallId", support::JsonValue(end->tool_call_id));
            o.emplace("toolName", support::JsonValue(end->tool_name));
            o.emplace("isError", support::JsonValue(end->is_error));
        }
        return object;
    }
};

[[nodiscard]] support::JsonValue scheduling_events_to_json(
    const std::vector<agent::AgentLifecycleEvent>& events) {
    SchedulingSerializer serializer;
    support::JsonValue array{support::JsonValue::array_t{}};
    for (const auto& event : events) {
        array.get_array().push_back(serializer.event_to_json(event));
    }
    return array;
}

[[nodiscard]] ai::AssistantMessage tool_call_message(
    std::initializer_list<std::pair<std::string, std::string>> calls,
    ai::AssistantStopReason stop_reason = ai::AssistantStopReason::ToolUse) {
    ai::AssistantMessage message;
    message.stop_reason = stop_reason;
    for (const auto& [id, name] : calls) {
        (void)name;
        message.content.push_back(ai::tool_call_content(
            id, name, R"({"x":1})", support::JsonValue::object_t{{{"x", 1}}}));
    }
    return message;
}

} // namespace

TEST_CASE(
    "tool scheduling golden covers parallel default, sequential override, truncated fail-all, and terminate hint",
    "[agent][fixture][issue355]") {
    auto runtime = std::make_shared<tests::FakeModelStream>();
    runtime->responses.push_back(tool_call_message({
        {"call-1", "alpha"},
        {"call-2", "beta"},
    }));
    auto truncated = tool_call_message(
        {
            {"call-3", "gamma"},
            {"call-4", "delta"},
        },
        ai::AssistantStopReason::Length);
    runtime->responses.push_back(std::move(truncated));
    runtime->responses.push_back(tool_call_message({
        {"call-5", "gamma"},
        {"call-6", "delta"},
    }));

    agent::ToolRegistry tools;
    REQUIRE(tools.add(make_delayed_evidence_tool(
        "alpha", agent::ToolConcurrency::ParallelSafe, std::chrono::milliseconds{20}, "alpha result")));
    REQUIRE(tools.add(make_delayed_evidence_tool(
        "beta", agent::ToolConcurrency::ParallelSafe, std::chrono::milliseconds{5}, "beta result")));
    REQUIRE(tools.add(make_delayed_evidence_tool(
        "gamma", agent::ToolConcurrency::Exclusive, std::chrono::milliseconds{}, "gamma result")));
    REQUIRE(tools.add(make_delayed_evidence_tool(
        "delta", agent::ToolConcurrency::ParallelSafe, std::chrono::milliseconds{}, "delta result")));

    // The terminate hint applies only to the gamma/delta batch in turn 3, so
    // turns 1 and 2 continue and the all-true hint ends the loop.
    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    options.after_tool_call = [](const agent::AfterToolCallContext& context,
                                      std::stop_token) -> support::AsyncResult<agent::AfterToolCallResult> {
        const bool terminate =
            context.tool_call.name == "gamma" ||
            context.tool_call.name == "delta";
        return support::AsyncResult<agent::AfterToolCallResult>{
                support::Expected<agent::AfterToolCallResult>{agent::AfterToolCallResult{
                        .content = std::nullopt,
                        .details = std::nullopt,
                        .is_error = std::nullopt,
                        .terminate = terminate,
                }}};
    };

    agent::Agent subject(runtime->factory(), std::move(tools), std::move(options));
    auto run = run_agent(subject, "schedule tools");

    CHECK(run.result.has_value());
    CHECK(count_events<agent::TurnStartEvent>(run.events) == 3);
    CHECK(final_stop_reason(run.events) == ai::AssistantStopReason::ToolUse);
    REQUIRE(runtime->calls.size() == 3);

    expect_json_equal(
        scheduling_events_to_json(run.events),
        "tool-scheduling.json");

    // The truncated turn executed none of its tools; gamma/delta ran exactly
    // once (turn 3) despite the Length stop in turn 2. Turn 2's request saw
    // only turn 1's four messages; the two truncated error results appear in
    // turn 3's request at indexes 5/6.
    CHECK(runtime->calls[1].context.messages.size() == 4);
    REQUIRE(runtime->calls[2].context.messages.size() == 7);
    const auto& third_turn_messages = runtime->calls[2].context.messages;
    REQUIRE(std::holds_alternative<ai::ToolResultMessage>(third_turn_messages[5]));
    REQUIRE(std::holds_alternative<ai::ToolResultMessage>(third_turn_messages[6]));
    CHECK(std::get<ai::ToolResultMessage>(third_turn_messages[5]).is_error);
    CHECK(std::get<ai::ToolResultMessage>(third_turn_messages[6]).is_error);
    CHECK(run.state.pending_tool_call_ids.empty());
}
