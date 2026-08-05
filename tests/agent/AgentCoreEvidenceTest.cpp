// pi-agent-core fixture evidence for #351 (T02): the committed goldens under
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
#include "agent/AgentLoop.hpp"
#include "ai/SimpleOptions.hpp"
#include "support/FakeModelRuntime.hpp"
#include "support/ModelFixture.hpp"
#include "support/ToolArgumentContracts.hpp"
#include "util/ExpectedMacros.hpp"
#include "util/Json.hpp"

#include "../../third_party/catch2/catch_test_macros.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>

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
    const util::JsonValue& actual,
    std::string_view fixture_name) {
    auto serialized = util::write_json(actual);
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
[[nodiscard]] util::JsonValue options_to_json(
    const ai::SimpleStreamOptions& options,
    const ai::Model& model) {
    util::JsonValue object{util::JsonValue::object_t{}};
    auto& o = object.get_object();

    util::JsonValue model_object{util::JsonValue::object_t{}};
    model_object.get_object().emplace("id", util::JsonValue(model.id));
    model_object.get_object().emplace(
        "provider", util::JsonValue(model.provider));
    model_object.get_object().emplace("api", util::JsonValue(model.api));
    o.emplace("model", std::move(model_object));

    if (options.reasoning) {
        o.emplace(
            "reasoning",
            util::JsonValue(thinking_level_name(*options.reasoning)));
    } else {
        o.emplace("reasoning", util::JsonValue(nullptr));
    }
    if (options.session_id) {
        o.emplace("sessionId", util::JsonValue(*options.session_id));
    } else {
        o.emplace("sessionId", util::JsonValue(nullptr));
    }
    o.emplace(
        "cacheRetention",
        util::JsonValue(cache_retention_name(
            ai::detail::resolve_cache_retention(
                options.cache_retention, options.env))));
    if (options.timeout_ms) {
        o.emplace(
            "timeoutMs",
            util::JsonValue(static_cast<int>(*options.timeout_ms)));
    } else {
        o.emplace("timeoutMs", util::JsonValue(nullptr));
    }
    o.emplace("maxRetries", util::JsonValue(static_cast<int>(options.max_retries)));
    if (options.max_retry_delay_ms) {
        o.emplace(
            "maxRetryDelayMs",
            util::JsonValue(static_cast<int>(*options.max_retry_delay_ms)));
    } else {
        o.emplace("maxRetryDelayMs", util::JsonValue(nullptr));
    }
    util::JsonValue headers{util::JsonValue::object_t{}};
    for (const auto& [name, value] : options.headers) {
        if (value) {
            headers.get_object().emplace(name, util::JsonValue(*value));
        } else {
            headers.get_object().emplace(name, util::JsonValue(nullptr));
        }
    }
    o.emplace("headers", std::move(headers));
    o.emplace(
        "signal",
        util::JsonValue(options.stop_token != std::stop_token{}));
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

    [[nodiscard]] util::JsonValue event_to_json(
        const agent::AgentLifecycleEvent& event) {
        util::JsonValue object{util::JsonValue::object_t{}};
        auto& o = object.get_object();
        if (std::holds_alternative<agent::AgentStartEvent>(event)) {
            o.emplace("type", util::JsonValue("agent_start"));
        } else if (const auto* end =
                       std::get_if<agent::AgentEndEvent>(&event)) {
            o.emplace("type", util::JsonValue("agent_end"));
            o.emplace(
                "messageCount",
                util::JsonValue(static_cast<int>(end->messages.size())));
        } else if (std::holds_alternative<agent::TurnStartEvent>(event)) {
            ++turn;
            o.emplace("type", util::JsonValue("turn_start"));
            o.emplace("turn", util::JsonValue(turn));
        } else if (const auto* end =
                       std::get_if<agent::TurnEndEvent>(&event)) {
            o.emplace("type", util::JsonValue("turn_end"));
            const auto* assistant =
                std::get_if<ai::AssistantMessage>(&end->message);
            REQUIRE(assistant != nullptr);
            o.emplace(
                "stopReason",
                util::JsonValue(ai::stop_reason_to_string(
                    assistant->stop_reason)));
            o.emplace(
                "toolResults",
                util::JsonValue(static_cast<int>(end->tool_results.size())));
        } else if (const auto* start =
                       std::get_if<agent::MessageStartEvent>(&event)) {
            o.emplace("type", util::JsonValue("message_start"));
            o.emplace("role", util::JsonValue(message_role(start->message)));
        } else if (const auto* update =
                       std::get_if<agent::MessageUpdateEvent>(&event)) {
            o.emplace("type", util::JsonValue("message_update"));
            o.emplace(
                "event",
                util::JsonValue(stream_event_name(update->assistant_event)));
        } else if (const auto* end =
                       std::get_if<agent::MessageEndEvent>(&event)) {
            o.emplace("type", util::JsonValue("message_end"));
            o.emplace("role", util::JsonValue(message_role(end->message)));
        } else if (const auto* start =
                       std::get_if<agent::ToolExecutionStartEvent>(&event)) {
            o.emplace("type", util::JsonValue("tool_execution_start"));
            o.emplace("toolCallId", util::JsonValue(start->tool_call_id));
            o.emplace("toolName", util::JsonValue(start->tool_name));
        } else if (const auto* update =
                       std::get_if<agent::ToolExecutionUpdateEvent>(&event)) {
            o.emplace("type", util::JsonValue("tool_execution_update"));
            o.emplace("toolCallId", util::JsonValue(update->tool_call_id));
            o.emplace("toolName", util::JsonValue(update->tool_name));
        } else if (const auto* end =
                       std::get_if<agent::ToolExecutionEndEvent>(&event)) {
            o.emplace("type", util::JsonValue("tool_execution_end"));
            o.emplace("toolCallId", util::JsonValue(end->tool_call_id));
            o.emplace("toolName", util::JsonValue(end->tool_name));
            o.emplace("isError", util::JsonValue(end->is_error));
        }
        return object;
    }
};

util::ExpectedVoid run_prompt(
    agent::Agent& subject,
    std::string prompt) {
    boost::asio::io_context io;
    std::optional<util::ExpectedVoid> result;
    boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<void> {
            result = co_await subject.prompt(std::move(prompt));
            co_return;
        },
        boost::asio::detached);
    io.run();
    if (!result) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Unknown,
            "agent prompt coroutine did not complete"));
    }
    return std::move(*result);
}

struct LoopRun {
    util::Expected<agent::AsyncAgentRunResult> result;
    std::vector<agent::AgentLifecycleEvent> events;
};

LoopRun run_loop(
    agent::AsyncAgentLoop& loop,
    std::string prompt,
    std::stop_token stop_token = {}) {
    boost::asio::io_context io;
    std::optional<util::Expected<agent::AsyncAgentRunResult>> result;
    std::vector<agent::AgentLifecycleEvent> events;
    boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<void> {
            result = co_await loop.continue_with(
                {},
                std::move(prompt),
                [&](const agent::AgentLifecycleEvent& event) {
                    events.push_back(event);
                    return util::ExpectedVoid{};
                },
                stop_token);
            co_return;
        },
        boost::asio::detached);
    io.run();
    REQUIRE(result.has_value());
    return LoopRun{std::move(*result), std::move(events)};
}

[[nodiscard]] util::JsonValue lifecycle_events_to_json(
    const std::vector<agent::AgentLifecycleEvent>& events) {
    LifecycleSerializer serializer;
    util::JsonValue array{util::JsonValue::array_t{}};
    for (const auto& event : events) {
        array.get_array().push_back(serializer.event_to_json(event));
    }
    return array;
}

[[nodiscard]] util::JsonValue stream_calls_to_json(
    const std::vector<tests::RecordedStreamSimpleCall>& calls) {
    util::JsonValue array{util::JsonValue::array_t{}};
    for (std::size_t index = 0; index < calls.size(); ++index) {
        const auto& call = calls[index];
        auto object = options_to_json(call.options, call.model);
        object.get_object().emplace(
            "turn", util::JsonValue(static_cast<int>(index + 1)));
        array.get_array().push_back(std::move(object));
    }
    return array;
}

class ReadTool final : public agent::AsyncAgentTool {
public:
    ReadTool()
        : definition_(
              ai::Tool{"read", "Read a file", test::permissive_object_tool_argument_contract()}) {}

    const ai::Tool& definition() const override {
        return definition_;
    }

    boost::asio::awaitable<util::Expected<agent::AsyncToolExecutionResult>> execute(
        agent::ToolInvocation,
        std::stop_token) override {
        agent::AsyncToolExecutionResult result;
        result.content.push_back(ai::text_content("tool says ok"));
        co_return result;
    }

private:
    ai::Tool definition_;
};

[[nodiscard]] ai::AssistantMessage read_tool_call_message() {
    auto args = util::read_json<util::JsonValue>(R"({"path":"README.md"})");
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
    auto runtime = std::make_shared<tests::FakeModelRuntime>();
    runtime->responses.push_back(ai::assistant_text_message("hello user"));

    agent::AsyncAgentOptions options;
    options.max_turns = 3;
    options.model = tests::make_model("gpt-test");
    options.session_id = "session-golden";
    options.cache_retention = ai::CacheRetention::Short;
    options.timeout_ms = 60000;
    options.max_retries = 2;
    options.max_retry_delay_ms = 120000;
    options.headers = {
        {std::string{"x-golden"}, std::string{"value"}},
    };
    agent::Agent subject(
        runtime,
        agent::AsyncToolRegistry{},
        std::move(options),
        agent::AgentInitialState{.thinking_level = "high"});

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
    auto runtime = std::make_shared<tests::FakeModelRuntime>();
    runtime->responses.push_back(ai::assistant_text_message("hello user"));

    // Every knob at its default: off thinking (no reasoning), no session id,
    // no timeout/retries/headers. Only the active prompt signal is forwarded,
    // and cacheRetention resolves to the pi "short" default.
    agent::AsyncAgentOptions options;
    options.max_turns = 3;
    options.model = tests::make_model("gpt-test");
    agent::Agent subject(runtime, agent::AsyncToolRegistry{}, std::move(options));

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
    auto runtime = std::make_shared<tests::FakeModelRuntime>();
    runtime->responses.push_back(read_tool_call_message());
    runtime->responses.push_back(ai::assistant_text_message("after tool"));
    runtime->responses.push_back(ai::assistant_text_message("after follow-up"));

    agent::AsyncToolRegistry tools;
    REQUIRE(tools.add(std::make_unique<ReadTool>()));

    int prepare_calls = 0;
    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    // prepare-next-turn runs after turn_end: its first update flips the
    // thinking level from medium to high, so the recorded per-turn reasoning
    // for calls 2/3 proves the update landed before the next stream call.
    options.prepare_next_turn =
        agent::adapt_sync_prepare_next_turn(
            [&prepare_calls](const agent::PrepareNextTurnContext&)
                -> util::Expected<
                    std::optional<agent::AgentLoopTurnUpdate>> {
                ++prepare_calls;
                if (prepare_calls == 1) {
                    return agent::AgentLoopTurnUpdate{
                        .thinking_level = std::string{"high"},
                    };
                }
                return std::nullopt;
            });

    agent::Agent subject(
        runtime,
        std::move(tools),
        std::move(options),
        agent::AgentInitialState{.thinking_level = "medium"});

    // A follow-up queued up front drains only after the last tool-call turn
    // completes; a steering message steered from the turn_end(1) observer
    // drains at the end of turn 1 and injects before the turn-2 request.
    REQUIRE(subject.follow_up(ai::user_text_message("follow me")));

    std::vector<agent::AgentLifecycleEvent> events;
    bool steered = false;
    auto subscription =
        subject.subscribe(
            [&](const agent::AgentLifecycleEvent& event)
                -> util::ExpectedVoid {
                events.push_back(event);
                if (!steered &&
                    std::holds_alternative<agent::TurnEndEvent>(event)) {
                    steered = true;
                    return subject.steer(ai::user_text_message("steer me"));
                }
                return util::ExpectedVoid{};
            });
    REQUIRE(subscription);
    REQUIRE(run_prompt(subject, "read the file"));

    REQUIRE(steered);
    REQUIRE(runtime->calls.size() == 3);

    util::JsonValue golden{util::JsonValue::object_t{}};
    golden.get_object().emplace("events", lifecycle_events_to_json(events));
    golden.get_object().emplace(
        "streamCalls", stream_calls_to_json(runtime->calls));
    expect_json_equal(golden, "loop-lifecycle.json");
}

TEST_CASE(
    "loop lifecycle ordering golden covers the error terminal",
    "[agent][fixture][terminal][issue351]") {
    auto runtime = std::make_shared<tests::FakeModelRuntime>();
    auto terminal = ai::assistant_text_message("");
    terminal.stop_reason = ai::AssistantStopReason::Error;
    terminal.error_message = "provider request failed";
    runtime->responses.push_back(std::move(terminal));

    agent::AsyncAgentOptions options;
    options.max_turns = 3;
    options.model = tests::make_model("gpt-test");
    agent::AsyncAgentLoop loop(runtime, agent::AsyncToolRegistry{}, std::move(options));

    auto run = run_loop(loop, "hi");

    REQUIRE(run.result.has_value());
    CHECK(run.result->stop_reason == ai::AssistantStopReason::Error);
    CHECK(runtime->terminal_events == 1);
    expect_json_equal(
        lifecycle_events_to_json(run.events), "loop-terminal-error.json");
}

TEST_CASE(
    "loop lifecycle ordering golden covers the aborted terminal",
    "[agent][fixture][terminal][abort][issue351]") {
    auto runtime = std::make_shared<tests::FakeModelRuntime>();
    runtime->responses.push_back(ai::assistant_text_message("unused"));

    agent::AsyncAgentOptions options;
    options.max_turns = 3;
    options.model = tests::make_model("gpt-test");
    agent::AsyncAgentLoop loop(runtime, agent::AsyncToolRegistry{}, std::move(options));

    std::stop_source stop_source;
    stop_source.request_stop();
    auto run = run_loop(loop, "cancel me", stop_source.get_token());

    REQUIRE(run.result.has_value());
    CHECK(run.result->stop_reason == ai::AssistantStopReason::Aborted);
    CHECK(runtime->terminal_events == 1);
    expect_json_equal(
        lifecycle_events_to_json(run.events), "loop-terminal-aborted.json");
}
