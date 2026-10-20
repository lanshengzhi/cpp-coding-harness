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
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
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

/// Records every key event the input edge delivers, in order.
class KeyEventRecorder final : public tui::Component, public tui::InputHandler, public tui::Focusable {
public:
    [[nodiscard]] support::Expected<tui::RenderResult> render(std::size_t) override { return tui::RenderResult{}; }

    void invalidate() override {}

    tui::InputAdmissionOutcome handle_input(const tui::InputEventVariant& input) override {
        const auto* key = std::get_if<tui::KeyEvent>(&input);
        if (key == nullptr) return tui::InputAdmissionOutcome::Unhandled;
        {
            std::lock_guard lock(mutex_);
            events_.push_back(*key);
        }
        return tui::InputAdmissionOutcome::Consumed;
    }

    void set_focused(bool focused) override { focused_ = focused; }
    [[nodiscard]] bool focused() const override { return focused_; }

    [[nodiscard]] std::vector<tui::KeyEvent> events() const {
        std::lock_guard lock(mutex_);
        return events_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<tui::KeyEvent> events_;
    bool focused_{false};
};

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

TEST_CASE("StdinBuffer holds a fragmented terminal color reply until its terminator",
        "[tui][stdin-buffer][issue1122][spec]") {
    // Frozen pi-v1.3.0 observation: fixtures/pi-v1.3.0/audit-1050/keys-and-terminal-replies.json,
    // scenario fragmented-color-reply-never-leaks-or-opens-editor (revision 71023aec):
    // after the header timeout nothing was delivered and the header stayed buffered;
    // the tail completed one reply, and a real BEL still arrived as ctrl+g.
    tui::StdinBuffer buffer({.timeout = std::chrono::milliseconds{20}, .escape_timeout = std::chrono::milliseconds{5}});
    std::vector<std::string> sequences;
    buffer.set_data_handler([&](std::string sequence) { sequences.push_back(std::move(sequence)); });

    buffer.process("\x1b]4;1;rgb:eaea/6c");
    CHECK(sequences.empty());
    CHECK(buffer.get_buffer() == "\x1b]4;1;rgb:eaea/6c");
    // A recognized reply header waits for its terminator instead of the sequence
    // timeout: no deadline is armed. Expire whatever deadline the edge would have
    // used, exactly as the terminal's deadline handler does, and deliver nothing.
    const auto deadline = buffer.deadline();
    CHECK(deadline == std::chrono::steady_clock::time_point::max());
    if (deadline != std::chrono::steady_clock::time_point::max()) {
        std::this_thread::sleep_until(deadline + std::chrono::milliseconds{1});
        (void)buffer.flush();
    }
    CHECK(sequences.empty());
    CHECK(buffer.get_buffer() == "\x1b]4;1;rgb:eaea/6c");

    buffer.process("6c/6d6d\x07");
    REQUIRE(sequences == std::vector<std::string>{"\x1b]4;1;rgb:eaea/6c6c/6d6d\x07"});
    CHECK(buffer.get_buffer().empty());
    // The completed reply is an out-of-band answer, not a key event.
    CHECK_FALSE(tui::parse_key(sequences.front(), true).has_value());

    buffer.process("\x07");
    REQUIRE(sequences.size() == 2);
    CHECK(sequences.back() == "\x07");
    const auto real_ctrl_g = tui::parse_key(sequences.back(), true);
    REQUIRE(real_ctrl_g);
    CHECK(tui::key_id(*real_ctrl_g) == "ctrl+g");
}

TEST_CASE("StdinBuffer recovers ambiguous Alt-bracket input within the sequence timeout",
        "[tui][stdin-buffer][issue1122][spec]") {
    // Frozen pi-v1.3.0 observation: same artifact, scenario
    // ambiguous-alt-bracket-input-recovers-within-sequence-timeout. At the
    // sequence timeout an ambiguous ESC] prefix is delivered as one Alt-prefixed
    // sequence and the batched tail is reparsed instead of being swallowed as a
    // reply.
    tui::StdinBuffer buffer({.timeout = std::chrono::milliseconds{20}, .escape_timeout = std::chrono::milliseconds{5}});
    std::vector<std::string> sequences;
    buffer.set_data_handler([&](std::string sequence) { sequences.push_back(std::move(sequence)); });

    buffer.process("\x1b]a");
    CHECK(sequences.empty());
    CHECK(buffer.get_buffer() == "\x1b]a");

    const auto flushed = buffer.flush();
    CHECK(flushed == std::vector<std::string>{"\x1b]a"});
    CHECK(sequences == (std::vector<std::string>{"\x1b]", "a"}));

    buffer.process("x\r");
    CHECK(sequences == (std::vector<std::string>{"\x1b]", "a", "x", "\r"}));
}

TEST_CASE("ProcessTerminal keeps a delayed color reply out of the prompt and preserves input order",
        "[tui][terminal][issue1122][spec]") {
    auto pty = cch::tests::open_pseudo_terminal();
    REQUIRE(pty);
    cch::tests::ImageEnvironmentGuard environment;

    cch::tui::ProcessTerminal terminal(
            {.input_fd = pty->slave.get(), .output_fd = pty->slave.get(), .executor = issue949_io().io.get_executor()});
    cch::tui::Tui tui(terminal);
    auto recorder = std::make_unique<KeyEventRecorder>();
    auto* recorder_pointer = recorder.get();
    REQUIRE(tui.add_child(std::move(recorder)));
    REQUIRE(tui.set_focus(recorder_pointer));
    REQUIRE(tui.start());
    (void)cch::tests::read_available(pty->master.get());

    constexpr std::string_view kReplyHeader = "\x1b]4;1;rgb:eaea/6c";
    REQUIRE(::write(pty->master.get(), kReplyHeader.data(), kReplyHeader.size()) ==
            static_cast<ssize_t>(kReplyHeader.size()));
    // Wait past the 50 ms sequence window: a recognized reply header must stay
    // buffered rather than reaching the prompt as garbage or a Ctrl+G trigger.
    std::this_thread::sleep_for(std::chrono::milliseconds{150});
    CHECK(recorder_pointer->events().empty());

    constexpr std::string_view kReplyTail = "6c/6d6d\x07";
    REQUIRE(::write(pty->master.get(), kReplyTail.data(), kReplyTail.size()) ==
            static_cast<ssize_t>(kReplyTail.size()));
    REQUIRE(::write(pty->master.get(), "z", 1) == 1);
    REQUIRE(cch::tests::wait_until([&] { return recorder_pointer->events().size() == 1; }));
    // The tail completed the reply out of band, so the next delivered key is the
    // ordinary "z" in normal input order, with no leaked reply characters.
    CHECK(cch::tui::key_id(recorder_pointer->events().front()) == "z");

    REQUIRE(::write(pty->master.get(), "\x07", 1) == 1);
    REQUIRE(cch::tests::wait_until([&] { return recorder_pointer->events().size() == 2; }));
    CHECK(cch::tui::key_id(recorder_pointer->events().back()) == "ctrl+g");
    REQUIRE(tui.stop());
}
