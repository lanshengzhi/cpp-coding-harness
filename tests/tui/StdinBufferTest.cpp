#include <cch/tui/Editor.hpp>
#include <cch/tui/Input.hpp>
#include <cch/tui/ProcessTerminal.hpp>
#include <cch/tui/StdinBuffer.hpp>
#include <cch/tui/Tui.hpp>

#include "support/ImageEnvironmentGuard.hpp"
#include "support/PseudoTerminal.hpp"

#include "tui/InputDecoder.hpp"

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace cch;

namespace {

struct IoContextRunner {
    boost::asio::io_context io;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> guard{io.get_executor()};
    std::jthread worker{[this] { io.run(); }};

    ~IoContextRunner() {
        guard.reset();
        io.stop();
    }
};

IoContextRunner& issue949_io() {
    static IoContextRunner runner;
    return runner;
}

constexpr std::string_view kIssue954PasteSegment = "\xce\xb1\tprotocol \x1b[?997;1n \x1b[A\r\n";

[[nodiscard]] std::string issue954_large_paste_body() {
    std::string body;
    body.reserve(1024 * 1024 + kIssue954PasteSegment.size() * 100);
    while (body.size() <= 1024 * 1024)
        body += kIssue954PasteSegment;
    body += "paste-suffix-954";
    return body;
}

} // namespace

TEST_CASE("stream decoder keeps ESC[M !! as one legacy mouse frame with no printable leak",
        "[tui][decoder][issue949][spec]") {
    tui::detail::TerminalStreamDecoder decoder;
    const std::string packet = "\x1b[M !!";
    std::string forwarded;
    std::string keys;
    for (const unsigned char byte : packet) {
        const auto result = decoder.feed(std::string_view(reinterpret_cast<const char*>(&byte), 1));
        forwarded += result.forwarded_input;
        for (const auto& event : result.events) {
            if (const auto* key = std::get_if<tui::KeyEvent>(&event)) keys += key->key;
        }
    }

    // Distinguishing expectation from frozen pi StdinBuffer: one complete 6-byte frame.
    // Printable payload bytes must not become Input/Editor keystrokes.
    CHECK(keys.find(' ') == std::string::npos);
    CHECK(keys.find('!') == std::string::npos);
    CHECK(keys.find("space") == std::string::npos);
    CHECK_FALSE(decoder.holds_fragment());
    // Current incomplete CSI-M framing leaks space/! into forwarded_input as separate chars.
    CHECK(forwarded.find(' ') == std::string::npos);
    CHECK(forwarded.find('!') == std::string::npos);
}

TEST_CASE(
        "StdinBuffer preserves a large active paste through fragment flushes", "[tui][stdin-buffer][issue954][spec]") {
    tui::StdinBuffer buffer;
    std::vector<std::string> pastes;
    std::vector<std::string> data;
    buffer.set_paste_handler([&pastes](std::string text) { pastes.push_back(std::move(text)); });
    buffer.set_data_handler([&data](std::string text) { data.push_back(std::move(text)); });

    const auto body = issue954_large_paste_body();
    buffer.process("\x1b[200~");
    for (std::size_t offset = 0; offset < body.size(); offset += 4093) {
        buffer.process(std::string_view(body).substr(offset, 4093));
        if (offset == 0 || offset == body.size() / 2) CHECK(buffer.flush().empty());
        CHECK(pastes.empty());
    }

    buffer.process("\x1b[201~q");
    REQUIRE(pastes.size() == 1);
    CHECK(pastes.front() == body);
    REQUIRE(data == std::vector<std::string>{"q"});
}

TEST_CASE("StdinBuffer emits one complete ESC[M !! frame across every byte boundary",
        "[tui][stdin-buffer][issue949][spec]") {
    tui::StdinBuffer buffer;
    std::vector<std::string> sequences;
    buffer.set_data_handler([&](std::string sequence) { sequences.push_back(std::move(sequence)); });

    const std::string packet = "\x1b[M !!";
    for (const unsigned char byte : packet) {
        buffer.process(std::string_view(reinterpret_cast<const char*>(&byte), 1));
        if (sequences.empty()) {
            CHECK(buffer.get_buffer() == packet.substr(0, buffer.get_buffer().size()));
        }
    }

    REQUIRE(sequences.size() == 1);
    CHECK(sequences.front() == packet);
    CHECK(buffer.get_buffer().empty());
}

TEST_CASE("StdinBuffer preserves interleaved CSI/OSC/DCS/UTF-8 order and ESC deadline selection",
        "[tui][stdin-buffer][issue949][spec]") {
    tui::StdinBuffer buffer(
            {.timeout = std::chrono::milliseconds{50}, .escape_timeout = std::chrono::milliseconds{10}});
    std::vector<std::string> sequences;
    buffer.set_data_handler([&](std::string sequence) { sequences.push_back(std::move(sequence)); });

    buffer.process("\x1b[");
    CHECK(sequences.empty());
    buffer.process("A");
    REQUIRE(sequences == std::vector<std::string>{"\x1b[A"});

    buffer.process("世");
    REQUIRE(sequences == (std::vector<std::string>{"\x1b[A", "世"}));

    buffer.process("\x1b]");
    CHECK(buffer.holds_fragment());
    CHECK(buffer.selected_timeout() == std::chrono::milliseconds{50});
    buffer.process("11;rgb:11/22/33\x07");
    REQUIRE(sequences.back() == "\x1b]11;rgb:11/22/33\x07");

    buffer.process("\x1bP");
    buffer.process(">|x\x1b\\");
    REQUIRE(sequences.back() == "\x1bP>|x\x1b\\");

    buffer.process("\x1b");
    CHECK(buffer.holds_lone_escape());
    CHECK(buffer.selected_timeout() == std::chrono::milliseconds{10});

    tui::StdinBuffer incomplete({.timeout = std::chrono::milliseconds{10}});
    std::vector<std::string> incomplete_sequences;
    incomplete.set_data_handler([&](std::string sequence) { incomplete_sequences.push_back(std::move(sequence)); });
    incomplete.process("\x1b[<35");
    CHECK(incomplete.get_buffer() == "\x1b[<35");
    CHECK(incomplete.selected_timeout() == std::chrono::milliseconds{10});
    const auto flushed_incomplete = incomplete.flush();
    REQUIRE(flushed_incomplete == std::vector<std::string>{"\x1b[<35"});
    REQUIRE(incomplete_sequences == flushed_incomplete);

    const auto flushed_escape = buffer.flush();
    REQUIRE(flushed_escape == std::vector<std::string>{"\x1b"});
    REQUIRE(sequences.back() == "\x1b");
}

TEST_CASE("ProcessTerminal PTY path keeps split ESC[M !! out of Input and Editor text",
        "[tui][terminal][issue949][spec]") {
    auto pty = cch::tests::open_pseudo_terminal();
    REQUIRE(pty);
    cch::tests::ImageEnvironmentGuard environment;

    cch::tui::ProcessTerminal terminal(
            {.input_fd = pty->slave.get(), .output_fd = pty->slave.get(), .executor = issue949_io().io.get_executor()});
    cch::tui::Tui tui(terminal);

    auto input = std::make_unique<tui::Input>();
    auto* input_pointer = input.get();
    REQUIRE(tui.add_child(std::move(input)));
    REQUIRE(tui.set_focus(input_pointer));
    REQUIRE(tui.start());
    (void)cch::tests::read_available(pty->master.get());

    const std::string packet = "\x1b[M !!";
    for (const unsigned char byte : packet) {
        REQUIRE(::write(pty->master.get(), &byte, 1) == 1);
    }
    REQUIRE(::write(pty->master.get(), "Z", 1) == 1);
    REQUIRE(cch::tests::wait_until([&] { return input_pointer->value() == "Z"; }));
    CHECK(input_pointer->value().find(' ') == std::string::npos);
    CHECK(input_pointer->value().find('!') == std::string::npos);

    auto editor = std::make_unique<tui::Editor>();
    auto* editor_pointer = editor.get();
    REQUIRE(tui.add_child(std::move(editor)));
    REQUIRE(tui.set_focus(editor_pointer));
    for (const unsigned char byte : packet) {
        REQUIRE(::write(pty->master.get(), &byte, 1) == 1);
    }
    REQUIRE(::write(pty->master.get(), "Q", 1) == 1);
    REQUIRE(cch::tests::wait_until([&] { return editor_pointer->text() == "Q"; }));
    CHECK(editor_pointer->text().find(' ') == std::string::npos);
    CHECK(editor_pointer->text().find('!') == std::string::npos);

    REQUIRE(tui.stop());
}
