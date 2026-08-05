#include <cch/ai/Content.hpp>
#include <cch/util/Error.hpp>
#include "agent/AgentLoop.hpp"
#include "support/ModelFixture.hpp"
#include "support/ToolArgumentContracts.hpp"
#include "util/ExpectedMacros.hpp"
#include "util/Json.hpp"

#include "../../third_party/catch2/catch_test_macros.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
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

class CancellationAwarePolicyClient final : public ai::StreamingChatClient {
public:
    boost::asio::awaitable<util::Expected<ai::AssistantMessage>> stream(
        const ai::StreamChatRequest& request,
        ai::AssistantEventSink sink) override {
        requests.push_back(request);
        if (request.stop_token.stop_requested()) {
            auto terminal = ai::assistant_text_message("");
            terminal.stop_reason = ai::AssistantStopReason::Aborted;
            terminal.error_message = "prompt aborted by policy";
            CCH_TRY_VOID(sink(ai::AssistantErrorEvent{
                .reason = ai::AssistantStopReason::Aborted,
                .error = terminal,
            }));
            co_return terminal;
        }

        if (responses.empty()) {
            co_return std::unexpected(util::make_error(
                util::ErrorCode::Provider,
                "no scripted policy response"));
        }
        auto response = responses.front();
        responses.pop_front();
        CCH_TRY_VOID(sink(ai::AssistantStartEvent{.partial = response}));
        for (std::size_t index = 0; index < response.content.size(); ++index) {
            if (const auto* call = std::get_if<ai::ToolCallContent>(&response.content[index])) {
                CCH_TRY_VOID(sink(ai::ToolCallStartEvent{
                    .content_index = index,
                    .partial = response,
                }));
                CCH_TRY_VOID(sink(ai::ToolCallDeltaEvent{
                    .content_index = index,
                    .delta = call->raw_arguments,
                    .partial = response,
                }));
                CCH_TRY_VOID(sink(ai::ToolCallEndEvent{
                    .content_index = index,
                    .tool_call = *call,
                    .partial = response,
                }));
            }
        }
        CCH_TRY_VOID(sink(ai::AssistantDoneEvent{
            .reason = response.stop_reason,
            .message = response,
        }));
        co_return response;
    }

    std::deque<ai::AssistantMessage> responses;
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
        agent::ToolInvocation invocation,
        std::stop_token) override {
        invocations.push_back(invocation);
        co_return agent::AsyncToolExecutionResult{
            .content = std::vector<ai::Content>{ai::text_content("tool says ok")},
            .details = std::nullopt,
            .is_error = false};
    }

    ai::Tool definition_;
    std::vector<agent::ToolInvocation> invocations;
};

class CancellableFakeTool final : public agent::AsyncAgentTool {
public:
    explicit CancellableFakeTool(ai::Tool definition) : definition_(std::move(definition)) {}

    const ai::Tool& definition() const override { return definition_; }

    boost::asio::awaitable<util::Expected<agent::AsyncToolExecutionResult>> execute(
        agent::ToolInvocation invocation,
        std::stop_token stop_token) override {
        invocations.push_back(std::move(invocation));
        observed_stop_token = stop_token;
        timer.emplace(co_await boost::asio::this_coro::executor);
        timer->expires_at(std::chrono::steady_clock::time_point::max());
        std::stop_callback cancellation{stop_token, [this] { timer->cancel(); }};
        boost::system::error_code error;
        co_await timer->async_wait(boost::asio::redirect_error(
            boost::asio::use_awaitable,
            error));
        if (stop_token.stop_requested()) {
            co_return std::unexpected(util::make_error(
                util::ErrorCode::Cancelled,
                "Operation aborted"));
        }
        co_return agent::AsyncToolExecutionResult{};
    }

    ai::Tool definition_;
    std::vector<agent::ToolInvocation> invocations;
    std::optional<std::stop_token> observed_stop_token;
    std::optional<boost::asio::steady_timer> timer;
};

struct RunResult {
    util::Expected<agent::AsyncAgentRunResult> result;
    std::vector<agent::AgentLifecycleEvent> events;
};

RunResult run_loop(
    agent::AsyncAgentLoop& loop,
    std::string prompt,
    std::vector<ai::MessageVariant> history = {},
    std::stop_token stop_token = {}) {
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
                },
                stop_token);
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

    auto zed = std::make_unique<FakeTool>(ai::Tool{"zed", "Zed tool", test::permissive_object_tool_argument_contract()});
    auto alpha = std::make_unique<FakeTool>(ai::Tool{"alpha", "Alpha tool", test::permissive_object_tool_argument_contract()});
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
    agent::AsyncAgentOptions options;
    options.max_turns = 3;
    options.model = tests::make_model("gpt-test");
    agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));

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
    "async agent loop synthesizes the assistant start for a done terminal before any start",
    "[agent][async][issue15]") {
    auto terminal = ai::assistant_text_message("host reply without a streamed start");
    terminal.api = "host-api";
    terminal.provider = "host-provider";
    terminal.model = "gpt-test";
    TerminalBeforeStartClient client(std::move(terminal));

    agent::AsyncToolRegistry registry;
    agent::AsyncAgentOptions options;
    options.max_turns = 3;
    options.model = tests::make_model("gpt-test");
    agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));
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
    agent::AsyncAgentOptions options;
    options.max_turns = 3;
    options.model = tests::make_model("gpt-test");
    agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));
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
    agent::AsyncAgentOptions options;
    options.max_turns = 3;
    options.model = tests::make_model("gpt-test");
    agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));
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
    agent::AsyncAgentOptions options;
    options.max_turns = 3;
    options.model = tests::make_model("gpt-test");
    agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));

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
    CHECK(ai::text_from_user_message(std::get<ai::UserMessage>(user_start->message)) == "hi");

    const auto* user_end = std::get_if<agent::MessageEndEvent>(&run.events[index++]);
    REQUIRE(user_end);
    REQUIRE(std::holds_alternative<ai::UserMessage>(user_end->message));
    CHECK(ai::text_from_user_message(std::get<ai::UserMessage>(user_end->message)) == "hi");

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
    CHECK(ai::text_from_user_message(std::get<ai::UserMessage>(agent_end->messages[0])) == "hi");
    CHECK(ai::text_from_assistant_content(std::get<ai::AssistantMessage>(agent_end->messages[1]).content) == "hello user");

    CHECK(index == run.events.size());
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
    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));

    auto run = run_loop(loop, "read");

    REQUIRE(run.result);
    CHECK(count_events<agent::MessageUpdateEvent>(run.events) >= 1);
    CHECK(count_events<agent::MessageStartEvent>(run.events) == 4);
    CHECK(count_events<agent::MessageEndEvent>(run.events) == 4);
    CHECK(run.result->state.model.id == "gpt-test");
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
    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));

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
    REQUIRE(registry.add(std::make_unique<FakeTool>(ai::Tool{"read_file", "Read", test::permissive_object_tool_argument_contract()})));
    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));

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

TEST_CASE("async agent loop default options impose no turn cap", "[agent][async][u5][issue68]") {
    FakeStreamingClient client;
    for (int turn = 0; turn < 9; ++turn) {
        client.responses.push_back(tool_call_response());
    }
    client.responses.push_back(ai::assistant_text_message("done"));
    agent::AsyncToolRegistry registry;
    REQUIRE(registry.add(std::make_unique<FakeTool>(ai::Tool{"read_file", "Read", test::permissive_object_tool_argument_contract()})));
    agent::AsyncAgentOptions options;
    options.model = tests::make_model("gpt-test");
    agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));

    auto run = run_loop(loop, "read");

    REQUIRE(run.result);
    CHECK(run.result->turns == 10);
    CHECK(run.result->stop_reason == ai::AssistantStopReason::Stop);
    CHECK(count_events<agent::AgentEndEvent>(run.events) == 1);
}

TEST_CASE("async agent loop enforces an explicit host-set turn cap", "[agent][async][u5][issue68]") {
    FakeStreamingClient client;
    client.responses.push_back(tool_call_response());
    agent::AsyncToolRegistry registry;
    REQUIRE(registry.add(std::make_unique<FakeTool>(ai::Tool{"read_file", "Read", test::permissive_object_tool_argument_contract()})));
    agent::AsyncAgentOptions options;
    options.max_turns = 1;
    options.model = tests::make_model("gpt-test");
    agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));

    auto run = run_loop(loop, "read");

    REQUIRE_FALSE(run.result);
    CHECK(run.result.error().code == util::ErrorCode::Validation);
    CHECK(run.result.error().message == "max turns exceeded");
    CHECK(count_events<agent::AgentEndEvent>(run.events) == 1);
}

TEST_CASE("agent queue bounds are documented configuration fields", "[agent][async][issue68]") {
    const agent::AsyncAgentOptions options;
    CHECK(options.max_queued_messages == 256);
    CHECK(options.max_queued_bytes == 16 * 1024 * 1024);
}

















TEST_CASE("beforeToolCall hook can block a tool call", "[agent][async][u7]") {
    FakeStreamingClient client;
    client.responses.push_back(tool_call_response());
    client.responses.push_back(ai::assistant_text_message("done"));

    auto tool = std::make_unique<FakeTool>(ai::Tool{"read_file", "Read", test::permissive_object_tool_argument_contract()});
    auto* tool_ptr = tool.get();
    agent::AsyncToolRegistry registry;
    REQUIRE(registry.add(std::move(tool)));

    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    options.before_tool_call =
        agent::adapt_sync_before_tool_call(
            [](const agent::BeforeToolCallContext&) -> util::Expected<agent::BeforeToolCallResult> {
        return agent::BeforeToolCallResult{true, "blocked by policy"};
    });

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

    auto tool = std::make_unique<FakeTool>(ai::Tool{"read_file", "Read", test::permissive_object_tool_argument_contract()});
    auto* tool_ptr = tool.get();
    agent::AsyncToolRegistry registry;
    REQUIRE(registry.add(std::move(tool)));

    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    options.before_tool_call =
        agent::adapt_sync_before_tool_call(
            [](const agent::BeforeToolCallContext& ctx) -> util::Expected<agent::BeforeToolCallResult> {
        REQUIRE(ctx.tool_call.name == "read_file");
        REQUIRE(ctx.args.get<util::JsonValue::object_t>().at("path").get_string() == "README.md");
        REQUIRE(!ctx.context.messages.empty());
        return agent::BeforeToolCallResult{false, std::nullopt};
    });

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
    REQUIRE(registry.add(std::make_unique<FakeTool>(ai::Tool{"read_file", "Read", test::permissive_object_tool_argument_contract()})));

    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    options.before_tool_call =
        agent::adapt_sync_before_tool_call(
            [](const agent::BeforeToolCallContext&) -> util::Expected<agent::BeforeToolCallResult> {
        return std::unexpected(util::make_error(util::ErrorCode::Tool, "policy rejected"));
    });

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
    REQUIRE(registry.add(std::make_unique<FakeTool>(ai::Tool{"read_file", "Read", test::permissive_object_tool_argument_contract()})));

    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    options.before_tool_call =
        agent::adapt_sync_before_tool_call(
            [](const agent::BeforeToolCallContext&) -> util::Expected<agent::BeforeToolCallResult> {
        throw std::runtime_error("boom");
    });

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
    REQUIRE(registry.add(std::make_unique<FakeTool>(ai::Tool{"read_file", "Read", test::permissive_object_tool_argument_contract()})));

    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    options.after_tool_call =
        agent::adapt_sync_after_tool_call(
            [](const agent::AfterToolCallContext&) -> util::Expected<agent::AfterToolCallResult> {
        return agent::AfterToolCallResult{
            std::vector<ai::Content>{ai::text_content("overridden")}, std::nullopt, std::nullopt, std::nullopt};
    });

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
    REQUIRE(registry.add(std::make_unique<FakeTool>(ai::Tool{"read_file", "Read", test::permissive_object_tool_argument_contract()})));

    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    options.after_tool_call =
        agent::adapt_sync_after_tool_call(
            [](const agent::AfterToolCallContext&) -> util::Expected<agent::AfterToolCallResult> {
        return agent::AfterToolCallResult{std::nullopt, std::nullopt, true, std::nullopt};
    });

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

TEST_CASE(
    "afterToolCall terminate hint stops automatic continuation",
    "[agent][async][issue35]") {
    FakeStreamingClient client;
    client.responses.push_back(tool_call_response());
    agent::AsyncToolRegistry registry;
    REQUIRE(registry.add(std::make_unique<FakeTool>(
        ai::Tool{"read_file", "Read", test::permissive_object_tool_argument_contract()})));

    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    options.after_tool_call =
        agent::adapt_sync_after_tool_call(
            [](const agent::AfterToolCallContext&) -> util::Expected<agent::AfterToolCallResult> {
        return agent::AfterToolCallResult{std::nullopt, std::nullopt, std::nullopt, true};
    });

    agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));
    auto run = run_loop(loop, "read");

    REQUIRE(run.result);
    CHECK(run.result->turns == 1);
    CHECK(run.result->stop_reason == ai::AssistantStopReason::ToolUse);
    CHECK(client.requests.size() == 1);
    CHECK(count_events<agent::AgentEndEvent>(run.events) == 1);
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
        agent::ToolInvocation invocation,
        std::stop_token) override {
        invocations.push_back(invocation);
        co_return agent::AsyncToolExecutionResult{
            .content = std::vector<ai::Content>{ai::text_content(result_text_)},
            .details = std::nullopt,
            .is_error = false};
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
        agent::ToolInvocation invocation,
        std::stop_token) override {
        auto timer = boost::asio::steady_timer(co_await boost::asio::this_coro::executor, delay_);
        co_await timer.async_wait(boost::asio::use_awaitable);
        invocations.push_back(invocation);
        co_return agent::AsyncToolExecutionResult{
            .content = std::vector<ai::Content>{ai::text_content(result_text_)},
            .details = std::nullopt,
            .is_error = false};
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
        agent::ToolInvocation invocation,
        std::stop_token) override {
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
        agent::ToolInvocation invocation,
        std::stop_token) override {
        invocations.push_back(invocation);
        const int current = ++probe_.active;
        int observed = probe_.max_active.load();
        while (current > observed && !probe_.max_active.compare_exchange_weak(observed, current)) {}
        auto timer = boost::asio::steady_timer(co_await boost::asio::this_coro::executor, std::chrono::milliseconds{30});
        co_await timer.async_wait(boost::asio::use_awaitable);
        --probe_.active;
        co_return agent::AsyncToolExecutionResult{
            .content = std::vector<ai::Content>{ai::text_content(definition_.name + " result")},
            .details = std::nullopt,
            .is_error = false};
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



TEST_CASE("terminate batch continues when one call declines", "[agent][async][u7]") {
    FakeStreamingClient client;
    client.responses.push_back(two_tool_call_response());
    client.responses.push_back(ai::assistant_text_message("done"));

    agent::AsyncToolRegistry registry;
    REQUIRE(registry.add(std::make_unique<FakeTool>(ai::Tool{"alpha", "Alpha", test::permissive_object_tool_argument_contract()})));
    REQUIRE(registry.add(std::make_unique<FakeTool>(ai::Tool{"beta", "Beta", test::permissive_object_tool_argument_contract()})));

    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    options.after_tool_call =
        agent::adapt_sync_after_tool_call(
            [](const agent::AfterToolCallContext& ctx) -> util::Expected<agent::AfterToolCallResult> {
        if (ctx.tool_call.name == "alpha") {
            return agent::AfterToolCallResult{std::nullopt, std::nullopt, std::nullopt, true};
        }
        return agent::AfterToolCallResult{std::nullopt, std::nullopt, std::nullopt, false};
    });

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
    REQUIRE(registry.add(std::make_unique<FakeTool>(ai::Tool{"alpha", "Alpha", test::permissive_object_tool_argument_contract()})));
    REQUIRE(registry.add(std::make_unique<FakeTool>(ai::Tool{"beta", "Beta", test::permissive_object_tool_argument_contract()})));

    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    options.before_tool_call =
        agent::adapt_sync_before_tool_call(
            [](const agent::BeforeToolCallContext& ctx) -> util::Expected<agent::BeforeToolCallResult> {
        if (ctx.tool_call.name == "alpha") {
            return agent::BeforeToolCallResult{true, "no alpha"};
        }
        return agent::BeforeToolCallResult{false, std::nullopt};
    });
    options.after_tool_call =
        agent::adapt_sync_after_tool_call(
            [](const agent::AfterToolCallContext&) -> util::Expected<agent::AfterToolCallResult> {
        return agent::AfterToolCallResult{std::nullopt, std::nullopt, std::nullopt, true};
    });

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
    REQUIRE(registry.add(std::make_unique<FakeTool>(ai::Tool{"alpha", "Alpha", test::permissive_object_tool_argument_contract()})));
    REQUIRE(registry.add(std::make_unique<FakeTool>(ai::Tool{"beta", "Beta", test::permissive_object_tool_argument_contract()})));

    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    options.after_tool_call =
        agent::adapt_sync_after_tool_call(
            [](const agent::AfterToolCallContext& ctx) -> util::Expected<agent::AfterToolCallResult> {
        if (ctx.tool_call.name == "alpha") {
            return agent::AfterToolCallResult{std::nullopt, std::nullopt, true, true};
        }
        return agent::AfterToolCallResult{std::nullopt, std::nullopt, std::nullopt, true};
    });

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
    REQUIRE(registry.add(std::make_unique<FakeTool>(ai::Tool{"read_file", "Read", test::permissive_object_tool_argument_contract()})));

    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    options.after_tool_call =
        agent::adapt_sync_after_tool_call(
            [](const agent::AfterToolCallContext&) -> util::Expected<agent::AfterToolCallResult> {
        return std::unexpected(util::make_error(util::ErrorCode::Tool, "post-processor failed"));
    });

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
    REQUIRE(registry.add(std::make_unique<FakeTool>(ai::Tool{"read_file", "Read", test::permissive_object_tool_argument_contract()})));

    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    options.after_tool_call =
        agent::adapt_sync_after_tool_call(
            [](const agent::AfterToolCallContext&) -> util::Expected<agent::AfterToolCallResult> {
        throw std::runtime_error("after boom");
    });

    agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));
    auto run = run_loop(loop, "read");

    REQUIRE_FALSE(run.result);
    CHECK(run.result.error().code == util::ErrorCode::Tool);
    CHECK(run.result.error().detail.find("after boom") != std::string::npos);
    CHECK(count_events<agent::MessageStartEvent>(run.events) == 2);
    CHECK(count_events<agent::MessageEndEvent>(run.events) == 2);
}

TEST_CASE("AsyncAgentOptions hooks are move-only", "[agent][async][u7][issue82]") {
    static_assert(!std::is_copy_constructible_v<agent::AsyncAgentOptions>);
    static_assert(!std::is_copy_assignable_v<agent::AsyncAgentOptions>);
    static_assert(std::is_move_constructible_v<agent::AsyncAgentOptions>);
    static_assert(!std::is_copy_constructible_v<agent::TransformContextHook>);
    static_assert(!std::is_copy_constructible_v<agent::BeforeToolCallHook>);
    static_assert(!std::is_copy_constructible_v<agent::AfterToolCallHook>);
}

TEST_CASE(
    "awaitable context policies suspend and resume in Agent executor order",
    "[agent][async][issue82]") {
    boost::asio::io_context io;
    FakeStreamingClient client;
    client.responses.push_back(ai::assistant_text_message("ok"));
    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");

    std::vector<std::string> ordering;
    bool resumed_on_same_executor = false;
    bool stop_requested_after_resume = false;
    std::optional<boost::asio::steady_timer> gate;
    options.transform_context = [owned = std::make_unique<int>(41),
                                 &ordering,
                                 &resumed_on_same_executor,
                                 &stop_requested_after_resume,
                                 &gate](std::vector<ai::MessageVariant> messages,
                                        std::stop_token stop_token)
        -> boost::asio::awaitable<util::Expected<std::vector<ai::MessageVariant>>> {
        const auto executor = co_await boost::asio::this_coro::executor;
        CHECK(*owned == 41);
        ordering.push_back("transform-start");
        gate.emplace(executor);
        gate->expires_at(std::chrono::steady_clock::time_point::max());
        boost::system::error_code error;
        co_await gate->async_wait(boost::asio::redirect_error(
            boost::asio::use_awaitable, error));
        const auto resumed_executor = co_await boost::asio::this_coro::executor;
        resumed_on_same_executor = resumed_executor == executor;
        stop_requested_after_resume = stop_token.stop_requested();
        ordering.push_back("transform-resume");
        co_return messages;
    };
    options.convert_to_llm = [&ordering](std::vector<ai::MessageVariant> messages)
        -> boost::asio::awaitable<util::Expected<std::vector<ai::MessageVariant>>> {
        ordering.push_back("convert");
        co_return messages;
    };

    agent::AsyncAgentLoop loop(client, agent::AsyncToolRegistry{}, std::move(options));
    std::stop_source stop_source;
    std::optional<util::Expected<agent::AsyncAgentRunResult>> result;
    boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<void> {
            result = co_await loop.continue_with({}, "hello", {}, stop_source.get_token());
            co_return;
        },
        boost::asio::detached);

    while (!gate) {
        REQUIRE(io.run_one() == 1);
    }
    CHECK_FALSE(result.has_value());
    const std::vector<std::string> suspended{"transform-start"};
    CHECK(ordering == suspended);

    REQUIRE(stop_source.request_stop());
    gate->cancel();
    io.run();

    REQUIRE(result.has_value());
    REQUIRE(*result);
    CHECK(resumed_on_same_executor);
    CHECK(stop_requested_after_resume);
    const std::vector<std::string> expected{
        "transform-start", "transform-resume", "convert"};
    CHECK(ordering == expected);
}

TEST_CASE(
    "awaitable signal-bearing policies receive the active run stop token",
    "[agent][async][issue39][issue82]") {
    FakeStreamingClient client;
    client.responses.push_back(tool_call_response());
    client.responses.push_back(ai::assistant_text_message("done"));
    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");

    bool transform_stop_possible = false;
    bool before_stop_possible = false;
    bool after_stop_possible = false;
    options.transform_context =
        agent::adapt_sync_transform_context(
            [&transform_stop_possible](std::vector<ai::MessageVariant> messages,
                                       std::stop_token stop_token)
                -> util::Expected<std::vector<ai::MessageVariant>> {
                transform_stop_possible = stop_token.stop_possible();
                return messages;
            });
    options.before_tool_call =
        agent::adapt_sync_before_tool_call(
            [&before_stop_possible](agent::BeforeToolCallContext,
                                    std::stop_token stop_token)
                -> util::Expected<agent::BeforeToolCallResult> {
                before_stop_possible = stop_token.stop_possible();
                return agent::BeforeToolCallResult{};
            });
    options.after_tool_call =
        agent::adapt_sync_after_tool_call(
            [&after_stop_possible](agent::AfterToolCallContext,
                                   std::stop_token stop_token)
                -> util::Expected<agent::AfterToolCallResult> {
                after_stop_possible = stop_token.stop_possible();
                return agent::AfterToolCallResult{};
            });

    agent::AsyncToolRegistry registry;
    REQUIRE(registry.add(std::make_unique<FakeTool>(ai::Tool{
        "read_file", "Read", test::permissive_object_tool_argument_contract()})));
    agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));
    std::stop_source stop_source;
    auto run = run_loop(loop, "read", {}, stop_source.get_token());

    REQUIRE(run.result);
    CHECK(run.result->stop_reason == ai::AssistantStopReason::Stop);
    CHECK(transform_stop_possible);
    CHECK(before_stop_possible);
    CHECK(after_stop_possible);
    CHECK(count_events<agent::AgentEndEvent>(run.events) == 1);
}

TEST_CASE(
    "cancellation reaches a suspended tool and completes one ordinary aborted lifecycle",
    "[agent][async][abort][issue40]") {
    boost::asio::io_context io;
    CancellationAwarePolicyClient client;
    client.responses.push_back(tool_call_response());
    agent::AsyncAgentOptions options;
    options.model = tests::make_model("gpt-test");

    auto tool = std::make_unique<CancellableFakeTool>(ai::Tool{
        .name = "read_file",
        .description = "Read",
        .parameters = test::permissive_object_tool_argument_contract(),
    });
    auto* tool_ptr = tool.get();
    agent::AsyncToolRegistry registry;
    REQUIRE(registry.add(std::move(tool)));
    agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));

    std::stop_source stop_source;
    std::optional<util::Expected<agent::AsyncAgentRunResult>> result;
    std::vector<agent::AgentLifecycleEvent> events;
    boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<void> {
            result = co_await loop.continue_with(
                {},
                "cancel active tool",
                [&](const agent::AgentLifecycleEvent& event) {
                    events.push_back(event);
                    return util::ExpectedVoid{};
                },
                stop_source.get_token());
            co_return;
        },
        boost::asio::detached);

    while (!tool_ptr->timer) {
        REQUIRE(io.run_one() == 1);
    }
    REQUIRE(stop_source.request_stop());
    io.run();

    REQUIRE(result.has_value());
    REQUIRE(*result);
    CHECK((*result)->stop_reason == ai::AssistantStopReason::Aborted);
    REQUIRE(tool_ptr->observed_stop_token.has_value());
    CHECK(*tool_ptr->observed_stop_token == stop_source.get_token());
    CHECK(tool_ptr->observed_stop_token->stop_requested());
    CHECK(tool_ptr->invocations.size() == 1);
    REQUIRE(client.requests.size() == 2);
    CHECK(client.requests[1].stop_token.stop_requested());
    CHECK(count_events<agent::AgentEndEvent>(events) == 1);
}

TEST_CASE(
    "transform policy cancellation completes through an aborted provider turn",
    "[agent][async][abort][issue39]") {
    CancellationAwarePolicyClient client;
    agent::AsyncAgentOptions options;
    options.model = tests::make_model("gpt-test");
    std::stop_source stop_source;
    options.transform_context = [&stop_source](
                                    std::vector<ai::MessageVariant>,
                                    std::stop_token)
        -> boost::asio::awaitable<util::Expected<std::vector<ai::MessageVariant>>> {
        (void)stop_source.request_stop();
        throw std::runtime_error("transform policy cancelled");
        co_return std::unexpected(util::make_error(
            util::ErrorCode::Cancelled,
            "unreachable transform result"));
    };

    agent::AsyncAgentLoop loop(client, agent::AsyncToolRegistry{}, std::move(options));
    auto run = run_loop(loop, "cancel transform", {}, stop_source.get_token());

    REQUIRE(run.result.has_value());
    CHECK(run.result->stop_reason == ai::AssistantStopReason::Aborted);
    REQUIRE(client.requests.size() == 1);
    CHECK(client.requests[0].stop_token.stop_requested());
    CHECK(count_events<agent::MessageEndEvent>(run.events) == 2);
    CHECK(count_events<agent::TurnEndEvent>(run.events) == 1);
    CHECK(count_events<agent::AgentEndEvent>(run.events) == 1);
}

TEST_CASE(
    "before-tool policy cancellation skips the tool and reaches an aborted provider turn",
    "[agent][async][abort][issue39][issue40]") {
    CancellationAwarePolicyClient client;
    client.responses.push_back(tool_call_response());
    std::stop_source stop_source;
    agent::AsyncAgentOptions options;
    options.max_turns = 1;
    options.model = tests::make_model("gpt-test");
    options.before_tool_call = [&stop_source](agent::BeforeToolCallContext, std::stop_token)
        -> boost::asio::awaitable<util::Expected<agent::BeforeToolCallResult>> {
        (void)stop_source.request_stop();
        co_return std::unexpected(util::make_error(
            util::ErrorCode::Cancelled,
            "before-tool policy cancelled"));
    };

    auto tool = std::make_unique<FakeTool>(ai::Tool{
        .name = "read_file",
        .description = "Read",
        .parameters = test::permissive_object_tool_argument_contract(),
    });
    auto* tool_ptr = tool.get();
    agent::AsyncToolRegistry registry;
    REQUIRE(registry.add(std::move(tool)));
    agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));
    auto run = run_loop(loop, "cancel before tool", {}, stop_source.get_token());

    REQUIRE(run.result.has_value());
    CHECK(run.result->stop_reason == ai::AssistantStopReason::Aborted);
    CHECK(run.result->turns == 2);
    CHECK(tool_ptr->invocations.empty());
    REQUIRE(client.requests.size() == 2);
    CHECK(client.requests[1].stop_token.stop_requested());
    CHECK(count_events<agent::TurnEndEvent>(run.events) == 2);
    CHECK(count_events<agent::AgentEndEvent>(run.events) == 1);
}

TEST_CASE(
    "after-tool policy cancellation keeps the tool result and reaches an aborted provider turn",
    "[agent][async][abort][issue39][issue40]") {
    CancellationAwarePolicyClient client;
    client.responses.push_back(tool_call_response());
    std::stop_source stop_source;
    agent::AsyncAgentOptions options;
    options.model = tests::make_model("gpt-test");
    options.after_tool_call = [&stop_source](agent::AfterToolCallContext, std::stop_token)
        -> boost::asio::awaitable<util::Expected<agent::AfterToolCallResult>> {
        (void)stop_source.request_stop();
        co_return std::unexpected(util::make_error(
            util::ErrorCode::Cancelled,
            "after-tool policy cancelled"));
    };

    auto tool = std::make_unique<FakeTool>(ai::Tool{
        .name = "read_file",
        .description = "Read",
        .parameters = test::permissive_object_tool_argument_contract(),
    });
    auto* tool_ptr = tool.get();
    agent::AsyncToolRegistry registry;
    REQUIRE(registry.add(std::move(tool)));
    agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));
    auto run = run_loop(loop, "cancel after tool", {}, stop_source.get_token());

    REQUIRE(run.result.has_value());
    CHECK(run.result->stop_reason == ai::AssistantStopReason::Aborted);
    REQUIRE(tool_ptr->invocations.size() == 1);
    REQUIRE(client.requests.size() == 2);
    CHECK(client.requests[1].stop_token.stop_requested());
    CHECK(count_events<agent::TurnEndEvent>(run.events) == 2);
    CHECK(count_events<agent::AgentEndEvent>(run.events) == 1);
}

TEST_CASE(
    "awaitable policy exceptions after suspension stay in the existing error channel",
    "[agent][async][issue82]") {
    FakeStreamingClient client;
    client.responses.push_back(ai::assistant_text_message("unused"));
    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    options.transform_context = [](std::vector<ai::MessageVariant> messages, std::stop_token)
        -> boost::asio::awaitable<util::Expected<std::vector<ai::MessageVariant>>> {
        auto timer = boost::asio::steady_timer(co_await boost::asio::this_coro::executor);
        timer.expires_after(std::chrono::milliseconds{0});
        co_await timer.async_wait(boost::asio::use_awaitable);
        throw std::runtime_error("suspended transform boom");
        co_return messages;
    };

    agent::AsyncAgentLoop loop(client, agent::AsyncToolRegistry{}, std::move(options));
    auto run = run_loop(loop, "hello");

    REQUIRE_FALSE(run.result);
    CHECK(run.result.error().code == util::ErrorCode::Tool);
    CHECK(run.result.error().message == "transformContext hook failed");
    CHECK(run.result.error().detail.find("suspended transform boom") != std::string::npos);
    CHECK(count_events<agent::AgentEndEvent>(run.events) == 1);
}

TEST_CASE("transformContext hook prunes old messages from LLM request", "[agent][async][u8]") {
    FakeStreamingClient client;
    client.responses.push_back(ai::assistant_text_message("ok"));
    agent::AsyncToolRegistry registry;

    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    options.transform_context =
        agent::adapt_sync_transform_context(
            [](const std::vector<ai::MessageVariant>& messages)
        -> util::Expected<std::vector<ai::MessageVariant>> {
        if (messages.size() <= 1) {
            return messages;
        }
        return std::vector<ai::MessageVariant>{messages.back()};
    });

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

    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    options.convert_to_llm =
        agent::adapt_sync_convert_to_llm(
            [](const std::vector<ai::MessageVariant>& messages)
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
    });

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

    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    options.convert_to_llm =
        agent::adapt_sync_convert_to_llm(
            [](const std::vector<ai::MessageVariant>&)
        -> util::Expected<std::vector<ai::MessageVariant>> { return std::vector<ai::MessageVariant>{}; });

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

    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    options.transform_context =
        agent::adapt_sync_transform_context(
            [](const std::vector<ai::MessageVariant>&)
        -> util::Expected<std::vector<ai::MessageVariant>> {
        return std::unexpected(util::make_error(util::ErrorCode::Tool, "context transform failed"));
    });

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

    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    options.convert_to_llm =
        agent::adapt_sync_convert_to_llm(
            [](const std::vector<ai::MessageVariant>&)
        -> util::Expected<std::vector<ai::MessageVariant>> {
        return std::unexpected(util::make_error(util::ErrorCode::Tool, "conversion failed"));
    });

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

        agent::AsyncAgentOptions options;
        options.max_turns = 4;
        options.model = tests::make_model("gpt-test");
        options.transform_context =
            agent::adapt_sync_transform_context(
                [](const std::vector<ai::MessageVariant>&)
            -> util::Expected<std::vector<ai::MessageVariant>> {
            throw std::runtime_error("transform boom");
        });

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

        agent::AsyncAgentOptions options;
        options.max_turns = 4;
        options.model = tests::make_model("gpt-test");
        options.convert_to_llm =
            agent::adapt_sync_convert_to_llm(
                [](const std::vector<ai::MessageVariant>&)
            -> util::Expected<std::vector<ai::MessageVariant>> {
            throw std::runtime_error("convert boom");
        });

        agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));
        auto run = run_loop(loop, "hi");

        REQUIRE_FALSE(run.result);
        CHECK(run.result.error().code == util::ErrorCode::Tool);
        CHECK(run.result.error().message == "convertToLlm hook failed");
        CHECK(run.result.error().detail.find("convert boom") != std::string::npos);
    }
}













TEST_CASE("agent_end contains only messages from the current invocation", "[agent][async][issue35]") {
    FakeStreamingClient client;
    client.responses.push_back(ai::assistant_text_message("current reply"));
    agent::AsyncToolRegistry registry;
    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    agent::AsyncAgentLoop loop(
        client,
        std::move(registry),
        std::move(options));

    auto run = run_loop(
        loop,
        "current prompt",
        std::vector<ai::MessageVariant>{ai::user_text_message("prior prompt")});

    REQUIRE(run.result);
    const auto* ended = std::get_if<agent::AgentEndEvent>(&run.events.back());
    REQUIRE(ended != nullptr);
    REQUIRE(ended->messages.size() == 2);
    REQUIRE(std::holds_alternative<ai::UserMessage>(ended->messages[0]));
    CHECK(ai::text_from_user_message(std::get<ai::UserMessage>(ended->messages[0])) ==
          "current prompt");
    REQUIRE(std::holds_alternative<ai::AssistantMessage>(ended->messages[1]));
}







TEST_CASE("prepareNextTurn model swap changes next request model", "[agent][async][u8]") {
    FakeStreamingClient client;
    client.responses.push_back(tool_call_response());
    client.responses.push_back(ai::assistant_text_message("second"));

    agent::AsyncToolRegistry registry;
    REQUIRE(registry.add(std::make_unique<FakeTool>(ai::Tool{"read_file", "Read", test::permissive_object_tool_argument_contract()})));

    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    options.prepare_next_turn =
        agent::adapt_sync_prepare_next_turn(
            [](const agent::PrepareNextTurnContext&)
        -> util::Expected<std::optional<agent::AgentLoopTurnUpdate>> {
        return agent::AgentLoopTurnUpdate{.model = tests::make_model("gpt-swapped")};
    });
    options.validate_turn_update =
        agent::adapt_sync_validate_turn_update(
            [](const agent::AgentLoopTurnUpdate& update) -> util::ExpectedVoid {
        if (update.model && update.model->id == "gpt-swapped") {
            return {};
        }
        return std::unexpected(util::make_error(util::ErrorCode::Validation, "unknown model"));
    });

    agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));
    auto run = run_loop(loop, "read");

    REQUIRE(run.result);
    REQUIRE(client.requests.size() == 2);
    CHECK(client.requests[1].model.id == "gpt-swapped");
    CHECK(run.result->state.model.id == "gpt-swapped");
}

TEST_CASE("prepareNextTurn model update without validator is rejected", "[agent][async][u8]") {
    FakeStreamingClient client;
    client.responses.push_back(tool_call_response());

    agent::AsyncToolRegistry registry;
    REQUIRE(registry.add(std::make_unique<FakeTool>(ai::Tool{"read_file", "Read", test::permissive_object_tool_argument_contract()})));

    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    options.prepare_next_turn =
        agent::adapt_sync_prepare_next_turn(
            [](const agent::PrepareNextTurnContext&)
        -> util::Expected<std::optional<agent::AgentLoopTurnUpdate>> {
        return agent::AgentLoopTurnUpdate{.model = tests::make_model("gpt-swapped")};
    });

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

    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    options.prepare_next_turn =
        agent::adapt_sync_prepare_next_turn(
            [](const agent::PrepareNextTurnContext&)
        -> util::Expected<std::optional<agent::AgentLoopTurnUpdate>> {
        return agent::AgentLoopTurnUpdate{std::nullopt, std::nullopt, std::string{"invalid"}};
    });

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
    REQUIRE(registry.add(std::make_unique<FakeTool>(ai::Tool{"read_file", "Read", test::permissive_object_tool_argument_contract()})));

    int prepare_calls = 0;
    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    options.prepare_next_turn =
        agent::adapt_sync_prepare_next_turn(
            [&](const agent::PrepareNextTurnContext&)
        -> util::Expected<std::optional<agent::AgentLoopTurnUpdate>> {
        ++prepare_calls;
        if (prepare_calls == 1) {
            return agent::AgentLoopTurnUpdate{
                    .model = tests::make_model("gpt-swapped"),
                    .thinking_level = std::string{"invalid"},
                };
        }
        return std::nullopt;
    });
    options.validate_turn_update =
        agent::adapt_sync_validate_turn_update(
            [](const agent::AgentLoopTurnUpdate&) -> util::ExpectedVoid { return {}; });

    agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));
    auto first = run_loop(loop, "read");
    REQUIRE_FALSE(first.result);
    CHECK(first.result.error().code == util::ErrorCode::Validation);

    auto second = run_loop(loop, "hi again");
    REQUIRE(second.result);
    REQUIRE(client.requests.size() == 2);
    CHECK(client.requests[1].model.id == "gpt-test");
    CHECK(second.result->state.model.id == "gpt-test");
}

TEST_CASE("prepareNextTurn replaces model context without publishing replacement messages", "[agent][async][issue35]") {
    FakeStreamingClient client;
    client.responses.push_back(tool_call_response());
    client.responses.push_back(ai::assistant_text_message("second"));

    agent::AsyncToolRegistry registry;
    REQUIRE(registry.add(std::make_unique<FakeTool>(
        ai::Tool{"read_file", "Read", test::permissive_object_tool_argument_contract()})));

    bool prepared = false;
    bool stop_observed_replacement = false;
    std::size_t first_prepare_new_message_count = 0;
    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    options.prepare_next_turn =
        agent::adapt_sync_prepare_next_turn(
            [&](const agent::PrepareNextTurnContext& context)
        -> util::Expected<std::optional<agent::AgentLoopTurnUpdate>> {
        if (prepared) {
            return std::nullopt;
        }
        prepared = true;
        first_prepare_new_message_count = context.new_messages.size();
        agent::AgentLoopContextReplacement replacement;
        replacement.system_prompt = "replacement prompt";
        replacement.messages.push_back(ai::user_text_message("replacement history"));
        return agent::AgentLoopTurnUpdate{
            std::move(replacement), std::nullopt, std::nullopt};
    });
    options.should_stop_after_turn =
        agent::adapt_sync_should_stop_after_turn(
            [&](const agent::PrepareNextTurnContext& context)
        -> util::Expected<bool> {
        if (context.context.system_prompt &&
            *context.context.system_prompt == "replacement prompt" &&
            context.context.messages.size() == 1) {
            stop_observed_replacement = true;
        }
        return false;
    });

    agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));
    auto run = run_loop(loop, "read");

    REQUIRE(run.result);
    CHECK(prepared);
    CHECK(first_prepare_new_message_count == 3);
    CHECK(stop_observed_replacement);
    REQUIRE(client.requests.size() == 2);
    REQUIRE(client.requests[1].context.system_prompt.has_value());
    CHECK(*client.requests[1].context.system_prompt == "replacement prompt");
    REQUIRE(client.requests[1].context.messages.size() == 1);
    CHECK(ai::text_from_user_message(std::get<ai::UserMessage>(client.requests[1].context.messages[0])) ==
          "replacement history");
    REQUIRE(client.requests[1].context.tools.size() == 1);
    CHECK(client.requests[1].context.tools[0].name == "read_file");

    REQUIRE(run.result->context.messages.size() == 2);
    CHECK(ai::text_from_user_message(std::get<ai::UserMessage>(run.result->context.messages[0])) ==
          "replacement history");
    CHECK(ai::text_from_assistant_content(
              std::get<ai::AssistantMessage>(run.result->context.messages[1]).content) ==
          "second");
    CHECK(count_events<agent::MessageStartEvent>(run.events) == 4);
    CHECK(count_events<agent::MessageEndEvent>(run.events) == 4);
    const auto* ended = std::get_if<agent::AgentEndEvent>(&run.events.back());
    REQUIRE(ended != nullptr);
    REQUIRE(ended->messages.size() == 4);
    CHECK(ai::text_from_user_message(std::get<ai::UserMessage>(ended->messages[0])) ==
          "read");
}

TEST_CASE("prepareNextTurn no update leaves model and thinking level unchanged", "[agent][async][u8]") {
    FakeStreamingClient client;
    client.responses.push_back(ai::assistant_text_message("first"));

    agent::AsyncToolRegistry registry;

    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    options.prepare_next_turn =
        agent::adapt_sync_prepare_next_turn(
            [](const agent::PrepareNextTurnContext&)
        -> util::Expected<std::optional<agent::AgentLoopTurnUpdate>> { return std::nullopt; });

    agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));
    auto run = run_loop(loop, "hi");

    REQUIRE(run.result);
    CHECK(run.result->state.model.id == "gpt-test");
    CHECK(run.result->state.thinking_level.empty());
}

TEST_CASE("prepareNextTurn valid thinking level is preserved in state", "[agent][async][u8]") {
    FakeStreamingClient client;
    client.responses.push_back(ai::assistant_text_message("first"));

    agent::AsyncToolRegistry registry;

    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    options.prepare_next_turn =
        agent::adapt_sync_prepare_next_turn(
            [](const agent::PrepareNextTurnContext&)
        -> util::Expected<std::optional<agent::AgentLoopTurnUpdate>> {
        return agent::AgentLoopTurnUpdate{std::nullopt, std::nullopt, std::string{"high"}};
    });

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
    REQUIRE(registry.add(std::make_unique<FakeTool>(ai::Tool{"read_file", "Read", test::permissive_object_tool_argument_contract()})));

    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    options.prepare_next_turn =
        agent::adapt_sync_prepare_next_turn(
            [](const agent::PrepareNextTurnContext&)
        -> util::Expected<std::optional<agent::AgentLoopTurnUpdate>> {
        return agent::AgentLoopTurnUpdate{.model = tests::make_model("missing-model")};
    });
    options.validate_turn_update =
        agent::adapt_sync_validate_turn_update(
            [](const agent::AgentLoopTurnUpdate& update) -> util::ExpectedVoid {
        if (update.model && update.model->id != "gpt-test") {
            return std::unexpected(util::make_error(util::ErrorCode::Validation, "unknown model", update.model->id));
        }
        return {};
    });

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

        agent::AsyncAgentOptions options;
        options.max_turns = 4;
        options.model = tests::make_model("gpt-test");
        options.prepare_next_turn =
            agent::adapt_sync_prepare_next_turn(
                [](const agent::PrepareNextTurnContext&)
            -> util::Expected<std::optional<agent::AgentLoopTurnUpdate>> {
            throw std::runtime_error("prepare boom");
        });

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

        agent::AsyncAgentOptions options;
        options.max_turns = 4;
        options.model = tests::make_model("gpt-test");
        options.prepare_next_turn =
            agent::adapt_sync_prepare_next_turn(
                [](const agent::PrepareNextTurnContext&)
            -> util::Expected<std::optional<agent::AgentLoopTurnUpdate>> {
            return agent::AgentLoopTurnUpdate{.model = tests::make_model("gpt-next")};
        });
        options.validate_turn_update =
            agent::adapt_sync_validate_turn_update(
                [](const agent::AgentLoopTurnUpdate&) -> util::ExpectedVoid {
            throw std::runtime_error("validator boom");
        });

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
        ai::Tool{"alpha", "Alpha", test::permissive_object_tool_argument_contract()},
        agent::ToolConcurrency::ParallelSafe,
        probe)));
    REQUIRE(registry.add(std::make_unique<ProbedFakeTool>(
        ai::Tool{"beta", "Beta", test::permissive_object_tool_argument_contract()},
        agent::ToolConcurrency::Exclusive,
        probe)));

    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
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
        ai::Tool{"alpha", "Alpha", test::permissive_object_tool_argument_contract()},
        agent::ToolConcurrency::ParallelSafe,
        "alpha result");
    auto beta = std::make_unique<ConfigurableFakeTool>(
        ai::Tool{"beta", "Beta", test::permissive_object_tool_argument_contract()},
        agent::ToolConcurrency::ParallelSafe,
        "beta result");
    auto* alpha_ptr = alpha.get();
    auto* beta_ptr = beta.get();
    REQUIRE(registry.add(std::move(alpha)));
    REQUIRE(registry.add(std::move(beta)));

    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
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

TEST_CASE(
    "agent loop recovers after a schema-invalid call in a bounded parallel batch",
    "[agent][async][tool-arguments][issue27]") {
    FakeStreamingClient client;

    ai::AssistantMessage mixed_calls;
    mixed_calls.stop_reason = ai::AssistantStopReason::ToolUse;
    mixed_calls.content.emplace_back(ai::tool_call_content(
        "call-invalid", "alpha", R"({"value":"not-an-integer"})"));
    mixed_calls.content.emplace_back(ai::tool_call_content(
        "call-valid", "beta", R"({"value":"2"})"));
    client.responses.push_back(std::move(mixed_calls));

    ai::AssistantMessage corrected_call;
    corrected_call.stop_reason = ai::AssistantStopReason::ToolUse;
    corrected_call.content.emplace_back(ai::tool_call_content(
        "call-corrected", "alpha", R"({"value":"3"})"));
    client.responses.push_back(std::move(corrected_call));
    client.responses.push_back(ai::assistant_text_message("recovered"));

    const auto strict_contract = test::integer_value_tool_argument_contract();

    agent::AsyncToolRegistry registry;
    auto alpha = std::make_unique<ConfigurableFakeTool>(
        ai::Tool{"alpha", "Alpha", strict_contract},
        agent::ToolConcurrency::ParallelSafe,
        "alpha result");
    auto beta = std::make_unique<ConfigurableFakeTool>(
        ai::Tool{"beta", "Beta", strict_contract},
        agent::ToolConcurrency::ParallelSafe,
        "beta result");
    auto* alpha_ptr = alpha.get();
    auto* beta_ptr = beta.get();
    REQUIRE(registry.add(std::move(alpha)));
    REQUIRE(registry.add(std::move(beta)));

    std::vector<std::string> before_hook_names;
    std::vector<util::JsonValue> before_hook_arguments;
    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    options.tool_execution = agent::BoundedParallelToolExecution{2};
    options.before_tool_call =
        agent::adapt_sync_before_tool_call(
            [&](const agent::BeforeToolCallContext& context)
        -> util::Expected<agent::BeforeToolCallResult> {
        before_hook_names.push_back(context.tool_call.name);
        before_hook_arguments.push_back(context.args);
        return agent::BeforeToolCallResult{};
    });

    agent::AsyncAgentLoop loop(client, std::move(registry), std::move(options));
    auto run = run_loop_on_pool(loop, "run both calls and recover");

    REQUIRE(run.result);
    CHECK(run.result->turns == 3);
    CHECK(run.result->stop_reason == ai::AssistantStopReason::Stop);
    REQUIRE(alpha_ptr->invocations.size() == 1);
    REQUIRE(beta_ptr->invocations.size() == 1);
    CHECK(alpha_ptr->invocations[0].call_id == "call-corrected");
    CHECK(beta_ptr->invocations[0].call_id == "call-valid");
    CHECK(alpha_ptr->invocations[0].arguments.at("value").get_number() == 3);
    CHECK(beta_ptr->invocations[0].arguments.at("value").get_number() == 2);

    REQUIRE((before_hook_names == std::vector<std::string>{"beta", "alpha"}));
    REQUIRE(before_hook_arguments.size() == 2);
    CHECK(before_hook_arguments[0].at("value").get_number() == 2);
    CHECK(before_hook_arguments[1].at("value").get_number() == 3);

    REQUIRE(client.requests.size() == 3);
    const auto& recovery_context = client.requests[1].context.messages;
    REQUIRE(recovery_context.size() == 4);
    REQUIRE(std::holds_alternative<ai::ToolResultMessage>(recovery_context[2]));
    REQUIRE(std::holds_alternative<ai::ToolResultMessage>(recovery_context[3]));
    const auto& invalid_result = std::get<ai::ToolResultMessage>(recovery_context[2]);
    const auto& valid_result = std::get<ai::ToolResultMessage>(recovery_context[3]);
    CHECK(invalid_result.tool_call_id == "call-invalid");
    CHECK(invalid_result.tool_name == "alpha");
    CHECK(invalid_result.is_error);
    CHECK(valid_result.tool_call_id == "call-valid");
    CHECK(valid_result.tool_name == "beta");
    CHECK_FALSE(valid_result.is_error);

    const auto& completion_context = client.requests[2].context.messages;
    REQUIRE(completion_context.size() == 6);
    REQUIRE(std::holds_alternative<ai::ToolResultMessage>(completion_context[5]));
    const auto& corrected_result = std::get<ai::ToolResultMessage>(completion_context[5]);
    CHECK(corrected_result.tool_call_id == "call-corrected");
    CHECK_FALSE(corrected_result.is_error);

    CHECK(count_events<agent::ToolExecutionStartEvent>(run.events) == 3);
    CHECK(count_events<agent::ToolExecutionEndEvent>(run.events) == 3);
}

TEST_CASE("bounded parallel limit one executes sequentially", "[agent][async][u8]") {
    FakeStreamingClient client;
    client.responses.push_back(two_tool_call_response());
    client.responses.push_back(ai::assistant_text_message("done"));

    ConcurrencyProbe probe;
    agent::AsyncToolRegistry registry;
    REQUIRE(registry.add(std::make_unique<ProbedFakeTool>(
        ai::Tool{"alpha", "Alpha", test::permissive_object_tool_argument_contract()},
        agent::ToolConcurrency::ParallelSafe,
        probe)));
    REQUIRE(registry.add(std::make_unique<ProbedFakeTool>(
        ai::Tool{"beta", "Beta", test::permissive_object_tool_argument_contract()},
        agent::ToolConcurrency::ParallelSafe,
        probe)));

    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
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
        ai::Tool{"alpha", "Alpha", test::permissive_object_tool_argument_contract()},
        agent::ToolConcurrency::ParallelSafe,
        probe)));
    REQUIRE(registry.add(std::make_unique<ProbedFakeTool>(
        ai::Tool{"beta", "Beta", test::permissive_object_tool_argument_contract()},
        agent::ToolConcurrency::ParallelSafe,
        probe)));

    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
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
        ai::Tool{"alpha", "Alpha", test::permissive_object_tool_argument_contract()},
        agent::ToolConcurrency::ParallelSafe,
        "alpha result");
    auto beta = std::make_unique<ConfigurableFakeTool>(
        ai::Tool{"beta", "Beta", test::permissive_object_tool_argument_contract()},
        agent::ToolConcurrency::ParallelSafe,
        "beta result");
    auto* alpha_ptr = alpha.get();
    auto* beta_ptr = beta.get();
    REQUIRE(registry.add(std::move(alpha)));
    REQUIRE(registry.add(std::move(beta)));

    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    options.tool_execution = agent::BoundedParallelToolExecution{2};
    options.before_tool_call =
        agent::adapt_sync_before_tool_call(
            [](const agent::BeforeToolCallContext& context)
        -> util::Expected<agent::BeforeToolCallResult> {
        if (context.tool_call.name == "alpha") {
            return agent::BeforeToolCallResult{true, "blocked alpha"};
        }
        return agent::BeforeToolCallResult{};
    });

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
        ai::Tool{"alpha", "Alpha", test::permissive_object_tool_argument_contract()},
        agent::ToolConcurrency::ParallelSafe,
        probe)));
    REQUIRE(registry.add(std::make_unique<ProbedFakeTool>(
        ai::Tool{"beta", "Beta", test::permissive_object_tool_argument_contract()},
        agent::ToolConcurrency::ParallelSafe,
        probe)));

    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    options.tool_execution = agent::BoundedParallelToolExecution{2};
    options.before_tool_call =
        agent::adapt_sync_before_tool_call(
            [](const agent::BeforeToolCallContext&)
        -> util::Expected<agent::BeforeToolCallResult> {
        return std::unexpected(util::make_error(util::ErrorCode::Tool, "preflight failed"));
    });

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
        ai::Tool{"alpha", "Alpha", test::permissive_object_tool_argument_contract()})));
    REQUIRE(registry.add(std::make_unique<ConfigurableFakeTool>(
        ai::Tool{"beta", "Beta", test::permissive_object_tool_argument_contract()},
        agent::ToolConcurrency::ParallelSafe,
        "beta result")));

    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
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
        ai::Tool{"alpha", "Alpha", test::permissive_object_tool_argument_contract()},
        agent::ToolConcurrency::ParallelSafe,
        "alpha result")));
    REQUIRE(registry.add(std::make_unique<ConfigurableFakeTool>(
        ai::Tool{"beta", "Beta", test::permissive_object_tool_argument_contract()},
        agent::ToolConcurrency::ParallelSafe,
        "beta result")));

    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
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
        ai::Tool{"alpha", "Alpha", test::permissive_object_tool_argument_contract()},
        agent::ToolConcurrency::ParallelSafe,
        "alpha result")));
    REQUIRE(registry.add(std::make_unique<ConfigurableFakeTool>(
        ai::Tool{"beta", "Beta", test::permissive_object_tool_argument_contract()},
        agent::ToolConcurrency::ParallelSafe,
        "beta result")));

    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    options.tool_execution = agent::BoundedParallelToolExecution{2};
    options.after_tool_call =
        agent::adapt_sync_after_tool_call(
            [](const agent::AfterToolCallContext& context)
        -> util::Expected<agent::AfterToolCallResult> {
        if (context.tool_call.name == "alpha") {
            return std::unexpected(util::make_error(util::ErrorCode::Tool, "post-processor failed"));
        }
        return agent::AfterToolCallResult{};
    });

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
        ai::Tool{"alpha", "Alpha", test::permissive_object_tool_argument_contract()},
        std::chrono::milliseconds{100},
        "alpha result")));
    REQUIRE(registry.add(std::make_unique<DelayedFakeTool>(
        ai::Tool{"beta", "Beta", test::permissive_object_tool_argument_contract()},
        std::chrono::milliseconds{10},
        "beta result")));

    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
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
        ai::Tool{"alpha", "Alpha", test::permissive_object_tool_argument_contract()},
        agent::ToolConcurrency::ParallelSafe,
        "alpha result");
    auto beta = std::make_unique<ConfigurableFakeTool>(
        ai::Tool{"beta", "Beta", test::permissive_object_tool_argument_contract()},
        agent::ToolConcurrency::ParallelSafe,
        "beta result");
    auto* alpha_ptr = alpha.get();
    auto* beta_ptr = beta.get();
    REQUIRE(registry.add(std::move(alpha)));
    REQUIRE(registry.add(std::move(beta)));

    int before_calls = 0;
    int after_calls = 0;
    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    options.tool_execution = agent::BoundedParallelToolExecution{2};
    options.before_tool_call =
        agent::adapt_sync_before_tool_call(
            [&](const agent::BeforeToolCallContext&)
        -> util::Expected<agent::BeforeToolCallResult> {
        ++before_calls;
        return agent::BeforeToolCallResult{};
    });
    options.after_tool_call =
        agent::adapt_sync_after_tool_call(
            [&](const agent::AfterToolCallContext&)
        -> util::Expected<agent::AfterToolCallResult> {
        ++after_calls;
        return agent::AfterToolCallResult{};
    });

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
