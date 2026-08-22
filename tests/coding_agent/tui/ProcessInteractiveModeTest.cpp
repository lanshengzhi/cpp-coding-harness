#include "coding_agent/tui/InteractiveMode.hpp"
#include "coding_agent/tui/InteractiveSessionRun.hpp"
#include "support/ModelsFixture.hpp"
#include "support/PseudoTerminal.hpp"
#include "support/TempWorkspace.hpp"

#include "coding_agent/AgentSession.hpp"
#include <cch/tui/ProcessTerminal.hpp>

#include "ai/providers/FakeProvider.hpp"

#include <cch/support/Error.hpp>
#include <catch2/catch_test_macros.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace {

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

} // namespace

TEST_CASE(
    "Process Terminal runs the private Native TUI composition and restores the PTY",
    "[coding_agent][tui][terminal][issue58]") {
    auto pty = cch::tests::open_pseudo_terminal(60, 12);
    REQUIRE(pty);
    termios original{};
    REQUIRE(::tcgetattr(pty->slave.get(), &original) == 0);

    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace config;
    cch::tests::ModelsSessionOptions options;
    options.session_target = cch::coding_agent::InMemorySessionTarget{};
    options.workspace = workspace.path();
    options.models = cch::tests::models_from_provider(cch::ai::providers::make_scripted_fake_provider());
    auto created = cch::coding_agent::create_agent_session(std::move(options));
    REQUIRE(created);

    cch::tui::ProcessTerminal terminal({
        .input_fd = pty->slave.get(),
        .output_fd = pty->slave.get(),
    });
    boost::asio::io_context io;
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

    REQUIRE(cch::tests::wait_until(
        [&] {
            output.append(cch::tests::read_available(
                pty->master.get(),
                std::chrono::milliseconds(20)));
            return output.find("fake: pty prompt") != std::string::npos;
        },
        std::chrono::seconds(2)));
    const auto snapshot = created->session->snapshot();
    REQUIRE_FALSE(snapshot.agent_state.messages.empty());
    const auto* user = std::get_if<cch::ai::UserMessage>(&snapshot.agent_state.messages.front());
    REQUIRE(user != nullptr);
    REQUIRE(std::get<std::vector<cch::ai::Content>>(user->content).size() == 2);
    CHECK(std::holds_alternative<cch::ai::ImageContent>(
        std::get<std::vector<cch::ai::Content>>(user->content)[1]));

    constexpr char kExit = '\x04';
    REQUIRE(::write(pty->master.get(), &kExit, 1) == 1);
    REQUIRE(cch::tests::wait_until(
        [&] { return !terminal.modes().started; },
        std::chrono::seconds(2)));
    runner.join();
    cleanup.dismiss();

    CHECK(run_exception == nullptr);
    REQUIRE(run_result);
    CHECK(*run_result);
    termios restored{};
    REQUIRE(::tcgetattr(pty->slave.get(), &restored) == 0);
    CHECK(cch::tests::same_terminal_state(restored, original));
}

