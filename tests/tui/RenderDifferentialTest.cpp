#include <cch/tui/Text.hpp>
#include <cch/tui/Tui.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include "../../third_party/catch2/catch_test_macros.hpp"

#include <memory>
#include <string>
#include <vector>

namespace {

/// A Component that returns a fixed set of lines on each render.
class FixedTextComponent final : public cch::tui::Component {
public:
    explicit FixedTextComponent(std::vector<std::string> lines)
        : lines_(std::move(lines)) {}

    void set_lines(std::vector<std::string> lines) {
        lines_ = std::move(lines);
        cache_valid_ = false;
    }

    [[nodiscard]] cch::util::Expected<cch::tui::RenderResult> render(std::size_t) override {
        if (cache_valid_) return cch::tui::RenderResult{.lines = cached_};
        cached_ = lines_;
        cache_valid_ = true;
        return cch::tui::RenderResult{.lines = lines_};
    }

    void invalidate() override {
        cache_valid_ = false;
    }

private:
    std::vector<std::string> lines_;
    std::vector<std::string> cached_;
    bool cache_valid_{false};
};

} // namespace

TEST_CASE("First render writes visible content without clearing scrollback", "[tui][render][issue49]") {
    cch::tui::VirtualTerminal terminal({.columns = 6, .rows = 3});
    cch::tui::Tui tui(terminal);
    REQUIRE(tui.add_child(std::make_unique<cch::tui::Text>("hello", 0, 0)));
    REQUIRE(tui.start());

    // First render should NOT call clear_screen
    REQUIRE(tui.render());
    CHECK_FALSE(terminal.check_clear_screen_called());

    // Content is visible
    const std::vector<std::string> expected_screen{"hello ", "", ""};
    CHECK(terminal.screen() == expected_screen);
}

TEST_CASE("Normal updates begin at first changed visible line", "[tui][render][issue49]") {
    cch::tui::VirtualTerminal terminal({.columns = 10, .rows = 5});
    cch::tui::Tui tui(terminal);

    auto text = std::make_unique<cch::tui::Text>("line one\nline two\nline thr", 0, 0);
    auto* text_ptr = text.get();
    REQUIRE(tui.add_child(std::move(text)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());

    // Reset clear_screen flag since first render path doesn't call it
    (void)terminal.check_clear_screen_called();

    // Change only the last line (stays within width at 10 columns)
    text_ptr->set_text("line one\nline two\nline chg");
    REQUIRE(tui.render());

    // Should NOT have cleared full screen for a trailing change
    CHECK_FALSE(terminal.check_clear_screen_called());

    // Screen should show updated content
    const std::vector<std::string> expected_screen{
        "line one  ",
        "line two  ",
        "line chg  ",
        "",
        "",
    };
    CHECK(terminal.screen() == expected_screen);
}

TEST_CASE("Shrinking content clears stale rows below new content", "[tui][render][issue49]") {
    cch::tui::VirtualTerminal terminal({.columns = 5, .rows = 4});
    cch::tui::Tui tui(terminal);

    auto text = std::make_unique<cch::tui::Text>("aaa\nbbb\nccc\nddd", 0, 0);
    auto* text_ptr = text.get();
    REQUIRE(tui.add_child(std::move(text)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());

    // Reset tracking
    (void)terminal.check_clear_screen_called();

    // Content shrinks from 4 lines to 2
    text_ptr->set_text("aaa\nnew");
    REQUIRE(tui.render());

    // Should NOT have cleared full screen for a simple shrink
    CHECK_FALSE(terminal.check_clear_screen_called());

    // Screen shows new content, rows 2-3 are cleared (spaces)
    const auto& screen = terminal.screen();
    REQUIRE(screen.size() == 4);
    CHECK(screen[0] == "aaa  ");
    CHECK(screen[1] == "new  ");
    // Rows 2 and 3 should have been cleared to spaces
    CHECK(screen[2] == "     ");
    CHECK(screen[3] == "     ");
}

TEST_CASE("Width change triggers full redraw with clear screen", "[tui][render][issue49]") {
    cch::tui::VirtualTerminal terminal({.columns = 5, .rows = 3});
    cch::tui::Tui tui(terminal);
    REQUIRE(tui.add_child(std::make_unique<cch::tui::Text>("hello", 0, 0)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());

    // Change dimensions by injecting a resize (this calls invalidate internally)
    REQUIRE(terminal.inject_resize({.columns = 10, .rows = 3}));

    // Render should detect width change and do a full redraw with clear_screen
    REQUIRE(tui.render());
    CHECK(terminal.check_clear_screen_called());

    // Content should be re-rendered at the new width
    const std::vector<std::string> expected_screen{"hello     ", "", ""};
    CHECK(terminal.screen() == expected_screen);
}

TEST_CASE("Changes above tracked viewport trigger safe full redraw", "[tui][render][issue49]") {
    cch::tui::VirtualTerminal terminal({.columns = 5, .rows = 3});
    cch::tui::Tui tui(terminal);

    auto text = std::make_unique<cch::tui::Text>("aaa\nbbb", 0, 0);
    auto* text_ptr = text.get();
    REQUIRE(tui.add_child(std::move(text)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());

    // Reset tracking
    (void)terminal.check_clear_screen_called();

    // Change the first line
    text_ptr->set_text("ccc\nbbb");
    REQUIRE(tui.render());

    // Since first_diff == 0 and previous was not empty, triggers full redraw
    CHECK(terminal.check_clear_screen_called());

    const std::vector<std::string> expected_screen{"ccc  ", "bbb  ", ""};
    CHECK(terminal.screen() == expected_screen);
}

TEST_CASE("Supported synchronized output wraps a render atomically", "[tui][render][issue49]") {
    cch::tui::VirtualTerminal terminal({.columns = 4, .rows = 2});
    cch::tui::Tui tui(terminal);
    REQUIRE(tui.add_child(std::make_unique<cch::tui::Text>("hi", 0, 0)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());

    // Output should be wrapped in sync markers
    const auto& output = terminal.output();
    REQUIRE(output.size() >= 3);
    CHECK(output.front() == "\x1b[?2026h");
    CHECK(output.back() == "\x1b[?2026l");
}

TEST_CASE("Render requests coalesce until render and include resize", "[tui][render][issue58]") {
    cch::tui::VirtualTerminal terminal({.columns = 7, .rows = 2});
    cch::tui::Tui tui(terminal);
    REQUIRE(tui.add_child(std::make_unique<cch::tui::Text>("initial", 0, 0)));
    std::size_t requests = 0;
    tui.set_render_request_sink([&requests] { ++requests; });
    REQUIRE(tui.start());
    REQUIRE(tui.render());

    tui.invalidate();
    tui.invalidate();
    CHECK(requests == 1);
    REQUIRE(tui.render());

    tui.invalidate();
    CHECK(requests == 2);
    REQUIRE(tui.render());
    REQUIRE(terminal.inject_resize({.columns = 8, .rows = 2}));
    CHECK(requests == 3);
}

TEST_CASE("Repeated invalidate coalesces without losing latest state", "[tui][render][issue49]") {
    cch::tui::VirtualTerminal terminal({.columns = 7, .rows = 2});
    cch::tui::Tui tui(terminal);
    REQUIRE(tui.add_child(std::make_unique<cch::tui::Text>("initial", 0, 0)));
    REQUIRE(tui.start());

    // Multiple invalidates before render should still produce correct final state
    tui.invalidate();
    tui.invalidate();
    tui.invalidate();
    REQUIRE(tui.render());

    const std::vector<std::string> expected_screen{"initial", ""};
    CHECK(terminal.screen() == expected_screen);
}

TEST_CASE("Empty content renders zero lines correctly", "[tui][render][issue49]") {
    cch::tui::VirtualTerminal terminal({.columns = 4, .rows = 2});
    cch::tui::Tui tui(terminal);
    // No children means no content
    REQUIRE(tui.start());

    REQUIRE(tui.render());

    // Screen should be empty
    const auto& screen = terminal.screen();
    CHECK(screen.size() == 2);
    CHECK(screen[0].empty());
    CHECK(screen[1].empty());
}

TEST_CASE("Shrink to empty content clears stale rows", "[tui][render][issue49]") {
    cch::tui::VirtualTerminal terminal({.columns = 4, .rows = 2});
    cch::tui::Tui tui(terminal);

    auto text = std::make_unique<cch::tui::Text>("hello", 0, 0);
    auto* text_ptr = text.get();
    REQUIRE(tui.add_child(std::move(text)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());

    // Change to empty text
    text_ptr->set_text("");
    text_ptr->invalidate();
    REQUIRE(tui.render());

    // When content goes to zero and previous had content, clear_screen is called
    // because the first_diff is 0 and previous_lines_ was not empty.
    // After clear_screen, no lines are written, screen shows empty cells.
    const auto& screen = terminal.screen();
    CHECK(screen.size() == 2);
    CHECK(screen[0].empty());
    CHECK(screen[1].empty());
}

TEST_CASE("Viewport height change does not trigger unnecessary clear", "[tui][render][issue49]") {
    cch::tui::VirtualTerminal terminal({.columns = 6, .rows = 2});
    cch::tui::Tui tui(terminal);
    REQUIRE(tui.add_child(std::make_unique<cch::tui::Text>("line1\nline2", 0, 0)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());

    // Resize height to 3 (width unchanged)
    REQUIRE(terminal.inject_resize({.columns = 6, .rows = 3}));

    // Height change alone: width is same, so no full-screen clear from the Tui.
    REQUIRE(tui.render());

    // Screen should show all content in the new height (no wrapping at 6 cols).
    // The extra row is empty because resize cells initialized it to empty strings.
    const auto& screen = terminal.screen();
    REQUIRE(screen.size() == 3);
    CHECK(screen[0] == "line1 ");
    CHECK(screen[1] == "line2 ");
    CHECK(screen[2].empty());
    CHECK(screen[2].size() == 0);
}

TEST_CASE("Pending render request is consumed on render call", "[tui][render][issue49]") {
    cch::tui::VirtualTerminal terminal({.columns = 4, .rows = 2});
    cch::tui::Tui tui(terminal);

    auto text = std::make_unique<cch::tui::Text>("hi", 0, 0);
    auto* text_ptr = text.get();
    REQUIRE(tui.add_child(std::move(text)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());

    // Change content and invalidate
    text_ptr->set_text("bye");
    tui.invalidate();

    // Pending render is consumed and produces the latest content
    REQUIRE(tui.render());
    const std::vector<std::string> expected_screen{"bye ", ""};
    CHECK(terminal.screen() == expected_screen);
}
