#include <cch/tui/Container.hpp>
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
    explicit RawLineComponent(std::string line)
        : line_(std::move(line)) {}

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
    cch::tui::Box box(0, 0, [owned = std::move(prefix)](std::string line) {
        return *owned + line;
    });
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

TEST_CASE("Box keeps full padding and clamps narrow content to one column like frozen pi",
        "[tui][container][issue966][spec]") {
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

TEST_CASE(
        "Text omits whitespace-only content and clamps narrow padding like frozen pi", "[tui][text][issue966][spec]") {
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
TEST_CASE("Container removes and reattaches a nonterminal child", "[tui][container][issue966][spec]") {
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
            "\x1b[48;5;22m \x1b[2mdim    \x1b[49m\x1b[0m\x1b]8;;\x07",
            "\x1b[48;5;22m plain  \x1b[49m\x1b[0m\x1b]8;;\x07",
            "\x1b[48;5;22m        \x1b[49m\x1b[0m\x1b]8;;\x07",
            "\x1b[?2026l",
    };
    CHECK(terminal.output() == expected_output);
}
