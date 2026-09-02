#include "coding_agent/tui/InteractiveMode.hpp"
#include "coding_agent/tui/InteractiveSessionRun.hpp"

#include "support/EnvVarGuard.hpp"
#include "support/PumpUntil.hpp"
#include "support/RuntimeFixture.hpp"
#include "support/RuntimeLoopDriver.hpp"
#include "support/ScriptedRuntimeFixture.hpp"
#include "support/FakeUserShell.hpp"
#include "support/ModelsFixture.hpp"
#include "support/TempWorkspace.hpp"

#include <cch/ai/Content.hpp>
#include <cch/agent/harness/session/SessionStore.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include "coding_agent/AgentSession.hpp"
#include "coding_agent/runtime/SessionFactory.hpp"

#include <cch/support/Error.hpp>
#include <catch2/catch_test_macros.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <chrono>
#include <csignal>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace cch;
using tests::drain_ready;
using tests::pump_until;

namespace {

[[nodiscard]] auto make_run(
    coding_agent::AgentSession& session,
    const std::filesystem::path& config_dir = {}) {
    return coding_agent::tui::InteractiveSessionRunBuilder{}
        .with_session(session)
        .with_agent_config_directory(config_dir)
        .build();
}

[[nodiscard]] std::string visible_screen(const tui::VirtualTerminal& terminal) {
    std::string text;
    for (const auto& line : terminal.screen()) {
        text.append(line);
        text.push_back('\n');
    }
    return text;
}

/// A persisted session with one user/assistant pair, resumable through the
/// interactive boot with an injected concrete runtime and scripted Provider.
struct ResumedSessionFixture {
    tests::TempWorkspace workspace;
    tests::TempWorkspace config;
    std::filesystem::path session_file;
    tests::ScriptedRuntimeFixture scripted;
    std::shared_ptr<coding_agent::ModelRuntime> runtime{scripted.runtime};
    tests::RuntimeFixture runtime_fixture;
    std::unique_ptr<coding_agent::AgentSession> session;

    void create() {
        session_file = workspace.path() / "footer-session.jsonl";
        auto store = harness::session::SessionStore::create_new(
            session_file,
            {
                .session_id = "footer-session",
                .created_at = "2026-08-10T00:00:00Z",
                .workspace = workspace.path(),
                .provider = "fake",
                .model = "fake-model",
            });
        REQUIRE(store);
        REQUIRE(store->append(ai::MessageVariant{
            ai::user_text_message("resume request", 1'700'000'000'000)}));
        ai::AssistantMessage assistant;
        assistant.provider = "fake";
        assistant.api = "fake";
        assistant.model = "fake-model";
        assistant.stop_reason = ai::AssistantStopReason::Stop;
        assistant.timestamp = 1'700'000'000'001;
        assistant.content.emplace_back(ai::text_content(
            "resumed reply text"));
        REQUIRE(store->append(ai::MessageVariant{assistant}));

        coding_agent::runtime::AgentSessionCreationRequest request;
        request.session_target =
            coding_agent::ExplicitResumeSessionTarget{session_file};
        request.execution_runtime_target = runtime_fixture.make_target();
        request.workspace = workspace.path();
        request.session_facts.no_skills = true;
        request.session_facts.no_prompt_templates = true;
        request.model_runtime = runtime;
        auto created =
                runtime_fixture.run(coding_agent::create_agent_session_async(std::move(request), std::nullopt, {}));
        REQUIRE(created);
        session = std::move(created->session);
    }
};

/// pi `error_terminal` for the retry path: an `error` terminal with a
/// retryable provider message.
[[nodiscard]] ai::AssistantMessage retryable_error_terminal(std::string message) {
    auto terminal = ai::assistant_text_message("");
    terminal.stop_reason = ai::AssistantStopReason::Error;
    terminal.error_message = std::move(message);
    return terminal;
}

/// A real Unix epoch millisecond timestamp (the session file round-trip
/// rejects placeholder timestamps).
[[nodiscard]] ai::TimestampMs wall_clock_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/// The context-overflow error terminal providers return when input exceeds
/// the context window (pi's OVERFLOW_PATTERNS). Carries the resolved
/// fake-model identity so the runtime's `same_model` compaction gate opens,
/// and a real timestamp so the committed entry round-trips the tree parser.
[[nodiscard]] ai::AssistantMessage overflow_terminal() {
    auto terminal = retryable_error_terminal(
        "Your input exceeds the context window of this model");
    terminal.provider = "fake";
    terminal.api = "fake";
    terminal.model = "fake-model";
    terminal.timestamp = wall_clock_ms();
    return terminal;
}

/// The compaction summarization response (pi `summarization_response`).
[[nodiscard]] ai::AssistantMessage summarization_response() {
    auto summary = ai::assistant_text_message("## Goal\nCompacted history summary");
    summary.provider = "fake";
    summary.api = "fake";
    summary.model = "fake-model";
    summary.timestamp = wall_clock_ms();
    summary.usage = ai::Usage{};
    summary.usage.input = 3000;
    summary.usage.output = 100;
    return summary;
}


} // namespace

TEST_CASE(
    "Native TUI footer renders usage totals, cache hit rate, context, and the model",
    "[coding_agent][tui][footer][issue411]") {
    ResumedSessionFixture fixture;
    fixture.create();
    tests::RuntimeLoopDriver runtime_driver(fixture.runtime_fixture);

    // Script one turn with usage so the footer's stats line has data.
    ai::AssistantMessage turn;
    turn.provider = "fake";
    turn.api = "fake";
    turn.model = "fake-model";
    turn.stop_reason = ai::AssistantStopReason::Stop;
    turn.content.emplace_back(ai::text_content("usage turn answer"));
    turn.usage = ai::Usage{};
    turn.usage.input = 1000;
    turn.usage.output = 250;
    turn.usage.cache_read = 8000;
    turn.usage.cache_write = 1000;
    turn.usage.cost.input = 0.001;
    turn.usage.cost.output = 0.0005;
    turn.usage.cost.cache_read = 0.0002;
    turn.usage.cost.cache_write = 0.0001;
    turn.usage.cost.total = 0.0018;
    fixture.scripted.control->responses.push_back(std::move(turn));

    tui::VirtualTerminal terminal({.columns = 100, .rows = 30});
    boost::asio::io_context io;
    std::optional<support::ExpectedVoid> run_result;
    boost::asio::co_spawn(
        io,
        coding_agent::tui::run_interactive_mode(
            terminal,
            make_run(*fixture.session, fixture.config.path())),
        [&](std::exception_ptr exception, support::ExpectedVoid result) {
            CHECK(exception == nullptr);
            run_result.emplace(std::move(result));
        });
    drain_ready(io);

    // The footer pwd line carries the workspace path (no git repo).
    auto screen = visible_screen(terminal);
    CHECK(screen.find(fixture.workspace.path().string()) != std::string::npos);

    REQUIRE(terminal.inject_input("usage turn\r"));
    drain_ready(io);
    screen = visible_screen(terminal);
    // Usage totals: ↑input ↓output RcacheRead WcacheWrite, the cache hit
    // rate (8000/(1000+8000+1000) = 80%), the cost, and the right-aligned
    // model.
    CHECK(screen.find("\xe2\x86\x91" "1.0k") != std::string::npos);
    CHECK(screen.find("\xe2\x86\x93" "250") != std::string::npos);
    CHECK(screen.find("R8.0k") != std::string::npos);
    CHECK(screen.find("W1.0k") != std::string::npos);
    CHECK(screen.find("CH80.0%") != std::string::npos);
    CHECK(screen.find("$0.002") != std::string::npos);
    // Context usage: the estimate from the resumed messages (no compaction).
    CHECK(screen.find("%/128k (auto)") != std::string::npos);
    // The model with its thinking level (the fake model supports no
    // reasoning, so no thinking suffix).
    CHECK(screen.find("fake-model") != std::string::npos);

    REQUIRE(terminal.inject_input("\x04"));
    drain_ready(io);
    REQUIRE(run_result);
    CHECK(*run_result);
}

TEST_CASE(
    "Native TUI shows the Working indicator while a prompt streams and clears at agent end",
    "[coding_agent][tui][status][issue411]") {
    ResumedSessionFixture fixture;
    fixture.create();
    tests::ScriptedRuntimeFixture gated;
    gated.control->gate_at = 0;
    gated.control->responses.push_back(ai::assistant_text_message("gated answer"));
    fixture.session->close();
    // Rebuild the fixture session with the gated runtime.
    coding_agent::runtime::AgentSessionCreationRequest request;
    request.session_target =
        coding_agent::ExplicitResumeSessionTarget{fixture.session_file};
    request.execution_runtime_target = fixture.runtime_fixture.make_target();
    request.workspace = fixture.workspace.path();
    request.session_facts.no_skills = true;
    request.session_facts.no_prompt_templates = true;
    request.model_runtime = gated.runtime;
    auto created =
            fixture.runtime_fixture.run(coding_agent::create_agent_session_async(std::move(request), std::nullopt, {}));
    REQUIRE(created);
    tests::RuntimeLoopDriver runtime_driver(fixture.runtime_fixture);

    tui::VirtualTerminal terminal({.columns = 100, .rows = 30});
    boost::asio::io_context io;
    std::optional<support::ExpectedVoid> run_result;
    boost::asio::co_spawn(
        io,
        coding_agent::tui::run_interactive_mode(
            terminal,
            make_run(*created->session, fixture.config.path())),
        [&](std::exception_ptr exception, support::ExpectedVoid result) {
            CHECK(exception == nullptr);
            run_result.emplace(std::move(result));
        });
    drain_ready(io);

    REQUIRE(terminal.inject_input("stream\r"));
    drain_ready(io);
    auto screen = visible_screen(terminal);
    // pi WorkingStatusIndicator: "Working..." with the accent spinner.
    CHECK(screen.find("Working...") != std::string::npos);

    gated.control->release();
    drain_ready(io);
    screen = visible_screen(terminal);
    // agent_end clears the indicator back to the two-row idle status.
    CHECK(screen.find("Working...") == std::string::npos);
    CHECK(screen.find("gated answer") != std::string::npos);

    REQUIRE(terminal.inject_input("\x04"));
    drain_ready(io);
    REQUIRE(run_result);
    CHECK(*run_result);
}

TEST_CASE(
    "Native TUI retry indicator counts down pi's backoff and clears on success",
    "[coding_agent][tui][status][retry][issue411]") {
    ResumedSessionFixture fixture;
    // The session reads its settings from the Agent Config Directory.
    const tests::EnvVarGuard agent_dir{
        "PI_CODING_AGENT_DIR", fixture.config.path().string()};
    fixture.create();
    tests::RuntimeLoopDriver runtime_driver(fixture.runtime_fixture);
    fixture.config.write(
        "settings.json",
        R"({"retry": {"enabled": true, "maxRetries": 3, "baseDelayMs": 2000}})");
    fixture.scripted.control->responses.push_back(retryable_error_terminal("overloaded_error"));
    fixture.scripted.control->responses.push_back(ai::assistant_text_message("Recovered after retry"));

    tui::VirtualTerminal terminal({.columns = 100, .rows = 30});
    boost::asio::io_context io;
    std::optional<support::ExpectedVoid> run_result;
    boost::asio::co_spawn(
        io,
        coding_agent::tui::run_interactive_mode(
            terminal,
            make_run(*fixture.session, fixture.config.path())),
        [&](std::exception_ptr exception, support::ExpectedVoid result) {
            CHECK(exception == nullptr);
            run_result.emplace(std::move(result));
        });
    drain_ready(io);

    REQUIRE(terminal.inject_input("retry me\r"));
    drain_ready(io);
    auto screen = visible_screen(terminal);
    // The RetryStatusIndicator with the initial countdown (ceil(2s)).
    CHECK(screen.find("Retrying (1/3) in 2s... (escape to cancel)") !=
        std::string::npos);

    // The countdown ticks once per second.
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    drain_ready(io);
    screen = visible_screen(terminal);
    CHECK(screen.find("Retrying (1/3) in 1s... (escape to cancel)") !=
        std::string::npos);

    // The backoff elapses, the retried turn succeeds, the indicator clears.
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    drain_ready(io);
    screen = visible_screen(terminal);
    CHECK(screen.find("Retrying (") == std::string::npos);
    CHECK(screen.find("Recovered after retry") != std::string::npos);

    REQUIRE(terminal.inject_input("\x04"));
    drain_ready(io);
    REQUIRE(run_result);
    CHECK(*run_result);
}

TEST_CASE(
    "Native TUI shows the overflow Compaction indicator and rebuilds the chat on compaction end",
    "[coding_agent][tui][status][compaction][issue411]") {
    // A tiny keepRecentTokens budget makes the small resumed session
    // summarizable (pi's findCutPoint keeps the recent budget), so the
    // overflow auto-compaction runs without a huge transcript (which would
    // make every spinner frame's re-render too slow for the test loop).
    ResumedSessionFixture fixture;
    // The session reads its settings from the Agent Config Directory; the
    // tiny keepRecentTokens budget makes the small resumed session
    // summarizable (pi's findCutPoint keeps the recent budget), so the
    // overflow auto-compaction runs without a huge transcript (which would
    // make every spinner frame's re-render too slow for the test loop).
    const tests::EnvVarGuard agent_dir{
        "PI_CODING_AGENT_DIR", fixture.config.path().string()};
    fixture.create();
    fixture.config.write(
        "settings.json",
        R"({"compaction": {"enabled": true, "keepRecentTokens": 1, "reserveTokens": 1}})");
    tests::ScriptedRuntimeFixture gated;
    gated.control->gate_at = 1;
    gated.control->responses.push_back(overflow_terminal());
    gated.control->responses.push_back(summarization_response());
    gated.control->responses.push_back(ai::assistant_text_message("Recovered after compaction"));
    fixture.session->close();
    coding_agent::runtime::AgentSessionCreationRequest request;
    request.session_target =
        coding_agent::ExplicitResumeSessionTarget{fixture.session_file};
    request.execution_runtime_target = fixture.runtime_fixture.make_target();
    request.workspace = fixture.workspace.path();
    request.session_facts.no_skills = true;
    request.session_facts.no_prompt_templates = true;
    request.model_runtime = gated.runtime;
    auto created =
            fixture.runtime_fixture.run(coding_agent::create_agent_session_async(std::move(request), std::nullopt, {}));
    REQUIRE(created);
    tests::RuntimeLoopDriver runtime_driver(fixture.runtime_fixture);

    tui::VirtualTerminal terminal({.columns = 100, .rows = 30});
    boost::asio::io_context io;
    std::optional<support::ExpectedVoid> run_result;
    boost::asio::co_spawn(
        io,
        coding_agent::tui::run_interactive_mode(
            terminal,
            make_run(*created->session, fixture.config.path())),
        [&](std::exception_ptr exception, support::ExpectedVoid result) {
            CHECK(exception == nullptr);
            run_result.emplace(std::move(result));
        });
    drain_ready(io);

    REQUIRE(terminal.inject_input("overflow me\r"));
    drain_ready(io);
    auto screen = visible_screen(terminal);
    // The overflow terminal triggers the auto-compaction policy; while the
    // summarization is gated, the overflow Compaction indicator shows.
    CHECK(
        screen.find(
            "Context overflow detected, Auto-compacting... (escape to cancel)") !=
        std::string::npos);

    gated.control->release();
    drain_ready(io);
    screen = visible_screen(terminal);
    CHECK(screen.find("Auto-compacting") == std::string::npos);
    // The chat rebuilt from the fresh snapshot shows the collapsed compaction
    // summary (pi compaction-summary-message.ts) and the retried turn
    // succeeded; the stale overflow error is gone from the rebuilt chat.
    CHECK(screen.find("[compaction]") != std::string::npos);
    CHECK(screen.find("Compacted from 12 tokens") != std::string::npos);
    CHECK(screen.find("Recovered after compaction") != std::string::npos);
    CHECK(screen.find("exceeds the context window") == std::string::npos);

    // Expanding the tool output reveals the summary text (pi's expanded
    // compaction block).
    REQUIRE(terminal.inject_input("\x0f"));
    drain_ready(io);
    screen = visible_screen(terminal);
    CHECK(screen.find("Compacted history summary") != std::string::npos);

    REQUIRE(terminal.inject_input("\x04"));
    drain_ready(io);
    REQUIRE(run_result);
    CHECK(*run_result);
}

TEST_CASE(
    "Native TUI /reload refuses during auto-compaction with pi's streaming warning",
    "[coding_agent][tui][reload][compaction][issue418]") {
    // The tiny keepRecentTokens budget makes the small resumed session
    // summarizable so the overflow auto-compaction runs (same shape as the
    // overflow Compaction indicator test).
    ResumedSessionFixture fixture;
    const tests::EnvVarGuard agent_dir{
        "PI_CODING_AGENT_DIR", fixture.config.path().string()};
    fixture.create();
    fixture.config.write(
        "settings.json",
        R"({"compaction": {"enabled": true, "keepRecentTokens": 1, "reserveTokens": 1}})");
    tests::ScriptedRuntimeFixture gated;
    gated.control->gate_at = 1;
    gated.control->responses.push_back(overflow_terminal());
    gated.control->responses.push_back(summarization_response());
    gated.control->responses.push_back(ai::assistant_text_message("Recovered after compaction"));
    fixture.session->close();
    coding_agent::runtime::AgentSessionCreationRequest request;
    request.session_target =
        coding_agent::ExplicitResumeSessionTarget{fixture.session_file};
    request.execution_runtime_target = fixture.runtime_fixture.make_target();
    request.workspace = fixture.workspace.path();
    request.session_facts.no_skills = true;
    request.session_facts.no_prompt_templates = true;
    request.model_runtime = gated.runtime;
    auto created =
            fixture.runtime_fixture.run(coding_agent::create_agent_session_async(std::move(request), std::nullopt, {}));
    REQUIRE(created);
    tests::RuntimeLoopDriver runtime_driver(fixture.runtime_fixture);

    tui::VirtualTerminal terminal({.columns = 100, .rows = 30});
    boost::asio::io_context io;
    std::optional<support::ExpectedVoid> run_result;
    boost::asio::co_spawn(
        io,
        coding_agent::tui::run_interactive_mode(
            terminal,
            make_run(*created->session, fixture.config.path())),
        [&](std::exception_ptr exception, support::ExpectedVoid result) {
            CHECK(exception == nullptr);
            run_result.emplace(std::move(result));
        });
    drain_ready(io);

    // The overflow terminal triggers the auto-compaction policy; while the
    // summarization is gated the compaction is in flight.
    REQUIRE(terminal.inject_input("overflow me\r"));
    drain_ready(io);
    auto screen = visible_screen(terminal);
    CHECK(
        screen.find(
            "Context overflow detected, Auto-compacting... (escape to cancel)") !=
        std::string::npos);

    // pi `handleReloadCommand` refusal: during the overflow auto-compaction,
    // pi's `isStreaming` (`_isAgentRunActive`) is still true (the post-run
    // continuation loop owns the compaction), so the streaming refusal is
    // the verbatim pi warning at this seam; the reload never runs.
    REQUIRE(terminal.inject_input("/reload\r"));
    drain_ready(io);
    screen = visible_screen(terminal);
    CHECK(
        screen.find("Wait for the current response to finish before reloading.") !=
        std::string::npos);
    // No reload status line (the reload never ran).
    CHECK(
        screen.find("Reloaded keybindings, skills, prompts, themes, and context files") ==
        std::string::npos);

    gated.control->release();
    drain_ready(io);
    REQUIRE(terminal.inject_input("\x04"));
    drain_ready(io);
    REQUIRE(run_result);
    CHECK(*run_result);
}

TEST_CASE(
    "Native TUI /reload refuses during a manual compaction with pi's compaction warning",
    "[coding_agent][tui][reload][compaction][issue418]") {
    ResumedSessionFixture fixture;
    const tests::EnvVarGuard agent_dir{
        "PI_CODING_AGENT_DIR", fixture.config.path().string()};
    fixture.create();
    // A tiny keepRecentTokens budget makes the small resumed session
    // summarizable (pi's findCutPoint keeps the recent budget), so the manual
    // compaction actually runs instead of failing "session too small".
    fixture.config.write(
        "settings.json",
        R"({"compaction": {"enabled": true, "keepRecentTokens": 1, "reserveTokens": 1}})");
    // The manual compaction's summarization is the first model call; gating
    // it keeps `isCompacting` true while `isStreaming` stays false (the
    // signal pair pi's `handleReloadCommand` checks second).
    tests::ScriptedRuntimeFixture gated;
    gated.control->gate_at = 0;
    gated.control->responses.push_back(summarization_response());
    fixture.session->close();
    coding_agent::runtime::AgentSessionCreationRequest request;
    request.session_target =
        coding_agent::ExplicitResumeSessionTarget{fixture.session_file};
    request.execution_runtime_target = fixture.runtime_fixture.make_target();
    request.workspace = fixture.workspace.path();
    request.session_facts.no_skills = true;
    request.session_facts.no_prompt_templates = true;
    request.model_runtime = gated.runtime;
    auto created =
            fixture.runtime_fixture.run(coding_agent::create_agent_session_async(std::move(request), std::nullopt, {}));
    REQUIRE(created);
    tests::RuntimeLoopDriver runtime_driver(fixture.runtime_fixture);

    tui::VirtualTerminal terminal({.columns = 100, .rows = 30});
    boost::asio::io_context io;
    std::optional<support::ExpectedVoid> run_result;
    std::optional<support::Expected<coding_agent::CompactionResult>> compact_result;
    boost::asio::co_spawn(
        io,
        coding_agent::tui::run_interactive_mode(
            terminal,
            make_run(*created->session, fixture.config.path())),
        [&](std::exception_ptr exception, support::ExpectedVoid result) {
            CHECK(exception == nullptr);
            run_result.emplace(std::move(result));
        });
    drain_ready(io);

    // Joins the spawned run on scope exit while the captured terminal and
    // session are still alive (#527 failure-path drain).
    const tests::RunJoinGuard join_run{io, [&] { return run_result.has_value(); }};

    // Drive a manual compaction on the same executor: it blocks on the gated
    // summarization, so the session reports `isCompacting` with no active
    // Agent run.
    boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<void> {
            compact_result = co_await created->session->compact();
            co_return;
        },
        boost::asio::detached);
    // The compaction status animation ticks render work on a fixed cadence;
    // wait on the observable state instead of spinning on one poll pass.
    REQUIRE(tests::pump_until(io, [&] { return created->session->is_compacting(); }));
    CHECK_FALSE(created->session->is_streaming());

    // pi `handleReloadCommand` refusal: `isCompacting` (with `isStreaming`
    // false) warns verbatim and leaves the resources untouched.
    REQUIRE(terminal.inject_input("/reload\r"));
    drain_ready(io);
    auto screen = visible_screen(terminal);
    CHECK(
        screen.find("Wait for compaction to finish before reloading.") !=
        std::string::npos);
    CHECK(
        screen.find(
            "Wait for the current response to finish before reloading.") ==
        std::string::npos);

    gated.control->release();
    // Compaction completion and the deferred exit both cross persistence and
    // close hops; wait on each outcome instead of asserting after one drain.
    REQUIRE(tests::pump_until(io, [&] { return compact_result.has_value(); }));
    REQUIRE(compact_result->has_value());
    REQUIRE(terminal.inject_input("\x04"));
    REQUIRE(tests::pump_until(io, [&] { return run_result.has_value(); }));
    CHECK(*run_result);
}

TEST_CASE(
    "Native TUI app.suspend stops the TUI, keeps the run alive, and resumes on SIGCONT",
    "[coding_agent][tui][suspend][issue411]") {
    ResumedSessionFixture fixture;
    fixture.create();
    tests::RuntimeLoopDriver runtime_driver(fixture.runtime_fixture);
    int suspend_calls = 0;

    tui::VirtualTerminal terminal({.columns = 100, .rows = 30});
    boost::asio::io_context io;
    std::optional<support::ExpectedVoid> run_result;
    auto run = coding_agent::tui::InteractiveSessionRunBuilder{}
        .with_session(*fixture.session)
        .with_agent_config_directory(fixture.config.path())
        .with_action_sink(
            [&suspend_calls](std::size_t /* action_generation */,
                            coding_agent::tui::TuiActionVariant action)
            -> support::Expected<coding_agent::tui::TuiActionResultVariant> {
                if (std::holds_alternative<coding_agent::tui::SuspendProcessAction>(
                        action)) {
                    ++suspend_calls;
                }
                return coding_agent::tui::TuiActionResultVariant{std::monostate{}};
            })
        .build();
    boost::asio::co_spawn(
        io,
        coding_agent::tui::run_interactive_mode(
            terminal,
            std::move(run)),
        [&](std::exception_ptr exception, support::ExpectedVoid result) {
            CHECK(exception == nullptr);
            run_result.emplace(std::move(result));
        });
    drain_ready(io);
    CHECK(terminal.modes().started);

    // Ctrl+Z: the TUI stops (terminal restored) and the process-group stop
    // action runs; the run stays alive while suspended.
    REQUIRE(terminal.inject_input("\x1a"));
    drain_ready(io);
    CHECK(suspend_calls == 1);
    CHECK_FALSE(terminal.modes().started);
    CHECK_FALSE(run_result.has_value());

    // SIGINT while suspended is swallowed (pi's ignore handler) and the
    // process survives; the SIGCONT wait stays armed.
    REQUIRE(::raise(SIGINT) == 0);
    drain_ready(io);
    CHECK_FALSE(terminal.modes().started);
    CHECK_FALSE(run_result.has_value());

    // SIGCONT resumes: the TUI restarts and re-renders.
    REQUIRE(::raise(SIGCONT) == 0);
    drain_ready(io);
    CHECK(terminal.modes().started);
    CHECK(visible_screen(terminal).find("resumed reply text") !=
        std::string::npos);

    REQUIRE(terminal.inject_input("\x04"));
    drain_ready(io);
    REQUIRE(run_result);
    CHECK(*run_result);
}

TEST_CASE(
    "Native TUI app.editor.external edits prompt.md through VISUAL and resumes",
    "[coding_agent][tui][external-editor][issue411]") {
    ResumedSessionFixture fixture;
    fixture.create();
    tests::RuntimeLoopDriver runtime_driver(fixture.runtime_fixture);

    // A fake editor: appends a line to the prompt file, then exits 0.
    fixture.config.write(
        "fake-editor/editor",
        "#!/bin/sh\n"
        "printf '\\nAPPENDED BY EDITOR\\n' >> \"$1\"\n"
        "exit 0\n");
    const auto editor_path = fixture.config.path() / "fake-editor" / "editor";
    std::filesystem::permissions(
        editor_path,
        std::filesystem::perms::owner_exec | std::filesystem::perms::owner_read |
            std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace);
    const tests::EnvVarGuard visual{"VISUAL", editor_path.string()};

    tui::VirtualTerminal terminal({.columns = 100, .rows = 30});
    boost::asio::io_context io;
    std::optional<support::ExpectedVoid> run_result;
    boost::asio::co_spawn(
        io,
        coding_agent::tui::run_interactive_mode(
            terminal,
            make_run(*fixture.session, fixture.config.path())),
        [&](std::exception_ptr exception, support::ExpectedVoid result) {
            CHECK(exception == nullptr);
            run_result.emplace(std::move(result));
        });
    drain_ready(io);

    REQUIRE(terminal.inject_input("draft content\r"));
    drain_ready(io);
    REQUIRE(terminal.inject_input("more draft"));
    drain_ready(io);

    // Ctrl+G: the TUI stops, the editor runs over prompt.md, and the TUI
    // resumes with the edited content in the editor.
    REQUIRE(terminal.inject_input("\x07"));
    drain_ready(io);
    CHECK(terminal.modes().started);
    const auto screen = visible_screen(terminal);
    CHECK(screen.find("APPENDED BY EDITOR") != std::string::npos);

    // Clear the editor (the edited content is non-empty) before Ctrl+D.
    REQUIRE(terminal.inject_input("\x03"));
    drain_ready(io);
    REQUIRE(terminal.inject_input("\x04"));
    drain_ready(io);
    REQUIRE(run_result);
    CHECK(*run_result);
}

TEST_CASE(
    "Native TUI editor border color transitions for bash mode and thinking level",
    "[coding_agent][tui][footer][issue411]") {
    ResumedSessionFixture fixture;
    fixture.create();
    // Bash mode requires the session-owned user shell (the interactive host
    // always provides one).
    fixture.session->close();
    coding_agent::runtime::AgentSessionCreationRequest request;
    request.session_target =
        coding_agent::ExplicitResumeSessionTarget{fixture.session_file};
    request.execution_runtime_target = fixture.runtime_fixture.make_target();
    request.workspace = fixture.workspace.path();
    request.session_facts.no_skills = true;
    request.session_facts.no_prompt_templates = true;
    request.model_runtime = fixture.runtime;
    auto created = fixture.runtime_fixture.run(coding_agent::create_agent_session_async(std::move(request),
            std::nullopt,
            coding_agent::runtime::AssemblyOverrides{.model_runtime = nullptr,
                    .cli_fake = false,
                    .models = nullptr,
                    .user_shell = std::make_unique<tests::FakeUserShell>()}));
    REQUIRE(created);
    tests::RuntimeLoopDriver runtime_driver(fixture.runtime_fixture);

    tui::VirtualTerminal terminal({.columns = 100, .rows = 30});
    boost::asio::io_context io;
    std::optional<support::ExpectedVoid> run_result;
    boost::asio::co_spawn(
        io,
        coding_agent::tui::run_interactive_mode(
            terminal,
            make_run(*created->session, fixture.config.path())),
        [&](std::exception_ptr exception, support::ExpectedVoid result) {
            CHECK(exception == nullptr);
            run_result.emplace(std::move(result));
        });
    drain_ready(io);

    // The editor renders with a top border; find its row via the screen's
    // border line above the editor content.
    const auto border_row = [&terminal]() -> std::optional<std::size_t> {
        const auto& cells = terminal.cells();
        for (std::size_t row = 0; row < cells.size(); ++row) {
            if (!cells[row].empty() &&
                cells[row][0].grapheme == "\xe2\x94\x80") {
                return row;
            }
        }
        return std::nullopt;
    };
    const auto idle_border = border_row();
    REQUIRE(idle_border.has_value());
    const auto idle_color = terminal.cells()[*idle_border][0].style.fg_color;

    // Bash mode (`!` prefix) switches the border to the bashMode color.
    REQUIRE(terminal.inject_input("!"));
    drain_ready(io);
    const auto bash_border = border_row();
    REQUIRE(bash_border.has_value());
    const auto bash_color = terminal.cells()[*bash_border][0].style.fg_color;
    CHECK_FALSE(bash_color.empty());
    CHECK(bash_color != idle_color);

    // Leaving bash mode restores the thinking-level border.
    REQUIRE(terminal.inject_input("\x7f"));
    drain_ready(io);
    const auto restored_border = border_row();
    REQUIRE(restored_border.has_value());
    CHECK(terminal.cells()[*restored_border][0].style.fg_color == idle_color);

    REQUIRE(terminal.inject_input("\x04"));
    drain_ready(io);
    REQUIRE(run_result);
    CHECK(*run_result);
}
