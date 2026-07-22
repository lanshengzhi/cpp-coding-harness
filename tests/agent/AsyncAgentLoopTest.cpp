#include "../../third_party/catch2/catch_test_macros.hpp"

#include "util/ExpectedMacros.hpp"

#include "../support/ToolArgumentContracts.hpp"
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
        if (sink) {
            CCH_TRY_VOID(sink(ai::AssistantStartEvent{response}));
        }
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

class TerminalOutcomeClient final : public ai::StreamingChatClient {
public:
    explicit TerminalOutcomeClient(ai::AssistantMessage terminal)
        : terminal_(std::move(terminal)) {}

    boost::asio::awaitable<util::Expected<ai::AssistantMessage>> stream(
        const ai::StreamChatRequest& request,
        ai::AssistantEventSink sink) override {
        requests.push_back(request);
        CCH_TRY_VOID(sink(ai::AssistantStartEvent{terminal_}));
        ++error_events;
        CCH_TRY_VOID(sink(ai::AssistantErrorEvent{terminal_.stop_reason, terminal_}));
        co_return terminal_;
    }

    ai::AssistantMessage terminal_;
    int error_events{0};
    std::vector<ai::StreamChatRequest> requests;
};

/// A malformed host-provided client used to prove that the Agent presents one
/// assistant lifecycle even if a provider repeats its start event.
class DuplicateStartClient final : public ai::StreamingChatClient {
public:
    explicit DuplicateStartClient(ai::AssistantMessage terminal)
        : terminal_(std::move(terminal)) {}

    boost::asio::awaitable<util::Expected<ai::AssistantMessage>> stream(
        const ai::StreamChatRequest& request,
        ai::AssistantEventSink sink) override {
        requests.push_back(request);
        CCH_TRY_VOID(sink(ai::AssistantStartEvent{terminal_}));
        CCH_TRY_VOID(sink(ai::AssistantStartEvent{terminal_}));
        CCH_TRY_VOID(sink(ai::AssistantErrorEvent{terminal_.stop_reason, terminal_}));
        co_return terminal_;
    }

    ai::AssistantMessage terminal_;
    std::vector<ai::StreamChatRequest> requests;
};

/// A conforming host-provided client whose accepted call reaches a terminal
/// event before any assistant start event and returns the same final
/// AssistantMessage through the value alternative.
class TerminalBeforeStartClient final : public ai::StreamingChatClient {
public:
    explicit TerminalBeforeStartClient(ai::AssistantMessage terminal)
        : terminal_(std::move(terminal)) {}

    boost::asio::awaitable<util::Expected<ai::AssistantMessage>> stream(
        const ai::StreamChatRequest& request,
        ai::AssistantEventSink sink) override {
        requests.push_back(request);
        ++terminal_events;
        if (terminal_.stop_reason == ai::AssistantStopReason::Error ||
            terminal_.stop_reason == ai::AssistantStopReason::Aborted) {
            CCH_TRY_VOID(sink(ai::AssistantErrorEvent{terminal_.stop_reason, terminal_}));
        } else {
            CCH_TRY_VOID(sink(ai::AssistantDoneEvent{terminal_.stop_reason, terminal_}));
        }
        co_return terminal_;
    }

    ai::AssistantMessage terminal_;
    int terminal_events{0};
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

RunResult run_loop(
    agent::AsyncAgentLoop& loop,
    std::string prompt,
    std::vector<ai::MessageVariant> history = {}) {
    boost::asio::io_context io;
    std::optional<util::Expected<agent::AsyncAgentRunResult>> result;
    std::vector<agent::AgentLifecycleEvent> events;

    boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<void> {
            result = co_await loop.continue_with(
                std::move(history),
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

    auto zed = std::make_unique<FakeTool>(ai::Tool{"zed", "Zed tool", test::empty_object_tool_argument_contract()});
    auto alpha = std::make_unique<FakeTool>(ai::Tool{"alpha", "Alpha tool", test::empty_object_tool_argument_contract()});
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
    CHECK(count_events<agent::MessageStartEvent>(run.events) == 2);
    CHECK(count_events<agent::MessageUpdateEvent>(run.events) == 1);
    CHECK(count_events<agent::MessageEndEvent>(run.events) == 2);
    CHECK(count_events<agent::TurnEndEvent>(run.events) == 1);
    CHECK(count_events<agent::AgentEndEvent>(run.events) == 1);
}

TEST_CASE(
    "async agent loop completes a terminal error turn without tools or continuation",
    "[agent][async][issue11]") {
    ai::AssistantMessage terminal;
    terminal.api = "openai-completions";
    terminal.provider = "openai-compatible";
    terminal.model = "gpt-test";
    terminal.stop_reason = ai::AssistantStopReason::Error;
    terminal.error_message = "could not resolve api.example: Name or service not known";
    TerminalOutcomeClient client(std::move(terminal));

    auto tool = std::make_unique<FakeTool>(
        ai::Tool{"read_file", "Read a file", test::empty_object_tool_argument_contract()});
    auto* tool_ptr = tool.get();
    agent::AsyncToolRegistry registry;
    REQUIRE(registry.add(std::move(tool)).has_value());

    int steering_decisions = 0;
    int follow_up_decisions = 0;
    int prepare_decisions = 0;
    agent::AsyncAgentOptions options{3, "gpt-test"};
    options.get_steering_messages = [&]() -> util::Expected<std::vector<ai::MessageVariant>> {
        ++steering_decisions;
        return std::vector<ai::MessageVariant>{};
    };
    options.get_follow_up_messages = [&]() -> util::Expected<std::vector<ai::MessageVariant>> {
        ++follow_up_decisions;
        return std::vector<ai::MessageVariant>{ai::user_text_message("must not be consumed")};
    };
    options.prepare_next_turn = [&](const agent::PrepareNextTurnContext&)
        -> util::Expected<std::optional<agent::AgentLoopTurnUpdate>> {
        ++prepare_decisions;
        return std::nullopt;
    };

    agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));
    auto run = run_loop(
        loop,
        "hello",
        std::vector<ai::MessageVariant>{ai::user_text_message("prior prompt")});

    REQUIRE(run.result.has_value());
    CHECK(run.result->stop_reason == ai::AssistantStopReason::Error);
    CHECK(run.result->turns == 1);
    REQUIRE(run.result->context.messages.size() == 3);
    const auto* final = std::get_if<ai::AssistantMessage>(&run.result->context.messages.back());
    REQUIRE(final != nullptr);
    CHECK(final->stop_reason == ai::AssistantStopReason::Error);
    CHECK(final->error_message == "could not resolve api.example: Name or service not known");

    CHECK(client.requests.size() == 1);
    CHECK(client.error_events == 1);
    CHECK(tool_ptr->invocations.empty());
    CHECK(steering_decisions == 1);
    CHECK(follow_up_decisions == 0);
    CHECK(prepare_decisions == 0);
    CHECK(count_events<agent::ToolExecutionStartEvent>(run.events) == 0);
    CHECK(count_events<agent::ToolExecutionEndEvent>(run.events) == 0);

    REQUIRE(run.events.size() == 8);
    std::size_t index = 0;
    CHECK(std::holds_alternative<agent::AgentStartEvent>(run.events[index++]));
    CHECK(std::holds_alternative<agent::TurnStartEvent>(run.events[index++]));
    CHECK(std::holds_alternative<agent::MessageStartEvent>(run.events[index++]));
    CHECK(std::holds_alternative<agent::MessageEndEvent>(run.events[index++]));
    const auto* assistant_start = std::get_if<agent::MessageStartEvent>(&run.events[index++]);
    REQUIRE(assistant_start != nullptr);
    REQUIRE(std::holds_alternative<ai::AssistantMessage>(assistant_start->message));
    CHECK(std::get<ai::AssistantMessage>(assistant_start->message).stop_reason == ai::AssistantStopReason::Error);
    const auto* assistant_end = std::get_if<agent::MessageEndEvent>(&run.events[index++]);
    REQUIRE(assistant_end != nullptr);
    REQUIRE(std::holds_alternative<ai::AssistantMessage>(assistant_end->message));
    CHECK(std::get<ai::AssistantMessage>(assistant_end->message).stop_reason == ai::AssistantStopReason::Error);
    const auto* turn_end = std::get_if<agent::TurnEndEvent>(&run.events[index++]);
    REQUIRE(turn_end != nullptr);
    CHECK(turn_end->tool_results.empty());
    REQUIRE(std::holds_alternative<ai::AssistantMessage>(turn_end->message));
    CHECK(std::get<ai::AssistantMessage>(turn_end->message).stop_reason == ai::AssistantStopReason::Error);
    const auto* agent_end = std::get_if<agent::AgentEndEvent>(&run.events[index++]);
    REQUIRE(agent_end != nullptr);
    REQUIRE(agent_end->messages.size() == 2);
    REQUIRE(std::holds_alternative<ai::UserMessage>(agent_end->messages[0]));
    CHECK(ai::text_from_content(std::get<ai::UserMessage>(agent_end->messages[0]).content) == "hello");
    REQUIRE(std::holds_alternative<ai::AssistantMessage>(agent_end->messages[1]));
    CHECK(std::get<ai::AssistantMessage>(agent_end->messages[1]).stop_reason == ai::AssistantStopReason::Error);
}

TEST_CASE(
    "async agent loop completes an aborted turn without tools or queued continuation",
    "[agent][async][issue13]") {
    ai::AssistantMessage terminal = tool_call_response();
    terminal.api = "openai-completions";
    terminal.provider = "openai-compatible";
    terminal.model = "gpt-test";
    terminal.stop_reason = ai::AssistantStopReason::Aborted;
    terminal.error_message = "transport operation was cancelled";
    TerminalOutcomeClient client(std::move(terminal));

    auto tool = std::make_unique<FakeTool>(
        ai::Tool{"read_file", "Read a file", test::empty_object_tool_argument_contract()});
    auto* tool_ptr = tool.get();
    agent::AsyncToolRegistry registry;
    REQUIRE(registry.add(std::move(tool)).has_value());

    int follow_up_decisions = 0;
    int prepare_decisions = 0;
    agent::AsyncAgentOptions options{3, "gpt-test"};
    options.get_follow_up_messages = [&]() -> util::Expected<std::vector<ai::MessageVariant>> {
        ++follow_up_decisions;
        return std::vector<ai::MessageVariant>{ai::user_text_message("must not be consumed")};
    };
    options.prepare_next_turn = [&](const agent::PrepareNextTurnContext&)
        -> util::Expected<std::optional<agent::AgentLoopTurnUpdate>> {
        ++prepare_decisions;
        return std::nullopt;
    };

    agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));
    auto run = run_loop(loop, "hello");

    REQUIRE(run.result.has_value());
    CHECK(run.result->stop_reason == ai::AssistantStopReason::Aborted);
    CHECK(run.result->turns == 1);
    REQUIRE(run.result->context.messages.size() == 2);
    const auto* final = std::get_if<ai::AssistantMessage>(&run.result->context.messages.back());
    REQUIRE(final != nullptr);
    CHECK(final->stop_reason == ai::AssistantStopReason::Aborted);
    CHECK(final->error_message == "transport operation was cancelled");

    CHECK(client.requests.size() == 1);
    CHECK(client.error_events == 1);
    CHECK(tool_ptr->invocations.empty());
    CHECK(follow_up_decisions == 0);
    CHECK(prepare_decisions == 0);
    CHECK(count_events<agent::ToolExecutionStartEvent>(run.events) == 0);
    CHECK(count_events<agent::ToolExecutionEndEvent>(run.events) == 0);

    REQUIRE(run.events.size() == 8);
    std::size_t index = 0;
    CHECK(std::holds_alternative<agent::AgentStartEvent>(run.events[index++]));
    CHECK(std::holds_alternative<agent::TurnStartEvent>(run.events[index++]));
    CHECK(std::holds_alternative<agent::MessageStartEvent>(run.events[index++]));
    CHECK(std::holds_alternative<agent::MessageEndEvent>(run.events[index++]));
    const auto* assistant_start = std::get_if<agent::MessageStartEvent>(&run.events[index++]);
    REQUIRE(assistant_start != nullptr);
    REQUIRE(std::holds_alternative<ai::AssistantMessage>(assistant_start->message));
    CHECK(std::get<ai::AssistantMessage>(assistant_start->message).stop_reason ==
          ai::AssistantStopReason::Aborted);
    const auto* assistant_end = std::get_if<agent::MessageEndEvent>(&run.events[index++]);
    REQUIRE(assistant_end != nullptr);
    REQUIRE(std::holds_alternative<ai::AssistantMessage>(assistant_end->message));
    CHECK(std::get<ai::AssistantMessage>(assistant_end->message).stop_reason ==
          ai::AssistantStopReason::Aborted);
    const auto* turn_end = std::get_if<agent::TurnEndEvent>(&run.events[index++]);
    REQUIRE(turn_end != nullptr);
    CHECK(turn_end->tool_results.empty());
    REQUIRE(std::holds_alternative<ai::AssistantMessage>(turn_end->message));
    CHECK(std::get<ai::AssistantMessage>(turn_end->message).stop_reason ==
          ai::AssistantStopReason::Aborted);
    const auto* agent_end = std::get_if<agent::AgentEndEvent>(&run.events[index++]);
    REQUIRE(agent_end != nullptr);
    REQUIRE(agent_end->messages.size() == 2);
    REQUIRE(std::holds_alternative<ai::AssistantMessage>(agent_end->messages[1]));
    CHECK(std::get<ai::AssistantMessage>(agent_end->messages[1]).stop_reason ==
          ai::AssistantStopReason::Aborted);
}

TEST_CASE(
    "async agent loop recovers an error terminal emitted before any assistant start",
    "[agent][async][issue15]") {
    ai::AssistantMessage terminal;
    terminal.api = "host-api";
    terminal.provider = "host-provider";
    terminal.model = "gpt-test";
    terminal.stop_reason = ai::AssistantStopReason::Error;
    terminal.error_message = "host transport lost before response start";
    terminal.content.emplace_back(ai::TextContent{"partial host output", std::nullopt});
    TerminalBeforeStartClient client(std::move(terminal));

    auto tool = std::make_unique<FakeTool>(
        ai::Tool{"read_file", "Read a file", test::empty_object_tool_argument_contract()});
    auto* tool_ptr = tool.get();
    agent::AsyncToolRegistry registry;
    REQUIRE(registry.add(std::move(tool)).has_value());

    int steering_decisions = 0;
    int follow_up_decisions = 0;
    int prepare_decisions = 0;
    agent::AsyncAgentOptions options{3, "gpt-test"};
    options.get_steering_messages = [&]() -> util::Expected<std::vector<ai::MessageVariant>> {
        ++steering_decisions;
        return std::vector<ai::MessageVariant>{};
    };
    options.get_follow_up_messages = [&]() -> util::Expected<std::vector<ai::MessageVariant>> {
        ++follow_up_decisions;
        return std::vector<ai::MessageVariant>{ai::user_text_message("must not be consumed")};
    };
    options.prepare_next_turn = [&](const agent::PrepareNextTurnContext&)
        -> util::Expected<std::optional<agent::AgentLoopTurnUpdate>> {
        ++prepare_decisions;
        return std::nullopt;
    };

    agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));
    auto run = run_loop(loop, "hello");

    REQUIRE(run.result.has_value());
    CHECK(run.result->stop_reason == ai::AssistantStopReason::Error);
    CHECK(run.result->turns == 1);
    REQUIRE(run.result->context.messages.size() == 2);
    const auto* final = std::get_if<ai::AssistantMessage>(&run.result->context.messages.back());
    REQUIRE(final != nullptr);
    CHECK(final->stop_reason == ai::AssistantStopReason::Error);
    REQUIRE(final->error_message.has_value());
    CHECK(*final->error_message == "host transport lost before response start");
    CHECK(ai::text_from_assistant_content(final->content) == "partial host output");

    // Steering is polled once before the provider request; tools, post-turn
    // steering, follow-up, prepare-next-turn, and further provider requests
    // are all bypassed after the terminal message.
    CHECK(client.requests.size() == 1);
    CHECK(client.terminal_events == 1);
    CHECK(tool_ptr->invocations.empty());
    CHECK(steering_decisions == 1);
    CHECK(follow_up_decisions == 0);
    CHECK(prepare_decisions == 0);
    CHECK(count_events<agent::ToolExecutionStartEvent>(run.events) == 0);
    CHECK(count_events<agent::ToolExecutionEndEvent>(run.events) == 0);

    // Exactly one assistant message_start (synthesized from the authoritative
    // final message) and exactly one message_end.
    REQUIRE(run.events.size() == 8);
    std::size_t index = 0;
    CHECK(std::holds_alternative<agent::AgentStartEvent>(run.events[index++]));
    CHECK(std::holds_alternative<agent::TurnStartEvent>(run.events[index++]));
    CHECK(std::holds_alternative<agent::MessageStartEvent>(run.events[index++]));
    CHECK(std::holds_alternative<agent::MessageEndEvent>(run.events[index++]));
    const auto* assistant_start = std::get_if<agent::MessageStartEvent>(&run.events[index++]);
    REQUIRE(assistant_start != nullptr);
    REQUIRE(std::holds_alternative<ai::AssistantMessage>(assistant_start->message));
    const auto& started = std::get<ai::AssistantMessage>(assistant_start->message);
    CHECK(started.stop_reason == ai::AssistantStopReason::Error);
    REQUIRE(started.error_message.has_value());
    CHECK(*started.error_message == "host transport lost before response start");
    CHECK(ai::text_from_assistant_content(started.content) == "partial host output");
    const auto* assistant_end = std::get_if<agent::MessageEndEvent>(&run.events[index++]);
    REQUIRE(assistant_end != nullptr);
    REQUIRE(std::holds_alternative<ai::AssistantMessage>(assistant_end->message));
    CHECK(std::get<ai::AssistantMessage>(assistant_end->message).stop_reason ==
          ai::AssistantStopReason::Error);
    const auto* turn_end = std::get_if<agent::TurnEndEvent>(&run.events[index++]);
    REQUIRE(turn_end != nullptr);
    CHECK(turn_end->tool_results.empty());
    REQUIRE(std::holds_alternative<ai::AssistantMessage>(turn_end->message));
    CHECK(std::get<ai::AssistantMessage>(turn_end->message).stop_reason ==
          ai::AssistantStopReason::Error);
    const auto* agent_end = std::get_if<agent::AgentEndEvent>(&run.events[index++]);
    REQUIRE(agent_end != nullptr);
    REQUIRE(agent_end->messages.size() == 2);
    REQUIRE(std::holds_alternative<ai::UserMessage>(agent_end->messages[0]));
    REQUIRE(std::holds_alternative<ai::AssistantMessage>(agent_end->messages[1]));
    CHECK(std::get<ai::AssistantMessage>(agent_end->messages[1]).stop_reason ==
          ai::AssistantStopReason::Error);
}

TEST_CASE(
    "async agent loop recovers an aborted terminal emitted before any assistant start",
    "[agent][async][issue15]") {
    ai::AssistantMessage terminal;
    terminal.api = "host-api";
    terminal.provider = "host-provider";
    terminal.model = "gpt-test";
    terminal.stop_reason = ai::AssistantStopReason::Aborted;
    terminal.error_message = "host cancelled before response start";
    TerminalBeforeStartClient client(std::move(terminal));

    auto tool = std::make_unique<FakeTool>(
        ai::Tool{"read_file", "Read a file", test::empty_object_tool_argument_contract()});
    auto* tool_ptr = tool.get();
    agent::AsyncToolRegistry registry;
    REQUIRE(registry.add(std::move(tool)).has_value());

    int follow_up_decisions = 0;
    int prepare_decisions = 0;
    agent::AsyncAgentOptions options{3, "gpt-test"};
    options.get_follow_up_messages = [&]() -> util::Expected<std::vector<ai::MessageVariant>> {
        ++follow_up_decisions;
        return std::vector<ai::MessageVariant>{ai::user_text_message("must not be consumed")};
    };
    options.prepare_next_turn = [&](const agent::PrepareNextTurnContext&)
        -> util::Expected<std::optional<agent::AgentLoopTurnUpdate>> {
        ++prepare_decisions;
        return std::nullopt;
    };

    agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));
    auto run = run_loop(loop, "hello");

    REQUIRE(run.result.has_value());
    CHECK(run.result->stop_reason == ai::AssistantStopReason::Aborted);
    CHECK(run.result->turns == 1);
    REQUIRE(run.result->context.messages.size() == 2);
    const auto* final = std::get_if<ai::AssistantMessage>(&run.result->context.messages.back());
    REQUIRE(final != nullptr);
    CHECK(final->stop_reason == ai::AssistantStopReason::Aborted);
    REQUIRE(final->error_message.has_value());
    CHECK(*final->error_message == "host cancelled before response start");

    CHECK(client.requests.size() == 1);
    CHECK(client.terminal_events == 1);
    CHECK(tool_ptr->invocations.empty());
    CHECK(follow_up_decisions == 0);
    CHECK(prepare_decisions == 0);
    CHECK(count_events<agent::ToolExecutionStartEvent>(run.events) == 0);
    CHECK(count_events<agent::ToolExecutionEndEvent>(run.events) == 0);

    REQUIRE(run.events.size() == 8);
    std::size_t index = 0;
    CHECK(std::holds_alternative<agent::AgentStartEvent>(run.events[index++]));
    CHECK(std::holds_alternative<agent::TurnStartEvent>(run.events[index++]));
    CHECK(std::holds_alternative<agent::MessageStartEvent>(run.events[index++]));
    CHECK(std::holds_alternative<agent::MessageEndEvent>(run.events[index++]));
    const auto* assistant_start = std::get_if<agent::MessageStartEvent>(&run.events[index++]);
    REQUIRE(assistant_start != nullptr);
    REQUIRE(std::holds_alternative<ai::AssistantMessage>(assistant_start->message));
    CHECK(std::get<ai::AssistantMessage>(assistant_start->message).stop_reason ==
          ai::AssistantStopReason::Aborted);
    const auto* assistant_end = std::get_if<agent::MessageEndEvent>(&run.events[index++]);
    REQUIRE(assistant_end != nullptr);
    REQUIRE(std::holds_alternative<ai::AssistantMessage>(assistant_end->message));
    const auto& ended = std::get<ai::AssistantMessage>(assistant_end->message);
    CHECK(ended.stop_reason == ai::AssistantStopReason::Aborted);
    REQUIRE(ended.error_message.has_value());
    CHECK(*ended.error_message == "host cancelled before response start");
    const auto* turn_end = std::get_if<agent::TurnEndEvent>(&run.events[index++]);
    REQUIRE(turn_end != nullptr);
    CHECK(turn_end->tool_results.empty());
    REQUIRE(std::holds_alternative<ai::AssistantMessage>(turn_end->message));
    CHECK(std::get<ai::AssistantMessage>(turn_end->message).stop_reason ==
          ai::AssistantStopReason::Aborted);
    const auto* agent_end = std::get_if<agent::AgentEndEvent>(&run.events[index++]);
    REQUIRE(agent_end != nullptr);
    REQUIRE(agent_end->messages.size() == 2);
    REQUIRE(std::holds_alternative<ai::AssistantMessage>(agent_end->messages[1]));
    CHECK(std::get<ai::AssistantMessage>(agent_end->messages[1]).stop_reason ==
          ai::AssistantStopReason::Aborted);
}

TEST_CASE(
    "async agent loop synthesizes the assistant start for a done terminal before any start",
    "[agent][async][issue15]") {
    auto terminal = ai::assistant_text_message("host reply without a streamed start");
    terminal.api = "host-api";
    terminal.provider = "host-provider";
    terminal.model = "gpt-test";
    TerminalBeforeStartClient client(std::move(terminal));

    agent::AsyncToolRegistry registry;
    agent::AsyncAgentLoop loop(client, std::move(registry), agent::AsyncAgentOptions{3, "gpt-test"});
    auto run = run_loop(loop, "hello");

    REQUIRE(run.result.has_value());
    CHECK(run.result->stop_reason == ai::AssistantStopReason::Stop);
    CHECK(run.result->turns == 1);
    REQUIRE(run.result->context.messages.size() == 2);
    const auto* final = std::get_if<ai::AssistantMessage>(&run.result->context.messages.back());
    REQUIRE(final != nullptr);
    CHECK(ai::text_from_assistant_content(final->content) == "host reply without a streamed start");

    CHECK(client.requests.size() == 1);
    CHECK(client.terminal_events == 1);

    REQUIRE(run.events.size() == 8);
    std::size_t index = 0;
    CHECK(std::holds_alternative<agent::AgentStartEvent>(run.events[index++]));
    CHECK(std::holds_alternative<agent::TurnStartEvent>(run.events[index++]));
    CHECK(std::holds_alternative<agent::MessageStartEvent>(run.events[index++]));
    CHECK(std::holds_alternative<agent::MessageEndEvent>(run.events[index++]));
    const auto* assistant_start = std::get_if<agent::MessageStartEvent>(&run.events[index++]);
    REQUIRE(assistant_start != nullptr);
    REQUIRE(std::holds_alternative<ai::AssistantMessage>(assistant_start->message));
    CHECK(std::get<ai::AssistantMessage>(assistant_start->message).stop_reason ==
          ai::AssistantStopReason::Stop);
    CHECK(std::holds_alternative<agent::MessageEndEvent>(run.events[index++]));
    const auto* turn_end = std::get_if<agent::TurnEndEvent>(&run.events[index++]);
    REQUIRE(turn_end != nullptr);
    CHECK(turn_end->tool_results.empty());
    CHECK(std::holds_alternative<agent::AgentEndEvent>(run.events[index++]));
}

TEST_CASE(
    "async agent loop does not synthesize a duplicate after a streamed assistant start",
    "[agent][async][issue15]") {
    ai::AssistantMessage terminal;
    terminal.api = "host-api";
    terminal.provider = "host-provider";
    terminal.model = "gpt-test";
    terminal.stop_reason = ai::AssistantStopReason::Error;
    terminal.error_message = "host transport lost after response start";
    TerminalOutcomeClient client(std::move(terminal));

    agent::AsyncToolRegistry registry;
    agent::AsyncAgentLoop loop(client, std::move(registry), agent::AsyncAgentOptions{3, "gpt-test"});
    auto run = run_loop(loop, "hello");

    REQUIRE(run.result.has_value());
    CHECK(run.result->stop_reason == ai::AssistantStopReason::Error);

    std::size_t assistant_starts = 0;
    std::size_t assistant_ends = 0;
    for (const auto& event : run.events) {
        if (const auto* start = std::get_if<agent::MessageStartEvent>(&event)) {
            if (std::holds_alternative<ai::AssistantMessage>(start->message)) {
                ++assistant_starts;
            }
        }
        if (const auto* end = std::get_if<agent::MessageEndEvent>(&event)) {
            if (std::holds_alternative<ai::AssistantMessage>(end->message)) {
                ++assistant_ends;
            }
        }
    }
    CHECK(assistant_starts == 1);
    CHECK(assistant_ends == 1);
}

TEST_CASE(
    "async agent loop suppresses duplicate assistant starts from a host provider",
    "[agent][async][issue15]") {
    ai::AssistantMessage terminal;
    terminal.api = "host-api";
    terminal.provider = "host-provider";
    terminal.model = "gpt-test";
    terminal.stop_reason = ai::AssistantStopReason::Error;
    terminal.error_message = "host transport lost after duplicate starts";
    DuplicateStartClient client(std::move(terminal));

    agent::AsyncToolRegistry registry;
    agent::AsyncAgentLoop loop(client, std::move(registry), agent::AsyncAgentOptions{3, "gpt-test"});
    auto run = run_loop(loop, "hello");

    REQUIRE(run.result.has_value());
    CHECK(run.result->stop_reason == ai::AssistantStopReason::Error);
    CHECK(client.requests.size() == 1);

    std::size_t assistant_starts = 0;
    std::size_t assistant_ends = 0;
    for (const auto& event : run.events) {
        if (const auto* start = std::get_if<agent::MessageStartEvent>(&event);
            start && std::holds_alternative<ai::AssistantMessage>(start->message)) {
            ++assistant_starts;
        }
        if (const auto* end = std::get_if<agent::MessageEndEvent>(&event);
            end && std::holds_alternative<ai::AssistantMessage>(end->message)) {
            ++assistant_ends;
        }
    }
    CHECK(assistant_starts == 1);
    CHECK(assistant_ends == 1);
}

TEST_CASE("async agent loop emits user message lifecycle before assistant response", "[agent][async]") {
    FakeStreamingClient client;
    client.responses.push_back(ai::assistant_text_message("hello user"));
    agent::AsyncToolRegistry registry;
    agent::AsyncAgentLoop loop(client, std::move(registry), agent::AsyncAgentOptions{3, "gpt-test"});

    auto run = run_loop(loop, "hi");

    REQUIRE(run.result);
    REQUIRE(run.result->context.messages.size() == 2);

    // Exact semantic order: agent_start, turn_start, user lifecycle, assistant lifecycle, turn_end, agent_end.
    std::size_t index = 0;
    REQUIRE(std::holds_alternative<agent::AgentStartEvent>(run.events[index++]));
    REQUIRE(std::holds_alternative<agent::TurnStartEvent>(run.events[index++]));

    const auto* user_start = std::get_if<agent::MessageStartEvent>(&run.events[index++]);
    REQUIRE(user_start);
    REQUIRE(std::holds_alternative<ai::UserMessage>(user_start->message));
    CHECK(ai::text_from_content(std::get<ai::UserMessage>(user_start->message).content) == "hi");

    const auto* user_end = std::get_if<agent::MessageEndEvent>(&run.events[index++]);
    REQUIRE(user_end);
    REQUIRE(std::holds_alternative<ai::UserMessage>(user_end->message));
    CHECK(ai::text_from_content(std::get<ai::UserMessage>(user_end->message).content) == "hi");

    const auto* assistant_start = std::get_if<agent::MessageStartEvent>(&run.events[index++]);
    REQUIRE(assistant_start);
    REQUIRE(std::holds_alternative<ai::AssistantMessage>(assistant_start->message));

    const auto* assistant_update = std::get_if<agent::MessageUpdateEvent>(&run.events[index++]);
    REQUIRE(assistant_update);
    REQUIRE(std::holds_alternative<ai::AssistantMessage>(assistant_update->message));
    CHECK(std::holds_alternative<ai::TextDeltaEvent>(assistant_update->assistant_event));
    CHECK(ai::text_from_assistant_content(std::get<ai::AssistantMessage>(assistant_update->message).content) == "hello user");

    const auto* assistant_end = std::get_if<agent::MessageEndEvent>(&run.events[index++]);
    REQUIRE(assistant_end);
    REQUIRE(std::holds_alternative<ai::AssistantMessage>(assistant_end->message));
    CHECK(ai::text_from_assistant_content(std::get<ai::AssistantMessage>(assistant_end->message).content) == "hello user");

    const auto* turn_end = std::get_if<agent::TurnEndEvent>(&run.events[index++]);
    REQUIRE(turn_end);
    REQUIRE(std::holds_alternative<ai::AssistantMessage>(turn_end->message));
    CHECK(turn_end->tool_results.empty());
    CHECK(ai::text_from_assistant_content(std::get<ai::AssistantMessage>(turn_end->message).content) == "hello user");

    const auto* agent_end = std::get_if<agent::AgentEndEvent>(&run.events[index++]);
    REQUIRE(agent_end);
    REQUIRE(agent_end->messages.size() == 2);
    REQUIRE(std::holds_alternative<ai::UserMessage>(agent_end->messages[0]));
    REQUIRE(std::holds_alternative<ai::AssistantMessage>(agent_end->messages[1]));
    CHECK(ai::text_from_content(std::get<ai::UserMessage>(agent_end->messages[0]).content) == "hi");
    CHECK(ai::text_from_assistant_content(std::get<ai::AssistantMessage>(agent_end->messages[1]).content) == "hello user");

    CHECK(index == run.events.size());
}

TEST_CASE("async agent loop emits user lifecycle before steering failure", "[agent][async]") {
    FakeStreamingClient client;
    client.responses.push_back(ai::assistant_text_message("should not reach"));
    agent::AsyncToolRegistry registry;

    agent::AsyncAgentOptions options{4, "gpt-test"};
    options.get_steering_messages = []() -> util::Expected<std::vector<ai::MessageVariant>> {
        return std::unexpected(util::make_error(util::ErrorCode::Validation, "steering validation failed"));
    };

    agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));
    auto run = run_loop(loop, "hi");

    REQUIRE_FALSE(run.result);
    CHECK(run.result.error().message == "steering validation failed");

    std::size_t user_start_count = 0;
    std::size_t user_end_count = 0;
    for (const auto& event : run.events) {
        if (const auto* start = std::get_if<agent::MessageStartEvent>(&event)) {
            if (std::holds_alternative<ai::UserMessage>(start->message)) {
                ++user_start_count;
                CHECK(ai::text_from_content(std::get<ai::UserMessage>(start->message).content) == "hi");
            }
        } else if (const auto* end = std::get_if<agent::MessageEndEvent>(&event)) {
            if (std::holds_alternative<ai::UserMessage>(end->message)) {
                ++user_end_count;
                CHECK(ai::text_from_content(std::get<ai::UserMessage>(end->message).content) == "hi");
            }
        }
    }
    CHECK(user_start_count == 1);
    CHECK(user_end_count == 1);

    const auto* agent_end = std::get_if<agent::AgentEndEvent>(&run.events.back());
    REQUIRE(agent_end);
    REQUIRE(agent_end->messages.size() == 1);
    REQUIRE(std::holds_alternative<ai::UserMessage>(agent_end->messages[0]));
    CHECK(ai::text_from_content(std::get<ai::UserMessage>(agent_end->messages[0]).content) == "hi");
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
        test::path_tool_argument_contract(),
    })));
    agent::AsyncAgentLoop loop(client, std::move(registry), agent::AsyncAgentOptions{4, "gpt-test"});

    auto run = run_loop(loop, "read");

    REQUIRE(run.result);
    CHECK(count_events<agent::MessageUpdateEvent>(run.events) >= 1);
    CHECK(count_events<agent::MessageStartEvent>(run.events) == 4);
    CHECK(count_events<agent::MessageEndEvent>(run.events) == 4);
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
        test::path_tool_argument_contract(),
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
    CHECK(count_events<agent::MessageUpdateEvent>(run.events) >= 1);
    CHECK(count_events<agent::MessageStartEvent>(run.events) == 4);
    CHECK(count_events<agent::MessageEndEvent>(run.events) == 4);
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
    REQUIRE(registry.add(std::make_unique<FakeTool>(ai::Tool{"read_file", "Read", test::empty_object_tool_argument_contract()})));
    agent::AsyncAgentLoop loop(client, std::move(registry), agent::AsyncAgentOptions{4, "gpt-test"});

    auto run = run_loop(loop, "read");

    REQUIRE(run.result);
    REQUIRE(client.requests.size() == 2);
    REQUIRE(std::holds_alternative<ai::ToolResultMessage>(client.requests[1].context.messages.back()));
    const auto& result = std::get<ai::ToolResultMessage>(client.requests[1].context.messages.back());
    CHECK(result.is_error);
    CHECK(count_events<agent::ToolExecutionEndEvent>(run.events) == 1);
    CHECK(count_events<agent::MessageStartEvent>(run.events) == 4);
    CHECK(count_events<agent::MessageEndEvent>(run.events) == 4);
}

TEST_CASE("async agent loop reports max turn exhaustion", "[agent][async][u5]") {
    FakeStreamingClient client;
    client.responses.push_back(tool_call_response());
    agent::AsyncToolRegistry registry;
    REQUIRE(registry.add(std::make_unique<FakeTool>(ai::Tool{"read_file", "Read", test::empty_object_tool_argument_contract()})));
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

    auto tool = std::make_unique<FakeTool>(ai::Tool{"read_file", "Read", test::empty_object_tool_argument_contract()});
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
    CHECK(count_events<agent::MessageStartEvent>(run.events) == 4);
    CHECK(count_events<agent::MessageEndEvent>(run.events) == 4);

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

    auto tool = std::make_unique<FakeTool>(ai::Tool{"read_file", "Read", test::empty_object_tool_argument_contract()});
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
    CHECK(count_events<agent::MessageStartEvent>(run.events) == 4);
    CHECK(count_events<agent::MessageEndEvent>(run.events) == 4);
}

TEST_CASE("beforeToolCall hook failure aborts the run", "[agent][async][u7]") {
    FakeStreamingClient client;
    client.responses.push_back(tool_call_response());
    agent::AsyncToolRegistry registry;
    REQUIRE(registry.add(std::make_unique<FakeTool>(ai::Tool{"read_file", "Read", test::empty_object_tool_argument_contract()})));

    agent::AsyncAgentOptions options{4, "gpt-test"};
    options.before_tool_call = [](const agent::BeforeToolCallContext&) -> util::Expected<agent::BeforeToolCallResult> {
        return std::unexpected(util::make_error(util::ErrorCode::Tool, "policy rejected"));
    };

    agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));
    auto run = run_loop(loop, "read");

    REQUIRE_FALSE(run.result);
    CHECK(run.result.error().code == util::ErrorCode::Tool);
    CHECK(run.result.error().message == "policy rejected");
    CHECK(count_events<agent::MessageStartEvent>(run.events) == 2);
    CHECK(count_events<agent::MessageEndEvent>(run.events) == 2);

    const auto* end_event = std::get_if<agent::AgentEndEvent>(&run.events.back());
    REQUIRE(end_event);
}

TEST_CASE("beforeToolCall hook exception becomes a tool error", "[agent][async][u7]") {
    FakeStreamingClient client;
    client.responses.push_back(tool_call_response());
    agent::AsyncToolRegistry registry;
    REQUIRE(registry.add(std::make_unique<FakeTool>(ai::Tool{"read_file", "Read", test::empty_object_tool_argument_contract()})));

    agent::AsyncAgentOptions options{4, "gpt-test"};
    options.before_tool_call = [](const agent::BeforeToolCallContext&) -> util::Expected<agent::BeforeToolCallResult> {
        throw std::runtime_error("boom");
    };

    agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));
    auto run = run_loop(loop, "read");

    REQUIRE_FALSE(run.result);
    CHECK(run.result.error().code == util::ErrorCode::Tool);
    CHECK(run.result.error().detail.find("boom") != std::string::npos);
    CHECK(count_events<agent::MessageStartEvent>(run.events) == 2);
    CHECK(count_events<agent::MessageEndEvent>(run.events) == 2);
}

TEST_CASE("afterToolCall hook overrides tool result content", "[agent][async][u7]") {
    FakeStreamingClient client;
    client.responses.push_back(tool_call_response());
    client.responses.push_back(ai::assistant_text_message("done"));
    agent::AsyncToolRegistry registry;
    REQUIRE(registry.add(std::make_unique<FakeTool>(ai::Tool{"read_file", "Read", test::empty_object_tool_argument_contract()})));

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
    CHECK(count_events<agent::MessageStartEvent>(run.events) == 4);
    CHECK(count_events<agent::MessageEndEvent>(run.events) == 4);
}

TEST_CASE("afterToolCall hook overrides error flag", "[agent][async][u7]") {
    FakeStreamingClient client;
    client.responses.push_back(tool_call_response());
    client.responses.push_back(ai::assistant_text_message("done"));
    agent::AsyncToolRegistry registry;
    REQUIRE(registry.add(std::make_unique<FakeTool>(ai::Tool{"read_file", "Read", test::empty_object_tool_argument_contract()})));

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
    CHECK(count_events<agent::MessageStartEvent>(run.events) == 4);
    CHECK(count_events<agent::MessageEndEvent>(run.events) == 4);
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
        agent::ToolConcurrency concurrency,
        std::string result_text = "tool says ok")
        : definition_(std::move(definition)),
          concurrency_(concurrency),
          result_text_(std::move(result_text)) {}

    const ai::Tool& definition() const override { return definition_; }

    agent::ToolConcurrency concurrency() const noexcept override {
        return concurrency_;
    }

    boost::asio::awaitable<util::Expected<agent::AsyncToolExecutionResult>> execute(
        agent::ToolInvocation invocation) override {
        invocations.push_back(invocation);
        co_return agent::AsyncToolExecutionResult{
            std::vector<ai::Content>{ai::text_content(result_text_)}, std::nullopt, false};
    }

    ai::Tool definition_;
    agent::ToolConcurrency concurrency_;
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

    agent::ToolConcurrency concurrency() const noexcept override {
        return agent::ToolConcurrency::ParallelSafe;
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

    agent::ToolConcurrency concurrency() const noexcept override {
        return agent::ToolConcurrency::ParallelSafe;
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
        agent::ToolConcurrency concurrency,
        ConcurrencyProbe& probe)
        : definition_(std::move(definition)), concurrency_(concurrency), probe_(probe) {}

    const ai::Tool& definition() const override { return definition_; }

    agent::ToolConcurrency concurrency() const noexcept override {
        return concurrency_;
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
    agent::ToolConcurrency concurrency_;
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
    REQUIRE(registry.add(std::make_unique<FakeTool>(ai::Tool{"read_file", "Read", test::empty_object_tool_argument_contract()})));

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
    CHECK(count_events<agent::MessageStartEvent>(run.events) == 3);
    CHECK(count_events<agent::MessageEndEvent>(run.events) == 3);
    const auto* end_event = std::get_if<agent::AgentEndEvent>(&run.events.back());
    REQUIRE(end_event);
}

TEST_CASE("terminate batch continues when one call declines", "[agent][async][u7]") {
    FakeStreamingClient client;
    client.responses.push_back(two_tool_call_response());
    client.responses.push_back(ai::assistant_text_message("done"));

    agent::AsyncToolRegistry registry;
    REQUIRE(registry.add(std::make_unique<FakeTool>(ai::Tool{"alpha", "Alpha", test::empty_object_tool_argument_contract()})));
    REQUIRE(registry.add(std::make_unique<FakeTool>(ai::Tool{"beta", "Beta", test::empty_object_tool_argument_contract()})));

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
    CHECK(count_events<agent::MessageStartEvent>(run.events) == 5);
    CHECK(count_events<agent::MessageEndEvent>(run.events) == 5);
}

TEST_CASE("blocked call prevents terminate batch", "[agent][async][u7]") {
    FakeStreamingClient client;
    client.responses.push_back(two_tool_call_response());
    client.responses.push_back(ai::assistant_text_message("done"));

    agent::AsyncToolRegistry registry;
    REQUIRE(registry.add(std::make_unique<FakeTool>(ai::Tool{"alpha", "Alpha", test::empty_object_tool_argument_contract()})));
    REQUIRE(registry.add(std::make_unique<FakeTool>(ai::Tool{"beta", "Beta", test::empty_object_tool_argument_contract()})));

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
    CHECK(count_events<agent::MessageStartEvent>(run.events) == 5);
    CHECK(count_events<agent::MessageEndEvent>(run.events) == 5);
}

TEST_CASE("tool execution error prevents terminate batch", "[agent][async][u7]") {
    FakeStreamingClient client;
    client.responses.push_back(two_tool_call_response());
    client.responses.push_back(ai::assistant_text_message("done"));

    agent::AsyncToolRegistry registry;
    REQUIRE(registry.add(std::make_unique<FakeTool>(ai::Tool{"alpha", "Alpha", test::empty_object_tool_argument_contract()})));
    REQUIRE(registry.add(std::make_unique<FakeTool>(ai::Tool{"beta", "Beta", test::empty_object_tool_argument_contract()})));

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
    CHECK(count_events<agent::MessageStartEvent>(run.events) == 5);
    CHECK(count_events<agent::MessageEndEvent>(run.events) == 5);
}

TEST_CASE("afterToolCall hook failure aborts the run", "[agent][async][u7]") {
    FakeStreamingClient client;
    client.responses.push_back(tool_call_response());
    agent::AsyncToolRegistry registry;
    REQUIRE(registry.add(std::make_unique<FakeTool>(ai::Tool{"read_file", "Read", test::empty_object_tool_argument_contract()})));

    agent::AsyncAgentOptions options{4, "gpt-test"};
    options.after_tool_call = [](const agent::AfterToolCallContext&) -> util::Expected<agent::AfterToolCallResult> {
        return std::unexpected(util::make_error(util::ErrorCode::Tool, "post-processor failed"));
    };

    agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));
    auto run = run_loop(loop, "read");

    REQUIRE_FALSE(run.result);
    CHECK(run.result.error().code == util::ErrorCode::Tool);
    CHECK(run.result.error().message == "post-processor failed");
    CHECK(count_events<agent::MessageStartEvent>(run.events) == 2);
    CHECK(count_events<agent::MessageEndEvent>(run.events) == 2);
}

TEST_CASE("afterToolCall hook exception becomes a tool error", "[agent][async][u7]") {
    FakeStreamingClient client;
    client.responses.push_back(tool_call_response());
    agent::AsyncToolRegistry registry;
    REQUIRE(registry.add(std::make_unique<FakeTool>(ai::Tool{"read_file", "Read", test::empty_object_tool_argument_contract()})));

    agent::AsyncAgentOptions options{4, "gpt-test"};
    options.after_tool_call = [](const agent::AfterToolCallContext&) -> util::Expected<agent::AfterToolCallResult> {
        throw std::runtime_error("after boom");
    };

    agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));
    auto run = run_loop(loop, "read");

    REQUIRE_FALSE(run.result);
    CHECK(run.result.error().code == util::ErrorCode::Tool);
    CHECK(run.result.error().detail.find("after boom") != std::string::npos);
    CHECK(count_events<agent::MessageStartEvent>(run.events) == 2);
    CHECK(count_events<agent::MessageEndEvent>(run.events) == 2);
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
    REQUIRE(registry.add(std::make_unique<FakeTool>(ai::Tool{"read_file", "Read", test::empty_object_tool_argument_contract()})));

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
        if (const auto* start = std::get_if<agent::MessageStartEvent>(&event)) {
            if (std::holds_alternative<ai::UserMessage>(start->message)) {
                ++queued_start_events;
            }
        } else if (const auto* end = std::get_if<agent::MessageEndEvent>(&event)) {
            if (std::holds_alternative<ai::UserMessage>(end->message)) {
                ++queued_end_events;
            }
        }
    }
    CHECK(queued_start_events == 2);
    CHECK(queued_end_events == 2);
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

TEST_CASE("steering messages emit ordinary message lifecycle in order", "[agent][async]") {
    FakeStreamingClient client;
    client.responses.push_back(ai::assistant_text_message("ok"));

    agent::AsyncToolRegistry registry;
    bool steering_returned = false;
    agent::AsyncAgentOptions options{4, "gpt-test"};
    options.get_steering_messages = [&]() -> util::Expected<std::vector<ai::MessageVariant>> {
        if (steering_returned) {
            return std::vector<ai::MessageVariant>{};
        }
        steering_returned = true;
        return std::vector<ai::MessageVariant>{
            ai::user_text_message("steer-one"),
            ai::user_text_message("steer-two"),
        };
    };

    agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));
    auto run = run_loop(loop, "hi");

    REQUIRE(run.result);
    REQUIRE(run.result->context.messages.size() == 4);
    CHECK(ai::text_from_content(std::get<ai::UserMessage>(run.result->context.messages[0]).content) == "hi");
    CHECK(ai::text_from_content(std::get<ai::UserMessage>(run.result->context.messages[1]).content) == "steer-one");
    CHECK(ai::text_from_content(std::get<ai::UserMessage>(run.result->context.messages[2]).content) == "steer-two");
    CHECK(ai::text_from_assistant_content(std::get<ai::AssistantMessage>(run.result->context.messages[3]).content) == "ok");

    std::vector<std::string> user_starts;
    std::vector<std::string> user_ends;
    for (const auto& event : run.events) {
        if (const auto* start = std::get_if<agent::MessageStartEvent>(&event)) {
            if (std::holds_alternative<ai::UserMessage>(start->message)) {
                user_starts.push_back(ai::text_from_content(std::get<ai::UserMessage>(start->message).content));
            }
        } else if (const auto* end = std::get_if<agent::MessageEndEvent>(&event)) {
            if (std::holds_alternative<ai::UserMessage>(end->message)) {
                user_ends.push_back(ai::text_from_content(std::get<ai::UserMessage>(end->message).content));
            }
        }
    }
    REQUIRE((user_starts == std::vector<std::string>{"hi", "steer-one", "steer-two"}));
    REQUIRE((user_ends == std::vector<std::string>{"hi", "steer-one", "steer-two"}));

    CHECK(count_events<agent::MessageStartEvent>(run.events) == 4);
    CHECK(count_events<agent::MessageEndEvent>(run.events) == 4);
    CHECK(count_events<agent::MessageUpdateEvent>(run.events) == 1);
}

TEST_CASE("follow-up messages emit ordinary message lifecycle across turns", "[agent][async]") {
    FakeStreamingClient client;
    client.responses.push_back(ai::assistant_text_message("first"));
    client.responses.push_back(ai::assistant_text_message("second"));

    agent::AsyncToolRegistry registry;
    bool follow_up_returned = false;
    agent::AsyncAgentOptions options{4, "gpt-test"};
    options.get_follow_up_messages = [&]() -> util::Expected<std::vector<ai::MessageVariant>> {
        if (follow_up_returned) {
            return std::vector<ai::MessageVariant>{};
        }
        follow_up_returned = true;
        return std::vector<ai::MessageVariant>{
            ai::user_text_message("follow-one"),
            ai::user_text_message("follow-two"),
        };
    };

    agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));
    auto run = run_loop(loop, "hi");

    REQUIRE(run.result);
    CHECK(run.result->turns == 2);
    REQUIRE(client.requests.size() == 2);
    REQUIRE(client.requests[1].context.messages.size() == 4);
    CHECK(ai::text_from_content(std::get<ai::UserMessage>(client.requests[1].context.messages[2]).content) == "follow-one");
    CHECK(ai::text_from_content(std::get<ai::UserMessage>(client.requests[1].context.messages[3]).content) == "follow-two");

    std::vector<std::string> user_starts;
    std::vector<std::size_t> user_start_indices;
    for (std::size_t i = 0; i < run.events.size(); ++i) {
        if (const auto* start = std::get_if<agent::MessageStartEvent>(&run.events[i])) {
            if (std::holds_alternative<ai::UserMessage>(start->message)) {
                user_starts.push_back(ai::text_from_content(std::get<ai::UserMessage>(start->message).content));
                user_start_indices.push_back(i);
            }
        }
    }
    REQUIRE((user_starts == std::vector<std::string>{"hi", "follow-one", "follow-two"}));

    std::size_t turn2_start_index = 0;
    std::size_t turn_start_count = 0;
    for (std::size_t i = 0; i < run.events.size(); ++i) {
        if (std::holds_alternative<agent::TurnStartEvent>(run.events[i])) {
            ++turn_start_count;
            if (turn_start_count == 2) {
                turn2_start_index = i;
            }
        }
    }
    REQUIRE(turn_start_count == 2);
    REQUIRE(turn2_start_index > 0);
    REQUIRE(turn2_start_index < user_start_indices[1]);
    REQUIRE(turn2_start_index < user_start_indices[2]);
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
    REQUIRE(registry.add(std::make_unique<FakeTool>(ai::Tool{"read_file", "Read", test::empty_object_tool_argument_contract()})));

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
    REQUIRE(registry.add(std::make_unique<FakeTool>(ai::Tool{"read_file", "Read", test::empty_object_tool_argument_contract()})));

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
    REQUIRE(registry.add(std::make_unique<FakeTool>(ai::Tool{"read_file", "Read", test::empty_object_tool_argument_contract()})));

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

TEST_CASE("prepareNextTurn append_messages emits ordinary message lifecycle", "[agent][async]") {
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
    CHECK(run.result->turns == 1);
    REQUIRE(run.result->context.messages.size() == 3);
    REQUIRE(std::holds_alternative<ai::UserMessage>(run.result->context.messages.back()));
    CHECK(ai::text_from_content(std::get<ai::UserMessage>(run.result->context.messages.back()).content) == "appended");

    std::size_t turn_end_index = 0;
    std::size_t agent_end_index = 0;
    for (std::size_t i = 0; i < run.events.size(); ++i) {
        if (std::holds_alternative<agent::TurnEndEvent>(run.events[i])) {
            turn_end_index = i;
        } else if (std::holds_alternative<agent::AgentEndEvent>(run.events[i])) {
            agent_end_index = i;
        }
    }
    REQUIRE(turn_end_index > 0);
    REQUIRE(agent_end_index > turn_end_index);

    std::size_t appended_starts = 0;
    std::size_t appended_ends = 0;
    std::optional<std::string> appended_text;
    for (std::size_t i = turn_end_index + 1; i < agent_end_index; ++i) {
        if (const auto* start = std::get_if<agent::MessageStartEvent>(&run.events[i])) {
            if (std::holds_alternative<ai::UserMessage>(start->message)) {
                ++appended_starts;
                appended_text = ai::text_from_content(std::get<ai::UserMessage>(start->message).content);
            }
        } else if (const auto* end = std::get_if<agent::MessageEndEvent>(&run.events[i])) {
            if (std::holds_alternative<ai::UserMessage>(end->message)) {
                ++appended_ends;
            }
        }
    }
    CHECK(appended_starts == 1);
    CHECK(appended_ends == 1);
    REQUIRE(appended_text.has_value());
    CHECK(*appended_text == "appended");
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
    REQUIRE(registry.add(std::make_unique<FakeTool>(ai::Tool{"read_file", "Read", test::empty_object_tool_argument_contract()})));

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

TEST_CASE("tool execution policy defaults to sequential", "[agent][async][u8]") {
    agent::AsyncAgentOptions options;
    CHECK(std::holds_alternative<agent::SequentialToolExecution>(options.tool_execution));
}

TEST_CASE("an exclusive tool forces a bounded batch to execute sequentially", "[agent][async][u8]") {
    FakeStreamingClient client;
    client.responses.push_back(two_tool_call_response());
    client.responses.push_back(ai::assistant_text_message("done"));

    ConcurrencyProbe probe;
    agent::AsyncToolRegistry registry;
    REQUIRE(registry.add(std::make_unique<ProbedFakeTool>(
        ai::Tool{"alpha", "Alpha", test::empty_object_tool_argument_contract()},
        agent::ToolConcurrency::ParallelSafe,
        probe)));
    REQUIRE(registry.add(std::make_unique<ProbedFakeTool>(
        ai::Tool{"beta", "Beta", test::empty_object_tool_argument_contract()},
        agent::ToolConcurrency::Exclusive,
        probe)));

    agent::AsyncAgentOptions options{4, "gpt-test"};
    options.tool_execution = agent::BoundedParallelToolExecution{2};

    agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));
    auto run = run_loop_on_pool(loop, "read");

    REQUIRE(run.result);
    REQUIRE(client.requests.size() == 2);
    CHECK(probe.max_active.load() == 1);
}

TEST_CASE("bounded parallel execution preserves source order in the transcript", "[agent][async][u8]") {
    FakeStreamingClient client;
    client.responses.push_back(two_tool_call_response());
    client.responses.push_back(ai::assistant_text_message("done"));

    agent::AsyncToolRegistry registry;
    auto alpha = std::make_unique<ConfigurableFakeTool>(
        ai::Tool{"alpha", "Alpha", test::empty_object_tool_argument_contract()},
        agent::ToolConcurrency::ParallelSafe,
        "alpha result");
    auto beta = std::make_unique<ConfigurableFakeTool>(
        ai::Tool{"beta", "Beta", test::empty_object_tool_argument_contract()},
        agent::ToolConcurrency::ParallelSafe,
        "beta result");
    auto* alpha_ptr = alpha.get();
    auto* beta_ptr = beta.get();
    REQUIRE(registry.add(std::move(alpha)));
    REQUIRE(registry.add(std::move(beta)));

    agent::AsyncAgentOptions options{4, "gpt-test"};
    options.tool_execution = agent::BoundedParallelToolExecution{2};

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
    CHECK(count_events<agent::MessageStartEvent>(run.events) == 5);
    CHECK(count_events<agent::MessageEndEvent>(run.events) == 5);

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

TEST_CASE("bounded parallel limit one executes sequentially", "[agent][async][u8]") {
    FakeStreamingClient client;
    client.responses.push_back(two_tool_call_response());
    client.responses.push_back(ai::assistant_text_message("done"));

    ConcurrencyProbe probe;
    agent::AsyncToolRegistry registry;
    REQUIRE(registry.add(std::make_unique<ProbedFakeTool>(
        ai::Tool{"alpha", "Alpha", test::empty_object_tool_argument_contract()},
        agent::ToolConcurrency::ParallelSafe,
        probe)));
    REQUIRE(registry.add(std::make_unique<ProbedFakeTool>(
        ai::Tool{"beta", "Beta", test::empty_object_tool_argument_contract()},
        agent::ToolConcurrency::ParallelSafe,
        probe)));

    agent::AsyncAgentOptions options{4, "gpt-test"};
    options.tool_execution = agent::BoundedParallelToolExecution{1};

    agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));
    auto run = run_loop_on_pool(loop, "read");

    REQUIRE(run.result);
    CHECK(probe.max_active.load() == 1);
    CHECK(count_events<agent::MessageStartEvent>(run.events) == 5);
    CHECK(count_events<agent::MessageEndEvent>(run.events) == 5);
}

TEST_CASE("bounded parallel policy rejects zero before tools start", "[agent][async][u8]") {
    FakeStreamingClient client;
    client.responses.push_back(two_tool_call_response());

    ConcurrencyProbe probe;
    agent::AsyncToolRegistry registry;
    REQUIRE(registry.add(std::make_unique<ProbedFakeTool>(
        ai::Tool{"alpha", "Alpha", test::empty_object_tool_argument_contract()},
        agent::ToolConcurrency::ParallelSafe,
        probe)));
    REQUIRE(registry.add(std::make_unique<ProbedFakeTool>(
        ai::Tool{"beta", "Beta", test::empty_object_tool_argument_contract()},
        agent::ToolConcurrency::ParallelSafe,
        probe)));

    agent::AsyncAgentOptions options{4, "gpt-test"};
    options.tool_execution = agent::BoundedParallelToolExecution{0};

    agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));
    auto run = run_loop_on_pool(loop, "read");

    REQUIRE_FALSE(run.result);
    CHECK(run.result.error().code == util::ErrorCode::Validation);
    CHECK(probe.max_active.load() == 0);
    CHECK(count_events<agent::ToolExecutionStartEvent>(run.events) == 0);
}

TEST_CASE("bounded parallel execution keeps blocked calls out of tool adapters", "[agent][async][u8]") {
    FakeStreamingClient client;
    client.responses.push_back(two_tool_call_response());
    client.responses.push_back(ai::assistant_text_message("done"));

    agent::AsyncToolRegistry registry;
    auto alpha = std::make_unique<ConfigurableFakeTool>(
        ai::Tool{"alpha", "Alpha", test::empty_object_tool_argument_contract()},
        agent::ToolConcurrency::ParallelSafe,
        "alpha result");
    auto beta = std::make_unique<ConfigurableFakeTool>(
        ai::Tool{"beta", "Beta", test::empty_object_tool_argument_contract()},
        agent::ToolConcurrency::ParallelSafe,
        "beta result");
    auto* alpha_ptr = alpha.get();
    auto* beta_ptr = beta.get();
    REQUIRE(registry.add(std::move(alpha)));
    REQUIRE(registry.add(std::move(beta)));

    agent::AsyncAgentOptions options{4, "gpt-test"};
    options.tool_execution = agent::BoundedParallelToolExecution{2};
    options.before_tool_call = [](const agent::BeforeToolCallContext& context)
        -> util::Expected<agent::BeforeToolCallResult> {
        if (context.tool_call.name == "alpha") {
            return agent::BeforeToolCallResult{true, "blocked alpha"};
        }
        return agent::BeforeToolCallResult{};
    };

    agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));
    auto run = run_loop_on_pool(loop, "read");

    REQUIRE(run.result);
    CHECK(alpha_ptr->invocations.empty());
    CHECK(beta_ptr->invocations.size() == 1);
    REQUIRE(client.requests.size() == 2);
    const auto& alpha_result = std::get<ai::ToolResultMessage>(client.requests[1].context.messages[2]);
    const auto& beta_result = std::get<ai::ToolResultMessage>(client.requests[1].context.messages[3]);
    CHECK(alpha_result.is_error);
    CHECK(ai::text_from_content(alpha_result.content) == "blocked alpha");
    CHECK_FALSE(beta_result.is_error);
    CHECK(count_events<agent::MessageStartEvent>(run.events) == 5);
    CHECK(count_events<agent::MessageEndEvent>(run.events) == 5);
}

TEST_CASE("bounded parallel before-hook failure starts no workers", "[agent][async][u8]") {
    FakeStreamingClient client;
    client.responses.push_back(two_tool_call_response());

    ConcurrencyProbe probe;
    agent::AsyncToolRegistry registry;
    REQUIRE(registry.add(std::make_unique<ProbedFakeTool>(
        ai::Tool{"alpha", "Alpha", test::empty_object_tool_argument_contract()},
        agent::ToolConcurrency::ParallelSafe,
        probe)));
    REQUIRE(registry.add(std::make_unique<ProbedFakeTool>(
        ai::Tool{"beta", "Beta", test::empty_object_tool_argument_contract()},
        agent::ToolConcurrency::ParallelSafe,
        probe)));

    agent::AsyncAgentOptions options{4, "gpt-test"};
    options.tool_execution = agent::BoundedParallelToolExecution{2};
    options.before_tool_call = [](const agent::BeforeToolCallContext&)
        -> util::Expected<agent::BeforeToolCallResult> {
        return std::unexpected(util::make_error(util::ErrorCode::Tool, "preflight failed"));
    };

    agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));
    auto run = run_loop_on_pool(loop, "read");

    REQUIRE_FALSE(run.result);
    CHECK(run.result.error().message == "preflight failed");
    CHECK(probe.max_active.load() == 0);
    CHECK(count_events<agent::AgentEndEvent>(run.events) == 1);
    CHECK(count_events<agent::MessageStartEvent>(run.events) == 2);
    CHECK(count_events<agent::MessageEndEvent>(run.events) == 2);
}

TEST_CASE("bounded parallel execution preserves peer success after a tool error", "[agent][async][u8]") {
    FakeStreamingClient client;
    client.responses.push_back(two_tool_call_response());
    client.responses.push_back(ai::assistant_text_message("done"));

    agent::AsyncToolRegistry registry;
    REQUIRE(registry.add(std::make_unique<FailingFakeTool>(
        ai::Tool{"alpha", "Alpha", test::empty_object_tool_argument_contract()})));
    REQUIRE(registry.add(std::make_unique<ConfigurableFakeTool>(
        ai::Tool{"beta", "Beta", test::empty_object_tool_argument_contract()},
        agent::ToolConcurrency::ParallelSafe,
        "beta result")));

    agent::AsyncAgentOptions options{4, "gpt-test"};
    options.tool_execution = agent::BoundedParallelToolExecution{2};

    agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));
    auto run = run_loop_on_pool(loop, "read");

    REQUIRE(run.result);
    REQUIRE(client.requests.size() == 2);
    const auto& alpha_result = std::get<ai::ToolResultMessage>(client.requests[1].context.messages[2]);
    const auto& beta_result = std::get<ai::ToolResultMessage>(client.requests[1].context.messages[3]);
    CHECK(alpha_result.is_error);
    CHECK_FALSE(beta_result.is_error);
    CHECK(count_events<agent::MessageStartEvent>(run.events) == 5);
    CHECK(count_events<agent::MessageEndEvent>(run.events) == 5);
}

TEST_CASE("bounded parallel event-sink failure drains workers and emits one agent end", "[agent][async][u8]") {
    FakeStreamingClient client;
    client.responses.push_back(two_tool_call_response());

    agent::AsyncToolRegistry registry;
    REQUIRE(registry.add(std::make_unique<ConfigurableFakeTool>(
        ai::Tool{"alpha", "Alpha", test::empty_object_tool_argument_contract()},
        agent::ToolConcurrency::ParallelSafe,
        "alpha result")));
    REQUIRE(registry.add(std::make_unique<ConfigurableFakeTool>(
        ai::Tool{"beta", "Beta", test::empty_object_tool_argument_contract()},
        agent::ToolConcurrency::ParallelSafe,
        "beta result")));

    agent::AsyncAgentOptions options{4, "gpt-test"};
    options.tool_execution = agent::BoundedParallelToolExecution{2};
    agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));

    boost::asio::thread_pool pool{4};
    std::optional<util::Expected<agent::AsyncAgentRunResult>> result;
    std::atomic<int> agent_end_events{0};
    std::vector<agent::AgentLifecycleEvent> events;
    std::mutex events_mutex;
    boost::asio::co_spawn(
        pool,
        [&]() -> boost::asio::awaitable<void> {
            result = co_await loop.run(
                "read",
                [&](const agent::AgentLifecycleEvent& event) -> util::ExpectedVoid {
                    {
                        std::lock_guard lock(events_mutex);
                        events.push_back(event);
                    }
                    if (std::holds_alternative<agent::ToolExecutionEndEvent>(event)) {
                        throw std::runtime_error("sink boom");
                    }
                    if (std::holds_alternative<agent::AgentEndEvent>(event)) {
                        ++agent_end_events;
                    }
                    return {};
                });
            co_return;
        },
        boost::asio::detached);
    pool.join();

    REQUIRE(result.has_value());
    REQUIRE_FALSE(*result);
    CHECK(result->error().message == "agent event sink failed");
    CHECK(agent_end_events.load() == 1);
    CHECK(count_events<agent::MessageStartEvent>(events) == 2);
    CHECK(count_events<agent::MessageEndEvent>(events) == 2);
}

TEST_CASE("bounded parallel execution keeps hook failures as agent errors", "[agent][async][u8]") {
    FakeStreamingClient client;
    client.responses.push_back(two_tool_call_response());

    agent::AsyncToolRegistry registry;
    REQUIRE(registry.add(std::make_unique<ConfigurableFakeTool>(
        ai::Tool{"alpha", "Alpha", test::empty_object_tool_argument_contract()},
        agent::ToolConcurrency::ParallelSafe,
        "alpha result")));
    REQUIRE(registry.add(std::make_unique<ConfigurableFakeTool>(
        ai::Tool{"beta", "Beta", test::empty_object_tool_argument_contract()},
        agent::ToolConcurrency::ParallelSafe,
        "beta result")));

    agent::AsyncAgentOptions options{4, "gpt-test"};
    options.tool_execution = agent::BoundedParallelToolExecution{2};
    options.after_tool_call = [](const agent::AfterToolCallContext& context)
        -> util::Expected<agent::AfterToolCallResult> {
        if (context.tool_call.name == "alpha") {
            return std::unexpected(util::make_error(util::ErrorCode::Tool, "post-processor failed"));
        }
        return agent::AfterToolCallResult{};
    };

    agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));
    auto run = run_loop_on_pool(loop, "read");

    REQUIRE_FALSE(run.result);
    CHECK(run.result.error().code == util::ErrorCode::Tool);
    CHECK(run.result.error().message == "post-processor failed");
    CHECK(count_events<agent::AgentEndEvent>(run.events) == 1);
    CHECK(count_events<agent::MessageStartEvent>(run.events) == 2);
    CHECK(count_events<agent::MessageEndEvent>(run.events) == 2);
}

TEST_CASE("bounded parallel execution emits end events in completion order", "[agent][async][u8]") {
    FakeStreamingClient client;
    client.responses.push_back(two_tool_call_response());
    client.responses.push_back(ai::assistant_text_message("done"));

    agent::AsyncToolRegistry registry;
    REQUIRE(registry.add(std::make_unique<DelayedFakeTool>(
        ai::Tool{"alpha", "Alpha", test::empty_object_tool_argument_contract()},
        std::chrono::milliseconds{100},
        "alpha result")));
    REQUIRE(registry.add(std::make_unique<DelayedFakeTool>(
        ai::Tool{"beta", "Beta", test::empty_object_tool_argument_contract()},
        std::chrono::milliseconds{10},
        "beta result")));

    agent::AsyncAgentOptions options{4, "gpt-test"};
    options.tool_execution = agent::BoundedParallelToolExecution{2};

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
    CHECK(count_events<agent::MessageStartEvent>(run.events) == 5);
    CHECK(count_events<agent::MessageEndEvent>(run.events) == 5);
}

TEST_CASE("length-truncated tool calls emit errors without crossing the executor seam", "[agent][async][u8]") {
    FakeStreamingClient client;
    auto truncated = two_tool_call_response();
    truncated.stop_reason = ai::AssistantStopReason::Length;
    client.responses.push_back(std::move(truncated));
    client.responses.push_back(ai::assistant_text_message("recovered"));

    agent::AsyncToolRegistry registry;
    auto alpha = std::make_unique<ConfigurableFakeTool>(
        ai::Tool{"alpha", "Alpha", test::empty_object_tool_argument_contract()},
        agent::ToolConcurrency::ParallelSafe,
        "alpha result");
    auto beta = std::make_unique<ConfigurableFakeTool>(
        ai::Tool{"beta", "Beta", test::empty_object_tool_argument_contract()},
        agent::ToolConcurrency::ParallelSafe,
        "beta result");
    auto* alpha_ptr = alpha.get();
    auto* beta_ptr = beta.get();
    REQUIRE(registry.add(std::move(alpha)));
    REQUIRE(registry.add(std::move(beta)));

    int before_calls = 0;
    int after_calls = 0;
    agent::AsyncAgentOptions options{4, "gpt-test"};
    options.tool_execution = agent::BoundedParallelToolExecution{2};
    options.before_tool_call = [&](const agent::BeforeToolCallContext&)
        -> util::Expected<agent::BeforeToolCallResult> {
        ++before_calls;
        return agent::BeforeToolCallResult{};
    };
    options.after_tool_call = [&](const agent::AfterToolCallContext&)
        -> util::Expected<agent::AfterToolCallResult> {
        ++after_calls;
        return agent::AfterToolCallResult{};
    };

    agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));
    auto run = run_loop_on_pool(loop, "read");

    REQUIRE(run.result);
    CHECK(run.result->turns == 2);
    CHECK(alpha_ptr->invocations.empty());
    CHECK(beta_ptr->invocations.empty());
    CHECK(before_calls == 0);
    CHECK(after_calls == 0);
    CHECK(count_events<agent::ToolExecutionStartEvent>(run.events) == 2);
    CHECK(count_events<agent::ToolExecutionEndEvent>(run.events) == 2);
    CHECK(count_events<agent::MessageStartEvent>(run.events) == 5);
    CHECK(count_events<agent::MessageEndEvent>(run.events) == 5);

    REQUIRE(client.requests.size() == 2);
    const auto& messages = client.requests[1].context.messages;
    REQUIRE(messages.size() == 4);
    const auto& alpha_result = std::get<ai::ToolResultMessage>(messages[2]);
    const auto& beta_result = std::get<ai::ToolResultMessage>(messages[3]);
    CHECK(alpha_result.is_error);
    CHECK(beta_result.is_error);
    CHECK(alpha_result.tool_name == "alpha");
    CHECK(beta_result.tool_name == "beta");
    CHECK(ai::text_from_content(alpha_result.content).find("output token limit") != std::string::npos);
    CHECK(ai::text_from_content(alpha_result.content).find(R"({"x":1})") == std::string::npos);
    CHECK(run.result->state.pending_tool_call_ids.empty());
    CHECK(run.result->state.active_tool_names.empty());
}

TEST_CASE("tool scheduling vocabulary belongs to cch::agent", "[agent][async][u8]") {
    static_assert(std::is_enum_v<agent::ToolConcurrency>);
    static_assert(std::is_same_v<
        agent::ToolExecutionPolicy,
        std::variant<agent::SequentialToolExecution, agent::BoundedParallelToolExecution>>);
}
