#include <cch/tui/Container.hpp>
#include <cch/tui/Editor.hpp>
#include <cch/tui/Image.hpp>
#include <cch/tui/Input.hpp>
#include <cch/tui/Text.hpp>
#include <cch/tui/TruncatedText.hpp>
#include <cch/tui/Tui.hpp>
#include <cch/tui/Utils.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include <cch/support/Error.hpp>
#include <catch2/catch_test_macros.hpp>

#include "support/ImageCapabilitiesGuard.hpp"
#include "support/PiTuiEvidence.hpp"
#include "support/RenderedScreen.hpp"
#include "tui/ContainerTestHooks.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

// Independently observed with frozen pi-v1.0.4 at
// 7c10bd4337495ee613f2224843ecdf349b80d1df (Node v26.11.1): Box(2, 1)
// around `hi` at width 1 renders ` `, `  h`, `  i`, ` `; the full left
// padding survives while child content width clamps to one. Whitespace-only
// Text renders no rows, and Box therefore emits no padding rows for that child.
class RawLineComponent final : public cch::tui::Component {
public:
    explicit RawLineComponent(std::string line) : line_(std::move(line)) {}

    [[nodiscard]] cch::support::Expected<cch::tui::RenderResult> render(std::size_t) override {
        return cch::tui::RenderResult{.lines = {line_}};
    }

    void invalidate() override {}

private:
    std::string line_;
};

} // namespace

TEST_CASE("Container renders children stacked vertically", "[tui][issue46][container][spec]") {
    cch::tui::Container container;
    auto first_child = std::make_unique<cch::tui::Text>("hello", 0, 0);
    auto second_child = std::make_unique<cch::tui::Text>("world", 0, 0);
    REQUIRE(container.add_child(std::move(first_child)));
    REQUIRE(container.add_child(std::move(second_child)));

    auto result = container.render(10);
    REQUIRE(result);
    REQUIRE(result->lines.size() >= 2);
    CHECK(result->lines[0].find("hello") != std::string::npos);
    CHECK(result->lines[1].find("world") != std::string::npos);
}

TEST_CASE("Container leaves child styling for the composed-row reset", "[tui][issue46][container][spec]") {
    cch::tui::Container container;
    REQUIRE(container.add_child(std::make_unique<RawLineComponent>("\x1b[31mred")));
    REQUIRE(container.add_child(std::make_unique<RawLineComponent>("plain")));

    const auto result = container.render(8);
    REQUIRE(result);
    REQUIRE(result->lines.size() == 2);
    // The component boundary carries no reset: the one full reset per row is
    // appended at the composed-line boundary.
    CHECK(result->lines[0] == "\x1b[31mred");
    CHECK(result->lines[1] == "plain");
}

TEST_CASE("Container rejects unsafe or overwide child lines", "[tui][issue46][container][spec]") {
    cch::tui::Container unsafe;
    REQUIRE(unsafe.add_child(std::make_unique<RawLineComponent>("\x1b[10Gx")));
    CHECK_FALSE(unsafe.render(8));

    cch::tui::Container overwide;
    REQUIRE(overwide.add_child(std::make_unique<RawLineComponent>("too wide")));
    CHECK_FALSE(overwide.render(3));
}

TEST_CASE("Container rejects null child", "[tui][issue46][container][spec]") {
    cch::tui::Container container;
    auto result = container.add_child(nullptr);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == cch::support::ErrorCode::Validation);
}

TEST_CASE("Container requires positive width", "[tui][container][issue966][spec]") {
    cch::tui::Container container;
    auto result = container.render(0);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == cch::support::ErrorCode::Validation);
    CHECK(result.error().message == "TUI Container requires a positive visible width");
}

TEST_CASE("Box renders with padding and background", "[tui][issue46][container][spec]") {
    cch::tui::Box box(1, 1);
    auto text = std::make_unique<cch::tui::Text>("hi", 0, 0);
    REQUIRE(box.add_child(std::move(text)));

    auto result = box.render(10);
    REQUIRE(result);
    // 1 top padding + 1 content + 1 bottom padding = 3 lines
    CHECK(result->lines.size() == 3);
    // Each line should be width 10
    for (const auto& line : result->lines) {
        CHECK(line.size() == 10);
    }
}

TEST_CASE("Box preserves background hook output beyond the visible width", "[tui][issue966][container][spec]") {
    cch::tui::Box box(0, 0, [](std::string line) { return line + "x"; });
    REQUIRE(box.add_child(std::make_unique<cch::tui::Text>("x", 0, 0)));

    const auto result = box.render(4);
    REQUIRE(result);
    REQUIRE(result->lines.size() == 1);
    CHECK(result->lines[0] == "x   x");
}

// Frozen pi Box samples bgFn("test") and compares actual child line strings
// on each render. Mutating Text and the captured background state at one width
// distinguishes that cache contract from revision-only cache authority.
TEST_CASE(
        "Box observes child and background closure changes without invalidation", "[tui][box][cache][issue966][spec]") {
    std::string background = "\x1b[44m";
    cch::tui::Box box(1, 0, [&background](std::string line) { return background + line; });
    auto child = std::make_unique<cch::tui::Text>("old", 0, 0);
    auto* child_ptr = child.get();
    REQUIRE(box.add_child(std::move(child)));

    const auto first = box.render(10);
    REQUIRE(first);
    REQUIRE(first->lines.size() == 1);
    CHECK(first->lines[0].starts_with("\x1b[44m"));
    CHECK(first->lines[0].find("old") != std::string::npos);

    child_ptr->set_text("new");
    const auto changed_child = box.render(10);
    REQUIRE(changed_child);
    REQUIRE(changed_child->lines.size() == 1);
    CHECK(changed_child->lines[0].starts_with("\x1b[44m"));
    CHECK(changed_child->lines[0].find("new") != std::string::npos);
    CHECK(changed_child->lines[0].find("old") == std::string::npos);

    background = "\x1b[41m";
    const auto changed_background = box.render(10);
    REQUIRE(changed_background);
    CHECK(changed_background->lines[0].starts_with("\x1b[41m"));
    CHECK(changed_background->lines[0].find("new") != std::string::npos);
    CHECK(cch::tui::detail::testing::box_tokenize_terminal_output_call_count(box) > 0);

    REQUIRE(box.render(10));
    CHECK(cch::tui::detail::testing::box_tokenize_terminal_output_call_count(box) == 0);
}

TEST_CASE("Box cache keeps public Image sidecars current when lines are unchanged",
        "[tui][box][image][issue966][spec][issue994]") {
    cch::tests::ImageCapabilitiesGuard image_capabilities({.images = cch::tui::InlineImageProtocol::Kitty});
    cch::tui::Box box(1, 0);
    auto image = std::make_unique<cch::tui::Image>(
            cch::tui::ImageContent{.encoded_data = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAAAElFTkSuQmCC",
                    .mime_type = "image/png",
                    .filename = "one.png"});
    auto* image_ptr = image.get();
    REQUIRE(box.add_child(std::move(image)));

    const auto first = box.render(10);
    REQUIRE(first);
    REQUIRE(first->images.size() == 1);
    CHECK(first->images[0].filename == std::optional<std::string>{"one.png"});
    const auto first_lines = first->lines;
    const auto first_revision = first->images[0].revision;

    image_ptr->set_content({
            .encoded_data = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAAAElFTkSuQmCC",
            .mime_type = "image/png",
            .filename = "two.png",
    });
    const auto updated = box.render(10);
    REQUIRE(updated);
    CHECK(updated->lines == first_lines);
    REQUIRE(updated->images.size() == 1);
    CHECK(updated->images[0].filename == std::optional<std::string>{"two.png"});
    CHECK(updated->images[0].revision != first_revision);
    CHECK(cch::tui::detail::testing::box_tokenize_terminal_output_call_count(box) == 0);
}

TEST_CASE("Box owns a move-only background hook", "[tui][issue46][container][spec]") {
    auto prefix = std::make_unique<std::string>("\x1b[44m");
    cch::tui::Box box(0, 0, [owned = std::move(prefix)](std::string line) { return *owned + line; });
    REQUIRE(box.add_child(std::make_unique<cch::tui::Text>("x", 0, 0)));

    const auto result = box.render(2);
    REQUIRE(result);
    REQUIRE(result->lines.size() == 1);
    // The hook's prefix is preserved and the component boundary adds no reset;
    // padding still happens before the hook.
    CHECK(result->lines[0] == "\x1b[44mx ");
}

TEST_CASE("Box rejects null child", "[tui][issue46][container][spec]") {
    cch::tui::Box box;
    auto result = box.add_child(nullptr);
    REQUIRE_FALSE(result);
}

// Dynamic horizontal padding on `Box` and `Text` (B63 / TPadding, #1121).
// The frozen pi-v1.3.0 observation lives in
// fixtures/pi-v1.3.0/audit-1050/padding-and-selection-reset.json, scenarios
// `box-set-padding-x-rerenders-dynamic-width` and
// `text-set-padding-x-rerenders-dynamic-width` (revision 71023aec1546).
//
// The expected values below are the frozen pi rows, not a recomputation of the
// C++ clamp: pi's `Box.setPaddingX` invalidates the cache and re-renders at
// the same width, and pi's `Text.setPaddingX` re-wraps the content at the new
// content width instead of only re-clamping the visible padding.
TEST_CASE("Box re-renders at a new horizontal padding like frozen pi", "[tui][box][padding][issue1121][spec]") {
    cch::tui::Box box(2, 0);
    REQUIRE(box.add_child(std::make_unique<cch::tui::Text>("hey", 0, 0)));

    const auto at_two = box.render(10);
    REQUIRE(at_two);
    CHECK(at_two->lines == std::vector<std::string>{"  hey     "});

    box.set_padding_x(3);
    const auto at_three = box.render(10);
    REQUIRE(at_three);
    CHECK(at_three->lines == std::vector<std::string>{"   hey    "});

    box.set_padding_x(0);
    const auto at_zero = box.render(10);
    REQUIRE(at_zero);
    CHECK(at_zero->lines == std::vector<std::string>{"hey       "});
}

TEST_CASE("Text re-wraps at a new horizontal padding like frozen pi",
        "[tui][text][padding][issue1121][issue1182][issue1220][spec]") {
    // The expected rows are the frozen observation, read from the registered artifact rather than
    // copied here, so an edited frozen row fails this comparison.
    const auto bundle = cch::tests::read_pi_v130_audit_evidence("audit-1050", "padding-and-selection-reset.json");
    REQUIRE(bundle);
    const cch::support::JsonValue& scenario =
            cch::tests::find_scenario(*bundle, "text-set-padding-x-rerenders-dynamic-width");
    const auto& inputs = scenario.at("inputs");
    const auto& expected = scenario.at("expected");
    const auto frozen_lines = [](const cch::support::JsonValue& rows) {
        std::vector<std::string> lines;
        for (const auto& row : rows.get_array())
            lines.push_back(row.get_string());
        return lines;
    };

    cch::tui::Text text(inputs.at("text").get_string(), 1, 0);
    const auto width = static_cast<std::size_t>(inputs.at("width").get_number());
    const auto& transitions = inputs.at("transitions").get_array();
    REQUIRE(transitions.size() == 3);
    REQUIRE(static_cast<std::size_t>(transitions[0].get_number()) == 1);

    const auto at_one = text.render(width);
    REQUIRE(at_one);
    CHECK(at_one->lines == frozen_lines(expected.at("atOne")));

    // A larger pad narrows the content width, so the text re-wraps instead of
    // keeping the previous single line.
    text.set_padding_x(static_cast<std::size_t>(transitions[1].get_number()));
    const auto at_three = text.render(width);
    REQUIRE(at_three);
    CHECK(at_three->lines == frozen_lines(expected.at("atThree")));

    text.set_padding_x(static_cast<std::size_t>(transitions[2].get_number()));
    const auto at_zero = text.render(width);
    REQUIRE(at_zero);
    CHECK(at_zero->lines == frozen_lines(expected.at("atZero")));
}

// pi's `Box.setPaddingX` invalidates the render cache, so a child whose
// rendered lines do not depend on the width still re-renders with the new
// padding. Without the invalidation the cached composition (the old padding)
// is returned, because the child's own lines are unchanged.
TEST_CASE("Box padding change invalidates the render cache",
        "[tui][box][padding][issue1182][issue1121][issue1209][issue1220][spec]") {
    cch::tui::Box box(2, 0);
    REQUIRE(box.add_child(std::make_unique<RawLineComponent>("hey")));

    const auto at_two = box.render(10);
    REQUIRE(at_two);
    CHECK(at_two->lines == std::vector<std::string>{"  hey     "});

    box.set_padding_x(3);
    const auto at_three = box.render(10);
    REQUIRE(at_three);
    CHECK(at_three->lines == std::vector<std::string>{"   hey    "});
}

TEST_CASE("Box cache keeps full padding and clamps narrow content to one column like frozen pi",
        "[tui][container][issue966][issue1220][spec]") {
    cch::tui::Box box(2, 1);
    REQUIRE(box.add_child(std::make_unique<cch::tui::Text>("hi", 0, 0)));

    const auto result = box.render(1);
    REQUIRE(result);
    CHECK(result->lines == std::vector<std::string>{" ", "  h", "  i", " "});
    CHECK(cch::tui::visible_width(result->lines[1]) == 3);

    const auto zero_width = box.render(0);
    REQUIRE(zero_width);
    CHECK(zero_width->lines == std::vector<std::string>{"", "  h", "  i", ""});
}

TEST_CASE("Text omits whitespace-only content and clamps narrow padding like frozen pi",
        "[tui][text][issue966][issue1220][spec]") {
    cch::tui::Text whitespace("  \t\n\r \xC2\xA0", 1, 1);
    const auto blank = whitespace.render(8);
    REQUIRE(blank);
    CHECK(blank->lines.empty());

    cch::tui::Text narrow("hello", 2, 1);
    const auto rendered = narrow.render(1);
    REQUIRE(rendered);
    CHECK(rendered->lines == std::vector<std::string>{" ", "h", "e", "l", "l", "o", " "});

    cch::tui::Text tabbed("a\tb", 0, 0);
    const auto tab_rendered = tabbed.render(12);
    REQUIRE(tab_rendered);
    CHECK(tab_rendered->lines == std::vector<std::string>{"a   b       "});
}

TEST_CASE("Text background hooks receive padded output without width rejection", "[tui][text][issue966][spec]") {
    cch::tui::Text text("x", 0, 0, [](std::string line) { return line + "x"; });
    const auto result = text.render(4);
    REQUIRE(result);
    REQUIRE(result->lines.size() == 1);
    CHECK(result->lines[0] == "x   x");
}

TEST_CASE("Box containing empty or whitespace Text has no rows", "[tui][box][issue966][spec]") {
    for (const auto& text : {std::string{}, std::string("  \t\n  ")}) {
        cch::tui::Box box(1, 1);
        REQUIRE(box.add_child(std::make_unique<cch::tui::Text>(text, 0, 0)));

        const auto result = box.render(10);
        REQUIRE(result);
        CHECK(result->lines.empty());
    }
}

// Frozen pi removeChild splices the matching child and leaves the object
// reference held by its caller usable. C++ returns the same ownership as a
// unique_ptr so that retained lifetime can be explicitly transferred back.
TEST_CASE("Container removes and reattaches a nonterminal child", "[tui][container][issue966][issue1220][spec]") {
    cch::tui::Container container;
    REQUIRE(container.add_child(std::make_unique<cch::tui::Text>("first", 0, 0)));
    auto spacer = std::make_unique<cch::tui::Spacer>(2);
    auto* spacer_ptr = spacer.get();
    REQUIRE(container.add_child(std::move(spacer)));
    REQUIRE(container.add_child(std::make_unique<cch::tui::Text>("last", 0, 0)));

    auto retained = container.remove_child(spacer_ptr);
    REQUIRE(retained);
    CHECK(retained.get() == spacer_ptr);
    CHECK_FALSE(container.remove_child(spacer_ptr));
    const auto without_spacer = container.render(10);
    REQUIRE(without_spacer);
    REQUIRE(without_spacer->lines.size() == 2);
    CHECK(without_spacer->lines[0].find("first") != std::string::npos);
    CHECK(without_spacer->lines[1].find("last") != std::string::npos);

    container.clear();
    REQUIRE(container.render(10));
    auto* retained_spacer = dynamic_cast<cch::tui::Spacer*>(retained.get());
    REQUIRE(retained_spacer != nullptr);
    retained_spacer->set_lines(3);
    REQUIRE(container.add_child(std::move(retained)));
    const auto reattached = container.render(10);
    REQUIRE(reattached);
    REQUIRE(reattached->lines.size() == 3);
    CHECK(std::all_of(
            reattached->lines.begin(), reattached->lines.end(), [](const auto& line) { return line.empty(); }));
}

TEST_CASE("Box removes re-adds and renders retained component capabilities", "[tui][box][issue966][spec]") {
    cch::tests::ImageCapabilitiesGuard capabilities(
            cch::tui::DetectedImageCapabilities{.images = cch::tui::InlineImageProtocol::Kitty});
    cch::tui::Box box(1, 1);
    auto input = std::make_unique<cch::tui::Input>();
    auto* input_ptr = input.get();
    input_ptr->set_value("cursor");
    input_ptr->set_focused(true);
    REQUIRE(box.add_child(std::move(input)));

    auto spacer = std::make_unique<cch::tui::Spacer>(1);
    auto* spacer_ptr = spacer.get();
    REQUIRE(box.add_child(std::move(spacer)));

    auto truncated = std::make_unique<cch::tui::TruncatedText>("abcdefghij");
    auto* truncated_ptr = truncated.get();
    REQUIRE(box.add_child(std::move(truncated)));
    auto image = std::make_unique<cch::tui::Image>(
            cch::tui::ImageContent{.encoded_data = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAAAElFTkSuQmCC",
                    .mime_type = "image/png",
                    .filename = "retained.png"});
    auto* image_ptr = image.get();
    REQUIRE(box.add_child(std::move(image)));

    auto retained_spacer = box.remove_child(spacer_ptr);
    auto retained_truncated = box.remove_child(truncated_ptr);
    auto retained_input = box.remove_child(input_ptr);
    auto retained_image = box.remove_child(image_ptr);
    REQUIRE(retained_spacer);
    REQUIRE(retained_truncated);
    REQUIRE(retained_input);
    REQUIRE(retained_image);
    CHECK(retained_spacer.get() == spacer_ptr);
    CHECK(retained_truncated.get() == truncated_ptr);
    CHECK(retained_input.get() == input_ptr);
    CHECK(retained_image.get() == image_ptr);
    CHECK_FALSE(box.remove_child(image_ptr));

    box.clear();
    REQUIRE(box.render(8));
    auto* retained_spacer_ptr = dynamic_cast<cch::tui::Spacer*>(retained_spacer.get());
    REQUIRE(retained_spacer_ptr != nullptr);
    retained_spacer_ptr->set_lines(2);
    auto* retained_truncated_ptr = dynamic_cast<cch::tui::TruncatedText*>(retained_truncated.get());
    REQUIRE(retained_truncated_ptr != nullptr);
    retained_truncated_ptr->set_text("abcdefghij");
    REQUIRE(box.add_child(std::move(retained_input)));
    REQUIRE(box.add_child(std::move(retained_spacer)));
    REQUIRE(box.add_child(std::move(retained_truncated)));
    REQUIRE(box.add_child(std::move(retained_image)));

    const auto rendered = box.render(8);
    REQUIRE(rendered);
    REQUIRE(rendered->images.size() == 1);
    CHECK(rendered->images[0].filename == std::optional<std::string>{"retained.png"});
    CHECK(rendered->images[0].region.row == 5);
    CHECK(rendered->images[0].region.column == 1);
    REQUIRE(rendered->lines.size() == 8);
    CHECK(rendered->lines[4] == " abc\x1b[0m...\x1b[0m ");
    REQUIRE(input_ptr->cursor_location());
    CHECK(input_ptr->cursor_location()->column > 0);
}

TEST_CASE("issue980 composes editor cursors through Containers and padded Boxes",
        "[tui][container][cursor][issue980][spec]") {
    cch::tui::Container container;
    for (std::size_t index = 0; index < 5; ++index)
        REQUIRE(container.add_child(std::make_unique<cch::tui::Text>(std::format("row {}", index), 0, 0)));

    auto outer_box = std::make_unique<cch::tui::Box>(2, 1);
    auto inner_container = std::make_unique<cch::tui::Container>();
    auto inner_box = std::make_unique<cch::tui::Box>(1, 2);
    auto editor = std::make_unique<cch::tui::Editor>();
    auto* editor_ptr = editor.get();
    editor_ptr->set_text("a\nb\nc");
    editor_ptr->set_focused(true);
    REQUIRE(inner_box->add_child(std::move(editor)));
    REQUIRE(inner_container->add_child(std::move(inner_box)));
    REQUIRE(outer_box->add_child(std::move(inner_container)));
    REQUIRE(container.add_child(std::move(outer_box)));

    const auto rendered = container.render(40);
    REQUIRE(rendered);
    REQUIRE(editor_ptr->cursor_location());
    CHECK(container.cursor_location() == cch::tui::CursorPosition{.column = 4, .row = 10});

    container.invalidate();
    CHECK(container.cursor_location() == std::nullopt);
    REQUIRE(container.render(40));
    CHECK(container.cursor_location() == cch::tui::CursorPosition{.column = 4, .row = 10});
    editor_ptr->set_focused(false);
    CHECK(container.cursor_location() == std::nullopt);
}

TEST_CASE("issue980 clears composed cursor after removing its child", "[tui][container][cursor][issue980][spec]") {
    cch::tui::Container container;
    auto editor = std::make_unique<cch::tui::Editor>();
    auto* editor_ptr = editor.get();
    editor_ptr->set_focused(true);
    REQUIRE(container.add_child(std::make_unique<cch::tui::Text>("before", 0, 0)));
    REQUIRE(container.add_child(std::move(editor)));
    REQUIRE(container.render(20));
    REQUIRE(container.cursor_location());

    auto removed = container.remove_child(editor_ptr);
    REQUIRE(removed);
    CHECK(container.cursor_location() == std::nullopt);
}

TEST_CASE("issue980 composes nested image metadata through Container and Box",
        "[tui][container][image][issue980][spec]") {
    cch::tests::ImageCapabilitiesGuard capabilities({.images = cch::tui::InlineImageProtocol::Kitty});
    cch::tui::Container container;
    REQUIRE(container.add_child(std::make_unique<cch::tui::Text>("five rows", 0, 0)));

    auto outer_box = std::make_unique<cch::tui::Box>(2, 1);
    auto inner_container = std::make_unique<cch::tui::Container>();
    auto inner_box = std::make_unique<cch::tui::Box>(1, 2);
    REQUIRE(inner_box->add_child(std::make_unique<cch::tui::Image>(
            cch::tui::ImageContent{.encoded_data = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAAAElFTkSuQmCC",
                    .mime_type = "image/png",
                    .filename = "nested.png"},
            cch::tui::ImageOptions{.dimensions = cch::tui::ImagePixelSize{.width = 1, .height = 1}})));
    REQUIRE(inner_container->add_child(std::move(inner_box)));
    REQUIRE(outer_box->add_child(std::move(inner_container)));
    REQUIRE(container.add_child(std::move(outer_box)));

    const auto rendered = container.render(40);
    REQUIRE(rendered);
    REQUIRE(rendered->images.size() == 1);
    CHECK(rendered->images[0].filename == std::optional<std::string>{"nested.png"});
    CHECK(rendered->images[0].region.row == 4);
    CHECK(rendered->images[0].region.column == 3);
}

TEST_CASE("Spacer renders empty lines", "[tui][issue46][container][spec]") {
    cch::tui::Spacer spacer(3);
    auto result = spacer.render(10);
    REQUIRE(result);
    CHECK(result->lines.size() == 3);
    for (const auto& line : result->lines) {
        CHECK(line.empty());
    }
}

TEST_CASE("Spacer can have lines updated", "[tui][issue46][container][spec]") {
    cch::tui::Spacer spacer(1);
    spacer.set_lines(5);
    auto result = spacer.render(10);
    REQUIRE(result);
    CHECK(result->lines.size() == 5);
}

TEST_CASE("Box background covers every cell of a styled row", "[tui][box][background][issue707][spec]") {
    // Regression for #707: a styled child line used to carry a full reset from
    // the component boundary, which cancelled the enclosing background so the
    // tint stopped where the text stopped.
    auto background = cch::tests::background_hook("\x1b[48;5;22m");
    cch::tui::VirtualTerminal terminal({.columns = 8, .rows = 4});
    cch::tui::Tui tui(terminal);
    auto box = std::make_unique<cch::tui::Box>(1, 1, std::move(background));
    REQUIRE(box->add_child(std::make_unique<RawLineComponent>("\x1b[2mdim")));
    REQUIRE(box->add_child(std::make_unique<RawLineComponent>("plain")));
    REQUIRE(tui.add_child(std::move(box)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());

    // Every cell of every row — top and bottom padding, the dim-styled content
    // row, and the unstyled content row — carries the configured background.
    cch::tests::check_background_cells(terminal, 4, 8, "48;5;22");

    // Exactly one full reset lands per composed row, after padding and after
    // the background hook; the background hook's own reset stays before it.
    const std::vector<std::string> expected_output{
            "\x1b[?2026h",
            "\x1b[48;5;22m        \x1b[49m\x1b[0m\x1b]8;;\x07",
            "\n\x1b[48;5;22m \x1b[2mdim    \x1b[49m\x1b[0m\x1b]8;;\x07",
            "\n\x1b[48;5;22m plain  \x1b[49m\x1b[0m\x1b]8;;\x07",
            "\n\x1b[48;5;22m        \x1b[49m\x1b[0m\x1b]8;;\x07",
            "\x1b[?2026l",
    };
    REQUIRE(terminal.output().size() == expected_output.size());
    for (std::size_t i = 0; i < expected_output.size(); ++i) {
        if (terminal.output()[i] != expected_output[i]) {
            for (char c : terminal.output()[i]) {
                if (c >= 32 && c <= 126)
                    printf("%c", c);
                else
                    printf("\\x%02x", (unsigned char)c);
            }
            printf("\n");
            for (char c : expected_output[i]) {
                if (c >= 32 && c <= 126)
                    printf("%c", c);
                else
                    printf("\\x%02x", (unsigned char)c);
            }
            printf("\n");
        }
        CHECK(terminal.output()[i] == expected_output[i]);
    }
}

TEST_CASE("pi-v1.3.0 Text styled mutation matches frozen observation", "[tui][text][component][issue1220][compat-pi]") {
    const auto bundle = cch::tests::read_pi_v130_audit_evidence("audit-1220", "component.json");
    REQUIRE(bundle);
    const auto& scenario = cch::tests::find_scenario(*bundle, "styled-text-mutation");
    const auto& inputs = scenario.at("inputs");
    const auto& expected = scenario.at("expected");

    const auto to_lines = [](const cch::support::JsonValue& val) {
        std::vector<std::string> lines;
        for (const auto& item : val.get_array())
            lines.push_back(item.get_string());
        return lines;
    };

    cch::tui::Text text(inputs.at("text").get_string(), 1, 1);
    const auto first = text.render(12);
    REQUIRE(first);
    CHECK(first->lines == to_lines(expected.at("first")));

    text.set_text(inputs.at("changedText").get_string());
    const auto changed = text.render(12);
    REQUIRE(changed);
    CHECK(changed->lines == to_lines(expected.at("changed")));
}

TEST_CASE("pi-v1.3.0 Text render cache and background hook match frozen observation",
        "[tui][text][component][issue1220][compat-pi]") {
    const auto bundle = cch::tests::read_pi_v130_audit_evidence("audit-1220", "component.json");
    REQUIRE(bundle);
    const auto& scenario = cch::tests::find_scenario(*bundle, "text-render-cache-and-bg");
    const auto& inputs = scenario.at("inputs");
    const auto& expected = scenario.at("expected");

    const auto to_lines = [](const cch::support::JsonValue& val) {
        std::vector<std::string> lines;
        for (const auto& item : val.get_array())
            lines.push_back(item.get_string());
        return lines;
    };

    cch::tui::Text text(inputs.at("text").get_string(), 1, 1);
    const auto initial = text.render(20);
    REQUIRE(initial);
    CHECK(initial->lines == to_lines(expected.at("initial")));

    // Re-rendering with identical width hits cache
    const auto cached = text.render(20);
    REQUIRE(cached);
    CHECK(cached->lines == to_lines(expected.at("cached")));

    // Re-rendering at narrow width recomputes
    const auto narrow = text.render(10);
    REQUIRE(narrow);
    CHECK(narrow->lines == to_lines(expected.at("narrow")));

    text.set_background_hook([](std::string s) { return "\x1b[44m" + s + "\x1b[49m"; });
    const auto with_bg = text.render(20);
    REQUIRE(with_bg);
    CHECK(with_bg->lines == to_lines(expected.at("withBg")));

    text.set_padding_x(2);
    const auto padded2 = text.render(20);
    REQUIRE(padded2);
    CHECK(padded2->lines == to_lines(expected.at("padded2")));

    text.invalidate();
    const auto invalidated = text.render(20);
    REQUIRE(invalidated);
    CHECK(invalidated->lines == to_lines(expected.at("invalidated")));
}

TEST_CASE("pi-v1.3.0 Box rendering padding background and cache match frozen observation",
        "[tui][box][component][issue1220][compat-pi]") {
    const auto bundle = cch::tests::read_pi_v130_audit_evidence("audit-1220", "component.json");
    REQUIRE(bundle);
    const auto& scenario = cch::tests::find_scenario(*bundle, "box-rendering-and-cache");
    const auto& expected = scenario.at("expected");

    const auto to_lines = [](const cch::support::JsonValue& val) {
        std::vector<std::string> lines;
        for (const auto& item : val.get_array())
            lines.push_back(item.get_string());
        return lines;
    };

    cch::tui::Box box(1, 1);
    auto child1 = std::make_unique<cch::tui::Text>("first child", 0, 0);
    auto child2 = std::make_unique<cch::tui::Text>("second child", 0, 0);
    auto* child2_ptr = child2.get();

    REQUIRE(box.add_child(std::move(child1)));
    REQUIRE(box.add_child(std::move(child2)));

    const auto initial = box.render(20);
    REQUIRE(initial);
    CHECK(initial->lines == to_lines(expected.at("initial")));

    const auto cached = box.render(20);
    REQUIRE(cached);
    CHECK(cached->lines == to_lines(expected.at("cached")));

    box.set_background_hook([](std::string s) { return "\x1b[42m" + s + "\x1b[49m"; });
    const auto with_bg = box.render(20);
    REQUIRE(with_bg);
    CHECK(with_bg->lines == to_lines(expected.at("withBg")));

    box.set_padding_x(2);
    const auto padded2 = box.render(20);
    REQUIRE(padded2);
    CHECK(padded2->lines == to_lines(expected.at("padded2")));

    auto removed = box.remove_child(child2_ptr);
    REQUIRE(removed);
    const auto without_child2 = box.render(20);
    REQUIRE(without_child2);
    CHECK(without_child2->lines == to_lines(expected.at("removedChild2")));

    box.invalidate();
    const auto invalidated = box.render(20);
    REQUIRE(invalidated);
    CHECK(invalidated->lines == to_lines(expected.at("invalidated")));

    box.clear();
    const auto cleared = box.render(20);
    REQUIRE(cleared);
    CHECK(cleared->lines == to_lines(expected.at("cleared")));
}

TEST_CASE("pi-v1.3.0 Box mouse event routing through padding matches frozen observation",
        "[tui][box][component][issue1220][compat-pi]") {
    const auto bundle = cch::tests::read_pi_v130_audit_evidence("audit-1220", "component.json");
    REQUIRE(bundle);
    const auto& scenario = cch::tests::find_scenario(*bundle, "box-mouse-routing");
    const auto& expected = scenario.at("expected");

    class ClickableChild final : public cch::tui::Component, public cch::tui::MouseHandler {
    public:
        [[nodiscard]] cch::support::Expected<cch::tui::RenderResult> render(std::size_t) override {
            return cch::tui::RenderResult{.lines = {"row 0", "row 1"}};
        }
        void invalidate() override {}
        [[nodiscard]] std::optional<cch::tui::MouseDispatchResult> handle_mouse_forwarded(
                const cch::tui::MouseEvent& event) override {
            last_event = event;
            return cch::tui::MouseDispatchResult{.handled = true};
        }
        std::optional<cch::tui::MouseEvent> last_event{};
    };

    cch::tui::Box box(2, 1);
    auto child = std::make_unique<ClickableChild>();
    auto* child_ptr = child.get();
    REQUIRE(box.add_child(std::move(child)));
    const auto rendered = box.render(20);
    REQUIRE(rendered);

    // Event inside child: (x=3, y=2)
    const auto event_inside = cch::tui::MouseEvent{
            .type = cch::tui::MouseEventType::Press, .button = cch::tui::MouseButton::Left, .x = 3, .y = 2};
    const auto result_inside = box.handle_mouse_forwarded(event_inside);
    CHECK(result_inside.has_value() == expected.at("resultInsideHandled").get_boolean());
    REQUIRE(child_ptr->last_event.has_value());
    CHECK(child_ptr->last_event->x ==
            static_cast<std::ptrdiff_t>(expected.at("childReceivedCoords").at("x").get_number()));
    CHECK(child_ptr->last_event->y ==
            static_cast<std::ptrdiff_t>(expected.at("childReceivedCoords").at("y").get_number()));

    // Event in left padding: (x=1, y=2)
    const auto event_left_pad = cch::tui::MouseEvent{
            .type = cch::tui::MouseEventType::Press, .button = cch::tui::MouseButton::Left, .x = 1, .y = 2};
    const auto result_left_pad = box.handle_mouse_forwarded(event_left_pad);
    CHECK(result_left_pad.has_value() == expected.at("resultLeftPadHandled").get_boolean());

    // Event in top padding: (x=3, y=0)
    const auto event_top_pad = cch::tui::MouseEvent{
            .type = cch::tui::MouseEventType::Press, .button = cch::tui::MouseButton::Left, .x = 3, .y = 0};
    const auto result_top_pad = box.handle_mouse_forwarded(event_top_pad);
    CHECK(result_top_pad.has_value() == expected.at("resultTopPadHandled").get_boolean());
}

TEST_CASE("pi-v1.3.0 Spacer rendering and line update match frozen observation",
        "[tui][spacer][component][issue1220][compat-pi]") {
    const auto bundle = cch::tests::read_pi_v130_audit_evidence("audit-1220", "component.json");
    REQUIRE(bundle);
    const auto& scenario = cch::tests::find_scenario(*bundle, "spacer-rendering");
    const auto& expected = scenario.at("expected");

    const auto to_lines = [](const cch::support::JsonValue& val) {
        std::vector<std::string> lines;
        for (const auto& item : val.get_array())
            lines.push_back(item.get_string());
        return lines;
    };

    cch::tui::Spacer spacer(3);
    const auto render3 = spacer.render(15);
    REQUIRE(render3);
    CHECK(render3->lines == to_lines(expected.at("render3")));

    spacer.set_lines(1);
    const auto render1 = spacer.render(15);
    REQUIRE(render1);
    CHECK(render1->lines == to_lines(expected.at("render1")));

    spacer.set_lines(0);
    const auto render0 = spacer.render(15);
    REQUIRE(render0);
    CHECK(render0->lines == to_lines(expected.at("render0")));

    spacer.invalidate();
}

TEST_CASE("pi-v1.3.0 TruncatedText single-line truncation and padding match frozen observation",
        "[tui][truncated][component][issue1220][compat-pi]") {
    const auto bundle = cch::tests::read_pi_v130_audit_evidence("audit-1220", "component.json");
    REQUIRE(bundle);
    const auto& scenario = cch::tests::find_scenario(*bundle, "truncated-text-rendering");
    const auto& inputs = scenario.at("inputs");
    const auto& expected = scenario.at("expected");

    const auto to_lines = [](const cch::support::JsonValue& val) {
        std::vector<std::string> lines;
        for (const auto& item : val.get_array())
            lines.push_back(item.get_string());
        return lines;
    };

    cch::tui::TruncatedText trunc(inputs.at("text").get_string(), 1, 1);
    const auto wide = trunc.render(25);
    REQUIRE(wide);
    CHECK(wide->lines == to_lines(expected.at("wide")));

    const auto narrow = trunc.render(12);
    REQUIRE(narrow);
    CHECK(narrow->lines == to_lines(expected.at("narrow")));

    cch::tui::TruncatedText ansi_trunc("\x1b[31mhello\x1b[0m world", 0, 0);
    const auto ansi_render = ansi_trunc.render(8);
    REQUIRE(ansi_render);
    CHECK(ansi_render->lines == to_lines(expected.at("ansiRender")));

    const auto zero_width = ansi_trunc.render(0);
    REQUIRE(zero_width);
    CHECK(zero_width->lines == to_lines(expected.at("zeroWidth")));

    trunc.invalidate();
}

TEST_CASE("Spacer zero lines renders empty vector", "[tui][spacer][boundary][issue1220]") {
    cch::tui::Spacer spacer(0);
    const auto render0 = spacer.render(20);
    REQUIRE(render0);
    CHECK(render0->lines.empty());

    spacer.set_lines(2);
    const auto render2 = spacer.render(20);
    REQUIRE(render2);
    CHECK(render2->lines.size() == 2);

    spacer.set_lines(0);
    const auto render_empty = spacer.render(20);
    REQUIRE(render_empty);
    CHECK(render_empty->lines.empty());
}

TEST_CASE(
        "Box mouse routing rejects coordinates outside content bounds and padding", "[tui][box][boundary][issue1220]") {
    cch::tui::Box box(2, 2);
    auto child = std::make_unique<cch::tui::Text>("hello", 0, 0);
    REQUIRE(box.add_child(std::move(child)));
    REQUIRE(box.render(10));

    // Coordinates past the right boundary
    const auto past_right = cch::tui::MouseEvent{
            .type = cch::tui::MouseEventType::Press, .button = cch::tui::MouseButton::Left, .x = 15, .y = 2};
    CHECK_FALSE(box.handle_mouse_forwarded(past_right).has_value());

    // Negative coordinates
    const auto negative = cch::tui::MouseEvent{
            .type = cch::tui::MouseEventType::Press, .button = cch::tui::MouseButton::Left, .x = -1, .y = 2};
    CHECK_FALSE(box.handle_mouse_forwarded(negative).has_value());
}

TEST_CASE("TruncatedText discriminating boundary: clamps padding to one column content width",
        "[tui][truncated][boundary][issue1220]") {
    cch::tui::TruncatedText text("hello world", 2, 1);
    const auto result = text.render(4);
    REQUIRE(result);
    REQUIRE(result->lines.size() == 3);
    CHECK(result->lines[1] == "  \x1b[0m.\x1b[0m  ");
    CHECK(cch::tui::visible_width(result->lines[1]) == 5);
}
