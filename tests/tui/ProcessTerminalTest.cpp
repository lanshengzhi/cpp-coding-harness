#include <cch/tui/ProcessTerminal.hpp>
#include <cch/tui/Tui.hpp>

#include "harness/UniqueFd.hpp"

#include "../../third_party/catch2/catch_test_macros.hpp"

#if defined(__linux__) || defined(__APPLE__)
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
    [[nodiscard]] cch::util::Expected<cch::tui::RenderResult> render(std::size_t) override {
        return cch::tui::RenderResult{.lines = {"shell"}};
    }

    void invalidate() override {
        ++invalidations;
    }

    void handle_input(const cch::tui::InputEventVariant& event) override {
        const auto* key = std::get_if<cch::tui::KeyEvent>(&event);
        if (key == nullptr || key->key != "q") return;
        ++inputs;
        if (tui != nullptr) stop_succeeded = tui->stop().has_value();
    }

    [[nodiscard]] bool accepts_key_releases() const override {
        return false;
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

struct PtyPair {
    cch::harness::UniqueFd master;
    cch::harness::UniqueFd slave;
    std::string slave_name;
};

[[nodiscard]] std::optional<PtyPair> open_pty(std::size_t columns = 80, std::size_t rows = 24) {
    cch::harness::UniqueFd master(::posix_openpt(O_RDWR | O_NOCTTY));
    if (!master || ::grantpt(master.get()) != 0 || ::unlockpt(master.get()) != 0) return std::nullopt;
    const auto* slave_name = ::ptsname(master.get());
    if (slave_name == nullptr) return std::nullopt;
    cch::harness::UniqueFd slave(::open(slave_name, O_RDWR | O_NOCTTY));
    if (!slave) return std::nullopt;

    winsize dimensions{
        .ws_row = static_cast<unsigned short>(rows),
        .ws_col = static_cast<unsigned short>(columns),
        .ws_xpixel = 0,
        .ws_ypixel = 0,
    };
    if (::ioctl(slave.get(), TIOCSWINSZ, &dimensions) != 0) return std::nullopt;
    return PtyPair{
        .master = std::move(master),
        .slave = std::move(slave),
        .slave_name = slave_name,
    };
}

[[nodiscard]] bool same_terminal_state(const termios& left, const termios& right) {
    return left.c_iflag == right.c_iflag && left.c_oflag == right.c_oflag &&
        left.c_cflag == right.c_cflag && left.c_lflag == right.c_lflag &&
        ::cfgetispeed(&left) == ::cfgetispeed(&right) &&
        ::cfgetospeed(&left) == ::cfgetospeed(&right) &&
        std::memcmp(left.c_cc, right.c_cc, NCCS) == 0;
}

template <typename Predicate>
[[nodiscard]] bool wait_until(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return predicate();
}

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

[[nodiscard]] std::string read_available(
    int descriptor,
    std::chrono::milliseconds timeout = std::chrono::milliseconds(100)) {
    std::string output;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::array<char, 4096> buffer{};
    while (std::chrono::steady_clock::now() < deadline) {
        pollfd item{.fd = descriptor, .events = POLLIN, .revents = 0};
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        const auto ready = ::poll(&item, 1, static_cast<int>(remaining.count()));
        if (ready <= 0 || (item.revents & POLLIN) == 0) break;
        const auto count = ::read(descriptor, buffer.data(), buffer.size());
        if (count <= 0) break;
        output.append(buffer.data(), static_cast<std::size_t>(count));
    }
    return output;
}

} // namespace

TEST_CASE("Process Terminal rejects non-TTY descriptors before changing modes", "[tui][terminal][issue54]") {
    std::array<int, 2> raw_descriptors{};
    REQUIRE(::pipe(raw_descriptors.data()) == 0);
    cch::harness::UniqueFd input(raw_descriptors[0]);
    cch::harness::UniqueFd output(raw_descriptors[1]);

    cch::tui::ProcessTerminal terminal({
        .input_fd = input.get(),
        .output_fd = output.get(),
    });
    const auto result = terminal.start([](std::string) {}, [](cch::tui::TerminalDimensions) {});

    REQUIRE_FALSE(result);
    CHECK(result.error().code == cch::util::ErrorCode::Validation);
    CHECK(result.error().message == "Process Terminal requires TTY input and output descriptors");
    CHECK(result.error().detail.size() < 256);
    CHECK(terminal.modes() == cch::tui::TerminalModeState{});
}

TEST_CASE("Process Terminal restores raw paste cursor and pending render modes", "[tui][terminal][issue54]") {
    auto pty = open_pty();
    REQUIRE(pty);
    termios original{};
    REQUIRE(::tcgetattr(pty->slave.get(), &original) == 0);

    cch::tui::ProcessTerminal terminal({
        .input_fd = pty->slave.get(),
        .output_fd = pty->slave.get(),
    });
    REQUIRE(terminal.start([](std::string) {}, [](cch::tui::TerminalDimensions) {}));

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
    CHECK(read_available(pty->master.get()).find("\x1b[?2004h") != std::string::npos);

    REQUIRE(terminal.set_cursor_visible(false));
    REQUIRE(terminal.begin_synchronized_update());
    REQUIRE(terminal.stop());
    REQUIRE(terminal.stop());

    termios restored{};
    REQUIRE(::tcgetattr(pty->slave.get(), &restored) == 0);
    CHECK(same_terminal_state(restored, original));
    CHECK(terminal.modes() == cch::tui::TerminalModeState{});
    const auto restoration = read_available(pty->master.get());
    CHECK(restoration.find("\x1b[?2026l") != std::string::npos);
    CHECK(restoration.find("\x1b[?25h") != std::string::npos);
    CHECK(restoration.find("\x1b[?2004l") != std::string::npos);
}

TEST_CASE("Process Terminal reports synchronized output conservatively", "[tui][terminal][issue54]") {
    auto pty = open_pty();
    REQUIRE(pty);
    ScopedEnvironmentVariable terminal_environment("TERM");
    ScopedEnvironmentVariable program_environment("TERM_PROGRAM");
    program_environment.unset();

    terminal_environment.set("xterm-unknown");
    cch::tui::ProcessTerminal generic_terminal({
        .input_fd = pty->slave.get(),
        .output_fd = pty->slave.get(),
    });
    REQUIRE(generic_terminal.start([](std::string) {}, [](cch::tui::TerminalDimensions) {}));
    CHECK_FALSE(generic_terminal.capabilities().synchronized_output);
    REQUIRE(generic_terminal.stop());
    (void)read_available(pty->master.get());

    terminal_environment.set("football");
    REQUIRE(generic_terminal.start([](std::string) {}, [](cch::tui::TerminalDimensions) {}));
    CHECK_FALSE(generic_terminal.capabilities().synchronized_output);
    REQUIRE(generic_terminal.stop());
    (void)read_available(pty->master.get());

    terminal_environment.set("xterm-unknown");
    program_environment.set("WezTerm");
    REQUIRE(generic_terminal.start([](std::string) {}, [](cch::tui::TerminalDimensions) {}));
    CHECK(generic_terminal.capabilities().synchronized_output);
    REQUIRE(generic_terminal.stop());
    (void)read_available(pty->master.get());

    terminal_environment.set("xterm-kitty");
    program_environment.unset();
    REQUIRE(generic_terminal.start([](std::string) {}, [](cch::tui::TerminalDimensions) {}));
    CHECK(generic_terminal.capabilities().synchronized_output);
    REQUIRE(generic_terminal.stop());
}

TEST_CASE("Process Terminal delivers pseudo-terminal input and resize", "[tui][terminal][issue54]") {
    auto pty = open_pty();
    REQUIRE(pty);
    std::mutex events_mutex;
    std::vector<std::string> inputs;
    std::vector<cch::tui::TerminalDimensions> resizes;

    cch::tui::ProcessTerminal terminal({
        .input_fd = pty->slave.get(),
        .output_fd = pty->slave.get(),
    });
    REQUIRE(terminal.start(
        [&](std::string input) {
            std::lock_guard lock(events_mutex);
            inputs.push_back(std::move(input));
        },
        [&](cch::tui::TerminalDimensions dimensions) {
            std::lock_guard lock(events_mutex);
            resizes.push_back(dimensions);
        }));
    const auto startup_output = read_available(pty->master.get());
    CHECK(startup_output.find("\x1b[>7u\x1b[?u\x1b[c") != std::string::npos);

    constexpr std::string_view kKittyResponse = "\x1b[?7u";
    REQUIRE(::write(pty->master.get(), kKittyResponse.data(), kKittyResponse.size()) ==
        static_cast<ssize_t>(kKittyResponse.size()));
    REQUIRE(wait_until([&] {
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

    REQUIRE(wait_until([&] {
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

TEST_CASE("Concurrent external and sink stops restore without deadlock", "[tui][terminal][issue54]") {
    auto pty = open_pty();
    REQUIRE(pty);
    termios original{};
    REQUIRE(::tcgetattr(pty->slave.get(), &original) == 0);
    std::atomic<bool> callback_started{false};
    std::atomic<bool> external_stop_started{false};
    std::atomic<bool> callback_stop_succeeded{false};
    cch::util::ExpectedVoid external_result;

    cch::tui::ProcessTerminal terminal({
        .input_fd = pty->slave.get(),
        .output_fd = pty->slave.get(),
    });
    REQUIRE(terminal.start(
        [&](std::string) {
            callback_started = true;
            while (!external_stop_started.load()) std::this_thread::yield();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            callback_stop_succeeded = terminal.stop().has_value();
        },
        [](cch::tui::TerminalDimensions) {}));
    (void)read_available(pty->master.get());
    REQUIRE(::write(pty->master.get(), "q", 1) == 1);
    REQUIRE(wait_until([&] { return callback_started.load(); }));

    std::jthread external_stop([&] {
        external_stop_started = true;
        external_result = terminal.stop();
    });
    external_stop.join();

    CHECK(callback_stop_succeeded.load());
    REQUIRE(external_result);
    termios restored{};
    REQUIRE(::tcgetattr(pty->slave.get(), &restored) == 0);
    CHECK(same_terminal_state(restored, original));
    REQUIRE(terminal.stop());
}

TEST_CASE("Process Terminal reports callback and restoration failures together", "[tui][terminal][issue54]") {
    auto pty = open_pty();
    REQUIRE(pty);
    cch::harness::UniqueFd output(::dup(pty->slave.get()));
    REQUIRE(output);
    const auto output_descriptor = output.get();
    std::atomic<bool> callback_failed{false};

    cch::tui::ProcessTerminal terminal({
        .input_fd = pty->slave.get(),
        .output_fd = output_descriptor,
    });
    REQUIRE(terminal.start(
        [&](std::string) {
            callback_failed = true;
            throw ExpectedUnwind{};
        },
        [](cch::tui::TerminalDimensions) {}));
    (void)read_available(pty->master.get());
    REQUIRE(::write(pty->master.get(), "q", 1) == 1);
    REQUIRE(wait_until([&] { return callback_failed.load(); }));
    REQUIRE(output.close() == 0);

    const auto failed_stop = terminal.stop();
    REQUIRE_FALSE(failed_stop);
    CHECK(failed_stop.error().message == "Process Terminal restoration encountered multiple failures");
    CHECK(failed_stop.error().detail.find("Process Terminal input sink failed") != std::string::npos);
    CHECK(failed_stop.error().detail.find("could not write terminal output") != std::string::npos);

    cch::harness::UniqueFd replacement(::open(pty->slave_name.c_str(), O_RDWR | O_NOCTTY));
    REQUIRE(replacement);
    REQUIRE(replacement.get() == output_descriptor);
    REQUIRE(terminal.stop());
    CHECK(terminal.modes() == cch::tui::TerminalModeState{});
}

TEST_CASE("Process Terminal enables and restores the keyboard fallback", "[tui][terminal][issue54]") {
    auto pty = open_pty();
    REQUIRE(pty);
    cch::tui::ProcessTerminal terminal({
        .input_fd = pty->slave.get(),
        .output_fd = pty->slave.get(),
    });
    REQUIRE(terminal.start([](std::string) {}, [](cch::tui::TerminalDimensions) {}));
    (void)read_available(pty->master.get());

    constexpr std::string_view kResponseStart = "\x1b[?";
    constexpr std::string_view kResponseEnd = "1;2c";
    REQUIRE(::write(pty->master.get(), kResponseStart.data(), kResponseStart.size()) ==
        static_cast<ssize_t>(kResponseStart.size()));
    REQUIRE(::write(pty->master.get(), kResponseEnd.data(), kResponseEnd.size()) ==
        static_cast<ssize_t>(kResponseEnd.size()));
    REQUIRE(wait_until([&] {
        return terminal.capabilities().keyboard_protocol ==
            cch::tui::KeyboardProtocol::ModifyOtherKeys;
    }));
    CHECK(read_available(pty->master.get()).find("\x1b[>4;2m") != std::string::npos);

    REQUIRE(terminal.stop());
    const auto restoration = read_available(pty->master.get());
    CHECK(restoration.find("\x1b[>4;0m") != std::string::npos);
    CHECK(restoration.find("\x1b[<u") != std::string::npos);
}

TEST_CASE("Process Terminal runs a minimal TUI shell and restores during unwinding", "[tui][terminal][issue54]") {
    auto pty = open_pty();
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
        REQUIRE(wait_until([&] { return shell->invalidations.load() >= 1; }));
        REQUIRE(tui.render());
        tui.invalidate();
        REQUIRE(::write(pty->master.get(), "q", 1) == 1);
        REQUIRE(wait_until([&] {
            return shell->inputs.load() == 1 && shell->stop_succeeded.load();
        }));
        throw ExpectedUnwind{};
    } catch (const ExpectedUnwind&) {
    }

    termios restored{};
    REQUIRE(::tcgetattr(pty->slave.get(), &restored) == 0);
    CHECK(same_terminal_state(restored, original));
    const auto output = read_available(pty->master.get());
    CHECK(output.find("shell") != std::string::npos);
    CHECK(output.find("\x1b[?25l") != std::string::npos);
    CHECK(output.find("\x1b[?25h") != std::string::npos);
    CHECK(output.find("\x1b[?2004l") != std::string::npos);
    CHECK(output.find("\x1b[<u") != std::string::npos);
}

TEST_CASE("Process Terminal rolls back raw input after partial startup failure", "[tui][terminal][issue54]") {
    auto pty = open_pty();
    REQUIRE(pty);
    cch::harness::UniqueFd read_only_output(::open(pty->slave_name.c_str(), O_RDONLY | O_NOCTTY));
    REQUIRE(read_only_output);
    termios original{};
    REQUIRE(::tcgetattr(pty->slave.get(), &original) == 0);

    cch::tui::ProcessTerminal terminal({
        .input_fd = pty->slave.get(),
        .output_fd = read_only_output.get(),
    });
    const auto result = terminal.start([](std::string) {}, [](cch::tui::TerminalDimensions) {});

    REQUIRE_FALSE(result);
    CHECK(result.error().code == cch::util::ErrorCode::Process);
    termios restored{};
    REQUIRE(::tcgetattr(pty->slave.get(), &restored) == 0);
    CHECK(same_terminal_state(restored, original));
    CHECK(terminal.modes() == cch::tui::TerminalModeState{});
}

#else

TEST_CASE("Process Terminal reports unsupported platforms without acquisition", "[tui][terminal][issue54]") {
    cch::tui::ProcessTerminal terminal;
    const auto result = terminal.start([](std::string) {}, [](cch::tui::TerminalDimensions) {});

    REQUIRE_FALSE(result);
    CHECK(result.error().message == "Process Terminal is unsupported on this platform");
    CHECK(result.error().detail.size() < 256);
    CHECK(terminal.modes() == cch::tui::TerminalModeState{});
}

#endif

#if defined(__linux__) || defined(__APPLE__)
TEST_CASE(
    "Process Terminal reports conservative color and appearance observations",
    "[tui][terminal][theme][issue55]") {
    auto pty = open_pty();
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
    REQUIRE(terminal.start([](std::string) {}, [](cch::tui::TerminalDimensions) {}));
    CHECK(std::chrono::steady_clock::now() - probe_started < std::chrono::milliseconds(500));
    CHECK(terminal.capabilities().color == cch::tui::TerminalColorCapability::Xterm256);
    CHECK(terminal.capabilities().appearance == cch::tui::TerminalAppearance::Unknown);
    REQUIRE(terminal.stop());
    (void)read_available(pty->master.get());

    color_terminal_environment.set("24BIT");
    foreground_background_environment.set("0; +15ignored ");
    REQUIRE(terminal.start([](std::string) {}, [](cch::tui::TerminalDimensions) {}));
    CHECK(terminal.capabilities().color == cch::tui::TerminalColorCapability::TrueColor);
    CHECK(terminal.capabilities().appearance == cch::tui::TerminalAppearance::Light);
    REQUIRE(terminal.stop());
    (void)read_available(pty->master.get());

    color_terminal_environment.unset();
    foreground_background_environment.set("15;0");
    terminal_environment.set("tmux-256color");
    program_environment.set("WezTerm");
    REQUIRE(terminal.start([](std::string) {}, [](cch::tui::TerminalDimensions) {}));
    CHECK(terminal.capabilities().color == cch::tui::TerminalColorCapability::Xterm256);
    CHECK(terminal.capabilities().appearance == cch::tui::TerminalAppearance::Dark);
    REQUIRE(terminal.stop());
    (void)read_available(pty->master.get());

    terminal_environment.set("xterm-unknown");
    REQUIRE(terminal.start([](std::string) {}, [](cch::tui::TerminalDimensions) {}));
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
        auto pty = open_pty();
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
            [&](std::string input) {
                if (input.find("typed") != std::string::npos) delivered_input = true;
            },
            [](cch::tui::TerminalDimensions) {}));
        responder.join();
        REQUIRE(answered.load());
        CHECK(terminal.capabilities().appearance == probe_case.expected);
        if (probe_case.expects_input) {
            CHECK(wait_until([&] { return delivered_input.load(); }));
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
    auto pty = open_pty();
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
        [&](std::string input) {
            std::lock_guard lock(input_mutex);
            delivered += input;
        },
        [](cch::tui::TerminalDimensions) {}));
    (void)read_available(pty->master.get());
    CHECK(terminal.capabilities().appearance == cch::tui::TerminalAppearance::Dark);

    constexpr std::string_view kMalformedStart = "\x1b]11;not-a-color";
    constexpr std::string_view kMalformedEnd = "\x07typed\x1b[?997;9n";
    REQUIRE(::write(pty->master.get(), kMalformedStart.data(), kMalformedStart.size()) ==
        static_cast<ssize_t>(kMalformedStart.size()));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    REQUIRE(::write(pty->master.get(), kMalformedEnd.data(), kMalformedEnd.size()) ==
        static_cast<ssize_t>(kMalformedEnd.size()));
    REQUIRE(wait_until([&] {
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
    REQUIRE(wait_until([&] {
        std::lock_guard lock(input_mutex);
        return delivered.find("late") != std::string::npos;
    }));
    REQUIRE(wait_until([&] {
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
    REQUIRE(wait_until([&] {
        std::lock_guard lock(input_mutex);
        return delivered.find(kAfterUnterminated) != std::string::npos;
    }));
    {
        std::lock_guard lock(input_mutex);
        CHECK(delivered.find("11;unterminated") == std::string::npos);
    }

    constexpr char kEscape = '\x1b';
    REQUIRE(::write(pty->master.get(), &kEscape, 1) == 1);
    REQUIRE(wait_until([&] {
        std::lock_guard lock(input_mutex);
        return delivered.find(kEscape) != std::string::npos;
    }));
    REQUIRE(terminal.stop());
}
#endif
