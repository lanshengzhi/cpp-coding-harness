// Named pi-v1.0.4 acceptance for the horizontal stack, its constrained layout
// node and nested stack geometry (#999). Every expectation is read from the
// verified named bundle through `scripts/tui/evidence.py`; nothing here
// regenerates expectations from Pike or falls back to the historical
// `fixtures/pi-tui/*.json` root snapshots.

#include <cch/tui/HStack.hpp>
#include <cch/tui/Stack.hpp>
#include <cch/tui/Utils.hpp>
#include <cch/tui/VStack.hpp>

#include "support/PiTuiEvidence.hpp"
#include "tui/Layout.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using cch::support::JsonValue;

/// Every width a probing leaf was rendered at, in call order — the capture's
/// `renderedAt` observation.
struct WidthLog {
    std::vector<std::size_t> widths;
};

/// A leaf that respects its render width and records every width it is
/// rendered at, the C++ shape of the capture's probing leaf: an entry that is
/// measured but never rendered is visible in the log it reports.
class ProbeChild final : public cch::tui::Component, public cch::tui::Focusable, public WidthLog {
public:
    explicit ProbeChild(std::vector<std::string> lines, std::optional<std::size_t> cursor_row = std::nullopt)
        : lines_(std::move(lines)), cursor_row_(cursor_row) {}

    void set_lines(std::vector<std::string> lines, std::optional<std::size_t> cursor_row = std::nullopt) {
        lines_ = std::move(lines);
        cursor_row_ = cursor_row;
    }

    [[nodiscard]] cch::support::Expected<cch::tui::RenderResult> render(std::size_t width) override {
        widths.push_back(width);
        rendered_.clear();
        rendered_.reserve(lines_.size());
        for (const auto& line : lines_)
            rendered_.push_back(line.size() > width ? line.substr(0, width) : line);
        return cch::tui::RenderResult{.lines = rendered_};
    }

    void invalidate() override {}

    void set_focused(bool focused) override { focused_ = focused; }
    [[nodiscard]] bool focused() const override { return focused_; }

    [[nodiscard]] std::optional<cch::tui::CursorPosition> cursor_location() const override {
        if (!focused_ || !cursor_row_ || *cursor_row_ >= rendered_.size()) return std::nullopt;
        return cch::tui::CursorPosition{.column = rendered_[*cursor_row_].size(), .row = *cursor_row_};
    }

private:
    std::vector<std::string> lines_;
    std::vector<std::string> rendered_;
    std::optional<std::size_t> cursor_row_;
    bool focused_{false};
};

/// A leaf whose intrinsic width is the width it is rendered at — the capture's
/// width-dependent entry that exposes the frame measurement pass.
class RepeatChild final : public cch::tui::Component, public WidthLog {
public:
    [[nodiscard]] cch::support::Expected<cch::tui::RenderResult> render(std::size_t width) override {
        widths.push_back(width);
        return cch::tui::RenderResult{.lines = {std::string(width, 'x')}};
    }

    void invalidate() override {}
};

/// The allocated box tree of a frame, compared recursively because a nested
/// stack allocates its own children inside the frame it was given.
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

[[nodiscard]] BoxTree read_tree(const JsonValue& box) {
    BoxTree node{.x = static_cast<std::ptrdiff_t>(box.at("x").get_number()),
            .y = static_cast<std::ptrdiff_t>(box.at("y").get_number()),
            .width = static_cast<std::size_t>(box.at("width").get_number()),
            .height = static_cast<std::size_t>(box.at("height").get_number()),
            .line_offset = static_cast<std::size_t>(box.at("lineOffset").get_number()),
            .children = {}};
    for (const auto& child : box.at("children").get_array())
        node.children.push_back(read_tree(child));
    return node;
}

[[nodiscard]] std::vector<std::string> read_lines(const JsonValue& expected) {
    std::vector<std::string> lines;
    for (const auto& line : expected.get_array())
        lines.push_back(line.get_string());
    return lines;
}

[[nodiscard]] std::vector<std::vector<std::size_t>> read_width_logs(const JsonValue& expected) {
    std::vector<std::vector<std::size_t>> logs;
    for (const auto& log : expected.get_array()) {
        std::vector<std::size_t> widths;
        for (const auto& width : log.get_array())
            widths.push_back(static_cast<std::size_t>(width.get_number()));
        logs.push_back(std::move(widths));
    }
    return logs;
}

/// The painted rows as visible text, pi's own `visibleLines` convention
/// (`test/layout.test.ts:11-13`): the shared splice leaves the same segment
/// resets pi's `compositeTuiLine` does, a composed row is padded to the frame
/// width, and neither is part of what the terminal shows.
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

[[nodiscard]] std::vector<std::vector<std::size_t>> logs_of(const std::vector<const WidthLog*>& logs) {
    std::vector<std::vector<std::size_t>> result;
    result.reserve(logs.size());
    for (const auto* log : logs)
        result.push_back(log->widths);
    return result;
}

[[nodiscard]] cch::tui::StackAlign align_of(std::string_view name) {
    if (name == "stretch") return cch::tui::StackAlign::Stretch;
    if (name == "center") return cch::tui::StackAlign::Center;
    if (name == "end") return cch::tui::StackAlign::End;
    return cch::tui::StackAlign::Start;
}

[[nodiscard]] const JsonValue* find_scenario(const JsonValue& bundle, std::string_view name) {
    for (const auto& scenario : bundle.at("scenarios").get_array()) {
        if (scenario.at("name").get_string() == name) return &scenario;
    }
    return nullptr;
}

[[nodiscard]] cch::support::Expected<JsonValue> named_bundle() {
    return cch::tests::read_pi_tui_evidence("layout.json");
}

[[nodiscard]] const JsonValue& expected_of(const JsonValue& bundle, std::string_view scenario) {
    const auto* found = find_scenario(bundle, scenario);
    REQUIRE(found != nullptr);
    return found->at("expected");
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

/// Compare the C++ engine's frame against one frozen pi frame: every allocated
/// box, the painted rows and the cursor the terminal would show
/// (`TuiBase.extractCursorPosition`).
void check_frame(const JsonValue& expected, cch::tui::Component& root, std::size_t columns, std::size_t rows) {
    const auto frame = cch::tui::detail::LayoutEngine::render(root, columns, rows);
    REQUIRE(frame);
    CHECK(box_tree(frame->root) == read_tree(expected.at("tree")));
    CHECK(visible_lines(frame->lines) == read_lines(expected.at("lines")));
    const auto& cursor = expected.at("cursor");
    if (cursor.holds<JsonValue::null_t>()) {
        CHECK_FALSE(frame->cursor);
    } else {
        REQUIRE(frame->cursor);
        CHECK(frame->cursor->row == static_cast<std::size_t>(cursor.at("row").get_number()));
        CHECK(frame->cursor->column == static_cast<std::size_t>(cursor.at("column").get_number()));
    }
}

} // namespace

TEST_CASE("a standalone HStack allocates and aligns exactly as the frozen pi stack does",
        "[tui][hstack][issue999][compat-pi]") {
    const auto bundle = named_bundle();
    REQUIRE(bundle);

    const auto& unbounded = expected_of(*bundle, "hstack-unbounded");
    for (const auto name : {"stretch", "start", "center", "end"}) {
        const auto& frozen = unbounded.at("aligns").at(name);
        auto left = std::make_unique<ProbeChild>(std::vector<std::string>{"left"});
        auto tall = std::make_unique<ProbeChild>(std::vector<std::string>{"x", "y", "z"});
        auto hidden = std::make_unique<ProbeChild>(std::vector<std::string>{"hidden"});
        auto right = std::make_unique<ProbeChild>(std::vector<std::string>{"rt"});
        const std::vector<const WidthLog*> logs{left.get(), tall.get(), hidden.get(), right.get()};
        cch::tui::HStack stack({.gap = 1, .align = align_of(name)});
        REQUIRE(stack.add_child(std::move(left), cch::tui::StackEntryOptions{.basis = 4, .shrink = 0}));
        REQUIRE(stack.add_child(std::move(tall), cch::tui::StackEntryOptions{.basis = 2, .grow = 1, .shrink = 0}));
        REQUIRE(stack.add_child(std::move(hidden), cch::tui::StackEntryOptions{.basis = 0, .shrink = 0}));
        REQUIRE(stack.add_child(std::move(right), cch::tui::StackEntryOptions{.basis = 5, .shrink = 1, .min_size = 1}));

        const auto rendered = stack.render(12);
        REQUIRE(rendered);
        CHECK(visible_lines(rendered->lines) == read_lines(frozen.at("lines")));
        CHECK(logs_of(logs) == read_width_logs(frozen.at("renderedAt")));
    }

    {
        const auto& frozen = unbounded.at("grow");
        auto flexible = std::make_unique<ProbeChild>(std::vector<std::string>{"a"});
        auto fixed = std::make_unique<ProbeChild>(std::vector<std::string>{"bb"});
        auto empty = std::make_unique<ProbeChild>(std::vector<std::string>{""});
        const std::vector<const WidthLog*> logs{flexible.get(), fixed.get(), empty.get()};
        cch::tui::HStack stack({.gap = 1});
        REQUIRE(stack.add_child(std::move(flexible), cch::tui::StackEntryOptions{.basis = 0, .grow = 1, .shrink = 0}));
        REQUIRE(stack.add_child(std::move(fixed), cch::tui::StackEntryOptions{.basis = 2, .shrink = 0}));
        REQUIRE(stack.add_child(std::move(empty), cch::tui::StackEntryOptions{.basis = 0, .shrink = 0}));

        const auto rendered = stack.render(10);
        REQUIRE(rendered);
        CHECK(visible_lines(rendered->lines) == read_lines(frozen.at("lines")));
        CHECK(logs_of(logs) == read_width_logs(frozen.at("renderedAt")));
    }
    {
        const auto& frozen = unbounded.at("clamps");
        auto below_minimum = std::make_unique<ProbeChild>(std::vector<std::string>{"a"});
        auto above_maximum = std::make_unique<ProbeChild>(std::vector<std::string>{"bb"});
        const std::vector<const WidthLog*> logs{below_minimum.get(), above_maximum.get()};
        cch::tui::HStack stack;
        REQUIRE(stack.add_child(
                std::move(below_minimum), cch::tui::StackEntryOptions{.basis = 0, .grow = 1, .min_size = 3}));
        REQUIRE(stack.add_child(std::move(above_maximum), cch::tui::StackEntryOptions{.shrink = 0, .max_size = 2}));

        const auto rendered = stack.render(8);
        REQUIRE(rendered);
        CHECK(visible_lines(rendered->lines) == read_lines(frozen.at("lines")));
        CHECK(logs_of(logs) == read_width_logs(frozen.at("renderedAt")));
    }
    {
        const auto& frozen = unbounded.at("zeroOnly");
        auto zero = std::make_unique<ProbeChild>(std::vector<std::string>{"z"});
        const std::vector<const WidthLog*> logs{zero.get()};
        cch::tui::HStack stack;
        REQUIRE(stack.add_child(std::move(zero), cch::tui::StackEntryOptions{.basis = 0, .shrink = 0}));

        const auto rendered = stack.render(5);
        REQUIRE(rendered);
        CHECK(visible_lines(rendered->lines) == read_lines(frozen.at("lines")));
        CHECK(logs_of(logs) == read_width_logs(frozen.at("renderedAt")));
    }
}

TEST_CASE("a constrained HStack frame allocates unequal columns as the frozen frames do",
        "[tui][hstack][issue999][compat-pi]") {
    const auto bundle = named_bundle();
    REQUIRE(bundle);

    const auto& uneven = expected_of(*bundle, "hstack-frame-unequal");
    const auto check_width = [&uneven](const JsonValue& frozen, std::size_t width) {
        auto left = std::make_unique<ProbeChild>(std::vector<std::string>{"lft"});
        auto right = std::make_unique<ProbeChild>(std::vector<std::string>{"rgt"});
        const std::vector<const WidthLog*> logs{left.get(), right.get()};
        cch::tui::HStack root({.gap = 1});
        REQUIRE(root.add_child(std::move(left), cch::tui::StackEntryOptions{.basis = 6, .shrink = 1, .min_size = 1}));
        REQUIRE(root.add_child(std::move(right), cch::tui::StackEntryOptions{.basis = 6, .shrink = 1, .min_size = 1}));

        check_frame(frozen, root, width, 1);
        CHECK(logs_of(logs) == read_width_logs(frozen.at("renderedAt")));
    };
    check_width(uneven.at("wide"), 12);
    check_width(uneven.at("narrow"), 8);
}

TEST_CASE("a constrained HStack frame aligns children inside their allocation as pi does",
        "[tui][hstack][issue999][compat-pi]") {
    const auto bundle = named_bundle();
    REQUIRE(bundle);

    const auto& aligns = expected_of(*bundle, "hstack-frame-align");
    for (const auto name : {"stretch", "start", "center", "end"}) {
        cch::tui::HStack root({.align = align_of(name)});
        REQUIRE(root.add_child(std::make_unique<ProbeChild>(std::vector<std::string>{"a1"}),
                cch::tui::StackEntryOptions{.basis = 4, .shrink = 0}));
        REQUIRE(root.add_child(std::make_unique<ProbeChild>(std::vector<std::string>{"b1", "b2"}),
                cch::tui::StackEntryOptions{.basis = 4, .shrink = 0}));

        check_frame(aligns.at(name), root, 8, 4);
    }
}

TEST_CASE("a constrained HStack frame measures an auto basis at the frame width as pi does",
        "[tui][hstack][issue999][compat-pi]") {
    const auto bundle = named_bundle();
    REQUIRE(bundle);

    const auto& frozen = expected_of(*bundle, "hstack-frame-auto-basis");
    auto wide = std::make_unique<RepeatChild>();
    auto narrow = std::make_unique<ProbeChild>(std::vector<std::string>{"ab"});
    const std::vector<const WidthLog*> logs{wide.get(), narrow.get()};
    cch::tui::HStack root({.gap = 1});
    REQUIRE(root.add_child(std::move(wide), cch::tui::StackEntryOptions{.basis = std::nullopt}));
    REQUIRE(root.add_child(std::move(narrow), cch::tui::StackEntryOptions{.basis = std::nullopt, .shrink = 1}));

    check_frame(frozen, root, 6, 1);
    CHECK(logs_of(logs) == read_width_logs(frozen.at("renderedAt")));
}

TEST_CASE("a nested HStack frame replays the frozen pi geometry through mutations",
        "[tui][hstack][issue999][compat-pi]") {
    const auto bundle = named_bundle();
    REQUIRE(bundle);

    const auto& expected = expected_of(*bundle, "hstack-nested");
    auto inner_leaf = std::make_unique<ProbeChild>(std::vector<std::string>{"m1", "m2"}, 1);
    auto* inner_leaf_ptr = inner_leaf.get();
    inner_leaf_ptr->set_focused(true);
    auto inner = std::make_unique<cch::tui::VStack>();
    REQUIRE(inner->add_child(std::move(inner_leaf), cch::tui::StackEntryOptions{.min_size = 2}));
    auto side = std::make_unique<ProbeChild>(std::vector<std::string>{"s1"});
    auto* side_ptr = side.get();

    cch::tui::HStack root({.gap = 1});
    REQUIRE(root.add_child(std::move(inner), cch::tui::StackEntryOptions{.basis = 5, .shrink = 0}));
    REQUIRE(root.add_child(std::move(side),
            cch::tui::StackEntryOptions{
                    .basis = 3, .shrink = 1, .min_size = 3, .visible = [](const cch::tui::LayoutViewport& viewport) {
                        return viewport.width >= 10;
                    }}));

    check_frame(expected.at("wide"), root, 10, 3);
    check_frame(expected.at("narrow"), root, 8, 3);

    inner_leaf_ptr->set_lines(std::vector<std::string>{"m1", "m2", "m3", "m4"}, 3);
    check_frame(expected.at("grown"), root, 10, 3);

    auto removed = root.remove_child(side_ptr);
    REQUIRE(removed);
    check_frame(expected.at("removed"), root, 10, 3);

    REQUIRE(root.add_child(std::make_unique<ProbeChild>(std::vector<std::string>{"s9"}),
            cch::tui::StackEntryOptions{.basis = std::nullopt}));
    check_frame(expected.at("readded"), root, 10, 3);
}

TEST_CASE("an HStack inside a constrained VStack allocates the width it was given",
        "[tui][hstack][issue999][compat-pi]") {
    const auto bundle = named_bundle();
    REQUIRE(bundle);

    const auto& frozen = expected_of(*bundle, "hstack-in-vstack");
    const auto check_width = [](const JsonValue& expected, std::size_t width) {
        auto row = std::make_unique<cch::tui::HStack>(cch::tui::StackOptions{.gap = 1});
        REQUIRE(row->add_child(std::make_unique<ProbeChild>(std::vector<std::string>{"left"}),
                cch::tui::StackEntryOptions{.basis = 6, .shrink = 1, .min_size = 1}));
        REQUIRE(row->add_child(std::make_unique<ProbeChild>(std::vector<std::string>{"right"}),
                cch::tui::StackEntryOptions{.basis = 6, .shrink = 1, .min_size = 1}));
        cch::tui::VStack root;
        REQUIRE(root.add_child(
                std::move(row), cch::tui::StackEntryOptions{.basis = 0, .grow = 1, .shrink = 1, .min_size = 1}));
        REQUIRE(root.add_child(std::make_unique<ProbeChild>(std::vector<std::string>{"f1"}),
                cch::tui::StackEntryOptions{.basis = 2, .shrink = 0}));

        check_frame(expected, root, width, 4);
    };
    check_width(frozen.at("wide"), 14);
    check_width(frozen.at("narrow"), 10);
}
