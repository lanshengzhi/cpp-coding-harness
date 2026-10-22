#include "coding_agent/tui/ClipboardWrite.hpp"

#include "support/EnvVarGuard.hpp"
#include "support/PseudoTerminal.hpp"

#include <cch/tui/Markdown.hpp>
#include <cch/tui/ProcessTerminal.hpp>
#include <cch/tui/TerminalImage.hpp>
#include <cch/tui/Tui.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

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

[[nodiscard]] std::string collect_pty_output(cch::tests::PseudoTerminal& pty) {
    return cch::tests::read_available(pty.master.get());
}

[[nodiscard]] std::vector<std::string> extract_osc8_sequences(std::string_view bytes) {
    std::vector<std::string> sequences;
    std::size_t position = 0;
    while ((position = bytes.find("\x1b]8;", position)) != std::string_view::npos) {
        const auto bel = bytes.find('\x07', position);
        const auto st = bytes.find("\x1b\\", position);
        const auto end = bel == std::string_view::npos ? st : (st == std::string_view::npos ? bel : std::min(bel, st));
        if (end == std::string_view::npos) break;
        const auto terminator = end == bel ? 1U : 2U;
        sequences.emplace_back(bytes.substr(position, end + terminator - position));
        position = end + terminator;
    }
    return sequences;
}

[[nodiscard]] std::string utc_timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
    if (::gmtime_r(&time, &utc) == nullptr) return "unknown";
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    return std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03}Z",
            utc.tm_year + 1900,
            utc.tm_mon + 1,
            utc.tm_mday,
            utc.tm_hour,
            utc.tm_min,
            utc.tm_sec,
            milliseconds);
}

[[nodiscard]] std::string environment_value(const char* name) {
    const auto* value = std::getenv(name);
    return value == nullptr ? "<unset>" : value;
}

} // namespace

TEST_CASE("real PTY records Herdr markdown and OSC 52 terminal bytes",
        "[coding_agent][tui][terminal][issue1124][evidence]") {
    const auto host_term = environment_value("TERM");
    const auto host_term_program = environment_value("TERM_PROGRAM");
    const auto host_colorterm = environment_value("COLORTERM");
    const auto host_term_program_version = environment_value("TERM_PROGRAM_VERSION");
    REQUIRE(host_term_program == "herdr");
    cch::tests::EnvVarGuard display{"DISPLAY", std::string{":0"}};
    cch::tests::EnvVarGuard wayland_display{"WAYLAND_DISPLAY", std::nullopt};
    cch::tests::EnvVarGuard termux_version{"TERMUX_VERSION", std::nullopt};
    cch::tests::EnvVarGuard ssh_connection{"SSH_CONNECTION", std::nullopt};
    cch::tests::EnvVarGuard ssh_client{"SSH_CLIENT", std::nullopt};
    cch::tests::EnvVarGuard mosh_connection{"MOSH_CONNECTION", std::nullopt};
    cch::tests::EnvVarGuard clipboard_osc52{"PI_CLIPBOARD_OSC52"};
    cch::tui::reset_image_capabilities_cache();

    auto pty = cch::tests::open_pseudo_terminal(100, 24);
    REQUIRE(pty);
    cch::tui::ProcessTerminal terminal({.input_fd = pty->slave.get(), .output_fd = pty->slave.get()});
    cch::tui::MainScreen screen{terminal};
    REQUIRE(screen.add_child(std::make_unique<cch::tui::Markdown>("See [the notes](https://example.com/notes)")));
    REQUIRE(screen.start());
    const auto startup_bytes = collect_pty_output(*pty);
    REQUIRE(screen.render());
    const auto markdown_frame = collect_pty_output(*pty);
    const auto capabilities = terminal.capabilities();
    CHECK(capabilities.hyperlinks);
    const auto markdown_bytes = startup_bytes + markdown_frame;
    const auto osc8_sequences = extract_osc8_sequences(markdown_bytes);
    CHECK(markdown_bytes.find("\x1b]8;;https://example.com/notes\x1b\\") != std::string::npos);
    CHECK(markdown_bytes.find("\x1b]8;;\x1b\\") != std::string::npos);
    REQUIRE(osc8_sequences.size() >= 2);

    clipboard_osc52.set("1");
    auto clipboard_environment = cch::coding_agent::tui::clipboard_environment_from_process();
    clipboard_environment.search_path.clear();
    std::vector<std::string> opted_in_sequences;
    cch::coding_agent::tui::ClipboardWriteOptions opted_in{
            .environment = std::move(clipboard_environment),
            .write_osc52 = [&terminal, &opted_in_sequences](std::string_view sequence) {
                opted_in_sequences.emplace_back(sequence);
                (void)terminal.write(sequence);
            },
    };
    const auto opted_in_result = cch::coding_agent::tui::write_clipboard_text("pty clipboard evidence", opted_in);
    const auto opted_in_bytes = collect_pty_output(*pty);
    REQUIRE(opted_in_result.copied);
    REQUIRE(opted_in_sequences.size() == 1);
    CHECK(opted_in_bytes.find("\x1b]52;c") == 0);

    clipboard_osc52.unset();
    auto local_environment = cch::coding_agent::tui::clipboard_environment_from_process();
    local_environment.search_path.clear();
    std::vector<std::string> local_sequences;
    cch::coding_agent::tui::ClipboardWriteOptions local{
            .environment = std::move(local_environment),
            .write_osc52 = [&terminal, &local_sequences](std::string_view sequence) {
                local_sequences.emplace_back(sequence);
                (void)terminal.write(sequence);
            },
    };
    const auto local_result = cch::coding_agent::tui::write_clipboard_text("pty clipboard evidence", local);
    const auto local_bytes = collect_pty_output(*pty);
    CHECK_FALSE(local_result.copied);
    CHECK(local_sequences.empty());
    CHECK(local_bytes.find("\x1b]52;") == std::string::npos);

    const char* capture_path = std::getenv("CCH_AUDIT_1124_CAPTURE_PATH");
    if (capture_path != nullptr && *capture_path != '\0') {
        std::ofstream capture(capture_path, std::ios::binary | std::ios::trunc);
        REQUIRE(capture);
        capture << "evidence=dated real-PTY capture from this host; not an emulator screenshot\n";
        capture << "captured_at_utc=" << utc_timestamp() << '\n';
        capture << "host_TERM=" << host_term << '\n';
        capture << "host_TERM_PROGRAM=" << host_term_program << '\n';
        capture << "host_COLORTERM=" << host_colorterm << '\n';
        capture << "host_TERM_PROGRAM_VERSION=" << host_term_program_version << '\n';
        capture << "pty_inherited_TERM=" << host_term << '\n';
        capture << "pty_inherited_TERM_PROGRAM=" << host_term_program << '\n';
        capture << "pty_inherited_COLORTERM=" << host_colorterm << '\n';
        capture << "hyperlinks=" << (capabilities.hyperlinks ? "true" : "false") << '\n';
        capture << "inline_images=" << static_cast<int>(capabilities.inline_images) << '\n';
        capture << "color_capability=" << static_cast<int>(capabilities.color) << '\n';
        capture << "synchronized_output=" << (capabilities.synchronized_output ? "true" : "false") << '\n';
        capture << "markdown_bytes_hex=" << hex_bytes(markdown_bytes) << '\n';
        capture << "osc8_sequences_hex=";
        for (std::size_t index = 0; index < osc8_sequences.size(); ++index) {
            if (index != 0) capture << ',';
            capture << hex_bytes(osc8_sequences[index]);
        }
        capture << '\n';
        capture << "osc52_opted_in_hex=" << hex_bytes(opted_in_bytes) << '\n';
        capture << "osc52_without_opt_in_hex=" << hex_bytes(local_bytes) << '\n';
        capture << "osc52_sequence_hex=" << (opted_in_sequences.empty() ? "" : hex_bytes(opted_in_sequences.front()))
                << '\n';
        capture << "osc52_without_opt_in_sequence_count=" << local_sequences.size() << '\n';
        capture << "limitation=PTY records emitted bytes; no observation verifies hyperlink rendering or clipboard write\n";
        capture << "pty_capture_scope=the PTY run validates this environment's capability branch; the PTY itself is not the Herdr host emulator\n";
    }

    REQUIRE(screen.stop());
    cch::tui::reset_image_capabilities_cache();
}
