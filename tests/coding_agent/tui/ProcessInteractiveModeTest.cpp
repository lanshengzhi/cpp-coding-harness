#include "coding_agent/tui/InteractiveMode.hpp"
#include "coding_agent/tui/InteractiveSessionRun.hpp"
#include "support/ModelsFixture.hpp"
#include "support/PseudoTerminal.hpp"
#include "support/TempWorkspace.hpp"

#include "coding_agent/AgentSession.hpp"
#include <cch/tui/ProcessTerminal.hpp>

#include <cch/support/Error.hpp>
#include <catch2/catch_test_macros.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>

#include <algorithm>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

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
        "[coding_agent][tui][terminal][issue58][issue530]") {
    auto pty = cch::tests::open_pseudo_terminal(60, 12);
    REQUIRE(pty);
    termios original{};
    REQUIRE(::tcgetattr(pty->slave.get(), &original) == 0);

    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace config;
    cch::tests::ModelsSessionOptions options;
    options.session_target = cch::coding_agent::InMemorySessionTarget{};
    options.workspace = workspace.path();
    options.models = cch::tests::models_from_provider(cch::tests::make_scripted_fake_provider());
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
