// VStack entry allocation and the constrained layout frame (issue #998).
//
// Every expected value below is derived from the frozen pi v1.0.4 revision
// 7c10bd4337495ee613f2224843ecdf349b80d1df: `packages/tui/src/components/stack.ts`,
// `packages/tui/src/components/v-stack.ts`, `packages/tui/src/layout.ts`, and
// the independent expectations in `packages/tui/test/layout.test.ts`.

#include <cch/tui/Stack.hpp>
#include <cch/tui/VStack.hpp>

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

    void set_lines(std::vector<std::string> lines) { lines_ = std::move(lines); }

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

[[nodiscard]] cch::support::Expected<cch::tui::detail::LayoutFrame> render_frame(
        cch::tui::Component& root, std::size_t width, std::size_t height) {
    return cch::tui::detail::LayoutEngine::render(root, width, height);
}

[[nodiscard]] std::vector<std::size_t> child_heights(const cch::tui::detail::LayoutBox& box) {
    std::vector<std::size_t> heights;
    heights.reserve(box.children.size());
    for (const auto& child : box.children)
        heights.push_back(child.rect.height);
    return heights;
}

[[nodiscard]] std::vector<std::size_t> child_rows(const cch::tui::detail::LayoutBox& box) {
    std::vector<std::size_t> rows;
    rows.reserve(box.children.size());
    for (const auto& child : box.children)
        rows.push_back(static_cast<std::size_t>(child.rect.y));
    return rows;
}

[[nodiscard]] cch::tui::InlineImageRenderRegion reserved_image() {
    cch::tui::InlineImageRenderRegion image;
    image.region = cch::tui::CellRegion{.column = 0, .row = 1, .columns = 2, .rows = 2};
    image.fallback_text = "im";
    image.rows_reserved_in_lines = true;
    return image;
}

} // namespace

TEST_CASE("VStack renders children in order with gap rows and numeric-basis padding", "[tui][vstack][issue998]") {
    // Frozen `v-stack.ts`: children render at `max(1, width)`, sizes come from
    // `allocateStackSizes` with no available size, and each child's lines are
    // sliced to the allocated size and padded with empty rows after `gap`
    // empty rows.
    cch::tui::VStack stack({.gap = 1});
    auto first = std::make_unique<LabeledChild>(std::vector<std::string>{"one"});
    auto* first_pointer = first.get();
    auto second = std::make_unique<LabeledChild>(std::vector<std::string>{"two", "three"});
    auto* second_pointer = second.get();
    REQUIRE(stack.add_child(std::move(first)));
    REQUIRE(stack.add_child(std::move(second), cch::tui::StackEntryOptions{.basis = 3}));

    auto rendered = stack.render(10);
    REQUIRE(rendered);
    CHECK(rendered->lines == std::vector<std::string>{"one", "", "two", "three", ""});
    CHECK(first_pointer->render_count == 1);
    CHECK(second_pointer->render_count == 1);
    CHECK(first_pointer->rendered_width == 10);
    CHECK(second_pointer->rendered_width == 10);
}

TEST_CASE("VStack omits gap rows around invisible entries", "[tui][vstack][issue998][spec]") {
    // Independent frozen pi observation (7c10bd43): `test/layout.test.ts:118-127`
    // expects ["one", "", "two"] for a stack whose middle entry is invisible.
    cch::tui::VStack stack({.gap = 1});
    REQUIRE(stack.add_child(std::make_unique<LabeledChild>(std::vector<std::string>{"one"})));
    REQUIRE(stack.add_child(std::make_unique<LabeledChild>(std::vector<std::string>{"hidden"}),
            cch::tui::StackEntryOptions{.visible = [](const cch::tui::LayoutViewport&) { return false; }}));
    REQUIRE(stack.add_child(std::make_unique<LabeledChild>(std::vector<std::string>{"two"})));

    auto rendered = stack.render(10);
    REQUIRE(rendered);
    CHECK(rendered->lines == std::vector<std::string>{"one", "", "two"});
}

TEST_CASE("VStack clamps entries to their min and max sizes when unbounded", "[tui][vstack][issue998]") {
    // Frozen `stack.ts` `clampSize`: the allocated size is the basis or measured
    // size clamped into [minSize, maxSize], so a short child gains padding rows
    // and a long child is cropped to its maximum.
    cch::tui::VStack stack;
    auto padded = std::make_unique<LabeledChild>(std::vector<std::string>{"a"});
    auto* padded_pointer = padded.get();
    auto cropped = std::make_unique<LabeledChild>(std::vector<std::string>{"x", "y", "z"});
    auto* cropped_pointer = cropped.get();
    REQUIRE(stack.add_child(std::move(padded), cch::tui::StackEntryOptions{.min_size = 2}));
    REQUIRE(stack.add_child(std::move(cropped), cch::tui::StackEntryOptions{.max_size = 1}));

    auto rendered = stack.render(4);
    REQUIRE(rendered);
    CHECK(rendered->lines == std::vector<std::string>{"a", "", "x"});
    // Clamping never changes the width a child renders at.
    CHECK(padded_pointer->rendered_width == 4);
    CHECK(cropped_pointer->rendered_width == 4);
}

TEST_CASE("VStack reports the first child cursor below earlier allocations and gaps", "[tui][vstack][issue998]") {
    cch::tui::VStack stack({.gap = 2});
    REQUIRE(stack.add_child(std::make_unique<LabeledChild>(std::vector<std::string>{"h1", "h2"})));
    auto editor = std::make_unique<CursorChild>(std::vector<std::string>{"e1", "e2"}, 1, 3);
    auto* editor_pointer = editor.get();
    REQUIRE(stack.add_child(std::move(editor)));
    editor_pointer->set_focused(true);

    auto rendered = stack.render(8);
    REQUIRE(rendered);
    CHECK(rendered->lines == std::vector<std::string>{"h1", "h2", "", "", "e1", "e2"});
    auto cursor = stack.cursor_location();
    REQUIRE(cursor);
    CHECK(cursor->column == 3);
    CHECK(cursor->row == 5);
}

TEST_CASE("VStack add, remove and clear keep entries and children aligned", "[tui][vstack][issue998]") {
    cch::tui::VStack stack;
    auto first = std::make_unique<LabeledChild>(std::vector<std::string>{"a"});
    auto second = std::make_unique<LabeledChild>(std::vector<std::string>{"b"});
    auto* first_pointer = first.get();
    auto* second_pointer = second.get();
    REQUIRE(stack.add_child(std::move(first)));
    REQUIRE(stack.add_child(std::move(second)));
    CHECK(stack.contains(first_pointer));
    CHECK(stack.contains(second_pointer));

    auto detached = stack.remove_child(first_pointer);
    REQUIRE(detached);
    CHECK_FALSE(stack.contains(first_pointer));
    CHECK(stack.remove_child(first_pointer) == nullptr);
    auto after_remove = stack.render(4);
    REQUIRE(after_remove);
    CHECK(after_remove->lines == std::vector<std::string>{"b"});

    detached->invalidate();
    REQUIRE(stack.add_child(std::move(detached)));
    stack.clear();
    CHECK_FALSE(stack.contains(first_pointer));
    auto cleared = stack.render(4);
    REQUIRE(cleared);
    CHECK(cleared->lines.empty());
}

TEST_CASE("VStack rejects a null child and a zero width", "[tui][vstack][issue998]") {
    cch::tui::VStack stack;
    auto rejected = stack.add_child(nullptr);
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().code == cch::support::ErrorCode::Validation);

    auto zero = stack.render(0);
    REQUIRE_FALSE(zero);
    CHECK(zero.error().code == cch::support::ErrorCode::Validation);
}

TEST_CASE("constrained VStack allocates vertical grow space deterministically", "[tui][layout][issue998][spec]") {
    // Independent frozen pi observation (7c10bd43): `test/layout.test.ts:13-27`
    // allocates [1, 3] for `{basis: 1, shrink: 0}` + `{basis: 0, grow: 1}` in a
    // 10x4 frame and paints ["top", "body", "", ""].
    cch::tui::VStack stack;
    REQUIRE(stack.add_child(std::make_unique<LabeledChild>(std::vector<std::string>{"top"}),
            cch::tui::StackEntryOptions{.basis = 1, .shrink = 0}));
    REQUIRE(stack.add_child(std::make_unique<LabeledChild>(std::vector<std::string>{"body"}),
            cch::tui::StackEntryOptions{.basis = 0, .grow = 1}));

    auto frame = render_frame(stack, 10, 4);
    REQUIRE(frame);
    CHECK(child_heights(frame->root) == std::vector<std::size_t>{1, 3});
    CHECK(child_rows(frame->root) == std::vector<std::size_t>{0, 1});
    CHECK(frame->lines == std::vector<std::string>{"top", "body", "", ""});
}

TEST_CASE("constrained VStack shrinks entries to their minimum sizes", "[tui][layout][issue998][spec]") {
    // Independent frozen pi observation (7c10bd43): `test/layout.test.ts:88-102`
    // shrinks the 3-line first entry to its `minSize: 1` and leaves the
    // `shrink: 0` entry at its natural size.
    cch::tui::VStack stack;
    REQUIRE(stack.add_child(std::make_unique<LabeledChild>(std::vector<std::string>{"a1", "a2", "a3"}),
            cch::tui::StackEntryOptions{.shrink = 1, .min_size = 1}));
    REQUIRE(stack.add_child(std::make_unique<LabeledChild>(std::vector<std::string>{"b1", "b2", "b3"}),
            cch::tui::StackEntryOptions{.shrink = 0}));

    auto frame = render_frame(stack, 10, 4);
    REQUIRE(frame);
    CHECK(child_heights(frame->root) == std::vector<std::size_t>{1, 3});
    CHECK(frame->lines == std::vector<std::string>{"a1", "b1", "b2", "b3"});
}

TEST_CASE("constrained VStack keeps shrink weights proportional to the allocated size", "[tui][layout][issue998]") {
    // Frozen `stack.ts` `distribute`: the shrink weight is
    // `shrink * max(1, size)`, so the 5-row entry gives back more cells than the
    // 3-row entry and the 4-row frame allocates [1, 3] instead of [0, 4].
    cch::tui::VStack stack;
    REQUIRE(stack.add_child(std::make_unique<LabeledChild>(std::vector<std::string>{"a1", "a2", "a3"})));
    REQUIRE(stack.add_child(std::make_unique<LabeledChild>(std::vector<std::string>{"b1", "b2", "b3", "b4", "b5"})));

    auto frame = render_frame(stack, 10, 4);
    REQUIRE(frame);
    CHECK(child_heights(frame->root) == std::vector<std::size_t>{1, 3});
    CHECK(frame->lines == std::vector<std::string>{"a1", "b1", "b2", "b3"});
}

TEST_CASE("constrained VStack saturates grow entries at their max size", "[tui][layout][issue998]") {
    // Frozen `stack.ts` `distribute`: a grow entry stops at `maxSize` and the
    // remaining cells keep flowing to the entries that can still grow.
    cch::tui::VStack stack;
    REQUIRE(stack.add_child(std::make_unique<LabeledChild>(std::vector<std::string>{"a"}),
            cch::tui::StackEntryOptions{.basis = 0, .grow = 1, .max_size = 2}));
    REQUIRE(stack.add_child(std::make_unique<LabeledChild>(std::vector<std::string>{"b"}),
            cch::tui::StackEntryOptions{.basis = 0, .grow = 1}));

    auto frame = render_frame(stack, 10, 8);
    REQUIRE(frame);
    CHECK(child_heights(frame->root) == std::vector<std::size_t>{2, 6});
    CHECK(frame->lines == std::vector<std::string>{"a", "", "b", "", "", "", "", ""});
}

TEST_CASE("constrained VStack keeps gap rows between allocated children", "[tui][layout][issue998]") {
    cch::tui::VStack stack({.gap = 2});
    REQUIRE(stack.add_child(std::make_unique<LabeledChild>(std::vector<std::string>{"a"}),
            cch::tui::StackEntryOptions{.basis = 1, .shrink = 0}));
    REQUIRE(stack.add_child(std::make_unique<LabeledChild>(std::vector<std::string>{"b"}),
            cch::tui::StackEntryOptions{.basis = 1, .grow = 1}));

    auto frame = render_frame(stack, 6, 6);
    REQUIRE(frame);
    CHECK(child_heights(frame->root) == std::vector<std::size_t>{1, 3});
    CHECK(child_rows(frame->root) == std::vector<std::size_t>{0, 3});
    CHECK(frame->lines == std::vector<std::string>{"a", "", "", "b", "", ""});
}

TEST_CASE("constrained VStack includes nested minimum sizes in intrinsic stack measurement",
        "[tui][layout][issue998][spec]") {
    // Independent frozen pi observation (7c10bd43): `test/layout.test.ts:104-131`
    // measures the dock through `VStack.render`, so the nested `minSize: 3`
    // selector contributes padding rows to the parent's auto basis and every
    // line lands exactly where pi puts it.
    auto dock = std::make_unique<cch::tui::VStack>();
    REQUIRE(dock->add_child(std::make_unique<LabeledChild>(std::vector<std::string>{"top1", "top2", "top3"})));
    REQUIRE(dock->add_child(std::make_unique<LabeledChild>(std::vector<std::string>{"selector"}),
            cch::tui::StackEntryOptions{.min_size = 3}));
    REQUIRE(dock->add_child(std::make_unique<LabeledChild>(std::vector<std::string>{"below"})));
    REQUIRE(dock->add_child(std::make_unique<LabeledChild>(std::vector<std::string>{"footer"}),
            cch::tui::StackEntryOptions{.min_size = 1}));

    cch::tui::VStack stack;
    REQUIRE(stack.add_child(std::make_unique<LabeledChild>(std::vector<std::string>{"body"}),
            cch::tui::StackEntryOptions{.basis = 0, .grow = 1, .min_size = 1}));
    REQUIRE(stack.add_child(std::move(dock), cch::tui::StackEntryOptions{.basis = std::nullopt, .min_size = 1}));

    auto frame = render_frame(stack, 10, 9);
    REQUIRE(frame);
    CHECK(child_heights(frame->root) == std::vector<std::size_t>{1, 8});
    CHECK(frame->lines ==
            std::vector<std::string>{"body", "top1", "top2", "top3", "selector", "", "", "below", "footer"});
}

TEST_CASE("constrained VStack keeps the cursor row visible inside a cropped leaf", "[tui][layout][issue998]") {
    // Frozen `layout.ts` leaf layout: when a leaf overflows its allocated
    // height, the rows above its cursor row are dropped (`lineOffset`), and the
    // cursor is translated into the cropped box. The `shrink: 0` footer cannot
    // give cells back, so the total allocation may exceed a frame that cannot
    // hold every minimum.
    cch::tui::VStack stack;
    auto editor = std::make_unique<CursorChild>(std::vector<std::string>{"e1", "e2", "e3", "e4", "e5"}, 4, 1);
    auto* editor_pointer = editor.get();
    REQUIRE(stack.add_child(std::move(editor), cch::tui::StackEntryOptions{.shrink = 1, .min_size = 3}));
    REQUIRE(stack.add_child(std::make_unique<LabeledChild>(std::vector<std::string>{"f1", "f2"}),
            cch::tui::StackEntryOptions{.shrink = 0}));
    editor_pointer->set_focused(true);

    auto frame = render_frame(stack, 6, 4);
    REQUIRE(frame);
    CHECK(child_heights(frame->root) == std::vector<std::size_t>{3, 2});
    CHECK(frame->root.children[0].line_offset == 2);
    CHECK(frame->root.children[1].clip.height == 1);
    CHECK(frame->lines == std::vector<std::string>{"e3", "e4", "e5", "f1"});
    REQUIRE(frame->cursor);
    CHECK(frame->cursor->row == 2);
    CHECK(frame->cursor->column == 1);
}

TEST_CASE("constrained VStack carries align without changing the vertical axis", "[tui][layout][issue998]") {
    // Frozen `v-stack.ts` never reads `StackOptions.align`: alignment is a
    // cross-axis option and a vertical stack has no cross-axis policy, so the
    // same entries allocate identically under every alignment.
    std::vector<cch::tui::detail::LayoutFrame> frames;
    for (const auto align : {cch::tui::StackAlign::Stretch,
                 cch::tui::StackAlign::Start,
                 cch::tui::StackAlign::Center,
                 cch::tui::StackAlign::End}) {
        cch::tui::VStack stack({.align = align});
        REQUIRE(stack.add_child(std::make_unique<LabeledChild>(std::vector<std::string>{"a"}),
                cch::tui::StackEntryOptions{.basis = 1, .shrink = 0}));
        REQUIRE(stack.add_child(std::make_unique<LabeledChild>(std::vector<std::string>{"b"}),
                cch::tui::StackEntryOptions{.basis = 0, .grow = 1}));
        auto frame = render_frame(stack, 6, 4);
        REQUIRE(frame);
        frames.push_back(std::move(*frame));
    }
    for (const auto& frame : frames) {
        CHECK(child_heights(frame.root) == std::vector<std::size_t>{1, 3});
        CHECK(frame.lines == std::vector<std::string>{"a", "b", "", ""});
    }
}

TEST_CASE("constrained VStack honors the responsive visibility predicate", "[tui][layout][issue998]") {
    // Frozen `visibleStackEntries` evaluates `visible` with the allocation's
    // viewport, so the same stack grows or drops an entry with the frame.
    cch::tui::VStack stack;
    REQUIRE(stack.add_child(std::make_unique<LabeledChild>(std::vector<std::string>{"head"})));
    REQUIRE(stack.add_child(std::make_unique<LabeledChild>(std::vector<std::string>{"menu"}),
            cch::tui::StackEntryOptions{
                    .visible = [](const cch::tui::LayoutViewport& viewport) { return viewport.height >= 5; }}));
    REQUIRE(stack.add_child(std::make_unique<LabeledChild>(std::vector<std::string>{"foot"})));

    auto tall = render_frame(stack, 6, 5);
    REQUIRE(tall);
    CHECK(child_heights(tall->root) == std::vector<std::size_t>{1, 1, 1});
    CHECK(tall->lines == std::vector<std::string>{"head", "menu", "foot", "", ""});

    auto short_frame = render_frame(stack, 6, 4);
    REQUIRE(short_frame);
    CHECK(child_heights(short_frame->root) == std::vector<std::size_t>{1, 1});
    CHECK(short_frame->lines == std::vector<std::string>{"head", "foot", "", ""});
}

TEST_CASE("constrained VStack translates and clips image placements", "[tui][layout][issue998][issue1000]") {
    // Frozen `layout.ts:340-344` places a leaf's images inside the leaf's
    // allocated rect and crops an image line the box's clip cuts at its bottom
    // (`cropKittyImageLine` with `hiddenRows = 0`). C++ carries images as
    // sidecars instead of kitty image lines, so the visible rows become the
    // placement's destination region and the source rows it must not draw are
    // the `source_row_offset` (#1000; #1012 owns the protocol crop).
    cch::tui::VStack stack;
    REQUIRE(stack.add_child(std::make_unique<LabeledChild>(std::vector<std::string>{"h"})));
    REQUIRE(stack.add_child(std::make_unique<ImageChild>(std::vector<std::string>{"c", "", "", "c"}, reserved_image()),
            cch::tui::StackEntryOptions{.basis = 0, .grow = 1, .shrink = 1, .min_size = 1}));
    REQUIRE(stack.add_child(std::make_unique<LabeledChild>(std::vector<std::string>{"f"})));

    auto roomy = render_frame(stack, 8, 6);
    REQUIRE(roomy);
    CHECK(child_heights(roomy->root) == std::vector<std::size_t>{1, 4, 1});
    REQUIRE(roomy->images.size() == 1);
    CHECK(roomy->images.front().region.column == 0);
    CHECK(roomy->images.front().region.row == 2);
    CHECK(roomy->images.front().region.columns == 2);
    CHECK(roomy->images.front().region.rows == 2);
    CHECK(roomy->lines == std::vector<std::string>{"h", "c", "", "", "c", "f"});

    auto short_frame = render_frame(stack, 8, 4);
    REQUIRE(short_frame);
    CHECK(child_heights(short_frame->root) == std::vector<std::size_t>{1, 2, 1});
    REQUIRE(short_frame->images.size() == 1);
    CHECK(short_frame->images.front().region.row == 2);
    CHECK(short_frame->images.front().region.rows == 1);
    CHECK(short_frame->images.front().source_row_offset == 0);
    CHECK(short_frame->lines == std::vector<std::string>{"h", "c", "", "f"});

    // A placement wider than the viewport is still not part of the frame: the
    // horizontal rule is unchanged (#1012 owns horizontal cropping).
    auto narrow = render_frame(stack, 1, 6);
    REQUIRE(narrow);
    CHECK(narrow->images.empty());
    CHECK(narrow->lines == std::vector<std::string>{"h", "c", "", "", "c", "f"});
}

TEST_CASE("constrained VStack reflows after adding, changing, removing and clearing children",
        "[tui][layout][issue998]") {
    cch::tui::VStack stack;
    REQUIRE(stack.add_child(std::make_unique<LabeledChild>(std::vector<std::string>{"head"}),
            cch::tui::StackEntryOptions{.basis = 1, .shrink = 0}));
    auto body = std::make_unique<CursorChild>(std::vector<std::string>{"b1", "b2", "b3", "b4"}, 3, 2);
    auto* body_pointer = body.get();
    REQUIRE(stack.add_child(
            std::move(body), cch::tui::StackEntryOptions{.basis = 0, .grow = 1, .shrink = 1, .min_size = 1}));
    REQUIRE(stack.add_child(std::make_unique<LabeledChild>(std::vector<std::string>{"foot"}),
            cch::tui::StackEntryOptions{.basis = 1, .shrink = 0}));
    body_pointer->set_focused(true);

    auto base = render_frame(stack, 10, 6);
    REQUIRE(base);
    CHECK(child_heights(base->root) == std::vector<std::size_t>{1, 4, 1});
    CHECK(base->lines == std::vector<std::string>{"head", "b1", "b2", "b3", "b4", "foot"});
    REQUIRE(base->cursor);
    CHECK(base->cursor->row == 4);
    CHECK(base->cursor->column == 2);

    auto banner = std::make_unique<LabeledChild>(std::vector<std::string>{"bn1", "bn2"});
    auto* banner_pointer = banner.get();
    REQUIRE(stack.add_child(std::move(banner), cch::tui::StackEntryOptions{.shrink = 1}));
    auto grown = render_frame(stack, 10, 6);
    REQUIRE(grown);
    CHECK(child_heights(grown->root) == std::vector<std::size_t>{1, 2, 1, 2});
    CHECK(grown->lines == std::vector<std::string>{"head", "b3", "b4", "foot", "bn1", "bn2"});
    REQUIRE(grown->cursor);
    CHECK(grown->cursor->row == 2);

    banner_pointer->set_lines(std::vector<std::string>{"bn1", "bn2", "bn3", "bn4"});
    auto changed = render_frame(stack, 10, 6);
    REQUIRE(changed);
    CHECK(child_heights(changed->root) == std::vector<std::size_t>{1, 1, 1, 3});
    CHECK(changed->lines == std::vector<std::string>{"head", "b4", "foot", "bn1", "bn2", "bn3"});
    REQUIRE(changed->cursor);
    CHECK(changed->cursor->row == 1);
    CHECK(changed->cursor->column == 2);

    auto detached = stack.remove_child(banner_pointer);
    REQUIRE(detached);
    auto restored = render_frame(stack, 10, 6);
    REQUIRE(restored);
    CHECK(restored->lines == base->lines);
    CHECK(child_heights(restored->root) == std::vector<std::size_t>{1, 4, 1});

    stack.clear();
    auto cleared = render_frame(stack, 10, 6);
    REQUIRE(cleared);
    CHECK(cleared->root.children.empty());
    CHECK(cleared->lines == std::vector<std::string>(6, std::string{}));
    CHECK_FALSE(cleared->cursor);
}

TEST_CASE("constrained VStack reflows structural changes at narrow and short frames", "[tui][layout][issue998]") {
    // Narrow (1 column) frames: every cell is one column, so the assertions are
    // the allocated rows themselves. `allocateStackSizes` gives `grow` to the
    // flexible middle entry, and a short frame takes those cells back from it
    // and then keeps its cursor row visible (`layout.ts` leaf `lineOffset`), so
    // the fixed head and foot rows survive a frame the natural sizes overflow.
    cch::tui::VStack stack;
    REQUIRE(stack.add_child(std::make_unique<LabeledChild>(std::vector<std::string>{"h"}),
            cch::tui::StackEntryOptions{.basis = 1, .shrink = 0}));
    auto body = std::make_unique<CursorChild>(std::vector<std::string>{"1", "2"}, 1, 0);
    auto* body_pointer = body.get();
    REQUIRE(stack.add_child(
            std::move(body), cch::tui::StackEntryOptions{.basis = 0, .grow = 1, .shrink = 1, .min_size = 1}));
    REQUIRE(stack.add_child(std::make_unique<LabeledChild>(std::vector<std::string>{"f"}),
            cch::tui::StackEntryOptions{.basis = 1, .shrink = 0}));
    body_pointer->set_focused(true);

    auto narrow = render_frame(stack, 1, 5);
    REQUIRE(narrow);
    CHECK(child_heights(narrow->root) == std::vector<std::size_t>{1, 3, 1});
    CHECK(narrow->lines == std::vector<std::string>{"h", "1", "2", "", "f"});
    REQUIRE(narrow->cursor);
    CHECK(narrow->cursor->row == 2);
    CHECK(narrow->cursor->column == 0);

    auto short_frame = render_frame(stack, 1, 3);
    REQUIRE(short_frame);
    CHECK(child_heights(short_frame->root) == std::vector<std::size_t>{1, 1, 1});
    CHECK(short_frame->root.children[1].line_offset == 1);
    CHECK(short_frame->lines == std::vector<std::string>{"h", "2", "f"});
    REQUIRE(short_frame->cursor);
    CHECK(short_frame->cursor->row == 1);

    auto detached = stack.remove_child(body_pointer);
    REQUIRE(detached);
    auto removed = render_frame(stack, 1, 5);
    REQUIRE(removed);
    CHECK(removed->root.children.size() == 2);
    CHECK(removed->lines == std::vector<std::string>{"h", "f", "", "", ""});
    CHECK_FALSE(removed->cursor);

    // Re-attaching appends to the entry list (pi `Stack.addChild`), so the
    // flexible entry is laid out after the fixed footer.
    REQUIRE(stack.add_child(
            std::move(detached), cch::tui::StackEntryOptions{.basis = 0, .grow = 1, .shrink = 1, .min_size = 1}));
    body_pointer->set_lines(std::vector<std::string>{"1"}, 0);
    auto changed = render_frame(stack, 1, 5);
    REQUIRE(changed);
    CHECK(child_heights(changed->root) == std::vector<std::size_t>{1, 1, 3});
    CHECK(changed->lines == std::vector<std::string>{"h", "f", "1", "", ""});
    REQUIRE(changed->cursor);
    CHECK(changed->cursor->row == 2);

    stack.clear();
    auto cleared = render_frame(stack, 1, 5);
    REQUIRE(cleared);
    CHECK(cleared->root.children.empty());
    CHECK(cleared->lines == std::vector<std::string>(5, std::string{}));
    CHECK_FALSE(cleared->cursor);
}
