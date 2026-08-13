#include "coding_agent/tui/InteractiveMode.hpp"

#include "support/EnvVarGuard.hpp"
#include "support/FakeModelRuntime.hpp"
#include "support/FakeUserShell.hpp"
#include "support/ModelsFixture.hpp"
#include "support/TempWorkspace.hpp"

#include <cch/ai/Content.hpp>
#include <cch/harness/session/JsonlSessionStore.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include "coding_agent/AgentSession.hpp"
#include "coding_agent/runtime/SessionFactory.hpp"
#include "ai/providers/FakeProvider.hpp"

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

namespace {

void drain_ready(boost::asio::io_context& io) {
    if (io.stopped()) io.restart();
    while (io.poll() != 0) {
    }
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
/// interactive boot with an injected fake runtime.
struct ResumedSessionFixture {
    tests::TempWorkspace workspace;
    tests::TempWorkspace config;
    std::filesystem::path session_file;
    std::shared_ptr<tests::FakeModelRuntime> runtime{
        std::make_shared<tests::FakeModelRuntime>()};
    std::unique_ptr<coding_agent::AgentSession> session;

    void create() {
        session_file = workspace.path() / "footer-session.jsonl";
        auto store = harness::session::JsonlSessionStore::create_new(
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
        request.workspace = workspace.path();
        request.no_skills = true;
        request.no_prompt_templates = true;
        request.model_runtime = runtime;
        auto created = coding_agent::create_agent_session_for_testing(
            std::move(request), ai::providers::make_scripted_fake_models());
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

/// A gated recording ModelRuntime for the status tests: scripted responses
/// are served in FIFO order until the call at index `gate_at`, which blocks
/// until release() (or the run's stop token fires). Standalone (not derived
/// from the recording fake, which stays `final` per §7.2) with the same
/// session-seam overrides.
class GatedModelRuntime final : public coding_agent::ModelRuntime {
public:
    explicit GatedModelRuntime(std::size_t gate_at) : gate_at_(gate_at) {}

    [[nodiscard]] std::optional<ai::Model> model(
        std::string_view provider_id,
        std::string_view model_id) const override {
        ai::Model model;
        model.id = std::string{model_id};
        model.name = model.id;
        model.api = "scripted-fake";
        model.provider = std::string{provider_id};
        model.reasoning = false;
        model.input = {ai::ModelInput::Text};
        model.context_window = 128000;
        model.max_tokens = 16384;
        return model;
    }

    boost::asio::awaitable<util::Expected<std::vector<ai::Model>>> get_available(
        std::optional<std::string_view> provider_id = std::nullopt) override {
        std::vector<ai::Model> models;
        if (auto resolved = model(provider_id.value_or("fake"), "fake-model")) {
            models.push_back(std::move(*resolved));
        }
        co_return models;
    }

    boost::asio::awaitable<util::Expected<std::optional<ai::AuthCheck>>> check_auth(
        std::string provider_id) override {
        (void)provider_id;
        co_return ai::AuthCheck{
            .source = "fake",
            .type = ai::AuthType::ApiKey,
        };
    }

    bool has_configured_auth(std::string_view provider_id) const override {
        (void)provider_id;
        return true;
    }

    std::vector<std::string> configured_api_key_env_names() const override {
        return {};
    }

    boost::asio::awaitable<util::Expected<ai::AssistantMessage>> stream_simple(
        ai::Model model,
        ai::AiContext context,
        ai::SimpleStreamOptions options,
        ai::AssistantEventSink sink) override {
        const std::size_t call_index = calls.size();
        calls.push_back(tests::RecordedStreamSimpleCall{
            std::move(model),
            std::move(context),
            std::move(options),
        });
        if (call_index == gate_at_) {
            // Hold the gated call; release on the run's stop token.
            const auto& stop_token = calls.back().options.stop_token;
            const auto executor = co_await boost::asio::this_coro::executor;
            gate_.emplace(executor);
            gate_->expires_at(std::chrono::steady_clock::time_point::max());
            std::stop_callback release_on_stop{stop_token, [this] { release(); }};
            boost::system::error_code error;
            co_await gate_->async_wait(
                boost::asio::redirect_error(boost::asio::use_awaitable, error));
        }
        if (responses.empty()) {
            co_return ai::assistant_text_message("default gated response");
        }
        auto response = std::move(responses.front());
        responses.pop_front();
        if (sink) {
            if (response.stop_reason == ai::AssistantStopReason::Error ||
                response.stop_reason == ai::AssistantStopReason::Aborted) {
                std::optional<util::Error> terminal_failure = std::nullopt;
                if (response.error_message) {
                    terminal_failure = util::make_error(
                        util::ErrorCode::Stream, *response.error_message);
                }
                CCH_TRY_VOID(sink(ai::AssistantErrorEvent{
                    .reason = response.stop_reason,
                    .error = response,
                    .failure = std::move(terminal_failure),
                }));
                co_return response;
            }
            CCH_TRY_VOID(sink(ai::AssistantStartEvent{response}));
            for (std::size_t index = 0; index < response.content.size(); ++index) {
                const auto& block = response.content[index];
                if (const auto* text = std::get_if<ai::TextContent>(&block)) {
                    CCH_TRY_VOID(sink(ai::TextDeltaEvent{index, text->text, response}));
                } else if (const auto* thinking = std::get_if<ai::ThinkingContent>(&block)) {
                    CCH_TRY_VOID(sink(ai::ThinkingDeltaEvent{index, thinking->thinking, response}));
                }
            }
        }
        co_return response;
    }

    void release() {
        if (gate_) (void)gate_->cancel();
    }

    /// Recorded per-turn calls in issue order.
    std::vector<tests::RecordedStreamSimpleCall> calls;
    /// Scripted assistant responses consumed in FIFO order.
    std::deque<ai::AssistantMessage> responses;

private:
    std::size_t gate_at_{0};
    std::optional<boost::asio::steady_timer> gate_;
};

} // namespace

TEST_CASE(
    "Native TUI footer renders usage totals, cache hit rate, context, and the model",
    "[coding_agent][tui][footer][issue411]") {
    ResumedSessionFixture fixture;
    fixture.create();

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
    fixture.runtime->responses.push_back(std::move(turn));

    tui::VirtualTerminal terminal({.columns = 100, .rows = 30});
    boost::asio::io_context io;
    std::optional<util::ExpectedVoid> run_result;
    boost::asio::co_spawn(
        io,
        coding_agent::tui::run_interactive_mode(
            *fixture.session,
            terminal,
            {.agent_config_directory = fixture.config.path()}),
        [&](std::exception_ptr exception, util::ExpectedVoid result) {
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
    auto gated = std::make_shared<GatedModelRuntime>(0);
    gated->responses.push_back(ai::assistant_text_message("gated answer"));
    fixture.session->close();
    // Rebuild the fixture session with the gated runtime.
    coding_agent::runtime::AgentSessionCreationRequest request;
    request.session_target =
        coding_agent::ExplicitResumeSessionTarget{fixture.session_file};
    request.workspace = fixture.workspace.path();
    request.no_skills = true;
    request.no_prompt_templates = true;
    request.model_runtime = gated;
    auto created = coding_agent::create_agent_session_for_testing(
        std::move(request), ai::providers::make_scripted_fake_models());
    REQUIRE(created);

    tui::VirtualTerminal terminal({.columns = 100, .rows = 30});
    boost::asio::io_context io;
    std::optional<util::ExpectedVoid> run_result;
    boost::asio::co_spawn(
        io,
        coding_agent::tui::run_interactive_mode(
            *created->session,
            terminal,
            {.agent_config_directory = fixture.config.path()}),
        [&](std::exception_ptr exception, util::ExpectedVoid result) {
            CHECK(exception == nullptr);
            run_result.emplace(std::move(result));
        });
    drain_ready(io);

    REQUIRE(terminal.inject_input("stream\r"));
    drain_ready(io);
    auto screen = visible_screen(terminal);
    // pi WorkingStatusIndicator: "Working..." with the accent spinner.
    CHECK(screen.find("Working...") != std::string::npos);

    gated->release();
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
    fixture.config.write(
        "settings.json",
        R"({"retry": {"enabled": true, "maxRetries": 3, "baseDelayMs": 2000}})");
    fixture.runtime->responses.push_back(
        retryable_error_terminal("overloaded_error"));
    fixture.runtime->responses.push_back(
        ai::assistant_text_message("Recovered after retry"));

    tui::VirtualTerminal terminal({.columns = 100, .rows = 30});
    boost::asio::io_context io;
    std::optional<util::ExpectedVoid> run_result;
    boost::asio::co_spawn(
        io,
        coding_agent::tui::run_interactive_mode(
            *fixture.session,
            terminal,
            {.agent_config_directory = fixture.config.path()}),
        [&](std::exception_ptr exception, util::ExpectedVoid result) {
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
    auto gated = std::make_shared<GatedModelRuntime>(1);
    gated->responses.push_back(overflow_terminal());
    gated->responses.push_back(summarization_response());
    gated->responses.push_back(ai::assistant_text_message("Recovered after compaction"));
    fixture.session->close();
    coding_agent::runtime::AgentSessionCreationRequest request;
    request.session_target =
        coding_agent::ExplicitResumeSessionTarget{fixture.session_file};
    request.workspace = fixture.workspace.path();
    request.no_skills = true;
    request.no_prompt_templates = true;
    request.model_runtime = gated;
    auto created = coding_agent::create_agent_session_for_testing(
        std::move(request), ai::providers::make_scripted_fake_models());
    REQUIRE(created);

    tui::VirtualTerminal terminal({.columns = 100, .rows = 30});
    boost::asio::io_context io;
    std::optional<util::ExpectedVoid> run_result;
    boost::asio::co_spawn(
        io,
        coding_agent::tui::run_interactive_mode(
            *created->session,
            terminal,
            {.agent_config_directory = fixture.config.path()}),
        [&](std::exception_ptr exception, util::ExpectedVoid result) {
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

    gated->release();
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
    auto gated = std::make_shared<GatedModelRuntime>(1);
    gated->responses.push_back(overflow_terminal());
    gated->responses.push_back(summarization_response());
    gated->responses.push_back(ai::assistant_text_message("Recovered after compaction"));
    fixture.session->close();
    coding_agent::runtime::AgentSessionCreationRequest request;
    request.session_target =
        coding_agent::ExplicitResumeSessionTarget{fixture.session_file};
    request.workspace = fixture.workspace.path();
    request.no_skills = true;
    request.no_prompt_templates = true;
    request.model_runtime = gated;
    auto created = coding_agent::create_agent_session_for_testing(
        std::move(request), ai::providers::make_scripted_fake_models());
    REQUIRE(created);

    tui::VirtualTerminal terminal({.columns = 100, .rows = 30});
    boost::asio::io_context io;
    std::optional<util::ExpectedVoid> run_result;
    boost::asio::co_spawn(
        io,
        coding_agent::tui::run_interactive_mode(
            *created->session,
            terminal,
            {.agent_config_directory = fixture.config.path()}),
        [&](std::exception_ptr exception, util::ExpectedVoid result) {
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

    gated->release();
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
    auto gated = std::make_shared<GatedModelRuntime>(0);
    gated->responses.push_back(summarization_response());
    fixture.session->close();
    coding_agent::runtime::AgentSessionCreationRequest request;
    request.session_target =
        coding_agent::ExplicitResumeSessionTarget{fixture.session_file};
    request.workspace = fixture.workspace.path();
    request.no_skills = true;
    request.no_prompt_templates = true;
    request.model_runtime = gated;
    auto created = coding_agent::create_agent_session_for_testing(
        std::move(request), ai::providers::make_scripted_fake_models());
    REQUIRE(created);

    tui::VirtualTerminal terminal({.columns = 100, .rows = 30});
    boost::asio::io_context io;
    std::optional<util::ExpectedVoid> run_result;
    std::optional<util::Expected<coding_agent::CompactionResult>> compact_result;
    boost::asio::co_spawn(
        io,
        coding_agent::tui::run_interactive_mode(
            *created->session,
            terminal,
            {.agent_config_directory = fixture.config.path()}),
        [&](std::exception_ptr exception, util::ExpectedVoid result) {
            CHECK(exception == nullptr);
            run_result.emplace(std::move(result));
        });
    drain_ready(io);

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
    while (!created->session->is_compacting() && io.poll() != 0) {
    }
    REQUIRE(created->session->is_compacting());
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

    gated->release();
    drain_ready(io);
    REQUIRE(compact_result.has_value());
    REQUIRE(compact_result->has_value());
    REQUIRE(terminal.inject_input("\x04"));
    drain_ready(io);
    REQUIRE(run_result);
    CHECK(*run_result);
}

TEST_CASE(
    "Native TUI app.suspend stops the TUI, keeps the run alive, and resumes on SIGCONT",
    "[coding_agent][tui][suspend][issue411]") {
    ResumedSessionFixture fixture;
    fixture.create();
    int suspend_calls = 0;

    tui::VirtualTerminal terminal({.columns = 100, .rows = 30});
    boost::asio::io_context io;
    std::optional<util::ExpectedVoid> run_result;
    coding_agent::tui::InteractiveModeConfig config;
    config.agent_config_directory = fixture.config.path();
    // The test never sends a real SIGTSTP to its own process group; the
    // recorder stands in for pi's `process.kill(0, "SIGTSTP")`.
    config.suspend_process_sink = [&suspend_calls] { ++suspend_calls; };
    boost::asio::co_spawn(
        io,
        coding_agent::tui::run_interactive_mode(
            *fixture.session,
            terminal,
            std::move(config)),
        [&](std::exception_ptr exception, util::ExpectedVoid result) {
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
    std::optional<util::ExpectedVoid> run_result;
    boost::asio::co_spawn(
        io,
        coding_agent::tui::run_interactive_mode(
            *fixture.session,
            terminal,
            {.agent_config_directory = fixture.config.path()}),
        [&](std::exception_ptr exception, util::ExpectedVoid result) {
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
    request.workspace = fixture.workspace.path();
    request.no_skills = true;
    request.no_prompt_templates = true;
    request.model_runtime = fixture.runtime;
    auto created = coding_agent::create_agent_session_for_testing(
        std::move(request),
        ai::providers::make_scripted_fake_models(),
        std::make_unique<tests::FakeUserShell>());
    REQUIRE(created);

    tui::VirtualTerminal terminal({.columns = 100, .rows = 30});
    boost::asio::io_context io;
    std::optional<util::ExpectedVoid> run_result;
    boost::asio::co_spawn(
        io,
        coding_agent::tui::run_interactive_mode(
            *created->session,
            terminal,
            {.agent_config_directory = fixture.config.path()}),
        [&](std::exception_ptr exception, util::ExpectedVoid result) {
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
