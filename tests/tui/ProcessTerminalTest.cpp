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
