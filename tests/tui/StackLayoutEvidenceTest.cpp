// Named pi-v1.0.4 acceptance for the constrained VStack layout and the
// fullscreen dock composition (#998). Every expectation is read from the
// verified named bundle through `scripts/tui/evidence.py`; nothing here
// regenerates expectations from Pike or falls back to the historical
// `fixtures/pi-tui/*.json` root snapshots.

#include <cch/tui/Stack.hpp>
#include <cch/tui/VStack.hpp>

#include "support/PiTuiEvidence.hpp"
#include "tui/Layout.hpp"

#include <cch/support/Error.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using cch::support::JsonValue;

/// A cursor-free leaf, the C++ shape of pi's `Text(text, 0, 0)`: it renders
/// exactly the frozen lines at any width.
class TextChild final : public cch::tui::Component {
public:
    explicit TextChild(std::vector<std::string> lines) : lines_(std::move(lines)) {}

    [[nodiscard]] cch::support::Expected<cch::tui::RenderResult> render(std::size_t) override {
        return cch::tui::RenderResult{.lines = lines_};
    }

    void invalidate() override {}

private:
    std::vector<std::string> lines_;
};

/// A leaf that reports a cursor, the C++ shape of pi's `CURSOR_MARKER` line.
class CursorChild final : public cch::tui::Component, public cch::tui::Focusable {
public:
    CursorChild(std::vector<std::string> lines, std::size_t cursor_row, std::size_t cursor_column)
        : lines_(std::move(lines)), cursor_row_(cursor_row), cursor_column_(cursor_column) {}

    [[nodiscard]] cch::support::Expected<cch::tui::RenderResult> render(std::size_t) override {
        return cch::tui::RenderResult{.lines = lines_};
    }

    void invalidate() override {}

    void set_focused(bool focused) override { focused_ = focused; }
    [[nodiscard]] bool focused() const override { return focused_; }

    [[nodiscard]] std::optional<cch::tui::CursorPosition> cursor_location() const override {
        if (!focused_) return std::nullopt;
        return cch::tui::CursorPosition{.column = cursor_column_, .row = cursor_row_};
    }

private:
    std::vector<std::string> lines_;
    std::size_t cursor_row_{0};
    std::size_t cursor_column_{0};
    bool focused_{false};
};

/// One frozen pi frame: the root's child geometry, the painted lines and the
/// cursor the terminal would show (`TuiBase.extractCursorPosition`).
struct FrozenFrame {
    std::vector<std::size_t> heights;
    std::vector<std::size_t> rows;
    std::vector<std::size_t> line_offsets;
    std::vector<std::string> lines;
    bool has_cursor{false};
    std::size_t cursor_row{0};
    std::size_t cursor_column{0};
};

[[nodiscard]] FrozenFrame read_frame(const JsonValue& expected) {
    FrozenFrame frame;
    for (const auto& child : expected.at("children").get_array()) {
        frame.heights.push_back(static_cast<std::size_t>(child.at("height").get_number()));
        frame.rows.push_back(static_cast<std::size_t>(child.at("y").get_number()));
        frame.line_offsets.push_back(static_cast<std::size_t>(child.at("lineOffset").get_number()));
    }
    for (const auto& line : expected.at("lines").get_array())
        frame.lines.push_back(line.get_string());
    const auto& cursor = expected.at("cursor");
    if (!cursor.holds<JsonValue::null_t>()) {
        frame.has_cursor = true;
        frame.cursor_row = static_cast<std::size_t>(cursor.at("row").get_number());
        frame.cursor_column = static_cast<std::size_t>(cursor.at("column").get_number());
    }
    return frame;
}

[[nodiscard]] std::vector<std::string> read_lines(const JsonValue& expected) {
    std::vector<std::string> lines;
    for (const auto& line : expected.get_array())
        lines.push_back(line.get_string());
    return lines;
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

[[nodiscard]] const JsonValue& expected_of(
        const JsonValue& bundle, std::string_view scenario, const std::string& group) {
    return expected_of(bundle, scenario).at(group);
}

/// Compare the C++ engine's frame against one frozen pi frame: child
/// allocation, painted lines and the cursor the terminal would place.
void check_frame(const FrozenFrame& frozen, cch::tui::Component& root, std::size_t columns, std::size_t rows) {
    const auto frame = cch::tui::detail::LayoutEngine::render(root, columns, rows);
    REQUIRE(frame);
    std::vector<std::size_t> heights;
    std::vector<std::size_t> positions;
    std::vector<std::size_t> offsets;
    for (const auto& child : frame->root.children) {
        heights.push_back(child.rect.height);
        positions.push_back(static_cast<std::size_t>(child.rect.y));
        offsets.push_back(child.line_offset);
    }
    CHECK(heights == frozen.heights);
    CHECK(positions == frozen.rows);
    CHECK(offsets == frozen.line_offsets);
    CHECK(frame->lines == frozen.lines);
    if (frozen.has_cursor) {
        REQUIRE(frame->cursor);
        CHECK(frame->cursor->row == frozen.cursor_row);
        CHECK(frame->cursor->column == frozen.cursor_column);
    } else {
        CHECK_FALSE(frame->cursor);
    }
}

} // namespace

TEST_CASE("constrained VStack grows and shrinks exactly as the frozen pi frames do",
        "[tui][layout][issue998][compat-pi]") {
    const auto bundle = named_bundle();
    REQUIRE(bundle);

    {
        cch::tui::VStack root;
        REQUIRE(root.add_child(std::make_unique<TextChild>(std::vector<std::string>{"top"}),
                cch::tui::StackEntryOptions{.basis = 1, .shrink = 0}));
        REQUIRE(root.add_child(std::make_unique<TextChild>(std::vector<std::string>{"body"}),
                cch::tui::StackEntryOptions{.basis = 0, .grow = 1}));
        check_frame(read_frame(expected_of(*bundle, "frame-vertical-grow")), root, 10, 4);
    }
    {
        cch::tui::VStack root;
        REQUIRE(root.add_child(std::make_unique<TextChild>(std::vector<std::string>{"a1", "a2", "a3"}),
                cch::tui::StackEntryOptions{.shrink = 1, .min_size = 1}));
        REQUIRE(root.add_child(std::make_unique<TextChild>(std::vector<std::string>{"b1", "b2", "b3"}),
                cch::tui::StackEntryOptions{.shrink = 0}));
        check_frame(read_frame(expected_of(*bundle, "frame-vertical-shrink")), root, 10, 4);
    }
}

TEST_CASE("a nested auto-basis dock is measured against minimum sizes as the frozen pi frame does",
        "[tui][layout][issue998][compat-pi]") {
    const auto bundle = named_bundle();
    REQUIRE(bundle);

    auto dock = std::make_unique<cch::tui::VStack>();
    REQUIRE(dock->add_child(std::make_unique<TextChild>(std::vector<std::string>{"top1", "top2", "top3"})));
    REQUIRE(dock->add_child(std::make_unique<TextChild>(std::vector<std::string>{"selector"}),
            cch::tui::StackEntryOptions{.min_size = 3}));
    REQUIRE(dock->add_child(std::make_unique<TextChild>(std::vector<std::string>{"below"})));
    REQUIRE(dock->add_child(std::make_unique<TextChild>(std::vector<std::string>{"footer"}),
            cch::tui::StackEntryOptions{.min_size = 1}));

    cch::tui::VStack root;
    REQUIRE(root.add_child(std::make_unique<TextChild>(std::vector<std::string>{"body"}),
            cch::tui::StackEntryOptions{.basis = 0, .grow = 1, .min_size = 1}));
    REQUIRE(root.add_child(std::move(dock), cch::tui::StackEntryOptions{.basis = std::nullopt, .min_size = 1}));
    check_frame(read_frame(expected_of(*bundle, "frame-nested-minimums")), root, 10, 9);
}

TEST_CASE("unbounded VStack composition clamps and gaps exactly as the frozen pi stack does",
        "[tui][layout][issue998][compat-pi]") {
    const auto bundle = named_bundle();
    REQUIRE(bundle);
    const JsonValue* expected = &expected_of(*bundle, "standalone-unbounded", "clampStack");

    cch::tui::VStack gaps({.gap = 1});
    REQUIRE(gaps.add_child(std::make_unique<TextChild>(std::vector<std::string>{"one"})));
    REQUIRE(gaps.add_child(std::make_unique<TextChild>(std::vector<std::string>{"hidden"}),
            cch::tui::StackEntryOptions{.visible = [](const cch::tui::LayoutViewport&) { return false; }}));
    REQUIRE(gaps.add_child(std::make_unique<TextChild>(std::vector<std::string>{"two"})));

    cch::tui::VStack clamps;
    REQUIRE(clamps.add_child(std::make_unique<TextChild>(std::vector<std::string>{"a"}),
            cch::tui::StackEntryOptions{.basis = 1, .min_size = 2}));
    REQUIRE(clamps.add_child(std::make_unique<TextChild>(std::vector<std::string>{"b1", "b2", "b3"}),
            cch::tui::StackEntryOptions{.max_size = 2}));
    REQUIRE(clamps.add_child(
            std::make_unique<TextChild>(std::vector<std::string>{"c"}), cch::tui::StackEntryOptions{.basis = 3}));

    const auto rendered_gaps = gaps.render(10);
    const auto rendered_clamps = clamps.render(10);
    REQUIRE(rendered_gaps);
    REQUIRE(rendered_clamps);
    CHECK(rendered_gaps->lines == read_lines(expected_of(*bundle, "standalone-unbounded", "gapStack")));
    CHECK(rendered_clamps->lines == read_lines(*expected));
}

TEST_CASE("a constrained VStack crops a leaf and translates its cursor as the frozen pi frame does",
        "[tui][layout][issue998][compat-pi]") {
    const auto bundle = named_bundle();
    REQUIRE(bundle);

    auto editor = std::make_unique<CursorChild>(std::vector<std::string>{"a1", "a2", "a3", "a4"}, 3, 2);
    editor->set_focused(true);
    cch::tui::VStack root;
    REQUIRE(root.add_child(std::move(editor), cch::tui::StackEntryOptions{.shrink = 1, .min_size = 1}));
    REQUIRE(root.add_child(std::make_unique<TextChild>(std::vector<std::string>{"b1", "b2"}),
            cch::tui::StackEntryOptions{.shrink = 0}));
    check_frame(read_frame(expected_of(*bundle, "frame-cursor-crop")), root, 10, 3);
}

TEST_CASE("a constrained VStack evaluates visible entries against the frozen pi viewport",
        "[tui][layout][issue998][compat-pi]") {
    const auto bundle = named_bundle();
    REQUIRE(bundle);

    const auto root_of = [](cch::tui::StackAlign align) {
        auto root = std::make_unique<cch::tui::VStack>(cch::tui::StackOptions{.align = align});
        REQUIRE(root->add_child(std::make_unique<TextChild>(std::vector<std::string>{"head"}),
                cch::tui::StackEntryOptions{.basis = 1, .shrink = 0}));
        REQUIRE(root->add_child(std::make_unique<TextChild>(std::vector<std::string>{"detail"}),
                cch::tui::StackEntryOptions{
                        .basis = 0, .grow = 1, .min_size = 1, .visible = [](const cch::tui::LayoutViewport& viewport) {
                            return viewport.height >= 5;
                        }}));
        REQUIRE(root->add_child(std::make_unique<TextChild>(std::vector<std::string>{"foot"}),
                cch::tui::StackEntryOptions{.basis = 1, .shrink = 0}));
        return root;
    };

    auto short_stretch = root_of(cch::tui::StackAlign::Stretch);
    check_frame(read_frame(expected_of(*bundle, "frame-responsive-visibility", "shortStretch")), *short_stretch, 10, 2);
    auto tall_stretch = root_of(cch::tui::StackAlign::Stretch);
    check_frame(read_frame(expected_of(*bundle, "frame-responsive-visibility", "tallStretch")), *tall_stretch, 10, 5);
    auto tall_center = root_of(cch::tui::StackAlign::Center);
    check_frame(read_frame(expected_of(*bundle, "frame-responsive-visibility", "tallCenter")), *tall_center, 10, 5);
}

TEST_CASE("the composed dock keeps the editor and footer rows exactly as the frozen pi frames do",
        "[tui][layout][issue998][compat-pi]") {
    const auto bundle = named_bundle();
    REQUIRE(bundle);

    const auto root_of = [](std::vector<std::string> editor_lines, bool menu_visible) {
        auto dock = std::make_unique<cch::tui::VStack>();
        REQUIRE(dock->add_child(std::make_unique<TextChild>(std::move(editor_lines)),
                cch::tui::StackEntryOptions{.shrink = 1, .min_size = 3}));
        REQUIRE(dock->add_child(std::make_unique<TextChild>(std::vector<std::string>{"menu1", "menu2", "menu3"}),
                cch::tui::StackEntryOptions{.shrink = 1,
                        .min_size = 0,
                        .visible = [menu_visible](const cch::tui::LayoutViewport&) { return menu_visible; }}));
        REQUIRE(dock->add_child(std::make_unique<TextChild>(std::vector<std::string>{"footer-1", "footer-2"}),
                cch::tui::StackEntryOptions{.basis = 2, .grow = 0, .shrink = 0}));

        auto root = std::make_unique<cch::tui::VStack>();
        REQUIRE(root->add_child(
                std::make_unique<TextChild>(std::vector<std::string>{"t1", "t2", "t3", "t4", "t5", "t6", "t7", "t8"}),
                cch::tui::StackEntryOptions{.basis = 0, .grow = 1, .shrink = 1, .min_size = 1}));
        REQUIRE(root->add_child(std::move(dock),
                cch::tui::StackEntryOptions{.basis = std::nullopt, .grow = 0, .shrink = 1, .min_size = 1}));
        return root;
    };

    const auto editor = [](std::size_t rows) {
        std::vector<std::string> lines;
        for (std::size_t index = 0; index < rows; ++index)
            lines.push_back("e" + std::to_string(index + 1));
        return lines;
    };

    const std::string_view groups[] = {"editorAtMinimum", "editorGrown", "menuOpen", "overConstrained", "shrunkBack"};
    const std::vector<std::size_t> editor_rows = {3, 6, 6, 9, 3};
    const std::vector<bool> menu_visible = {false, false, true, true, false};
    for (std::size_t index = 0; index < std::size(groups); ++index) {
        CAPTURE(groups[index]);
        auto root = root_of(editor(editor_rows[index]), menu_visible[index]);
        check_frame(read_frame(expected_of(*bundle, "frame-dock-growth", std::string(groups[index]))), *root, 8, 12);
    }
}
