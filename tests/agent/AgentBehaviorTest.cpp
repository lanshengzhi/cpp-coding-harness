#include <cch/agent/Agent.hpp>
#include <cch/ai/Content.hpp>
#include <cch/support/Error.hpp>
#include "agent/AgentMessageAccess.hpp"
#include "support/AsyncResultBridge.hpp"
#include "support/FakeModelStream.hpp"
#include "support/FakeTool.hpp"
#include "support/ModelFixture.hpp"
#include "support/PumpUntil.hpp"
#include "support/ToolArgumentContracts.hpp"
#include "support/ExpectedMacros.hpp"
#include "support/Json.hpp"

#include <catch2/catch_test_macros.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>

#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <semaphore>
#include <stop_token>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

using namespace cch;

namespace {

class FakeStreamingClient final : public std::enable_shared_from_this<FakeStreamingClient> {
public:
    [[nodiscard]] ai::ModelStreamFactory factory() {
        auto self = shared_from_this();
        return tests::adapt_stream_simple([self](ai::Model model,
                                                  ai::AiContext context,
                                                  ai::SimpleStreamOptions options,
                                                  ai::AssistantEventSink sink) {
            return self->stream_simple(std::move(model), std::move(context), std::move(options), std::move(sink));
        });
    }

    boost::asio::awaitable<support::Expected<ai::AssistantMessage>> stream_simple(
            ai::Model model, ai::AiContext context, ai::SimpleStreamOptions options, ai::AssistantEventSink sink) {
        requests.push_back(tests::RecordedStreamSimpleCall{std::move(model), std::move(context), std::move(options)});
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
    std::optional<support::Error> failure;
    std::vector<tests::RecordedStreamSimpleCall> requests;
};

class CancellationAwarePolicyClient final : public std::enable_shared_from_this<CancellationAwarePolicyClient> {
public:
    [[nodiscard]] ai::ModelStreamFactory factory() {
        auto self = shared_from_this();
        return tests::adapt_stream_simple([self](ai::Model model,
                                                  ai::AiContext context,
                                                  ai::SimpleStreamOptions options,
                                                  ai::AssistantEventSink sink) {
            return self->stream_simple(std::move(model), std::move(context), std::move(options), std::move(sink));
        });
    }

    boost::asio::awaitable<support::Expected<ai::AssistantMessage>> stream_simple(
            ai::Model model, ai::AiContext context, ai::SimpleStreamOptions options, ai::AssistantEventSink sink) {
        requests.push_back(tests::RecordedStreamSimpleCall{std::move(model), std::move(context), std::move(options)});
        if (requests.back().options.stop_token.stop_requested()) {
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
            co_return std::unexpected(support::make_error(support::ErrorCode::Provider, "no scripted policy response"));
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
    std::vector<tests::RecordedStreamSimpleCall> requests;
};

class TerminalOutcomeClient final : public std::enable_shared_from_this<TerminalOutcomeClient> {
public:
    explicit TerminalOutcomeClient(ai::AssistantMessage terminal) : terminal_(std::move(terminal)) {}

    [[nodiscard]] ai::ModelStreamFactory factory() {
        auto self = shared_from_this();
        return tests::adapt_stream_simple([self](ai::Model model,
                                                  ai::AiContext context,
                                                  ai::SimpleStreamOptions options,
                                                  ai::AssistantEventSink sink) {
            return self->stream_simple(std::move(model), std::move(context), std::move(options), std::move(sink));
        });
    }

    boost::asio::awaitable<support::Expected<ai::AssistantMessage>> stream_simple(
            ai::Model model, ai::AiContext context, ai::SimpleStreamOptions options, ai::AssistantEventSink sink) {
        requests.push_back(tests::RecordedStreamSimpleCall{std::move(model), std::move(context), std::move(options)});
        CCH_TRY_VOID(sink(ai::AssistantStartEvent{terminal_}));
        ++error_events;
        CCH_TRY_VOID(sink(ai::AssistantErrorEvent{terminal_.stop_reason, terminal_}));
        co_return terminal_;
    }

    ai::AssistantMessage terminal_;
    int error_events{0};
    std::vector<tests::RecordedStreamSimpleCall> requests;
};

/// A malformed host-provided runtime used to prove that the Agent presents one
/// assistant lifecycle even if a provider repeats its start event.
class DuplicateStartClient final : public std::enable_shared_from_this<DuplicateStartClient> {
public:
    explicit DuplicateStartClient(ai::AssistantMessage terminal) : terminal_(std::move(terminal)) {}

    [[nodiscard]] ai::ModelStreamFactory factory() {
        auto self = shared_from_this();
        return tests::adapt_stream_simple([self](ai::Model model,
                                                  ai::AiContext context,
                                                  ai::SimpleStreamOptions options,
                                                  ai::AssistantEventSink sink) {
            return self->stream_simple(std::move(model), std::move(context), std::move(options), std::move(sink));
        });
    }

    boost::asio::awaitable<support::Expected<ai::AssistantMessage>> stream_simple(
            ai::Model model, ai::AiContext context, ai::SimpleStreamOptions options, ai::AssistantEventSink sink) {
        requests.push_back(tests::RecordedStreamSimpleCall{std::move(model), std::move(context), std::move(options)});
        CCH_TRY_VOID(sink(ai::AssistantStartEvent{terminal_}));
        CCH_TRY_VOID(sink(ai::AssistantStartEvent{terminal_}));
        CCH_TRY_VOID(sink(ai::AssistantErrorEvent{terminal_.stop_reason, terminal_}));
        co_return terminal_;
    }

    ai::AssistantMessage terminal_;
    std::vector<tests::RecordedStreamSimpleCall> requests;
};

/// A conforming host-provided runtime whose accepted call reaches a terminal
/// event before any assistant start event and returns the same final
/// AssistantMessage through the value alternative.
class TerminalBeforeStartClient final : public std::enable_shared_from_this<TerminalBeforeStartClient> {
public:
    explicit TerminalBeforeStartClient(ai::AssistantMessage terminal) : terminal_(std::move(terminal)) {}

    [[nodiscard]] ai::ModelStreamFactory factory() {
        auto self = shared_from_this();
        return tests::adapt_stream_simple([self](ai::Model model,
                                                  ai::AiContext context,
                                                  ai::SimpleStreamOptions options,
                                                  ai::AssistantEventSink sink) {
            return self->stream_simple(std::move(model), std::move(context), std::move(options), std::move(sink));
        });
    }

    boost::asio::awaitable<support::Expected<ai::AssistantMessage>> stream_simple(
            ai::Model model, ai::AiContext context, ai::SimpleStreamOptions options, ai::AssistantEventSink sink) {
        requests.push_back(tests::RecordedStreamSimpleCall{std::move(model), std::move(context), std::move(options)});
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
    std::vector<tests::RecordedStreamSimpleCall> requests;
};

/// A stream fake whose accepted request parks on a gate until the test calls
/// release(), so the test can observe the Agent mid-run (busy rejection,
/// abort) deterministically. Mirrors the GatedModelRuntime prior art.
class GatedStreamingClient final : public std::enable_shared_from_this<GatedStreamingClient> {
public:
    [[nodiscard]] ai::ModelStreamFactory factory() {
        auto self = shared_from_this();
        return tests::adapt_stream_simple([self](ai::Model model,
                                                  ai::AiContext context,
                                                  ai::SimpleStreamOptions options,
                                                  ai::AssistantEventSink sink) {
            return self->stream_simple(std::move(model), std::move(context), std::move(options), std::move(sink));
        });
    }

    boost::asio::awaitable<support::Expected<ai::AssistantMessage>> stream_simple(
            ai::Model model, ai::AiContext context, ai::SimpleStreamOptions options, ai::AssistantEventSink sink) {
        requests.push_back(tests::RecordedStreamSimpleCall{std::move(model), std::move(context), std::move(options)});
        started = true;
        // The stop state shared with the Agent's run: the recorded call holds
        // the token, and a live alias keeps it usable while options moved.
        const std::stop_token live_token = requests.back().options.stop_token;
        if (requests.size() == 1) {
            auto executor = co_await boost::asio::this_coro::executor;
            gate.emplace(executor);
            gate->expires_at(std::chrono::steady_clock::time_point::max());
            std::stop_callback cancellation{live_token, [this] {
                                                if (gate) {
                                                    (void)gate->cancel();
                                                }
                                            }};
            boost::system::error_code error;
            co_await gate->async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
        }
        if (live_token.stop_requested()) {
            auto terminal = ai::assistant_text_message("");
            terminal.stop_reason = ai::AssistantStopReason::Aborted;
            terminal.error_message = "Request was aborted";
            CCH_TRY_VOID(sink(ai::AssistantErrorEvent{
                    .reason = ai::AssistantStopReason::Aborted,
                    .error = terminal,
            }));
            co_return terminal;
        }
        if (responses.empty()) {
            co_return ai::assistant_text_message("default fake response");
        }
        auto response = responses.front();
        responses.pop_front();
        CCH_TRY_VOID(sink(ai::AssistantStartEvent{response}));
        co_return response;
    }

    /// Release the gated request. Idempotent.
    void release() {
        if (gate) {
            (void)gate->cancel();
        }
    }

    std::deque<ai::AssistantMessage> responses;
    std::vector<tests::RecordedStreamSimpleCall> requests;
    bool started{false};
    std::optional<boost::asio::steady_timer> gate;
};

struct FakeToolState {
    std::vector<agent::ToolInvocation> invocations;
};

struct FakeToolHandle {
    agent::Tool tool;
    std::shared_ptr<FakeToolState> state;
};

[[nodiscard]] FakeToolHandle make_fake_tool(ai::Tool definition) {
    auto state = std::make_shared<FakeToolState>();
    return FakeToolHandle{
            tests::make_fake_tool(std::move(definition),
                    agent::ToolConcurrency::Exclusive,
                    [state](agent::ToolInvocation invocation, std::stop_token, agent::ToolUpdateSink)
                            -> boost::asio::awaitable<support::Expected<agent::AsyncToolExecutionResult>> {
                        state->invocations.push_back(invocation);
                        co_return agent::AsyncToolExecutionResult{
                                .content = std::vector<ai::Content>{ai::text_content("tool says ok")},
                                .details = std::nullopt,
                                .is_error = false};
                    }),
            state,
    };
}

struct CancellableFakeToolState {
    std::vector<agent::ToolInvocation> invocations;
    std::optional<std::stop_token> observed_stop_token;
    std::atomic<bool> suspended{false};
    std::binary_semaphore suspended_signal{0};
};

struct CancellableFakeToolHandle {
    agent::Tool tool;
    std::shared_ptr<CancellableFakeToolState> state;
};

[[nodiscard]] CancellableFakeToolHandle make_cancellable_fake_tool(ai::Tool definition) {
    auto state = std::make_shared<CancellableFakeToolState>();
    return CancellableFakeToolHandle{
            tests::make_fake_tool(std::move(definition),
                    agent::ToolConcurrency::Exclusive,
                    [state](agent::ToolInvocation invocation, std::stop_token stop_token, agent::ToolUpdateSink)
                            -> boost::asio::awaitable<support::Expected<agent::AsyncToolExecutionResult>> {
                        state->invocations.push_back(std::move(invocation));
                        state->observed_stop_token = stop_token;
                        boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor);
                        timer.expires_at(std::chrono::steady_clock::time_point::max());
                        // The stop callback runs on the requesting thread, but asio
                        // timers are single-threaded: post the cancel to the timer's
                        // executor so it can only land after async_wait is initiated
                        // (a direct cancel here races the wait registration — TSan,
                        // issue #473).
                        const auto timer_executor = timer.get_executor();
                        std::stop_callback cancellation{stop_token, [timer_executor, &timer] {
                                                            boost::asio::post(
                                                                    timer_executor, [&timer] { (void)timer.cancel(); });
                                                        }};
                        state->suspended.store(true, std::memory_order_release);
                        state->suspended_signal.release();
                        boost::system::error_code error;
                        co_await timer.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
                        if (stop_token.stop_requested()) {
                            co_return std::unexpected(
                                    support::make_error(support::ErrorCode::Cancelled, "Operation aborted"));
                        }
                        co_return agent::AsyncToolExecutionResult{};
                    }),
            state,
    };
}

struct AgentRun {
    support::ExpectedVoid result;
    std::vector<agent::AgentLifecycleEvent> events;
    agent::AgentState state;
};

AgentRun run_agent(agent::Agent& subject, support::AsyncResult<void> operation) {
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
                result = co_await support::detail::await_async_result(std::move(operation));
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

AgentRun run_agent(agent::Agent& subject, std::string prompt, agent::AgentEventCommitter commitment = {}) {
    return run_agent(subject, subject.prompt(std::move(prompt), std::move(commitment)));
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

ai::AssistantMessage tool_call_response(std::string raw_arguments = R"({"path":"README.md"})") {
    auto args = support::read_json(raw_arguments);
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

TEST_CASE("async tool registry owns tools and returns deterministic definitions", "[agent][u6][spec]") {
    static_assert(!std::is_copy_constructible_v<agent::ToolRegistry>);
    static_assert(std::is_move_constructible_v<agent::ToolRegistry>);

    agent::ToolRegistry registry;
    REQUIRE(registry.add(
            make_fake_tool(ai::Tool{"zed", "Zed tool", test::permissive_object_tool_argument_contract()}).tool));
    REQUIRE(registry.add(
            make_fake_tool(ai::Tool{"alpha", "Alpha tool", test::permissive_object_tool_argument_contract()}).tool));

    CHECK(registry.find("zed") != nullptr);
    CHECK(registry.find("alpha") != nullptr);
    CHECK(registry.find("missing") == nullptr);

    const auto definitions = registry.definitions();
    REQUIRE(definitions.size() == 2);
    CHECK(definitions[0].name == "alpha");
    CHECK(definitions[1].name == "zed");
}

TEST_CASE("async agent loop emits deterministic lifecycle events for text", "[agent][async][u5][spec]") {
    auto client = std::make_shared<FakeStreamingClient>();
    client->responses.push_back(ai::assistant_text_message("hello user"));
    agent::ToolRegistry registry;
    agent::AsyncAgentOptions options;
    options.max_turns = 3;
    options.model = tests::make_model("gpt-test");
    agent::Agent subject(client->factory(), std::move(registry), std::move(options));

    auto run = run_agent(subject, "hi");

    CHECK(run.result);
    CHECK(count_events<agent::TurnStartEvent>(run.events) == 1);
    CHECK(final_stop_reason(run.events) == ai::AssistantStopReason::Stop);
    REQUIRE(run.state.messages.size() == 2);
    CHECK(count_events<agent::AgentStartEvent>(run.events) == 1);
    CHECK(count_events<agent::TurnStartEvent>(run.events) == 1);
    CHECK(count_events<agent::MessageStartEvent>(run.events) == 2);
    CHECK(count_events<agent::MessageUpdateEvent>(run.events) == 1);
    CHECK(count_events<agent::MessageEndEvent>(run.events) == 2);
    CHECK(count_events<agent::TurnEndEvent>(run.events) == 1);
    CHECK(count_events<agent::AgentEndEvent>(run.events) == 1);
}

TEST_CASE("Agent continuation continues retained history without adding a prompt", "[agent][async][compat-pi][spec]") {
    auto client = std::make_shared<FakeStreamingClient>();
    client->responses.push_back(ai::assistant_text_message("continued"));
    agent::ToolRegistry registry;
    agent::AsyncAgentOptions options;
    options.model = tests::make_model("gpt-test");
    agent::AgentInitialState initial_state;
    initial_state.messages.emplace_back(ai::user_text_message("previous prompt"));
    agent::Agent subject(client->factory(), std::move(registry), std::move(options), std::move(initial_state));

    auto run = run_agent(subject, subject.continue_run());

    REQUIRE(run.result);
    REQUIRE(client->requests.size() == 1);
    REQUIRE(client->requests.front().context.messages.size() == 1);
    CHECK(std::holds_alternative<ai::UserMessage>(client->requests.front().context.messages.front()));
    REQUIRE(run.state.messages.size() == 2);
    CHECK(count_events<agent::MessageStartEvent>(run.events) == 1);
}

TEST_CASE("Agent continuation from an assistant tail prioritizes steering before follow-up",
        "[agent][async][compat-pi][spec]") {
    auto client = std::make_shared<FakeStreamingClient>();
    client->responses.push_back(ai::assistant_text_message("steering response"));
    client->responses.push_back(ai::assistant_text_message("follow-up response"));
    agent::ToolRegistry registry;
    agent::AsyncAgentOptions options;
    options.model = tests::make_model("gpt-test");
    agent::AgentInitialState initial_state;
    initial_state.messages.emplace_back(ai::user_text_message("previous prompt"));
    initial_state.messages.emplace_back(ai::assistant_text_message("previous answer"));
    agent::Agent subject(client->factory(), std::move(registry), std::move(options), std::move(initial_state));
    REQUIRE(subject.steer(ai::user_text_message("steering input")));
    REQUIRE(subject.follow_up(ai::user_text_message("follow-up input")));

    auto run = run_agent(subject, subject.continue_run());

    REQUIRE(run.result);
    REQUIRE(client->requests.size() == 2);
    const auto& first_messages = client->requests[0].context.messages;
    REQUIRE(first_messages.size() == 3);
    CHECK(ai::text_from_user_message(std::get<ai::UserMessage>(first_messages.back())) == "steering input");
    const auto& second_messages = client->requests[1].context.messages;
    REQUIRE(second_messages.size() == 5);
    CHECK(ai::text_from_user_message(std::get<ai::UserMessage>(second_messages.back())) == "follow-up input");
}

TEST_CASE("Agent continuation from an assistant tail falls back to queued follow-up messages",
        "[agent][async][compat-pi][spec]") {
    auto client = std::make_shared<FakeStreamingClient>();
    client->responses.push_back(ai::assistant_text_message("continued from follow-up"));
    agent::ToolRegistry registry;
    agent::AsyncAgentOptions options;
    options.model = tests::make_model("gpt-test");
    agent::AgentInitialState initial_state;
    initial_state.messages.emplace_back(ai::user_text_message("previous prompt"));
    initial_state.messages.emplace_back(ai::assistant_text_message("previous answer"));
    agent::Agent subject(client->factory(), std::move(registry), std::move(options), std::move(initial_state));
    REQUIRE(subject.follow_up(ai::user_text_message("follow-up continuation input")));

    auto run = run_agent(subject, subject.continue_run());

    REQUIRE(run.result);
    REQUIRE(client->requests.size() == 1);
    const auto& messages = client->requests.front().context.messages;
    REQUIRE(messages.size() == 3);
    CHECK(ai::text_from_user_message(std::get<ai::UserMessage>(messages.back())) == "follow-up continuation input");
}

TEST_CASE("Agent continuation from an assistant tail drains all configured steering messages",
        "[agent][async][compat-pi][spec]") {
    auto client = std::make_shared<FakeStreamingClient>();
    client->responses.push_back(ai::assistant_text_message("response"));
    agent::ToolRegistry registry;
    agent::AsyncAgentOptions options;
    options.model = tests::make_model("gpt-test");
    options.steering_mode = agent::InputQueueMode::All;
    agent::AgentInitialState initial_state;
    initial_state.messages.emplace_back(ai::user_text_message("previous prompt"));
    initial_state.messages.emplace_back(ai::assistant_text_message("previous answer"));
    agent::Agent subject(client->factory(), std::move(registry), std::move(options), std::move(initial_state));
    REQUIRE(subject.steer(ai::user_text_message("first steering input")));
    REQUIRE(subject.steer(ai::user_text_message("second steering input")));

    auto run = run_agent(subject, subject.continue_run());

    REQUIRE(run.result);
    REQUIRE(client->requests.size() == 1);
    const auto& messages = client->requests.front().context.messages;
    REQUIRE(messages.size() == 4);
    CHECK(ai::text_from_user_message(std::get<ai::UserMessage>(messages[2])) == "first steering input");
    CHECK(ai::text_from_user_message(std::get<ai::UserMessage>(messages[3])) == "second steering input");
}

TEST_CASE("Agent continuation rejects empty, system-only, and assistant-tail histories without fallback input",
        "[agent][async][compat-pi][spec]") {
    auto empty_client = std::make_shared<FakeStreamingClient>();
    agent::ToolRegistry empty_registry;
    agent::AsyncAgentOptions empty_options;
    empty_options.model = tests::make_model("gpt-test");
    agent::Agent empty_subject(empty_client->factory(), std::move(empty_registry), std::move(empty_options));
    auto empty_run = run_agent(empty_subject, empty_subject.continue_run());
    CHECK_FALSE(empty_run.result);
    CHECK(empty_client->requests.empty());

    auto system_client = std::make_shared<FakeStreamingClient>();
    agent::ToolRegistry system_registry;
    agent::AsyncAgentOptions system_options;
    system_options.model = tests::make_model("gpt-test");
    agent::AgentInitialState system_state;
    system_state.messages.emplace_back(ai::SystemMessage{.content = "system prompt"});
    agent::Agent system_subject(
            system_client->factory(), std::move(system_registry), std::move(system_options), std::move(system_state));
    REQUIRE(system_subject.follow_up(ai::user_text_message("must remain queued")));
    auto system_run = run_agent(system_subject, system_subject.continue_run());
    CHECK_FALSE(system_run.result);
    CHECK(system_client->requests.empty());
    CHECK(system_subject.input_queue_counts().follow_up == 1);

    auto assistant_client = std::make_shared<FakeStreamingClient>();
    agent::ToolRegistry assistant_registry;
    agent::AsyncAgentOptions assistant_options;
    assistant_options.model = tests::make_model("gpt-test");
    agent::AgentInitialState assistant_state;
    assistant_state.messages.emplace_back(ai::user_text_message("previous prompt"));
    assistant_state.messages.emplace_back(ai::assistant_text_message("previous answer"));
    agent::Agent assistant_subject(assistant_client->factory(),
            std::move(assistant_registry),
            std::move(assistant_options),
            std::move(assistant_state));
    auto assistant_run = run_agent(assistant_subject, assistant_subject.continue_run());
    CHECK_FALSE(assistant_run.result);
    CHECK(assistant_client->requests.empty());
}

TEST_CASE("Agent continuation honors OneAtATime steering drain without prematurely draining remaining input",
        "[agent][async][continuation][issue782][spec]") {
    auto client = std::make_shared<FakeStreamingClient>();
    client->responses.push_back(ai::assistant_text_message("first response"));
    client->responses.push_back(ai::assistant_text_message("second response"));
    client->responses.push_back(ai::assistant_text_message("third response"));
    agent::ToolRegistry registry;
    agent::AsyncAgentOptions options;
    options.model = tests::make_model("gpt-test");
    agent::AgentInitialState initial_state;
    initial_state.messages.emplace_back(ai::user_text_message("previous prompt"));
    initial_state.messages.emplace_back(ai::assistant_text_message("previous answer"));
    agent::Agent subject(client->factory(), std::move(registry), std::move(options), std::move(initial_state));
    REQUIRE(subject.steer(ai::user_text_message("first steer")));
    REQUIRE(subject.steer(ai::user_text_message("second steer")));
    REQUIRE(subject.steer(ai::user_text_message("third steer")));
    REQUIRE(subject.input_queue_counts().steering == 3);

    auto run = run_agent(subject, subject.continue_run());

    REQUIRE(run.result);
    CHECK(final_stop_reason(run.events) == ai::AssistantStopReason::Stop);

    // Three queued steers at OneAtATime => three turns, one steer per turn,
    // delivered in FIFO order. The first request carries only the first
    // steer: remaining queued input is not prematurely drained.
    REQUIRE(client->requests.size() == 3);
    const auto& first_messages = client->requests[0].context.messages;
    REQUIRE(first_messages.size() == 3);
    CHECK(ai::text_from_user_message(std::get<ai::UserMessage>(first_messages.back())) == "first steer");

    // Remaining input is consumed by the normal turn loop, one per turn,
    // in order.
    const auto& second_messages = client->requests[1].context.messages;
    REQUIRE(second_messages.size() == 5);
    CHECK(ai::text_from_user_message(std::get<ai::UserMessage>(second_messages.back())) == "second steer");
    const auto& third_messages = client->requests[2].context.messages;
    REQUIRE(third_messages.size() == 7);
    CHECK(ai::text_from_user_message(std::get<ai::UserMessage>(third_messages.back())) == "third steer");

    // All queued steering is consumed exactly once.
    CHECK(run.state.input_queues.steering.messages.empty());
    CHECK(run.state.input_queues.follow_up.messages.empty());

    // Each steer flows through the normal lifecycle as a user message.
    REQUIRE(count_events<agent::MessageStartEvent>(run.events) == 6);
    std::vector<std::string> user_texts;
    for (const auto& event : run.events) {
        if (const auto* start = std::get_if<agent::MessageStartEvent>(&event)) {
            if (const auto* user = std::get_if<ai::UserMessage>(&start->message)) {
                user_texts.push_back(ai::text_from_user_message(*user));
            }
        }
    }
    REQUIRE(user_texts.size() == 3);
    CHECK(user_texts[0] == "first steer");
    CHECK(user_texts[1] == "second steer");
    CHECK(user_texts[2] == "third steer");
}

TEST_CASE("Agent continuation during an active run is rejected before consuming queued input",
        "[agent][async][continuation][issue782][spec]") {
    boost::asio::io_context io;
    auto client = std::make_shared<GatedStreamingClient>();
    client->responses.push_back(ai::assistant_text_message("gated response"));
    agent::ToolRegistry registry;
    agent::AsyncAgentOptions options;
    options.model = tests::make_model("gpt-test");
    agent::AgentInitialState initial_state;
    initial_state.messages.emplace_back(ai::user_text_message("previous prompt"));
    initial_state.messages.emplace_back(ai::assistant_text_message("previous answer"));
    agent::Agent subject(client->factory(), std::move(registry), std::move(options), std::move(initial_state));
    REQUIRE(subject.steer(ai::user_text_message("queued steer one")));
    REQUIRE(subject.steer(ai::user_text_message("queued steer two")));
    REQUIRE(subject.follow_up(ai::user_text_message("queued follow-up")));
    const agent::AgentInputQueueCounts counts_before = subject.input_queue_counts();
    REQUIRE(counts_before.steering == 2);
    REQUIRE(counts_before.follow_up == 1);

    std::optional<support::ExpectedVoid> first_result;
    bool first_done = false;
    boost::asio::co_spawn(
            io,
            [&]() -> boost::asio::awaitable<void> {
                first_result = co_await support::detail::await_async_result(subject.continue_run());
                first_done = true;
                co_return;
            },
            boost::asio::detached);

    // Advance until the continuation's provider request is in flight. The
    // active run has already selected its own queued input (one steer at
    // OneAtATime) by this point.
    REQUIRE(io.run_one() == 1);
    REQUIRE(client->started);
    CHECK(subject.state().is_running);

    // A second continuation attempt while the run is active is rejected.
    auto second_run = run_agent(subject, subject.continue_run());
    REQUIRE_FALSE(second_run.result);
    CHECK(second_run.result.error().code == support::ErrorCode::Validation);
    CHECK(second_run.result.error().message == "agent is busy (prompt already in flight)");

    // The rejection consumed no queued input beyond the active run's own
    // selection and emitted no lifecycle events.
    const agent::AgentInputQueueCounts counts_after = subject.input_queue_counts();
    CHECK(counts_after.steering == counts_before.steering - 1);
    CHECK(counts_after.follow_up == counts_before.follow_up);
    CHECK(second_run.events.empty());
    CHECK(client->requests.size() == 1);

    // Release the gate so the active run completes on its own executor.
    client->release();
    io.run();
    REQUIRE(first_done);
    REQUIRE(first_result.has_value());
    REQUIRE(*first_result);
    CHECK_FALSE(subject.state().is_running);
    CHECK(subject.input_queue_counts().steering == 0);
}

TEST_CASE("Agent continuation with committer reduces state and delivers observers before commitment",
        "[agent][async][continuation][issue782][spec]") {
    auto client = std::make_shared<FakeStreamingClient>();
    client->responses.push_back(ai::assistant_text_message("committed reply"));
    agent::ToolRegistry registry;
    agent::AsyncAgentOptions options;
    options.model = tests::make_model("gpt-test");
    agent::AgentInitialState initial_state;
    initial_state.messages.emplace_back(ai::user_text_message("previous prompt"));
    agent::Agent subject(client->factory(), std::move(registry), std::move(options), std::move(initial_state));

    // Sequence stamps proving order: state reduction (observed via the
    // subscription snapshot) precedes weak observer delivery, which precedes
    // strong commitment for every committed event. One shared counter is
    // stamped by the observer and then by the committer within each event.
    int event_seq = 0;
    std::vector<std::string> order;
    std::vector<std::size_t> reduced_state_lengths;
    auto subscription_handle = subject.subscribe([&](const agent::AgentLifecycleEvent&) -> support::ExpectedVoid {
        // Snapshot the live reduced state as of this event's delivery.
        const auto snapshot = subject.state();
        reduced_state_lengths.push_back(snapshot.messages.size());
        order.push_back("observer:" + std::to_string(event_seq));
        return support::ExpectedVoid{};
    });
    REQUIRE(subscription_handle);

    int committed_events = 0;
    agent::AgentEventCommitter commitment = [&](const agent::AgentLifecycleEvent& event) -> support::ExpectedVoid {
        ++committed_events;
        order.push_back("commit:" + std::to_string(event_seq));
        ++event_seq;
        if (std::holds_alternative<agent::AgentEndEvent>(event)) {
            // Commitment failure on AgentEnd: the async result must report it
            // while already-reduced Agent state stays intact.
            return support::ExpectedVoid{std::unexpected(
                    support::make_error(support::ErrorCode::Validation, "commitment failed at agent end"))};
        }
        return support::ExpectedVoid{};
    };

    boost::asio::io_context io;
    std::optional<support::ExpectedVoid> result;
    boost::asio::co_spawn(
            io,
            [&]() -> boost::asio::awaitable<void> {
                result = co_await support::detail::await_async_result(subject.continue_run(std::move(commitment)));
                co_return;
            },
            boost::asio::detached);
    io.run();

    // Every committed event was stamped in order: observer before commitment.
    REQUIRE(order.size() > 0);
    REQUIRE(order.size() % 2 == 0);
    for (std::size_t i = 0; i < order.size(); i += 2) {
        CHECK(order[i] == "observer:" + std::to_string(i / 2));
        CHECK(order[i + 1] == "commit:" + std::to_string(i / 2));
    }

    // The commitment failure is reported through the async result...
    REQUIRE(result.has_value());
    REQUIRE_FALSE(*result);
    CHECK((*result).error().message == "commitment failed at agent end");

    // ...without rolling back the already-reduced Agent state: the user
    // history message plus the assistant response remain in live history.
    const auto final_state = subject.state();
    REQUIRE(final_state.messages.size() == 2);
    REQUIRE(std::holds_alternative<ai::UserMessage>(final_state.messages[0]));
    CHECK(ai::text_from_user_message(std::get<ai::UserMessage>(final_state.messages[0])) == "previous prompt");
    REQUIRE(std::holds_alternative<ai::AssistantMessage>(final_state.messages[1]));
    CHECK(ai::text_from_assistant_content(std::get<ai::AssistantMessage>(final_state.messages[1]).content) ==
            "committed reply");
    // State was reduced before observer delivery: at the final observer
    // delivery the live history already contained both messages.
    REQUIRE(reduced_state_lengths.size() >= 2);
    CHECK(reduced_state_lengths.back() == 2);
}

TEST_CASE("aborting an active continuation completes through the ordinary aborted lifecycle",
        "[agent][async][continuation][abort][issue782][spec]") {
    boost::asio::io_context io;
    auto client = std::make_shared<GatedStreamingClient>();
    agent::ToolRegistry registry;
    agent::AsyncAgentOptions options;
    options.model = tests::make_model("gpt-test");
    agent::AgentInitialState initial_state;
    initial_state.messages.emplace_back(ai::user_text_message("previous prompt"));
    initial_state.messages.emplace_back(ai::assistant_text_message("previous answer"));
    agent::Agent subject(client->factory(), std::move(registry), std::move(options), std::move(initial_state));
    REQUIRE(subject.steer(ai::user_text_message("steering to be interrupted")));

    std::vector<agent::AgentLifecycleEvent> events;
    auto subscribed = subject.subscribe([&events](const agent::AgentLifecycleEvent& event) {
        events.push_back(event);
        return support::ExpectedVoid{};
    });
    REQUIRE(subscribed);
    auto subscription = std::move(*subscribed);

    std::optional<support::ExpectedVoid> result;
    bool continuation_done = false;
    boost::asio::co_spawn(
            io,
            [&]() -> boost::asio::awaitable<void> {
                result = co_await support::detail::await_async_result(subject.continue_run());
                continuation_done = true;
                co_return;
            },
            boost::asio::detached);

    // Advance until the continuation's provider request is in flight.
    REQUIRE(io.run_one() == 1);
    REQUIRE(client->started);
    CHECK(subject.state().is_running);

    // Abort the active continuation through the ordinary cancellation path.
    subject.abort();

    // Bounded drain: after the stop request the run must settle on its own;
    // the backstop keeps a delivery failure from hanging the shard.
    tests::drain_ready(io);
    REQUIRE(continuation_done);
    REQUIRE(result.has_value());

    REQUIRE(*result);

    // The provider saw the stop signal and completed through the ordinary
    // aborted assistant lifecycle.
    REQUIRE(client->requests.size() == 1);
    CHECK(client->requests[0].options.stop_token.stop_requested());
    CHECK(final_stop_reason(events) == ai::AssistantStopReason::Aborted);
    CHECK(count_events<agent::AgentEndEvent>(events) == 1);
    CHECK(count_events<agent::TurnEndEvent>(events) == 1);

    // The Agent is idle after completion.
    const auto final_state = subject.state();
    CHECK_FALSE(final_state.is_running);
    CHECK_FALSE(final_state.streaming_message.has_value());
}

TEST_CASE("async agent loop seeds the session system prompt into every request context",
        "[agent][async][issue414][spec]") {
    auto client = std::make_shared<FakeStreamingClient>();
    client->responses.push_back(ai::assistant_text_message("first"));
    client->responses.push_back(ai::assistant_text_message("second"));
    agent::ToolRegistry registry;
    agent::AsyncAgentOptions options;
    options.max_turns = 3;
    options.model = tests::make_model("gpt-test");
    // The session System Prompt (pi `AgentState.systemPrompt`): seeded into
    // every per-run request context exactly like pi's `createContextSnapshot`.
    options.system_prompt = "You are an expert coding assistant operating inside pike, a coding "
                            "agent harness.\n\nCurrent working directory: /workspace";
    agent::Agent subject(client->factory(), std::move(registry), std::move(options));

    auto run = run_agent(subject, "hi");
    CHECK(run.result);
    REQUIRE(client->requests.size() == 1);
    REQUIRE(client->requests[0].context.system_prompt.has_value());
    const std::string expected_prompt = "You are an expert coding assistant operating inside pike, a coding "
                                        "agent harness.\n\nCurrent working directory: /workspace";
    CHECK(*client->requests[0].context.system_prompt == expected_prompt);
    CHECK(run.state.system_prompt == expected_prompt);

    // A second run re-seeds the same prompt from the loop options.
    auto second = run_agent(subject, "again");
    CHECK(second.result);
    REQUIRE(client->requests.size() == 2);
    REQUIRE(client->requests[1].context.system_prompt.has_value());
    CHECK(*client->requests[1].context.system_prompt == expected_prompt);
}

TEST_CASE("async agent loop synthesizes the assistant start for a done terminal before any start",
        "[agent][async][issue15][spec]") {
    auto terminal = ai::assistant_text_message("host reply without a streamed start");
    terminal.api = "host-api";
    terminal.provider = "host-provider";
    terminal.model = "gpt-test";
    auto client = std::make_shared<TerminalBeforeStartClient>(std::move(terminal));

    agent::ToolRegistry registry;
    agent::AsyncAgentOptions options;
    options.max_turns = 3;
    options.model = tests::make_model("gpt-test");
    agent::Agent subject(client->factory(), std::move(registry), std::move(options));
    auto run = run_agent(subject, "hello");

    CHECK(run.result.has_value());
    CHECK(final_stop_reason(run.events) == ai::AssistantStopReason::Stop);
    CHECK(count_events<agent::TurnStartEvent>(run.events) == 1);
    REQUIRE(run.state.messages.size() == 2);
    const auto* final = std::get_if<ai::AssistantMessage>(&run.state.messages.back());
    REQUIRE(final != nullptr);
    CHECK(ai::text_from_assistant_content(final->content) == "host reply without a streamed start");

    CHECK(client->requests.size() == 1);
    CHECK(client->terminal_events == 1);

    REQUIRE(run.events.size() == 8);
    std::size_t index = 0;
    CHECK(std::holds_alternative<agent::AgentStartEvent>(run.events[index++]));
    CHECK(std::holds_alternative<agent::TurnStartEvent>(run.events[index++]));
    CHECK(std::holds_alternative<agent::MessageStartEvent>(run.events[index++]));
    CHECK(std::holds_alternative<agent::MessageEndEvent>(run.events[index++]));
    const auto* assistant_start = std::get_if<agent::MessageStartEvent>(&run.events[index++]);
    REQUIRE(assistant_start != nullptr);
    REQUIRE(std::holds_alternative<ai::AssistantMessage>(assistant_start->message));
    CHECK(std::get<ai::AssistantMessage>(assistant_start->message).stop_reason == ai::AssistantStopReason::Stop);
    CHECK(std::holds_alternative<agent::MessageEndEvent>(run.events[index++]));
    const auto* turn_end = std::get_if<agent::TurnEndEvent>(&run.events[index++]);
    REQUIRE(turn_end != nullptr);
    CHECK(turn_end->tool_results.empty());
    CHECK(std::holds_alternative<agent::AgentEndEvent>(run.events[index++]));
}

TEST_CASE("async agent loop does not synthesize a duplicate after a streamed assistant start",
        "[agent][async][issue15][spec]") {
    ai::AssistantMessage terminal;
    terminal.api = "host-api";
    terminal.provider = "host-provider";
    terminal.model = "gpt-test";
    terminal.stop_reason = ai::AssistantStopReason::Error;
    terminal.error_message = "host transport lost after response start";
    auto client = std::make_shared<TerminalOutcomeClient>(std::move(terminal));

    agent::ToolRegistry registry;
    agent::AsyncAgentOptions options;
    options.max_turns = 3;
    options.model = tests::make_model("gpt-test");
    agent::Agent subject(client->factory(), std::move(registry), std::move(options));
    auto run = run_agent(subject, "hello");

    CHECK(run.result.has_value());
    CHECK(final_stop_reason(run.events) == ai::AssistantStopReason::Error);

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

TEST_CASE("async agent loop suppresses duplicate assistant starts from a host provider",
        "[agent][async][issue15][spec]") {
    ai::AssistantMessage terminal;
    terminal.api = "host-api";
    terminal.provider = "host-provider";
    terminal.model = "gpt-test";
    terminal.stop_reason = ai::AssistantStopReason::Error;
    terminal.error_message = "host transport lost after duplicate starts";
    auto client = std::make_shared<DuplicateStartClient>(std::move(terminal));

    agent::ToolRegistry registry;
    agent::AsyncAgentOptions options;
    options.max_turns = 3;
    options.model = tests::make_model("gpt-test");
    agent::Agent subject(client->factory(), std::move(registry), std::move(options));
    auto run = run_agent(subject, "hello");

    CHECK(run.result.has_value());
    CHECK(final_stop_reason(run.events) == ai::AssistantStopReason::Error);
    CHECK(client->requests.size() == 1);

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

TEST_CASE("async agent loop emits user message lifecycle before assistant response", "[agent][async][spec]") {
    auto client = std::make_shared<FakeStreamingClient>();
    client->responses.push_back(ai::assistant_text_message("hello user"));
    agent::ToolRegistry registry;
    agent::AsyncAgentOptions options;
    options.max_turns = 3;
    options.model = tests::make_model("gpt-test");
    agent::Agent subject(client->factory(), std::move(registry), std::move(options));

    auto run = run_agent(subject, "hi");

    CHECK(run.result);
    REQUIRE(run.state.messages.size() == 2);

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
    CHECK(ai::text_from_assistant_content(std::get<ai::AssistantMessage>(assistant_update->message).content) ==
            "hello user");

    const auto* assistant_end = std::get_if<agent::MessageEndEvent>(&run.events[index++]);
    REQUIRE(assistant_end);
    REQUIRE(std::holds_alternative<ai::AssistantMessage>(assistant_end->message));
    CHECK(ai::text_from_assistant_content(std::get<ai::AssistantMessage>(assistant_end->message).content) ==
            "hello user");

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
    CHECK(ai::text_from_assistant_content(std::get<ai::AssistantMessage>(agent_end->messages[1]).content) ==
            "hello user");

    CHECK(index == run.events.size());
}

TEST_CASE("async agent loop forwards thinking and tool-call stream lifecycle events", "[agent][async][u5][spec]") {
    auto client = std::make_shared<FakeStreamingClient>();
    ai::AssistantMessage message;
    message.stop_reason = ai::AssistantStopReason::ToolUse;
    message.content.emplace_back(ai::thinking_content("SECRET_THOUGHT"));
    message.content.emplace_back(ai::tool_call_content("call-1", "read_file", R"({"path":"README.md"})"));
    client->responses.push_back(std::move(message));
    client->responses.push_back(ai::assistant_text_message("done"));

    agent::ToolRegistry registry;
    REQUIRE(registry.add(make_fake_tool(ai::Tool{
                                                "read_file",
                                                "Read a workspace file",
                                                test::path_tool_argument_contract(),
                                        })
                    .tool));
    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    agent::Agent subject(client->factory(), std::move(registry), std::move(options));

    auto run = run_agent(subject, "read");

    CHECK(run.result);
    CHECK(count_events<agent::MessageUpdateEvent>(run.events) >= 1);
    CHECK(count_events<agent::MessageStartEvent>(run.events) == 4);
    CHECK(count_events<agent::MessageEndEvent>(run.events) == 4);
    CHECK(run.state.model.id == "gpt-test");
    CHECK(run.state.pending_tool_call_ids.empty());
    REQUIRE(run.state.active_tool_names.size() == 1);
    CHECK(run.state.active_tool_names[0] == "read_file");
    CHECK(run.state.messages.size() == client->requests.back().context.messages.size() + 1);
}

TEST_CASE("async agent loop executes tool calls and continues with tool result context",
        "[agent][async][u5][ae2][spec]") {
    auto client = std::make_shared<FakeStreamingClient>();
    client->responses.push_back(tool_call_response());
    client->responses.push_back(ai::assistant_text_message("done"));

    auto tool = make_fake_tool(ai::Tool{
            "read_file",
            "Read a workspace file",
            test::path_tool_argument_contract(),
    });
    auto* tool_ptr = tool.state.get();
    agent::ToolRegistry registry;
    REQUIRE(registry.add(std::move(tool.tool)));
    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    agent::Agent subject(client->factory(), std::move(registry), std::move(options));

    auto run = run_agent(subject, "read");

    CHECK(run.result);
    CHECK(count_events<agent::TurnStartEvent>(run.events) == 2);
    REQUIRE(tool_ptr->invocations.size() == 1);
    CHECK(tool_ptr->invocations[0].call_id == "call-1");
    CHECK(tool_ptr->invocations[0].name == "read_file");
    CHECK(tool_ptr->invocations[0].arguments.get<support::JsonValue::object_t>().at("path").get_string() ==
            "README.md");
    REQUIRE(client->requests.size() == 2);
    REQUIRE(client->requests[1].context.messages.size() == 3);
    REQUIRE(std::holds_alternative<ai::ToolResultMessage>(client->requests[1].context.messages.back()));
    CHECK(count_events<agent::MessageUpdateEvent>(run.events) >= 1);
    CHECK(count_events<agent::MessageStartEvent>(run.events) == 4);
    CHECK(count_events<agent::MessageEndEvent>(run.events) == 4);
    CHECK(count_events<agent::ToolExecutionStartEvent>(run.events) == 1);
    CHECK(count_events<agent::ToolExecutionEndEvent>(run.events) == 1);
    CHECK(run.state.pending_tool_call_ids.empty());
    REQUIRE(run.state.active_tool_names.size() == 1);
    CHECK(run.state.active_tool_names[0] == "read_file");
}

TEST_CASE("async agent loop turns malformed tool arguments into error tool results", "[agent][async][u5][spec]") {
    auto client = std::make_shared<FakeStreamingClient>();
    client->responses.push_back(tool_call_response("not-json"));
    client->responses.push_back(ai::assistant_text_message("saw error"));
    agent::ToolRegistry registry;
    REQUIRE(registry.add(
            make_fake_tool(ai::Tool{"read_file", "Read", test::permissive_object_tool_argument_contract()}).tool));
    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    agent::Agent subject(client->factory(), std::move(registry), std::move(options));

    auto run = run_agent(subject, "read");

    CHECK(run.result);
    REQUIRE(client->requests.size() == 2);
    REQUIRE(std::holds_alternative<ai::ToolResultMessage>(client->requests[1].context.messages.back()));
    const auto& result = std::get<ai::ToolResultMessage>(client->requests[1].context.messages.back());
    CHECK(result.is_error);
    CHECK(count_events<agent::ToolExecutionEndEvent>(run.events) == 1);
    CHECK(count_events<agent::MessageStartEvent>(run.events) == 4);
    CHECK(count_events<agent::MessageEndEvent>(run.events) == 4);
}

TEST_CASE("async agent loop default options impose no turn cap", "[agent][async][u5][issue68][spec]") {
    auto client = std::make_shared<FakeStreamingClient>();
    for (int turn = 0; turn < 9; ++turn) {
        client->responses.push_back(tool_call_response());
    }
    client->responses.push_back(ai::assistant_text_message("done"));
    agent::ToolRegistry registry;
    REQUIRE(registry.add(
            make_fake_tool(ai::Tool{"read_file", "Read", test::permissive_object_tool_argument_contract()}).tool));
    agent::AsyncAgentOptions options;
    options.model = tests::make_model("gpt-test");
    agent::Agent subject(client->factory(), std::move(registry), std::move(options));

    auto run = run_agent(subject, "read");

    CHECK(run.result);
    CHECK(count_events<agent::TurnStartEvent>(run.events) == 10);
    CHECK(final_stop_reason(run.events) == ai::AssistantStopReason::Stop);
    CHECK(count_events<agent::AgentEndEvent>(run.events) == 1);
}

TEST_CASE("async agent loop enforces an explicit host-set turn cap", "[agent][async][u5][issue68][spec]") {
    auto client = std::make_shared<FakeStreamingClient>();
    client->responses.push_back(tool_call_response());
    agent::ToolRegistry registry;
    REQUIRE(registry.add(
            make_fake_tool(ai::Tool{"read_file", "Read", test::permissive_object_tool_argument_contract()}).tool));
    agent::AsyncAgentOptions options;
    options.max_turns = 1;
    options.model = tests::make_model("gpt-test");
    agent::Agent subject(client->factory(), std::move(registry), std::move(options));

    auto run = run_agent(subject, "read");

    REQUIRE_FALSE(run.result);
    CHECK(run.result.error().code == support::ErrorCode::Validation);
    CHECK(run.result.error().message == "max turns exceeded");
    CHECK(count_events<agent::AgentEndEvent>(run.events) == 1);
}

TEST_CASE("agent queue bounds are documented configuration fields", "[agent][async][issue68][spec]") {
    const agent::AsyncAgentOptions options;
    CHECK(options.max_queued_messages == 256);
    CHECK(options.max_queued_bytes == 16 * 1024 * 1024);
}

TEST_CASE("beforeToolCall hook can block a tool call", "[agent][async][u7][spec]") {
    auto client = std::make_shared<FakeStreamingClient>();
    client->responses.push_back(tool_call_response());
    client->responses.push_back(ai::assistant_text_message("done"));

    auto tool = make_fake_tool(ai::Tool{"read_file", "Read", test::permissive_object_tool_argument_contract()});
    auto* tool_ptr = tool.state.get();
    agent::ToolRegistry registry;
    REQUIRE(registry.add(std::move(tool.tool)));

    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    options.before_tool_call = [](const agent::BeforeToolCallContext&,
                                       std::stop_token) -> support::AsyncResult<agent::BeforeToolCallResult> {
        return support::AsyncResult<agent::BeforeToolCallResult>{support::Expected<agent::BeforeToolCallResult>{
                agent::BeforeToolCallResult{.block = true, .reason = "blocked by policy"}}};
    };

    agent::Agent subject(client->factory(), std::move(registry), std::move(options));
    auto run = run_agent(subject, "read");

    CHECK(run.result);
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

    REQUIRE(client->requests.size() == 2);
    REQUIRE(std::holds_alternative<ai::ToolResultMessage>(client->requests[1].context.messages.back()));
    const auto& result = std::get<ai::ToolResultMessage>(client->requests[1].context.messages.back());
    CHECK(result.is_error);
    CHECK(ai::text_from_content(result.content) == "blocked by policy");
}

TEST_CASE("beforeToolCall hook passes context and skips execution on block", "[agent][async][u7][spec]") {
    auto client = std::make_shared<FakeStreamingClient>();
    client->responses.push_back(tool_call_response());
    client->responses.push_back(ai::assistant_text_message("done"));

    auto tool = make_fake_tool(ai::Tool{"read_file", "Read", test::permissive_object_tool_argument_contract()});
    auto* tool_ptr = tool.state.get();
    agent::ToolRegistry registry;
    REQUIRE(registry.add(std::move(tool.tool)));

    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    options.before_tool_call = [](const agent::BeforeToolCallContext& ctx,
                                       std::stop_token) -> support::AsyncResult<agent::BeforeToolCallResult> {
        REQUIRE(ctx.tool_call.name == "read_file");
        REQUIRE(ctx.args.get<support::JsonValue::object_t>().at("path").get_string() == "README.md");
        REQUIRE(!ctx.context.messages.empty());
        return support::AsyncResult<agent::BeforeToolCallResult>{
                support::Expected<agent::BeforeToolCallResult>{agent::BeforeToolCallResult{}}};
    };

    agent::Agent subject(client->factory(), std::move(registry), std::move(options));
    auto run = run_agent(subject, "read");

    CHECK(run.result);
    CHECK(tool_ptr->invocations.size() == 1);
    CHECK(count_events<agent::MessageStartEvent>(run.events) == 4);
    CHECK(count_events<agent::MessageEndEvent>(run.events) == 4);
}

TEST_CASE("beforeToolCall hook failure finalizes only its call", "[agent][async][u7][spec]") {
    auto client = std::make_shared<FakeStreamingClient>();
    client->responses.push_back(tool_call_response());
    client->responses.push_back(ai::assistant_text_message("recovered"));
    agent::ToolRegistry registry;
    REQUIRE(registry.add(
            make_fake_tool(ai::Tool{"read_file", "Read", test::permissive_object_tool_argument_contract()}).tool));

    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    options.before_tool_call = [](const agent::BeforeToolCallContext&,
                                       std::stop_token) -> support::AsyncResult<agent::BeforeToolCallResult> {
        return support::AsyncResult<agent::BeforeToolCallResult>{support::Expected<agent::BeforeToolCallResult>{
                std::unexpected(support::make_error(support::ErrorCode::Tool, "policy rejected"))}};
    };

    agent::Agent subject(client->factory(), std::move(registry), std::move(options));
    auto run = run_agent(subject, "read");

    // pi prepareToolCall catches a failing before hook into that call's error
    // result; the run continues (ADR 0008).
    CHECK(run.result);
    CHECK(count_events<agent::TurnStartEvent>(run.events) == 2);
    REQUIRE(client->requests.size() == 2);
    const auto& messages = client->requests[1].context.messages;
    REQUIRE(messages.size() == 3);
    const auto* tool_result = std::get_if<ai::ToolResultMessage>(&messages.back());
    REQUIRE(tool_result != nullptr);
    CHECK(tool_result->is_error);
    CHECK(ai::text_from_content(tool_result->content) == "policy rejected");
    CHECK(count_events<agent::MessageStartEvent>(run.events) == 4);
    CHECK(count_events<agent::MessageEndEvent>(run.events) == 4);
}

TEST_CASE(
        "beforeToolCall hook failure diagnostic becomes a per-call tool error", "[agent][async][u7][issue483][spec]") {
    auto client = std::make_shared<FakeStreamingClient>();
    client->responses.push_back(tool_call_response());
    client->responses.push_back(ai::assistant_text_message("recovered"));
    agent::ToolRegistry registry;
    REQUIRE(registry.add(
            make_fake_tool(ai::Tool{"read_file", "Read", test::permissive_object_tool_argument_contract()}).tool));

    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    options.before_tool_call = [](const agent::BeforeToolCallContext&,
                                       std::stop_token) -> support::AsyncResult<agent::BeforeToolCallResult> {
        return support::AsyncResult<agent::BeforeToolCallResult>{support::Expected<agent::BeforeToolCallResult>{
                std::unexpected(support::make_error(support::ErrorCode::Tool, "beforeToolCall hook failed", "boom"))}};
    };

    agent::Agent subject(client->factory(), std::move(registry), std::move(options));
    auto run = run_agent(subject, "read");

    CHECK(run.result);
    CHECK(count_events<agent::TurnStartEvent>(run.events) == 2);
    REQUIRE(client->requests.size() == 2);
    const auto& messages = client->requests[1].context.messages;
    REQUIRE(messages.size() == 3);
    const auto* tool_result = std::get_if<ai::ToolResultMessage>(&messages.back());
    REQUIRE(tool_result != nullptr);
    CHECK(tool_result->is_error);
    CHECK(ai::text_from_content(tool_result->content).find("boom") != std::string::npos);
    CHECK(count_events<agent::MessageStartEvent>(run.events) == 4);
    CHECK(count_events<agent::MessageEndEvent>(run.events) == 4);
}

TEST_CASE("afterToolCall hook overrides tool result content", "[agent][async][u7][spec]") {
    auto client = std::make_shared<FakeStreamingClient>();
    client->responses.push_back(tool_call_response());
    client->responses.push_back(ai::assistant_text_message("done"));
    agent::ToolRegistry registry;
    REQUIRE(registry.add(
            make_fake_tool(ai::Tool{"read_file", "Read", test::permissive_object_tool_argument_contract()}).tool));

    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    options.after_tool_call = [](const agent::AfterToolCallContext&,
                                      std::stop_token) -> support::AsyncResult<agent::AfterToolCallResult> {
        return support::AsyncResult<agent::AfterToolCallResult>{
                support::Expected<agent::AfterToolCallResult>{agent::AfterToolCallResult{
                        .content = std::vector<ai::Content>{ai::text_content("overridden")},
                        .details = std::nullopt,
                        .is_error = std::nullopt,
                        .terminate = std::nullopt,
                }}};
    };

    agent::Agent subject(client->factory(), std::move(registry), std::move(options));
    auto run = run_agent(subject, "read");

    CHECK(run.result);
    REQUIRE(client->requests.size() == 2);
    REQUIRE(std::holds_alternative<ai::ToolResultMessage>(client->requests[1].context.messages.back()));
    const auto& result = std::get<ai::ToolResultMessage>(client->requests[1].context.messages.back());
    CHECK(ai::text_from_content(result.content) == "overridden");
    CHECK(count_events<agent::MessageStartEvent>(run.events) == 4);
    CHECK(count_events<agent::MessageEndEvent>(run.events) == 4);
}

TEST_CASE("afterToolCall hook overrides error flag", "[agent][async][u7][spec]") {
    auto client = std::make_shared<FakeStreamingClient>();
    client->responses.push_back(tool_call_response());
    client->responses.push_back(ai::assistant_text_message("done"));
    agent::ToolRegistry registry;
    REQUIRE(registry.add(
            make_fake_tool(ai::Tool{"read_file", "Read", test::permissive_object_tool_argument_contract()}).tool));

    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    options.after_tool_call = [](const agent::AfterToolCallContext&,
                                      std::stop_token) -> support::AsyncResult<agent::AfterToolCallResult> {
        return support::AsyncResult<agent::AfterToolCallResult>{
                support::Expected<agent::AfterToolCallResult>{agent::AfterToolCallResult{
                        .content = std::nullopt,
                        .details = std::nullopt,
                        .is_error = true,
                        .terminate = std::nullopt,
                }}};
    };

    agent::Agent subject(client->factory(), std::move(registry), std::move(options));
    auto run = run_agent(subject, "read");

    CHECK(run.result);
    REQUIRE(client->requests.size() == 2);
    REQUIRE(std::holds_alternative<ai::ToolResultMessage>(client->requests[1].context.messages.back()));
    const auto& result = std::get<ai::ToolResultMessage>(client->requests[1].context.messages.back());
    CHECK(result.is_error);
    CHECK(count_events<agent::MessageStartEvent>(run.events) == 4);
    CHECK(count_events<agent::MessageEndEvent>(run.events) == 4);
}

TEST_CASE("afterToolCall terminate hint stops automatic continuation", "[agent][async][issue35][spec]") {
    auto client = std::make_shared<FakeStreamingClient>();
    client->responses.push_back(tool_call_response());
    agent::ToolRegistry registry;
    REQUIRE(registry.add(
            make_fake_tool(ai::Tool{"read_file", "Read", test::permissive_object_tool_argument_contract()}).tool));

    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    options.after_tool_call = [](const agent::AfterToolCallContext&,
                                      std::stop_token) -> support::AsyncResult<agent::AfterToolCallResult> {
        return support::AsyncResult<agent::AfterToolCallResult>{
                support::Expected<agent::AfterToolCallResult>{agent::AfterToolCallResult{
                        .content = std::nullopt,
                        .details = std::nullopt,
                        .is_error = std::nullopt,
                        .terminate = true,
                }}};
    };

    agent::Agent subject(client->factory(), std::move(registry), std::move(options));
    auto run = run_agent(subject, "read");

    CHECK(run.result);
    CHECK(count_events<agent::TurnStartEvent>(run.events) == 1);
    CHECK(final_stop_reason(run.events) == ai::AssistantStopReason::ToolUse);
    CHECK(client->requests.size() == 1);
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

struct ConfigurableFakeToolState {
    std::string result_text{"tool says ok"};
    std::vector<agent::ToolInvocation> invocations;
};

struct ConfigurableFakeToolHandle {
    agent::Tool tool;
    std::shared_ptr<ConfigurableFakeToolState> state;
};

[[nodiscard]] ConfigurableFakeToolHandle make_configurable_fake_tool(
        ai::Tool definition, agent::ToolConcurrency concurrency, std::string result_text = "tool says ok") {
    auto state = std::make_shared<ConfigurableFakeToolState>();
    state->result_text = std::move(result_text);
    return ConfigurableFakeToolHandle{
            tests::make_fake_tool(std::move(definition),
                    concurrency,
                    [state](agent::ToolInvocation invocation, std::stop_token, agent::ToolUpdateSink)
                            -> boost::asio::awaitable<support::Expected<agent::AsyncToolExecutionResult>> {
                        state->invocations.push_back(invocation);
                        co_return agent::AsyncToolExecutionResult{
                                .content = std::vector<ai::Content>{ai::text_content(state->result_text)},
                                .details = std::nullopt,
                                .is_error = false};
                    }),
            state,
    };
}

struct DelayedFakeToolState {
    std::chrono::milliseconds delay;
    std::string result_text{"tool says ok"};
    std::vector<agent::ToolInvocation> invocations;
};

struct DelayedFakeToolHandle {
    agent::Tool tool;
    std::shared_ptr<DelayedFakeToolState> state;
};

[[nodiscard]] DelayedFakeToolHandle make_delayed_fake_tool(
        ai::Tool definition, std::chrono::milliseconds delay, std::string result_text = "tool says ok") {
    auto state = std::make_shared<DelayedFakeToolState>();
    state->delay = delay;
    state->result_text = std::move(result_text);
    return DelayedFakeToolHandle{
            tests::make_fake_tool(std::move(definition),
                    agent::ToolConcurrency::ParallelSafe,
                    [state](agent::ToolInvocation invocation, std::stop_token, agent::ToolUpdateSink)
                            -> boost::asio::awaitable<support::Expected<agent::AsyncToolExecutionResult>> {
                        auto timer = boost::asio::steady_timer(co_await boost::asio::this_coro::executor, state->delay);
                        co_await timer.async_wait(boost::asio::use_awaitable);
                        state->invocations.push_back(invocation);
                        co_return agent::AsyncToolExecutionResult{
                                .content = std::vector<ai::Content>{ai::text_content(state->result_text)},
                                .details = std::nullopt,
                                .is_error = false};
                    }),
            state,
    };
}

struct FailingFakeToolState {
    std::vector<agent::ToolInvocation> invocations;
};

struct FailingFakeToolHandle {
    agent::Tool tool;
    std::shared_ptr<FailingFakeToolState> state;
};

[[nodiscard]] FailingFakeToolHandle make_failing_fake_tool(ai::Tool definition) {
    auto state = std::make_shared<FailingFakeToolState>();
    return FailingFakeToolHandle{
            tests::make_fake_tool(std::move(definition),
                    agent::ToolConcurrency::ParallelSafe,
                    [state](agent::ToolInvocation invocation, std::stop_token, agent::ToolUpdateSink)
                            -> boost::asio::awaitable<support::Expected<agent::AsyncToolExecutionResult>> {
                        state->invocations.push_back(invocation);
                        co_return std::unexpected(support::make_error(support::ErrorCode::Tool, "tool failed", "boom"));
                    }),
            state,
    };
}

struct ConcurrencyProbe {
    std::atomic<int> active{0};
    std::atomic<int> max_active{0};
};

struct ProbedFakeToolState {
    ConcurrencyProbe* probe{};
    std::string name;
    std::vector<agent::ToolInvocation> invocations;
};

struct ProbedFakeToolHandle {
    agent::Tool tool;
    std::shared_ptr<ProbedFakeToolState> state;
};

[[nodiscard]] ProbedFakeToolHandle make_probed_fake_tool(
        ai::Tool definition, agent::ToolConcurrency concurrency, ConcurrencyProbe& probe) {
    auto state = std::make_shared<ProbedFakeToolState>();
    state->probe = &probe;
    state->name = definition.name;
    return ProbedFakeToolHandle{
            tests::make_fake_tool(std::move(definition),
                    concurrency,
                    [state](agent::ToolInvocation invocation, std::stop_token, agent::ToolUpdateSink)
                            -> boost::asio::awaitable<support::Expected<agent::AsyncToolExecutionResult>> {
                        state->invocations.push_back(invocation);
                        const int current = ++state->probe->active;
                        int observed = state->probe->max_active.load();
                        while (current > observed &&
                                !state->probe->max_active.compare_exchange_weak(observed, current)) {
                        }
                        auto timer = boost::asio::steady_timer(
                                co_await boost::asio::this_coro::executor, std::chrono::milliseconds{30});
                        co_await timer.async_wait(boost::asio::use_awaitable);
                        --state->probe->active;
                        co_return agent::AsyncToolExecutionResult{
                                .content = std::vector<ai::Content>{ai::text_content(state->name + " result")},
                                .details = std::nullopt,
                                .is_error = false};
                    }),
            state,
    };
}

AgentRun run_agent_on_pool(agent::Agent& subject, std::string prompt, agent::AgentEventCommitter commitment = {}) {
    boost::asio::thread_pool pool{4};
    std::optional<support::ExpectedVoid> result;
    std::vector<agent::AgentLifecycleEvent> events;
    std::mutex events_mutex;
    auto subscribed = subject.subscribe([&events, &events_mutex](const agent::AgentLifecycleEvent& event) {
        std::lock_guard lock(events_mutex);
        events.push_back(event);
        return support::ExpectedVoid{};
    });
    REQUIRE(subscribed);
    auto subscription = std::move(*subscribed);

    boost::asio::co_spawn(
            pool,
            [&]() -> boost::asio::awaitable<void> {
                result = co_await support::detail::await_async_result(
                        subject.prompt(std::move(prompt), std::move(commitment)));
                co_return;
            },
            boost::asio::detached);

    pool.join();
    REQUIRE(result.has_value());
    return AgentRun{
            .result = std::move(*result),
            .events = std::move(events),
            .state = subject.state(),
    };
}

} // namespace

TEST_CASE("terminate batch continues when one call declines", "[agent][async][u7][spec]") {
    auto client = std::make_shared<FakeStreamingClient>();
    client->responses.push_back(two_tool_call_response());
    client->responses.push_back(ai::assistant_text_message("done"));

    agent::ToolRegistry registry;
    REQUIRE(registry.add(
            make_fake_tool(ai::Tool{"alpha", "Alpha", test::permissive_object_tool_argument_contract()}).tool));
    REQUIRE(registry.add(
            make_fake_tool(ai::Tool{"beta", "Beta", test::permissive_object_tool_argument_contract()}).tool));

    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    options.after_tool_call = [](const agent::AfterToolCallContext& ctx,
                                      std::stop_token) -> support::AsyncResult<agent::AfterToolCallResult> {
        if (ctx.tool_call.name == "alpha") {
            return support::AsyncResult<agent::AfterToolCallResult>{
                    support::Expected<agent::AfterToolCallResult>{agent::AfterToolCallResult{
                            .content = std::nullopt,
                            .details = std::nullopt,
                            .is_error = std::nullopt,
                            .terminate = true,
                    }}};
        }
        return support::AsyncResult<agent::AfterToolCallResult>{
                support::Expected<agent::AfterToolCallResult>{agent::AfterToolCallResult{
                        .content = std::nullopt,
                        .details = std::nullopt,
                        .is_error = std::nullopt,
                        .terminate = false,
                }}};
    };

    agent::Agent subject(client->factory(), std::move(registry), std::move(options));
    auto run = run_agent(subject, "read");

    CHECK(run.result);
    CHECK(count_events<agent::TurnStartEvent>(run.events) == 2);
    CHECK(final_stop_reason(run.events) == ai::AssistantStopReason::Stop);
    CHECK(count_events<agent::MessageStartEvent>(run.events) == 5);
    CHECK(count_events<agent::MessageEndEvent>(run.events) == 5);
}

TEST_CASE("blocked call prevents terminate batch", "[agent][async][u7][spec]") {
    auto client = std::make_shared<FakeStreamingClient>();
    client->responses.push_back(two_tool_call_response());
    client->responses.push_back(ai::assistant_text_message("done"));

    agent::ToolRegistry registry;
    REQUIRE(registry.add(
            make_fake_tool(ai::Tool{"alpha", "Alpha", test::permissive_object_tool_argument_contract()}).tool));
    REQUIRE(registry.add(
            make_fake_tool(ai::Tool{"beta", "Beta", test::permissive_object_tool_argument_contract()}).tool));

    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    options.before_tool_call = [](const agent::BeforeToolCallContext& ctx,
                                       std::stop_token) -> support::AsyncResult<agent::BeforeToolCallResult> {
        if (ctx.tool_call.name == "alpha") {
            return support::AsyncResult<agent::BeforeToolCallResult>{support::Expected<agent::BeforeToolCallResult>{
                    agent::BeforeToolCallResult{.block = true, .reason = "no alpha"}}};
        }
        return support::AsyncResult<agent::BeforeToolCallResult>{
                support::Expected<agent::BeforeToolCallResult>{agent::BeforeToolCallResult{}}};
    };
    options.after_tool_call = [](const agent::AfterToolCallContext&,
                                      std::stop_token) -> support::AsyncResult<agent::AfterToolCallResult> {
        return support::AsyncResult<agent::AfterToolCallResult>{
                support::Expected<agent::AfterToolCallResult>{agent::AfterToolCallResult{
                        .content = std::nullopt,
                        .details = std::nullopt,
                        .is_error = std::nullopt,
                        .terminate = true,
                }}};
    };

    agent::Agent subject(client->factory(), std::move(registry), std::move(options));
    auto run = run_agent(subject, "read");

    CHECK(run.result);
    CHECK(count_events<agent::TurnStartEvent>(run.events) == 2);
    CHECK(count_events<agent::MessageStartEvent>(run.events) == 5);
    CHECK(count_events<agent::MessageEndEvent>(run.events) == 5);
}

TEST_CASE("an error result with an explicit terminate hint still terminates the batch", "[agent][async][u7][spec]") {
    auto client = std::make_shared<FakeStreamingClient>();
    client->responses.push_back(two_tool_call_response());

    agent::ToolRegistry registry;
    REQUIRE(registry.add(
            make_fake_tool(ai::Tool{"alpha", "Alpha", test::permissive_object_tool_argument_contract()}).tool));
    REQUIRE(registry.add(
            make_fake_tool(ai::Tool{"beta", "Beta", test::permissive_object_tool_argument_contract()}).tool));

    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    options.after_tool_call = [](const agent::AfterToolCallContext& ctx,
                                      std::stop_token) -> support::AsyncResult<agent::AfterToolCallResult> {
        if (ctx.tool_call.name == "alpha") {
            return support::AsyncResult<agent::AfterToolCallResult>{
                    support::Expected<agent::AfterToolCallResult>{agent::AfterToolCallResult{
                            .content = std::nullopt,
                            .details = std::nullopt,
                            .is_error = true,
                            .terminate = true,
                    }}};
        }
        return support::AsyncResult<agent::AfterToolCallResult>{
                support::Expected<agent::AfterToolCallResult>{agent::AfterToolCallResult{
                        .content = std::nullopt,
                        .details = std::nullopt,
                        .is_error = std::nullopt,
                        .terminate = true,
                }}};
    };

    agent::Agent subject(client->factory(), std::move(registry), std::move(options));
    auto run = run_agent(subject, "read");

    // pi shouldTerminateToolBatch: every finalized result carries
    // terminate === true. The alpha error result still counts because the
    // after hook explicitly set the hint; there is no implicit ban on error
    // results (ADR 0008).
    CHECK(run.result);
    CHECK(count_events<agent::TurnStartEvent>(run.events) == 1);
    CHECK(final_stop_reason(run.events) == ai::AssistantStopReason::ToolUse);
    CHECK(client->requests.size() == 1);
    CHECK(count_events<agent::MessageStartEvent>(run.events) == 4);
    CHECK(count_events<agent::MessageEndEvent>(run.events) == 4);
}

TEST_CASE("Async Agent Loop continues after an afterToolCall hook failure", "[agent][async][u7][spec]") {
    auto client = std::make_shared<FakeStreamingClient>();
    client->responses.push_back(tool_call_response());
    client->responses.push_back(ai::assistant_text_message("recovered"));
    agent::ToolRegistry registry;
    REQUIRE(registry.add(
            make_fake_tool(ai::Tool{"read_file", "Read", test::permissive_object_tool_argument_contract()}).tool));

    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    options.after_tool_call = [](const agent::AfterToolCallContext&,
                                      std::stop_token) -> support::AsyncResult<agent::AfterToolCallResult> {
        return support::AsyncResult<agent::AfterToolCallResult>{support::Expected<agent::AfterToolCallResult>{
                std::unexpected(support::make_error(support::ErrorCode::Tool, "post-processor failed"))}};
    };

    agent::Agent subject(client->factory(), std::move(registry), std::move(options));
    auto run = run_agent(subject, "read");

    // pi finalizeExecutedToolCall catches a failing after hook into that
    // call's error result; the run continues (ADR 0008).
    CHECK(run.result);
    CHECK(count_events<agent::TurnStartEvent>(run.events) == 2);
    REQUIRE(client->requests.size() == 2);
    const auto& messages = client->requests[1].context.messages;
    REQUIRE(messages.size() == 3);
    const auto* tool_result = std::get_if<ai::ToolResultMessage>(&messages.back());
    REQUIRE(tool_result != nullptr);
    CHECK(tool_result->is_error);
    CHECK(ai::text_from_content(tool_result->content) == "post-processor failed");
    CHECK(count_events<agent::MessageStartEvent>(run.events) == 4);
    CHECK(count_events<agent::MessageEndEvent>(run.events) == 4);
}

TEST_CASE("afterToolCall hook failure diagnostic becomes a per-call tool error", "[agent][async][u7][issue483][spec]") {
    auto client = std::make_shared<FakeStreamingClient>();
    client->responses.push_back(tool_call_response());
    client->responses.push_back(ai::assistant_text_message("recovered"));
    agent::ToolRegistry registry;
    REQUIRE(registry.add(
            make_fake_tool(ai::Tool{"read_file", "Read", test::permissive_object_tool_argument_contract()}).tool));

    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    options.after_tool_call = [](const agent::AfterToolCallContext&,
                                      std::stop_token) -> support::AsyncResult<agent::AfterToolCallResult> {
        return support::AsyncResult<agent::AfterToolCallResult>{
                support::Expected<agent::AfterToolCallResult>{std::unexpected(
                        support::make_error(support::ErrorCode::Tool, "afterToolCall hook failed", "after boom"))}};
    };

    agent::Agent subject(client->factory(), std::move(registry), std::move(options));
    auto run = run_agent(subject, "read");

    CHECK(run.result);
    CHECK(count_events<agent::TurnStartEvent>(run.events) == 2);
    REQUIRE(client->requests.size() == 2);
    const auto& messages = client->requests[1].context.messages;
    REQUIRE(messages.size() == 3);
    const auto* tool_result = std::get_if<ai::ToolResultMessage>(&messages.back());
    REQUIRE(tool_result != nullptr);
    CHECK(tool_result->is_error);
    CHECK(ai::text_from_content(tool_result->content).find("after boom") != std::string::npos);
    CHECK(count_events<agent::MessageStartEvent>(run.events) == 4);
    CHECK(count_events<agent::MessageEndEvent>(run.events) == 4);
}

TEST_CASE("AsyncAgentOptions hooks are move-only", "[agent][async][u7][issue82][spec]") {
    static_assert(!std::is_copy_constructible_v<agent::AsyncAgentOptions>);
    static_assert(!std::is_copy_assignable_v<agent::AsyncAgentOptions>);
    static_assert(std::is_move_constructible_v<agent::AsyncAgentOptions>);
    static_assert(!std::is_copy_constructible_v<agent::TransformContextHook>);
    static_assert(!std::is_copy_constructible_v<agent::BeforeToolCallHook>);
    static_assert(!std::is_copy_constructible_v<agent::AfterToolCallHook>);
}

TEST_CASE("awaitable context policies suspend and resume in Agent executor order", "[agent][async][issue82][spec]") {
    boost::asio::io_context io;
    auto client = std::make_shared<FakeStreamingClient>();
    client->responses.push_back(ai::assistant_text_message("ok"));
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
                                        &gate](
                                        std::vector<ai::MessageVariant> messages, std::stop_token stop_token) mutable {
        return support::detail::make_async_result(
                [owned = std::move(owned),
                        &ordering,
                        &resumed_on_same_executor,
                        &stop_requested_after_resume,
                        &gate,
                        messages = std::move(messages),
                        stop_token]() mutable
                        -> boost::asio::awaitable<support::Expected<std::vector<ai::MessageVariant>>> {
                    const auto executor = co_await boost::asio::this_coro::executor;
                    CHECK(*owned == 41);
                    ordering.push_back("transform-start");
                    gate.emplace(executor);
                    gate->expires_at(std::chrono::steady_clock::time_point::max());
                    boost::system::error_code error;
                    co_await gate->async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
                    const auto resumed_executor = co_await boost::asio::this_coro::executor;
                    resumed_on_same_executor = resumed_executor == executor;
                    stop_requested_after_resume = stop_token.stop_requested();
                    ordering.push_back("transform-resume");
                    co_return messages;
                });
    };
    options.convert_to_llm = [&ordering](std::vector<ai::MessageVariant> messages) {
        ordering.push_back("convert");
        return support::AsyncResult<std::vector<ai::MessageVariant>>{std::move(messages)};
    };

    agent::Agent subject(client->factory(), agent::ToolRegistry{}, std::move(options));
    std::optional<support::ExpectedVoid> result;
    boost::asio::co_spawn(
            io,
            [&]() -> boost::asio::awaitable<void> {
                result = co_await support::detail::await_async_result(subject.prompt("hello"));
                co_return;
            },
            boost::asio::detached);

    while (!gate) {
        REQUIRE(io.run_one() == 1);
    }
    CHECK_FALSE(result.has_value());
    const std::vector<std::string> suspended{"transform-start"};
    CHECK(ordering == suspended);

    subject.abort();
    gate->cancel();
    io.run();

    REQUIRE(result.has_value());
    REQUIRE(*result);
    CHECK(resumed_on_same_executor);
    CHECK(stop_requested_after_resume);
    const std::vector<std::string> expected{"transform-start", "transform-resume", "convert"};
    CHECK(ordering == expected);
}

TEST_CASE("awaitable signal-bearing policies receive the active run stop token",
        "[agent][async][issue39][issue82][spec]") {
    auto client = std::make_shared<FakeStreamingClient>();
    client->responses.push_back(tool_call_response());
    client->responses.push_back(ai::assistant_text_message("done"));
    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");

    bool transform_stop_possible = false;
    bool before_stop_possible = false;
    bool after_stop_possible = false;
    options.transform_context =
            [&transform_stop_possible](std::vector<ai::MessageVariant> messages,
                    std::stop_token stop_token) -> support::AsyncResult<std::vector<ai::MessageVariant>> {
        transform_stop_possible = stop_token.stop_possible();
        return support::AsyncResult<std::vector<ai::MessageVariant>>{
                support::Expected<std::vector<ai::MessageVariant>>{messages}};
    };
    options.before_tool_call =
            [&before_stop_possible](agent::BeforeToolCallContext,
                    std::stop_token stop_token) -> support::AsyncResult<agent::BeforeToolCallResult> {
        before_stop_possible = stop_token.stop_possible();
        return support::AsyncResult<agent::BeforeToolCallResult>{
                support::Expected<agent::BeforeToolCallResult>{agent::BeforeToolCallResult{}}};
    };
    options.after_tool_call = [&after_stop_possible](agent::AfterToolCallContext,
                                      std::stop_token stop_token) -> support::AsyncResult<agent::AfterToolCallResult> {
        after_stop_possible = stop_token.stop_possible();
        return support::AsyncResult<agent::AfterToolCallResult>{
                support::Expected<agent::AfterToolCallResult>{agent::AfterToolCallResult{}}};
    };

    agent::ToolRegistry registry;
    REQUIRE(registry.add(
            make_fake_tool(ai::Tool{"read_file", "Read", test::permissive_object_tool_argument_contract()}).tool));
    agent::Agent subject(client->factory(), std::move(registry), std::move(options));
    auto run = run_agent(subject, "read");

    CHECK(run.result);
    CHECK(final_stop_reason(run.events) == ai::AssistantStopReason::Stop);
    CHECK(transform_stop_possible);
    CHECK(before_stop_possible);
    CHECK(after_stop_possible);
    CHECK(count_events<agent::AgentEndEvent>(run.events) == 1);
}

TEST_CASE("cancellation reaches a suspended tool and completes one ordinary aborted lifecycle",
        "[agent][async][abort][issue40][spec]") {
    boost::asio::io_context io;
    auto client = std::make_shared<CancellationAwarePolicyClient>();
    client->responses.push_back(tool_call_response());
    agent::AsyncAgentOptions options;
    options.model = tests::make_model("gpt-test");

    auto tool = make_cancellable_fake_tool(ai::Tool{
            .name = "read_file",
            .description = "Read",
            .parameters = test::permissive_object_tool_argument_contract(),
    });
    auto* tool_ptr = tool.state.get();
    agent::ToolRegistry registry;
    REQUIRE(registry.add(std::move(tool.tool)));
    agent::Agent subject(client->factory(), std::move(registry), std::move(options));

    std::vector<agent::AgentLifecycleEvent> events;
    auto subscribed = subject.subscribe([&events](const agent::AgentLifecycleEvent& event) {
        events.push_back(event);
        return support::ExpectedVoid{};
    });
    REQUIRE(subscribed);
    auto subscription = std::move(*subscribed);
    std::optional<support::ExpectedVoid> result;
    boost::asio::co_spawn(
            io,
            [&]() -> boost::asio::awaitable<void> {
                result = co_await support::detail::await_async_result(subject.prompt("cancel active tool"));
                co_return;
            },
            boost::asio::detached);

    // Run the Agent executor on a background thread while the tool suspends on
    // its private executor; the work guard keeps io.run() alive across the
    // suspension so it can deliver the tool's terminal outcome.
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work{boost::asio::make_work_guard(io)};
    std::jthread io_thread{[&] { io.run(); }};

    tool.state->suspended_signal.acquire();
    subject.abort();

    work.reset();
    io_thread.join();

    REQUIRE(result.has_value());
    REQUIRE(*result);
    CHECK(final_stop_reason(events) == ai::AssistantStopReason::Aborted);
    REQUIRE(tool_ptr->observed_stop_token.has_value());
    CHECK(tool_ptr->observed_stop_token->stop_requested());
    CHECK(tool_ptr->invocations.size() == 1);
    REQUIRE(client->requests.size() == 2);
    CHECK(client->requests[1].options.stop_token.stop_requested());
    CHECK(count_events<agent::AgentEndEvent>(events) == 1);
}

TEST_CASE("transform policy cancellation completes through an aborted provider turn",
        "[agent][async][abort][issue39][spec]") {
    auto client = std::make_shared<CancellationAwarePolicyClient>();
    agent::AsyncAgentOptions options;
    options.model = tests::make_model("gpt-test");
    // Stored policy hooks below run only while this test-owned Agent is alive.
    // The non-owning pointer must outlive the prompt that invokes the hook.
    agent::Agent* subject_ptr = nullptr;
    options.transform_context = [&subject_ptr](std::vector<ai::MessageVariant>, std::stop_token) {
        subject_ptr->abort();
        return support::AsyncResult<std::vector<ai::MessageVariant>>{
                std::unexpected(support::make_error(support::ErrorCode::Cancelled, "transform policy cancelled"))};
    };

    agent::Agent subject(client->factory(), agent::ToolRegistry{}, std::move(options));
    subject_ptr = &subject;
    auto run = run_agent(subject, "cancel transform");

    CHECK(run.result.has_value());
    CHECK(final_stop_reason(run.events) == ai::AssistantStopReason::Aborted);
    REQUIRE(client->requests.size() == 1);
    CHECK(client->requests[0].options.stop_token.stop_requested());
    CHECK(count_events<agent::MessageEndEvent>(run.events) == 2);
    CHECK(count_events<agent::TurnEndEvent>(run.events) == 1);
    CHECK(count_events<agent::AgentEndEvent>(run.events) == 1);
}

TEST_CASE("before-tool policy cancellation skips the tool and reaches an aborted provider turn",
        "[agent][async][abort][issue39][issue40][spec]") {
    auto client = std::make_shared<CancellationAwarePolicyClient>();
    client->responses.push_back(tool_call_response());
    // Stored policy hooks below run only while this test-owned Agent is alive.
    // The non-owning pointer must outlive the prompt that invokes the hook.
    agent::Agent* subject_ptr = nullptr;
    agent::AsyncAgentOptions options;
    options.max_turns = 1;
    options.model = tests::make_model("gpt-test");
    options.before_tool_call = [&subject_ptr](agent::BeforeToolCallContext, std::stop_token) {
        subject_ptr->abort();
        return support::AsyncResult<agent::BeforeToolCallResult>{
                std::unexpected(support::make_error(support::ErrorCode::Cancelled, "before-tool policy cancelled"))};
    };

    auto tool = make_fake_tool(ai::Tool{
            .name = "read_file",
            .description = "Read",
            .parameters = test::permissive_object_tool_argument_contract(),
    });
    auto* tool_ptr = tool.state.get();
    agent::ToolRegistry registry;
    REQUIRE(registry.add(std::move(tool.tool)));
    agent::Agent subject(client->factory(), std::move(registry), std::move(options));
    subject_ptr = &subject;
    auto run = run_agent(subject, "cancel before tool");

    CHECK(run.result.has_value());
    CHECK(final_stop_reason(run.events) == ai::AssistantStopReason::Aborted);
    CHECK(count_events<agent::TurnStartEvent>(run.events) == 2);
    CHECK(tool_ptr->invocations.empty());
    REQUIRE(client->requests.size() == 2);
    CHECK(client->requests[1].options.stop_token.stop_requested());
    CHECK(count_events<agent::TurnEndEvent>(run.events) == 2);
    CHECK(count_events<agent::AgentEndEvent>(run.events) == 1);
}

TEST_CASE("after-tool policy cancellation keeps the tool result and reaches an aborted provider turn",
        "[agent][async][abort][issue39][issue40][spec]") {
    auto client = std::make_shared<CancellationAwarePolicyClient>();
    client->responses.push_back(tool_call_response());
    // Stored policy hooks below run only while this test-owned Agent is alive.
    // The non-owning pointer must outlive the prompt that invokes the hook.
    agent::Agent* subject_ptr = nullptr;
    agent::AsyncAgentOptions options;
    options.model = tests::make_model("gpt-test");
    options.after_tool_call = [&subject_ptr](agent::AfterToolCallContext, std::stop_token) {
        subject_ptr->abort();
        return support::AsyncResult<agent::AfterToolCallResult>{
                std::unexpected(support::make_error(support::ErrorCode::Cancelled, "after-tool policy cancelled"))};
    };

    auto tool = make_fake_tool(ai::Tool{
            .name = "read_file",
            .description = "Read",
            .parameters = test::permissive_object_tool_argument_contract(),
    });
    auto* tool_ptr = tool.state.get();
    agent::ToolRegistry registry;
    REQUIRE(registry.add(std::move(tool.tool)));
    agent::Agent subject(client->factory(), std::move(registry), std::move(options));
    subject_ptr = &subject;
    auto run = run_agent(subject, "cancel after tool");

    CHECK(run.result.has_value());
    CHECK(final_stop_reason(run.events) == ai::AssistantStopReason::Aborted);
    REQUIRE(tool_ptr->invocations.size() == 1);
    REQUIRE(client->requests.size() == 2);
    CHECK(client->requests[1].options.stop_token.stop_requested());
    CHECK(count_events<agent::TurnEndEvent>(run.events) == 2);
    CHECK(count_events<agent::AgentEndEvent>(run.events) == 1);
}

TEST_CASE("awaitable policy failures after suspension stay in the existing error channel",
        "[agent][async][issue82][issue483][spec]") {
    auto client = std::make_shared<FakeStreamingClient>();
    client->responses.push_back(ai::assistant_text_message("unused"));
    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    options.transform_context = [](std::vector<ai::MessageVariant> messages, std::stop_token) {
        return support::detail::make_async_result(
                [messages = std::move(messages)]() mutable
                        -> boost::asio::awaitable<support::Expected<std::vector<ai::MessageVariant>>> {
                    auto timer = boost::asio::steady_timer(co_await boost::asio::this_coro::executor);
                    timer.expires_after(std::chrono::milliseconds{0});
                    co_await timer.async_wait(boost::asio::use_awaitable);
                    co_return std::unexpected(support::make_error(
                            support::ErrorCode::Tool, "transformContext hook failed", "suspended transform boom"));
                });
    };

    agent::Agent subject(client->factory(), agent::ToolRegistry{}, std::move(options));
    auto run = run_agent(subject, "hello");

    REQUIRE_FALSE(run.result);
    CHECK(run.result.error().code == support::ErrorCode::Tool);
    CHECK(run.result.error().message == "transformContext hook failed");
    CHECK(run.result.error().detail.find("suspended transform boom") != std::string::npos);
    CHECK(count_events<agent::AgentEndEvent>(run.events) == 1);
}

TEST_CASE("transformContext hook prunes old messages from LLM request", "[agent][async][u8][spec]") {
    auto client = std::make_shared<FakeStreamingClient>();
    client->responses.push_back(ai::assistant_text_message("ok"));
    agent::ToolRegistry registry;

    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    options.transform_context = [](const std::vector<ai::MessageVariant>& messages,
                                        std::stop_token) -> support::AsyncResult<std::vector<ai::MessageVariant>> {
        if (messages.size() <= 1) {
            return support::AsyncResult<std::vector<ai::MessageVariant>>{
                    support::Expected<std::vector<ai::MessageVariant>>{messages}};
        }
        return support::AsyncResult<std::vector<ai::MessageVariant>>{
                support::Expected<std::vector<ai::MessageVariant>>{std::vector<ai::MessageVariant>{messages.back()}}};
    };

    agent::Agent subject(client->factory(), std::move(registry), std::move(options));
    auto run = run_agent(subject, "hi");

    CHECK(run.result);
    REQUIRE(client->requests.size() == 1);
    REQUIRE(client->requests[0].context.messages.size() == 1);
    REQUIRE(std::holds_alternative<ai::UserMessage>(client->requests[0].context.messages[0]));
    REQUIRE(run.state.messages.size() == 2);
}

TEST_CASE("convertToLlm hook filters non-LLM messages", "[agent][async][u8][spec]") {
    auto client = std::make_shared<FakeStreamingClient>();
    client->responses.push_back(ai::assistant_text_message("ok"));
    agent::ToolRegistry registry;

    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    options.convert_to_llm = [](const std::vector<ai::MessageVariant>& messages)
            -> support::AsyncResult<std::vector<ai::MessageVariant>> {
        std::vector<ai::MessageVariant> result;
        for (const auto& message : messages) {
            if (std::holds_alternative<ai::UserMessage>(message) ||
                    std::holds_alternative<ai::AssistantMessage>(message) ||
                    std::holds_alternative<ai::ToolResultMessage>(message)) {
                result.push_back(message);
            }
        }
        return support::AsyncResult<std::vector<ai::MessageVariant>>{
                support::Expected<std::vector<ai::MessageVariant>>{result}};
    };

    agent::Agent subject(client->factory(), std::move(registry), std::move(options));
    auto run = run_agent(subject, "hi");

    CHECK(run.result);
    REQUIRE(client->requests.size() == 1);
    REQUIRE(client->requests[0].context.messages.size() == 1);
}

TEST_CASE("convertToLlm returning empty aborts with validation error", "[agent][async][u8][spec]") {
    auto client = std::make_shared<FakeStreamingClient>();
    client->responses.push_back(ai::assistant_text_message("ok"));
    agent::ToolRegistry registry;

    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    options.convert_to_llm =
            [](const std::vector<ai::MessageVariant>&) -> support::AsyncResult<std::vector<ai::MessageVariant>> {
        return support::AsyncResult<std::vector<ai::MessageVariant>>{
                support::Expected<std::vector<ai::MessageVariant>>{std::vector<ai::MessageVariant>{}}};
    };

    agent::Agent subject(client->factory(), std::move(registry), std::move(options));
    auto run = run_agent(subject, "hi");

    REQUIRE_FALSE(run.result);
    CHECK(run.result.error().code == support::ErrorCode::Validation);
    const auto* end_event = std::get_if<agent::AgentEndEvent>(&run.events.back());
    REQUIRE(end_event);
}

TEST_CASE("transformContext hook error aborts the run", "[agent][async][u8][spec]") {
    auto client = std::make_shared<FakeStreamingClient>();
    client->responses.push_back(ai::assistant_text_message("ok"));
    agent::ToolRegistry registry;

    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    options.transform_context = [](const std::vector<ai::MessageVariant>&,
                                        std::stop_token) -> support::AsyncResult<std::vector<ai::MessageVariant>> {
        return support::AsyncResult<std::vector<ai::MessageVariant>>{support::Expected<std::vector<ai::MessageVariant>>{
                std::unexpected(support::make_error(support::ErrorCode::Tool, "context transform failed"))}};
    };

    agent::Agent subject(client->factory(), std::move(registry), std::move(options));
    auto run = run_agent(subject, "hi");

    REQUIRE_FALSE(run.result);
    CHECK(run.result.error().code == support::ErrorCode::Tool);
    CHECK(run.result.error().message == "context transform failed");
}

TEST_CASE("convertToLlm hook error aborts the run", "[agent][async][u8][spec]") {
    auto client = std::make_shared<FakeStreamingClient>();
    client->responses.push_back(ai::assistant_text_message("ok"));
    agent::ToolRegistry registry;

    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    options.convert_to_llm =
            [](const std::vector<ai::MessageVariant>&) -> support::AsyncResult<std::vector<ai::MessageVariant>> {
        return support::AsyncResult<std::vector<ai::MessageVariant>>{support::Expected<std::vector<ai::MessageVariant>>{
                std::unexpected(support::make_error(support::ErrorCode::Tool, "conversion failed"))}};
    };

    agent::Agent subject(client->factory(), std::move(registry), std::move(options));
    auto run = run_agent(subject, "hi");

    REQUIRE_FALSE(run.result);
    CHECK(run.result.error().code == support::ErrorCode::Tool);
    CHECK(run.result.error().message == "conversion failed");
}

TEST_CASE("transformContext and convertToLlm failure diagnostics abort cleanly", "[agent][async][u8][issue483][spec]") {
    {
        auto client = std::make_shared<FakeStreamingClient>();
        client->responses.push_back(ai::assistant_text_message("ok"));
        agent::ToolRegistry registry;

        agent::AsyncAgentOptions options;
        options.max_turns = 4;
        options.model = tests::make_model("gpt-test");
        options.transform_context = [](const std::vector<ai::MessageVariant>&,
                                            std::stop_token) -> support::AsyncResult<std::vector<ai::MessageVariant>> {
            return support::AsyncResult<std::vector<ai::MessageVariant>>{
                    support::Expected<std::vector<ai::MessageVariant>>{std::unexpected(support::make_error(
                            support::ErrorCode::Tool, "transformContext hook failed", "transform boom"))}};
        };

        agent::Agent subject(client->factory(), std::move(registry), std::move(options));
        auto run = run_agent(subject, "hi");

        REQUIRE_FALSE(run.result);
        CHECK(run.result.error().code == support::ErrorCode::Tool);
        CHECK(run.result.error().message == "transformContext hook failed");
        CHECK(run.result.error().detail.find("transform boom") != std::string::npos);
    }

    {
        auto client = std::make_shared<FakeStreamingClient>();
        client->responses.push_back(ai::assistant_text_message("ok"));
        agent::ToolRegistry registry;

        agent::AsyncAgentOptions options;
        options.max_turns = 4;
        options.model = tests::make_model("gpt-test");
        options.convert_to_llm =
                [](const std::vector<ai::MessageVariant>&) -> support::AsyncResult<std::vector<ai::MessageVariant>> {
            return support::AsyncResult<std::vector<ai::MessageVariant>>{
                    support::Expected<std::vector<ai::MessageVariant>>{std::unexpected(support::make_error(
                            support::ErrorCode::Tool, "convertToLlm hook failed", "convert boom"))}};
        };

        agent::Agent subject(client->factory(), std::move(registry), std::move(options));
        auto run = run_agent(subject, "hi");

        REQUIRE_FALSE(run.result);
        CHECK(run.result.error().code == support::ErrorCode::Tool);
        CHECK(run.result.error().message == "convertToLlm hook failed");
        CHECK(run.result.error().detail.find("convert boom") != std::string::npos);
    }
}

TEST_CASE("agent_end contains only messages from the current invocation", "[agent][async][issue35][spec]") {
    auto client = std::make_shared<FakeStreamingClient>();
    client->responses.push_back(ai::assistant_text_message("current reply"));
    agent::ToolRegistry registry;
    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    agent::Agent subject(client->factory(),
            std::move(registry),
            std::move(options),
            agent::AgentInitialState{
                    .messages = {ai::user_text_message("prior prompt")},
            });

    auto run = run_agent(subject, "current prompt");

    CHECK(run.result);
    const auto* ended = std::get_if<agent::AgentEndEvent>(&run.events.back());
    REQUIRE(ended != nullptr);
    REQUIRE(ended->messages.size() == 2);
    REQUIRE(std::holds_alternative<ai::UserMessage>(ended->messages[0]));
    CHECK(ai::text_from_user_message(std::get<ai::UserMessage>(ended->messages[0])) == "current prompt");
    REQUIRE(std::holds_alternative<ai::AssistantMessage>(ended->messages[1]));
}

TEST_CASE("prepareNextTurn model swap changes next request model", "[agent][async][u8][spec]") {
    auto client = std::make_shared<FakeStreamingClient>();
    client->responses.push_back(tool_call_response());
    client->responses.push_back(ai::assistant_text_message("second"));

    agent::ToolRegistry registry;
    REQUIRE(registry.add(
            make_fake_tool(ai::Tool{"read_file", "Read", test::permissive_object_tool_argument_contract()}).tool));

    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    options.prepare_next_turn = [](const agent::PrepareNextTurnContext&)
            -> support::AsyncResult<std::optional<agent::AgentLoopTurnUpdate>> {
        return support::AsyncResult<std::optional<agent::AgentLoopTurnUpdate>>{
                support::Expected<std::optional<agent::AgentLoopTurnUpdate>>{
                        agent::AgentLoopTurnUpdate{.model = tests::make_model("gpt-swapped")}}};
    };
    options.validate_turn_update = [](const agent::AgentLoopTurnUpdate& update) -> support::AsyncResult<void> {
        if (update.model && update.model->id == "gpt-swapped") {
            return support::AsyncResult<void>{support::ExpectedVoid{}};
        }
        return support::AsyncResult<void>{support::ExpectedVoid{
                std::unexpected(support::make_error(support::ErrorCode::Validation, "unknown model"))}};
    };

    agent::Agent subject(client->factory(), std::move(registry), std::move(options));
    auto run = run_agent(subject, "read");

    CHECK(run.result);
    REQUIRE(client->requests.size() == 2);
    CHECK(client->requests[1].model.id == "gpt-swapped");
    CHECK(run.state.model.id == "gpt-swapped");
}

TEST_CASE("prepareNextTurn model update without validator is rejected", "[agent][async][u8][spec]") {
    auto client = std::make_shared<FakeStreamingClient>();
    client->responses.push_back(tool_call_response());

    agent::ToolRegistry registry;
    REQUIRE(registry.add(
            make_fake_tool(ai::Tool{"read_file", "Read", test::permissive_object_tool_argument_contract()}).tool));

    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    options.prepare_next_turn = [](const agent::PrepareNextTurnContext&)
            -> support::AsyncResult<std::optional<agent::AgentLoopTurnUpdate>> {
        return support::AsyncResult<std::optional<agent::AgentLoopTurnUpdate>>{
                support::Expected<std::optional<agent::AgentLoopTurnUpdate>>{
                        agent::AgentLoopTurnUpdate{.model = tests::make_model("gpt-swapped")}}};
    };

    agent::Agent subject(client->factory(), std::move(registry), std::move(options));
    auto run = run_agent(subject, "read");

    REQUIRE_FALSE(run.result);
    CHECK(run.result.error().code == support::ErrorCode::Validation);
    CHECK(run.result.error().message == "model update requires validation");
}

TEST_CASE("prepareNextTurn thinking level is validated", "[agent][async][u8][spec]") {
    auto client = std::make_shared<FakeStreamingClient>();
    client->responses.push_back(ai::assistant_text_message("first"));

    agent::ToolRegistry registry;

    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    options.prepare_next_turn = [](const agent::PrepareNextTurnContext&)
            -> support::AsyncResult<std::optional<agent::AgentLoopTurnUpdate>> {
        return support::AsyncResult<std::optional<agent::AgentLoopTurnUpdate>>{
                support::Expected<std::optional<agent::AgentLoopTurnUpdate>>{
                        agent::AgentLoopTurnUpdate{.thinking_level = std::string{"invalid"}}}};
    };

    agent::Agent subject(client->factory(), std::move(registry), std::move(options));
    // The hook runs only when the loop continues (#745): the rejection row
    // continues the run with a queued follow-up.
    REQUIRE(subject.follow_up(ai::user_text_message("continue")));
    auto run = run_agent(subject, "hi");

    REQUIRE_FALSE(run.result);
    CHECK(run.result.error().code == support::ErrorCode::Validation);
    // The run aborted at the hook before another assistant response.
    CHECK(client->requests.size() == 1);
}

TEST_CASE("prepareNextTurn rejected update does not persist partial model changes", "[agent][async][u8][spec]") {
    auto client = std::make_shared<FakeStreamingClient>();
    client->responses.push_back(tool_call_response());
    client->responses.push_back(ai::assistant_text_message("second run"));

    agent::ToolRegistry registry;
    REQUIRE(registry.add(
            make_fake_tool(ai::Tool{"read_file", "Read", test::permissive_object_tool_argument_contract()}).tool));

    int prepare_calls = 0;
    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    options.prepare_next_turn = [&](const agent::PrepareNextTurnContext&)
            -> support::AsyncResult<std::optional<agent::AgentLoopTurnUpdate>> {
        ++prepare_calls;
        if (prepare_calls == 1) {
            return support::AsyncResult<std::optional<agent::AgentLoopTurnUpdate>>{
                    support::Expected<std::optional<agent::AgentLoopTurnUpdate>>{agent::AgentLoopTurnUpdate{
                            .model = tests::make_model("gpt-swapped"),
                            .thinking_level = std::string{"invalid"},
                    }}};
        }
        return support::AsyncResult<std::optional<agent::AgentLoopTurnUpdate>>{
                support::Expected<std::optional<agent::AgentLoopTurnUpdate>>{std::nullopt}};
    };
    options.validate_turn_update = [](const agent::AgentLoopTurnUpdate&) -> support::AsyncResult<void> {
        return support::AsyncResult<void>{support::ExpectedVoid{}};
    };

    agent::Agent subject(client->factory(), std::move(registry), std::move(options));
    auto first = run_agent(subject, "read");
    REQUIRE_FALSE(first.result);
    CHECK(first.result.error().code == support::ErrorCode::Validation);

    auto second = run_agent(subject, "hi again");
    CHECK(second.result);
    REQUIRE(client->requests.size() == 2);
    CHECK(client->requests[1].model.id == "gpt-test");
    CHECK(second.state.model.id == "gpt-test");
}

TEST_CASE("prepareNextTurn replaces model context without publishing replacement messages",
        "[agent][async][issue35][spec]") {
    auto client = std::make_shared<FakeStreamingClient>();
    client->responses.push_back(tool_call_response());
    client->responses.push_back(ai::assistant_text_message("second"));

    agent::ToolRegistry registry;
    REQUIRE(registry.add(
            make_fake_tool(ai::Tool{"read_file", "Read", test::permissive_object_tool_argument_contract()}).tool));

    bool prepared = false;
    bool stop_observed_pre_prepare_context = false;
    std::size_t first_prepare_new_message_count = 0;
    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    options.prepare_next_turn = [&](const agent::PrepareNextTurnContext& context)
            -> support::AsyncResult<std::optional<agent::AgentLoopTurnUpdate>> {
        if (prepared) {
            return support::AsyncResult<std::optional<agent::AgentLoopTurnUpdate>>{
                    support::Expected<std::optional<agent::AgentLoopTurnUpdate>>{std::nullopt}};
        }
        prepared = true;
        first_prepare_new_message_count = context.new_messages.size();
        agent::AgentLoopContextReplacement replacement;
        replacement.system_prompt = "replacement prompt";
        replacement.messages.push_back(ai::user_text_message("replacement history"));
        return support::AsyncResult<std::optional<agent::AgentLoopTurnUpdate>>{
                support::Expected<std::optional<agent::AgentLoopTurnUpdate>>{
                        agent::AgentLoopTurnUpdate{.context = std::move(replacement)}}};
    };
    // pi runs `shouldStopAfterTurn` immediately after the turn, before
    // prepare-next-turn: it observes the pre-prepare context (#745, ADR 0014).
    options.should_stop_after_turn = [&](const agent::PrepareNextTurnContext& context) -> support::AsyncResult<bool> {
        if (context.context.system_prompt && context.context.system_prompt->empty() &&
                context.context.messages.size() == 3) {
            stop_observed_pre_prepare_context = true;
        }
        return support::AsyncResult<bool>{support::Expected<bool>{false}};
    };

    agent::Agent subject(client->factory(), std::move(registry), std::move(options));
    auto run = run_agent(subject, "read");

    CHECK(run.result);
    CHECK(prepared);
    CHECK(first_prepare_new_message_count == 3);
    CHECK(stop_observed_pre_prepare_context);
    REQUIRE(client->requests.size() == 2);
    REQUIRE(client->requests[1].context.system_prompt.has_value());
    CHECK(*client->requests[1].context.system_prompt == "replacement prompt");
    REQUIRE(client->requests[1].context.messages.size() == 1);
    REQUIRE(std::holds_alternative<ai::UserMessage>(client->requests[1].context.messages[0]));
    CHECK(ai::text_from_user_message(std::get<ai::UserMessage>(client->requests[1].context.messages[0])) ==
            "replacement history");
    REQUIRE(client->requests[1].context.tools.size() == 1);
    CHECK(client->requests[1].context.tools[0].name == "read_file");

    const auto& replacement_context = client->requests[1].context.messages;
    REQUIRE(replacement_context.size() == 1);
    REQUIRE(std::holds_alternative<ai::UserMessage>(replacement_context[0]));
    CHECK(ai::text_from_user_message(std::get<ai::UserMessage>(replacement_context[0])) == "replacement history");
    const agent::MessageEndEvent* final_message_end = nullptr;
    for (const auto& event : run.events) {
        if (const auto* message_end = std::get_if<agent::MessageEndEvent>(&event);
                message_end && std::holds_alternative<ai::AssistantMessage>(message_end->message)) {
            final_message_end = message_end;
        }
    }
    REQUIRE(final_message_end != nullptr);
    CHECK(ai::text_from_assistant_content(std::get<ai::AssistantMessage>(final_message_end->message).content) ==
            "second");
    CHECK(count_events<agent::MessageStartEvent>(run.events) == 4);
    CHECK(count_events<agent::MessageEndEvent>(run.events) == 4);
    const auto* ended = std::get_if<agent::AgentEndEvent>(&run.events.back());
    REQUIRE(ended != nullptr);
    REQUIRE(ended->messages.size() == 4);
    REQUIRE(std::holds_alternative<ai::UserMessage>(ended->messages[0]));
    CHECK(ai::text_from_user_message(std::get<ai::UserMessage>(ended->messages[0])) == "read");
}

TEST_CASE("between-turn context replacement keeps the invocation window valid", "[agent][async][compat-pi][spec]") {
    auto client = std::make_shared<FakeStreamingClient>();
    client->responses.push_back(tool_call_response());
    client->responses.push_back(ai::assistant_text_message("second"));

    agent::ToolRegistry registry;
    REQUIRE(registry.add(
            make_fake_tool(ai::Tool{"read_file", "Read", test::permissive_object_tool_argument_contract()}).tool));

    auto subject_holder = std::make_shared<agent::Agent*>(nullptr);
    bool replaced_once = false;
    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    options.prepare_next_turn = [subject_holder, &replaced_once](const agent::PrepareNextTurnContext&)
            -> support::AsyncResult<std::optional<agent::AgentLoopTurnUpdate>> {
        // The hook runs only when the loop continues, so the run's final turn
        // never re-replaces the context (#745; the session's estimate gate
        // behaves the same way).
        if (replaced_once) {
            return support::AsyncResult<std::optional<agent::AgentLoopTurnUpdate>>{
                    support::Expected<std::optional<agent::AgentLoopTurnUpdate>>{std::nullopt}};
        }
        replaced_once = true;
        // pi `_runAutoCompaction` replaces the live history between turns. The
        // rebuilt context is shorter than the history that predates the run,
        // so the invocation window must restart at the replacement.
        auto& subject = **subject_holder;
        std::vector<ai::MessageVariant> rebuilt;
        rebuilt.push_back(ai::user_text_message("compacted history"));
        if (auto replaced = agent::detail::AgentMessageAccess::replace_messages(subject, std::move(rebuilt));
                !replaced) {
            return support::AsyncResult<std::optional<agent::AgentLoopTurnUpdate>>{
                    support::Expected<std::optional<agent::AgentLoopTurnUpdate>>{std::unexpected(replaced.error())}};
        }
        agent::AgentLoopContextReplacement replacement;
        replacement.system_prompt = "replacement prompt";
        replacement.messages = subject.state().messages;
        return support::AsyncResult<std::optional<agent::AgentLoopTurnUpdate>>{
                support::Expected<std::optional<agent::AgentLoopTurnUpdate>>{
                        agent::AgentLoopTurnUpdate{.context = std::move(replacement)}}};
    };

    agent::AgentInitialState initial_state;
    initial_state.messages.push_back(ai::user_text_message("earlier prompt"));
    initial_state.messages.push_back(ai::assistant_text_message("earlier answer"));
    initial_state.messages.push_back(ai::user_text_message("earlier follow-up"));
    agent::Agent subject(client->factory(), std::move(registry), std::move(options), std::move(initial_state));
    *subject_holder = &subject;

    auto run = run_agent(subject, "read");

    CHECK(run.result);
    REQUIRE(client->requests.size() == 2);
    REQUIRE(client->requests[1].context.messages.size() == 1);
    CHECK(ai::text_from_user_message(std::get<ai::UserMessage>(client->requests[1].context.messages[0])) ==
            "compacted history");
    // Only the message produced after the replacement is reported, and no
    // position outside the replaced list is ever addressed.
    const auto* ended = std::get_if<agent::AgentEndEvent>(&run.events.back());
    REQUIRE(ended != nullptr);
    REQUIRE(ended->messages.size() == 1);
    REQUIRE(std::holds_alternative<ai::AssistantMessage>(ended->messages[0]));
    CHECK(ai::text_from_assistant_content(std::get<ai::AssistantMessage>(ended->messages[0]).content) == "second");
    // Live history is the replacement plus the answer produced after it.
    REQUIRE(run.state.messages.size() == 2);
    REQUIRE(std::holds_alternative<ai::UserMessage>(run.state.messages[0]));
    CHECK(ai::text_from_user_message(std::get<ai::UserMessage>(run.state.messages[0])) == "compacted history");
}

// pi's agent loop invokes `prepareNextTurn` only at the top of a continuing
// inner-loop iteration: the run's final turn never reaches it (#745).
TEST_CASE(
        "prepareNextTurn is not invoked after the run's final turn", "[agent][async][u8][issue745][compat-pi][spec]") {
    auto client = std::make_shared<FakeStreamingClient>();
    client->responses.push_back(ai::assistant_text_message("final answer"));

    agent::ToolRegistry registry;

    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    int prepare_calls = 0;
    options.prepare_next_turn = [&prepare_calls](const agent::PrepareNextTurnContext&)
            -> support::AsyncResult<std::optional<agent::AgentLoopTurnUpdate>> {
        ++prepare_calls;
        return support::AsyncResult<std::optional<agent::AgentLoopTurnUpdate>>{
                support::Expected<std::optional<agent::AgentLoopTurnUpdate>>{std::nullopt}};
    };

    agent::Agent subject(client->factory(), std::move(registry), std::move(options));
    auto run = run_agent(subject, "hi");

    CHECK(run.result);
    REQUIRE(client->requests.size() == 1);
    CHECK(prepare_calls == 0);
    CHECK(count_events<agent::TurnEndEvent>(run.events) == 1);
}

// A continuing run invokes the hook exactly once per completed turn, after
// turn_end and before the next turn_start (#745, pi turn lifecycle).
TEST_CASE("prepareNextTurn runs once per continuing turn before the next turn_start",
        "[agent][async][u8][issue745][compat-pi][spec]") {
    auto client = std::make_shared<FakeStreamingClient>();
    client->responses.push_back(tool_call_response());
    client->responses.push_back(ai::assistant_text_message("second"));

    agent::ToolRegistry registry;
    REQUIRE(registry.add(
            make_fake_tool(ai::Tool{"read_file", "Read", test::permissive_object_tool_argument_contract()}).tool));

    // Shared observation point: the subscriber's event list, so the hook can
    // pin its position between turn_end and the following turn_start.
    auto events = std::make_shared<std::vector<agent::AgentLifecycleEvent>>();
    int prepare_calls = 0;
    std::vector<std::size_t> turn_starts_seen;
    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    options.prepare_next_turn = [&](const agent::PrepareNextTurnContext&)
            -> support::AsyncResult<std::optional<agent::AgentLoopTurnUpdate>> {
        ++prepare_calls;
        std::size_t turn_starts = 0;
        for (const auto& event : *events) {
            if (std::holds_alternative<agent::TurnStartEvent>(event)) {
                ++turn_starts;
            }
        }
        turn_starts_seen.push_back(turn_starts);
        return support::AsyncResult<std::optional<agent::AgentLoopTurnUpdate>>{
                support::Expected<std::optional<agent::AgentLoopTurnUpdate>>{std::nullopt}};
    };

    agent::Agent subject(client->factory(), std::move(registry), std::move(options));
    auto subscribed = subject.subscribe([&events](const agent::AgentLifecycleEvent& event) {
        events->push_back(event);
        return support::ExpectedVoid{};
    });
    REQUIRE(subscribed);
    auto subscription = std::move(*subscribed);
    auto run = run_agent(subject, "read");

    CHECK(run.result);
    REQUIRE(client->requests.size() == 2);
    // Exactly one invocation for the one completed continuing turn (turn 1);
    // the final turn 2 invokes it zero times.
    CHECK(prepare_calls == 1);
    REQUIRE(turn_starts_seen.size() == 1);
    // The hook ran after turn 1's turn_start and before turn 2's turn_start.
    CHECK(turn_starts_seen[0] == 1);
    CHECK(count_events<agent::TurnStartEvent>(run.events) == 2);
}

// pi agent-loop: preparation can be long-running (for example, compaction);
// steering queued while it runs is picked up by the re-poll after it settles,
// and the re-poll runs only when the earlier poll returned nothing so
// one-at-a-time mode still delivers exactly one message per turn (#745).
TEST_CASE("steering queued while prepare-next-turn runs is injected before the next response",
        "[agent][async][u8][issue745][compat-pi][spec]") {
    auto client = std::make_shared<FakeStreamingClient>();
    client->responses.push_back(tool_call_response());
    client->responses.push_back(ai::assistant_text_message("second"));

    agent::ToolRegistry registry;
    REQUIRE(registry.add(
            make_fake_tool(ai::Tool{"read_file", "Read", test::permissive_object_tool_argument_contract()}).tool));

    auto subject_holder = std::make_shared<agent::Agent*>(nullptr);
    int prepare_calls = 0;
    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    options.prepare_next_turn = [subject_holder, &prepare_calls](const agent::PrepareNextTurnContext&)
            -> support::AsyncResult<std::optional<agent::AgentLoopTurnUpdate>> {
        ++prepare_calls;
        // Simulate steering queued during a long preparation: nothing was
        // queued when the earlier end-of-turn poll ran, so the re-poll after
        // this hook settles must pick it up.
        REQUIRE((**subject_holder).steer(ai::user_text_message("late steer")));
        return support::AsyncResult<std::optional<agent::AgentLoopTurnUpdate>>{
                support::Expected<std::optional<agent::AgentLoopTurnUpdate>>{std::nullopt}};
    };

    agent::Agent subject(client->factory(), std::move(registry), std::move(options));
    *subject_holder = &subject;
    auto run = run_agent(subject, "read");

    CHECK(run.result);
    CHECK(prepare_calls == 1);
    REQUIRE(client->requests.size() == 2);
    // Turn 2's context: the turn-1 exchange plus exactly one injected steer.
    REQUIRE(client->requests[1].context.messages.size() == 4);
    const auto& steered = client->requests[1].context.messages.back();
    REQUIRE(std::holds_alternative<ai::UserMessage>(steered));
    CHECK(ai::text_from_user_message(std::get<ai::UserMessage>(steered)) == "late steer");
    // The queue is empty after the single one-at-a-time delivery.
    CHECK(run.state.input_queues.steering.messages.empty());
}

TEST_CASE("prepareNextTurn no update leaves model and thinking level unchanged", "[agent][async][u8][spec]") {
    auto client = std::make_shared<FakeStreamingClient>();
    client->responses.push_back(ai::assistant_text_message("first"));

    agent::ToolRegistry registry;

    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    options.prepare_next_turn = [](const agent::PrepareNextTurnContext&)
            -> support::AsyncResult<std::optional<agent::AgentLoopTurnUpdate>> {
        return support::AsyncResult<std::optional<agent::AgentLoopTurnUpdate>>{
                support::Expected<std::optional<agent::AgentLoopTurnUpdate>>{std::nullopt}};
    };

    agent::Agent subject(client->factory(), std::move(registry), std::move(options));
    auto run = run_agent(subject, "hi");

    CHECK(run.result);
    CHECK(run.state.model.id == "gpt-test");
    // The unset level requested pi's DEFAULT_THINKING_LEVEL ("medium") and was
    // clamped at creation against the non-reasoning model's only supported
    // level (#352). The no-update turn leaves that clamped level unchanged.
    CHECK(run.state.thinking_level == "off");
}

TEST_CASE("prepareNextTurn valid thinking level is preserved in state", "[agent][async][u8][spec]") {
    auto client = std::make_shared<FakeStreamingClient>();
    client->responses.push_back(ai::assistant_text_message("first"));
    client->responses.push_back(ai::assistant_text_message("second"));

    agent::ToolRegistry registry;

    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    // A full-map reasoning model supports "high", so the level-only update
    // passes through creation and re-clamp unchanged (#352).
    options.model = tests::make_full_thinking_model("gpt-test");
    options.prepare_next_turn = [](const agent::PrepareNextTurnContext&)
            -> support::AsyncResult<std::optional<agent::AgentLoopTurnUpdate>> {
        return support::AsyncResult<std::optional<agent::AgentLoopTurnUpdate>>{
                support::Expected<std::optional<agent::AgentLoopTurnUpdate>>{
                        agent::AgentLoopTurnUpdate{.thinking_level = std::string{"high"}}}};
    };

    agent::Agent subject(client->factory(), std::move(registry), std::move(options));
    // Continues the run so the update row reaches the hook (#745).
    REQUIRE(subject.follow_up(ai::user_text_message("continue")));
    auto run = run_agent(subject, "hi");

    CHECK(run.result);
    CHECK(run.state.thinking_level == "high");
    CHECK(client->requests.size() == 2);
}

TEST_CASE("prepareNextTurn model validation hook can reject unknown models", "[agent][async][u8][spec]") {
    auto client = std::make_shared<FakeStreamingClient>();
    client->responses.push_back(tool_call_response());
    client->responses.push_back(ai::assistant_text_message("second"));

    agent::ToolRegistry registry;
    REQUIRE(registry.add(
            make_fake_tool(ai::Tool{"read_file", "Read", test::permissive_object_tool_argument_contract()}).tool));

    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    options.prepare_next_turn = [](const agent::PrepareNextTurnContext&)
            -> support::AsyncResult<std::optional<agent::AgentLoopTurnUpdate>> {
        return support::AsyncResult<std::optional<agent::AgentLoopTurnUpdate>>{
                support::Expected<std::optional<agent::AgentLoopTurnUpdate>>{
                        agent::AgentLoopTurnUpdate{.model = tests::make_model("missing-model")}}};
    };
    options.validate_turn_update = [](const agent::AgentLoopTurnUpdate& update) -> support::AsyncResult<void> {
        if (update.model && update.model->id != "gpt-test") {
            return support::AsyncResult<void>{support::ExpectedVoid{std::unexpected(
                    support::make_error(support::ErrorCode::Validation, "unknown model", update.model->id))}};
        }
        return support::AsyncResult<void>{support::ExpectedVoid{}};
    };

    agent::Agent subject(client->factory(), std::move(registry), std::move(options));
    auto run = run_agent(subject, "read");

    REQUIRE_FALSE(run.result);
    CHECK(run.result.error().code == support::ErrorCode::Validation);
    CHECK(run.result.error().message == "unknown model");
}

TEST_CASE("prepareNextTurn and turn-update validation failures abort cleanly", "[agent][async][u8][issue483][spec]") {
    {
        auto client = std::make_shared<FakeStreamingClient>();
        client->responses.push_back(ai::assistant_text_message("first"));
        agent::ToolRegistry registry;

        agent::AsyncAgentOptions options;
        options.max_turns = 4;
        options.model = tests::make_model("gpt-test");
        options.prepare_next_turn = [](const agent::PrepareNextTurnContext&)
                -> support::AsyncResult<std::optional<agent::AgentLoopTurnUpdate>> {
            return support::AsyncResult<std::optional<agent::AgentLoopTurnUpdate>>{
                    support::Expected<std::optional<agent::AgentLoopTurnUpdate>>{std::unexpected(support::make_error(
                            support::ErrorCode::Tool, "prepareNextTurn hook failed", "prepare boom"))}};
        };

        agent::Agent subject(client->factory(), std::move(registry), std::move(options));
        // The hook runs only when the loop continues (#745); the failure row
        // continues the run with a queued follow-up.
        REQUIRE(subject.follow_up(ai::user_text_message("continue")));
        auto run = run_agent(subject, "hi");

        REQUIRE_FALSE(run.result);
        CHECK(run.result.error().message == "prepareNextTurn hook failed");
        CHECK(run.result.error().detail.find("prepare boom") != std::string::npos);
    }

    {
        auto client = std::make_shared<FakeStreamingClient>();
        client->responses.push_back(ai::assistant_text_message("first"));
        agent::ToolRegistry registry;

        agent::AsyncAgentOptions options;
        options.max_turns = 4;
        options.model = tests::make_model("gpt-test");
        options.prepare_next_turn = [](const agent::PrepareNextTurnContext&)
                -> support::AsyncResult<std::optional<agent::AgentLoopTurnUpdate>> {
            return support::AsyncResult<std::optional<agent::AgentLoopTurnUpdate>>{
                    support::Expected<std::optional<agent::AgentLoopTurnUpdate>>{
                            agent::AgentLoopTurnUpdate{.model = tests::make_model("gpt-next")}}};
        };
        options.validate_turn_update = [](const agent::AgentLoopTurnUpdate&) -> support::AsyncResult<void> {
            return support::AsyncResult<void>{support::ExpectedVoid{std::unexpected(support::make_error(
                    support::ErrorCode::Tool, "validateTurnUpdate hook failed", "validator boom"))}};
        };

        agent::Agent subject(client->factory(), std::move(registry), std::move(options));
        REQUIRE(subject.follow_up(ai::user_text_message("continue")));
        auto run = run_agent(subject, "hi");

        REQUIRE_FALSE(run.result);
        CHECK(run.result.error().message == "validateTurnUpdate hook failed");
        CHECK(run.result.error().detail.find("validator boom") != std::string::npos);
    }
}

TEST_CASE("tool execution policy defaults to bounded parallel", "[agent][async][u8][spec]") {
    agent::AsyncAgentOptions options;
    CHECK(std::holds_alternative<agent::BoundedParallelToolExecution>(options.tool_execution));
    CHECK(std::get<agent::BoundedParallelToolExecution>(options.tool_execution).max_in_flight == 0);
}

TEST_CASE("an exclusive tool forces a bounded batch to execute sequentially", "[agent][async][u8][spec]") {
    auto client = std::make_shared<FakeStreamingClient>();
    client->responses.push_back(two_tool_call_response());
    client->responses.push_back(ai::assistant_text_message("done"));

    ConcurrencyProbe probe;
    agent::ToolRegistry registry;
    REQUIRE(registry.add(
            make_probed_fake_tool(ai::Tool{"alpha", "Alpha", test::permissive_object_tool_argument_contract()},
                    agent::ToolConcurrency::ParallelSafe,
                    probe)
                    .tool));
    REQUIRE(registry.add(
            make_probed_fake_tool(ai::Tool{"beta", "Beta", test::permissive_object_tool_argument_contract()},
                    agent::ToolConcurrency::Exclusive,
                    probe)
                    .tool));

    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    options.tool_execution = agent::BoundedParallelToolExecution{2};

    agent::Agent subject(client->factory(), std::move(registry), std::move(options));
    auto run = run_agent_on_pool(subject, "read");

    CHECK(run.result);
    REQUIRE(client->requests.size() == 2);
    CHECK(probe.max_active.load() == 1);
}

TEST_CASE("bounded parallel execution preserves source order in the transcript", "[agent][async][u8][spec]") {
    auto client = std::make_shared<FakeStreamingClient>();
    client->responses.push_back(two_tool_call_response());
    client->responses.push_back(ai::assistant_text_message("done"));

    agent::ToolRegistry registry;
    auto alpha =
            make_configurable_fake_tool(ai::Tool{"alpha", "Alpha", test::permissive_object_tool_argument_contract()},
                    agent::ToolConcurrency::ParallelSafe,
                    "alpha result");
    auto beta = make_configurable_fake_tool(ai::Tool{"beta", "Beta", test::permissive_object_tool_argument_contract()},
            agent::ToolConcurrency::ParallelSafe,
            "beta result");
    auto* alpha_ptr = alpha.state.get();
    auto* beta_ptr = beta.state.get();
    REQUIRE(registry.add(std::move(alpha.tool)));
    REQUIRE(registry.add(std::move(beta.tool)));

    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    options.tool_execution = agent::BoundedParallelToolExecution{2};

    agent::Agent subject(client->factory(), std::move(registry), std::move(options));
    auto run = run_agent_on_pool(subject, "read");

    CHECK(run.result);
    CHECK(alpha_ptr->invocations.size() == 1);
    CHECK(beta_ptr->invocations.size() == 1);

    REQUIRE(client->requests.size() == 2);
    const auto& second_request = client->requests[1];
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

TEST_CASE("agent loop recovers after a schema-invalid call in a bounded parallel batch",
        "[agent][async][tool-arguments][issue27][spec]") {
    auto client = std::make_shared<FakeStreamingClient>();

    ai::AssistantMessage mixed_calls;
    mixed_calls.stop_reason = ai::AssistantStopReason::ToolUse;
    mixed_calls.content.emplace_back(ai::tool_call_content("call-invalid", "alpha", R"({"value":"not-an-integer"})"));
    mixed_calls.content.emplace_back(ai::tool_call_content("call-valid", "beta", R"({"value":"2"})"));
    client->responses.push_back(std::move(mixed_calls));

    ai::AssistantMessage corrected_call;
    corrected_call.stop_reason = ai::AssistantStopReason::ToolUse;
    corrected_call.content.emplace_back(ai::tool_call_content("call-corrected", "alpha", R"({"value":"3"})"));
    client->responses.push_back(std::move(corrected_call));
    client->responses.push_back(ai::assistant_text_message("recovered"));

    const auto strict_contract = test::integer_value_tool_argument_contract();

    agent::ToolRegistry registry;
    auto alpha = make_configurable_fake_tool(
            ai::Tool{"alpha", "Alpha", strict_contract}, agent::ToolConcurrency::ParallelSafe, "alpha result");
    auto beta = make_configurable_fake_tool(
            ai::Tool{"beta", "Beta", strict_contract}, agent::ToolConcurrency::ParallelSafe, "beta result");
    auto* alpha_ptr = alpha.state.get();
    auto* beta_ptr = beta.state.get();
    REQUIRE(registry.add(std::move(alpha.tool)));
    REQUIRE(registry.add(std::move(beta.tool)));

    std::vector<std::string> before_hook_names;
    std::vector<support::JsonValue> before_hook_arguments;
    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    options.tool_execution = agent::BoundedParallelToolExecution{2};
    options.before_tool_call = [&](const agent::BeforeToolCallContext& context,
                                       std::stop_token) -> support::AsyncResult<agent::BeforeToolCallResult> {
        before_hook_names.push_back(context.tool_call.name);
        before_hook_arguments.push_back(context.args);
        return support::AsyncResult<agent::BeforeToolCallResult>{
                support::Expected<agent::BeforeToolCallResult>{agent::BeforeToolCallResult{}}};
    };

    agent::Agent subject(client->factory(), std::move(registry), std::move(options));
    auto run = run_agent_on_pool(subject, "run both calls and recover");

    CHECK(run.result);
    CHECK(count_events<agent::TurnStartEvent>(run.events) == 3);
    CHECK(final_stop_reason(run.events) == ai::AssistantStopReason::Stop);
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

    REQUIRE(client->requests.size() == 3);
    const auto& recovery_context = client->requests[1].context.messages;
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

    const auto& completion_context = client->requests[2].context.messages;
    REQUIRE(completion_context.size() == 6);
    REQUIRE(std::holds_alternative<ai::ToolResultMessage>(completion_context[5]));
    const auto& corrected_result = std::get<ai::ToolResultMessage>(completion_context[5]);
    CHECK(corrected_result.tool_call_id == "call-corrected");
    CHECK_FALSE(corrected_result.is_error);

    CHECK(count_events<agent::ToolExecutionStartEvent>(run.events) == 3);
    CHECK(count_events<agent::ToolExecutionEndEvent>(run.events) == 3);
}

TEST_CASE("bounded parallel limit one executes sequentially", "[agent][async][u8][spec]") {
    auto client = std::make_shared<FakeStreamingClient>();
    client->responses.push_back(two_tool_call_response());
    client->responses.push_back(ai::assistant_text_message("done"));

    ConcurrencyProbe probe;
    agent::ToolRegistry registry;
    REQUIRE(registry.add(
            make_probed_fake_tool(ai::Tool{"alpha", "Alpha", test::permissive_object_tool_argument_contract()},
                    agent::ToolConcurrency::ParallelSafe,
                    probe)
                    .tool));
    REQUIRE(registry.add(
            make_probed_fake_tool(ai::Tool{"beta", "Beta", test::permissive_object_tool_argument_contract()},
                    agent::ToolConcurrency::ParallelSafe,
                    probe)
                    .tool));

    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    options.tool_execution = agent::BoundedParallelToolExecution{1};

    agent::Agent subject(client->factory(), std::move(registry), std::move(options));
    auto run = run_agent_on_pool(subject, "read");

    CHECK(run.result);
    CHECK(probe.max_active.load() == 1);
    CHECK(count_events<agent::MessageStartEvent>(run.events) == 5);
    CHECK(count_events<agent::MessageEndEvent>(run.events) == 5);
}

TEST_CASE("Async Agent Loop treats bounded parallel zero as no explicit cap", "[agent][async][u8][spec]") {
    auto client = std::make_shared<FakeStreamingClient>();
    client->responses.push_back(two_tool_call_response());
    client->responses.push_back(ai::assistant_text_message("done"));

    ConcurrencyProbe probe;
    agent::ToolRegistry registry;
    REQUIRE(registry.add(
            make_probed_fake_tool(ai::Tool{"alpha", "Alpha", test::permissive_object_tool_argument_contract()},
                    agent::ToolConcurrency::ParallelSafe,
                    probe)
                    .tool));
    REQUIRE(registry.add(
            make_probed_fake_tool(ai::Tool{"beta", "Beta", test::permissive_object_tool_argument_contract()},
                    agent::ToolConcurrency::ParallelSafe,
                    probe)
                    .tool));

    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    options.tool_execution = agent::BoundedParallelToolExecution{0};

    agent::Agent subject(client->factory(), std::move(registry), std::move(options));
    auto run = run_agent_on_pool(subject, "read");

    CHECK(run.result);
    CHECK(probe.max_active.load() == 2);
    CHECK(count_events<agent::MessageStartEvent>(run.events) == 5);
    CHECK(count_events<agent::MessageEndEvent>(run.events) == 5);
}

TEST_CASE("bounded parallel execution keeps blocked calls out of tool adapters", "[agent][async][u8][spec]") {
    auto client = std::make_shared<FakeStreamingClient>();
    client->responses.push_back(two_tool_call_response());
    client->responses.push_back(ai::assistant_text_message("done"));

    agent::ToolRegistry registry;
    auto alpha =
            make_configurable_fake_tool(ai::Tool{"alpha", "Alpha", test::permissive_object_tool_argument_contract()},
                    agent::ToolConcurrency::ParallelSafe,
                    "alpha result");
    auto beta = make_configurable_fake_tool(ai::Tool{"beta", "Beta", test::permissive_object_tool_argument_contract()},
            agent::ToolConcurrency::ParallelSafe,
            "beta result");
    auto* alpha_ptr = alpha.state.get();
    auto* beta_ptr = beta.state.get();
    REQUIRE(registry.add(std::move(alpha.tool)));
    REQUIRE(registry.add(std::move(beta.tool)));

    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    options.tool_execution = agent::BoundedParallelToolExecution{2};
    options.before_tool_call = [](const agent::BeforeToolCallContext& context,
                                       std::stop_token) -> support::AsyncResult<agent::BeforeToolCallResult> {
        if (context.tool_call.name == "alpha") {
            return support::AsyncResult<agent::BeforeToolCallResult>{support::Expected<agent::BeforeToolCallResult>{
                    agent::BeforeToolCallResult{.block = true, .reason = "blocked alpha"}}};
        }
        return support::AsyncResult<agent::BeforeToolCallResult>{
                support::Expected<agent::BeforeToolCallResult>{agent::BeforeToolCallResult{}}};
    };

    agent::Agent subject(client->factory(), std::move(registry), std::move(options));
    auto run = run_agent_on_pool(subject, "read");

    CHECK(run.result);
    CHECK(alpha_ptr->invocations.empty());
    CHECK(beta_ptr->invocations.size() == 1);
    REQUIRE(client->requests.size() == 2);
    const auto& messages = client->requests[1].context.messages;
    REQUIRE(messages.size() == 4);
    REQUIRE(std::holds_alternative<ai::ToolResultMessage>(messages[2]));
    REQUIRE(std::holds_alternative<ai::ToolResultMessage>(messages[3]));
    const auto& alpha_result = std::get<ai::ToolResultMessage>(messages[2]);
    const auto& beta_result = std::get<ai::ToolResultMessage>(messages[3]);
    CHECK(alpha_result.is_error);
    CHECK(ai::text_from_content(alpha_result.content) == "blocked alpha");
    CHECK_FALSE(beta_result.is_error);
    CHECK(count_events<agent::MessageStartEvent>(run.events) == 5);
    CHECK(count_events<agent::MessageEndEvent>(run.events) == 5);
}

TEST_CASE("bounded parallel before-hook failure finalizes every call without starting workers",
        "[agent][async][u8][spec]") {
    auto client = std::make_shared<FakeStreamingClient>();
    client->responses.push_back(two_tool_call_response());

    ConcurrencyProbe probe;
    agent::ToolRegistry registry;
    REQUIRE(registry.add(
            make_probed_fake_tool(ai::Tool{"alpha", "Alpha", test::permissive_object_tool_argument_contract()},
                    agent::ToolConcurrency::ParallelSafe,
                    probe)
                    .tool));
    REQUIRE(registry.add(
            make_probed_fake_tool(ai::Tool{"beta", "Beta", test::permissive_object_tool_argument_contract()},
                    agent::ToolConcurrency::ParallelSafe,
                    probe)
                    .tool));

    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    options.tool_execution = agent::BoundedParallelToolExecution{2};
    options.before_tool_call = [](const agent::BeforeToolCallContext&,
                                       std::stop_token) -> support::AsyncResult<agent::BeforeToolCallResult> {
        return support::AsyncResult<agent::BeforeToolCallResult>{support::Expected<agent::BeforeToolCallResult>{
                std::unexpected(support::make_error(support::ErrorCode::Tool, "preflight failed"))}};
    };

    // pi prepareToolCall: every failing before hook finalizes its own call's
    // error result; no worker ever starts and the run continues (ADR 0008).
    agent::Agent subject(client->factory(), std::move(registry), std::move(options));
    auto run = run_agent_on_pool(subject, "read");

    CHECK(run.result);
    CHECK(count_events<agent::TurnStartEvent>(run.events) == 2);
    CHECK(probe.max_active.load() == 0);
    REQUIRE(client->requests.size() == 2);
    const auto& messages = client->requests[1].context.messages;
    REQUIRE(messages.size() == 4);
    REQUIRE(std::holds_alternative<ai::ToolResultMessage>(messages[2]));
    REQUIRE(std::holds_alternative<ai::ToolResultMessage>(messages[3]));
    const auto& alpha_result = std::get<ai::ToolResultMessage>(messages[2]);
    const auto& beta_result = std::get<ai::ToolResultMessage>(messages[3]);
    CHECK(alpha_result.is_error);
    CHECK(beta_result.is_error);
    CHECK(ai::text_from_content(alpha_result.content) == "preflight failed");
    CHECK(ai::text_from_content(beta_result.content) == "preflight failed");
    CHECK(count_events<agent::MessageStartEvent>(run.events) == 5);
    CHECK(count_events<agent::MessageEndEvent>(run.events) == 5);
}

TEST_CASE("bounded parallel execution preserves peer success after a tool error", "[agent][async][u8][spec]") {
    auto client = std::make_shared<FakeStreamingClient>();
    client->responses.push_back(two_tool_call_response());
    client->responses.push_back(ai::assistant_text_message("done"));

    agent::ToolRegistry registry;
    REQUIRE(registry.add(
            make_failing_fake_tool(ai::Tool{"alpha", "Alpha", test::permissive_object_tool_argument_contract()}).tool));
    REQUIRE(registry.add(
            make_configurable_fake_tool(ai::Tool{"beta", "Beta", test::permissive_object_tool_argument_contract()},
                    agent::ToolConcurrency::ParallelSafe,
                    "beta result")
                    .tool));

    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    options.tool_execution = agent::BoundedParallelToolExecution{2};

    agent::Agent subject(client->factory(), std::move(registry), std::move(options));
    auto run = run_agent_on_pool(subject, "read");

    CHECK(run.result);
    REQUIRE(client->requests.size() == 2);
    const auto& messages = client->requests[1].context.messages;
    REQUIRE(messages.size() == 4);
    REQUIRE(std::holds_alternative<ai::ToolResultMessage>(messages[2]));
    REQUIRE(std::holds_alternative<ai::ToolResultMessage>(messages[3]));
    const auto& alpha_result = std::get<ai::ToolResultMessage>(messages[2]);
    const auto& beta_result = std::get<ai::ToolResultMessage>(messages[3]);
    CHECK(alpha_result.is_error);
    CHECK_FALSE(beta_result.is_error);
    CHECK(count_events<agent::MessageStartEvent>(run.events) == 5);
    CHECK(count_events<agent::MessageEndEvent>(run.events) == 5);
}

TEST_CASE("bounded parallel event-sink failure drains workers and emits one agent end",
        "[agent][async][u8][issue483][spec]") {
    auto client = std::make_shared<FakeStreamingClient>();
    client->responses.push_back(two_tool_call_response());

    agent::ToolRegistry registry;
    REQUIRE(registry.add(
            make_configurable_fake_tool(ai::Tool{"alpha", "Alpha", test::permissive_object_tool_argument_contract()},
                    agent::ToolConcurrency::ParallelSafe,
                    "alpha result")
                    .tool));
    REQUIRE(registry.add(
            make_configurable_fake_tool(ai::Tool{"beta", "Beta", test::permissive_object_tool_argument_contract()},
                    agent::ToolConcurrency::ParallelSafe,
                    "beta result")
                    .tool));

    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    options.tool_execution = agent::BoundedParallelToolExecution{2};
    agent::Agent subject(client->factory(), std::move(registry), std::move(options));

    std::atomic<int> agent_end_events{0};
    auto commitment = [&agent_end_events](const agent::AgentLifecycleEvent& event) {
        if (std::holds_alternative<agent::ToolExecutionEndEvent>(event)) {
            return support::ExpectedVoid{std::unexpected(
                    support::make_error(support::ErrorCode::Tool, "agent event sink failed", "sink boom"))};
        }
        if (std::holds_alternative<agent::AgentEndEvent>(event)) {
            ++agent_end_events;
        }
        return support::ExpectedVoid{};
    };
    auto run = run_agent_on_pool(subject, "read", std::move(commitment));

    REQUIRE_FALSE(run.result);
    CHECK(run.result.error().message == "agent event sink failed");
    CHECK(agent_end_events.load() == 1);
    CHECK(count_events<agent::MessageStartEvent>(run.events) == 2);
    CHECK(count_events<agent::MessageEndEvent>(run.events) == 2);
}

TEST_CASE("bounded parallel after-hook failure finalizes only its call", "[agent][async][u8][spec]") {
    auto client = std::make_shared<FakeStreamingClient>();
    client->responses.push_back(two_tool_call_response());
    client->responses.push_back(ai::assistant_text_message("done"));

    agent::ToolRegistry registry;
    REQUIRE(registry.add(
            make_configurable_fake_tool(ai::Tool{"alpha", "Alpha", test::permissive_object_tool_argument_contract()},
                    agent::ToolConcurrency::ParallelSafe,
                    "alpha result")
                    .tool));
    REQUIRE(registry.add(
            make_configurable_fake_tool(ai::Tool{"beta", "Beta", test::permissive_object_tool_argument_contract()},
                    agent::ToolConcurrency::ParallelSafe,
                    "beta result")
                    .tool));

    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    options.tool_execution = agent::BoundedParallelToolExecution{2};
    options.after_tool_call = [](const agent::AfterToolCallContext& context,
                                      std::stop_token) -> support::AsyncResult<agent::AfterToolCallResult> {
        if (context.tool_call.name == "alpha") {
            return support::AsyncResult<agent::AfterToolCallResult>{support::Expected<agent::AfterToolCallResult>{
                    std::unexpected(support::make_error(support::ErrorCode::Tool, "post-processor failed"))}};
        }
        return support::AsyncResult<agent::AfterToolCallResult>{
                support::Expected<agent::AfterToolCallResult>{agent::AfterToolCallResult{}}};
    };

    agent::Agent subject(client->factory(), std::move(registry), std::move(options));
    auto run = run_agent_on_pool(subject, "read");

    CHECK(run.result);
    CHECK(count_events<agent::TurnStartEvent>(run.events) == 2);
    REQUIRE(client->requests.size() == 2);
    const auto& messages = client->requests[1].context.messages;
    REQUIRE(messages.size() == 4);
    REQUIRE(std::holds_alternative<ai::ToolResultMessage>(messages[2]));
    REQUIRE(std::holds_alternative<ai::ToolResultMessage>(messages[3]));
    const auto& alpha_result = std::get<ai::ToolResultMessage>(messages[2]);
    const auto& beta_result = std::get<ai::ToolResultMessage>(messages[3]);
    CHECK(alpha_result.is_error);
    CHECK(ai::text_from_content(alpha_result.content) == "post-processor failed");
    CHECK_FALSE(beta_result.is_error);
    CHECK(count_events<agent::MessageStartEvent>(run.events) == 5);
    CHECK(count_events<agent::MessageEndEvent>(run.events) == 5);
}

TEST_CASE("bounded parallel execution emits end events in completion order", "[agent][async][u8][spec]") {
    auto client = std::make_shared<FakeStreamingClient>();
    client->responses.push_back(two_tool_call_response());
    client->responses.push_back(ai::assistant_text_message("done"));

    agent::ToolRegistry registry;
    REQUIRE(registry.add(
            make_delayed_fake_tool(ai::Tool{"alpha", "Alpha", test::permissive_object_tool_argument_contract()},
                    std::chrono::milliseconds{100},
                    "alpha result")
                    .tool));
    REQUIRE(registry.add(
            make_delayed_fake_tool(ai::Tool{"beta", "Beta", test::permissive_object_tool_argument_contract()},
                    std::chrono::milliseconds{10},
                    "beta result")
                    .tool));

    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    options.tool_execution = agent::BoundedParallelToolExecution{2};

    agent::Agent subject(client->factory(), std::move(registry), std::move(options));
    auto run = run_agent_on_pool(subject, "read");

    CHECK(run.result);

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

TEST_CASE("length-truncated tool calls emit errors without crossing the executor seam", "[agent][async][u8][spec]") {
    auto client = std::make_shared<FakeStreamingClient>();
    auto truncated = two_tool_call_response();
    truncated.stop_reason = ai::AssistantStopReason::Length;
    client->responses.push_back(std::move(truncated));
    client->responses.push_back(ai::assistant_text_message("recovered"));

    agent::ToolRegistry registry;
    auto alpha =
            make_configurable_fake_tool(ai::Tool{"alpha", "Alpha", test::permissive_object_tool_argument_contract()},
                    agent::ToolConcurrency::ParallelSafe,
                    "alpha result");
    auto beta = make_configurable_fake_tool(ai::Tool{"beta", "Beta", test::permissive_object_tool_argument_contract()},
            agent::ToolConcurrency::ParallelSafe,
            "beta result");
    auto* alpha_ptr = alpha.state.get();
    auto* beta_ptr = beta.state.get();
    REQUIRE(registry.add(std::move(alpha.tool)));
    REQUIRE(registry.add(std::move(beta.tool)));

    int before_calls = 0;
    int after_calls = 0;
    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    options.tool_execution = agent::BoundedParallelToolExecution{2};
    options.before_tool_call = [&](const agent::BeforeToolCallContext&,
                                       std::stop_token) -> support::AsyncResult<agent::BeforeToolCallResult> {
        ++before_calls;
        return support::AsyncResult<agent::BeforeToolCallResult>{
                support::Expected<agent::BeforeToolCallResult>{agent::BeforeToolCallResult{}}};
    };
    options.after_tool_call = [&](const agent::AfterToolCallContext&,
                                      std::stop_token) -> support::AsyncResult<agent::AfterToolCallResult> {
        ++after_calls;
        return support::AsyncResult<agent::AfterToolCallResult>{
                support::Expected<agent::AfterToolCallResult>{agent::AfterToolCallResult{}}};
    };

    agent::Agent subject(client->factory(), std::move(registry), std::move(options));
    auto run = run_agent_on_pool(subject, "read");

    CHECK(run.result);
    CHECK(count_events<agent::TurnStartEvent>(run.events) == 2);
    CHECK(alpha_ptr->invocations.empty());
    CHECK(beta_ptr->invocations.empty());
    CHECK(before_calls == 0);
    CHECK(after_calls == 0);
    CHECK(count_events<agent::ToolExecutionStartEvent>(run.events) == 2);
    CHECK(count_events<agent::ToolExecutionEndEvent>(run.events) == 2);
    CHECK(count_events<agent::MessageStartEvent>(run.events) == 5);
    CHECK(count_events<agent::MessageEndEvent>(run.events) == 5);

    REQUIRE(client->requests.size() == 2);
    const auto& messages = client->requests[1].context.messages;
    REQUIRE(messages.size() == 4);
    REQUIRE(std::holds_alternative<ai::ToolResultMessage>(messages[2]));
    REQUIRE(std::holds_alternative<ai::ToolResultMessage>(messages[3]));
    const auto& alpha_result = std::get<ai::ToolResultMessage>(messages[2]);
    const auto& beta_result = std::get<ai::ToolResultMessage>(messages[3]);
    CHECK(alpha_result.is_error);
    CHECK(beta_result.is_error);
    CHECK(alpha_result.tool_name == "alpha");
    CHECK(beta_result.tool_name == "beta");
    CHECK(ai::text_from_content(alpha_result.content).find("output token limit") != std::string::npos);
    CHECK(ai::text_from_content(alpha_result.content).find(R"({"x":1})") == std::string::npos);
    CHECK(run.state.pending_tool_call_ids.empty());
    REQUIRE(run.state.active_tool_names.size() == 2);
    CHECK(run.state.active_tool_names[0] == "alpha");
    CHECK(run.state.active_tool_names[1] == "beta");
}

TEST_CASE("tool scheduling vocabulary belongs to cch::agent", "[agent][async][u8][spec]") {
    static_assert(std::is_enum_v<agent::ToolConcurrency>);
    static_assert(std::is_same_v<agent::ToolExecutionPolicy,
            std::variant<agent::SequentialToolExecution, agent::BoundedParallelToolExecution>>);
}

TEST_CASE("default tool execution runs a parallel-safe batch concurrently", "[agent][async][issue355][spec]") {
    auto client = std::make_shared<FakeStreamingClient>();
    client->responses.push_back(two_tool_call_response());
    client->responses.push_back(ai::assistant_text_message("done"));

    ConcurrencyProbe probe;
    agent::ToolRegistry registry;
    REQUIRE(registry.add(
            make_probed_fake_tool(ai::Tool{"alpha", "Alpha", test::permissive_object_tool_argument_contract()},
                    agent::ToolConcurrency::ParallelSafe,
                    probe)
                    .tool));
    REQUIRE(registry.add(
            make_probed_fake_tool(ai::Tool{"beta", "Beta", test::permissive_object_tool_argument_contract()},
                    agent::ToolConcurrency::ParallelSafe,
                    probe)
                    .tool));

    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    // No explicit tool_execution: the loop default is bounded parallel with
    // no cap, so a parallel-safe batch overlaps (pi's parallel default).
    agent::Agent subject(client->factory(), std::move(registry), std::move(options));
    auto run = run_agent_on_pool(subject, "read");

    CHECK(run.result);
    CHECK(count_events<agent::TurnStartEvent>(run.events) == 2);
    CHECK(probe.max_active.load() == 2);
    REQUIRE(client->requests.size() == 2);
    const auto& messages = client->requests[1].context.messages;
    REQUIRE(messages.size() == 4);
    REQUIRE(std::holds_alternative<ai::ToolResultMessage>(messages[2]));
    REQUIRE(std::holds_alternative<ai::ToolResultMessage>(messages[3]));
    CHECK(std::get<ai::ToolResultMessage>(messages[2]).tool_name == "alpha");
    CHECK(std::get<ai::ToolResultMessage>(messages[3]).tool_name == "beta");
}

TEST_CASE("an exclusive tool serializes the whole batch with full per-call lifecycle at the loop",
        "[agent][async][issue355][spec]") {
    auto client = std::make_shared<FakeStreamingClient>();
    client->responses.push_back(two_tool_call_response());
    client->responses.push_back(ai::assistant_text_message("done"));

    ConcurrencyProbe probe;
    agent::ToolRegistry registry;
    REQUIRE(registry.add(
            make_probed_fake_tool(ai::Tool{"alpha", "Alpha", test::permissive_object_tool_argument_contract()},
                    agent::ToolConcurrency::ParallelSafe,
                    probe)
                    .tool));
    REQUIRE(registry.add(
            make_probed_fake_tool(ai::Tool{"beta", "Beta", test::permissive_object_tool_argument_contract()},
                    agent::ToolConcurrency::Exclusive,
                    probe)
                    .tool));

    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    options.tool_execution = agent::BoundedParallelToolExecution{2};

    agent::Agent subject(client->factory(), std::move(registry), std::move(options));
    auto run = run_agent_on_pool(subject, "read");

    CHECK(run.result);
    CHECK(probe.max_active.load() == 1);

    // pi executeToolCallsSequential: each call's full lifecycle completes
    // before the next call starts, in assistant source order.
    std::vector<std::string> sequence;
    for (const auto& event : run.events) {
        if (const auto* start = std::get_if<agent::ToolExecutionStartEvent>(&event)) {
            sequence.push_back("start:" + start->tool_call_id);
        } else if (const auto* end = std::get_if<agent::ToolExecutionEndEvent>(&event)) {
            sequence.push_back("end:" + end->tool_call_id);
        } else if (const auto* start = std::get_if<agent::MessageStartEvent>(&event)) {
            if (const auto* result = std::get_if<ai::ToolResultMessage>(&start->message)) {
                sequence.push_back("message-start:" + result->tool_call_id);
            }
        }
    }
    REQUIRE((sequence == std::vector<std::string>{
                                 "start:call-1",
                                 "end:call-1",
                                 "message-start:call-1",
                                 "start:call-2",
                                 "end:call-2",
                                 "message-start:call-2",
                         }));
}

TEST_CASE("length-truncated fail-all matches pi's message and emits source-order errors",
        "[agent][async][issue355][spec]") {
    auto client = std::make_shared<FakeStreamingClient>();
    auto truncated = two_tool_call_response();
    truncated.stop_reason = ai::AssistantStopReason::Length;
    client->responses.push_back(std::move(truncated));
    client->responses.push_back(ai::assistant_text_message("recovered"));

    agent::ToolRegistry registry;
    auto alpha =
            make_configurable_fake_tool(ai::Tool{"alpha", "Alpha", test::permissive_object_tool_argument_contract()},
                    agent::ToolConcurrency::ParallelSafe,
                    "alpha result");
    auto beta = make_configurable_fake_tool(ai::Tool{"beta", "Beta", test::permissive_object_tool_argument_contract()},
            agent::ToolConcurrency::ParallelSafe,
            "beta result");
    auto* alpha_ptr = alpha.state.get();
    auto* beta_ptr = beta.state.get();
    REQUIRE(registry.add(std::move(alpha.tool)));
    REQUIRE(registry.add(std::move(beta.tool)));

    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");

    agent::Agent subject(client->factory(), std::move(registry), std::move(options));
    auto run = run_agent_on_pool(subject, "read");

    CHECK(run.result);
    CHECK(count_events<agent::TurnStartEvent>(run.events) == 2);
    CHECK(alpha_ptr->invocations.empty());
    CHECK(beta_ptr->invocations.empty());

    // pi failToolCallsFromTruncatedMessage: per-call start/end pairs in
    // source order with the verbatim truncated-arguments message.
    std::vector<std::string> sequence;
    const std::string expected_message =
            "Tool call \"alpha\" was not executed: the response hit the output token limit, so its "
            "arguments may be truncated. Re-issue the tool call with complete arguments.";
    for (const auto& event : run.events) {
        if (const auto* start = std::get_if<agent::ToolExecutionStartEvent>(&event)) {
            sequence.push_back("start:" + start->tool_call_id);
        } else if (const auto* end = std::get_if<agent::ToolExecutionEndEvent>(&event)) {
            sequence.push_back("end:" + end->tool_call_id);
        } else if (const auto* start = std::get_if<agent::MessageStartEvent>(&event)) {
            if (const auto* result = std::get_if<ai::ToolResultMessage>(&start->message)) {
                sequence.push_back("message-start:" + result->tool_call_id);
                if (result->tool_call_id == "call-1") {
                    CHECK(ai::text_from_content(result->content) == expected_message);
                }
            }
        }
    }
    REQUIRE((sequence == std::vector<std::string>{
                                 "start:call-1",
                                 "end:call-1",
                                 "message-start:call-1",
                                 "start:call-2",
                                 "end:call-2",
                                 "message-start:call-2",
                         }));

    REQUIRE(client->requests.size() == 2);
    const auto& messages = client->requests[1].context.messages;
    REQUIRE(messages.size() == 4);
    REQUIRE(std::holds_alternative<ai::ToolResultMessage>(messages[2]));
    REQUIRE(std::holds_alternative<ai::ToolResultMessage>(messages[3]));
    CHECK(std::get<ai::ToolResultMessage>(messages[2]).is_error);
    CHECK(std::get<ai::ToolResultMessage>(messages[3]).is_error);
}

TEST_CASE("all-true terminate batch ends the loop after one turn", "[agent][async][issue355][spec]") {
    auto client = std::make_shared<FakeStreamingClient>();
    client->responses.push_back(two_tool_call_response());
    client->responses.push_back(ai::assistant_text_message("unused"));

    agent::ToolRegistry registry;
    REQUIRE(registry.add(
            make_fake_tool(ai::Tool{"alpha", "Alpha", test::permissive_object_tool_argument_contract()}).tool));
    REQUIRE(registry.add(
            make_fake_tool(ai::Tool{"beta", "Beta", test::permissive_object_tool_argument_contract()}).tool));

    agent::AsyncAgentOptions options;
    options.max_turns = 4;
    options.model = tests::make_model("gpt-test");
    options.after_tool_call = [](const agent::AfterToolCallContext&,
                                      std::stop_token) -> support::AsyncResult<agent::AfterToolCallResult> {
        return support::AsyncResult<agent::AfterToolCallResult>{
                support::Expected<agent::AfterToolCallResult>{agent::AfterToolCallResult{
                        .content = std::nullopt,
                        .details = std::nullopt,
                        .is_error = std::nullopt,
                        .terminate = true,
                }}};
    };

    agent::Agent subject(client->factory(), std::move(registry), std::move(options));
    auto run = run_agent(subject, "read");

    CHECK(run.result);
    CHECK(count_events<agent::TurnStartEvent>(run.events) == 1);
    CHECK(final_stop_reason(run.events) == ai::AssistantStopReason::ToolUse);
    CHECK(client->requests.size() == 1);
    CHECK(count_events<agent::AgentEndEvent>(run.events) == 1);
}
