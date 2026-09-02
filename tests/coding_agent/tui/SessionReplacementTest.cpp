// Reuse one Runtime root across Session replacement (issue #466): New,
// Fork, and Session switching quiesce the previous current Session before
// installing the replacement while one shared Runtime loop, worker capacity,
// and Models resources are retained.
//
// Coverage per the ticket's acceptance criteria:
// - successful replacement installs a working Session and the previous
//   Session is closed before the replacement becomes current;
// - a failed load/fork keeps the previous Session running;
// - replacement during an active prompt cancels the previous Session's run
//   and the replacement accepts a fresh prompt immediately (the retired
//   Session's late completion cannot render or un-gate the new Session);
// - rapid repeated switching stays interactive;
// - shutdown during a transition exits cleanly.

#include "coding_agent/tui/InteractiveMode.hpp"
#include "coding_agent/tui/InteractiveSessionRun.hpp"
#include "coding_agent/tui/TestTuiActionSink.hpp"

#include "support/AsyncResultBridge.hpp"
#include "support/EnvVarGuard.hpp"
#include "support/GatedChatProvider.hpp"
#include "support/ModelsFixture.hpp"
#include "support/PumpUntil.hpp"
#include "support/RuntimeFixture.hpp"
#include "support/RuntimeLoopDriver.hpp"
#include "support/TempWorkspace.hpp"

#include "coding_agent/AgentSession.hpp"
#include "coding_agent/runtime/AsyncUserShell.hpp"
#include "coding_agent/runtime/SessionFactory.hpp"
#include "agent/harness/RuntimeRoot.hpp"
#include <cch/tui/VirtualTerminal.hpp>

#include <cch/support/Error.hpp>
#include <catch2/catch_test_macros.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <chrono>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <atomic>
#include <stop_token>
#include <string>
#include <vector>

using namespace cch;
using tests::drain_ready;

namespace {

[[nodiscard]] std::string visible_screen(const tui::VirtualTerminal& terminal) {
    std::string text;
    for (const auto& line : terminal.screen()) {
        text.append(line);
        text.push_back('\n');
    }
    return text;
}

struct Fixture {
    tests::TempWorkspace workspace;
    tests::TempWorkspace agent_dir;
    tests::EnvVarGuard agent_dir_guard{"PI_CODING_AGENT_DIR"};
    tests::RuntimeFixture runtime;
    tests::RuntimeLoopDriver runtime_driver;

    Fixture() : runtime_driver(runtime) { agent_dir_guard.set(agent_dir.path().string()); }
};

struct Running {
    tui::VirtualTerminal terminal{tui::VirtualTerminalOptions{.columns = 100, .rows = 40}};
    boost::asio::io_context io;
    std::optional<support::ExpectedVoid> run_result;
};

/// The replace-session creator shared by these tests (pi `createRuntime`):
/// every created Session shares one GatedChatProvider, so the Runtime's
/// provider (Models) resource is reused across replacements and the shared
/// provider's request/gate state is observable end to end (issue #466).
[[nodiscard]] coding_agent::tui::AsyncSessionReplacementSink shared_provider_creator(
        const std::shared_ptr<tests::GatedChatProvider>& provider,
        std::shared_ptr<harness::RuntimeTarget> runtime_target) {
    return [provider, runtime_target](std::size_t /* action_generation */,
                   coding_agent::runtime::AgentSessionCreationRequest request,
                   std::stop_token stop_token) -> support::AsyncResult<coding_agent::CreateAgentSessionResult> {
        request.session_facts.no_skills = true;
        request.session_facts.no_prompt_templates = true;
        request.execution_runtime_target = runtime_target;
        return coding_agent::create_agent_session_async(std::move(request),
                std::nullopt,
                coding_agent::runtime::AssemblyOverrides{.model_runtime = nullptr,
                        .models = tests::models_from_provider(provider),
                        .user_shell = nullptr},
                stop_token);
    };
}

/// Boot the interactive mode through the deferred-boot entry (the boot
/// session is created through the asynchronous replacement sink).
void boot(
    Fixture& fixture,
    Running& running,
    std::shared_ptr<coding_agent::tui::testing::ActionSinkRecorder> actions,
    coding_agent::runtime::AgentSessionCreationRequest request = {}) {
    if (request.workspace.empty()) request.workspace = fixture.workspace.path();
    if (std::holds_alternative<coding_agent::DefaultPersistedSessionTarget>(
            request.session_target)) {
        request.session_target = coding_agent::InMemorySessionTarget{};
    }
    auto runtime_io = std::shared_ptr<boost::asio::io_context>(&running.io, [](boost::asio::io_context*) {});
    auto runtime_root = std::make_shared<harness::RuntimeRoot>(std::move(runtime_io), harness::RuntimeLimits{});
    auto run = coding_agent::tui::InteractiveSessionRunBuilder{}
                       .with_defer_boot(std::move(request))
                       .with_agent_config_directory(fixture.agent_dir.path())
                       .with_action_sink(actions->make_sink())
                       .with_async_session_replacement_sink(actions->make_async_session_replacement_sink())
                       .with_runtime_root(std::move(runtime_root))
                       .build();
    boost::asio::co_spawn(
        running.io,
        coding_agent::tui::run_interactive_mode(
            running.terminal,
            std::move(run)),
        [&](std::exception_ptr exception, support::ExpectedVoid result) {
            CHECK(exception == nullptr);
            running.run_result.emplace(std::move(result));
        });
    // The deferred boot's session creation is asynchronous: wait until the
    // replacement result has crossed the seam, then drain so the engine's
    // installation continuation has run before the test drives input.
    REQUIRE(tests::pump_until(
            running.io, [&] { return actions->replacement_completions.load(std::memory_order_acquire) >= 1; }));
    drain_ready(running.io);
}

/// Wait until the `completions`-th asynchronous replacement result has
/// crossed the seam and the engine has installed the outcome before the
/// test drives more input or asserts the screen (a `/new` replacement is
/// still in flight until then; input driven mid-transition is dropped).
void wait_replacement(Running& running,
        const std::shared_ptr<coding_agent::tui::testing::ActionSinkRecorder>& actions,
        std::size_t completions) {
    REQUIRE(tests::pump_until(running.io,
            [&] { return actions->replacement_completions.load(std::memory_order_acquire) >= completions; }));
    drain_ready(running.io);
}

/// Pump until the screen shows `text`: a replacement install, a turn
/// completion, or a warning can render one loop turn after a plain drain
/// observed a quiet queue.
void wait_for_screen(Running& running, const std::string& text) {
    REQUIRE(tests::pump_until(
            running.io, [&] { return visible_screen(running.terminal).find(text) != std::string::npos; }));
}

/// A User Shell whose gating and recording live in one shared state, so the
/// boot Session and its replacement share the same shell resource and a User
/// Bash started before /new stays in flight across the replacement.
class SharedGateShell final : public coding_agent::runtime::AsyncUserShell {
public:
    struct State {
        std::vector<std::string> commands;
        std::size_t started{0};
        std::size_t cancellation_requests{0};
        std::optional<boost::asio::steady_timer> gate;
    };

    explicit SharedGateShell(std::shared_ptr<State> state) : state_(std::move(state)) {}

    [[nodiscard]] support::AsyncResult<
        coding_agent::runtime::UserShellResult>
    execute(
        std::string command,
        coding_agent::runtime::UserShellUpdateSink update_sink,
        std::stop_token stop_token) override {
        return support::detail::make_async_result(
                [state = state_,
                        command = std::move(command),
                        update_sink = std::move(update_sink),
                        stop_token]() mutable
                        -> boost::asio::awaitable<support::Expected<coding_agent::runtime::UserShellResult>> {
                    state->commands.push_back(command);
                    ++state->started;
                    if (update_sink) {
                        if (auto delivered = update_sink(std::format("$ {} starting\n", command)); !delivered) {
                            co_return std::unexpected(delivered.error());
                        }
                    }
                    const auto executor = co_await boost::asio::this_coro::executor;
                    state->gate.emplace(executor);
                    state->gate->expires_at(std::chrono::steady_clock::time_point::max());
                    std::stop_callback cancellation{stop_token, [state] {
                                                        ++state->cancellation_requests;
                                                        if (state->gate) {
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
                                                            try {
#endif
                                                                (void)state->gate->cancel();
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
                                                            } catch (...) {
                                                            }
#endif
                                                        }
                                                    }};
                    boost::system::error_code error;
                    if (!stop_token.stop_requested()) {
                        co_await state->gate->async_wait(
                                boost::asio::redirect_error(boost::asio::use_awaitable, error));
                    }
                    state->gate.reset();
                    coding_agent::runtime::UserShellResult result;
                    if (stop_token.stop_requested()) {
                        result.cancelled = true;
                    } else {
                        result.exit_code = 0;
                    }
                    co_return result;
                });
    }

    [[nodiscard]] std::shared_ptr<State> state() const { return state_; }

private:
    std::shared_ptr<State> state_;
};

} // namespace

TEST_CASE(
    "a replacement installs a working Session and reuses the shared provider",
    "[coding_agent][tui][replacement][issue466]") {
    Fixture fixture;
    Running running;
    auto provider = std::make_shared<tests::GatedChatProvider>();
    auto actions = std::make_shared<coding_agent::tui::testing::ActionSinkRecorder>();
    actions->replace_session_async = shared_provider_creator(provider, fixture.runtime.make_target());
    boot(fixture, running, actions);

    // The boot session bound through one replacement request.
    REQUIRE(actions->replace_sessions.size() == 1);

    // A prompt on the boot Session streams through the shared provider.
    REQUIRE(running.terminal.inject_input("first prompt\r"));
    drain_ready(running.io);
    REQUIRE(provider->requests.size() == 1);
    provider->release();
    wait_for_screen(running, "turn 1");

    // /new replaces the Session; the replacement binds and stays interactive.
    REQUIRE(running.terminal.inject_input("/new\r"));
    wait_replacement(running, actions, 2);
    CHECK(visible_screen(running.terminal).find("✓ New session started") != std::string::npos);
    REQUIRE(actions->replace_sessions.size() == 2);

    // A prompt on the replacement runs through the same shared provider (the
    // Runtime's Models resource was reused, not reconstructed).
    REQUIRE(running.terminal.inject_input("second prompt\r"));
    drain_ready(running.io);
    REQUIRE(provider->requests.size() == 2);
    provider->release();
    wait_for_screen(running, "turn 2");

    REQUIRE(running.terminal.inject_input("\x04"));
    drain_ready(running.io);
    REQUIRE(running.run_result);
    CHECK(*running.run_result);
}

TEST_CASE(
    "a failed replacement keeps the previous Session running",
    "[coding_agent][tui][replacement][issue466]") {
    Fixture fixture;
    Running running;
    auto provider = std::make_shared<tests::GatedChatProvider>();
    auto actions = std::make_shared<coding_agent::tui::testing::ActionSinkRecorder>();
    int replacement_calls = 0;
    actions->replace_session_async =
            [&replacement_calls, provider, runtime_target = fixture.runtime.make_target()](
                    std::size_t /* action_generation */,
                    coding_agent::runtime::AgentSessionCreationRequest request,
                    std::stop_token stop_token) -> support::AsyncResult<coding_agent::CreateAgentSessionResult> {
        ++replacement_calls;
        request.session_facts.no_skills = true;
        request.session_facts.no_prompt_templates = true;
        request.execution_runtime_target = runtime_target;
        if (replacement_calls > 1) {
            return support::AsyncResult<coding_agent::CreateAgentSessionResult>{
                    std::unexpected(support::make_error(support::ErrorCode::Session, "host rejected the replacement"))};
        }
        return coding_agent::create_agent_session_async(std::move(request),
                std::nullopt,
                coding_agent::runtime::AssemblyOverrides{.model_runtime = nullptr,
                        .models = tests::models_from_provider(provider),
                        .user_shell = nullptr},
                stop_token);
    };
    boot(fixture, running, actions);

    // The in-session replacement is rejected by the host; the error surfaces.
    REQUIRE(running.terminal.inject_input("/new\r"));
    wait_replacement(running, actions, 2);
    wait_for_screen(running, "host rejected the replacement");

    // The previous Session was never closed (the replacement was not
    // installed) and still accepts a prompt.
    REQUIRE(running.terminal.inject_input("still alive\r"));
    drain_ready(running.io);
    REQUIRE(provider->requests.size() == 1);
    provider->release();
    wait_for_screen(running, "turn 1");

    REQUIRE(running.terminal.inject_input("\x04"));
    drain_ready(running.io);
    REQUIRE(running.run_result);
    CHECK(*running.run_result);
}

TEST_CASE(
    "replacement retires the previous Session's late prompt so the replacement accepts a fresh prompt",
    "[coding_agent][tui][replacement][issue466]") {
    Fixture fixture;
    Running running;
    auto provider = std::make_shared<tests::GatedChatProvider>();
    auto actions = std::make_shared<coding_agent::tui::testing::ActionSinkRecorder>();
    actions->replace_session_async = shared_provider_creator(provider, fixture.runtime.make_target());
    boot(fixture, running, actions);

    // Start a prompt on the boot Session; the shared provider gates it (the
    // run stays in flight across the replacement, so its late completion is
    // observable after the Session is retired).
    REQUIRE(running.terminal.inject_input("start A\r"));
    drain_ready(running.io);
    REQUIRE(provider->requests.size() == 1);

    // /new during the active run: the previous Session's prompt admission
    // stops and its active work is cancelled before the replacement installs.
    REQUIRE(running.terminal.inject_input("/new\r"));
    wait_replacement(running, actions, 2);
    CHECK(visible_screen(running.terminal).find("✓ New session started") != std::string::npos);

    // The replacement accepts a fresh prompt immediately: the retired
    // Session's in-flight prompt no longer gates the current Session (exactly
    // one Session accepts prompts during the transition).
    REQUIRE(running.terminal.inject_input("start B\r"));
    REQUIRE(tests::pump_until(running.io, [&] { return provider->requests.size() == 2; }));

    // The replacement's turn completes and renders; the retired Session's
    // late completion is dropped and cannot render as the new Session.
    provider->release();
    wait_for_screen(running, "turn 2");

    REQUIRE(running.terminal.inject_input("\x04"));
    drain_ready(running.io);
    REQUIRE(running.run_result);
    CHECK(*running.run_result);
}

TEST_CASE(
    "rapid repeated Session replacement stays interactive",
    "[coding_agent][tui][replacement][issue466]") {
    Fixture fixture;
    Running running;
    auto provider = std::make_shared<tests::GatedChatProvider>();
    auto actions = std::make_shared<coding_agent::tui::testing::ActionSinkRecorder>();
    actions->replace_session_async = shared_provider_creator(provider, fixture.runtime.make_target());
    boot(fixture, running, actions);

    // Three back-to-back replacements: each installs and retires the previous
    // Session without blocking the run. Each replacement is awaited before
    // the next is driven: input driven mid-transition is dropped by the
    // engine while the asynchronous creation is still in flight.
    REQUIRE(running.terminal.inject_input("/new\r"));
    wait_replacement(running, actions, 2);
    REQUIRE(running.terminal.inject_input("/new\r"));
    wait_replacement(running, actions, 3);
    REQUIRE(running.terminal.inject_input("/new\r"));
    wait_replacement(running, actions, 4);
    REQUIRE(actions->replace_sessions.size() == 4);

    // The final Session is interactive.
    REQUIRE(running.terminal.inject_input("after rapid switches\r"));
    drain_ready(running.io);
    REQUIRE(provider->requests.size() == 1);
    provider->release();
    wait_for_screen(running, "turn 1");

    REQUIRE(running.terminal.inject_input("\x04"));
    drain_ready(running.io);
    REQUIRE(running.run_result);
    CHECK(*running.run_result);
}

TEST_CASE(
    "shutdown during a Session transition exits cleanly",
    "[coding_agent][tui][replacement][issue466]") {
    Fixture fixture;
    Running running;
    auto provider = std::make_shared<tests::GatedChatProvider>();
    auto actions = std::make_shared<coding_agent::tui::testing::ActionSinkRecorder>();
    actions->replace_session_async = shared_provider_creator(provider, fixture.runtime.make_target());
    boot(fixture, running, actions);

    // A prompt is in flight on the boot Session when the user replaces and
    // immediately exits: the transition and the shutdown complete cleanly
    // with the replacement installed.
    REQUIRE(running.terminal.inject_input("prompt in flight\r"));
    drain_ready(running.io);
    REQUIRE(provider->requests.size() == 1);

    REQUIRE(running.terminal.inject_input("/new\r"));
    drain_ready(running.io);
    REQUIRE(running.terminal.inject_input("\x04"));
    drain_ready(running.io);

    // The retired Session's prompt is still gated when the app closes; the
    // run must not hang. The exit completes once the in-flight transition
    // settles, which can cross the Runtime loop after a drain went quiet.
    REQUIRE(tests::pump_until(running.io, [&] { return running.run_result.has_value(); }));
    CHECK(*running.run_result);
    // Settle the orphaned prompt so the test's loop can be destroyed cleanly.
    provider->release();
    drain_ready(running.io);
}

TEST_CASE(
    "replacement clears a retired Session's pending User Bash block",
    "[coding_agent][tui][replacement][issue466]") {
    Fixture fixture;
    Running running;
    auto shell_state = std::make_shared<SharedGateShell::State>();
    auto actions = std::make_shared<coding_agent::tui::testing::ActionSinkRecorder>();
    actions->replace_session_async =
            [shell_state, runtime_target = fixture.runtime.make_target()](std::size_t /* action_generation */,
                    coding_agent::runtime::AgentSessionCreationRequest request,
                    std::stop_token stop_token) -> support::AsyncResult<coding_agent::CreateAgentSessionResult> {
        request.session_facts.no_skills = true;
        request.session_facts.no_prompt_templates = true;
        request.execution_runtime_target = runtime_target;
        return coding_agent::create_agent_session_async(std::move(request),
                std::nullopt,
                coding_agent::runtime::AssemblyOverrides{.model_runtime = nullptr,
                        .models = tests::make_scripted_fake_models(),
                        .user_shell = std::make_unique<SharedGateShell>(shell_state)},
                stop_token);
    };
    boot(fixture, running, actions);

    // A User Bash starts on the boot Session and stays in flight (gated); its
    // pending block renders with the running loader.
    REQUIRE(running.terminal.inject_input("! long running\r"));
    drain_ready(running.io);
    REQUIRE(shell_state->commands.size() == 1);
    CHECK(shell_state->started == 1);
    CHECK(visible_screen(running.terminal).find("Running...") != std::string::npos);

    // /new while the User Bash is in flight: the previous Session's pending
    // bash block is cleared from the replacement's view (criterion 5), and
    // the old Session's close requested cancellation of the in-flight bash.
    REQUIRE(running.terminal.inject_input("/new\r"));
    wait_replacement(running, actions, 2);
    CHECK(shell_state->cancellation_requests == 1);
    CHECK(visible_screen(running.terminal).find("Running...") == std::string::npos);
    CHECK(visible_screen(running.terminal).find("✓ New session started") != std::string::npos);

    REQUIRE(running.terminal.inject_input("\x04"));
    drain_ready(running.io);
    REQUIRE(running.run_result);
    CHECK(*running.run_result);
}
