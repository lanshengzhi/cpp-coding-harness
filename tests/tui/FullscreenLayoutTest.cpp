// Fullscreen constrained layout composition through AltScreen::set_layout_root
// (issues #998, #999).
//
// Frozen contract: pi v1.0.4 at 7c10bd4337495ee613f2224843ecdf349b80d1df —
// `packages/tui/src/tui-alt-screen.ts:318-334` (`setLayoutRoot`,
// `getMountedRoots`), `packages/tui/src/tui.ts:911-913` (`TuiBase.invalidate`)
// and the product composition in
// `packages/coding-agent/src/modes/interactive/chat-viewport.ts:31-44`.

#include <cch/tui/AltScreen.hpp>
#include <cch/tui/Stack.hpp>
#include <cch/tui/VStack.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

class FrameChild final : public cch::tui::Component {
public:
    explicit FrameChild(std::vector<std::string> lines) : lines_(std::move(lines)) {}

    void set_lines(std::vector<std::string> lines) { lines_ = std::move(lines); }

    [[nodiscard]] cch::support::Expected<cch::tui::RenderResult> render(std::size_t width) override {
        rendered_width = width;
        return cch::tui::RenderResult{.lines = lines_};
    }

    void invalidate() override { ++invalidate_count; }

    std::size_t rendered_width{0};
    std::size_t invalidate_count{0};

private:
    std::vector<std::string> lines_;
};

class CursorChild final : public cch::tui::Component, public cch::tui::Focusable {
public:
    CursorChild(std::vector<std::string> lines, std::size_t cursor_row, std::size_t cursor_column)
        : lines_(std::move(lines)), cursor_row_(cursor_row), cursor_column_(cursor_column) {}

    void set_lines(std::vector<std::string> lines, std::size_t cursor_row) {
        lines_ = std::move(lines);
        cursor_row_ = cursor_row;
    }

    [[nodiscard]] cch::support::Expected<cch::tui::RenderResult> render(std::size_t width) override {
        rendered_width = width;
        return cch::tui::RenderResult{.lines = lines_};
    }

    void invalidate() override {}

    void set_focused(bool focused) override { focused_ = focused; }
    [[nodiscard]] bool focused() const override { return focused_; }

    [[nodiscard]] std::optional<cch::tui::CursorPosition> cursor_location() const override {
        if (!focused_) return std::nullopt;
        return cch::tui::CursorPosition{.column = cursor_column_, .row = cursor_row_};
    }

    std::size_t rendered_width{0};

private:
    std::vector<std::string> lines_;
    std::size_t cursor_row_{0};
    std::size_t cursor_column_{0};
    bool focused_{false};
};

} // namespace

TEST_CASE("fullscreen layout root keeps the fixed footer through editor and menu growth",
        "[tui][layout][issue998][spec]") {
    // Root = [transcript {basis 0, grow 1, shrink 1, min 1}, dock {basis auto, grow 0, shrink 1, min 1}];
    // dock = [editor {shrink 1, min 3}, menu {shrink 1, visible}, footer (fixed 2 rows)].
    // The dock keeps its natural height at the bottom of the viewport while the
    // editor grows and the completion menu opens (issue #831: pi keeps both
    // footer rows below a grown dock).
    cch::tui::VirtualTerminal terminal({.columns = 8, .rows = 12});
    cch::tui::AltScreen tui(terminal);

    auto transcript =
            std::make_unique<FrameChild>(std::vector<std::string>{"t1", "t2", "t3", "t4", "t5", "t6", "t7", "t8"});
    auto editor = std::make_unique<CursorChild>(std::vector<std::string>{"e1", "e2", "e3"}, 2, 1);
    auto* editor_pointer = editor.get();
    bool menu_open = false;
    auto menu = std::make_unique<FrameChild>(std::vector<std::string>{"m1", "m2", "m3"});
    auto footer = std::make_unique<FrameChild>(std::vector<std::string>{"footer-1", "footer-2"});

    auto dock = std::make_unique<cch::tui::VStack>();
    REQUIRE(dock->add_child(std::move(editor), cch::tui::StackEntryOptions{.shrink = 1, .min_size = 3}));
    REQUIRE(dock->add_child(std::move(menu),
            cch::tui::StackEntryOptions{
                    .shrink = 1, .visible = [&menu_open](const cch::tui::LayoutViewport&) { return menu_open; }}));
    REQUIRE(dock->add_child(std::move(footer), cch::tui::StackEntryOptions{.basis = 2, .grow = 0, .shrink = 0}));

    auto root = std::make_unique<cch::tui::VStack>();
    REQUIRE(root->add_child(
            std::move(transcript), cch::tui::StackEntryOptions{.basis = 0, .grow = 1, .shrink = 1, .min_size = 1}));
    REQUIRE(root->add_child(std::move(dock),
            cch::tui::StackEntryOptions{.basis = std::nullopt, .grow = 0, .shrink = 1, .min_size = 1}));

    tui.set_layout_root(std::move(root));
    REQUIRE(tui.start());
    REQUIRE(tui.set_focus(editor_pointer));

    // Editor at its 3-row minimum: the transcript takes the remaining 7 rows and
    // the dock keeps editor + both footer rows at the bottom.
    REQUIRE(tui.render());
    CHECK(terminal.screen() == std::vector<std::string>{"t1      ",
                                       "t2      ",
                                       "t3      ",
                                       "t4      ",
                                       "t5      ",
                                       "t6      ",
                                       "t7      ",
                                       "e1      ",
                                       "e2      ",
                                       "e3      ",
                                       "footer-1",
                                       "footer-2"});
    CHECK(terminal.cursor().row == 9);
    CHECK(terminal.cursor().column == 1);

    // Multiline editor growth: the dock takes the rows the transcript gives up,
    // and both footer rows stay.
    editor_pointer->set_lines(std::vector<std::string>{"e1", "e2", "e3", "e4", "e5", "e6"}, 5);
    REQUIRE(tui.render());
    CHECK(terminal.screen() == std::vector<std::string>{"t1      ",
                                       "t2      ",
                                       "t3      ",
                                       "t4      ",
                                       "e1      ",
                                       "e2      ",
                                       "e3      ",
                                       "e4      ",
                                       "e5      ",
                                       "e6      ",
                                       "footer-1",
                                       "footer-2"});
    CHECK(terminal.cursor().row == 9);

    // Completion menu open: it lands between the editor and the footer.
    menu_open = true;
    REQUIRE(tui.render());
    CHECK(terminal.screen() == std::vector<std::string>{"t1      ",
                                       "e1      ",
                                       "e2      ",
                                       "e3      ",
                                       "e4      ",
                                       "e5      ",
                                       "e6      ",
                                       "m1      ",
                                       "m2      ",
                                       "m3      ",
                                       "footer-1",
                                       "footer-2"});
    CHECK(terminal.cursor().row == 6);

    // Over-constrained growth: the editor is cropped around its cursor and the
    // menu shrinks first, but the fixed footer keeps both rows.
    editor_pointer->set_lines(std::vector<std::string>{"e1", "e2", "e3", "e4", "e5", "e6", "e7", "e8", "e9"}, 8);
    REQUIRE(tui.render());
    CHECK(terminal.screen() == std::vector<std::string>{"t1      ",
                                       "e3      ",
                                       "e4      ",
                                       "e5      ",
                                       "e6      ",
                                       "e7      ",
                                       "e8      ",
                                       "e9      ",
                                       "m1      ",
                                       "m2      ",
                                       "footer-1",
                                       "footer-2"});
    CHECK(terminal.cursor().row == 7);
    CHECK(terminal.cursor().column == 1);

    // Menu closed again: the dock shrinks back and the footer rows never move.
    menu_open = false;
    editor_pointer->set_lines(std::vector<std::string>{"e1", "e2", "e3"}, 2);
    REQUIRE(tui.render());
    CHECK(terminal.screen() == std::vector<std::string>{"t1      ",
                                       "t2      ",
                                       "t3      ",
                                       "t4      ",
                                       "t5      ",
                                       "t6      ",
                                       "t7      ",
                                       "e1      ",
                                       "e2      ",
                                       "e3      ",
                                       "footer-1",
                                       "footer-2"});
}

TEST_CASE("fullscreen layout root falls back to the sequential children composition", "[tui][layout][issue998]") {
    // pi `setLayoutRoot(undefined)` restores the default mounted roots
    // (`tui-alt-screen.ts:318-334`), so the #997 sequential composition stays
    // available after a layout root was installed.
    cch::tui::VirtualTerminal terminal({.columns = 6, .rows = 3});
    cch::tui::AltScreen tui(terminal);

    auto root = std::make_unique<cch::tui::VStack>();
    REQUIRE(root->add_child(std::make_unique<FrameChild>(std::vector<std::string>{"stack"})));
    tui.set_layout_root(std::move(root));
    REQUIRE(tui.add_child(std::make_unique<FrameChild>(std::vector<std::string>{"child"})));

    REQUIRE(tui.start());
    REQUIRE(tui.render());
    // While a layout root is installed, children are not mounted roots.
    CHECK(terminal.screen() == std::vector<std::string>{"stack ", "      ", "      "});

    tui.set_layout_root(nullptr);
    REQUIRE(tui.render());
    CHECK(terminal.screen() == std::vector<std::string>{"child ", "      ", "      "});
    REQUIRE(tui.stop());
}

TEST_CASE("fullscreen invalidate reaches the mounted layout root instead of the children", "[tui][layout][issue999]") {
    // pi `TuiBase.invalidate` (`tui.ts:911-913`) invalidates every mounted root,
    // and the layout root replaces the children as the set of mounted roots
    // (`tui-alt-screen.ts:318-334`); removing it restores them.
    cch::tui::VirtualTerminal terminal({.columns = 6, .rows = 2});
    cch::tui::AltScreen tui(terminal);

    auto root = std::make_unique<FrameChild>(std::vector<std::string>{"stack"});
    auto* root_child = root.get();
    auto child = std::make_unique<FrameChild>(std::vector<std::string>{"child"});
    auto* mounted_child = child.get();
    REQUIRE(tui.add_child(std::move(child)));

    tui.set_layout_root(std::move(root));
    tui.invalidate();
    CHECK(root_child->invalidate_count == 1);
    CHECK(mounted_child->invalidate_count == 0);

    tui.set_layout_root(nullptr);
    tui.invalidate();
    CHECK(root_child->invalidate_count == 1);
    CHECK(mounted_child->invalidate_count == 1);
}
