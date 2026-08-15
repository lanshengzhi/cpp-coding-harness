#include <cch/agent/Agent.hpp>
#include "ai/AsyncResultBridge.hpp"
#include <cch/ai/Content.hpp>
#include "agent/AgentLoop.hpp"
#include "ai/SimpleOptions.hpp"
#include "support/FakeModelStream.hpp"
#include "support/ModelFixture.hpp"
#include "support/ExpectedMacros.hpp"
#include "support/Json.hpp"

#include <catch2/catch_test_macros.hpp>

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

support::ExpectedVoid run_prompt(agent::Agent& subject, std::string prompt) {
    boost::asio::io_context io;
    std::optional<support::ExpectedVoid> result;
    boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<void> {
            result = co_await ai::detail::await_async_result(subject.prompt(std::move(prompt)));
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

struct LoopRun {
    support::Expected<agent::AsyncAgentRunResult> result;
    std::vector<agent::AgentLifecycleEvent> events;
};

LoopRun run_loop(
    agent::AsyncAgentLoop& loop,
    std::string prompt,
    std::stop_token stop_token = {}) {
    boost::asio::io_context io;
    std::optional<support::Expected<agent::AsyncAgentRunResult>> result;
    std::vector<agent::AgentLifecycleEvent> events;
    boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<void> {
            result = co_await loop.continue_with(
                {},
                std::move(prompt),
                [&](const agent::AgentLifecycleEvent& event) {
                    events.push_back(event);
                    return support::ExpectedVoid{};
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
    auto runtime = std::make_shared<tests::FakeModelStream>();
    runtime->responses.push_back(ai::assistant_text_message("hello user"));

    agent::ToolRegistry tools;
    agent::AsyncAgentOptions options;
    options.max_turns = 3;
    options.model = tests::make_model("gpt-test");
    options.session_id = "session-1";
    agent::Agent subject(runtime->factory(), std::move(tools), std::move(options));

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
    auto runtime = std::make_shared<tests::FakeModelStream>();
    runtime->responses.push_back(ai::assistant_text_message("ok"));
    agent::AsyncAgentOptions options;
    options.max_turns = 3;
    options.model = tests::make_model("gpt-test");
    options.session_id = "session-2";
    agent::AsyncAgentLoop loop(runtime->factory(), agent::ToolRegistry{}, std::move(options));

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
    auto runtime = std::make_shared<tests::FakeModelStream>();
    runtime->responses.push_back(ai::assistant_text_message("hello user"));

    agent::AsyncAgentOptions options;
    options.max_turns = 3;
    // Full-map reasoning model so the requested "high" survives creation-time
    // clamping and reaches the stream (#352).
    options.model = tests::make_full_thinking_model("gpt-test");
    options.session_id = "session-full";
    options.cache_retention = ai::CacheRetention::Short;
    options.timeout_ms = 60000;
    options.max_retries = 2;
    options.max_retry_delay_ms = 120000;
    options.headers = {{std::string{"x-golden"}, std::string{"value"}}};
    agent::Agent subject(
        runtime->factory(),
        agent::ToolRegistry{},
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
    auto runtime = std::make_shared<tests::FakeModelStream>();
    runtime->responses.push_back(ai::assistant_text_message("hello user"));

    agent::AsyncAgentOptions options;
    options.max_turns = 3;
    options.model = tests::make_model("gpt-test");
    // The unset level requested pi's DEFAULT_THINKING_LEVEL ("medium") and was
    // clamped at creation to the non-reasoning model's only supported level
    // ("off", #352); every retry/header/timeout knob stays at its default and
    // only the active prompt signal is forwarded.
    agent::Agent subject(runtime->factory(), agent::ToolRegistry{}, std::move(options));

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
    support::ErrorCode category,
    const std::string& category_name) {
    auto runtime = std::make_shared<tests::FakeModelStream>();
    runtime->terminal_failure_code = category;
    runtime->responses.push_back(
        terminal_message("terminal " + category_name));

    agent::AsyncAgentOptions options;
    options.max_turns = 3;
    options.model = tests::make_model("gpt-test");
    agent::AsyncAgentLoop loop(runtime->factory(), agent::ToolRegistry{}, std::move(options));

    auto run = run_loop(loop, "hi");

    // Exactly one terminal event plus an agreeing final AssistantMessage, with
    // the category flowing through the single support::Expected error value (the
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
        support::ErrorCode::ModelSource, "model_source");
    expect_terminal_matrix_row(
        support::ErrorCode::ModelValidation, "model_validation");
    expect_terminal_matrix_row(support::ErrorCode::Provider, "provider");
    expect_terminal_matrix_row(support::ErrorCode::Stream, "stream");
    expect_terminal_matrix_row(support::ErrorCode::Auth, "auth");
    expect_terminal_matrix_row(support::ErrorCode::OAuth, "oauth");
}

TEST_CASE(
    "recording fake ModelRuntime drives one successful turn end to end",
    "[agent][streamSimple][issue350]") {
    auto runtime = std::make_shared<tests::FakeModelStream>();
    runtime->responses.push_back(ai::assistant_text_message("hello user"));

    agent::AsyncAgentOptions options;
    options.max_turns = 3;
    options.model = tests::make_model("gpt-test");
    agent::AsyncAgentLoop loop(runtime->factory(), agent::ToolRegistry{}, std::move(options));

    auto run = run_loop(loop, "hi");

    REQUIRE(run.result);
    CHECK(run.result->turns == 1);
    CHECK(run.result->stop_reason == ai::AssistantStopReason::Stop);
    REQUIRE(run.result->context.messages.size() == 2);
    REQUIRE(runtime->calls.size() == 1);
    REQUIRE(runtime->calls[0].context.messages.size() == 1);
    REQUIRE(std::holds_alternative<ai::UserMessage>(runtime->calls[0].context.messages[0]));
    CHECK(ai::text_from_user_message(std::get<ai::UserMessage>(
              runtime->calls[0].context.messages[0])) == "hi");
}

TEST_CASE(
    "terminal-error turn yields exactly one error terminal event and an agreeing final message",
    "[agent][streamSimple][issue350]") {
    auto runtime = std::make_shared<tests::FakeModelStream>();
    auto terminal = ai::assistant_text_message("");
    terminal.stop_reason = ai::AssistantStopReason::Error;
    terminal.error_message = "provider request failed";
    runtime->responses.push_back(std::move(terminal));

    agent::AsyncAgentOptions options;
    options.max_turns = 3;
    options.model = tests::make_model("gpt-test");
    agent::AsyncAgentLoop loop(runtime->factory(), agent::ToolRegistry{}, std::move(options));

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
    auto runtime = std::make_shared<tests::FakeModelStream>();
    runtime->responses.push_back(ai::assistant_text_message("unused"));

    agent::AsyncAgentOptions options;
    options.max_turns = 3;
    options.model = tests::make_model("gpt-test");
    agent::AsyncAgentLoop loop(runtime->factory(), agent::ToolRegistry{}, std::move(options));

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
    auto runtime = std::make_shared<tests::FakeModelStream>();
    runtime->responses.push_back(ai::assistant_text_message("current reply"));

    agent::AsyncAgentOptions options;
    options.max_turns = 3;
    options.model = tests::make_model("gpt-test");
    agent::Agent subject(runtime->factory(), agent::ToolRegistry{}, std::move(options));

    REQUIRE(run_prompt(subject, "first"));
    REQUIRE(run_prompt(subject, "second"));

    const auto state = subject.state();
    REQUIRE(state.messages.size() == 4);
    REQUIRE(std::holds_alternative<ai::UserMessage>(state.messages[0]));
    REQUIRE(std::holds_alternative<ai::UserMessage>(state.messages[2]));
    CHECK(ai::text_from_user_message(std::get<ai::UserMessage>(state.messages[0])) == "first");
    CHECK(ai::text_from_user_message(std::get<ai::UserMessage>(state.messages[2])) == "second");
    REQUIRE(runtime->calls.size() == 2);
}

namespace {

/// Partial-map reasoning model shared by the clamp tests: low/high/xhigh carry
/// explicit mappings and "max" is absent, so the supported set is off..xhigh
/// (a missing xhigh/max mapping means unsupported, pi
/// `getSupportedThinkingLevels`).
[[nodiscard]] ai::ThinkingLevelMap partial_thinking_map() {
    return ai::ThinkingLevelMap{
        {ai::ModelThinkingLevel::Off, "off"},
        {ai::ModelThinkingLevel::Low, "low"},
        {ai::ModelThinkingLevel::High, "high"},
        {ai::ModelThinkingLevel::XHigh, "xhigh"},
    };
}

/// The per-turn wire reasoning pi's harness consumer derives from a clamped
/// level: "off" forwards no reasoning, every other level forwards as the
/// stream `ThinkingLevel` (agent-harness.ts `off → undefined`).
[[nodiscard]] std::optional<ai::ThinkingLevel> wire_reasoning(
    std::string_view level) {
    if (level == "off") {
        return std::nullopt;
    }
    if (level == "minimal") {
        return ai::ThinkingLevel::Minimal;
    }
    if (level == "low") {
        return ai::ThinkingLevel::Low;
    }
    if (level == "medium") {
        return ai::ThinkingLevel::Medium;
    }
    if (level == "high") {
        return ai::ThinkingLevel::High;
    }
    if (level == "xhigh") {
        return ai::ThinkingLevel::XHigh;
    }
    if (level == "max") {
        return ai::ThinkingLevel::Max;
    }
    return std::nullopt;
}

/// Run one turn with the requested level against the given model and assert
/// that creation-time clamping produced the expected effective level both in
/// run state and in the reasoning option recorded at the fake ModelRuntime
/// seam (an unsupported level can never reach the wire, #352).
void expect_creation_clamp(
    const ai::Model& model,
    std::string_view requested,
    std::string_view expected_level) {
    auto runtime = std::make_shared<tests::FakeModelStream>();
    runtime->responses.push_back(ai::assistant_text_message("ok"));

    agent::AsyncAgentOptions options;
    options.max_turns = 3;
    options.model = model;
    options.thinking_level = std::string{requested};
    agent::AsyncAgentLoop loop(runtime->factory(), agent::ToolRegistry{}, std::move(options));
    auto run = run_loop(loop, "hi");

    REQUIRE(run.result.has_value());
    CHECK(run.result->state.thinking_level == expected_level);
    REQUIRE(runtime->calls.size() == 1);
    CHECK(runtime->calls[0].options.reasoning == wire_reasoning(expected_level));
}

} // namespace

TEST_CASE(
    "creation-time thinking clamping covers every level against full, partial, and null thinking maps",
    "[agent][streamSimple][issue352]") {
    // Full map: every seven-level name mapped, so nothing clamps.
    const auto full_map = tests::make_full_thinking_model("gpt-full");
    // Partial map: "max" absent, so the supported set is off..xhigh.
    const auto partial_map = tests::make_reasoning_model(
        "gpt-partial", partial_thinking_map());
    // Null map on a reasoning model: no mapping at all, so xhigh/max require
    // an explicit mapping (pi) and clamp down to the supported off..high set.
    auto null_map = tests::make_model("gpt-null");
    null_map.reasoning = true;

    constexpr std::string_view kLevels[] = {
        "off", "minimal", "low", "medium", "high", "xhigh", "max"};

    for (const auto requested : kLevels) {
        expect_creation_clamp(full_map, requested, requested);
        const auto expected_partial =
            requested == "max" ? std::string_view{"xhigh"} : requested;
        expect_creation_clamp(partial_map, requested, expected_partial);
        const auto expected_null =
            (requested == "xhigh" || requested == "max")
                ? std::string_view{"high"}
                : requested;
        expect_creation_clamp(null_map, requested, expected_null);
    }
}

TEST_CASE(
    "model switch re-clamps the thinking level so an unsupported level never reaches the wire",
    "[agent][streamSimple][issue352]") {
    auto runtime = std::make_shared<tests::FakeModelStream>();
    runtime->responses.push_back(ai::assistant_text_message("first"));
    runtime->responses.push_back(ai::assistant_text_message("second"));

    agent::AsyncAgentOptions options;
    options.max_turns = 3;
    options.model = tests::make_reasoning_model(
        "gpt-partial", partial_thinking_map());
    // The host switches to a non-reasoning model after turn 1; the active
    // "max" level (already clamped to "xhigh" at creation) must re-clamp to
    // the new model's only supported level ("off") before turn 2 (pi
    // agent-session.ts re-clamps on model switch).
    options.prepare_next_turn =
        agent::adapt_sync_prepare_next_turn(
            [](const agent::PrepareNextTurnContext&)
                -> support::Expected<
                    std::optional<agent::AgentLoopTurnUpdate>> {
                return agent::AgentLoopTurnUpdate{
                    .model = tests::make_model("gpt-basic"),
                };
            });
    options.validate_turn_update =
        agent::adapt_sync_validate_turn_update(
            [](const agent::AgentLoopTurnUpdate&)
                -> support::ExpectedVoid { return {}; });

    agent::Agent subject(
        runtime->factory(),
        agent::ToolRegistry{},
        std::move(options),
        // The Agent requests the level from its initial state; "max" clamps
        // to the partial model's top supported level at creation.
        agent::AgentInitialState{.thinking_level = "max"});
    REQUIRE(run_prompt(subject, "first"));
    REQUIRE(run_prompt(subject, "second"));

    REQUIRE(runtime->calls.size() == 2);
    CHECK(runtime->calls[0].options.reasoning == ai::ThinkingLevel::XHigh);
    CHECK(runtime->calls[1].options.reasoning == std::nullopt);
    CHECK(subject.state().model.id == "gpt-basic");
    CHECK(subject.state().thinking_level == "off");
}

TEST_CASE(
    "the Agent holds kDefaultModel with no special-casing until a real model resolves",
    "[agent][streamSimple][issue352]") {
    auto runtime = std::make_shared<tests::FakeModelStream>();
    runtime->responses.push_back(ai::assistant_text_message("ok"));

    // No model configured: the loop forwards the concrete unknown kDefaultModel
    // exactly as configured, and the unset level requests pi's
    // DEFAULT_THINKING_LEVEL ("medium") which clamps to kDefaultModel's only
    // supported level ("off") — a fresh Agent's first turn carries no
    // reasoning, matching pi's `if (!model) thinkingLevel = "off"`.
    agent::AsyncAgentLoop loop(runtime->factory(), agent::ToolRegistry{}, {});
    auto run = run_loop(loop, "hi");

    REQUIRE(run.result.has_value());
    REQUIRE(runtime->calls.size() == 1);
    CHECK(runtime->calls[0].model.id == "unknown");
    CHECK(runtime->calls[0].model.provider == "unknown");
    CHECK(run.result->state.model.id == "unknown");
    CHECK(run.result->state.thinking_level == "off");
    CHECK(runtime->calls[0].options.reasoning == std::nullopt);
}

TEST_CASE(
    "the Agent's live state reflects the clamped thinking level at creation",
    "[agent][streamSimple][issue352]") {
    auto runtime = std::make_shared<tests::FakeModelStream>();
    runtime->responses.push_back(ai::assistant_text_message("ok"));

    agent::AsyncAgentOptions options;
    options.model = tests::make_reasoning_model(
        "gpt-partial", partial_thinking_map());
    // "max" requests clamp to the partial model's top supported level
    // ("xhigh") at construction; live state reports the effective level before
    // any turn runs.
    agent::Agent subject(
        runtime->factory(),
        agent::ToolRegistry{},
        std::move(options),
        agent::AgentInitialState{.thinking_level = "max"});

    CHECK(subject.state().thinking_level == "xhigh");
}

TEST_CASE(
    "Agent set_thinking_level clamps to the active model and updates live state",
    "[agent][streamSimple][issue353]") {
    auto runtime = std::make_shared<tests::FakeModelStream>();
    runtime->responses.push_back(ai::assistant_text_message("ok"));

    // A reasoning model without an xhigh/max mapping supports off..high, so
    // a "max" request clamps to "high" (pi setThinkingLevel clamps before
    // persisting).
    agent::AsyncAgentOptions options;
    options.model = tests::make_reasoning_model(
        "gpt-partial", partial_thinking_map());
    agent::Agent subject(
        runtime->factory(),
        agent::ToolRegistry{},
        std::move(options),
        agent::AgentInitialState{.thinking_level = "medium"});

    auto clamped = subject.set_thinking_level("max");
    REQUIRE(clamped.has_value());
    CHECK(*clamped == "xhigh");
    CHECK(subject.state().thinking_level == "xhigh");

    // The next stream request forwards the effective level.
    REQUIRE(run_prompt(subject, "hi"));
    REQUIRE(runtime->calls.size() == 1);
    CHECK(runtime->calls[0].options.reasoning == ai::ThinkingLevel::XHigh);
}

TEST_CASE(
    "Agent set_thinking_level rejects invalid levels and no-ops on unchanged clamped level",
    "[agent][streamSimple][issue353]") {
    auto runtime = std::make_shared<tests::FakeModelStream>();
    runtime->responses.push_back(ai::assistant_text_message("ok"));

    agent::AsyncAgentOptions options;
    options.model = tests::make_full_thinking_model("gpt-test");
    agent::Agent subject(
        runtime->factory(),
        agent::ToolRegistry{},
        std::move(options),
        agent::AgentInitialState{.thinking_level = "high"});

    auto invalid = subject.set_thinking_level("sometimes");
    REQUIRE_FALSE(invalid.has_value());
    CHECK(invalid.error().code == support::ErrorCode::Validation);
    CHECK(subject.state().thinking_level == "high");

    // A request whose clamped level equals the current level is a no-op
    // success that leaves the level untouched.
    auto unchanged = subject.set_thinking_level("high");
    REQUIRE(unchanged.has_value());
    CHECK(*unchanged == "high");
    CHECK(subject.state().thinking_level == "high");

    // A non-reasoning model clamps every level to its only supported level
    // ("off"), so a change request becomes a no-op and never reaches the wire
    // as reasoning.
    agent::AsyncAgentOptions basic_options;
    basic_options.model = tests::make_model("gpt-basic");
    agent::Agent basic(
        runtime->factory(), agent::ToolRegistry{}, std::move(basic_options));
    auto clamped = basic.set_thinking_level("high");
    REQUIRE(clamped.has_value());
    CHECK(*clamped == "off");
    CHECK(basic.state().thinking_level == "off");
}
