#include "../../third_party/catch2/catch_test_macros.hpp"

#include "../../src/util/ExpectedMacros.hpp"

#include "../../include/cch/agent/AgentLoop.hpp"
#include "../../include/cch/util/Error.hpp"
#include "../../include/cch/util/Json.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>

#include <deque>
#include <memory>
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
        for (const auto& block : response.content) {
            if (const auto* text = std::get_if<ai::TextContent>(&block)) {
                CCH_TRY_VOID(sink(ai::TextDeltaEvent{0, text->text, response}));
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
        co_return agent::AsyncToolExecutionResult{"tool says ok", std::nullopt, false};
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
    registry.add(std::move(zed));
    registry.add(std::move(alpha));

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
    registry.add(std::move(tool));
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
    CHECK(count_events<agent::ToolExecutionStartEvent>(run.events) == 1);
    CHECK(count_events<agent::ToolExecutionEndEvent>(run.events) == 1);
}

TEST_CASE("async agent loop turns malformed tool arguments into error tool results", "[agent][async][u5]") {
    FakeStreamingClient client;
    client.responses.push_back(tool_call_response("not-json"));
    client.responses.push_back(ai::assistant_text_message("saw error"));
    agent::AsyncToolRegistry registry;
    registry.add(std::make_unique<FakeTool>(ai::Tool{"read_file", "Read", ai::JsonSchema::object()}));
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
    registry.add(std::make_unique<FakeTool>(ai::Tool{"read_file", "Read", ai::JsonSchema::object()}));
    agent::AsyncAgentLoop loop(client, std::move(registry), agent::AsyncAgentOptions{1, "gpt-test"});

    auto run = run_loop(loop, "read");

    REQUIRE_FALSE(run.result);
    CHECK(run.result.error().message == "max turns exceeded");
    CHECK(count_events<agent::AgentEndEvent>(run.events) == 1);
}
