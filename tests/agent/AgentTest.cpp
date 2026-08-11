#include <cch/agent/Agent.hpp>
#include <cch/ai/Content.hpp>
#include "support/FakeModelRuntime.hpp"
#include "support/ModelFixture.hpp"
#include "support/ToolArgumentContracts.hpp"
#include "util/Json.hpp"

#include "../../third_party/catch2/catch_test_macros.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <chrono>
#include <deque>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <variant>
#include <vector>

using namespace cch;

namespace {

std::string event_label(const agent::AgentLifecycleEvent& event) {
    if (std::holds_alternative<agent::AgentStartEvent>(event)) return "agent_start";
    if (std::holds_alternative<agent::AgentEndEvent>(event)) return "agent_end";
    if (std::holds_alternative<agent::TurnStartEvent>(event)) return "turn_start";
    if (std::holds_alternative<agent::TurnEndEvent>(event)) return "turn_end";
    if (const auto* start = std::get_if<agent::MessageStartEvent>(&event)) {
        return std::holds_alternative<ai::UserMessage>(start->message)
            ? "message_start:user"
            : "message_start:assistant";
    }
    if (std::holds_alternative<agent::MessageUpdateEvent>(event)) return "message_update";
    if (const auto* end = std::get_if<agent::MessageEndEvent>(&event)) {
        return std::holds_alternative<ai::UserMessage>(end->message)
            ? "message_end:user"
            : "message_end:assistant";
    }
    if (std::holds_alternative<agent::ToolExecutionStartEvent>(event)) return "tool_start";
    return "tool_end";
}

util::ExpectedVoid run_prompt(agent::Agent& subject, std::string prompt) {
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

util::ExpectedVoid run_prompt(
    agent::Agent& subject,
    std::string prompt,
    agent::AgentEventCommitter commitment) {
    boost::asio::io_context io;
    std::optional<util::ExpectedVoid> result;
    boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<void> {
            result = co_await subject.prompt(
                std::move(prompt), std::move(commitment));
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
        result.content.push_back(ai::text_content("file contents"));
        co_return result;
    }

private:
    ai::Tool definition_;
};

class RecordingModelRuntime final : public coding_agent::ModelRuntime {
public:
    boost::asio::awaitable<util::Expected<ai::AssistantMessage>> stream_simple(
        ai::Model model,
        ai::AiContext context,
        ai::SimpleStreamOptions options,
        ai::AssistantEventSink sink) override {
        request_models.push_back(model.id);
        calls.push_back(tests::RecordedStreamSimpleCall{
            std::move(model), std::move(context), std::move(options)});
        if (responses.empty()) {
            co_return std::unexpected(util::make_error(
                util::ErrorCode::Provider,
                "no scripted response"));
        }
        auto response = std::move(responses.front());
        responses.pop_front();
        if (sink) {
            if (auto emitted = sink(ai::AssistantStartEvent{response}); !emitted) {
                co_return std::unexpected(emitted.error());
            }
        }
        co_return response;
    }

    std::deque<ai::AssistantMessage> responses;
    std::vector<std::string> request_models;
    std::vector<tests::RecordedStreamSimpleCall> calls;
};

/// Emits one complete assistant lifecycle for a scripted response: a start,
/// then per content block the text/tool-call deltas, matching the scripted
/// fake provider's observable event shape.
[[nodiscard]] util::ExpectedVoid emit_scripted_lifecycle(
    ai::AssistantEventSink& sink,
    const ai::AssistantMessage& final_message) {
    auto emit = [&sink](const ai::AssistantStreamEvent& event) -> util::ExpectedVoid {
        return sink(event);
    };
    auto partial = final_message;
    partial.content.clear();
    if (auto emitted = emit(ai::AssistantStartEvent{.partial = partial}); !emitted) {
        return emitted;
    }
    for (std::size_t content_index = 0;
         content_index < final_message.content.size();
         ++content_index) {
        const auto& completed = final_message.content[content_index];
        if (const auto* text = std::get_if<ai::TextContent>(&completed)) {
            partial.content.emplace_back(ai::TextContent{
                .text = "",
                .text_signature = std::nullopt,
            });
            if (auto emitted = emit(ai::TextStartEvent{
                    .content_index = content_index,
                    .partial = partial,
                });
                !emitted) {
                return emitted;
            }
            if (!text->text.empty()) {
                std::get<ai::TextContent>(partial.content[content_index]).text = text->text;
                if (auto emitted = emit(ai::TextDeltaEvent{
                        .content_index = content_index,
                        .delta = text->text,
                        .partial = partial,
                    });
                    !emitted) {
                    return emitted;
                }
            }
            partial.content[content_index] = *text;
            if (auto emitted = emit(ai::TextEndEvent{
                    .content_index = content_index,
                    .content = text->text,
                    .partial = partial,
                });
                !emitted) {
                return emitted;
            }
            continue;
        }
        const auto& tool_call = std::get<ai::ToolCallContent>(completed);
        partial.content.emplace_back(ai::ToolCallContent{
            .id = tool_call.id,
            .name = tool_call.name,
            .arguments = util::JsonValue{util::JsonValue::object_t{}},
            .raw_arguments = {},
            .thought_signature = std::nullopt,
            .arguments_valid = true,
            .argument_error = std::nullopt,
        });
        if (auto emitted = emit(ai::ToolCallStartEvent{
                .content_index = content_index,
                .partial = partial,
            });
            !emitted) {
            return emitted;
        }
        if (!tool_call.raw_arguments.empty()) {
            std::get<ai::ToolCallContent>(partial.content[content_index]).raw_arguments =
                tool_call.raw_arguments;
            if (auto emitted = emit(ai::ToolCallDeltaEvent{
                    .content_index = content_index,
                    .delta = tool_call.raw_arguments,
                    .partial = partial,
                });
                !emitted) {
                return emitted;
            }
        }
        partial.content[content_index] = tool_call;
        if (auto emitted = emit(ai::ToolCallEndEvent{
                .content_index = content_index,
                .tool_call = tool_call,
                .partial = partial,
            });
            !emitted) {
            return emitted;
        }
    }
    return emit(ai::AssistantDoneEvent{
        .reason = final_message.stop_reason,
        .message = final_message,
    });
}

/// Scripted fake ModelRuntime mirroring the scripted fake provider's behavior
/// ("fake: <prompt>", `read` tool calls, tool-result observation, abort
/// terminals) so the stateful Agent tests keep their observable responses.
class ScriptedFakeRuntime final : public coding_agent::ModelRuntime {
public:
    boost::asio::awaitable<util::Expected<ai::AssistantMessage>> stream_simple(
        ai::Model model,
        ai::AiContext context,
        ai::SimpleStreamOptions options,
        ai::AssistantEventSink sink) override {
        calls.push_back(tests::RecordedStreamSimpleCall{
            std::move(model), std::move(context), std::move(options)});
        ai::AssistantMessage assistant;
        assistant.model = calls.back().model.id;
        assistant.provider = "fake";
        assistant.api = "scripted-fake";
        assistant.usage = ai::Usage{};
        assistant.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        if (calls.back().options.stop_token.stop_requested()) {
            assistant.stop_reason = ai::AssistantStopReason::Aborted;
            assistant.error_message = "Request was aborted";
            if (auto emitted = sink(ai::AssistantErrorEvent{
                    .reason = assistant.stop_reason,
                    .error = assistant,
                    .failure = util::make_error(
                        util::ErrorCode::Cancelled, *assistant.error_message),
                });
                !emitted) {
                co_return std::unexpected(emitted.error());
            }
            co_return assistant;
        }

        if (!calls.back().context.messages.empty()) {
            if (const auto* last_tool =
                    std::get_if<ai::ToolResultMessage>(&calls.back().context.messages.back())) {
                assistant.content.emplace_back(ai::text_content(
                    "fake observed: " + ai::text_from_content(last_tool->content)));
                if (auto emitted = emit_scripted_lifecycle(sink, assistant); !emitted) {
                    co_return std::unexpected(emitted.error());
                }
                co_return assistant;
            }
        }

        std::string prompt;
        for (auto it = calls.back().context.messages.rbegin();
             it != calls.back().context.messages.rend();
             ++it) {
            if (const auto* user = std::get_if<ai::UserMessage>(&*it)) {
                prompt = ai::text_from_user_message(*user);
                break;
            }
        }

        if (prompt.starts_with("read ")) {
            ai::ToolCallContent call;
            call.id = "fake-read-1";
            call.name = "read";
            const auto raw = "{\"path\":\"" + prompt.substr(5) + "\"}";
            call.raw_arguments = raw;
            call.arguments_valid = true;
            call.arguments = util::JsonValue{util::JsonValue::object_t{}};
            if (auto parsed = util::read_json(call.raw_arguments)) {
                call.arguments = std::move(*parsed);
            }
            assistant.content.emplace_back(ai::text_content("reading " + prompt.substr(5)));
            assistant.content.emplace_back(std::move(call));
            assistant.stop_reason = ai::AssistantStopReason::ToolUse;
            if (auto emitted = emit_scripted_lifecycle(sink, assistant); !emitted) {
                co_return std::unexpected(emitted.error());
            }
            co_return assistant;
        }

        assistant.content.emplace_back(ai::text_content("fake: " + prompt));
        if (auto emitted = emit_scripted_lifecycle(sink, assistant); !emitted) {
            co_return std::unexpected(emitted.error());
        }
        co_return assistant;
    }

    std::vector<tests::RecordedStreamSimpleCall> calls;
};

ai::AssistantMessage read_tool_call_response() {
    ai::AssistantMessage response;
    response.stop_reason = ai::AssistantStopReason::ToolUse;
    ai::ToolCallContent call;
    call.id = "read-1";
    call.name = "read";
    call.arguments = util::JsonValue{util::JsonValue::object_t{}};
    call.raw_arguments = "{}";
    response.content.emplace_back(std::move(call));
    return response;
}

class ThrowingThenRecoveringRuntime final : public coding_agent::ModelRuntime {
public:
    boost::asio::awaitable<util::Expected<ai::AssistantMessage>> stream_simple(
        ai::Model,
        ai::AiContext,
        ai::SimpleStreamOptions,
        ai::AssistantEventSink sink) override {
        ++calls;
        if (calls == 1) {
            throw std::runtime_error("host provider threw");
        }
        auto response = ai::assistant_text_message("recovered");
        if (sink) {
            if (auto emitted = sink(ai::AssistantStartEvent{response}); !emitted) {
                co_return std::unexpected(emitted.error());
            }
        }
        co_return response;
    }

    int calls{0};
};

class GatedModelRuntime final : public coding_agent::ModelRuntime {
public:
    boost::asio::awaitable<util::Expected<ai::AssistantMessage>> stream_simple(
        ai::Model,
        ai::AiContext,
        ai::SimpleStreamOptions options,
        ai::AssistantEventSink sink) override {
        ++request_count;
        request_stop_token = options.stop_token;
        started = true;
        if (request_count == 1) {
            auto executor = co_await boost::asio::this_coro::executor;
            gate.emplace(executor);
            gate->expires_at(std::chrono::steady_clock::time_point::max());
            std::stop_callback cancellation{options.stop_token, [this] {
                if (gate) {
                    gate->cancel();
                }
            }};

            boost::system::error_code error;
            co_await gate->async_wait(
                boost::asio::redirect_error(boost::asio::use_awaitable, error));
        }

        auto response = options.stop_token.stop_requested()
            ? ai::assistant_text_message("")
            : ai::assistant_text_message("released");
        if (options.stop_token.stop_requested()) {
            response.stop_reason = ai::AssistantStopReason::Aborted;
            response.error_message = "prompt aborted";
            if (sink) {
                if (auto emitted = sink(ai::AssistantErrorEvent{
                        .reason = ai::AssistantStopReason::Aborted,
                        .error = response,
                    });
                    !emitted) {
                    co_return std::unexpected(emitted.error());
                }
            }
            co_return response;
        }
        if (sink) {
            if (auto emitted = sink(ai::AssistantStartEvent{response}); !emitted) {
                co_return std::unexpected(emitted.error());
            }
            if (auto emitted = sink(ai::TextDeltaEvent{
                    .content_index = 0,
                    .delta = "released",
                    .partial = response,
                });
                !emitted) {
                co_return std::unexpected(emitted.error());
            }
        }
        co_return response;
    }

    void release() {
        if (gate) {
            gate->cancel();
        }
    }

    bool started{false};
    int request_count{0};
    std::stop_token request_stop_token;
    std::optional<boost::asio::steady_timer> gate;
};

} // namespace

TEST_CASE("stateful Agent retains a scripted fake-provider prompt in its passive snapshot", "[agent][stateful][issue35]") {
    auto client = std::make_shared<ScriptedFakeRuntime>();
    agent::AsyncToolRegistry tools;
    agent::AsyncAgentOptions options;
    options.max_turns = 3;
    options.model = tests::make_model("fake-model");
    agent::Agent subject(client, std::move(tools), std::move(options));

    REQUIRE(run_prompt(subject, "hello"));
    auto snapshot = subject.state();
    CHECK_FALSE(snapshot.is_running);
    CHECK_FALSE(snapshot.streaming_message.has_value());
    CHECK(snapshot.pending_tool_call_ids.empty());
    CHECK(snapshot.model.id == "fake-model");
    CHECK(snapshot.thinking_level == "off");
    REQUIRE(snapshot.messages.size() == 2);
    REQUIRE(std::holds_alternative<ai::UserMessage>(snapshot.messages[0]));
    CHECK(ai::text_from_user_message(std::get<ai::UserMessage>(snapshot.messages[0])) == "hello");
    REQUIRE(std::holds_alternative<ai::AssistantMessage>(snapshot.messages[1]));
    CHECK(ai::text_from_assistant_content(
              std::get<ai::AssistantMessage>(snapshot.messages[1]).content) ==
          "fake: hello");

    snapshot.messages.clear();
    CHECK(subject.state().messages.size() == 2);
}

TEST_CASE(
    "stateful Agent gives awaitable policies its active run stop token",
    "[agent][stateful][issue82]") {
    auto client = std::make_shared<ScriptedFakeRuntime>();
    agent::AsyncAgentOptions options;
    options.max_turns = 3;
    options.model = tests::make_model("fake-model");

    bool stop_possible = false;
    bool stop_requested = true;
    options.transform_context = [&stop_possible, &stop_requested](
                                    std::vector<ai::MessageVariant> messages,
                                    std::stop_token stop_token)
        -> boost::asio::awaitable<util::Expected<std::vector<ai::MessageVariant>>> {
        stop_possible = stop_token.stop_possible();
        stop_requested = stop_token.stop_requested();
        co_return messages;
    };

    agent::Agent subject(client, agent::AsyncToolRegistry{}, std::move(options));
    REQUIRE(run_prompt(subject, "hello"));
    CHECK(stop_possible);
    CHECK_FALSE(stop_requested);
}

TEST_CASE(
    "stateful Agent abort requests the same token observed by policies and provider",
    "[agent][stateful][abort][issue39]") {
    boost::asio::io_context io;
    auto client = std::make_shared<GatedModelRuntime>();
    std::optional<std::stop_token> transform_stop_token;
    agent::AsyncAgentOptions options;
    options.model = tests::make_model("fake-model");
    options.transform_context = [&transform_stop_token](
                                    std::vector<ai::MessageVariant> messages,
                                    std::stop_token stop_token)
        -> boost::asio::awaitable<util::Expected<std::vector<ai::MessageVariant>>> {
        transform_stop_token = stop_token;
        co_return messages;
    };
    agent::Agent subject(client, agent::AsyncToolRegistry{}, std::move(options));

    subject.abort();
    std::optional<util::ExpectedVoid> result;
    boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<void> {
            result = co_await subject.prompt("abort me");
            co_return;
        },
        boost::asio::detached);

    while (!client->started) {
        REQUIRE(io.poll_one() == 1);
    }
    REQUIRE(transform_stop_token.has_value());
    CHECK(*transform_stop_token == client->request_stop_token);
    CHECK_FALSE(transform_stop_token->stop_requested());
    subject.abort();
    subject.abort();
    if (io.stopped()) {
        io.restart();
    }
    io.run();

    REQUIRE(result.has_value());
    REQUIRE(result->has_value());
    CHECK(transform_stop_token->stop_requested());
    CHECK(client->request_stop_token.stop_requested());
    CHECK_FALSE(subject.state().is_running);
    REQUIRE(subject.state().messages.size() == 2);
    REQUIRE(std::holds_alternative<ai::AssistantMessage>(subject.state().messages[1]));
    CHECK(std::get<ai::AssistantMessage>(subject.state().messages[1]).stop_reason ==
          ai::AssistantStopReason::Aborted);
    subject.abort();
}

TEST_CASE("stateful Agent reduces lifecycle state before ordered move-only observers", "[agent][stateful][issue35]") {
    auto client = std::make_shared<ScriptedFakeRuntime>();
    agent::AsyncToolRegistry tools;
    agent::AsyncAgentOptions options;
    options.max_turns = 3;
    options.model = tests::make_model("fake-model");
    agent::Agent subject(client, std::move(tools), std::move(options));

    std::vector<std::string> events;
    bool user_was_committed_before_delivery = false;
    bool assistant_was_committed_before_delivery = false;
    bool agent_end_observed_active_run = false;
    auto subscribed = subject.subscribe(
        [owned_count = std::make_unique<int>(0),
         &subject,
         &events,
         &user_was_committed_before_delivery,
         &assistant_was_committed_before_delivery,
         &agent_end_observed_active_run](const agent::AgentLifecycleEvent& event) mutable {
            ++*owned_count;
            events.push_back(event_label(event));
            if (const auto* ended = std::get_if<agent::MessageEndEvent>(&event)) {
                const auto snapshot = subject.state();
                if (std::holds_alternative<ai::UserMessage>(ended->message)) {
                    user_was_committed_before_delivery = snapshot.messages.size() == 1;
                } else if (std::holds_alternative<ai::AssistantMessage>(ended->message)) {
                    assistant_was_committed_before_delivery =
                        snapshot.messages.size() == 2 && !snapshot.streaming_message;
                }
            }
            if (const auto* ended = std::get_if<agent::AgentEndEvent>(&event)) {
                agent_end_observed_active_run =
                    subject.state().is_running && ended->messages.size() == 2;
            }
            return util::ExpectedVoid{};
        });
    REQUIRE(subscribed);
    auto subscription = std::move(*subscribed);

    REQUIRE(run_prompt(subject, "hello"));

    const std::vector<std::string> expected{
        "agent_start",
        "turn_start",
        "message_start:user",
        "message_end:user",
        "message_start:assistant",
        "message_update",
        "message_update",
        "message_update",
        "message_end:assistant",
        "turn_end",
        "agent_end",
    };
    CHECK(events == expected);
    CHECK(user_was_committed_before_delivery);
    CHECK(assistant_was_committed_before_delivery);
    CHECK(agent_end_observed_active_run);
    CHECK(subscription);
}

TEST_CASE(
    "stateful Agent commits after live state and weak observers",
    "[agent][stateful][commitment][issue36]") {
    auto client = std::make_shared<ScriptedFakeRuntime>();
    agent::AsyncToolRegistry tools;
    agent::AsyncAgentOptions options;
    options.max_turns = 3;
    options.model = tests::make_model("fake-model");
    agent::Agent subject(client, std::move(tools), std::move(options));

    std::vector<std::string> ordering;
    auto subscribed = subject.subscribe(
        [&](const agent::AgentLifecycleEvent& event) -> util::ExpectedVoid {
            if (const auto* end = std::get_if<agent::MessageEndEvent>(&event)) {
                ordering.push_back("observer");
                const auto snapshot = subject.state();
                REQUIRE_FALSE(snapshot.messages.empty());
                CHECK(snapshot.messages.back().index() == end->message.index());
            }
            return {};
        });
    REQUIRE(subscribed);
    auto subscription = std::move(*subscribed);

    auto prompted = run_prompt(
        subject,
        "hello",
        [&](const agent::AgentLifecycleEvent& event) -> util::ExpectedVoid {
            if (std::holds_alternative<agent::MessageEndEvent>(event)) {
                ordering.push_back("commitment");
            }
            return {};
        });
    REQUIRE(prompted);
    const std::vector<std::string> expected{
        "observer", "commitment", "observer", "commitment"};
    CHECK(ordering == expected);
    CHECK(subscription);
}

TEST_CASE(
    "stateful Agent weak observer failure cannot veto strong commitment",
    "[agent][stateful][commitment][issue36]") {
    auto client = std::make_shared<ScriptedFakeRuntime>();
    agent::AsyncToolRegistry tools;
    agent::AsyncAgentOptions options;
    options.max_turns = 3;
    options.model = tests::make_model("fake-model");
    agent::Agent subject(client, std::move(tools), std::move(options));

    int failing_calls = 0;
    auto failing_result = subject.subscribe(
        [&](const agent::AgentLifecycleEvent&) -> util::ExpectedVoid {
            ++failing_calls;
            return std::unexpected(util::make_error(
                util::ErrorCode::Unknown, "observer failed"));
        });
    REQUIRE(failing_result);
    auto failing = std::move(*failing_result);

    int healthy_calls = 0;
    auto healthy_result = subject.subscribe(
        [&](const agent::AgentLifecycleEvent&) -> util::ExpectedVoid {
            ++healthy_calls;
            return {};
        });
    REQUIRE(healthy_result);
    auto healthy = std::move(*healthy_result);

    int committed_events = 0;
    REQUIRE(run_prompt(
        subject,
        "hello",
        [&](const agent::AgentLifecycleEvent&) -> util::ExpectedVoid {
            ++committed_events;
            return {};
        }));

    CHECK(failing_calls == 1);
    CHECK_FALSE(failing);
    CHECK(healthy);
    CHECK(healthy_calls == 11);
    CHECK(committed_events == 11);
}

TEST_CASE(
    "stateful Agent returns strong commitment failure with retained live state and recovers",
    "[agent][stateful][commitment][issue36]") {
    auto client = std::make_shared<ScriptedFakeRuntime>();
    agent::AsyncToolRegistry tools;
    agent::AsyncAgentOptions options;
    options.max_turns = 3;
    options.model = tests::make_model("fake-model");
    agent::Agent subject(client, std::move(tools), std::move(options));

    auto failed = run_prompt(
        subject,
        "first",
        [](const agent::AgentLifecycleEvent& event) -> util::ExpectedVoid {
            const auto* end = std::get_if<agent::MessageEndEvent>(&event);
            if (end != nullptr && std::holds_alternative<ai::UserMessage>(end->message)) {
                return std::unexpected(util::make_error(
                    util::ErrorCode::Session,
                    "commitment rejected user message"));
            }
            return {};
        });
    REQUIRE_FALSE(failed);
    CHECK(failed.error().code == util::ErrorCode::Session);
    CHECK(failed.error().message == "commitment rejected user message");
    CHECK_FALSE(subject.state().is_running);
    REQUIRE(subject.state().messages.size() == 1);

    REQUIRE(run_prompt(subject, "second"));
    CHECK(subject.state().messages.size() == 3);
}

TEST_CASE("stateful Agent keeps weak observer failure from vetoing a prompt", "[agent][stateful][issue35]") {
    auto client = std::make_shared<ScriptedFakeRuntime>();
    agent::AsyncToolRegistry tools;
    agent::AsyncAgentOptions options;
    options.max_turns = 3;
    options.model = tests::make_model("fake-model");
    agent::Agent subject(client, std::move(tools), std::move(options));

    int failing_calls = 0;
    auto failing_result = subject.subscribe(
        [&failing_calls](const agent::AgentLifecycleEvent&) -> util::ExpectedVoid {
            ++failing_calls;
            return std::unexpected(util::make_error(
                util::ErrorCode::Unknown,
                "observer failed"));
        });
    REQUIRE(failing_result);
    auto failing = std::move(*failing_result);

    int throwing_calls = 0;
    auto throwing_result = subject.subscribe(
        [&throwing_calls](const agent::AgentLifecycleEvent&) -> util::ExpectedVoid {
            ++throwing_calls;
            throw std::runtime_error("observer threw");
        });
    REQUIRE(throwing_result);
    auto throwing = std::move(*throwing_result);

    int healthy_calls = 0;
    auto healthy_result = subject.subscribe(
        [&healthy_calls](const agent::AgentLifecycleEvent&) -> util::ExpectedVoid {
            ++healthy_calls;
            return {};
        });
    REQUIRE(healthy_result);
    auto healthy = std::move(*healthy_result);

    REQUIRE(run_prompt(subject, "hello"));

    CHECK(failing_calls == 1);
    CHECK(throwing_calls == 1);
    CHECK_FALSE(failing);
    CHECK_FALSE(throwing);
    CHECK(healthy);
    CHECK(healthy_calls == 11);
    const auto snapshot = subject.state();
    CHECK(snapshot.messages.size() == 2);
    REQUIRE(snapshot.diagnostics.size() == 2);
    CHECK(snapshot.diagnostics[0].message == "agent event observer failed");
    CHECK(snapshot.diagnostics[0].detail.find("observer failed") != std::string::npos);
    CHECK(snapshot.diagnostics[1].message == "agent event observer failed");
    CHECK(snapshot.diagnostics[1].detail.find("observer threw") != std::string::npos);
}

TEST_CASE("stateful Agent bounds accumulated weak-observer diagnostics", "[agent][stateful][issue35]") {
    auto client = std::make_shared<ScriptedFakeRuntime>();
    agent::AsyncToolRegistry tools;
    agent::AsyncAgentOptions options;
    options.max_turns = 3;
    options.model = tests::make_model("fake-model");
    agent::Agent subject(client, std::move(tools), std::move(options));

    std::vector<agent::AgentEventSubscription> subscriptions;
    for (int index = 0; index < 20; ++index) {
        auto subscribed = subject.subscribe(
            [index](const agent::AgentLifecycleEvent&) -> util::ExpectedVoid {
                return std::unexpected(util::make_error(
                    util::ErrorCode::Unknown,
                    "observer " + std::to_string(index),
                    std::string(2048, 'x')));
            });
        REQUIRE(subscribed);
        subscriptions.push_back(std::move(*subscribed));
    }

    REQUIRE(run_prompt(subject, "hello"));

    const auto snapshot = subject.state();
    REQUIRE(snapshot.diagnostics.size() == 16);
    for (const auto& diagnostic : snapshot.diagnostics) {
        CHECK(diagnostic.message == "agent event observer failed");
        CHECK(diagnostic.detail.size() <= 1024);
    }
}

TEST_CASE("stateful Agent retains history while agent_end stays invocation-local", "[agent][stateful][issue35]") {
    auto client = std::make_shared<ScriptedFakeRuntime>();
    agent::AsyncToolRegistry tools;
    agent::AsyncAgentOptions options;
    options.max_turns = 3;
    options.model = tests::make_model("fake-model");
    agent::Agent subject(client, std::move(tools), std::move(options));

    std::vector<std::size_t> invocation_message_counts;
    auto subscribed = subject.subscribe(
        [&invocation_message_counts](const agent::AgentLifecycleEvent& event) {
            if (const auto* ended = std::get_if<agent::AgentEndEvent>(&event)) {
                invocation_message_counts.push_back(ended->messages.size());
            }
            return util::ExpectedVoid{};
        });
    REQUIRE(subscribed);
    auto subscription = std::move(*subscribed);

    REQUIRE(run_prompt(subject, "first"));
    REQUIRE(run_prompt(subject, "second"));

    const auto snapshot = subject.state();
    REQUIRE(snapshot.messages.size() == 4);
    CHECK(ai::text_from_user_message(std::get<ai::UserMessage>(snapshot.messages[0])) == "first");
    CHECK(ai::text_from_user_message(std::get<ai::UserMessage>(snapshot.messages[2])) == "second");
    const std::vector<std::size_t> expected_counts{2, 2};
    CHECK(invocation_message_counts == expected_counts);
    CHECK(subscription);
}

TEST_CASE("stateful Agent rejects a second prompt while its active run is suspended", "[agent][stateful][issue35]") {
    boost::asio::io_context io;
    auto client = std::make_shared<GatedModelRuntime>();
    agent::AsyncToolRegistry tools;
    agent::AsyncAgentOptions options;
    options.max_turns = 3;
    options.model = tests::make_model("fake-model");
    agent::Agent subject(client, std::move(tools), std::move(options));

    std::optional<util::ExpectedVoid> first_prompt;
    boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<void> {
            first_prompt = co_await subject.prompt("first");
            co_return;
        },
        boost::asio::detached);

    REQUIRE(io.run_one() == 1);
    REQUIRE(client->started);
    const auto active_snapshot = subject.state();
    CHECK(active_snapshot.is_running);
    REQUIRE(active_snapshot.messages.size() == 1);

    std::optional<util::ExpectedVoid> second_prompt;
    boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<void> {
            second_prompt = co_await subject.prompt("second");
            co_return;
        },
        boost::asio::detached);

    REQUIRE(io.run_one() == 1);
    REQUIRE(second_prompt.has_value());
    REQUIRE_FALSE(*second_prompt);
    CHECK(second_prompt->error().code == util::ErrorCode::Validation);
    CHECK(second_prompt->error().message == "agent is busy (prompt already in flight)");
    CHECK(subject.state().is_running);
    CHECK(subject.state().messages.size() == 1);

    client->release();
    io.run();

    REQUIRE(first_prompt.has_value());
    REQUIRE(*first_prompt);
    CHECK_FALSE(subject.state().is_running);
    REQUIRE(subject.state().messages.size() == 2);
}

TEST_CASE("stateful Agent keeps a suspended run valid when its handle is moved", "[agent][stateful][issue35]") {
    boost::asio::io_context io;
    auto client = std::make_shared<GatedModelRuntime>();
    agent::AsyncToolRegistry tools;
    agent::AsyncAgentOptions options;
    options.max_turns = 3;
    options.model = tests::make_model("fake-model");
    agent::Agent original(client, std::move(tools), std::move(options));

    std::optional<util::ExpectedVoid> prompted;
    boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<void> {
            prompted = co_await original.prompt("hello");
            co_return;
        },
        boost::asio::detached);
    REQUIRE(io.run_one() == 1);
    REQUIRE(client->started);

    agent::Agent moved(std::move(original));
    CHECK(moved.state().is_running);
    client->release();
    io.run();

    REQUIRE(prompted.has_value());
    REQUIRE(*prompted);
    CHECK_FALSE(moved.state().is_running);
    REQUIRE(moved.state().messages.size() == 2);
}

TEST_CASE("stateful Agent releases its active run after an unexpected provider exception", "[agent][stateful][issue35]") {
    auto client = std::make_shared<ThrowingThenRecoveringRuntime>();
    agent::AsyncToolRegistry tools;
    agent::AsyncAgentOptions options;
    options.max_turns = 3;
    options.model = tests::make_model("fake-model");
    agent::Agent subject(client, std::move(tools), std::move(options));

    const auto failed = run_prompt(subject, "first");
    REQUIRE_FALSE(failed);
    CHECK(failed.error().code == util::ErrorCode::Unknown);
    CHECK_FALSE(subject.state().is_running);

    REQUIRE(run_prompt(subject, "second"));
    CHECK_FALSE(subject.state().is_running);
    CHECK(client->calls == 2);
}

TEST_CASE("stateful Agent retains applied run-state updates after a later policy failure", "[agent][stateful][issue35]") {
    auto client = std::make_shared<RecordingModelRuntime>();
    client->responses.push_back(read_tool_call_response());
    client->responses.push_back(ai::assistant_text_message("first run reply"));
    client->responses.push_back(ai::assistant_text_message("second run reply"));

    agent::AsyncToolRegistry tools;
    REQUIRE(tools.add(std::make_unique<ReadTool>()));
    int prepared_turns = 0;
    int stop_decisions = 0;
    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("model-old");
    options.prepare_next_turn =
        agent::adapt_sync_prepare_next_turn(
            [&prepared_turns](const agent::PrepareNextTurnContext&)
        -> util::Expected<std::optional<agent::AgentLoopTurnUpdate>> {
        ++prepared_turns;
        if (prepared_turns == 1) {
            return agent::AgentLoopTurnUpdate{
                .context = std::nullopt,
                // The new model supports thinking, so the "high" update
                // survives model-switch re-clamping (#352).
                .model = tests::make_full_thinking_model("model-new"),
                .thinking_level = std::string{"high"},
            };
        }
        return std::nullopt;
    });
    options.validate_turn_update =
        agent::adapt_sync_validate_turn_update(
            [](const agent::AgentLoopTurnUpdate&)
        -> util::ExpectedVoid { return {}; });
    options.should_stop_after_turn =
        agent::adapt_sync_should_stop_after_turn(
            [&stop_decisions](const agent::PrepareNextTurnContext&)
        -> util::Expected<bool> {
        ++stop_decisions;
        if (stop_decisions == 2) {
            return std::unexpected(util::make_error(
                util::ErrorCode::Tool,
                "stop policy failed"));
        }
        return false;
    });

    agent::Agent subject(client, std::move(tools), std::move(options));

    const auto first = run_prompt(subject, "first");
    REQUIRE_FALSE(first);
    CHECK(first.error().message == "stop policy failed");
    CHECK(subject.state().model.id == "model-new");
    CHECK(subject.state().thinking_level == "high");

    REQUIRE(run_prompt(subject, "second"));
    const std::vector<std::string> expected_models{
        "model-old", "model-new", "model-new"};
    CHECK(client->request_models == expected_models);
    CHECK(subject.state().thinking_level == "high");
}

TEST_CASE("stateful Agent owns configured tool state through a fake tool run", "[agent][stateful][issue35]") {
    auto client = std::make_shared<ScriptedFakeRuntime>();
    agent::AsyncToolRegistry tools;
    REQUIRE(tools.add(std::make_unique<ReadTool>()));
    agent::AsyncAgentOptions options;
    options.max_turns = 3;
    options.model = tests::make_model("fake-model");
    agent::Agent subject(client, std::move(tools), std::move(options));

    const auto initial = subject.state();
    REQUIRE(initial.active_tool_names.size() == 1);
    CHECK(initial.active_tool_names[0] == "read");

    REQUIRE(run_prompt(subject, "read README.md"));

    const auto snapshot = subject.state();
    REQUIRE(snapshot.active_tool_names.size() == 1);
    CHECK(snapshot.active_tool_names[0] == "read");
    CHECK(snapshot.pending_tool_call_ids.empty());
    REQUIRE(snapshot.messages.size() == 4);
    REQUIRE(std::holds_alternative<ai::ToolResultMessage>(snapshot.messages[2]));
    CHECK(ai::text_from_content(
              std::get<ai::ToolResultMessage>(snapshot.messages[2]).content) ==
          "file contents");
}

TEST_CASE("stateful Agent retains its configured thinking state across a run", "[agent][stateful][issue35]") {
    auto client = std::make_shared<ScriptedFakeRuntime>();
    agent::AsyncToolRegistry tools;
    agent::AgentInitialState initial_state;
    initial_state.thinking_level = "high";
    agent::AsyncAgentOptions options;
    options.max_turns = 3;
    // A full-map reasoning model supports "high", so the configured level
    // survives creation-time clamping (#352).
    options.model = tests::make_full_thinking_model("fake-model");
    agent::Agent subject(
        client,
        std::move(tools),
        std::move(options),
        std::move(initial_state));

    REQUIRE(run_prompt(subject, "hello"));
    CHECK(subject.state().thinking_level == "high");
}

TEST_CASE(
    "stateful Agent admits bounded queues and drains steering before follow-up in pi order",
    "[agent][stateful][issue44]") {
    auto client = std::make_shared<RecordingModelRuntime>();
    client->responses.push_back(ai::assistant_text_message("first"));
    client->responses.push_back(ai::assistant_text_message("second"));
    client->responses.push_back(ai::assistant_text_message("third"));

    agent::AsyncAgentOptions options;
    options.model = tests::make_model("fake-model");
    options.max_queued_messages = 2;
    agent::Agent subject(client, agent::AsyncToolRegistry{}, std::move(options));

    REQUIRE(subject.steer(ai::user_text_message("steer-one")));
    REQUIRE(subject.steer(ai::user_text_message("steer-two")));
    REQUIRE(subject.follow_up(ai::user_text_message("follow-up")));

    const auto queued = subject.state();
    CHECK(queued.input_queues.max_messages == 2);
    CHECK(queued.input_queues.max_bytes == 16 * 1024 * 1024);
    CHECK(queued.input_queues.steering.mode == agent::InputQueueMode::OneAtATime);
    REQUIRE(queued.input_queues.steering.messages.size() == 2);
    REQUIRE(queued.input_queues.follow_up.messages.size() == 1);

    REQUIRE(run_prompt(subject, "prompt"));
    REQUIRE(client->request_models.size() == 3);
    const auto state = subject.state();
    CHECK(state.input_queues.steering.messages.empty());
    CHECK(state.input_queues.follow_up.messages.empty());
    REQUIRE(state.messages.size() == 7);
    CHECK(ai::text_from_user_message(std::get<ai::UserMessage>(state.messages[0])) == "prompt");
    CHECK(ai::text_from_user_message(std::get<ai::UserMessage>(state.messages[1])) == "steer-one");
    CHECK(ai::text_from_user_message(std::get<ai::UserMessage>(state.messages[3])) == "steer-two");
    CHECK(ai::text_from_user_message(std::get<ai::UserMessage>(state.messages[5])) == "follow-up");
}

TEST_CASE(
    "stateful Agent queue admission is atomic at UTF-8 byte and item boundaries",
    "[agent][stateful][issue44]") {
    auto client = std::make_shared<ScriptedFakeRuntime>();
    agent::AsyncAgentOptions options;
    options.model = tests::make_model("fake-model");
    options.max_queued_messages = 1;
    options.max_queued_bytes = 6;
    agent::Agent subject(client, agent::AsyncToolRegistry{}, std::move(options));

    const std::string multibyte = "\xC3\xA9\xC3\xA9\xC3\xA9";
    REQUIRE(multibyte.size() == 6);
    REQUIRE(subject.steer(ai::user_text_message(multibyte)));
    auto rejected_count = subject.steer(ai::user_text_message("x"));
    REQUIRE_FALSE(rejected_count);
    CHECK(rejected_count.error().message == "too many queued messages");
    auto rejected_again = subject.steer(ai::user_text_message("y"));
    REQUIRE_FALSE(rejected_again);
    CHECK(rejected_again.error().message == "too many queued messages");
    REQUIRE(subject.state().input_queues.steering.messages.size() == 1);
    CHECK(ai::text_from_user_message(
              std::get<ai::UserMessage>(
                  subject.state().input_queues.steering.messages.front())) ==
          multibyte);

    REQUIRE(subject.clear_steering_queue());
    auto rejected_bytes = subject.follow_up(ai::user_text_message(multibyte + "x"));
    REQUIRE_FALSE(rejected_bytes);
    CHECK(rejected_bytes.error().message == "queued messages too large");
    CHECK(subject.state().input_queues.follow_up.messages.empty());
    REQUIRE(subject.follow_up(ai::user_text_message(multibyte)));
    REQUIRE(subject.clear_input_queues());
    CHECK(subject.state().input_queues.steering.messages.empty());
    CHECK(subject.state().input_queues.follow_up.messages.empty());
}

TEST_CASE(
    "stateful Agent accepts steering and follow-up while streaming without disrupting the active run",
    "[agent][stateful][issue44]") {
    boost::asio::io_context io;
    auto client = std::make_shared<GatedModelRuntime>();
    agent::AsyncAgentOptions options;
    options.model = tests::make_model("fake-model");
    options.max_queued_messages = 1;
    agent::Agent subject(client, agent::AsyncToolRegistry{}, std::move(options));

    std::optional<util::ExpectedVoid> prompted;
    boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<void> {
            prompted = co_await subject.prompt("first");
            co_return;
        },
        boost::asio::detached);
    while (!client->started) {
        REQUIRE(io.poll_one() == 1);
    }

    REQUIRE(subject.steer(ai::user_text_message("steer")));
    REQUIRE(subject.follow_up(ai::user_text_message("follow")));
    auto rejected = subject.steer(ai::user_text_message("rejected"));
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().message == "too many queued messages");
    REQUIRE(subject.state().is_running);
    REQUIRE(subject.state().input_queues.steering.messages.size() == 1);
    REQUIRE(subject.state().input_queues.follow_up.messages.size() == 1);

    client->release();
    if (io.stopped()) {
        io.restart();
    }
    io.run();

    REQUIRE(prompted.has_value());
    REQUIRE(*prompted);
    CHECK_FALSE(subject.state().is_running);
    CHECK(subject.state().input_queues.steering.messages.empty());
    CHECK(subject.state().input_queues.follow_up.messages.empty());
    REQUIRE(subject.state().messages.size() == 6);
}

TEST_CASE("stateful Agent all queue modes preserve FIFO order", "[agent][stateful][issue44]") {
    auto client = std::make_shared<RecordingModelRuntime>();
    client->responses.push_back(ai::assistant_text_message("done"));
    agent::AsyncAgentOptions options;
    options.model = tests::make_model("fake-model");
    agent::Agent subject(client, agent::AsyncToolRegistry{}, std::move(options));

    REQUIRE(subject.set_steering_mode(agent::InputQueueMode::All));
    REQUIRE(subject.set_follow_up_mode(agent::InputQueueMode::All));
    CHECK(subject.state().input_queues.steering.mode == agent::InputQueueMode::All);
    CHECK(subject.state().input_queues.follow_up.mode == agent::InputQueueMode::All);
    REQUIRE(subject.steer(ai::user_text_message("first")));
    REQUIRE(subject.steer(ai::user_text_message("second")));
    REQUIRE(run_prompt(subject, "prompt"));
    const auto state = subject.state();
    REQUIRE(state.messages.size() == 4);
    CHECK(ai::text_from_user_message(std::get<ai::UserMessage>(state.messages[1])) == "first");
    CHECK(ai::text_from_user_message(std::get<ai::UserMessage>(state.messages[2])) == "second");
}

TEST_CASE("stateful Agent drains all follow-up messages together in FIFO order", "[agent][stateful][issue44]") {
    auto client = std::make_shared<RecordingModelRuntime>();
    client->responses.push_back(ai::assistant_text_message("first"));
    client->responses.push_back(ai::assistant_text_message("second"));
    agent::AsyncAgentOptions options;
    options.model = tests::make_model("fake-model");
    agent::Agent subject(client, agent::AsyncToolRegistry{}, std::move(options));

    REQUIRE(subject.set_follow_up_mode(agent::InputQueueMode::All));
    REQUIRE(subject.follow_up(ai::user_text_message("first follow-up")));
    REQUIRE(subject.follow_up(ai::user_text_message("second follow-up")));
    REQUIRE(run_prompt(subject, "prompt"));

    const auto state = subject.state();
    REQUIRE(state.messages.size() == 5);
    CHECK(ai::text_from_user_message(std::get<ai::UserMessage>(state.messages[2])) == "first follow-up");
    CHECK(ai::text_from_user_message(std::get<ai::UserMessage>(state.messages[3])) == "second follow-up");
}

TEST_CASE(
    "Agent::set_system_prompt reaches live state and the next stream request",
    "[agent][stateful][issue418]") {
    auto client = std::make_shared<ScriptedFakeRuntime>();
    agent::AsyncAgentOptions options;
    options.max_turns = 1;
    options.model = tests::make_model("fake-model");
    options.system_prompt = "original system prompt";
    agent::Agent subject(client, agent::AsyncToolRegistry{}, std::move(options));

    // The construction-time prompt mirrors into live state (pi
    // `agent.state.systemPrompt`).
    CHECK(subject.state().system_prompt == "original system prompt");

    // `/reload` rebuild: replace the prompt; live state advances in step.
    subject.set_system_prompt("rebuilt system prompt");
    CHECK(subject.state().system_prompt == "rebuilt system prompt");

    REQUIRE(run_prompt(subject, "hello"));
    REQUIRE_FALSE(client->calls.empty());
    // The rebuilt prompt seeds the next stream request's AiContext.
    CHECK(client->calls.front().context.system_prompt == "rebuilt system prompt");
    // Live state keeps the rebuilt prompt after the run settles.
    CHECK(subject.state().system_prompt == "rebuilt system prompt");
}
