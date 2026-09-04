#include <cch/tui/ProcessTerminal.hpp>
#include <cch/tui/Tui.hpp>

#include "support/ImageEnvironmentGuard.hpp"
#include "support/UniqueFd.hpp"
#include "support/PseudoTerminal.hpp"

#include <cch/support/Error.hpp>
#include <catch2/catch_test_macros.hpp>

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

namespace {

struct ExpectedUnwind {};

class ScopedEnvironmentVariable final {
public:
    explicit ScopedEnvironmentVariable(std::string name)
        : name_(std::move(name)) {
        const auto* value = std::getenv(name_.c_str());
        if (value != nullptr) original_ = value;
    }
    ScopedEnvironmentVariable(ScopedEnvironmentVariable&&) = delete;
    ScopedEnvironmentVariable& operator=(ScopedEnvironmentVariable&&) = delete;
    ~ScopedEnvironmentVariable() {
        if (original_) (void)::setenv(name_.c_str(), original_->c_str(), 1);
        else (void)::unsetenv(name_.c_str());
    }

    ScopedEnvironmentVariable(const ScopedEnvironmentVariable&) = delete;
    ScopedEnvironmentVariable& operator=(const ScopedEnvironmentVariable&) = delete;

    void set(std::string_view value) {
        (void)::setenv(name_.c_str(), std::string(value).c_str(), 1);
    }

    void unset() {
        (void)::unsetenv(name_.c_str());
    }

private:
    std::string name_;
    std::optional<std::string> original_;
};

class MinimalShell final
    : public cch::tui::Component,
      public cch::tui::InputHandler,
      public cch::tui::Focusable {
public:
    [[nodiscard]] cch::support::Expected<cch::tui::RenderResult> render(std::size_t) override {
        return cch::tui::RenderResult{.lines = {"shell"}};
    }

    void invalidate() override {
        ++invalidations;
    }

    cch::tui::InputAdmissionOutcome handle_input(const cch::tui::InputEventVariant& event) override {
        const auto* key = std::get_if<cch::tui::KeyEvent>(&event);
        if (key == nullptr || key->key != "q") return cch::tui::InputAdmissionOutcome::Unhandled;
        ++inputs;
        if (tui != nullptr) stop_succeeded = tui->stop().has_value();
        return cch::tui::InputAdmissionOutcome::Consumed;
    }

    void set_focused(bool focused) override {
        focused_ = focused;
    }

    [[nodiscard]] bool focused() const override {
        return focused_;
    }

    cch::tui::Tui* tui{nullptr}; // must outlive this attached Component.
    std::atomic<std::size_t> inputs{0};
    std::atomic<std::size_t> invalidations{0};
    std::atomic<bool> stop_succeeded{false};

private:
    bool focused_{false};
};

[[nodiscard]] bool answer_appearance_query(
    int descriptor,
    std::string_view response) {
    constexpr std::string_view kColorSchemeQuery = "\x1b[?996n";
    constexpr std::string_view kBackgroundQuery = "\x1b]11;?\x07";
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    std::string output;
    std::array<char, 4096> buffer{};
    while (std::chrono::steady_clock::now() < deadline) {
        pollfd item{.fd = descriptor, .events = POLLIN, .revents = 0};
        const auto ready = ::poll(&item, 1, 10);
        if (ready < 0) return false;
        if (ready == 0 || (item.revents & POLLIN) == 0) continue;
        const auto count = ::read(descriptor, buffer.data(), buffer.size());
        if (count <= 0) return false;
        output.append(buffer.data(), static_cast<std::size_t>(count));
        if (output.find(kColorSchemeQuery) == std::string::npos ||
            output.find(kBackgroundQuery) == std::string::npos) {
            continue;
        }
        return ::write(descriptor, response.data(), response.size()) ==
            static_cast<ssize_t>(response.size());
    }
    return false;
}

[[nodiscard]] bool answer_cursor_position_query(
    int descriptor,
    std::string_view response) {
    constexpr std::string_view kCursorPositionQuery = "\x1b[6n";
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    std::string output;
    std::array<char, 4096> buffer{};
    while (std::chrono::steady_clock::now() < deadline) {
        pollfd item{.fd = descriptor, .events = POLLIN, .revents = 0};
        const auto ready = ::poll(&item, 1, 10);
        if (ready < 0) return false;
        if (ready == 0 || (item.revents & POLLIN) == 0) continue;
        const auto count = ::read(descriptor, buffer.data(), buffer.size());
        if (count <= 0) return false;
        output.append(buffer.data(), static_cast<std::size_t>(count));
        if (output.find(kCursorPositionQuery) == std::string::npos) {
            continue;
        }
        return ::write(descriptor, response.data(), response.size()) ==
            static_cast<ssize_t>(response.size());
    }
    return false;
}

} // namespace

TEST_CASE("Process Terminal rejects non-TTY descriptors before changing modes", "[tui][terminal][issue54]") {
    std::array<int, 2> raw_descriptors{};
    REQUIRE(::pipe(raw_descriptors.data()) == 0);
    cch::support::UniqueFd input(raw_descriptors[0]);
    cch::support::UniqueFd output(raw_descriptors[1]);

    cch::tui::ProcessTerminal terminal({
        .input_fd = input.get(),
        .output_fd = output.get(),
    });
    const auto result = terminal.start(
        [](std::string) -> cch::support::ExpectedVoid { return {}; },
        [](cch::tui::TerminalDimensions) -> cch::support::ExpectedVoid { return {}; });

    REQUIRE_FALSE(result);
    CHECK(result.error().code == cch::support::ErrorCode::Validation);
    CHECK(result.error().message == "Process Terminal requires TTY input and output descriptors");
    CHECK(result.error().detail.size() < 256);
    CHECK(terminal.modes() == cch::tui::TerminalModeState{});
}

TEST_CASE("Process Terminal restores raw paste cursor and pending render modes", "[tui][terminal][issue54]") {
    auto pty = cch::tests::open_pseudo_terminal();
    REQUIRE(pty);
    termios original{};
    REQUIRE(::tcgetattr(pty->slave.get(), &original) == 0);

    // Scrub the image-capability env surface so the conservative unknown-
    // terminal default is pinned regardless of the runner's own terminal.
    cch::tests::ImageEnvironmentGuard environment;

    cch::tui::ProcessTerminal terminal({
        .input_fd = pty->slave.get(),
        .output_fd = pty->slave.get(),
    });
    REQUIRE(terminal.start(
        [](std::string) -> cch::support::ExpectedVoid { return {}; },
        [](cch::tui::TerminalDimensions) -> cch::support::ExpectedVoid { return {}; }));

    termios acquired{};
    REQUIRE(::tcgetattr(pty->slave.get(), &acquired) == 0);
    CHECK((acquired.c_lflag & (ICANON | ECHO)) == 0);
    CHECK(terminal.modes().started);
    CHECK(terminal.modes().raw_input);
    CHECK(terminal.modes().bracketed_paste);
    CHECK(terminal.modes().cursor_visible);
    const cch::tui::TerminalDimensions expected_dimensions{.columns = 80, .rows = 24};
    CHECK(terminal.dimensions() == expected_dimensions);
    CHECK(terminal.capabilities().inline_images == cch::tui::InlineImageProtocol::None);
    CHECK(cch::tests::read_available(pty->master.get()).find("\x1b[?2004h") != std::string::npos);

    REQUIRE(terminal.set_cursor_visible(false));
    REQUIRE(terminal.begin_synchronized_update());
    REQUIRE(terminal.stop());
    REQUIRE(terminal.stop());

    termios restored{};
    REQUIRE(::tcgetattr(pty->slave.get(), &restored) == 0);
    CHECK(cch::tests::same_terminal_state(restored, original));
    CHECK(terminal.modes() == cch::tui::TerminalModeState{});
    const auto restoration = cch::tests::read_available(pty->master.get());
    CHECK(restoration.find("\x1b[?2026l") != std::string::npos);
    CHECK(restoration.find("\x1b[?25h") != std::string::npos);
    CHECK(restoration.find("\x1b[?2004l") != std::string::npos);
}

TEST_CASE("Process Terminal reports synchronized output conservatively", "[tui][terminal][issue54]") {
    auto pty = cch::tests::open_pseudo_terminal();
    REQUIRE(pty);
    ScopedEnvironmentVariable terminal_environment("TERM");
    ScopedEnvironmentVariable program_environment("TERM_PROGRAM");
    program_environment.unset();

    terminal_environment.set("xterm-unknown");
    cch::tui::ProcessTerminal generic_terminal({
        .input_fd = pty->slave.get(),
        .output_fd = pty->slave.get(),
    });
    REQUIRE(generic_terminal.start(
        [](std::string) -> cch::support::ExpectedVoid { return {}; },
        [](cch::tui::TerminalDimensions) -> cch::support::ExpectedVoid { return {}; }));
    CHECK_FALSE(generic_terminal.capabilities().synchronized_output);
    REQUIRE(generic_terminal.stop());
    (void)cch::tests::read_available(pty->master.get());

    terminal_environment.set("football");
    REQUIRE(generic_terminal.start(
        [](std::string) -> cch::support::ExpectedVoid { return {}; },
        [](cch::tui::TerminalDimensions) -> cch::support::ExpectedVoid { return {}; }));
    CHECK_FALSE(generic_terminal.capabilities().synchronized_output);
    REQUIRE(generic_terminal.stop());
    (void)cch::tests::read_available(pty->master.get());

    terminal_environment.set("xterm-unknown");
    program_environment.set("WezTerm");
    REQUIRE(generic_terminal.start(
        [](std::string) -> cch::support::ExpectedVoid { return {}; },
        [](cch::tui::TerminalDimensions) -> cch::support::ExpectedVoid { return {}; }));
    CHECK(generic_terminal.capabilities().synchronized_output);
    REQUIRE(generic_terminal.stop());
    (void)cch::tests::read_available(pty->master.get());

    terminal_environment.set("xterm-kitty");
    program_environment.unset();
    REQUIRE(generic_terminal.start(
        [](std::string) -> cch::support::ExpectedVoid { return {}; },
        [](cch::tui::TerminalDimensions) -> cch::support::ExpectedVoid { return {}; }));
    CHECK(generic_terminal.capabilities().synchronized_output);
    REQUIRE(generic_terminal.stop());
}

TEST_CASE("Process Terminal delivers pseudo-terminal input and resize", "[tui][terminal][issue54]") {
    auto pty = cch::tests::open_pseudo_terminal();
    REQUIRE(pty);
    std::mutex events_mutex;
    std::vector<std::string> inputs;
    std::vector<cch::tui::TerminalDimensions> resizes;

    cch::tui::ProcessTerminal terminal({
        .input_fd = pty->slave.get(),
        .output_fd = pty->slave.get(),
    });
    REQUIRE(terminal.start(
        [&](std::string input) -> cch::support::ExpectedVoid {
            std::lock_guard lock(events_mutex);
            inputs.push_back(std::move(input));
            return {};
        },
        [&](cch::tui::TerminalDimensions dimensions) -> cch::support::ExpectedVoid {
            std::lock_guard lock(events_mutex);
            resizes.push_back(dimensions);
            return {};
        }));
    const auto startup_output = cch::tests::read_available(pty->master.get());
    CHECK(startup_output.find("\x1b[>7u\x1b[?u\x1b[c") != std::string::npos);

    constexpr std::string_view kKittyResponse = "\x1b[?7u";
    REQUIRE(::write(pty->master.get(), kKittyResponse.data(), kKittyResponse.size()) ==
        static_cast<ssize_t>(kKittyResponse.size()));
    REQUIRE(cch::tests::wait_until([&] {
        return terminal.capabilities().keyboard_protocol == cch::tui::KeyboardProtocol::Kitty;
    }));
    REQUIRE(::write(pty->master.get(), "q", 1) == 1);
    winsize resized{
        .ws_row = 30,
        .ws_col = 100,
        .ws_xpixel = 0,
        .ws_ypixel = 0,
    };
    REQUIRE(::ioctl(pty->master.get(), TIOCSWINSZ, &resized) == 0);

    REQUIRE(cch::tests::wait_until([&] {
        std::lock_guard lock(events_mutex);
        return !inputs.empty() && !resizes.empty();
    }));
    {
        std::lock_guard lock(events_mutex);
        CHECK(inputs.front() == "q");
        const cch::tui::TerminalDimensions expected_resize{.columns = 100, .rows = 30};
        CHECK(resizes.back() == expected_resize);
    }
    const cch::tui::TerminalDimensions expected_dimensions{.columns = 100, .rows = 30};
    CHECK(terminal.dimensions() == expected_dimensions);
    REQUIRE(terminal.stop());
}

TEST_CASE(
    "Process Terminal detects a resize with no input activity",
    "[tui][terminal][issue462]") {
    auto pty = cch::tests::open_pseudo_terminal();
    REQUIRE(pty);
    std::vector<cch::tui::TerminalDimensions> resizes;
    cch::tui::ProcessTerminal terminal({
        .input_fd = pty->slave.get(),
        .output_fd = pty->slave.get(),
    });
    REQUIRE(terminal.start(
        [](std::string) -> cch::support::ExpectedVoid { return {}; },
        [&](cch::tui::TerminalDimensions dimensions) -> cch::support::ExpectedVoid {
            resizes.push_back(dimensions);
            return {};
        }));
    (void)cch::tests::read_available(pty->master.get());

    // No input is sent: the readiness-driven worker is parked and only the
    // low-rate resize watchdog is armed. The TIOCGWINSZ change must still be
    // detected (within the watchdog window) so resize handling stays correct
    // without any input activity (issue #462).
    winsize resized{
        .ws_row = 31,
        .ws_col = 101,
        .ws_xpixel = 0,
        .ws_ypixel = 0,
    };
    REQUIRE(::ioctl(pty->master.get(), TIOCSWINSZ, &resized) == 0);
    REQUIRE(cch::tests::wait_until(
        [&] { return !resizes.empty(); },
        std::chrono::seconds(2)));
    CHECK(resizes.back() == (cch::tui::TerminalDimensions{.columns = 101, .rows = 31}));
    CHECK(terminal.dimensions() == (cch::tui::TerminalDimensions{.columns = 101, .rows = 31}));
    REQUIRE(terminal.stop());
}

TEST_CASE("Concurrent external and sink stops restore without deadlock", "[tui][terminal][issue54]") {
    auto pty = cch::tests::open_pseudo_terminal();
    REQUIRE(pty);
    termios original{};
    REQUIRE(::tcgetattr(pty->slave.get(), &original) == 0);
    std::atomic<bool> callback_started{false};
    std::atomic<bool> external_stop_started{false};
    std::atomic<bool> callback_stop_succeeded{false};
    cch::support::ExpectedVoid external_result;

    cch::tui::ProcessTerminal terminal({
        .input_fd = pty->slave.get(),
        .output_fd = pty->slave.get(),
    });
    REQUIRE(terminal.start(
        [&](std::string) -> cch::support::ExpectedVoid {
            callback_started = true;
            while (!external_stop_started.load()) std::this_thread::yield();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            callback_stop_succeeded = terminal.stop().has_value();
            return {};
        },
        [](cch::tui::TerminalDimensions) -> cch::support::ExpectedVoid { return {}; }));
    (void)cch::tests::read_available(pty->master.get());
    REQUIRE(::write(pty->master.get(), "q", 1) == 1);
    REQUIRE(cch::tests::wait_until([&] { return callback_started.load(); }));

    std::jthread external_stop([&] {
        external_stop_started = true;
        external_result = terminal.stop();
    });
    external_stop.join();

    CHECK(callback_stop_succeeded.load());
    REQUIRE(external_result);
    termios restored{};
    REQUIRE(::tcgetattr(pty->slave.get(), &restored) == 0);
    CHECK(cch::tests::same_terminal_state(restored, original));
    REQUIRE(terminal.stop());
}

#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
// The staged build's defensive sink boundary still isolates a throwing
// interaction callback and reports it with the restoration failure; the
// no-exception build enforces non-throwing callbacks by construction.
TEST_CASE("Process Terminal reports callback and restoration failures together", "[tui][terminal][issue54]") {
    auto pty = cch::tests::open_pseudo_terminal();
    REQUIRE(pty);
    cch::support::UniqueFd output(::dup(pty->slave.get()));
    REQUIRE(output);
    const auto output_descriptor = output.get();
    std::atomic<bool> callback_failed{false};

    cch::tui::ProcessTerminal terminal({
        .input_fd = pty->slave.get(),
        .output_fd = output_descriptor,
    });
    REQUIRE(terminal.start(
        [&](std::string) -> cch::support::ExpectedVoid {
            callback_failed = true;
            throw ExpectedUnwind{};
            return {};
        },
        [](cch::tui::TerminalDimensions) -> cch::support::ExpectedVoid { return {}; }));
    (void)cch::tests::read_available(pty->master.get());
    REQUIRE(::write(pty->master.get(), "q", 1) == 1);
    REQUIRE(cch::tests::wait_until([&] { return callback_failed.load(); }));
    REQUIRE(output.close() == 0);

    const auto failed_stop = terminal.stop();
    REQUIRE_FALSE(failed_stop);
    CHECK(failed_stop.error().message == "Process Terminal restoration encountered multiple failures");
    CHECK(failed_stop.error().detail.find("Process Terminal input sink failed") != std::string::npos);
    CHECK(failed_stop.error().detail.find("could not write terminal output") != std::string::npos);

    cch::support::UniqueFd replacement(::open(pty->slave_name.c_str(), O_RDWR | O_NOCTTY));
    REQUIRE(replacement);
    REQUIRE(replacement.get() == output_descriptor);
    REQUIRE(terminal.stop());
    CHECK(terminal.modes() == cch::tui::TerminalModeState{});
}
#endif

TEST_CASE("Process Terminal enables and restores the keyboard fallback", "[tui][terminal][issue54]") {
    auto pty = cch::tests::open_pseudo_terminal();
    REQUIRE(pty);
    cch::tui::ProcessTerminal terminal({
        .input_fd = pty->slave.get(),
        .output_fd = pty->slave.get(),
    });
    REQUIRE(terminal.start(
        [](std::string) -> cch::support::ExpectedVoid { return {}; },
        [](cch::tui::TerminalDimensions) -> cch::support::ExpectedVoid { return {}; }));
    (void)cch::tests::read_available(pty->master.get());

    constexpr std::string_view kResponseStart = "\x1b[?";
    constexpr std::string_view kResponseEnd = "1;2c";
    REQUIRE(::write(pty->master.get(), kResponseStart.data(), kResponseStart.size()) ==
        static_cast<ssize_t>(kResponseStart.size()));
    REQUIRE(::write(pty->master.get(), kResponseEnd.data(), kResponseEnd.size()) ==
        static_cast<ssize_t>(kResponseEnd.size()));
    REQUIRE(cch::tests::wait_until([&] {
        return terminal.capabilities().keyboard_protocol ==
            cch::tui::KeyboardProtocol::ModifyOtherKeys;
    }));
    CHECK(cch::tests::read_available(pty->master.get()).find("\x1b[>4;2m") != std::string::npos);

    REQUIRE(terminal.stop());
    const auto restoration = cch::tests::read_available(pty->master.get());
    CHECK(restoration.find("\x1b[>4;0m") != std::string::npos);
    CHECK(restoration.find("\x1b[<u") != std::string::npos);
}

#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
// Unwinding through the TUI stack is an exception-enabled-build concern: the
// no-exception build cannot throw through the runtime loop at all.
TEST_CASE("Process Terminal runs a minimal TUI shell and restores during unwinding", "[tui][terminal][issue54]") {
    auto pty = cch::tests::open_pseudo_terminal();
    REQUIRE(pty);
    termios original{};
    REQUIRE(::tcgetattr(pty->slave.get(), &original) == 0);

    try {
        cch::tui::ProcessTerminal terminal({
            .input_fd = pty->slave.get(),
            .output_fd = pty->slave.get(),
        });
        cch::tui::Tui tui(terminal);
        auto component = std::make_unique<MinimalShell>();
        auto* shell = component.get();
        shell->tui = &tui;
        REQUIRE(tui.add_child(std::move(component)));
        REQUIRE(tui.start());
        REQUIRE(tui.set_focus(shell));
        REQUIRE(tui.render());

        winsize resized{
            .ws_row = 28,
            .ws_col = 90,
            .ws_xpixel = 0,
            .ws_ypixel = 0,
        };
        REQUIRE(::ioctl(pty->master.get(), TIOCSWINSZ, &resized) == 0);
        REQUIRE(cch::tests::wait_until([&] { return shell->invalidations.load() >= 1; }));
        REQUIRE(tui.render());
        tui.invalidate();
        REQUIRE(::write(pty->master.get(), "q", 1) == 1);
        REQUIRE(cch::tests::wait_until([&] {
            return shell->inputs.load() == 1 && shell->stop_succeeded.load();
        }));
        throw ExpectedUnwind{};
    } catch (const ExpectedUnwind&) {
    }

    termios restored{};
    REQUIRE(::tcgetattr(pty->slave.get(), &restored) == 0);
    CHECK(cch::tests::same_terminal_state(restored, original));
    const auto output = cch::tests::read_available(pty->master.get());
    CHECK(output.find("shell") != std::string::npos);
    CHECK(output.find("\x1b[?25l") != std::string::npos);
    CHECK(output.find("\x1b[?25h") != std::string::npos);
    CHECK(output.find("\x1b[?2004l") != std::string::npos);
    CHECK(output.find("\x1b[<u") != std::string::npos);
}
#endif

TEST_CASE("Process Terminal rolls back raw input after partial startup failure", "[tui][terminal][issue54]") {
    auto pty = cch::tests::open_pseudo_terminal();
    REQUIRE(pty);
    cch::support::UniqueFd read_only_output(::open(pty->slave_name.c_str(), O_RDONLY | O_NOCTTY));
    REQUIRE(read_only_output);
    termios original{};
    REQUIRE(::tcgetattr(pty->slave.get(), &original) == 0);

    cch::tui::ProcessTerminal terminal({
        .input_fd = pty->slave.get(),
        .output_fd = read_only_output.get(),
    });
    const auto result = terminal.start(
        [](std::string) -> cch::support::ExpectedVoid { return {}; },
        [](cch::tui::TerminalDimensions) -> cch::support::ExpectedVoid { return {}; });

    REQUIRE_FALSE(result);
    CHECK(result.error().code == cch::support::ErrorCode::Process);
    termios restored{};
    REQUIRE(::tcgetattr(pty->slave.get(), &restored) == 0);
    CHECK(cch::tests::same_terminal_state(restored, original));
    CHECK(terminal.modes() == cch::tui::TerminalModeState{});
}


TEST_CASE(
    "Process Terminal reports conservative color and appearance observations",
    "[tui][terminal][theme][issue55]") {
    auto pty = cch::tests::open_pseudo_terminal();
    REQUIRE(pty);
    ScopedEnvironmentVariable terminal_environment("TERM");
    ScopedEnvironmentVariable program_environment("TERM_PROGRAM");
    ScopedEnvironmentVariable color_terminal_environment("COLORTERM");
    ScopedEnvironmentVariable foreground_background_environment("COLORFGBG");
    ScopedEnvironmentVariable terminal_emulator_environment("TERMINAL_EMULATOR");
    ScopedEnvironmentVariable tmux_environment("TMUX");
    ScopedEnvironmentVariable kitty_environment("KITTY_WINDOW_ID");
    ScopedEnvironmentVariable ghostty_environment("GHOSTTY_RESOURCES_DIR");
    ScopedEnvironmentVariable wezterm_environment("WEZTERM_PANE");
    ScopedEnvironmentVariable warp_environment("WARP_SESSION_ID");
    ScopedEnvironmentVariable warp_uuid_environment("WARP_TERMINAL_SESSION_UUID");
    ScopedEnvironmentVariable iterm_environment("ITERM_SESSION_ID");
    ScopedEnvironmentVariable windows_terminal_environment("WT_SESSION");
    terminal_environment.set("xterm-unknown");
    program_environment.unset();
    color_terminal_environment.unset();
    foreground_background_environment.unset();
    terminal_emulator_environment.unset();
    tmux_environment.unset();
    kitty_environment.unset();
    ghostty_environment.unset();
    wezterm_environment.unset();
    warp_environment.unset();
    warp_uuid_environment.unset();
    iterm_environment.unset();
    windows_terminal_environment.unset();

    cch::tui::ProcessTerminal terminal({
        .input_fd = pty->slave.get(),
        .output_fd = pty->slave.get(),
    });
    const auto probe_started = std::chrono::steady_clock::now();
    REQUIRE(terminal.start(
        [](std::string) -> cch::support::ExpectedVoid { return {}; },
        [](cch::tui::TerminalDimensions) -> cch::support::ExpectedVoid { return {}; }));
    CHECK(std::chrono::steady_clock::now() - probe_started < std::chrono::milliseconds(500));
    CHECK(terminal.capabilities().color == cch::tui::TerminalColorCapability::Xterm256);
    CHECK(terminal.capabilities().appearance == cch::tui::TerminalAppearance::Unknown);
    REQUIRE(terminal.stop());
    (void)cch::tests::read_available(pty->master.get());

    color_terminal_environment.set("24BIT");
    foreground_background_environment.set("0; +15ignored ");
    REQUIRE(terminal.start(
        [](std::string) -> cch::support::ExpectedVoid { return {}; },
        [](cch::tui::TerminalDimensions) -> cch::support::ExpectedVoid { return {}; }));
    CHECK(terminal.capabilities().color == cch::tui::TerminalColorCapability::TrueColor);
    CHECK(terminal.capabilities().appearance == cch::tui::TerminalAppearance::Light);
    REQUIRE(terminal.stop());
    (void)cch::tests::read_available(pty->master.get());

    color_terminal_environment.unset();
    foreground_background_environment.set("15;0");
    terminal_environment.set("tmux-256color");
    program_environment.set("WezTerm");
    REQUIRE(terminal.start(
        [](std::string) -> cch::support::ExpectedVoid { return {}; },
        [](cch::tui::TerminalDimensions) -> cch::support::ExpectedVoid { return {}; }));
    CHECK(terminal.capabilities().color == cch::tui::TerminalColorCapability::Xterm256);
    CHECK(terminal.capabilities().appearance == cch::tui::TerminalAppearance::Dark);
    REQUIRE(terminal.stop());
    (void)cch::tests::read_available(pty->master.get());

    terminal_environment.set("xterm-unknown");
    REQUIRE(terminal.start(
        [](std::string) -> cch::support::ExpectedVoid { return {}; },
        [](cch::tui::TerminalDimensions) -> cch::support::ExpectedVoid { return {}; }));
    CHECK(terminal.capabilities().color == cch::tui::TerminalColorCapability::TrueColor);
    CHECK(terminal.capabilities().appearance == cch::tui::TerminalAppearance::Dark);
    REQUIRE(terminal.stop());
}

TEST_CASE(
    "Process Terminal probes color scheme and OSC background without consuming user input",
    "[tui][terminal][theme][issue55]") {
    struct ProbeCase {
        std::string response;
        cch::tui::TerminalAppearance expected{cch::tui::TerminalAppearance::Unknown};
        bool expects_input{false};
    };
    const std::array cases{
        ProbeCase{
            .response = "\x1b]11;#000000\x07\x1b[?997;2ntyped",
            .expected = cch::tui::TerminalAppearance::Light,
            .expects_input = true,
        },
        ProbeCase{
            .response = "\x1b]11;#000000\x07",
            .expected = cch::tui::TerminalAppearance::Dark,
        },
        ProbeCase{
            .response = "\x1b]11;#ffffffffffff\x1b\\",
            .expected = cch::tui::TerminalAppearance::Light,
        },
        ProbeCase{
            .response = "\x1b]11;rgba:f/0/0/f\x07",
            .expected = cch::tui::TerminalAppearance::Dark,
        },
        ProbeCase{
            .response = "\x1b[?997;1n\x1b]11;rgb:ffff/ffff/ffff\x07",
            .expected = cch::tui::TerminalAppearance::Dark,
        },
    };
    ScopedEnvironmentVariable foreground_background_environment("COLORFGBG");
    foreground_background_environment.set("0;15");

    for (const auto& probe_case : cases) {
        auto pty = cch::tests::open_pseudo_terminal();
        REQUIRE(pty);
        std::atomic<bool> answered{false};
        std::atomic<bool> delivered_input{false};
        std::jthread responder([&] {
            answered = answer_appearance_query(pty->master.get(), probe_case.response);
        });
        cch::tui::ProcessTerminal terminal({
            .input_fd = pty->slave.get(),
            .output_fd = pty->slave.get(),
        });

        REQUIRE(terminal.start(
            [&](std::string input) -> cch::support::ExpectedVoid {
                if (input.find("typed") != std::string::npos) delivered_input = true;
                return {};
            },
            [](cch::tui::TerminalDimensions) -> cch::support::ExpectedVoid { return {}; }));
        responder.join();
        REQUIRE(answered.load());
        CHECK(terminal.capabilities().appearance == probe_case.expected);
        if (probe_case.expects_input) {
            CHECK(cch::tests::wait_until([&] { return delivered_input.load(); }));
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            CHECK_FALSE(delivered_input.load());
        }
        REQUIRE(terminal.stop());
    }
}

TEST_CASE(
    "Process Terminal consumes malformed fragmented and late appearance replies",
    "[tui][terminal][theme][issue55]") {
    auto pty = cch::tests::open_pseudo_terminal();
    REQUIRE(pty);
    ScopedEnvironmentVariable foreground_background_environment("COLORFGBG");
    foreground_background_environment.set("15;0");
    std::mutex input_mutex;
    std::string delivered;
    cch::tui::ProcessTerminal terminal({
        .input_fd = pty->slave.get(),
        .output_fd = pty->slave.get(),
    });
    REQUIRE(terminal.start(
        [&](std::string input) -> cch::support::ExpectedVoid {
            std::lock_guard lock(input_mutex);
            delivered += input;
            return {};
        },
        [](cch::tui::TerminalDimensions) -> cch::support::ExpectedVoid { return {}; }));
    (void)cch::tests::read_available(pty->master.get());
    CHECK(terminal.capabilities().appearance == cch::tui::TerminalAppearance::Dark);

    constexpr std::string_view kMalformedStart = "\x1b]11;not-a-color";
    constexpr std::string_view kMalformedEnd = "\x07typed\x1b[?997;9n";
    REQUIRE(::write(pty->master.get(), kMalformedStart.data(), kMalformedStart.size()) ==
        static_cast<ssize_t>(kMalformedStart.size()));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    REQUIRE(::write(pty->master.get(), kMalformedEnd.data(), kMalformedEnd.size()) ==
        static_cast<ssize_t>(kMalformedEnd.size()));
    REQUIRE(cch::tests::wait_until([&] {
        std::lock_guard lock(input_mutex);
        return delivered.find("typed") != std::string::npos;
    }));
    {
        std::lock_guard lock(input_mutex);
        CHECK(delivered.find("not-a-color") == std::string::npos);
        CHECK(delivered.find("997") == std::string::npos);
    }
    CHECK(terminal.capabilities().appearance == cch::tui::TerminalAppearance::Dark);

    constexpr std::string_view kLateStart = "\x1b[?997;";
    constexpr std::string_view kLateEnd = "2nlate";
    REQUIRE(::write(pty->master.get(), kLateStart.data(), kLateStart.size()) ==
        static_cast<ssize_t>(kLateStart.size()));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    REQUIRE(::write(pty->master.get(), kLateEnd.data(), kLateEnd.size()) ==
        static_cast<ssize_t>(kLateEnd.size()));
    REQUIRE(cch::tests::wait_until([&] {
        std::lock_guard lock(input_mutex);
        return delivered.find("late") != std::string::npos;
    }));
    REQUIRE(cch::tests::wait_until([&] {
        return terminal.capabilities().appearance == cch::tui::TerminalAppearance::Light;
    }));
    {
        std::lock_guard lock(input_mutex);
        CHECK(delivered.find("997") == std::string::npos);
    }

    constexpr std::string_view kUnterminated = "\x1b]11;unterminated";
    REQUIRE(::write(pty->master.get(), kUnterminated.data(), kUnterminated.size()) ==
        static_cast<ssize_t>(kUnterminated.size()));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    constexpr std::string_view kAfterUnterminated = "after-unterminated";
    REQUIRE(::write(pty->master.get(), kAfterUnterminated.data(), kAfterUnterminated.size()) ==
        static_cast<ssize_t>(kAfterUnterminated.size()));
    REQUIRE(cch::tests::wait_until([&] {
        std::lock_guard lock(input_mutex);
        return delivered.find(kAfterUnterminated) != std::string::npos;
    }));
    {
        std::lock_guard lock(input_mutex);
        CHECK(delivered.find("11;unterminated") == std::string::npos);
    }

    constexpr char kEscape = '\x1b';
    REQUIRE(::write(pty->master.get(), &kEscape, 1) == 1);
    REQUIRE(cch::tests::wait_until([&] {
        std::lock_guard lock(input_mutex);
        return delivered.find(kEscape) != std::string::npos;
    }));
    REQUIRE(terminal.stop());
}

TEST_CASE("Process Terminal writes OSC 0 window titles", "[tui][terminal][issue378]") {
    auto pty = cch::tests::open_pseudo_terminal();
    REQUIRE(pty);
    cch::tui::ProcessTerminal terminal({
        .input_fd = pty->slave.get(),
        .output_fd = pty->slave.get(),
    });

    const auto before_start = terminal.set_title("title");
    REQUIRE_FALSE(before_start);
    CHECK(
        before_start.error().message ==
        "Process Terminal must be started before terminal operations");

    REQUIRE(terminal.start(
        [](std::string) -> cch::support::ExpectedVoid { return {}; },
        [](cch::tui::TerminalDimensions) -> cch::support::ExpectedVoid { return {}; }));
    (void)cch::tests::read_available(pty->master.get());
    REQUIRE(terminal.set_title("pike - session - workspace"));
    CHECK(cch::tests::read_available(pty->master.get()) == "\x1b]0;pike - session - workspace\x07");
    REQUIRE(terminal.stop());
}

TEST_CASE(
    "Process Terminal clear screen emits the pi-exact clear-scrollback bytes",
    "[tui][terminal][issue435]") {
    auto pty = cch::tests::open_pseudo_terminal();
    REQUIRE(pty);
    cch::tui::ProcessTerminal terminal({
        .input_fd = pty->slave.get(),
        .output_fd = pty->slave.get(),
    });

    const auto before_start = terminal.clear_screen();
    REQUIRE_FALSE(before_start);
    CHECK(
        before_start.error().message ==
        "Process Terminal must be started before terminal operations");

    REQUIRE(terminal.start(
        [](std::string) -> cch::support::ExpectedVoid { return {}; },
        [](cch::tui::TerminalDimensions) -> cch::support::ExpectedVoid { return {}; }));
    (void)cch::tests::read_available(pty->master.get());
    // ADR 0037: the resize full-redraw path clears screen, homes, and clears
    // the terminal's scroll history together (`\x1b[2J\x1b[H\x1b[3J`), matching
    // pi's TuiMainScreen resize redraw.
    REQUIRE(terminal.clear_screen());
    CHECK(
        cch::tests::read_available(pty->master.get()) == "\x1b[2J\x1b[H\x1b[3J");
    REQUIRE(terminal.stop());
}

TEST_CASE(
    "Process Terminal re-emits active progress on the one-second keepalive",
    "[tui][terminal][issue378]") {
    constexpr std::string_view kProgressActiveSequence = "\x1b]9;4;3\x07";
    constexpr std::string_view kProgressClearSequence = "\x1b]9;4;0;\x07";
    auto pty = cch::tests::open_pseudo_terminal();
    REQUIRE(pty);
    cch::tui::ProcessTerminal terminal({
        .input_fd = pty->slave.get(),
        .output_fd = pty->slave.get(),
    });
    REQUIRE(terminal.start(
        [](std::string) -> cch::support::ExpectedVoid { return {}; },
        [](cch::tui::TerminalDimensions) -> cch::support::ExpectedVoid { return {}; }));
    std::string accumulated = cch::tests::read_available(pty->master.get());

    const auto count_active = [&] {
        std::size_t count = 0;
        std::size_t position = 0;
        while ((position = accumulated.find(kProgressActiveSequence, position)) != std::string::npos) {
            ++count;
            position += kProgressActiveSequence.size();
        }
        return count;
    };

    REQUIRE(terminal.set_progress(true));
    accumulated += cch::tests::read_available(pty->master.get());
    CHECK(accumulated.find(kProgressActiveSequence) != std::string::npos);
    CHECK(accumulated.find(kProgressClearSequence) == std::string::npos);

    // pi's 1-second keepalive: the active sequence is re-emitted while active.
    REQUIRE(cch::tests::wait_until(
        [&] {
            accumulated += cch::tests::read_available(pty->master.get());
            return count_active() >= 2;
        },
        std::chrono::seconds(3)));
    REQUIRE(terminal.set_progress(false));
    accumulated += cch::tests::read_available(pty->master.get());
    CHECK(accumulated.find(kProgressClearSequence) != std::string::npos);
    const auto emissions_before_clear = count_active();

    // Deactivation stops the keepalive: no further active sequences.
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    accumulated += cch::tests::read_available(pty->master.get());
    CHECK(count_active() == emissions_before_clear);
    REQUIRE(terminal.stop());
}

TEST_CASE(
    "Process Terminal stop clears an active progress indicator and restarts cleanly",
    "[tui][terminal][issue378]") {
    auto pty = cch::tests::open_pseudo_terminal();
    REQUIRE(pty);
    cch::tui::ProcessTerminal terminal({
        .input_fd = pty->slave.get(),
        .output_fd = pty->slave.get(),
    });
    REQUIRE(terminal.start(
        [](std::string) -> cch::support::ExpectedVoid { return {}; },
        [](cch::tui::TerminalDimensions) -> cch::support::ExpectedVoid { return {}; }));
    (void)cch::tests::read_available(pty->master.get());

    REQUIRE(terminal.set_progress(true));
    (void)cch::tests::read_available(pty->master.get());
    REQUIRE(terminal.stop());
    const auto restoration = cch::tests::read_available(pty->master.get());
    CHECK(restoration.find("\x1b]9;4;0;\x07") != std::string::npos);
    CHECK(restoration.find("\x1b[?2004l") != std::string::npos);
    CHECK(terminal.modes() == cch::tui::TerminalModeState{});

    // A restart must not re-arm the keepalive from the previous session.
    REQUIRE(terminal.start(
        [](std::string) -> cch::support::ExpectedVoid { return {}; },
        [](cch::tui::TerminalDimensions) -> cch::support::ExpectedVoid { return {}; }));
    (void)cch::tests::read_available(pty->master.get());
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    const auto after_restart = cch::tests::read_available(pty->master.get());
    CHECK(after_restart.find("\x1b]9;4;3\x07") == std::string::npos);
    REQUIRE(terminal.stop());
}

TEST_CASE(
    "Process Terminal drains buffered input before exit without delivering it",
    "[tui][terminal][issue378]") {
    auto pty = cch::tests::open_pseudo_terminal();
    REQUIRE(pty);
    std::mutex delivered_mutex;
    std::vector<std::string> delivered;
    cch::tui::ProcessTerminal terminal({
        .input_fd = pty->slave.get(),
        .output_fd = pty->slave.get(),
    });
    REQUIRE(terminal.start(
        [&](std::string input) -> cch::support::ExpectedVoid {
            std::lock_guard lock(delivered_mutex);
            delivered.push_back(std::move(input));
            return {};
        },
        [](cch::tui::TerminalDimensions) -> cch::support::ExpectedVoid { return {}; }));
    (void)cch::tests::read_available(pty->master.get());

    // A writer floods the terminal with Kitty key-release-style garbage. It
    // waits out the drain's entry so every byte arrives after draining began.
    std::jthread writer([&] {
        const std::string burst(4096, 'x');
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        for (int i = 0; i < 40; ++i) {
            if (::write(pty->master.get(), burst.data(), burst.size()) !=
                static_cast<ssize_t>(burst.size())) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });

    const auto drain_started = std::chrono::steady_clock::now();
    REQUIRE(terminal.drain_input(
        std::chrono::seconds(2),
        std::chrono::milliseconds(100)));
    const auto drain_elapsed = std::chrono::steady_clock::now() - drain_started;
    writer.join();

    // The drain stayed alive while input flowed and exited once input went idle.
    CHECK(drain_elapsed >= std::chrono::milliseconds(300));
    CHECK(drain_elapsed < std::chrono::seconds(2));
    // Nothing reached the input sink during the drain.
    {
        std::lock_guard lock(delivered_mutex);
        CHECK(delivered.empty());
    }
    // Everything was consumed: nothing remains buffered for the parent shell.
    CHECK(cch::tests::read_available(pty->slave.get(), std::chrono::milliseconds(50)).empty());
    REQUIRE(terminal.stop());
}

TEST_CASE(
    "Process Terminal drain disables keyboard protocols once before exit",
    "[tui][terminal][issue378]") {
    auto pty = cch::tests::open_pseudo_terminal();
    REQUIRE(pty);
    cch::tui::ProcessTerminal terminal({
        .input_fd = pty->slave.get(),
        .output_fd = pty->slave.get(),
    });
    REQUIRE(terminal.start(
        [](std::string) -> cch::support::ExpectedVoid { return {}; },
        [](cch::tui::TerminalDimensions) -> cch::support::ExpectedVoid { return {}; }));
    (void)cch::tests::read_available(pty->master.get());

    // Activate the modifyOtherKeys fallback via the device-attributes response.
    constexpr std::string_view kResponse = "\x1b[?1;2c";
    REQUIRE(
        ::write(pty->master.get(), kResponse.data(), kResponse.size()) ==
        static_cast<ssize_t>(kResponse.size()));
    REQUIRE(cch::tests::wait_until([&] {
        return terminal.capabilities().keyboard_protocol ==
            cch::tui::KeyboardProtocol::ModifyOtherKeys;
    }));
    (void)cch::tests::read_available(pty->master.get());

    const auto drain_started = std::chrono::steady_clock::now();
    REQUIRE(terminal.drain_input(
        std::chrono::milliseconds(1000),
        std::chrono::milliseconds(20)));
    CHECK(
        std::chrono::steady_clock::now() - drain_started <
        std::chrono::milliseconds(300));
    CHECK(terminal.capabilities().keyboard_protocol == cch::tui::KeyboardProtocol::Legacy);
    const auto drained = cch::tests::read_available(pty->master.get());
    CHECK(drained.find("\x1b[<u") != std::string::npos);
    CHECK(drained.find("\x1b[>4;0m") != std::string::npos);

    // stop() must not re-emit the disables drain_input already wrote.
    REQUIRE(terminal.stop());
    const auto restoration = cch::tests::read_available(pty->master.get());
    CHECK(restoration.find("\x1b[<u") == std::string::npos);
    CHECK(restoration.find("\x1b[>4;0m") == std::string::npos);
    CHECK(terminal.modes() == cch::tui::TerminalModeState{});
}

namespace {

using cch::tests::ImageEnvironmentGuard;

} // namespace

TEST_CASE(
    "Process Terminal detects image capabilities and queries cell size from the environment",
    "[tui][terminal][image][issue385]") {
    auto pty = cch::tests::open_pseudo_terminal();
    REQUIRE(pty);
    ImageEnvironmentGuard environment;
    environment.set("TERM_PROGRAM", "ghostty");

    cch::tui::ProcessTerminal terminal({
        .input_fd = pty->slave.get(),
        .output_fd = pty->slave.get(),
    });
    REQUIRE(terminal.start(
        [](std::string) -> cch::support::ExpectedVoid { return {}; },
        [](cch::tui::TerminalDimensions) -> cch::support::ExpectedVoid { return {}; }));
    CHECK(terminal.capabilities().inline_images == cch::tui::InlineImageProtocol::Kitty);
    CHECK(terminal.capabilities().hyperlinks);
    // pi's 9x18 default applies until a CSI 16 t response arrives.
    CHECK(terminal.capabilities().cell_pixels == cch::tui::CellPixelDimensions{});

    const auto startup = cch::tests::read_available(pty->master.get());
    CHECK(startup.find("\x1b[16t") != std::string::npos);
    REQUIRE(terminal.stop());
}

TEST_CASE(
    "Process Terminal stays conservative under unknown and tmux environments",
    "[tui][terminal][image][issue385]") {
    auto pty = cch::tests::open_pseudo_terminal();
    REQUIRE(pty);
    {
        ImageEnvironmentGuard environment;
        cch::tui::ProcessTerminal terminal({
            .input_fd = pty->slave.get(),
            .output_fd = pty->slave.get(),
        });
        REQUIRE(terminal.start(
        [](std::string) -> cch::support::ExpectedVoid { return {}; },
        [](cch::tui::TerminalDimensions) -> cch::support::ExpectedVoid { return {}; }));
        CHECK(terminal.capabilities().inline_images == cch::tui::InlineImageProtocol::None);
        CHECK_FALSE(terminal.capabilities().hyperlinks);
        CHECK_FALSE(cch::tests::read_available(pty->master.get()).find("\x1b[16t") != std::string::npos);
        REQUIRE(terminal.stop());
    }
    {
        ImageEnvironmentGuard environment;
        environment.set("TMUX", "/tmp/tmux-1000/default,1234,0");
        environment.set("TERM", "tmux-256color");
        environment.set("TERM_PROGRAM", "ghostty");
        cch::tui::ProcessTerminal terminal({
            .input_fd = pty->slave.get(),
            .output_fd = pty->slave.get(),
        });
        REQUIRE(terminal.start(
        [](std::string) -> cch::support::ExpectedVoid { return {}; },
        [](cch::tui::TerminalDimensions) -> cch::support::ExpectedVoid { return {}; }));
        CHECK(terminal.capabilities().inline_images == cch::tui::InlineImageProtocol::None);
        REQUIRE(terminal.stop());
    }
}

TEST_CASE(
    "Process Terminal consumes CSI 16 t cell-size responses and notifies re-render",
    "[tui][terminal][image][issue385]") {
    auto pty = cch::tests::open_pseudo_terminal();
    REQUIRE(pty);
    ImageEnvironmentGuard environment;
    environment.set("TERM_PROGRAM", "ghostty");
    std::mutex events_mutex;
    std::vector<std::string> inputs;
    std::vector<cch::tui::TerminalDimensions> notifications;

    cch::tui::ProcessTerminal terminal({
        .input_fd = pty->slave.get(),
        .output_fd = pty->slave.get(),
    });
    REQUIRE(terminal.start(
        [&](std::string input) -> cch::support::ExpectedVoid {
            std::lock_guard lock(events_mutex);
            inputs.push_back(std::move(input));
            return {};
        },
        [&](cch::tui::TerminalDimensions dimensions) -> cch::support::ExpectedVoid {
            std::lock_guard lock(events_mutex);
            notifications.push_back(dimensions);
            return {};
        }));
    (void)cch::tests::read_available(pty->master.get());

    constexpr std::string_view kResponse = "\x1b[6;20;10t";
    REQUIRE(
        ::write(pty->master.get(), kResponse.data(), kResponse.size()) ==
        static_cast<ssize_t>(kResponse.size()));
    REQUIRE(cch::tests::wait_until([&] {
        const auto capabilities = terminal.capabilities();
        return capabilities.cell_pixels &&
            *capabilities.cell_pixels ==
            cch::tui::CellPixelDimensions{.width = 10, .height = 20};
    }));
    REQUIRE(cch::tests::wait_until([&] {
        std::lock_guard lock(events_mutex);
        return !notifications.empty();
    }));
    {
        std::lock_guard lock(events_mutex);
        CHECK(notifications.back() == terminal.dimensions());
        CHECK(inputs.empty());
    }

    // User input after the response still forwards.
    REQUIRE(::write(pty->master.get(), "q", 1) == 1);
    REQUIRE(cch::tests::wait_until([&] {
        std::lock_guard lock(events_mutex);
        return !inputs.empty();
    }));
    {
        std::lock_guard lock(events_mutex);
        CHECK(inputs.front() == "q");
    }
    REQUIRE(terminal.stop());
}

TEST_CASE(
    "Process Terminal places and removes images with pi-exact protocol bytes",
    "[tui][terminal][image][issue385]") {
    auto pty = cch::tests::open_pseudo_terminal();
    REQUIRE(pty);
    ImageEnvironmentGuard environment;
    environment.set("TERM_PROGRAM", "ghostty");

    cch::tui::ProcessTerminal terminal({
        .input_fd = pty->slave.get(),
        .output_fd = pty->slave.get(),
    });
    REQUIRE(terminal.start(
        [](std::string) -> cch::support::ExpectedVoid { return {}; },
        [](cch::tui::TerminalDimensions) -> cch::support::ExpectedVoid { return {}; }));
    (void)cch::tests::read_available(pty->master.get());

    const cch::tui::TerminalImage image{
        .encoded_data = "QUFBQQ==",
        .mime_type = "image/png",
        .filename = std::string_view{"name.png"},
        .pixel_width = 10,
        .pixel_height = 10,
        .resource_id = 7,
        .revision = 2,
        .region = {.column = 2, .row = 1, .columns = 2, .rows = 2},
    };
    const auto placed = terminal.place_image(image);
    REQUIRE(placed);
    const auto placed_output = cch::tests::read_available(pty->master.get());
    // Cursor positioned at the region origin (row 1 -> 2, column 2 -> 3),
    // then the Kitty placement with the handle as the image ID.
    CHECK(placed_output.find("\x1b[2;3H") != std::string::npos);
    CHECK(placed_output.find("\x1b_Ga=T,f=100,q=2,C=1,c=2,r=2,i=1;QUFBQQ==\x1b\\") != std::string::npos);

    // An animation frame re-place reuses the protocol image id (pi imageId
    // reuse): the same `i=1` is re-transmitted and no fresh id is allocated.
    const cch::tui::TerminalImage frame{
        .encoded_data = "QUFBQQ==",
        .mime_type = "image/png",
        .pixel_width = 10,
        .pixel_height = 10,
        .resource_id = 7,
        .revision = 3,
        .region = {.column = 2, .row = 1, .columns = 2, .rows = 2},
        .preferred_handle = *placed,
    };
    const auto replaced = terminal.place_image(frame);
    REQUIRE(replaced);
    CHECK(*replaced == *placed);
    const auto replaced_output = cch::tests::read_available(pty->master.get());
    CHECK(replaced_output.find("\x1b_Ga=T,f=100,q=2,C=1,c=2,r=2,i=1;QUFBQQ==\x1b\\") != std::string::npos);

    REQUIRE(terminal.remove_image(*placed, image.region));
    const auto removed_output = cch::tests::read_available(pty->master.get());
    CHECK(removed_output.find("\x1b_Ga=d,d=I,i=1,q=2\x1b\\") != std::string::npos);
    // The owned cells are blanked so protocols without addressable deletion
    // (iTerm2) clear exactly the placement, matching VirtualTerminal.
    CHECK(removed_output.find("\x1b[2;3H") != std::string::npos);
    CHECK(removed_output.find(std::string(4, ' ')) != std::string::npos);
    REQUIRE(terminal.stop());
}

TEST_CASE(
    "Process Terminal iTerm2 placement omits default preserveAspectRatio and blanks removal",
    "[tui][terminal][image][issue385]") {
    auto pty = cch::tests::open_pseudo_terminal();
    REQUIRE(pty);
    ImageEnvironmentGuard environment;
    environment.set("TERM_PROGRAM", "iterm.app");

    cch::tui::ProcessTerminal terminal({
        .input_fd = pty->slave.get(),
        .output_fd = pty->slave.get(),
    });
    REQUIRE(terminal.start(
        [](std::string) -> cch::support::ExpectedVoid { return {}; },
        [](cch::tui::TerminalDimensions) -> cch::support::ExpectedVoid { return {}; }));
    (void)cch::tests::read_available(pty->master.get());

    const cch::tui::TerminalImage image{
        .encoded_data = "QUFBQQ==",
        .mime_type = "image/png",
        .pixel_width = 10,
        .pixel_height = 10,
        .resource_id = 1,
        .revision = 1,
        .region = {.column = 0, .row = 0, .columns = 1, .rows = 1},
    };
    const auto placed = terminal.place_image(image);
    REQUIRE(placed);
    const auto placed_output = cch::tests::read_available(pty->master.get());
    CHECK(placed_output.find("\x1b]1337;File=inline=1;width=1;height=1:QUFBQQ==\x07") != std::string::npos);
    CHECK(placed_output.find("preserveAspectRatio") == std::string::npos);

    REQUIRE(terminal.remove_image(*placed, image.region));
    const auto removed_output = cch::tests::read_available(pty->master.get());
    CHECK(removed_output.find("a=d") == std::string::npos);
    CHECK(removed_output.find(" ") != std::string::npos);
    REQUIRE(terminal.stop());
}

TEST_CASE(
    "Process Terminal scrolls the native scrollback with CRLF line flow past the viewport",
    "[tui][terminal][issue435]") {
    auto pty = cch::tests::open_pseudo_terminal(40, 8);
    REQUIRE(pty);
    cch::tui::ProcessTerminal terminal({
        .input_fd = pty->slave.get(),
        .output_fd = pty->slave.get(),
    });
    REQUIRE(terminal.start(
        [](std::string) -> cch::support::ExpectedVoid { return {}; },
        [](cch::tui::TerminalDimensions) -> cch::support::ExpectedVoid { return {}; }));
    (void)cch::tests::read_available(pty->master.get());

    // Buffer row 10 on an 8-row viewport is 3 rows past the bottom: the
    // terminal moves to the bottom (row 7) then emits CRLF line flow so the
    // real terminal's native scrollback receives the overflow (pi's append
    // `"\r\n".repeat(scroll)`; absolute CUP beyond the bottom clamps instead
    // of scrolling, which is why the emulated scroll is emitted explicitly).
    // The exact \r/\n mix depends on the PTY's output processing (ONLCR), so
    // the assertions count line-flow characters rather than pin bytes.
    REQUIRE(terminal.set_cursor({.column = 0, .row = 10}));
    auto output = cch::tests::read_available(pty->master.get());
    CHECK(output.find("\x1b[7B") != std::string::npos);
    CHECK(output.find("\x1b[11;1H") == std::string::npos);
    CHECK(std::count(output.begin(), output.end(), '\r') +
            std::count(output.begin(), output.end(), '\n') >= 3);

    // A subsequent sequential row past the new viewport bottom scrolls one
    // line: the cursor is already at the bottom, so only line flow is emitted
    // (no absolute CUP, no move-down).
    REQUIRE(terminal.set_cursor({.column = 0, .row = 11}));
    output = cch::tests::read_available(pty->master.get());
    CHECK(std::count(output.begin(), output.end(), '\r') +
            std::count(output.begin(), output.end(), '\n') >= 1);
    CHECK(output.find("\x1b[") == std::string::npos);

    // An in-viewport buffer row converts to a screen-relative CUP row using
    // the emulated viewport top: after 4 scrolled lines the viewport top is
    // 4, so buffer row 9 maps to screen row 5.
    REQUIRE(terminal.set_cursor({.column = 0, .row = 9}));
    output = cch::tests::read_available(pty->master.get());
    CHECK(output.find("\x1b[6;1H") != std::string::npos);

    REQUIRE(terminal.stop());
}

TEST_CASE(
    "Process Terminal anchors the buffer origin at the probed cursor row",
    "[tui][terminal][issue476]") {
    auto pty = cch::tests::open_pseudo_terminal(40, 12);
    REQUIRE(pty);
    // Answer the startup DSR cursor-position query (`\x1b[6n`) with the
    // shell's mid-screen cursor row 8 (1-based), mirroring
    // answer_appearance_query (ADR 0041 anchored absolute flow).
    std::atomic<bool> answered{false};
    std::jthread responder([&] {
        answered = answer_cursor_position_query(pty->master.get(), "\x1b[8;1R");
    });
    std::mutex events_mutex;
    std::vector<std::string> inputs;
    cch::tui::ProcessTerminal terminal({
        .input_fd = pty->slave.get(),
        .output_fd = pty->slave.get(),
    });
    REQUIRE(terminal.start(
        [&](std::string input) -> cch::support::ExpectedVoid {
            std::lock_guard lock(events_mutex);
            inputs.push_back(std::move(input));
            return {};
        },
        [](cch::tui::TerminalDimensions) -> cch::support::ExpectedVoid { return {}; }));
    responder.join();
    REQUIRE(answered.load());
    const auto startup_output = cch::tests::read_available(pty->master.get());
    // A successful probe never emits the DSR-timeout clear-screen fallback.
    CHECK(startup_output.find("\x1b[2J") == std::string::npos);

    // Buffer row 0 maps to the anchored screen row 7 (0-based): CUP row 8
    // instead of the hard-coded `\x1b[1;1H` of the pre-anchor model.
    REQUIRE(terminal.set_cursor({.column = 0, .row = 0}));
    auto output = cch::tests::read_available(pty->master.get());
    CHECK(output.find("\x1b[8;1H") != std::string::npos);
    CHECK(output.find("\x1b[1;1H") == std::string::npos);

    // Buffer row 11 maps to screen row 18 on a 12-row screen: the scroll-flow
    // branch moves from the probed cursor row 7 to the bottom (`\x1b[4B`) and
    // emits 7 CRLF line-flow steps instead of a clamping absolute CUP.
    REQUIRE(terminal.set_cursor({.column = 0, .row = 11}));
    output = cch::tests::read_available(pty->master.get());
    CHECK(output.find("\x1b[4B") != std::string::npos);
    CHECK(output.find("\x1b[19;1H") == std::string::npos);
    CHECK(std::count(output.begin(), output.end(), '\n') >= 7);

    // After 7 scrolls the viewport top is 7, so buffer row 10 maps to screen
    // row 7 + 10 - 7 = 10 (CUP row 11).
    REQUIRE(terminal.set_cursor({.column = 0, .row = 10}));
    output = cch::tests::read_available(pty->master.get());
    CHECK(output.find("\x1b[11;1H") != std::string::npos);

    // A late or duplicate CPR arriving on the worker is consumed and ignored:
    // it never leaks into the user key stream (ADR 0041).
    constexpr std::string_view kLateResponse = "\x1b[10;5R";
    REQUIRE(::write(pty->master.get(), kLateResponse.data(), kLateResponse.size()) ==
        static_cast<ssize_t>(kLateResponse.size()));
    REQUIRE(::write(pty->master.get(), "q", 1) == 1);
    REQUIRE(cch::tests::wait_until([&] {
        std::lock_guard lock(events_mutex);
        return !inputs.empty();
    }));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    {
        std::lock_guard lock(events_mutex);
        CHECK(inputs.front() == "q");
        for (const auto& input : inputs) {
            CHECK(input.find('R') == std::string::npos);
            CHECK(input.find("10;5") == std::string::npos);
        }
    }
    REQUIRE(terminal.stop());
}

TEST_CASE(
    "Process Terminal clears the screen and anchors at row 0 when the cursor probe times out",
    "[tui][terminal][issue476]") {
    auto pty = cch::tests::open_pseudo_terminal(40, 12);
    REQUIRE(pty);
    cch::tui::ProcessTerminal terminal({
        .input_fd = pty->slave.get(),
        .output_fd = pty->slave.get(),
    });
    // Nothing answers the DSR query: after the ~250 ms deadline the adapter
    // emits the clear-screen + home + scrollback fallback and anchors the
    // origin at row 0 (ADR 0041). One startup pays the timeout once.
    REQUIRE(terminal.start(
        [](std::string) -> cch::support::ExpectedVoid { return {}; },
        [](cch::tui::TerminalDimensions) -> cch::support::ExpectedVoid { return {}; }));
    const auto startup_output = cch::tests::read_available(pty->master.get());
    CHECK(startup_output.find("\x1b[6n") != std::string::npos);
    CHECK(startup_output.find("\x1b[2J\x1b[H\x1b[3J") != std::string::npos);

    // Origin 0: buffer row 0 is screen row 0, exactly the pre-anchor model.
    REQUIRE(terminal.set_cursor({.column = 0, .row = 0}));
    const auto output = cch::tests::read_available(pty->master.get());
    CHECK(output.find("\x1b[1;1H") != std::string::npos);
    REQUIRE(terminal.stop());
}

TEST_CASE(
    "Process Terminal waits on readiness and wakes promptly for stop",
    "[tui][terminal][issue462]") {
    auto pty = cch::tests::open_pseudo_terminal();
    REQUIRE(pty);
    cch::tui::ProcessTerminal terminal({
        .input_fd = pty->slave.get(),
        .output_fd = pty->slave.get(),
    });
    REQUIRE(terminal.start(
        [](std::string) -> cch::support::ExpectedVoid { return {}; },
        [](cch::tui::TerminalDimensions) -> cch::support::ExpectedVoid { return {}; }));
    (void)cch::tests::read_available(pty->master.get());

    // Idle: the delivery worker parks in a blocking readiness wait (the only
    // armed timer is the 500 ms resize watchdog). stop() wakes it through the
    // wakeup pipe instead of waiting for a poll interval, so cancellation is
    // prompt even with no input activity (issue #462).
    const auto started = std::chrono::steady_clock::now();
    REQUIRE(terminal.stop());
    const auto elapsed = std::chrono::steady_clock::now() - started;
    CHECK(elapsed < std::chrono::milliseconds(400));
    CHECK(terminal.modes() == cch::tui::TerminalModeState{});
}

TEST_CASE(
    "Process Terminal stops promptly while input is streaming",
    "[tui][terminal][issue462]") {
    auto pty = cch::tests::open_pseudo_terminal();
    REQUIRE(pty);
    std::atomic<bool> delivering{false};
    cch::tui::ProcessTerminal terminal({
        .input_fd = pty->slave.get(),
        .output_fd = pty->slave.get(),
    });
    REQUIRE(terminal.start(
        [&](std::string) -> cch::support::ExpectedVoid { delivering = true; return {}; },
        [](cch::tui::TerminalDimensions) -> cch::support::ExpectedVoid { return {}; }));
    (void)cch::tests::read_available(pty->master.get());

    // Stream input continuously so the worker is busy reading and delivering;
    // stop() must still interrupt the wait promptly (issue #462).
    std::jthread writer([&] {
        const std::string burst(256, 'x');
        while (!terminal.modes().started) std::this_thread::yield();
        while (terminal.modes().started) {
            if (::write(pty->master.get(), burst.data(), burst.size()) !=
                static_cast<ssize_t>(burst.size())) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    REQUIRE(cch::tests::wait_until([&] { return delivering.load(); }));

    const auto started = std::chrono::steady_clock::now();
    REQUIRE(terminal.stop());
    const auto elapsed = std::chrono::steady_clock::now() - started;
    CHECK(elapsed < std::chrono::milliseconds(400));
    writer.join();
    CHECK(terminal.modes() == cch::tui::TerminalModeState{});
}

TEST_CASE(
    "Process Terminal backpressures bounded output in write order",
    "[tui][terminal][issue462]") {
    auto pty = cch::tests::open_pseudo_terminal();
    REQUIRE(pty);
    cch::tui::ProcessTerminal terminal({
        .input_fd = pty->slave.get(),
        .output_fd = pty->slave.get(),
    });
    REQUIRE(terminal.start(
        [](std::string) -> cch::support::ExpectedVoid { return {}; },
        [](cch::tui::TerminalDimensions) -> cch::support::ExpectedVoid { return {}; }));
    (void)cch::tests::read_available(pty->master.get());

    // With the master never read, the PTY output buffer fills and writes back
    // up: the terminal falls back to its bounded ordered queue and reports
    // explicit backpressure (`Busy`) instead of blocking the caller. Each
    // chunk is unique so the final concatenation proves write order.
    std::string expected;
    bool saw_busy = false;
    for (std::size_t i = 0; i < 2000; ++i) {
        const auto chunk = std::format(
            "chunk-{:06d}-{};", i, std::string(4000, 'x'));
        auto result = terminal.write(chunk);
        if (!result) {
            CHECK(result.error().code == cch::support::ErrorCode::Busy);
            saw_busy = true;
            break;
        }
        expected += chunk;
    }
    REQUIRE(saw_busy);
    REQUIRE_FALSE(expected.empty());

    // Once the master drains, every admitted byte arrives in write order.
    std::string received;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (received.size() < expected.size() &&
        std::chrono::steady_clock::now() < deadline) {
        received += cch::tests::read_available(
            pty->master.get(), std::chrono::milliseconds(50));
    }
    REQUIRE(received.size() == expected.size());
    CHECK(received == expected);
    REQUIRE(terminal.stop());
}

TEST_CASE(
    "Process Terminal admits a large single write on a draining terminal",
    "[tui][terminal][issue462]") {
    auto pty = cch::tests::open_pseudo_terminal();
    REQUIRE(pty);
    cch::tui::ProcessTerminal terminal({
        .input_fd = pty->slave.get(),
        .output_fd = pty->slave.get(),
    });
    REQUIRE(terminal.start(
        [](std::string) -> cch::support::ExpectedVoid { return {}; },
        [](cch::tui::TerminalDimensions) -> cch::support::ExpectedVoid { return {}; }));
    (void)cch::tests::read_available(pty->master.get());

    // A single write larger than the queue bound (e.g. a large inline image)
    // is admitted when nothing is queued: the fast path writes the writable
    // prefix and the delivery worker drains the remainder in order on the
    // healthy, draining terminal. The bound only caps accumulated backlog.
    const std::string large(1024 * 1024, 'L'); // 1 MiB > the 256 KiB bound
    REQUIRE(terminal.write(large));
    std::string received;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (received.size() < large.size() &&
        std::chrono::steady_clock::now() < deadline) {
        received += cch::tests::read_available(
            pty->master.get(), std::chrono::milliseconds(50));
    }
    REQUIRE(received.size() == large.size());
    CHECK(received == large);
    REQUIRE(terminal.stop());
}

TEST_CASE(
    "Process Terminal restores output descriptor flags on every exit",
    "[tui][terminal][issue462]") {
    auto pty = cch::tests::open_pseudo_terminal();
    REQUIRE(pty);
    cch::tui::ProcessTerminal terminal({
        .input_fd = pty->slave.get(),
        .output_fd = pty->slave.get(),
    });
    const int original_flags = ::fcntl(pty->slave.get(), F_GETFL);
    REQUIRE(original_flags >= 0);

    REQUIRE(terminal.start(
        [](std::string) -> cch::support::ExpectedVoid { return {}; },
        [](cch::tui::TerminalDimensions) -> cch::support::ExpectedVoid { return {}; }));
    const int started_flags = ::fcntl(pty->slave.get(), F_GETFL);
    REQUIRE(started_flags >= 0);
    CHECK((started_flags & O_NONBLOCK) != 0);

    REQUIRE(terminal.stop());
    const int restored_flags = ::fcntl(pty->slave.get(), F_GETFL);
    REQUIRE(restored_flags >= 0);
    CHECK((restored_flags & O_NONBLOCK) == 0);
    CHECK((restored_flags & ~O_NONBLOCK) == (original_flags & ~O_NONBLOCK));
}

TEST_CASE(
    "Process Terminal stops promptly after input EOF without hanging",
    "[tui][terminal][issue462]") {
    auto pty = cch::tests::open_pseudo_terminal();
    REQUIRE(pty);
    cch::tui::ProcessTerminal terminal({
        .input_fd = pty->slave.get(),
        .output_fd = pty->slave.get(),
    });
    REQUIRE(terminal.start(
        [](std::string) -> cch::support::ExpectedVoid { return {}; },
        [](cch::tui::TerminalDimensions) -> cch::support::ExpectedVoid { return {}; }));
    (void)cch::tests::read_available(pty->master.get());

    // Closing the master makes the slave read return EOF (POLLIN|POLLHUP):
    // the delivery worker stops reading input instead of spinning. stop()
    // still returns promptly; restoration writes fail with EIO on the dead
    // terminal, so stop() may report that Process error — the contract under
    // test is prompt, hang-free shutdown.
    REQUIRE(pty->master.close() == 0);
    const auto started = std::chrono::steady_clock::now();
    const auto stopped = terminal.stop();
    const auto elapsed = std::chrono::steady_clock::now() - started;
    CHECK(elapsed < std::chrono::milliseconds(400));
    if (!stopped) {
        CHECK(stopped.error().code == cch::support::ErrorCode::Process);
    }

    // Idempotent and equally prompt.
    const auto second_started = std::chrono::steady_clock::now();
    (void)terminal.stop();
    const auto second_elapsed = std::chrono::steady_clock::now() - second_started;
    CHECK(second_elapsed < std::chrono::milliseconds(400));
}