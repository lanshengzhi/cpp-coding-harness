#include <cch/tui/Keys.hpp>
#include <cch/tui/ProcessTerminal.hpp>
#include <cch/tui/StdinBuffer.hpp>
#include <cch/tui/Tui.hpp>

#include "support/PseudoTerminal.hpp"

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

class Issue951IoContext final {
public:
    Issue951IoContext() : guard(boost::asio::make_work_guard(io)), thread([this] { io.run(); }) {}

    ~Issue951IoContext() {
        guard.reset();
        io.stop();
        thread.join();
    }

    Issue951IoContext(const Issue951IoContext&) = delete;
    Issue951IoContext& operator=(const Issue951IoContext&) = delete;

    boost::asio::io_context io;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> guard;
    std::jthread thread;
};

Issue951IoContext& issue951_io() {
    static Issue951IoContext context;
    return context;
}

class KeyEventRecorder final : public cch::tui::Component, public cch::tui::InputHandler, public cch::tui::Focusable {
public:
    [[nodiscard]] cch::support::Expected<cch::tui::RenderResult> render(std::size_t) override {
        return cch::tui::RenderResult{};
    }

    void invalidate() override {}

    cch::tui::InputAdmissionOutcome handle_input(const cch::tui::InputEventVariant& input) override {
        const auto* key = std::get_if<cch::tui::KeyEvent>(&input);
        if (key == nullptr) return cch::tui::InputAdmissionOutcome::Unhandled;
        {
            std::lock_guard lock(mutex_);
            events_.push_back(*key);
        }
        return cch::tui::InputAdmissionOutcome::Consumed;
    }

    void set_focused(bool focused) override { focused_ = focused; }

    [[nodiscard]] bool focused() const override { return focused_; }

    [[nodiscard]] std::vector<cch::tui::KeyEvent> events() const {
        std::lock_guard lock(mutex_);
        return events_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<cch::tui::KeyEvent> events_;
    bool focused_{false};
};

struct BufferedKeys {
    std::vector<std::string> sequences;
    std::vector<cch::tui::KeyEvent> events;
};

[[nodiscard]] BufferedKeys buffer_keys(std::string_view kitty, std::string_view raw) {
    cch::tui::StdinBuffer buffer;
    BufferedKeys result;
    buffer.set_data_handler([&](std::string sequence) {
        result.sequences.push_back(sequence);
        if (auto key = cch::tui::parse_key(sequence, true)) result.events.push_back(std::move(*key));
    });
    buffer.process(kitty);
    buffer.process(raw);
    return result;
}

} // namespace

TEST_CASE("legacy controls and modifyOtherKeys keep frozen key identities", "[tui][input][issue951]") {
    struct LegacyKeyCase {
        std::string bytes;
        std::string_view id;
        bool kitty_active;
    };
    const LegacyKeyCase kLegacyCases[]{
            {"\x01", "ctrl+a", false},
            {std::string{"\x00", 1}, "ctrl+space", false},
            {"\x1c", "ctrl+\\", false},
            {"\x1d", "ctrl+]", false},
            {"\x1f", "ctrl+-", false},
            {"\x1b\x1c", "ctrl+alt+\\", false},
            {"\x1b\x1d", "ctrl+alt+]", false},
            {"\x1b\x1f", "ctrl+alt+-", false},
            {"\x1b"
             "B",
                    "alt+left",
                    false},
            {"\x1b"
             "F",
                    "alt+right",
                    false},
            {"\x1b"
             "b",
                    "alt+left",
                    true},
            {"\x1b"
             "f",
                    "alt+right",
                    true},
            {"\x1bp", "alt+up", true},
            {"\x1bn", "alt+down", true},
            {"\x1b ", "alt+space", false},
            {"\x1b\r", "alt+enter", false},
            {"\n", "enter", false},
            {"\n", "shift+enter", true},
    };

    for (const auto& test : kLegacyCases) {
        INFO(std::string(test.bytes));
        const auto key = cch::tui::parse_key(test.bytes, test.kitty_active);
        REQUIRE(key);
        CHECK(cch::tui::key_id(*key) == test.id);
    }

    const auto modified = cch::tui::parse_key("\x1b[27;5;97~", false);
    REQUIRE(modified);
    CHECK(cch::tui::key_id(*modified) == "ctrl+a");
    CHECK_FALSE(cch::tui::printable_text(*modified));

    const auto shifted_non_ascii = cch::tui::parse_key("\x1b[27;2;233~", false);
    REQUIRE(shifted_non_ascii);
    CHECK(shifted_non_ascii->shift);
    CHECK(shifted_non_ascii->text == "\xc3\xa9");
    CHECK(cch::tui::printable_text(*shifted_non_ascii) == std::optional<std::string>{"\xc3\xa9"});

    const auto alt_non_ascii = cch::tui::parse_key("\x1b[27;3;233~", false);
    REQUIRE(alt_non_ascii);
    CHECK(alt_non_ascii->alt);
    CHECK(alt_non_ascii->text.empty());
    CHECK_FALSE(cch::tui::printable_text(*alt_non_ascii));
}

TEST_CASE("Kitty duplicate suppression matches pi for explicit and bare modifiers", "[tui][input][issue951]") {
    const auto explicit_press = buffer_keys("\x1b[97;1u", "a");
    // Frozen pi 7c10bd4337495ee613f2224843ecdf349b80d1df emits both: its
    // StdinBuffer duplicate recognizer accepts bare CSI-u only, not explicit ;1.
    REQUIRE(explicit_press.sequences == std::vector<std::string>{"\x1b[97;1u", "a"});
    REQUIRE(explicit_press.events.size() == 2);
    CHECK(explicit_press.events[0].key == "a");
    CHECK(explicit_press.events[0].text == "a");
    CHECK(explicit_press.events[1].key == "a");
    CHECK(explicit_press.events[1].text == "a");

    const auto bare_press = buffer_keys("\x1b[97u", "a");
    REQUIRE(bare_press.sequences == std::vector<std::string>{"\x1b[97u"});
    REQUIRE(bare_press.events.size() == 1);
    CHECK(bare_press.events.front().text == "a");
}

TEST_CASE(
        "Kitty BMP duplicate suppression handles split UTF-8 and preserves modifier order", "[tui][input][issue951]") {
    cch::tui::StdinBuffer bare_buffer;
    std::vector<std::string> bare_sequences;
    bare_buffer.set_data_handler([&](std::string sequence) { bare_sequences.push_back(std::move(sequence)); });
    bare_buffer.process("\x1b[224u");
    bare_buffer.process(std::string_view("\xc3", 1));
    CHECK(bare_sequences == std::vector<std::string>{"\x1b[224u"});
    bare_buffer.process(std::string_view("\xa0", 1));
    CHECK(bare_sequences == std::vector<std::string>{"\x1b[224u"});

    const auto shifted = buffer_keys("\x1b[224;2u", "\xc3\xa0");
    REQUIRE(shifted.sequences == std::vector<std::string>{"\x1b[224;2u", "\xc3\xa0"});
    REQUIRE(shifted.events.size() == 2);
    CHECK(shifted.events[0].shift);
    CHECK(shifted.events[0].text == "\xc3\xa0");
    CHECK(shifted.events[1].text == "\xc3\xa0");

    const auto alt = buffer_keys("\x1b[224;3u", "\xc3\xa0");
    REQUIRE(alt.sequences == std::vector<std::string>{"\x1b[224;3u", "\xc3\xa0"});
    REQUIRE(alt.events.size() == 2);
    CHECK(alt.events[0].alt);
    CHECK(alt.events[0].text.empty());
    CHECK(alt.events[1].text == "\xc3\xa0");

    const auto modify_other_keys = buffer_keys("\x1b[27;2;233~", "\xc3\xa9");
    REQUIRE(modify_other_keys.sequences == std::vector<std::string>{"\x1b[27;2;233~", "\xc3\xa9"});
    REQUIRE(modify_other_keys.events.size() == 2);
    CHECK(modify_other_keys.events[0].text == "\xc3\xa9");
    CHECK(modify_other_keys.events[1].text == "\xc3\xa9");
}

TEST_CASE("StdinBuffer selects and resolves ambiguous ESC deadlines", "[tui][input][issue951]") {
    cch::tui::StdinBuffer buffer(
            {.timeout = std::chrono::milliseconds{50}, .escape_timeout = std::chrono::milliseconds{10}});
    std::vector<std::string> sequences;
    buffer.set_data_handler([&](std::string sequence) { sequences.push_back(std::move(sequence)); });

    buffer.process("\x1b");
    CHECK(buffer.holds_lone_escape());
    CHECK(buffer.selected_timeout() == std::chrono::milliseconds{10});
    REQUIRE(buffer.flush() == std::vector<std::string>{"\x1b"});
    CHECK(sequences == std::vector<std::string>{"\x1b"});
    CHECK(cch::tui::key_id(*cch::tui::parse_key(sequences.front(), false)) == "escape");

    buffer.clear();
    sequences.clear();
    buffer.process("\x1b");
    buffer.process("\r");
    REQUIRE(sequences == std::vector<std::string>{"\x1b\r"});
    const auto alt_enter = cch::tui::parse_key(sequences.front(), false);
    REQUIRE(alt_enter);
    CHECK(cch::tui::key_id(*alt_enter) == "alt+enter");
}

TEST_CASE("ProcessTerminal protocol switching updates TUI decoding and consumes replies",
        "[tui][terminal][input][issue951]") {
    auto pty = cch::tests::open_pseudo_terminal();
    REQUIRE(pty);

    cch::tui::ProcessTerminal terminal({
            .input_fd = pty->slave.get(),
            .output_fd = pty->slave.get(),
            .executor = issue951_io().io.get_executor(),
    });
    cch::tui::Tui tui(terminal);
    auto component = std::make_unique<KeyEventRecorder>();
    auto* component_pointer = component.get();
    REQUIRE(tui.add_child(std::move(component)));
    REQUIRE(tui.set_focus(component_pointer));
    REQUIRE(tui.start());
    (void)cch::tests::read_available(pty->master.get());

    constexpr std::string_view kDeviceAttributes = "\x1b[?1;2c";
    REQUIRE(::write(pty->master.get(), kDeviceAttributes.data(), kDeviceAttributes.size()) ==
            static_cast<ssize_t>(kDeviceAttributes.size()));
    REQUIRE(cch::tests::wait_until(
            [&] { return terminal.capabilities().keyboard_protocol == cch::tui::KeyboardProtocol::ModifyOtherKeys; }));

    constexpr std::string_view kModifyOtherKeys = "\x1b[27;5;97~";
    REQUIRE(::write(pty->master.get(), kModifyOtherKeys.data(), kModifyOtherKeys.size()) ==
            static_cast<ssize_t>(kModifyOtherKeys.size()));
    REQUIRE(cch::tests::wait_until([&] { return component_pointer->events().size() == 1; }));

    constexpr std::string_view kKittyFlags = "\x1b[?7u";
    REQUIRE(::write(pty->master.get(), kKittyFlags.data(), kKittyFlags.size()) ==
            static_cast<ssize_t>(kKittyFlags.size()));
    REQUIRE(cch::tests::wait_until(
            [&] { return terminal.capabilities().keyboard_protocol == cch::tui::KeyboardProtocol::Kitty; }));
    REQUIRE(::write(pty->master.get(), "\n\x1b\rx", 4) == 4);
    REQUIRE(cch::tests::wait_until([&] { return component_pointer->events().size() == 4; }));

    const auto events = component_pointer->events();
    REQUIRE(events.size() == 4);
    CHECK(cch::tui::key_id(events[0]) == "ctrl+a");
    CHECK(cch::tui::key_id(events[1]) == "shift+enter");
    CHECK(cch::tui::key_id(events[2]) == "shift+enter");
    CHECK(cch::tui::key_id(events[3]) == "x");
    CHECK(events[3].text == "x");
    CHECK(terminal.capabilities().keyboard_protocol == cch::tui::KeyboardProtocol::Kitty);
    REQUIRE(tui.stop());
}
