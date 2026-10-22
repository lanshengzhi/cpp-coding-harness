#include <cch/tui/AltScreen.hpp>
#include <cch/tui/Input.hpp>
#include <cch/tui/ProcessTerminal.hpp>
#include <cch/tui/Text.hpp>
#include <cch/tui/Tui.hpp>

#include "support/ImageEnvironmentGuard.hpp"
#include "support/IoContextRunner.hpp"
#include "support/PseudoTerminal.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <format>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>

namespace {

cch::tests::IoContextRunner& test_io() {
    static cch::tests::IoContextRunner runner;
    return runner;
}

[[nodiscard]] std::string hex_bytes(std::string_view bytes) {
    constexpr char kHex[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (const unsigned char byte : bytes) {
        result.push_back(kHex[byte >> 4]);
        result.push_back(kHex[byte & 0x0f]);
    }
    return result;
}

[[nodiscard]] std::string utc_timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
    if (::gmtime_r(&time, &utc) == nullptr) return "unknown";
    const auto milliseconds =
            std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    return std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03}Z",
            utc.tm_year + 1900,
            utc.tm_mon + 1,
            utc.tm_mday,
            utc.tm_hour,
            utc.tm_min,
            utc.tm_sec,
            milliseconds);
}

} // namespace

TEST_CASE("real PTY records hardware cursor positioning and visibility restoration",
        "[tui][cursor-marker][issue1120][evidence]") {
    cch::tests::ImageEnvironmentGuard image_environment;
    auto pty = cch::tests::open_pseudo_terminal(80, 24);
    REQUIRE(pty);

    cch::tui::ProcessTerminal terminal(
            {.input_fd = pty->slave.get(), .output_fd = pty->slave.get(), .executor = test_io().io.get_executor()});
    cch::tui::MainScreen screen{terminal, true};
    auto input = std::make_unique<cch::tui::Input>();
    input->set_value("abc");
    input->move_cursor_to_end();
    REQUIRE(screen.add_child(std::move(input)));
    REQUIRE(screen.add_child(std::make_unique<cch::tui::Text>("below", 0, 0)));

    REQUIRE(screen.start());
    const auto startup_bytes = cch::tests::read_available(pty->master.get());
    REQUIRE(screen.render());
    const auto first_render_bytes = cch::tests::read_available(pty->master.get());
    const auto session_bytes = startup_bytes + first_render_bytes;
    const auto column_position = session_bytes.find("\x1b[6G");
    REQUIRE(column_position != std::string::npos);
    const auto vertical_sequence = std::string_view{"\x1b[1A"};
    const auto vertical_position = session_bytes.find(vertical_sequence);
    REQUIRE(vertical_position != std::string::npos);
    CHECK(vertical_position < column_position);
    CHECK(session_bytes.find("\x1b[?25l") != std::string::npos);
    const auto show_cursor = session_bytes.find("\x1b[?25h");
    REQUIRE(show_cursor != std::string::npos);
    CHECK(column_position < show_cursor);
    CHECK(session_bytes.find("\x1b[7m") == std::string::npos);
    CHECK(session_bytes.find("\x1b[27m") == std::string::npos);
    CHECK(session_bytes.find("\x1b_pi:fc\x07") == std::string::npos);

    REQUIRE(screen.stop());
    const auto restoration_bytes = cch::tests::read_available(pty->master.get());
    CHECK(restoration_bytes.find("\x1b[2B") != std::string::npos);
    CHECK(restoration_bytes.find("\r\n") != std::string::npos);
    CHECK(restoration_bytes.find("\x1b[?25h") != std::string::npos);
    CHECK(terminal.modes().cursor_visible);
    CHECK_FALSE(terminal.modes().started);

    cch::tui::AltScreen fullscreen{terminal, true};
    auto fullscreen_input = std::make_unique<cch::tui::Input>();
    fullscreen_input->set_value("abc");
    fullscreen_input->move_cursor_to_end();
    REQUIRE(fullscreen.add_child(std::move(fullscreen_input)));
    REQUIRE(fullscreen.start());
    REQUIRE(fullscreen.render());
    const auto fullscreen_session_bytes = cch::tests::read_available(pty->master.get());
    CHECK(fullscreen_session_bytes.find("\x1b[?1049h") != std::string::npos);
    CHECK(fullscreen_session_bytes.find("\x1b[1;6H") != std::string::npos);
    CHECK(fullscreen_session_bytes.find("\x1b[?25h") != std::string::npos);
    REQUIRE(fullscreen.stop());
    const auto fullscreen_restoration_bytes = cch::tests::read_available(pty->master.get());
    CHECK(fullscreen_restoration_bytes.find("\x1b[?25h") != std::string::npos);
    CHECK(fullscreen_restoration_bytes.find("\x1b[?1049l") != std::string::npos);
    CHECK(terminal.modes().cursor_visible);
    CHECK_FALSE(terminal.modes().started);

    const char* capture_path = std::getenv("CCH_AUDIT_1120_CAPTURE_PATH");
    if (capture_path != nullptr && *capture_path != '\0') {
        std::ofstream capture(capture_path, std::ios::binary | std::ios::trunc);
        REQUIRE(capture);
        capture << "captured_at_utc=" << utc_timestamp() << '\n';
        capture << "pty_size=80x24\n";
        capture << "main_screen_hardware_cursor=true\n";
        capture << "focused_component_row=0\n";
        capture << "session_bytes_hex=" << hex_bytes(session_bytes) << '\n';
        capture << "relative_vertical_move_hex=" << hex_bytes(vertical_sequence) << '\n';
        capture << "cha_column_hex=" << hex_bytes(std::string_view{session_bytes}.substr(column_position, 4)) << '\n';
        capture << "restoration_bytes_hex=" << hex_bytes(restoration_bytes) << '\n';
        capture << "fullscreen_session_bytes_hex=" << hex_bytes(fullscreen_session_bytes) << '\n';
        capture << "fullscreen_restoration_bytes_hex=" << hex_bytes(fullscreen_restoration_bytes) << '\n';
        capture << "limitation=PTY records cursor control bytes and mode restoration; it does not visually emulate a "
                   "terminal\n";
    }
}
