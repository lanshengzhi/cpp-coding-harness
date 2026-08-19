// Unit tests for the unified terminal stream decoder: response demux
// (CPR, cell size, keyboard protocol, color scheme), single-buffer fragment
// reassembly across chunk boundaries, 150 ms timeout flush semantics, and
// key/paste pass-through. Behavioral baseline: pi 83114817
// packages/tui/src (terminal.ts negotiation, terminal-image.ts cell size,
// terminal-colors.ts appearance, keys.ts parseKey).

#include "tui/InputDecoder.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>
#include <vector>

using namespace cch;

TEST_CASE(
    "stream decoder demuxes a cursor position report out of the byte stream",
    "[tui][decoder]") {
    tui::detail::TerminalStreamDecoder decoder;
    const auto result = decoder.feed("a\x1b[8;3Rz");

    REQUIRE(result.responses.size() == 1);
    const auto* position = std::get_if<tui::CursorPosition>(&result.responses.front());
    REQUIRE(position != nullptr);
    CHECK(*position == tui::CursorPosition{.column = 2, .row = 7});

    REQUIRE(result.events.size() == 2);
    const auto* first = std::get_if<tui::KeyEvent>(&result.events.front());
    REQUIRE(first != nullptr);
    CHECK(first->key == "a");
    const auto* second = std::get_if<tui::KeyEvent>(&result.events.back());
    REQUIRE(second != nullptr);
    CHECK(second->key == "z");
    CHECK(result.forwarded_input == "az");
}

TEST_CASE(
    "stream decoder demuxes a cell-size response without leaking bytes",
    "[tui][decoder]") {
    tui::detail::TerminalStreamDecoder decoder;
    const auto result = decoder.feed("\x1b[6;20;10t");

    REQUIRE(result.responses.size() == 1);
    const auto* cell_size = std::get_if<tui::detail::CellSizeResponse>(&result.responses.front());
    REQUIRE(cell_size != nullptr);
    CHECK(*cell_size == tui::detail::CellSizeResponse{.height_px = 20, .width_px = 10});
    CHECK(result.events.empty());
    CHECK(result.forwarded_input.empty());
}

TEST_CASE(
    "stream decoder demuxes keyboard protocol negotiation responses",
    "[tui][decoder]") {
    tui::detail::TerminalStreamDecoder decoder;

    const auto kitty = decoder.feed("\x1b[?7u");
    REQUIRE(kitty.responses.size() == 1);
    const auto* flags =
        std::get_if<tui::detail::KeyboardProtocolResponse>(&kitty.responses.front());
    REQUIRE(flags != nullptr);
    CHECK(flags->kind == tui::detail::KeyboardProtocolResponseKind::KittyFlags);
    CHECK(flags->flags == 7);
    CHECK(kitty.events.empty());
    CHECK(kitty.forwarded_input.empty());

    const auto attributes = decoder.feed("\x1b[?1;2c");
    REQUIRE(attributes.responses.size() == 1);
    const auto* device =
        std::get_if<tui::detail::KeyboardProtocolResponse>(&attributes.responses.front());
    REQUIRE(device != nullptr);
    CHECK(device->kind == tui::detail::KeyboardProtocolResponseKind::DeviceAttributes);
    CHECK(attributes.events.empty());
}

TEST_CASE(
    "stream decoder demuxes color scheme and background appearance reports",
    "[tui][decoder]") {
    tui::detail::TerminalStreamDecoder decoder;

    const auto dark = decoder.feed("\x1b[?997;1n");
    REQUIRE(dark.responses.size() == 1);
    const auto* scheme =
        std::get_if<tui::detail::ColorSchemeResponse>(&dark.responses.front());
    REQUIRE(scheme != nullptr);
    CHECK(scheme->kind == tui::detail::ColorSchemeResponseKind::ColorScheme);
    CHECK(scheme->appearance == tui::TerminalAppearance::Dark);
    CHECK(dark.forwarded_input.empty());

    const auto light = decoder.feed("\x1b]11;rgb:ffff/ffff/ffff\x07");
    REQUIRE(light.responses.size() == 1);
    const auto* background =
        std::get_if<tui::detail::ColorSchemeResponse>(&light.responses.front());
    REQUIRE(background != nullptr);
    CHECK(background->kind == tui::detail::ColorSchemeResponseKind::Background);
    CHECK(background->appearance == tui::TerminalAppearance::Light);

    const auto string_terminated = decoder.feed("\x1b]11;#000000\x1b\\");
    REQUIRE(string_terminated.responses.size() == 1);
    const auto* terminated =
        std::get_if<tui::detail::ColorSchemeResponse>(&string_terminated.responses.front());
    REQUIRE(terminated != nullptr);
    CHECK(terminated->appearance == tui::TerminalAppearance::Dark);
}

TEST_CASE(
    "stream decoder drops malformed appearance responses instead of forwarding them",
    "[tui][decoder]") {
    tui::detail::TerminalStreamDecoder decoder;

    const auto malformed = decoder.feed("\x1b]11;not-a-color\x07typed");
    CHECK(malformed.responses.empty());
    CHECK(malformed.forwarded_input == "typed");

    const auto unknown_value = decoder.feed("\x1b[?997;9n");
    CHECK(unknown_value.responses.empty());
    CHECK(unknown_value.events.empty());
    CHECK(unknown_value.forwarded_input.empty());
}

TEST_CASE(
    "stream decoder forwards malformed response-shaped sequences as in-band bytes",
    "[tui][decoder]") {
    tui::detail::TerminalStreamDecoder decoder;

    // A CPR-shaped sequence that fails validation (0-based overflow) is not a
    // response: the bytes forward verbatim and decode to no event, exactly
    // the pre-unification sidecar pass-through.
    const auto result = decoder.feed("\x1b[0;0R");
    CHECK(result.responses.empty());
    CHECK(result.events.empty());
    CHECK(result.forwarded_input == "\x1b[0;0R");
}

TEST_CASE(
    "stream decoder reassembles responses split across chunk boundaries",
    "[tui][decoder]") {
    tui::detail::TerminalStreamDecoder decoder;

    std::vector<tui::detail::TerminalResponseVariant> responses;
    std::string forwarded;
    const std::string_view stream = "\x1b[6;20;10t";
    for (const auto byte : stream) {
        auto result = decoder.feed(std::string_view(&byte, 1));
        responses.insert(
            responses.end(),
            std::make_move_iterator(result.responses.begin()),
            std::make_move_iterator(result.responses.end()));
        forwarded += result.forwarded_input;
    }
    REQUIRE(responses.size() == 1);
    const auto* cell_size = std::get_if<tui::detail::CellSizeResponse>(&responses.front());
    REQUIRE(cell_size != nullptr);
    CHECK(*cell_size == tui::detail::CellSizeResponse{.height_px = 20, .width_px = 10});
    CHECK(forwarded.empty());
}

TEST_CASE(
    "stream decoder reassembles key sequences split across chunk boundaries",
    "[tui][decoder]") {
    tui::detail::TerminalStreamDecoder decoder;

    std::vector<tui::InputEventVariant> events;
    std::string forwarded;
    const std::string_view stream = "\x1b[1;5A";
    for (const auto byte : stream) {
        auto result = decoder.feed(std::string_view(&byte, 1));
        events.insert(
            events.end(),
            std::make_move_iterator(result.events.begin()),
            std::make_move_iterator(result.events.end()));
        forwarded += result.forwarded_input;
    }
    REQUIRE(events.size() == 1);
    const auto* key = std::get_if<tui::KeyEvent>(&events.front());
    REQUIRE(key != nullptr);
    CHECK(key->key == "up");
    CHECK(key->ctrl);
    CHECK(forwarded == stream);
}

TEST_CASE(
    "stream decoder flush resolves a lone escape as the escape key",
    "[tui][decoder]") {
    tui::detail::TerminalStreamDecoder decoder;

    const auto pending = decoder.feed("\x1b");
    CHECK(pending.events.empty());
    CHECK(pending.forwarded_input.empty());

    const auto flushed = decoder.flush();
    // Byte-level consumers receive the fragment verbatim; event consumers
    // receive the best-effort key decode (pi's fragment timeout).
    CHECK(flushed.forwarded_input == "\x1b");
    REQUIRE(flushed.events.size() == 1);
    const auto* key = std::get_if<tui::KeyEvent>(&flushed.events.front());
    REQUIRE(key != nullptr);
    CHECK(key->key == "escape");
}

TEST_CASE(
    "stream decoder flush drops appearance response fragments",
    "[tui][decoder]") {
    tui::detail::TerminalStreamDecoder decoder;

    CHECK(decoder.feed("\x1b]11;unterminated").forwarded_input.empty());
    const auto osc_flush = decoder.flush();
    CHECK(osc_flush.forwarded_input.empty());
    CHECK(osc_flush.events.empty());
    CHECK(osc_flush.responses.empty());

    CHECK(decoder.feed("\x1b[?997;").forwarded_input.empty());
    const auto scheme_flush = decoder.flush();
    CHECK(scheme_flush.forwarded_input.empty());
    CHECK(scheme_flush.events.empty());
}

TEST_CASE(
    "stream decoder flush forwards fragments that could still be user input",
    "[tui][decoder]") {
    tui::detail::TerminalStreamDecoder decoder;

    // A partial modifier sequence is not response-shaped: it forwards
    // verbatim so the byte-level consumer's own decoder can finish it.
    CHECK(decoder.feed("\x1b[1;5").events.empty());
    const auto flushed = decoder.flush();
    CHECK(flushed.forwarded_input == "\x1b[1;5");
    CHECK(flushed.events.empty());
}

TEST_CASE(
    "stream decoder completes a fragmented appearance response before the flush window",
    "[tui][decoder]") {
    tui::detail::TerminalStreamDecoder decoder;

    CHECK(decoder.feed("\x1b[?997;").responses.empty());
    const auto completed = decoder.feed("2nlate");
    REQUIRE(completed.responses.size() == 1);
    const auto* scheme =
        std::get_if<tui::detail::ColorSchemeResponse>(&completed.responses.front());
    REQUIRE(scheme != nullptr);
    CHECK(scheme->appearance == tui::TerminalAppearance::Light);
    CHECK(completed.forwarded_input == "late");
}

TEST_CASE(
    "stream decoder passes bracketed paste through verbatim with one paste event",
    "[tui][decoder]") {
    tui::detail::TerminalStreamDecoder decoder;

    const std::string_view framed = "\x1b[200~hello\nworld\x1b[201~";
    const auto result = decoder.feed(framed);
    REQUIRE(result.events.size() == 1);
    const auto* paste = std::get_if<tui::PasteEvent>(&result.events.front());
    REQUIRE(paste != nullptr);
    CHECK(paste->text == "hello\nworld");
    CHECK(paste->lines == 2);
    // Byte-level consumers see the complete framing so the downstream input
    // decoder re-derives the same paste (pi stdin-buffer re-wraps paste
    // content with its bracketed markers).
    CHECK(result.forwarded_input == framed);
    CHECK(result.responses.empty());
}

TEST_CASE(
    "stream decoder protects response-shaped bytes inside bracketed paste",
    "[tui][decoder]") {
    tui::detail::TerminalStreamDecoder decoder;

    const auto result = decoder.feed("\x1b[200~line \x1b[?997;1n text\x1b[201~");
    CHECK(result.responses.empty());
    REQUIRE(result.events.size() == 1);
    const auto* paste = std::get_if<tui::PasteEvent>(&result.events.front());
    REQUIRE(paste != nullptr);
    CHECK(paste->text == "line \x1b[?997;1n text");
}

TEST_CASE(
    "stream decoder reset clears a buffered fragment",
    "[tui][decoder]") {
    tui::detail::TerminalStreamDecoder decoder;

    CHECK(decoder.feed("\x1b[6;20").responses.empty());
    decoder.reset();
    const auto result = decoder.feed("q");
    CHECK(result.responses.empty());
    REQUIRE(result.events.size() == 1);
    const auto* key = std::get_if<tui::KeyEvent>(&result.events.front());
    REQUIRE(key != nullptr);
    CHECK(key->key == "q");
    CHECK(result.forwarded_input == "q");
}

TEST_CASE(
    "stream decoder bounds and discards a malformed oversized escape sequence",
    "[tui][decoder]") {
    tui::detail::TerminalStreamDecoder decoder;

    const std::string oversized = "\x1b]52;" + std::string(300, 'y');
    const auto buffered = decoder.feed(oversized);
    CHECK(buffered.forwarded_input.empty());
    CHECK(buffered.events.empty());

    // The BEL terminates the discard; ordinary input decodes again.
    const auto recovered = decoder.feed("\x07q");
    REQUIRE(recovered.events.size() == 1);
    const auto* key = std::get_if<tui::KeyEvent>(&recovered.events.front());
    REQUIRE(key != nullptr);
    CHECK(key->key == "q");
    CHECK(recovered.forwarded_input == "q");
}

TEST_CASE(
    "stream decoder interleaves responses and user input in stream order",
    "[tui][decoder]") {
    tui::detail::TerminalStreamDecoder decoder;

    const auto result = decoder.feed("x\x1b[?997;2n\x1b[A");
    REQUIRE(result.responses.size() == 1);
    CHECK(
        std::get_if<tui::detail::ColorSchemeResponse>(&result.responses.front()) != nullptr);
    REQUIRE(result.events.size() == 2);
    const auto* typed = std::get_if<tui::KeyEvent>(&result.events.front());
    REQUIRE(typed != nullptr);
    CHECK(typed->key == "x");
    const auto* up = std::get_if<tui::KeyEvent>(&result.events.back());
    REQUIRE(up != nullptr);
    CHECK(up->key == "up");
    CHECK(result.forwarded_input == "x\x1b[A");
}

TEST_CASE(
    "stream decoder forwards kitty image protocol acknowledgements without events",
    "[tui][decoder]") {
    tui::detail::TerminalStreamDecoder decoder;

    // APC image acknowledgements are not demuxed responses: they pass through
    // to the byte-level consumer and decode to no key event, exactly as the
    // pre-unification pipeline dropped them at the input decoder.
    const std::string_view acknowledgement = "\x1b_Gi=1;OK\x1b\\";
    const auto result = decoder.feed(acknowledgement);
    CHECK(result.responses.empty());
    CHECK(result.events.empty());
    CHECK(result.forwarded_input == acknowledgement);
}
