// HStack entry allocation, row composition and the constrained horizontal
// layout node (issue #999).
//
// Every expected value below is derived from the frozen pi v1.0.4 revision
// 7c10bd4337495ee613f2224843ecdf349b80d1df: `packages/tui/src/components/stack.ts`,
// `packages/tui/src/components/h-stack.ts`, `packages/tui/src/layout.ts`
// (the hstack node), and the independent expectations in
// `packages/tui/test/layout.test.ts`. The composed rows are compared as visible
// text, pi's own `visibleLines` convention.

#include <cch/tui/HStack.hpp>
#include <cch/tui/Stack.hpp>
#include <cch/tui/Utils.hpp>
#include <cch/tui/VStack.hpp>

#include "tui/Layout.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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

    void set_lines(std::vector<std::string> lines) { lines_ = std::move(lines); }

    [[nodiscard]] cch::support::Expected<cch::tui::RenderResult> render(std::size_t width) override {
        ++render_count;
        rendered_width = width;
        rendered_widths.push_back(width);
        return cch::tui::RenderResult{.lines = lines_};
    }

    void invalidate() override { ++invalidate_count; }

    std::size_t render_count{0};
    std::size_t rendered_width{0};
    std::size_t invalidate_count{0};
    std::vector<std::size_t> rendered_widths{};

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
        ++render_count;
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

    std::size_t render_count{0};
    std::size_t rendered_width{0};

private:
    std::vector<std::string> lines_;
    std::size_t cursor_row_{0};
    std::size_t cursor_column_{0};
    bool focused_{false};
};

class ImageChild final : public cch::tui::Component {
public:
    ImageChild(std::vector<std::string> lines, cch::tui::InlineImageRenderRegion image)
        : lines_(std::move(lines)), image_(std::move(image)) {}

    [[nodiscard]] cch::support::Expected<cch::tui::RenderResult> render(std::size_t width) override {
        ++render_count;
        rendered_width = width;
        return cch::tui::RenderResult{.lines = lines_, .images = {image_}};
    }

    void invalidate() override {}

    std::size_t render_count{0};
    std::size_t rendered_width{0};

private:
    std::vector<std::string> lines_;
    cch::tui::InlineImageRenderRegion image_;
};

/// A leaf that respects its render width — the C++ shape of the capture's
/// probing leaf and of pi's `Text` at a width that fits its lines.
class FillChild final : public cch::tui::Component {
public:
    FillChild(char fill, std::size_t limit) : fill_(fill), limit_(limit) {}

    [[nodiscard]] cch::support::Expected<cch::tui::RenderResult> render(std::size_t width) override {
        rendered_widths.push_back(width);
        return cch::tui::RenderResult{.lines = {std::string(std::min(width, limit_), fill_)}};
    }

    void invalidate() override {}

    std::vector<std::size_t> rendered_widths{};

private:
    char fill_{'x'};
    std::size_t limit_{0};
};

/// The allocated box tree of one frame, compared recursively so a nested
/// stack's own children are part of the expectation.
struct BoxTree {
    std::ptrdiff_t x{0};
    std::ptrdiff_t y{0};
    std::size_t width{0};
    std::size_t height{0};
    std::size_t line_offset{0};
    std::vector<BoxTree> children{};

    bool operator==(const BoxTree&) const = default;
};

[[nodiscard]] BoxTree box_tree(const cch::tui::detail::LayoutBox& box) {
    BoxTree node{.x = box.rect.x,
            .y = box.rect.y,
            .width = box.rect.width,
            .height = box.rect.height,
            .line_offset = box.line_offset,
            .children = {}};
    node.children.reserve(box.children.size());
    for (const auto& child : box.children)
        node.children.push_back(box_tree(child));
    return node;
}

[[nodiscard]] std::string to_text(const BoxTree& box) {
    std::string text = "(" + std::to_string(box.x) + "," + std::to_string(box.y) + " " + std::to_string(box.width) +
                       "x" + std::to_string(box.height) + " off" + std::to_string(box.line_offset);
    for (const auto& child : box.children)
        text += " " + to_text(child);
    return text + ")";
}

} // namespace

namespace Catch {
template <> struct StringMaker<BoxTree> {
    [[nodiscard]] static std::string convert(const BoxTree& box) { return to_text(box); }
};
} // namespace Catch

namespace {
[[nodiscard]] cch::support::Expected<cch::tui::detail::LayoutFrame> render_frame(
        cch::tui::Component& root, std::size_t width, std::size_t height) {
    return cch::tui::detail::LayoutEngine::render(root, width, height);
}

/// The painted rows as visible text, pi's own `visibleLines` convention
/// (`test/layout.test.ts:11-13`): the shared splice closes and reopens only
/// the style state it tracks (see `LineCompositor.hpp`), a composed row is
/// padded to the frame width, and neither is part of what the terminal
/// shows.
[[nodiscard]] std::vector<std::string> visible_lines(const std::vector<std::string>& lines) {
    std::vector<std::string> result;
    result.reserve(lines.size());
    for (const auto& line : lines) {
        auto visible = cch::tui::strip_terminal_sequences(line);
        while (!visible.empty() && visible.back() == ' ')
            visible.pop_back();
        result.push_back(std::move(visible));
    }
    return result;
}

[[nodiscard]] cch::tui::InlineImageRenderRegion image_at(std::size_t column, std::size_t row) {
    cch::tui::InlineImageRenderRegion image;
    image.region = cch::tui::CellRegion{.column = column, .row = row, .columns = 2, .rows = 1};
    image.fallback_text = "im";
    image.rows_reserved_in_lines = false;
    return image;
}

} // namespace

TEST_CASE("HStack composes every entry at its allocated columns", "[tui][hstack][issue999]") {
    // Frozen `h-stack.ts:13-43`: the entries are measured at the requested
    // width, `allocateStackSizes` clamps and distributes, a zero-width entry is
    // skipped, and each rendered child covers exactly its own columns of the
    // shared row.
    cch::tui::HStack stack({.gap = 1});
    auto left = std::make_unique<LabeledChild>(std::vector<std::string>{"aa"});
    auto hidden = std::make_unique<LabeledChild>(std::vector<std::string>{"hidden"});
    auto right = std::make_unique<LabeledChild>(std::vector<std::string>{"bb"});
    auto* left_child = left.get();
    auto* hidden_child = hidden.get();
    auto* right_child = right.get();
    REQUIRE(stack.add_child(std::move(left), cch::tui::StackEntryOptions{.basis = 4, .shrink = 0}));
    REQUIRE(stack.add_child(std::move(hidden), cch::tui::StackEntryOptions{.basis = 0, .shrink = 0}));
    REQUIRE(stack.add_child(std::move(right), cch::tui::StackEntryOptions{.basis = 4, .grow = 1}));

    const auto rendered = stack.render(10);
    REQUIRE(rendered);
    // Columns: "aa" 0-1, gap 2-3 (the zero-width entry still consumes its gap
    // column), "bb" 6-7.
    CHECK(visible_lines(rendered->lines) == std::vector<std::string>{"aa    bb"});
    CHECK(left_child->rendered_widths == std::vector<std::size_t>{10, 4});
    // A zero-width entry is measured once at the requested width and never
    // rendered.
    CHECK(hidden_child->rendered_widths == std::vector<std::size_t>{10});
    CHECK(right_child->rendered_widths == std::vector<std::size_t>{10, 4});
}

TEST_CASE("HStack carries align across the composed rows", "[tui][hstack][issue999]") {
    // Frozen `h-stack.ts:32-34`: a centered or end-aligned entry is offset by
    // its own line count against the tallest entry, without moving the column
    // allocation.
    const auto stack_of = [](cch::tui::StackAlign align) {
        auto stack = std::make_unique<cch::tui::HStack>(cch::tui::StackOptions{.gap = 1, .align = align});
        REQUIRE(stack->add_child(std::make_unique<LabeledChild>(std::vector<std::string>{"left"}),
                cch::tui::StackEntryOptions{.basis = 4, .shrink = 0}));
        REQUIRE(stack->add_child(std::make_unique<LabeledChild>(std::vector<std::string>{"x", "y", "z"}),
                cch::tui::StackEntryOptions{.basis = 2, .grow = 1, .shrink = 0}));
        REQUIRE(stack->add_child(std::make_unique<LabeledChild>(std::vector<std::string>{"hidden"}),
                cch::tui::StackEntryOptions{.basis = 0, .shrink = 0}));
        REQUIRE(stack->add_child(std::make_unique<LabeledChild>(std::vector<std::string>{"rt"}),
                cch::tui::StackEntryOptions{.basis = 5, .shrink = 1, .min_size = 1}));
        return stack;
    };

    const auto start = stack_of(cch::tui::StackAlign::Start)->render(12);
    REQUIRE(start);
    CHECK(visible_lines(start->lines) == std::vector<std::string>{"left x   rt", "     y", "     z"});

    const auto center = stack_of(cch::tui::StackAlign::Center)->render(12);
    REQUIRE(center);
    CHECK(visible_lines(center->lines) == std::vector<std::string>{"     x", "left y   rt", "     z"});

    const auto end = stack_of(cch::tui::StackAlign::End)->render(12);
    REQUIRE(end);
    CHECK(visible_lines(end->lines) == std::vector<std::string>{"     x", "     y", "left z   rt"});
}

TEST_CASE("HStack grows and clamps entries exactly as the frozen pi stack does", "[tui][hstack][issue999]") {
    // Frozen `stack.ts:89-153`: `grow` fills the difference up to `maxSize`,
    // `shrink * max(1, size)` takes it back saturating at `minSize`, and a
    // measured width above `maxSize` is clamped before growth.
    cch::tui::HStack grow({.gap = 1});
    auto flexible = std::make_unique<LabeledChild>(std::vector<std::string>{"a"});
    auto fixed = std::make_unique<LabeledChild>(std::vector<std::string>{"bb"});
    auto empty = std::make_unique<LabeledChild>(std::vector<std::string>{""});
    auto* flexible_child = flexible.get();
    auto* empty_child = empty.get();
    REQUIRE(grow.add_child(std::move(flexible), cch::tui::StackEntryOptions{.basis = 0, .grow = 1, .shrink = 0}));
    REQUIRE(grow.add_child(std::move(fixed), cch::tui::StackEntryOptions{.basis = 2, .shrink = 0}));
    REQUIRE(grow.add_child(std::move(empty), cch::tui::StackEntryOptions{.basis = 0, .shrink = 0}));

    const auto grown = grow.render(10);
    REQUIRE(grown);
    CHECK(visible_lines(grown->lines) == std::vector<std::string>{"a      bb"});
    CHECK(flexible_child->rendered_width == 6);
    CHECK(empty_child->rendered_width == 10);

    cch::tui::HStack clamps;
    auto below_minimum = std::make_unique<LabeledChild>(std::vector<std::string>{"a"});
    auto above_maximum = std::make_unique<LabeledChild>(std::vector<std::string>{"bb"});
    auto* below_minimum_child = below_minimum.get();
    auto* above_maximum_child = above_maximum.get();
    REQUIRE(clamps.add_child(
            std::move(below_minimum), cch::tui::StackEntryOptions{.basis = 0, .grow = 1, .min_size = 3}));
    REQUIRE(clamps.add_child(std::move(above_maximum), cch::tui::StackEntryOptions{.shrink = 0, .max_size = 2}));

    const auto clamped = clamps.render(8);
    REQUIRE(clamped);
    CHECK(visible_lines(clamped->lines) == std::vector<std::string>{"a     bb"});
    CHECK(below_minimum_child->rendered_width == 6);
    CHECK(above_maximum_child->rendered_width == 2);

    cch::tui::HStack zero_only;
    auto zero = std::make_unique<LabeledChild>(std::vector<std::string>{"z"});
    auto* zero_child = zero.get();
    REQUIRE(zero_only.add_child(std::move(zero), cch::tui::StackEntryOptions{.basis = 0, .shrink = 0}));
    const auto nothing = zero_only.render(5);
    REQUIRE(nothing);
    CHECK(nothing->lines.empty());
    CHECK(zero_child->render_count == 1);
}

TEST_CASE("HStack reports the first entry cursor that survives its allocation", "[tui][hstack][issue999]") {
    // The toolkit's standalone cursor seam: the entry's own cursor is
    // translated into the composed row, and a row the entry did not render
    // cannot be the stack cursor (`VStack` reports the same way).
    cch::tui::HStack stack({.gap = 1});
    REQUIRE(stack.add_child(std::make_unique<LabeledChild>(std::vector<std::string>{"a"}),
            cch::tui::StackEntryOptions{.basis = 2, .shrink = 0}));
    auto editor = std::make_unique<CursorChild>(std::vector<std::string>{"b1", "b2"}, 1, 0);
    editor->set_focused(true);
    auto* editor_child = editor.get();
    REQUIRE(stack.add_child(std::move(editor), cch::tui::StackEntryOptions{.basis = 2, .shrink = 0}));
    auto next = std::make_unique<CursorChild>(std::vector<std::string>{"c1"}, 0, 1);
    next->set_focused(true);
    auto* next_child = next.get();
    REQUIRE(stack.add_child(std::move(next), cch::tui::StackEntryOptions{.basis = 2, .shrink = 0}));

    REQUIRE(stack.render(8));
    const auto cursor = stack.cursor_location();
    REQUIRE(cursor);
    CHECK(cursor->row == 1);
    CHECK(cursor->column == 3);

    // The editor no longer reports the cropped row, so the next entry owns it.
    editor_child->set_lines(std::vector<std::string>{"b1"}, 1);
    REQUIRE(stack.render(8));
    const auto moved = stack.cursor_location();
    REQUIRE(moved);
    CHECK(moved->row == 0);
    CHECK(moved->column == 7);

    // Blurring every entry clears the cursor, and invalidation drops the
    // placement record until the next render.
    editor_child->set_focused(false);
    next_child->set_focused(false);
    REQUIRE(stack.render(8));
    CHECK_FALSE(stack.cursor_location());
    next_child->set_focused(true);
    REQUIRE(stack.render(8));
    REQUIRE(stack.cursor_location());
    stack.invalidate();
    CHECK_FALSE(stack.cursor_location());
}

TEST_CASE("HStack translates image placements into the composed rows", "[tui][hstack][issue999]") {
    // Image sidecars are the toolkit's own seam: a placement is translated to
    // the entry's column and row offset, and one the allocation cannot hold is
    // dropped.
    cch::tui::HStack stack({.gap = 1});
    auto first = std::make_unique<ImageChild>(std::vector<std::string>{"a", "a"}, image_at(0, 0));
    auto second = std::make_unique<ImageChild>(std::vector<std::string>{"b"}, image_at(0, 0));
    auto dropped = std::make_unique<ImageChild>(std::vector<std::string>{"c"}, image_at(2, 0));
    REQUIRE(stack.add_child(std::move(first), cch::tui::StackEntryOptions{.basis = 3, .shrink = 0}));
    REQUIRE(stack.add_child(std::move(second), cch::tui::StackEntryOptions{.basis = 3, .shrink = 0}));
    REQUIRE(stack.add_child(std::move(dropped), cch::tui::StackEntryOptions{.basis = 3, .shrink = 0}));

    const auto rendered = stack.render(8);
    REQUIRE(rendered);
    REQUIRE(rendered->images.size() == 2);
    CHECK(rendered->images[0].region.column == 0);
    CHECK(rendered->images[0].region.row == 0);
    CHECK(rendered->images[1].region.column == 4);
    CHECK(rendered->images[1].region.row == 0);
}

TEST_CASE("HStack validates its attachments and rejects a zero width", "[tui][hstack][issue999]") {
    cch::tui::HStack stack;
    CHECK_FALSE(stack.add_child(nullptr));
    CHECK(stack.render(0).error().code == cch::support::ErrorCode::Validation);

    auto child = std::make_unique<LabeledChild>(std::vector<std::string>{"a"});
    auto* child_ptr = child.get();
    REQUIRE(stack.add_child(std::move(child)));
    CHECK(stack.contains(child_ptr));
    CHECK(stack.contains(&stack));

    auto removed = stack.remove_child(child_ptr);
    REQUIRE(removed);
    CHECK(removed.get() == child_ptr);
    CHECK_FALSE(stack.contains(child_ptr));
    CHECK(stack.remove_child(child_ptr) == nullptr);

    REQUIRE(stack.add_child(std::make_unique<LabeledChild>(std::vector<std::string>{"b"})));
    stack.clear();
    CHECK_FALSE(stack.contains(child_ptr));
}

TEST_CASE("a constrained frame composes sibling columns instead of overwriting them", "[tui][hstack][issue999]") {
    // pi `test/layout.test.ts:148-159` ("composes horizontal children at
    // allocated widths") is the independent expectation for the composed row;
    // the tree pins both allocations.
    cch::tui::HStack root;
    REQUIRE(root.add_child(std::make_unique<LabeledChild>(std::vector<std::string>{"left"}),
            cch::tui::StackEntryOptions{.basis = 6, .shrink = 0}));
    REQUIRE(root.add_child(std::make_unique<LabeledChild>(std::vector<std::string>{"right"}),
            cch::tui::StackEntryOptions{.basis = 6, .shrink = 0}));

    const auto frame = render_frame(root, 12, 1);
    REQUIRE(frame);
    CHECK(visible_lines(frame->lines) == std::vector<std::string>{"left  right"});
    CHECK(box_tree(frame->root) == BoxTree{.x = 0,
                                           .y = 0,
                                           .width = 12,
                                           .height = 1,
                                           .children = {BoxTree{.x = 0, .y = 0, .width = 6, .height = 1},
                                                   BoxTree{.x = 6, .y = 0, .width = 6, .height = 1}}});
}

TEST_CASE("a constrained frame skips zero-width horizontal children", "[tui][hstack][issue999]") {
    // pi `test/layout.test.ts:161-172` ("does not paint zero-width horizontal
    // children"): the width-0 entry is never rendered, so its component cannot
    // mark a row.
    cch::tui::HStack root;
    auto hidden = std::make_unique<LabeledChild>(std::vector<std::string>{"hidden"});
    auto shown = std::make_unique<LabeledChild>(std::vector<std::string>{"shown"});
    auto* hidden_child = hidden.get();
    REQUIRE(root.add_child(std::move(hidden), cch::tui::StackEntryOptions{.basis = 0, .shrink = 0}));
    REQUIRE(root.add_child(std::move(shown), cch::tui::StackEntryOptions{.basis = 0, .grow = 1}));

    const auto frame = render_frame(root, 5, 1);
    REQUIRE(frame);
    CHECK(visible_lines(frame->lines) == std::vector<std::string>{"shown"});
    // The width-0 entry is still measured for its height at width 1 — pi
    // `measureHeight(context, entry.component, Math.max(1, widths[index]))` —
    // but it is never laid out or painted.
    CHECK(hidden_child->render_count == 1);
    CHECK(hidden_child->rendered_width == 1);
    CHECK(box_tree(frame->root) == BoxTree{.x = 0,
                                           .y = 0,
                                           .width = 5,
                                           .height = 1,
                                           .children = {BoxTree{.x = 0, .y = 0, .width = 0, .height = 1},
                                                   BoxTree{.x = 0, .y = 0, .width = 5, .height = 1}}});
}

TEST_CASE("a constrained frame measures an auto basis at the frame width", "[tui][hstack][issue999]") {
    // Frozen `layout.ts:200-206`: an absent basis is measured at the frame
    // width while every child height is measured at its own allocated width, so
    // a width-dependent component reports two different renders.
    cch::tui::HStack root({.gap = 1});
    auto wide = std::make_unique<FillChild>('x', cch::tui::kStackUnboundedSize);
    auto narrow = std::make_unique<FillChild>('b', 2);
    auto* wide_child = wide.get();
    auto* narrow_child = narrow.get();
    REQUIRE(root.add_child(std::move(wide), cch::tui::StackEntryOptions{.basis = std::nullopt}));
    REQUIRE(root.add_child(std::move(narrow), cch::tui::StackEntryOptions{.basis = std::nullopt, .shrink = 1}));

    const auto frame = render_frame(root, 6, 1);
    REQUIRE(frame);
    CHECK(visible_lines(frame->lines) == std::vector<std::string>{"xxxx b"});
    CHECK(wide_child->rendered_widths == std::vector<std::size_t>{6, 4});
    CHECK(narrow_child->rendered_widths == std::vector<std::size_t>{6, 1});
    CHECK(box_tree(frame->root) == BoxTree{.x = 0,
                                           .y = 0,
                                           .width = 6,
                                           .height = 1,
                                           .children = {BoxTree{.x = 0, .y = 0, .width = 4, .height = 1},
                                                   BoxTree{.x = 5, .y = 0, .width = 1, .height = 1}}});
}

TEST_CASE("a constrained frame allocates a nested HStack from the width it was given", "[tui][hstack][issue999]") {
    // The product shape: a vertical root grows an HStack row above a fixed
    // footer, and the row allocates its own children against the frame width.
    cch::tui::VStack root;
    auto row = std::make_unique<cch::tui::HStack>();
    REQUIRE(row->add_child(std::make_unique<LabeledChild>(std::vector<std::string>{"l"}),
            cch::tui::StackEntryOptions{.basis = 3, .shrink = 0}));
    REQUIRE(row->add_child(std::make_unique<LabeledChild>(std::vector<std::string>{"r"}),
            cch::tui::StackEntryOptions{.basis = 3, .shrink = 0}));
    REQUIRE(root.add_child(std::move(row), cch::tui::StackEntryOptions{.basis = 0, .grow = 1, .min_size = 1}));
    REQUIRE(root.add_child(std::make_unique<LabeledChild>(std::vector<std::string>{"f"}),
            cch::tui::StackEntryOptions{.basis = 1, .shrink = 0}));

    const auto frame = render_frame(root, 10, 3);
    REQUIRE(frame);
    CHECK(visible_lines(frame->lines) == std::vector<std::string>{"l  r", "", "f"});
    CHECK(box_tree(frame->root) ==
            BoxTree{.x = 0,
                    .y = 0,
                    .width = 10,
                    .height = 3,
                    .children = {BoxTree{.x = 0,
                                         .y = 0,
                                         .width = 10,
                                         .height = 2,
                                         .children = {BoxTree{.x = 0, .y = 0, .width = 3, .height = 2},
                                                 BoxTree{.x = 3, .y = 0, .width = 3, .height = 2}}},
                            BoxTree{.x = 0, .y = 2, .width = 10, .height = 1}}});
}
