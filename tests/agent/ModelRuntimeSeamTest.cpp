#include <cch/agent/Agent.hpp>
#include <cch/ai/Content.hpp>
#include "agent/AgentLoop.hpp"
#include "ai/SimpleOptions.hpp"
#include "support/FakeModelRuntime.hpp"
#include "support/ModelFixture.hpp"
#include "util/ExpectedMacros.hpp"
#include "util/Json.hpp"

#include "../../third_party/catch2/catch_test_macros.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>

#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

using namespace cch;

namespace {

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

} // namespace

TEST_CASE(
    "Agent issues every turn through ModelRuntime::streamSimple with the harness-consumer option set",
    "[agent][streamSimple][issue350]") {
    auto runtime = std::make_shared<tests::FakeModelRuntime>();
    runtime->responses.push_back(ai::assistant_text_message("hello user"));

    agent::AsyncToolRegistry tools;
    agent::AsyncAgentOptions options;
    options.max_turns = 3;
    options.model = tests::make_model("gpt-test");
    options.session_id = "session-1";
    agent::Agent subject(runtime, std::move(tools), std::move(options));

    REQUIRE(run_prompt(subject, "hi"));

    REQUIRE(runtime->calls.size() == 1);
    const auto& call = runtime->calls[0];
    CHECK(call.model.id == "gpt-test");
    CHECK(call.options.session_id == "session-1");
    // Ordinary turns forward the cancellation signal and leave cacheRetention
    // unset so the pi-aligned default "short" applies (ADR 0033:
    // `resolve_cache_retention` maps unset to Short; compaction is the only
    // agent-core consumer that overrides it, with "none" and a fresh session
    // id). This matches pi's harness consumer, which omits cacheRetention for
    // default turns.
    CHECK(call.options.stop_token != std::stop_token{});
    CHECK_FALSE(call.options.stop_token.stop_requested());
    CHECK(call.options.cache_retention == std::nullopt);
    CHECK(ai::detail::resolve_cache_retention(
              call.options.cache_retention, call.options.env) ==
          ai::CacheRetention::Short);

    const auto state = subject.state();
    REQUIRE(state.messages.size() == 2);
    REQUIRE(std::holds_alternative<ai::AssistantMessage>(state.messages[1]));
    CHECK(ai::text_from_assistant_content(
              std::get<ai::AssistantMessage>(state.messages[1]).content) ==
          "hello user");
}

TEST_CASE(
    "Agent loop forwards the exact run stop token through streamSimple",
    "[agent][streamSimple][issue350]") {
    auto runtime = std::make_shared<tests::FakeModelRuntime>();
    runtime->responses.push_back(ai::assistant_text_message("ok"));
    agent::AsyncAgentOptions options;
    options.max_turns = 3;
    options.model = tests::make_model("gpt-test");
    options.session_id = "session-2";
    agent::AsyncAgentLoop loop(runtime, agent::AsyncToolRegistry{}, std::move(options));

    std::stop_source stop_source;
    auto run = run_loop(loop, "hi", stop_source.get_token());

    REQUIRE(run.result);
    REQUIRE(runtime->calls.size() == 1);
    CHECK(runtime->calls[0].options.stop_token == stop_source.get_token());
    CHECK_FALSE(runtime->calls[0].options.stop_token.stop_requested());
}

TEST_CASE(
    "Agent forwards the full harness-consumer option set through streamSimple each turn",
    "[agent][streamSimple][issue351]") {
    auto runtime = std::make_shared<tests::FakeModelRuntime>();
    runtime->responses.push_back(ai::assistant_text_message("hello user"));

    agent::AsyncAgentOptions options;
    options.max_turns = 3;
    options.model = tests::make_model("gpt-test");
    options.session_id = "session-full";
    options.cache_retention = ai::CacheRetention::Short;
    options.timeout_ms = 60000;
    options.max_retries = 2;
    options.max_retry_delay_ms = 120000;
    options.headers = {{std::string{"x-golden"}, std::string{"value"}}};
    agent::Agent subject(
        runtime,
        agent::AsyncToolRegistry{},
        std::move(options),
        agent::AgentInitialState{.thinking_level = "high"});

    REQUIRE(run_prompt(subject, "hi"));

    REQUIRE(runtime->calls.size() == 1);
    const auto& call = runtime->calls[0];
    // The complete harness-consumer option set, per turn, exactly as pi's
    // agent-harness.ts createStreamFn forwards it: reasoning (thinking level
    // unless off), sessionId, cacheRetention, timeoutMs, maxRetries,
    // maxRetryDelayMs, headers, signal.
    CHECK(call.options.reasoning == ai::ThinkingLevel::High);
    CHECK(call.options.session_id == "session-full");
    CHECK(call.options.cache_retention == ai::CacheRetention::Short);
    CHECK(call.options.timeout_ms == 60000);
    CHECK(call.options.max_retries == 2);
    CHECK(call.options.max_retry_delay_ms == 120000);
    REQUIRE(call.options.headers.size() == 1);
    const auto found = call.options.headers.find("x-golden");
    REQUIRE(found != call.options.headers.end());
    REQUIRE(found->second.has_value());
    CHECK(*found->second == "value");
    CHECK(call.options.stop_token != std::stop_token{});
    CHECK_FALSE(call.options.stop_token.stop_requested());
}

TEST_CASE(
    "default turns forward pi's harness-consumer defaults through streamSimple",
    "[agent][streamSimple][issue351]") {
    auto runtime = std::make_shared<tests::FakeModelRuntime>();
    runtime->responses.push_back(ai::assistant_text_message("hello user"));

    agent::AsyncAgentOptions options;
    options.max_turns = 3;
    options.model = tests::make_model("gpt-test");
    // thinking_level stays off and every retry/header/timeout knob stays at
    // its default; only the active prompt signal is forwarded.
    agent::Agent subject(runtime, agent::AsyncToolRegistry{}, std::move(options));

    REQUIRE(run_prompt(subject, "hi"));

    REQUIRE(runtime->calls.size() == 1);
    const auto& call = runtime->calls[0];
    // off thinking forwards no reasoning (pi createLoopConfig off → undefined).
    CHECK(call.options.reasoning == std::nullopt);
    CHECK(call.options.session_id == std::nullopt);
    // cacheRetention unset resolves to the pi-aligned "short" default (ADR
    // 0033); the stream layer maps nullopt to Short.
    CHECK(call.options.cache_retention == std::nullopt);
    CHECK(ai::detail::resolve_cache_retention(
              call.options.cache_retention, call.options.env) ==
          ai::CacheRetention::Short);
    CHECK(call.options.timeout_ms == std::nullopt);
    CHECK(call.options.max_retries == 0);
    CHECK(call.options.max_retry_delay_ms == std::nullopt);
    CHECK(call.options.headers.empty());
    CHECK(call.options.stop_token != std::stop_token{});
    CHECK_FALSE(call.options.stop_token.stop_requested());
}

namespace {

[[nodiscard]] ai::AssistantMessage terminal_message(
    std::string error_message) {
    auto terminal = ai::assistant_text_message("");
    terminal.stop_reason = ai::AssistantStopReason::Error;
    terminal.error_message = std::move(error_message);
    return terminal;
}

void expect_terminal_matrix_row(
    util::ErrorCode category,
    const std::string& category_name) {
    auto runtime = std::make_shared<tests::FakeModelRuntime>();
    runtime->terminal_failure_code = category;
    runtime->responses.push_back(
        terminal_message("terminal " + category_name));

    agent::AsyncAgentOptions options;
    options.max_turns = 3;
    options.model = tests::make_model("gpt-test");
    agent::AsyncAgentLoop loop(runtime, agent::AsyncToolRegistry{}, std::move(options));

    auto run = run_loop(loop, "hi");

    // Exactly one terminal event plus an agreeing final AssistantMessage, with
    // the category flowing through the single util::Expected error value (the
    // #326 six-category channel; no second exception hierarchy).
    REQUIRE(run.result.has_value());
    CHECK(run.result->stop_reason == ai::AssistantStopReason::Error);
    REQUIRE(run.result->context.messages.size() == 2);
    const auto* final =
        std::get_if<ai::AssistantMessage>(&run.result->context.messages.back());
    REQUIRE(final != nullptr);
    CHECK(final->stop_reason == ai::AssistantStopReason::Error);
    REQUIRE(final->error_message.has_value());
    CHECK(*final->error_message == "terminal " + category_name);
    CHECK(runtime->terminal_events == 1);
    REQUIRE(runtime->last_terminal_failure.has_value());
    CHECK(runtime->last_terminal_failure->code == category);
    CHECK(count_events<agent::TurnEndEvent>(run.events) == 1);
    CHECK(count_events<agent::AgentEndEvent>(run.events) == 1);
}

} // namespace

TEST_CASE(
    "six-category terminal matrix yields exactly one terminal event plus a final AssistantMessage each",
    "[agent][streamSimple][terminal][issue351]") {
    expect_terminal_matrix_row(
        util::ErrorCode::ModelSource, "model_source");
    expect_terminal_matrix_row(
        util::ErrorCode::ModelValidation, "model_validation");
    expect_terminal_matrix_row(util::ErrorCode::Provider, "provider");
    expect_terminal_matrix_row(util::ErrorCode::Stream, "stream");
    expect_terminal_matrix_row(util::ErrorCode::Auth, "auth");
    expect_terminal_matrix_row(util::ErrorCode::OAuth, "oauth");
}

TEST_CASE(
    "recording fake ModelRuntime drives one successful turn end to end",
    "[agent][streamSimple][issue350]") {
    auto runtime = std::make_shared<tests::FakeModelRuntime>();
    runtime->responses.push_back(ai::assistant_text_message("hello user"));

    agent::AsyncAgentOptions options;
    options.max_turns = 3;
    options.model = tests::make_model("gpt-test");
    agent::AsyncAgentLoop loop(runtime, agent::AsyncToolRegistry{}, std::move(options));

    auto run = run_loop(loop, "hi");

    REQUIRE(run.result);
    CHECK(run.result->turns == 1);
    CHECK(run.result->stop_reason == ai::AssistantStopReason::Stop);
    REQUIRE(run.result->context.messages.size() == 2);
    REQUIRE(runtime->calls.size() == 1);
    CHECK(runtime->calls[0].context.messages.size() == 1);
    CHECK(ai::text_from_user_message(std::get<ai::UserMessage>(
              runtime->calls[0].context.messages[0])) == "hi");
}

TEST_CASE(
    "terminal-error turn yields exactly one error terminal event and an agreeing final message",
    "[agent][streamSimple][issue350]") {
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

    // The run completes through the single Expected value channel with the
    // agreeing terminal AssistantMessage; exactly one error terminal event was
    // forwarded and one assistant lifecycle was presented.
    REQUIRE(run.result.has_value());
    CHECK(run.result->stop_reason == ai::AssistantStopReason::Error);
    REQUIRE(run.result->context.messages.size() == 2);
    const auto* final = std::get_if<ai::AssistantMessage>(&run.result->context.messages.back());
    REQUIRE(final != nullptr);
    CHECK(final->stop_reason == ai::AssistantStopReason::Error);
    REQUIRE(final->error_message.has_value());
    CHECK(*final->error_message == "provider request failed");
    CHECK(runtime->terminal_events == 1);

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
    CHECK(count_events<agent::TurnEndEvent>(run.events) == 1);
    CHECK(count_events<agent::AgentEndEvent>(run.events) == 1);
}

TEST_CASE(
    "cancellation yields exactly one aborted terminal event end to end",
    "[agent][streamSimple][abort][issue350]") {
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
    REQUIRE(run.result->context.messages.size() == 2);
    const auto* final = std::get_if<ai::AssistantMessage>(&run.result->context.messages.back());
    REQUIRE(final != nullptr);
    CHECK(final->stop_reason == ai::AssistantStopReason::Aborted);
    REQUIRE(final->error_message.has_value());
    CHECK(*final->error_message == "Request was aborted");
    CHECK(runtime->terminal_events == 1);
    CHECK(count_events<agent::TurnEndEvent>(run.events) == 1);
    CHECK(count_events<agent::AgentEndEvent>(run.events) == 1);
}

TEST_CASE(
    "agent_end carries only the current invocation's messages through the fake runtime",
    "[agent][streamSimple][issue350]") {
    auto runtime = std::make_shared<tests::FakeModelRuntime>();
    runtime->responses.push_back(ai::assistant_text_message("current reply"));

    agent::AsyncAgentOptions options;
    options.max_turns = 3;
    options.model = tests::make_model("gpt-test");
    agent::Agent subject(runtime, agent::AsyncToolRegistry{}, std::move(options));

    REQUIRE(run_prompt(subject, "first"));
    REQUIRE(run_prompt(subject, "second"));

    const auto state = subject.state();
    REQUIRE(state.messages.size() == 4);
    CHECK(ai::text_from_user_message(std::get<ai::UserMessage>(state.messages[0])) == "first");
    CHECK(ai::text_from_user_message(std::get<ai::UserMessage>(state.messages[2])) == "second");
    REQUIRE(runtime->calls.size() == 2);
}
