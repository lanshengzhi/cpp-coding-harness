#include "coding_agent/tui/AltScreenHost.hpp"

#include "TestTuiActionSink.hpp"

#include <cch/tui/Text.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <string>
#include <vector>

namespace cch::coding_agent::tui {

using testing::ActionSinkRecorder;

namespace {

void select_alpha(cch::tui::VirtualTerminal& terminal) {
    REQUIRE(terminal.inject_input("\x1b[<0;1;1M"));
    REQUIRE(terminal.inject_input("\x1b[<32;5;1M"));
    REQUIRE(terminal.inject_input("\x1b[<0;5;1m"));
}

} // namespace

TEST_CASE("AltScreenHost sends selection copy through one generation-stamped clipboard action",
        "[coding_agent][tui][fullscreen][copy][issue1006][spec]") {
    ActionSinkRecorder recorder;
    auto sink = recorder.make_sink();
    const std::size_t generation = 17;
    std::vector<ClipboardCopyResult> feedback;
    cch::tui::VirtualTerminal terminal({.columns = 20, .rows = 3});
    AltScreenHost host(
            terminal, sink, generation, [&feedback](const ClipboardCopyResult& result) { feedback.push_back(result); });
    REQUIRE(host.alt_screen().add_child(std::make_unique<cch::tui::Text>("alpha beta", 0, 0)));
    REQUIRE(host.alt_screen().start());
    // `selection_source_lines_` fills during a rendered frame; without one the
    // selection has no text to copy (the toolkit selection tests render first).
    REQUIRE(host.alt_screen().render());

    CHECK(host.copy_on_select());
    select_alpha(terminal);
    CHECK_FALSE(host.copy_selection_for_keyboard());

    REQUIRE(recorder.write_clipboard.size() == 1);
    CHECK(recorder.write_clipboard.front().text == "alpha");
    CHECK(recorder.generations == std::vector<std::size_t>{generation});
    REQUIRE(feedback.size() == 1);
    CHECK(feedback.front().copied);
    CHECK(feedback.front().error.empty());
    REQUIRE(host.alt_screen().stop());
}

TEST_CASE("AltScreenHost defers automatic selection copy when disabled and copies explicitly once",
        "[coding_agent][tui][fullscreen][copy][issue1006][spec]") {
    ActionSinkRecorder recorder;
    auto sink = recorder.make_sink();
    const std::size_t generation = 9;
    cch::tui::VirtualTerminal terminal({.columns = 20, .rows = 3});
    AltScreenHost host(terminal, sink, generation);
    REQUIRE(host.alt_screen().add_child(std::make_unique<cch::tui::Text>("alpha beta", 0, 0)));
    host.set_copy_on_select(false);
    REQUIRE(host.alt_screen().start());
    // `selection_source_lines_` fills during a rendered frame; without one the
    // selection has no text to copy (the toolkit selection tests render first).
    REQUIRE(host.alt_screen().render());

    select_alpha(terminal);
    CHECK(host.has_active_selection());
    CHECK(recorder.write_clipboard.empty());

    CHECK(host.copy_selection_for_keyboard());
    REQUIRE(recorder.write_clipboard.size() == 1);
    CHECK(recorder.write_clipboard.front().text == "alpha");
    CHECK(recorder.generations == std::vector<std::size_t>{generation});
    REQUIRE(host.alt_screen().stop());
}

TEST_CASE("AltScreenHost skips the clipboard action for an empty selection and returns failures",
        "[coding_agent][tui][fullscreen][copy][issue1006][spec]") {
    ActionSinkRecorder recorder;
    auto sink = recorder.make_sink();
    const std::size_t generation = 3;
    std::vector<ClipboardCopyResult> feedback;
    cch::tui::VirtualTerminal terminal({.columns = 20, .rows = 3});
    AltScreenHost host(
            terminal, sink, generation, [&feedback](const ClipboardCopyResult& result) { feedback.push_back(result); });
    REQUIRE(host.alt_screen().add_child(std::make_unique<cch::tui::Text>("alpha beta", 0, 0)));
    host.set_copy_on_select(false);
    REQUIRE(host.alt_screen().start());
    // `selection_source_lines_` fills during a rendered frame; without one the
    // selection has no text to copy (the toolkit selection tests render first).
    REQUIRE(host.alt_screen().render());

    CHECK_FALSE(host.copy_selection_for_keyboard());
    CHECK(recorder.write_clipboard.empty());
    CHECK(feedback.empty());

    select_alpha(terminal);
    recorder.state->clipboard_copy_result =
            ClipboardCopyResult{.copied = false, .error = "Clipboard unavailable: install wl-clipboard"};
    CHECK(host.copy_selection_for_keyboard());
    REQUIRE(recorder.write_clipboard.size() == 1);
    CHECK(recorder.write_clipboard.front().text == "alpha");
    REQUIRE(feedback.size() == 1);
    CHECK_FALSE(feedback.front().copied);
    CHECK(feedback.front().error == "Clipboard unavailable: install wl-clipboard");
    REQUIRE(host.alt_screen().stop());
}

} // namespace cch::coding_agent::tui
