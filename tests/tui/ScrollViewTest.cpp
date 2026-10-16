// ScrollView viewport, follow-end, and nested overscroll (issue #1000).
//
// Every expected value below is derived from the frozen pi v1.0.4 revision
// 7c10bd4337495ee613f2224843ecdf349b80d1df:
// `packages/tui/src/components/scroll-view.ts` (the scroll algebra),
// `packages/tui/src/layout.ts:104-186,300-380` (the scroll node, the image
// crop), `packages/tui/src/tui-alt-screen.ts:262-270,284-290,483-494,1673-1680`
// (the implicit document and viewport), and the independent expectations in
// `packages/tui/test/layout.test.ts:131-148,170-189,250-258,346-353` and
// `packages/tui/test/tui-alt-screen.test.ts:63-90`.

#include <cch/tui/AltScreen.hpp>
#include <cch/tui/ScrollView.hpp>
#include <cch/tui/Stack.hpp>
#include <cch/tui/Text.hpp>
#include <cch/tui/VStack.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include "tui/Layout.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

class LabeledChild final : public cch::tui::Component {
public:
    explicit LabeledChild(std::vector<std::string> lines) : lines_(std::move(lines)) {}

    [[nodiscard]] cch::support::Expected<cch::tui::RenderResult> render(std::size_t width) override {
        ++render_count;
        rendered_width = width;
        return cch::tui::RenderResult{.lines = lines_};
    }

    void invalidate() override { ++invalidate_count; }

    std::size_t render_count{0};
    std::size_t rendered_width{0};
    std::size_t invalidate_count{0};

private:
    std::vector<std::string> lines_;
};

class CursorChild final : public cch::tui::Component {
public:
    CursorChild(std::vector<std::string> lines, std::size_t cursor_row, std::size_t cursor_column)
        : lines_(std::move(lines)), cursor_row_(cursor_row), cursor_column_(cursor_column) {}

    void set_cursor(std::size_t cursor_row, std::size_t cursor_column) {
        cursor_row_ = cursor_row;
        cursor_column_ = cursor_column;
    }

    [[nodiscard]] cch::support::Expected<cch::tui::RenderResult> render(std::size_t width) override {
        rendered_width = width;
        return cch::tui::RenderResult{.lines = lines_};
    }

    void invalidate() override { ++invalidate_count; }

    [[nodiscard]] std::optional<cch::tui::CursorPosition> cursor_location() const override {
        return cch::tui::CursorPosition{.column = cursor_column_, .row = cursor_row_};
    }

    std::size_t rendered_width{0};
    std::size_t invalidate_count{0};

private:
    std::vector<std::string> lines_;
    std::size_t cursor_row_{0};
    std::size_t cursor_column_{0};
};

class ImageChild final : public cch::tui::Component {
public:
    ImageChild(std::vector<std::string> lines, cch::tui::InlineImageRenderRegion image)
        : lines_(std::move(lines)), image_(std::move(image)) {}

    [[nodiscard]] cch::support::Expected<cch::tui::RenderResult> render(std::size_t width) override {
        rendered_width = width;
        return cch::tui::RenderResult{.lines = lines_, .images = {image_}};
    }

    void invalidate() override {}

    std::size_t rendered_width{0};

private:
    std::vector<std::string> lines_;
    cch::tui::InlineImageRenderRegion image_;
};

/// A placement of `rows` reserved cell rows over `columns` cells, starting at
/// content row `row` (the shape a component reports before any clipping).
[[nodiscard]] cch::tui::InlineImageRenderRegion reserved_image(std::size_t row, std::size_t columns, std::size_t rows) {
    cch::tui::InlineImageRenderRegion image;
    image.resource_id = 7;
    image.region = cch::tui::CellRegion{.column = 0, .row = row, .columns = columns, .rows = rows};
    image.fallback_text = "im";
    image.rows_reserved_in_lines = true;
    return image;
}

[[nodiscard]] std::vector<std::string> numbered_lines(std::size_t count) {
    std::vector<std::string> lines;
    lines.reserve(count);
    for (std::size_t index = 1; index <= count; ++index)
        lines.emplace_back(std::to_string(index));
    return lines;
}

[[nodiscard]] cch::support::Expected<cch::tui::detail::LayoutFrame> render_frame(
        cch::tui::Component& root, std::size_t width, std::size_t height) {
    return cch::tui::detail::LayoutEngine::render(root, width, height);
}

/// The frame's painted rows with the leaf's width padding removed: pi's frame
/// comparisons trim the padding (`test/layout.test.ts` maps `line.trimEnd()`).
[[nodiscard]] std::vector<std::string> drawn_lines(const cch::tui::detail::LayoutFrame& frame) {
    std::vector<std::string> rows;
    rows.reserve(frame.lines.size());
    for (const auto& row : frame.lines) {
        auto trimmed = row;
        while (!trimmed.empty() && trimmed.back() == ' ')
            trimmed.pop_back();
        rows.push_back(std::move(trimmed));
    }
    return rows;
}

[[nodiscard]] std::vector<std::string> trimmed_rows(const cch::tui::VirtualTerminal& terminal) {
    std::vector<std::string> rows;
    rows.reserve(terminal.screen().size());
    for (const auto& row : terminal.screen()) {
        auto trimmed = row;
        while (!trimmed.empty() && trimmed.back() == ' ')
            trimmed.pop_back();
        rows.push_back(std::move(trimmed));
    }
    return rows;
}

} // namespace

TEST_CASE("ScrollView forwards one child's output, invalidation, containment, and cursor",
        "[tui][scroll-view][issue1000]") {
    auto child = std::make_unique<CursorChild>(std::vector<std::string>{"one", "two"}, 1, 2);
    auto* child_pointer = child.get();
    cch::tui::ScrollView view(std::move(child),
            cch::tui::ScrollViewOptions{
                    .follow_end = true, .primary = true, .overscroll = cch::tui::ScrollOverscroll::Contain});

    CHECK(view.follow_end());
    CHECK(view.primary());
    CHECK(view.overscroll() == cch::tui::ScrollOverscroll::Contain);
    CHECK(view.scroll_top() == 0);
    CHECK(view.viewport_height() == 0);
    CHECK(view.content_height() == 0);

    auto rendered = view.render(8);
    REQUIRE(rendered);
    // No scrollbar reserves columns yet (#1001), so the child is measured at
    // the full width and the viewport's content is the child's output.
    CHECK(child_pointer->rendered_width == 8);
    CHECK(rendered->lines == std::vector<std::string>{"one", "two"});
    CHECK(rendered->images.empty());

    view.invalidate();
    CHECK(child_pointer->invalidate_count == 1);

    CHECK(view.contains(&view));
    CHECK(view.contains(child_pointer));

    auto cursor = view.cursor_location();
    REQUIRE(cursor);
    CHECK(cursor->row == 1);
    CHECK(cursor->column == 2);
}

TEST_CASE("ScrollView clamps programmatic scrolls and follows the content end", "[tui][scroll-view][issue1000]") {
    // pi `scroll-view.ts:127-146` (`scrollTo`): the target clamps to
    // `[0, contentHeight - viewportHeight]`, a target at the content end with
    // `disableFollow` suppresses follow until the viewport leaves the end, and a
    // target at the end without it follows again.
    cch::tui::ScrollView view(
            std::make_unique<LabeledChild>(numbered_lines(6)), cch::tui::ScrollViewOptions{.follow_end = true});
    std::size_t render_requests = 0;
    view.update_layout(6, 3, [&render_requests] { ++render_requests; });

    CHECK(view.content_height() == 6);
    CHECK(view.viewport_height() == 3);
    CHECK(view.scroll_top() == 3);
    CHECK(view.following_end());

    view.scroll_to(99);
    CHECK(view.scroll_top() == 3);
    CHECK(view.following_end());

    view.scroll_to(-4);
    CHECK(view.scroll_top() == 0);
    CHECK_FALSE(view.following_end());

    view.scroll_to(3);
    CHECK(view.scroll_top() == 3);
    CHECK(view.following_end());

    view.scroll_to(3, /*disable_follow=*/true);
    CHECK(view.scroll_top() == 3);
    CHECK_FALSE(view.following_end());
    // The suppression survives a measurement at the same heights, and content
    // growth that leaves the viewport away from the end releases it.
    view.update_layout(6, 3);
    CHECK_FALSE(view.following_end());
    view.update_layout(7, 3);
    CHECK(view.scroll_top() == 3);
    CHECK_FALSE(view.following_end());
    CHECK(render_requests > 0);

    view.scroll_to(4);
    CHECK(view.scroll_top() == 4);
    CHECK(view.following_end());

    view.scroll_to_start();
    CHECK(view.scroll_top() == 0);
    CHECK_FALSE(view.following_end());
    view.scroll_to_end();
    CHECK(view.scroll_top() == 4);
    CHECK(view.following_end());

    // Content that fits entirely in the window keeps follow-end settled at row
    // zero (pi `scrollToStart` follow test).
    view.update_layout(2, 3);
    CHECK(view.scroll_top() == 0);
    CHECK(view.following_end());
    view.scroll_to(0, /*disable_follow=*/true);
    CHECK_FALSE(view.following_end());
    view.scroll_to_start();
    CHECK(view.following_end());
}

TEST_CASE(
        "ScrollView reports unused scroll deltas and tracks follow-end transitions", "[tui][scroll-view][issue1000]") {
    // Frozen `packages/tui/test/layout.test.ts:170-189`: a 10x3 frame over six
    // rows follows the end at row three, and every `scrollBy` returns the lines
    // it could not consume.
    cch::tui::ScrollView view(std::make_unique<cch::tui::Text>("1\n2\n3\n4\n5\n6", 0, 0),
            cch::tui::ScrollViewOptions{.follow_end = true, .primary = true});
    auto frame = render_frame(view, 10, 3);
    REQUIRE(frame);
    CHECK(drawn_lines(*frame) == std::vector<std::string>{"4", "5", "6"});
    CHECK(view.scroll_top() == 3);
    CHECK(view.following_end());
    CHECK(view.viewport_height() == 3);
    CHECK(view.content_height() == 6);
    REQUIRE(frame->root.children.size() == 1);
    CHECK(frame->root.scroll_view == &view);
    CHECK(frame->root.children.front().rect.height == 6);
    CHECK(frame->root.children.front().rect.y == -3);
    CHECK(frame->primary_scroll_view == &view);

    CHECK(view.scroll_by(-2) == 0);
    CHECK(view.scroll_top() == 1);
    CHECK_FALSE(view.following_end());

    CHECK(view.scroll_by(-3) == -2);
    CHECK(view.scroll_top() == 0);

    CHECK(view.scroll_by(10) == 7);
    CHECK(view.scroll_top() == 3);
    CHECK(view.following_end());
}

TEST_CASE("ScrollView follows appended rows at the end and keeps a manual position away from it",
        "[tui][scroll-view][issue1000]") {
    // Frozen `packages/tui/test/layout.test.ts:250-258`: appending one row while
    // following moves the window down by one row.
    auto text = std::make_unique<cch::tui::Text>("1\n2\n3\n4\n5", 0, 0);
    auto* text_pointer = text.get();
    cch::tui::ScrollView view(std::move(text), cch::tui::ScrollViewOptions{.follow_end = true});

    auto frame = render_frame(view, 6, 3);
    REQUIRE(frame);
    CHECK(view.scroll_top() == 2);
    CHECK(drawn_lines(*frame) == std::vector<std::string>{"3", "4", "5"});

    text_pointer->set_text("1\n2\n3\n4\n5\n6");
    frame = render_frame(view, 6, 3);
    REQUIRE(frame);
    CHECK(view.scroll_top() == 3);
    CHECK(drawn_lines(*frame) == std::vector<std::string>{"4", "5", "6"});

    // Away from the end the manual position survives appended rows (pi
    // `tui-alt-screen.test.ts:63-90` keeps the manual position when the
    // document grows), and shrinking content clamps it back.
    view.scroll_to(1);
    CHECK_FALSE(view.following_end());
    text_pointer->set_text("1\n2\n3\n4\n5\n6\n7");
    frame = render_frame(view, 6, 3);
    REQUIRE(frame);
    CHECK(view.scroll_top() == 1);
    CHECK(drawn_lines(*frame) == std::vector<std::string>{"2", "3", "4"});

    text_pointer->set_text("1\n2");
    frame = render_frame(view, 6, 3);
    REQUIRE(frame);
    CHECK(view.scroll_top() == 0);
    CHECK(drawn_lines(*frame) == std::vector<std::string>{"1", "2", ""});
}

TEST_CASE("ScrollView clamps the window across content and viewport resizes", "[tui][scroll-view][issue1000]") {
    auto text = std::make_unique<cch::tui::Text>("1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n12", 0, 0);
    cch::tui::ScrollView view(std::move(text), cch::tui::ScrollViewOptions{.follow_end = true});

    auto frame = render_frame(view, 10, 4);
    REQUIRE(frame);
    CHECK(view.scroll_top() == 8);
    CHECK(drawn_lines(*frame) == std::vector<std::string>{"9", "10", "11", "12"});

    frame = render_frame(view, 10, 6);
    REQUIRE(frame);
    CHECK(view.scroll_top() == 6);
    CHECK(drawn_lines(*frame) == std::vector<std::string>{"7", "8", "9", "10", "11", "12"});

    // A window taller than the content settles at row zero and keeps following.
    frame = render_frame(view, 10, 20);
    REQUIRE(frame);
    CHECK(view.scroll_top() == 0);
    CHECK(view.following_end());

    frame = render_frame(view, 10, 3);
    REQUIRE(frame);
    CHECK(view.scroll_top() == 9);

    // A manual position is clamped by a window that no longer leaves room.
    view.scroll_to(5);
    CHECK_FALSE(view.following_end());
    frame = render_frame(view, 10, 12);
    REQUIRE(frame);
    CHECK(view.scroll_top() == 0);
    CHECK(view.following_end());
}

TEST_CASE("Nested ScrollViews measure the inner window and return the unused delta to the enclosing viewport",
        "[tui][scroll-view][issue1000]") {
    // Frozen `packages/tui/test/layout.test.ts:346-353`: the inner viewport is
    // allocated its stack basis, and a delta the outer viewport cannot consume
    // is what an enclosing viewport receives.
    auto inner = std::make_unique<cch::tui::ScrollView>(std::make_unique<cch::tui::Text>("1\n2\n3\n4\n5\n6", 0, 0));
    auto* inner_pointer = inner.get();
    cch::tui::VStack content;
    REQUIRE(content.add_child(std::move(inner), cch::tui::StackEntryOptions{.basis = 2}));
    REQUIRE(content.add_child(std::make_unique<cch::tui::Text>("tail", 0, 0)));
    cch::tui::ScrollView outer(std::make_unique<cch::tui::VStack>(std::move(content)));

    auto frame = render_frame(outer, 10, 2);
    REQUIRE(frame);
    CHECK(inner_pointer->viewport_height() == 2);
    CHECK(inner_pointer->content_height() == 6);
    CHECK(outer.content_height() == 3);
    CHECK(outer.viewport_height() == 2);
    CHECK(drawn_lines(*frame) == std::vector<std::string>{"1", "2"});

    CHECK(outer.scroll_by(10) == 9);
    CHECK(outer.scroll_top() == 1);

    frame = render_frame(outer, 10, 2);
    REQUIRE(frame);
    CHECK(drawn_lines(*frame) == std::vector<std::string>{"2", "tail"});
}

TEST_CASE("Nested viewports propagate a scroll delta deepest first and stop at overscroll contain",
        "[tui][scroll-view][issue1000]") {
    // pi `layout.ts:436-448` (`getScrollViewsAt`) orders the viewports deepest
    // first and `tui-alt-screen.ts:983-996` (`routeWheel`) offers the delta to
    // each in that order, stopping when a viewport declares `contain`.
    auto inner = std::make_unique<cch::tui::ScrollView>(std::make_unique<cch::tui::Text>("1\n2\n3\n4", 0, 0),
            cch::tui::ScrollViewOptions{.follow_end = true, .overscroll = cch::tui::ScrollOverscroll::Contain});
    auto* inner_pointer = inner.get();
    cch::tui::VStack content;
    REQUIRE(content.add_child(std::make_unique<cch::tui::Text>("head", 0, 0)));
    REQUIRE(content.add_child(std::move(inner), cch::tui::StackEntryOptions{.basis = 2}));
    REQUIRE(content.add_child(std::make_unique<cch::tui::Text>("tail", 0, 0)));
    cch::tui::ScrollView outer(std::make_unique<cch::tui::VStack>(std::move(content)));

    auto frame = render_frame(outer, 10, 3);
    REQUIRE(frame);
    CHECK(inner_pointer->scroll_top() == 2);
    CHECK(outer.scroll_top() == 0);

    const auto at_inner = cch::tui::detail::scroll_views_at(*frame, 0, 2);
    REQUIRE(at_inner.size() == 2);
    CHECK(at_inner[0] == inner_pointer);
    CHECK(at_inner[1] == &outer);
    const auto at_header = cch::tui::detail::scroll_views_at(*frame, 0, 0);
    REQUIRE(at_header.size() == 1);
    CHECK(at_header[0] == &outer);
    CHECK(cch::tui::detail::scroll_views_at(*frame, 0, 3).empty());

    // The inner viewport follows the end and cannot consume the delta, so the
    // chain stops there and the outer viewport never moves.
    std::ptrdiff_t remaining = 10;
    for (auto* viewport : at_inner) {
        remaining = viewport->scroll_by(remaining);
        if (remaining == 0 || viewport->overscroll() == cch::tui::ScrollOverscroll::Contain) break;
    }
    CHECK(remaining == 10);
    CHECK(outer.scroll_top() == 0);

    // With the default `chain` the outer viewport receives exactly what the
    // inner one did not consume (pi's nested value: `outer.scrollBy(10) == 9`).
    remaining = 10;
    for (auto* viewport : at_inner) {
        remaining = viewport->scroll_by(remaining);
        if (remaining == 0) break;
    }
    CHECK(remaining == 9);
    CHECK(outer.scroll_top() == 1);

    frame = render_frame(outer, 10, 3);
    REQUIRE(frame);
    // The outer window moved one row: the inner viewport's first row scrolled
    // out and the tail row entered at the bottom.
    CHECK(drawn_lines(*frame) == std::vector<std::string>{"3", "4", "tail"});
}

TEST_CASE("A cursor in scrolled content is reported only while its row is inside the window",
        "[tui][scroll-view][issue1000]") {
    // pi `layout.ts:111-125,230-248` crops a leaf's rows around the marker and
    // `extractCursorPosition` can only find a marker a painted row carries, so a
    // cursor scrolled out of the window is not reported.
    auto child = std::make_unique<CursorChild>(numbered_lines(6), 1, 2);
    auto* child_pointer = child.get();
    cch::tui::ScrollView view(std::move(child), cch::tui::ScrollViewOptions{.follow_end = true});

    auto frame = render_frame(view, 8, 3);
    REQUIRE(frame);
    CHECK(view.scroll_top() == 3);
    CHECK_FALSE(frame->cursor);

    view.scroll_to_start();
    frame = render_frame(view, 8, 3);
    REQUIRE(frame);
    REQUIRE(frame->cursor);
    CHECK(frame->cursor->row == 1);
    CHECK(frame->cursor->column == 2);

    view.scroll_to(1);
    frame = render_frame(view, 8, 3);
    REQUIRE(frame);
    REQUIRE(frame->cursor);
    CHECK(frame->cursor->row == 0);
    CHECK(frame->cursor->column == 2);

    // A reported row the frame does not paint carries no marker.
    child_pointer->set_cursor(9, 2);
    frame = render_frame(view, 8, 3);
    REQUIRE(frame);
    CHECK_FALSE(frame->cursor);
}

TEST_CASE("Scrolled image placements keep clipped destination rows and the hidden source offset",
        "[tui][scroll-view][issue1000]") {
    // A placement whose top rows scrolled above the window keeps its visible
    // part at the window's first row and reports the hidden source rows
    // (pi `layout.ts:359-373` writes the cropped line at `box.rect.y`).
    {
        cch::tui::ScrollView view(std::make_unique<ImageChild>(numbered_lines(6), reserved_image(2, 2, 2)),
                cch::tui::ScrollViewOptions{.follow_end = true});
        auto frame = render_frame(view, 8, 3);
        REQUIRE(frame);
        CHECK(view.scroll_top() == 3);
        REQUIRE(frame->images.size() == 1);
        CHECK(frame->images[0].region.row == 0);
        CHECK(frame->images[0].region.rows == 1);
        CHECK(frame->images[0].source_row_offset == 1);
    }

    // A placement whose bottom rows pass the window's last row keeps the rows
    // that fit and no source offset (pi `layout.test.ts:131-148`:
    // `cropKittyImageLine(line, 0, visibleRows)`).
    {
        cch::tui::ScrollView view(std::make_unique<ImageChild>(numbered_lines(6), reserved_image(4, 2, 3)),
                cch::tui::ScrollViewOptions{.follow_end = true});
        auto frame = render_frame(view, 8, 3);
        REQUIRE(frame);
        CHECK(view.scroll_top() == 3);
        REQUIRE(frame->images.size() == 1);
        CHECK(frame->images[0].region.row == 1);
        CHECK(frame->images[0].region.rows == 2);
        CHECK(frame->images[0].source_row_offset == 0);
    }

    // A placement entirely above the window is not part of the frame.
    {
        cch::tui::ScrollView view(std::make_unique<ImageChild>(numbered_lines(6), reserved_image(0, 2, 2)),
                cch::tui::ScrollViewOptions{.follow_end = true});
        auto frame = render_frame(view, 8, 3);
        REQUIRE(frame);
        CHECK(view.scroll_top() == 3);
        CHECK(frame->images.empty());
    }
}

TEST_CASE("AltScreen shows the implicit document's end and stops following after a scroll",
        "[tui][alt-screen][scroll-view][issue1000]") {
    // Frozen `packages/tui/test/tui-alt-screen.test.ts:63-90`: children mounted
    // without a layout root are one document behind an implicit follow-end
    // viewport, a programmatic scroll stops following, and appended rows do not
    // restore the manual position.
    cch::tui::VirtualTerminal terminal({.columns = 20, .rows = 4});
    cch::tui::AltScreen tui(terminal);
    auto document = std::make_unique<cch::tui::Text>("1\n2\n3\n4\n5\n6\n7\n8\n9\n10", 0, 0);
    auto* document_pointer = document.get();
    REQUIRE(tui.add_child(std::move(document)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());
    CHECK(trimmed_rows(terminal) == std::vector<std::string>{"7", "8", "9", "10"});
    CHECK(tui.viewport_top() == 6);
    CHECK(tui.is_following_output());

    tui.scroll_by(-4);
    REQUIRE(tui.render());
    CHECK(trimmed_rows(terminal) == std::vector<std::string>{"3", "4", "5", "6"});
    CHECK(tui.viewport_top() == 2);
    CHECK_FALSE(tui.is_following_output());

    document_pointer->set_text("1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n12");
    REQUIRE(tui.render());
    CHECK(tui.viewport_top() == 2);
    CHECK(trimmed_rows(terminal) == std::vector<std::string>{"3", "4", "5", "6"});

    tui.scroll_to_bottom();
    REQUIRE(tui.render());
    CHECK(tui.viewport_top() == 8);
    CHECK(tui.is_following_output());
    CHECK(trimmed_rows(terminal) == std::vector<std::string>{"9", "10", "11", "12"});

    tui.scroll_to_top();
    REQUIRE(tui.render());
    CHECK(tui.viewport_top() == 0);
    CHECK_FALSE(tui.is_following_output());
    CHECK(trimmed_rows(terminal) == std::vector<std::string>{"1", "2", "3", "4"});
    CHECK(terminal.scrollback().empty());
}

TEST_CASE(
        "AltScreen clamps the implicit viewport across terminal resizes", "[tui][alt-screen][scroll-view][issue1000]") {
    cch::tui::VirtualTerminal terminal({.columns = 20, .rows = 4});
    cch::tui::AltScreen tui(terminal);
    REQUIRE(tui.add_child(std::make_unique<cch::tui::Text>("1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n12\n13\n14\n15\n16\n17\n"
                                                           "18\n19\n20\n21\n22\n23\n24\n25\n26\n27\n28\n29\n30",
            0,
            0)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());
    CHECK(tui.viewport_top() == 26);
    CHECK(trimmed_rows(terminal).front() == "27");

    REQUIRE(terminal.inject_resize(cch::tui::TerminalDimensions{.columns = 20, .rows = 6}));
    REQUIRE(tui.render());
    CHECK(tui.viewport_top() == 24);
    CHECK(trimmed_rows(terminal).front() == "25");

    REQUIRE(terminal.inject_resize(cch::tui::TerminalDimensions{.columns = 20, .rows = 2}));
    REQUIRE(tui.render());
    CHECK(tui.viewport_top() == 28);
    CHECK(trimmed_rows(terminal) == std::vector<std::string>{"29", "30"});

    // A manual position is re-clamped by a taller window and lands on the
    // content end, where the viewport follows again (pi `scroll-view.ts:196-201`
    // re-enables follow when the clamp reaches the end).
    tui.scroll_by(-10);
    CHECK(tui.viewport_top() == 18);
    REQUIRE(terminal.inject_resize(cch::tui::TerminalDimensions{.columns = 20, .rows = 20}));
    REQUIRE(tui.render());
    CHECK(tui.viewport_top() == 10);
    CHECK(tui.is_following_output());
}

TEST_CASE("AltScreen reports a cursor only while its row is inside the implicit viewport",
        "[tui][alt-screen][scroll-view][issue1000]") {
    cch::tui::VirtualTerminal terminal({.columns = 20, .rows = 4});
    cch::tui::AltScreen tui(terminal, true);
    auto child = std::make_unique<CursorChild>(numbered_lines(10), 2, 1);
    auto* child_pointer = child.get();
    REQUIRE(tui.add_child(std::move(child)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());
    // Following the document end scrolled the cursor's row above the window, so
    // no painted row carries it.
    CHECK(tui.viewport_top() == 6);
    CHECK_FALSE(terminal.modes().cursor_visible);

    tui.scroll_to_top();
    REQUIRE(tui.render());
    CHECK(terminal.modes().cursor_visible);
    CHECK(terminal.cursor().row == 2);
    CHECK(terminal.cursor().column == 1);

    child_pointer->set_cursor(1, 3);
    tui.scroll_by(1);
    REQUIRE(tui.render());
    CHECK(tui.viewport_top() == 1);
    CHECK(terminal.modes().cursor_visible);
    CHECK(terminal.cursor().row == 0);
    CHECK(terminal.cursor().column == 3);
}
