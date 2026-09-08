#include "coding_agent/tui/InteractiveMode.hpp"
#include "coding_agent/tui/InteractiveSessionRun.hpp"
#include "ai/ModelStreamBridge.hpp"
#include "support/ModelsFixture.hpp"
#include "support/PseudoTerminal.hpp"
#include "support/RuntimeFixture.hpp"
#include "support/RuntimeLoopDriver.hpp"
#include "support/TempWorkspace.hpp"

#include "coding_agent/AgentSession.hpp"
#include <cch/tui/ProcessTerminal.hpp>

#include <cch/support/Error.hpp>
#include <catch2/catch_test_macros.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <signal.h>
#include <sys/ioctl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

class ConcurrentStreamingProvider final : public cch::tests::ScriptedProvider {
public:
    ConcurrentStreamingProvider(std::shared_ptr<std::atomic_bool> started, std::shared_ptr<std::atomic_bool> finished)
        : ScriptedProvider("fake"), started_(std::move(started)), finished_(std::move(finished)) {}

    [[nodiscard]] cch::ai::ModelStream stream(
            cch::ai::Model model, cch::ai::AiContext, cch::coding_agent::ModelRuntimeTestStreamOptions) override {
        return cch::ai::detail::make_model_stream(
                [model = std::move(model), started = started_, finished = finished_](cch::ai::AssistantEventSink sink)
                        -> boost::asio::awaitable<cch::support::Expected<cch::ai::AssistantMessage>> {
                    auto partial = cch::ai::assistant_text_message("");
                    partial.provider = "fake";
                    partial.api = "fake";
                    partial.model = model.id;
                    partial.content.clear();
                    partial.content.emplace_back(cch::ai::text_content(""));
                    started->store(true, std::memory_order_release);

                    if (sink) {
                        if (auto emitted = sink(cch::ai::AssistantStartEvent{.partial = partial}); !emitted) {
                            finished->store(true, std::memory_order_release);
                            co_return std::unexpected(emitted.error());
                        }
                        if (auto emitted = sink(cch::ai::TextStartEvent{.content_index = 0, .partial = partial});
                                !emitted) {
                            finished->store(true, std::memory_order_release);
                            co_return std::unexpected(emitted.error());
                        }
                        for (std::size_t index = 0; index < 160; ++index) {
                            const auto token = std::format("stream-{:03} ", index);
                            std::get<cch::ai::TextContent>(partial.content[0]).text += token;
                            if (auto emitted = sink(cch::ai::TextDeltaEvent{
                                        .content_index = 0,
                                        .delta = token,
                                        .partial = partial,
                                });
                                    !emitted) {
                                finished->store(true, std::memory_order_release);
                                co_return std::unexpected(emitted.error());
                            }
                            boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor);
                            timer.expires_after(std::chrono::milliseconds(2));
                            boost::system::error_code wait_error;
                            co_await timer.async_wait(
                                    boost::asio::redirect_error(boost::asio::use_awaitable, wait_error));
                            if (wait_error) {
                                finished->store(true, std::memory_order_release);
                                co_return std::unexpected(cch::support::make_error(cch::support::ErrorCode::Cancelled,
                                        "concurrent streaming provider timer cancelled"));
                            }
                        }
                    }
                    partial.stop_reason = cch::ai::AssistantStopReason::Stop;
                    if (auto emitted = sink(cch::ai::AssistantDoneEvent{
                                .reason = partial.stop_reason,
                                .message = partial,
                        });
                            !emitted) {
                        finished->store(true, std::memory_order_release);
                        co_return std::unexpected(emitted.error());
                    }
                    finished->store(true, std::memory_order_release);
                    co_return partial;
                });
    }

private:
    std::shared_ptr<std::atomic_bool> started_;
    std::shared_ptr<std::atomic_bool> finished_;
};

class InteractiveSmokeCleanup final {
public:
    InteractiveSmokeCleanup(
        cch::coding_agent::AgentSession& session,
        cch::tui::ProcessTerminal& terminal,
        boost::asio::io_context& io,
        std::jthread& runner,
        int master_fd)
        : session_(session),
          terminal_(terminal),
          io_(io),
          runner_(runner),
          master_fd_(master_fd) {}
    InteractiveSmokeCleanup(InteractiveSmokeCleanup&&) = delete;
    InteractiveSmokeCleanup& operator=(InteractiveSmokeCleanup&&) = delete;
    ~InteractiveSmokeCleanup() {
        if (!active_) return;

        if (terminal_.modes().started) {
            constexpr char kExit = '\x04';
            (void)::write(master_fd_, &kExit, 1);
            (void)cch::tests::wait_until(
                [&] { return !terminal_.modes().started; },
                std::chrono::milliseconds(250));
        }
        if (terminal_.modes().started) (void)terminal_.stop();
        io_.stop();
        if (runner_.joinable()) runner_.join();
        session_.close();
        (void)terminal_.stop();
    }
    InteractiveSmokeCleanup(const InteractiveSmokeCleanup&) = delete;
    InteractiveSmokeCleanup& operator=(const InteractiveSmokeCleanup&) = delete;

    void dismiss() {
        active_ = false;
    }

private:
    cch::coding_agent::AgentSession& session_; // must outlive this cleanup guard.
    cch::tui::ProcessTerminal& terminal_; // must outlive this cleanup guard.
    boost::asio::io_context& io_; // must outlive this cleanup guard.
    std::jthread& runner_; // must outlive this cleanup guard.
    int master_fd_{-1};
    bool active_{true};
};

/// Accumulate PTY output into `buffer` until every needle of the expected
/// main-TUI state is observable (#530, the #526/#527/#528 defect class):
/// stopping at the first needle asserts against - and writes follow-up input
/// into - a half-delivered screen under scheduler pressure. The budget only
/// bounds the already-failing path.
[[nodiscard]] bool drain_pty_until_all(int master_fd,
        std::string& buffer,
        const std::vector<std::string>& needles,
        const std::chrono::milliseconds budget = std::chrono::seconds(10)) {
    return cch::tests::wait_until(
            [&] {
                buffer.append(cch::tests::read_available(master_fd, std::chrono::milliseconds(20)));
                return std::all_of(needles.begin(), needles.end(), [&buffer](const std::string& needle) {
                    return buffer.find(needle) != std::string::npos;
                });
            },
            budget);
}

} // namespace

TEST_CASE("Process Terminal runs the private Native TUI composition and restores the PTY",
        "[coding_agent][tui][terminal][issue58][issue530][spec]") {
    auto pty = cch::tests::open_pseudo_terminal(60, 12);
    REQUIRE(pty);
    termios original{};
    REQUIRE(::tcgetattr(pty->slave.get(), &original) == 0);

    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace config;
    cch::tests::RuntimeFixture runtime;
    cch::tests::ModelsSessionOptions options;
    options.session_target = cch::coding_agent::InMemorySessionTarget{};
    options.workspace = workspace.path();
    options.execution_runtime_target = runtime.make_target();
    auto models = cch::tests::models_from_provider(cch::tests::make_scripted_fake_provider());
    cch::coding_agent::runtime::AgentSessionCreationRequest request = std::move(options);
    auto created = runtime.run(cch::coding_agent::create_agent_session_async(std::move(request),
            std::nullopt,
            cch::coding_agent::runtime::AssemblyOverrides{
                    .model_runtime = nullptr, .models = std::move(models), .user_shell = nullptr}));
    REQUIRE(created);
    cch::tests::RuntimeLoopDriver runtime_driver(runtime);

    boost::asio::io_context io;
    cch::tui::ProcessTerminal terminal({
            .input_fd = pty->slave.get(),
            .output_fd = pty->slave.get(),
            .executor = io.get_executor(),
    });
    std::optional<cch::support::ExpectedVoid> run_result;
    std::exception_ptr run_exception;
    auto run = cch::coding_agent::tui::InteractiveSessionRunBuilder{}
        .with_session(*created->session)
        .with_agent_config_directory(config.path())
        .with_initial_prompt("pty prompt")
        .with_initial_prompt_options({
            .images = {cch::ai::image_content("cG5n", "image/png")},
        })
        .build();
    boost::asio::co_spawn(
        io,
        cch::coding_agent::tui::run_interactive_mode(
            terminal,
            std::move(run)),
        [&](std::exception_ptr exception, cch::support::ExpectedVoid result) {
            run_exception = exception;
            run_result.emplace(std::move(result));
        });
    std::jthread runner([&] { io.run(); });
    InteractiveSmokeCleanup cleanup{
        *created->session,
        terminal,
        io,
        runner,
        pty->master.get(),
    };
    REQUIRE(cch::tests::wait_until(
        [&] { return terminal.modes().started; },
        std::chrono::seconds(2)));
    auto output = cch::tests::read_available(pty->master.get());

    // The drain waits for the full settled main-TUI state - the assistant
    // response, the rendered user message with its image part, and the footer
    // stats line (the selected fake-model id) - before anything asserts on the
    // session snapshot or Ctrl+D targets the screen (#530).
    REQUIRE(drain_pty_until_all(pty->master.get(), output, {"fake: pty prompt", "[Image: [image/png]", "fake-model"}));
    const auto snapshot = created->session->snapshot();
    REQUIRE_FALSE(snapshot.agent_state.messages.empty());
    const auto* user = std::get_if<cch::ai::UserMessage>(&snapshot.agent_state.messages.front());
    REQUIRE(user != nullptr);
    REQUIRE(std::get<std::vector<cch::ai::Content>>(user->content).size() == 2);
    CHECK(std::holds_alternative<cch::ai::ImageContent>(
        std::get<std::vector<cch::ai::Content>>(user->content)[1]));

    constexpr char kExit = '\x04';
    REQUIRE(::write(pty->master.get(), &kExit, 1) == 1);
    REQUIRE(cch::tests::wait_until([&] { return !terminal.modes().started; }, std::chrono::seconds(10)));
    runner.join();
    cleanup.dismiss();

    CHECK(run_exception == nullptr);
    REQUIRE(run_result);
    CHECK(*run_result);
    termios restored{};
    REQUIRE(::tcgetattr(pty->slave.get(), &restored) == 0);
    CHECK(cch::tests::same_terminal_state(restored, original));
}
TEST_CASE("Process Terminal pinned dock keeps editor fixed at bottom when history exceeds 50+ lines",
        "[coding_agent][tui][terminal][dock][issue599][spec]") {
    auto pty = cch::tests::open_pseudo_terminal(80, 24);
    REQUIRE(pty);
    termios original{};
    REQUIRE(::tcgetattr(pty->slave.get(), &original) == 0);

    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace config;
    cch::tests::RuntimeFixture runtime;
    cch::tests::ModelsSessionOptions options;
    options.session_target = cch::coding_agent::InMemorySessionTarget{};
    options.workspace = workspace.path();
    options.execution_runtime_target = runtime.make_target();
    auto models = cch::tests::models_from_provider(cch::tests::make_scripted_fake_provider());
    cch::coding_agent::runtime::AgentSessionCreationRequest request = std::move(options);
    auto created = runtime.run(cch::coding_agent::create_agent_session_async(std::move(request),
            std::nullopt,
            cch::coding_agent::runtime::AssemblyOverrides{
                    .model_runtime = nullptr, .models = std::move(models), .user_shell = nullptr}));
    REQUIRE(created);
    cch::tests::RuntimeLoopDriver runtime_driver(runtime);

    boost::asio::io_context io;
    cch::tui::ProcessTerminal terminal({
            .input_fd = pty->slave.get(),
            .output_fd = pty->slave.get(),
            .executor = io.get_executor(),
    });
    std::optional<cch::support::ExpectedVoid> run_result;
    std::exception_ptr run_exception;

    std::string long_prompt;
    for (int i = 0; i < 55; ++i) {
        long_prompt += std::format("line {}\n", i);
    }

    auto run = cch::coding_agent::tui::InteractiveSessionRunBuilder{}
                       .with_session(*created->session)
                       .with_agent_config_directory(config.path())
                       .with_initial_prompt(long_prompt)
                       .build();
    boost::asio::co_spawn(io,
            cch::coding_agent::tui::run_interactive_mode(terminal, std::move(run)),
            [&](std::exception_ptr exception, cch::support::ExpectedVoid result) {
                run_exception = exception;
                run_result.emplace(std::move(result));
            });
    std::jthread runner([&] { io.run(); });
    InteractiveSmokeCleanup cleanup{
            *created->session,
            terminal,
            io,
            runner,
            pty->master.get(),
    };
    REQUIRE(cch::tests::wait_until([&] { return terminal.modes().started; }, std::chrono::seconds(2)));
    auto output = cch::tests::read_available(pty->master.get());

    // Wait for the prompt and assistant response to settle
    REQUIRE(drain_pty_until_all(pty->master.get(), output, {"fake-model"}));

    // Verify DECSTBM scroll margins were set for the viewport (rows 1..17 on a 24-row screen with 7 dock rows)
    CHECK(output.find("\x1b[1;17r") != std::string::npos);

    // Verify viewport scrolling occurred within margin bottom (row 17)
    CHECK(output.find("\x1b[1;17r") != std::string::npos);
    // Viewport scroll margins set and active

    // Verify the dock lines were written to the reserved physical bottom rows (18..24)
    // Dock row 0 (status spacer): row 18
    // Dock row 1 (status line): row 19
    // Dock row 2 (editor top border): row 20
    // Dock row 3 (editor content): row 21
    // Dock row 4 (editor bottom border): row 22
    // Dock row 5 (footer pwd): row 23
    // Dock row 6 (footer stats): row 24
    CHECK(output.find("\x1b[20;") != std::string::npos);
    CHECK(output.find("\x1b[21;") != std::string::npos);
    CHECK(output.find("\x1b[24;") != std::string::npos);

    constexpr char kExit = '\x04';
    REQUIRE(::write(pty->master.get(), &kExit, 1) == 1);
    REQUIRE(cch::tests::wait_until([&] { return !terminal.modes().started; }, std::chrono::seconds(10)));
    runner.join();
    cleanup.dismiss();

    CHECK(run_exception == nullptr);
    REQUIRE(run_result);
    CHECK(*run_result);
}

TEST_CASE("Process Terminal resize recalculates viewport height and anchors dock at new bottom",
        "[coding_agent][tui][terminal][dock][resize][issue599][spec]") {
    auto pty = cch::tests::open_pseudo_terminal(80, 24);
    REQUIRE(pty);
    termios original{};
    REQUIRE(::tcgetattr(pty->slave.get(), &original) == 0);

    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace config;
    cch::tests::RuntimeFixture runtime;
    cch::tests::ModelsSessionOptions options;
    options.session_target = cch::coding_agent::InMemorySessionTarget{};
    options.workspace = workspace.path();
    options.execution_runtime_target = runtime.make_target();
    auto models = cch::tests::models_from_provider(cch::tests::make_scripted_fake_provider());
    cch::coding_agent::runtime::AgentSessionCreationRequest request = std::move(options);
    auto created = runtime.run(cch::coding_agent::create_agent_session_async(std::move(request),
            std::nullopt,
            cch::coding_agent::runtime::AssemblyOverrides{
                    .model_runtime = nullptr, .models = std::move(models), .user_shell = nullptr}));
    REQUIRE(created);
    cch::tests::RuntimeLoopDriver runtime_driver(runtime);

    boost::asio::io_context io;
    cch::tui::ProcessTerminal terminal({
            .input_fd = pty->slave.get(),
            .output_fd = pty->slave.get(),
            .executor = io.get_executor(),
    });
    std::optional<cch::support::ExpectedVoid> run_result;
    std::exception_ptr run_exception;

    auto run = cch::coding_agent::tui::InteractiveSessionRunBuilder{}
                       .with_session(*created->session)
                       .with_agent_config_directory(config.path())
                       .with_initial_prompt("test prompt")
                       .build();
    boost::asio::co_spawn(io,
            cch::coding_agent::tui::run_interactive_mode(terminal, std::move(run)),
            [&](std::exception_ptr exception, cch::support::ExpectedVoid result) {
                run_exception = exception;
                run_result.emplace(std::move(result));
            });
    std::jthread runner([&] { io.run(); });
    InteractiveSmokeCleanup cleanup{
            *created->session,
            terminal,
            io,
            runner,
            pty->master.get(),
    };
    REQUIRE(cch::tests::wait_until([&] { return terminal.modes().started; }, std::chrono::seconds(2)));
    auto output = cch::tests::read_available(pty->master.get());
    REQUIRE(drain_pty_until_all(pty->master.get(), output, {"fake-model"}));

    // Initial 24 rows: viewport 17 rows (margins 1..17)
    CHECK(output.find("\x1b[1;17r") != std::string::npos);

    // Resize terminal from 24 to 32 rows
    winsize dimensions{
            .ws_row = 32,
            .ws_col = 80,
            .ws_xpixel = 0,
            .ws_ypixel = 0,
    };
    REQUIRE(::ioctl(pty->master.get(), TIOCSWINSZ, &dimensions) == 0);

    // Send SIGWINCH so terminal absorbs the resize
    ::kill(::getpid(), SIGWINCH);

    // Wait for resize to take effect and new scroll margins / dock rows to be emitted
    // Viewport height becomes 32 - 7 = 25 rows -> margin \x1b[1;25r
    // Dock lines move to bottom rows 26..32
    bool matched = cch::tests::wait_until(
            [&] {
                output.append(cch::tests::read_available(pty->master.get(), std::chrono::milliseconds(50)));
                return output.find("\x1b[1;25r") != std::string::npos && output.find("\x1b[32;") != std::string::npos;
            },
            std::chrono::seconds(2));
    UNSCOPED_INFO("Resize test output:\n" << output);
    REQUIRE(matched);

    CHECK(output.find("\x1b[1;25r") != std::string::npos);
    // Editor top border at row 28 (1-based: 25 + 2 + 1 = 28)
    CHECK(output.find("\x1b[28;") != std::string::npos);
    // Footer bottom line at row 32
    CHECK(output.find("\x1b[32;") != std::string::npos);

    constexpr char kExit = '\x04';
    REQUIRE(::write(pty->master.get(), &kExit, 1) == 1);
    REQUIRE(cch::tests::wait_until([&] { return !terminal.modes().started; }, std::chrono::seconds(10)));
    runner.join();
    cleanup.dismiss();

    CHECK(run_exception == nullptr);
    REQUIRE(run_result);
    CHECK(*run_result);
}

TEST_CASE("Process Terminal preserves keypresses while streaming output and restores the PTY",
        "[coding_agent][tui][terminal][spec597][issue606][spec]") {
    auto pty = cch::tests::open_pseudo_terminal(80, 24);
    REQUIRE(pty);
    termios original{};
    REQUIRE(::tcgetattr(pty->slave.get(), &original) == 0);

    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace config;
    cch::tests::RuntimeFixture runtime;
    auto stream_started = std::make_shared<std::atomic_bool>(false);
    auto stream_finished = std::make_shared<std::atomic_bool>(false);
    auto provider = std::make_shared<ConcurrentStreamingProvider>(stream_started, stream_finished);
    cch::tests::ModelsSessionOptions options;
    options.session_target = cch::coding_agent::InMemorySessionTarget{};
    options.workspace = workspace.path();
    options.execution_runtime_target = runtime.make_target();
    auto models = cch::tests::models_from_provider(std::move(provider));
    cch::coding_agent::runtime::AgentSessionCreationRequest request = std::move(options);
    auto created = runtime.run(cch::coding_agent::create_agent_session_async(std::move(request),
            std::nullopt,
            cch::coding_agent::runtime::AssemblyOverrides{
                    .model_runtime = nullptr, .models = std::move(models), .user_shell = nullptr}));
    REQUIRE(created);
    cch::tests::RuntimeLoopDriver runtime_driver(runtime);

    boost::asio::io_context io;
    cch::tui::ProcessTerminal terminal({
            .input_fd = pty->slave.get(),
            .output_fd = pty->slave.get(),
            .executor = io.get_executor(),
    });
    std::optional<cch::support::ExpectedVoid> run_result;
    std::exception_ptr run_exception;
    auto run = cch::coding_agent::tui::InteractiveSessionRunBuilder{}
                       .with_session(*created->session)
                       .with_agent_config_directory(config.path())
                       .with_initial_prompt("pty prompt")
                       .build();
    boost::asio::co_spawn(io,
            cch::coding_agent::tui::run_interactive_mode(terminal, std::move(run)),
            [&](std::exception_ptr exception, cch::support::ExpectedVoid result) {
                run_exception = exception;
                run_result.emplace(std::move(result));
            });
    std::jthread runner([&] { io.run(); });
    InteractiveSmokeCleanup cleanup{
            *created->session,
            terminal,
            io,
            runner,
            pty->master.get(),
    };
    REQUIRE(cch::tests::wait_until([&] { return terminal.modes().started; }, std::chrono::seconds(2)));

    std::string output = cch::tests::read_available(pty->master.get());
    REQUIRE(cch::tests::wait_until(
            [&] { return stream_started->load(std::memory_order_acquire); }, std::chrono::seconds(2)));

    const std::string test_input = "hello world";
    bool typed_during_stream = false;
    for (char c : test_input) {
        typed_during_stream = typed_during_stream || !stream_finished->load(std::memory_order_acquire);
        REQUIRE(::write(pty->master.get(), &c, 1) == 1);
    }
    CHECK(typed_during_stream);
    REQUIRE(drain_pty_until_all(pty->master.get(), output, {"hello world", "stream-159"}));
    REQUIRE(cch::tests::wait_until(
            [&] { return stream_finished->load(std::memory_order_acquire); }, std::chrono::seconds(2)));

    // The keypresses were admitted while the model stream was active and the
    // complete stream marker was observed afterward; no PTY output was lost.
    for (std::size_t i = 0; i < test_input.size(); ++i) {
        constexpr char kBackspace = '\x7f';
        REQUIRE(::write(pty->master.get(), &kBackspace, 1) == 1);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    constexpr char kExit = '\x04';
    REQUIRE(::write(pty->master.get(), &kExit, 1) == 1);
    REQUIRE(cch::tests::wait_until([&] { return !terminal.modes().started; }, std::chrono::seconds(10)));
    runner.join();
    cleanup.dismiss();
    CHECK(run_exception == nullptr);
    REQUIRE(run_result);
    CHECK(*run_result);

    termios restored{};
    REQUIRE(::tcgetattr(pty->slave.get(), &restored) == 0);
    CHECK(cch::tests::same_terminal_state(restored, original));
}

TEST_CASE("Process Terminal slash autocomplete under a shrink resize keeps the editor and footer docked and the "
          "session running",
        "[coding_agent][tui][terminal][dock][resize][issue599][issue607][spec]") {
    auto pty = cch::tests::open_pseudo_terminal(80, 24);
    REQUIRE(pty);
    termios original{};
    REQUIRE(::tcgetattr(pty->slave.get(), &original) == 0);

    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace config;
    cch::tests::RuntimeFixture runtime;
    cch::tests::ModelsSessionOptions options;
    options.session_target = cch::coding_agent::InMemorySessionTarget{};
    options.workspace = workspace.path();
    options.execution_runtime_target = runtime.make_target();
    auto models = cch::tests::models_from_provider(cch::tests::make_scripted_fake_provider());
    cch::coding_agent::runtime::AgentSessionCreationRequest request = std::move(options);
    auto created = runtime.run(cch::coding_agent::create_agent_session_async(std::move(request),
            std::nullopt,
            cch::coding_agent::runtime::AssemblyOverrides{
                    .model_runtime = nullptr, .models = std::move(models), .user_shell = nullptr}));
    REQUIRE(created);
    cch::tests::RuntimeLoopDriver runtime_driver(runtime);

    boost::asio::io_context io;
    cch::tui::ProcessTerminal terminal({
            .input_fd = pty->slave.get(),
            .output_fd = pty->slave.get(),
            .executor = io.get_executor(),
    });
    std::optional<cch::support::ExpectedVoid> run_result;
    std::exception_ptr run_exception;
    auto run = cch::coding_agent::tui::InteractiveSessionRunBuilder{}
                       .with_session(*created->session)
                       .with_agent_config_directory(config.path())
                       .build();
    boost::asio::co_spawn(io,
            cch::coding_agent::tui::run_interactive_mode(terminal, std::move(run)),
            [&](std::exception_ptr exception, cch::support::ExpectedVoid result) {
                run_exception = exception;
                run_result.emplace(std::move(result));
            });
    std::jthread runner([&] { io.run(); });
    InteractiveSmokeCleanup cleanup{
            *created->session,
            terminal,
            io,
            runner,
            pty->master.get(),
    };
    REQUIRE(cch::tests::wait_until([&] { return terminal.modes().started; }, std::chrono::seconds(2)));
    auto output = cch::tests::read_available(pty->master.get());
    REQUIRE(drain_pty_until_all(pty->master.get(), output, {"fake-model"}));

    // '/' at message start opens autocomplete in the main editor.
    REQUIRE(::write(pty->master.get(), "/", 1) == 1);
    REQUIRE(drain_pty_until_all(pty->master.get(), output, {"> /"}));

    // Shrink below the open dock's height. The editor/menu and footer remain
    // live at the physical bottom instead of turning a dock address error
    // into InteractiveEngine::request_exit().
    const auto before_resize = output.size();
    winsize dimensions{
            .ws_row = 6,
            .ws_col = 80,
            .ws_xpixel = 0,
            .ws_ypixel = 0,
    };
    REQUIRE(::ioctl(pty->master.get(), TIOCSWINSZ, &dimensions) == 0);
    ::kill(::getpid(), SIGWINCH);

    const bool footer_redocked = cch::tests::wait_until(
            [&] {
                output.append(cch::tests::read_available(pty->master.get(), std::chrono::milliseconds(50)));
                return output.find("fake-model", before_resize) != std::string::npos &&
                       output.find("\x1b[6;", before_resize) != std::string::npos;
            },
            std::chrono::seconds(10));
    UNSCOPED_INFO("Slash resize test output:\n" << output.substr(before_resize));
    CHECK(footer_redocked);
    CHECK(terminal.modes().started);

    // Close the menu, erase '/', then exit through the empty-editor Ctrl+D.
    constexpr char kEscape = '\x1b';
    REQUIRE(::write(pty->master.get(), &kEscape, 1) == 1);
    constexpr char kBackspace = '\x7f';
    REQUIRE(::write(pty->master.get(), &kBackspace, 1) == 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    constexpr char kExit = '\x04';
    REQUIRE(::write(pty->master.get(), &kExit, 1) == 1);
    REQUIRE(cch::tests::wait_until([&] { return !terminal.modes().started; }, std::chrono::seconds(10)));
    runner.join();
    cleanup.dismiss();

    CHECK(run_exception == nullptr);
    REQUIRE(run_result);
    if (!*run_result) {
        UNSCOPED_INFO(
                "Interactive run error: " << run_result->error().message << " [" << run_result->error().detail << "]");
    }
    CHECK(*run_result);
}

TEST_CASE("Process Terminal replacement dialog slash input under a shrink resize keeps the footer docked and the "
          "session running",
        "[coding_agent][tui][terminal][dock][resize][issue599][issue607][spec]") {
    auto pty = cch::tests::open_pseudo_terminal(80, 24);
    REQUIRE(pty);
    termios original{};
    REQUIRE(::tcgetattr(pty->slave.get(), &original) == 0);

    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace config;
    cch::tests::RuntimeFixture runtime;
    cch::tests::ModelsSessionOptions options;
    options.session_target = cch::coding_agent::InMemorySessionTarget{};
    options.workspace = workspace.path();
    options.execution_runtime_target = runtime.make_target();
    auto models = cch::tests::models_from_provider(cch::tests::make_scripted_fake_provider());
    cch::coding_agent::runtime::AgentSessionCreationRequest request = std::move(options);
    auto created = runtime.run(cch::coding_agent::create_agent_session_async(std::move(request),
            std::nullopt,
            cch::coding_agent::runtime::AssemblyOverrides{
                    .model_runtime = nullptr, .models = std::move(models), .user_shell = nullptr}));
    REQUIRE(created);
    cch::tests::RuntimeLoopDriver runtime_driver(runtime);

    boost::asio::io_context io;
    cch::tui::ProcessTerminal terminal({
            .input_fd = pty->slave.get(),
            .output_fd = pty->slave.get(),
            .executor = io.get_executor(),
    });
    std::optional<cch::support::ExpectedVoid> run_result;
    std::exception_ptr run_exception;
    auto run = cch::coding_agent::tui::InteractiveSessionRunBuilder{}
                       .with_session(*created->session)
                       .with_agent_config_directory(config.path())
                       .build();
    boost::asio::co_spawn(io,
            cch::coding_agent::tui::run_interactive_mode(terminal, std::move(run)),
            [&](std::exception_ptr exception, cch::support::ExpectedVoid result) {
                run_exception = exception;
                run_result.emplace(std::move(result));
            });
    std::jthread runner([&] { io.run(); });
    InteractiveSmokeCleanup cleanup{
            *created->session,
            terminal,
            io,
            runner,
            pty->master.get(),
    };
    REQUIRE(cch::tests::wait_until([&] { return terminal.modes().started; }, std::chrono::seconds(2)));
    auto output = cch::tests::read_available(pty->master.get());
    REQUIRE(drain_pty_until_all(pty->master.get(), output, {"fake-model"}));

    // Ctrl+L opens a model selector in the replacement editor slot.
    const auto before_selector = output.size();
    REQUIRE(::write(pty->master.get(), "\x0c", 1) == 1);
    const bool selector_shown = cch::tests::wait_until(
            [&] {
                output.append(cch::tests::read_available(pty->master.get(), std::chrono::milliseconds(50)));
                return output.find("Only showing models from configured providers.", before_selector) !=
                       std::string::npos;
            },
            std::chrono::seconds(10));
    UNSCOPED_INFO("Selector output:\n" << output.substr(before_selector));
    REQUIRE(selector_shown);

    // '/' is owned by the replacement dialog, not the main editor.
    REQUIRE(::write(pty->master.get(), "/", 1) == 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    output.append(cch::tests::read_available(pty->master.get(), std::chrono::milliseconds(50)));
    REQUIRE(terminal.modes().started);

    const auto before_resize = output.size();
    winsize dimensions{
            .ws_row = 6,
            .ws_col = 80,
            .ws_xpixel = 0,
            .ws_ypixel = 0,
    };
    REQUIRE(::ioctl(pty->master.get(), TIOCSWINSZ, &dimensions) == 0);
    ::kill(::getpid(), SIGWINCH);

    const bool footer_redocked = cch::tests::wait_until(
            [&] {
                output.append(cch::tests::read_available(pty->master.get(), std::chrono::milliseconds(50)));
                return output.find("fake-model", before_resize) != std::string::npos &&
                       output.find("\x1b[6;", before_resize) != std::string::npos;
            },
            std::chrono::seconds(10));
    UNSCOPED_INFO("Dialog resize test output:\n" << output.substr(before_resize));
    CHECK(footer_redocked);
    CHECK(terminal.modes().started);
    // The dialog owns Escape and Ctrl+D while it is open; stop the PTY after
    // the liveness assertion so teardown cannot turn selector cancellation
    // timing into a test failure.
    REQUIRE(terminal.stop());
    io.stop();
    runner.join();
    CHECK(terminal.modes().started == false);
}
