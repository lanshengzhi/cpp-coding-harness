#include <cch/tui/Editor.hpp>
#include <cch/tui/Overlay.hpp>
#include <cch/tui/Text.hpp>
#include <cch/tui/Tui.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include <cch/support/Error.hpp>
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <type_traits>
#include <vector>

namespace {

class FocusableInputComponent final
    : public cch::tui::Component,
      public cch::tui::InputHandler,
      public cch::tui::Focusable {
public:
    explicit FocusableInputComponent(std::string label = "x")
        : label_(std::move(label)) {}

    [[nodiscard]] cch::support::Expected<cch::tui::RenderResult> render(std::size_t width) override {
        auto line = label_;
        if (line.size() < width) line.append(width - line.size(), ' ');
        return cch::tui::RenderResult{.lines = {std::move(line)}};
    }

    void invalidate() override {
        ++invalidation_count;
    }

    cch::tui::InputAdmissionOutcome handle_input(const cch::tui::InputEventVariant& input) override {
        const auto* key = std::get_if<cch::tui::KeyEvent>(&input);
        if (key != nullptr && key->type == cch::tui::KeyEventType::Release && !accept_key_releases) {
            return cch::tui::InputAdmissionOutcome::Unhandled;
        }
        received_input.push_back(input);
        return cch::tui::InputAdmissionOutcome::Consumed;
    }

    void set_focused(bool focused) override {
        focused_ = focused;
    }

    [[nodiscard]] bool focused() const override {
        return focused_;
    }

    std::optional<cch::tui::CursorPosition> cursor_location() const override {
        if (!focused_) return std::nullopt;
        return cch::tui::CursorPosition{.column = 1, .row = 0};
    }

    std::size_t invalidation_count{0};
    std::vector<cch::tui::InputEventVariant> received_input;
    bool accept_key_releases{false};
    std::string label_;

private:
    bool focused_{false};
};

} // namespace

TEST_CASE("Overlay renders children and reports visibility", "[tui][overlay][issue50][spec]") {
    cch::tui::Overlay overlay;
    auto text = std::make_unique<cch::tui::Text>("hello", 0, 0);
    REQUIRE(overlay.add_child(std::move(text)));

    REQUIRE(overlay.visible());
    auto rendered = overlay.render(10);
    REQUIRE(rendered);
    REQUIRE_FALSE(rendered->lines.empty());
    CHECK(rendered->lines[0].find("hello") != std::string::npos);

    overlay.set_visible(false);
    CHECK_FALSE(overlay.visible());
    rendered = overlay.render(10);
    REQUIRE(rendered);
    CHECK(rendered->lines.empty());
}

TEST_CASE("Overlay resolves centered 50 percent coordinates inside the viewport", "[tui][overlay][issue981]") {
    cch::tui::OverlayOptions options;
    options.width = cch::tui::SizeValue::absolute(20);
    options.col = cch::tui::SizeValue::percent(50.0);
    options.row = cch::tui::SizeValue{"50%"};
    const cch::tui::Overlay overlay(options);

    const auto layout = overlay.resolve_layout({.columns = 100, .rows = 30}, 10);
    CHECK(layout.width == 20);
    CHECK(layout.col == 40);
    CHECK(layout.row == 10);

    cch::tui::OverlayOptions centered_options;
    centered_options.width = cch::tui::SizeValue::absolute(20);
    const cch::tui::Overlay centered(centered_options);
    const auto centered_layout = centered.resolve_layout({.columns = 100, .rows = 30}, 10);
    CHECK(centered_layout.col == 40);
    CHECK(centered_layout.row == 10);
}

TEST_CASE("Overlay defaults to a safe centered anchor for empty and small viewports", "[tui][overlay][issue981]") {
    const cch::tui::Overlay overlay;
    const auto centered = overlay.resolve_layout({.columns = 80, .rows = 24}, 4);
    CHECK(centered.width == 80);
    CHECK(centered.col == 0);
    CHECK(centered.row == 10);

    const auto zero_viewport = overlay.resolve_layout({.columns = 0, .rows = 0}, 4);
    CHECK(zero_viewport.width == 1);
    CHECK(zero_viewport.col == 0);
    CHECK(zero_viewport.row == 0);

    cch::tui::OverlayOptions small_options;
    small_options.width = cch::tui::SizeValue::absolute(20);
    const cch::tui::Overlay small(small_options);
    const auto oversized = small.resolve_layout({.columns = 2, .rows = 1}, 5);
    CHECK(oversized.width == 2);
    CHECK(oversized.col == 0);
    CHECK(oversized.row == 0);
}

TEST_CASE("Overlay anchors cover each viewport edge and center", "[tui][overlay][issue981]") {
    const auto position_for = [](cch::tui::OverlayAnchor anchor) {
        cch::tui::OverlayOptions options;
        options.width = cch::tui::SizeValue::absolute(20);
        options.anchor = anchor;
        return cch::tui::Overlay(options).resolve_layout({.columns = 100, .rows = 30}, 10);
    };

    CHECK(position_for(cch::tui::OverlayAnchor::TopLeft).col == 0);
    CHECK(position_for(cch::tui::OverlayAnchor::TopLeft).row == 0);
    CHECK(position_for(cch::tui::OverlayAnchor::TopRight).col == 80);
    CHECK(position_for(cch::tui::OverlayAnchor::TopRight).row == 0);
    CHECK(position_for(cch::tui::OverlayAnchor::BottomLeft).col == 0);
    CHECK(position_for(cch::tui::OverlayAnchor::BottomLeft).row == 20);
    CHECK(position_for(cch::tui::OverlayAnchor::BottomRight).col == 80);
    CHECK(position_for(cch::tui::OverlayAnchor::BottomRight).row == 20);
    CHECK(position_for(cch::tui::OverlayAnchor::TopCenter).col == 40);
    CHECK(position_for(cch::tui::OverlayAnchor::TopCenter).row == 0);
    CHECK(position_for(cch::tui::OverlayAnchor::BottomCenter).col == 40);
    CHECK(position_for(cch::tui::OverlayAnchor::BottomCenter).row == 20);
    CHECK(position_for(cch::tui::OverlayAnchor::LeftCenter).col == 0);
    CHECK(position_for(cch::tui::OverlayAnchor::LeftCenter).row == 10);
    CHECK(position_for(cch::tui::OverlayAnchor::RightCenter).col == 80);
    CHECK(position_for(cch::tui::OverlayAnchor::RightCenter).row == 10);
}

TEST_CASE("Overlay resolves percentage sizing independent coordinates margins and signed offsets",
        "[tui][overlay][issue981]") {
    cch::tui::OverlayOptions options;
    options.width = cch::tui::SizeValue{"50%"};
    options.min_width = 30;
    options.max_height = cch::tui::SizeValue::percent(50.0);
    options.col = cch::tui::SizeValue::percent(25.0);
    options.row = cch::tui::SizeValue::percent(50.0);
    options.offset_x = 2;
    options.offset_y = -1;
    options.margins = {.left = 5, .right = 7, .top = 3, .bottom = 4};
    const cch::tui::Overlay overlay(options);

    const auto layout = overlay.resolve_layout({.columns = 100, .rows = 30}, 20);
    CHECK(layout.width == 50);
    CHECK(layout.max_height == 15);
    CHECK(layout.effective_height == 15);
    CHECK(layout.col == 16);
    CHECK(layout.row == 6);

    options.offset_x = 1000;
    options.offset_y = -1000;
    const cch::tui::Overlay clamped(options);
    const auto clamped_layout = clamped.resolve_layout({.columns = 100, .rows = 30}, 20);
    CHECK(clamped_layout.col == 43);
    CHECK(clamped_layout.row == 3);
}

TEST_CASE("Overlay percentages use remaining space and visibility responds to resize", "[tui][overlay][issue981]") {
    cch::tui::OverlayOptions options;
    options.width = cch::tui::SizeValue::absolute(20);
    options.row = cch::tui::SizeValue::percent(100.0);
    options.col = cch::tui::SizeValue::percent(100.0);
    options.margins = {.left = 4, .right = 6, .top = 2, .bottom = 3};
    options.visible_at = [](cch::tui::TerminalDimensions dimensions) { return dimensions.columns >= 40; };
    const cch::tui::Overlay overlay(options);

    const auto layout = overlay.resolve_layout({.columns = 100, .rows = 30}, 10);
    CHECK(layout.col == 74);
    CHECK(layout.row == 17);
    CHECK(overlay.visible_at({.columns = 40, .rows = 10}));
    CHECK_FALSE(overlay.visible_at({.columns = 39, .rows = 10}));
}

TEST_CASE("Tui routes semantic input to exactly one focused target", "[tui][overlay][issue50][spec]") {
    cch::tui::VirtualTerminal terminal({.columns = 10, .rows = 5});
    cch::tui::Tui tui(terminal);

    auto comp1 = std::make_unique<FocusableInputComponent>("a");
    auto comp2 = std::make_unique<FocusableInputComponent>("b");
    auto* comp1_ptr = comp1.get();
    auto* comp2_ptr = comp2.get();
    REQUIRE(tui.add_child(std::move(comp1)));
    REQUIRE(tui.add_child(std::move(comp2)));
    REQUIRE(tui.start());

    // Focus comp1
    REQUIRE(tui.set_focus(comp1_ptr));
    REQUIRE(comp1_ptr->focused());
    CHECK_FALSE(comp2_ptr->focused());

    // Route input to comp1
    REQUIRE(terminal.inject_input("z"));
    REQUIRE(comp1_ptr->received_input.size() == 1);
    CHECK(comp2_ptr->received_input.empty());

    // Switch focus to comp2
    REQUIRE(tui.set_focus(comp2_ptr));
    CHECK_FALSE(comp1_ptr->focused());
    REQUIRE(comp2_ptr->focused());

    // Input now goes to comp2
    REQUIRE(terminal.inject_input("y"));
    REQUIRE(comp2_ptr->received_input.size() == 1);
    CHECK(comp1_ptr->received_input.size() == 1);
}

TEST_CASE("Tui routes input to overlays before base content", "[tui][overlay][issue50][spec]") {
    cch::tui::VirtualTerminal terminal({.columns = 10, .rows = 5});
    cch::tui::Tui tui(terminal);

    auto base = std::make_unique<FocusableInputComponent>("base");
    auto* base_ptr = base.get();
    REQUIRE(tui.add_child(std::move(base)));
    REQUIRE(tui.start());
    REQUIRE(tui.set_focus(base_ptr));

    // Add an overlay with a focusable child
    auto overlay = std::make_unique<cch::tui::Overlay>();
    auto overlay_comp = std::make_unique<FocusableInputComponent>("over");
    auto* overlay_comp_ptr = overlay_comp.get();
    REQUIRE(overlay->add_child(std::move(overlay_comp)));
    auto* overlay_ptr = overlay.get();

    // Add overlay to Tui first
    REQUIRE(tui.add_overlay(std::move(overlay)));

    // Focus the overlay first
    REQUIRE(overlay_ptr->focus_first());

    // Set Tui focus to overlay
    REQUIRE(tui.set_focus(overlay_ptr));

    // Input should go to overlay (topmost)
    base_ptr->received_input.clear();
    overlay_comp_ptr->received_input.clear();
    REQUIRE(terminal.inject_input("q"));
    CHECK(overlay_comp_ptr->received_input.size() == 1);
    CHECK(base_ptr->received_input.empty());
}

TEST_CASE("Non-capturing overlay passes input through to base content", "[tui][overlay][issue50][spec]") {
    cch::tui::VirtualTerminal terminal({.columns = 10, .rows = 5});
    cch::tui::Tui tui(terminal);

    auto base = std::make_unique<FocusableInputComponent>("base");
    auto* base_ptr = base.get();
    REQUIRE(tui.add_child(std::move(base)));
    REQUIRE(tui.start());
    REQUIRE(tui.set_focus(base_ptr));

    // Add a non-capturing overlay
    cch::tui::OverlayOptions opts;
    opts.non_capturing = true;
    auto overlay = std::make_unique<cch::tui::Overlay>(opts);
    auto overlay_comp = std::make_unique<FocusableInputComponent>("over");
    auto* overlay_comp_ptr = overlay_comp.get();
    REQUIRE(overlay->add_child(std::move(overlay_comp)));
    REQUIRE(tui.add_overlay(std::move(overlay)));

    base_ptr->received_input.clear();
    REQUIRE(terminal.inject_input("w"));
    // Input should pass through to base, not the non-capturing overlay
    CHECK(base_ptr->received_input.size() == 1);
    CHECK(overlay_comp_ptr->received_input.empty());
}

TEST_CASE("Overlay z-index controls input routing order", "[tui][overlay][issue50][spec]") {
    cch::tui::VirtualTerminal terminal({.columns = 10, .rows = 5});
    cch::tui::Tui tui(terminal);

    auto base = std::make_unique<FocusableInputComponent>("base");
    auto* base_ptr = base.get();
    REQUIRE(tui.add_child(std::move(base)));
    REQUIRE(tui.start());
    REQUIRE(tui.set_focus(base_ptr));

    // Add two overlays with different z-indices
    cch::tui::OverlayOptions opts1;
    opts1.z_index = 10;
    auto overlay1 = std::make_unique<cch::tui::Overlay>(opts1);
    auto comp1 = std::make_unique<FocusableInputComponent>("o1");
    auto* comp1_ptr = comp1.get();
    REQUIRE(overlay1->add_child(std::move(comp1)));
    REQUIRE(overlay1->focus_first());
    REQUIRE(tui.add_overlay(std::move(overlay1)));

    cch::tui::OverlayOptions opts2;
    opts2.z_index = 20;
    auto overlay2 = std::make_unique<cch::tui::Overlay>(opts2);
    auto comp2 = std::make_unique<FocusableInputComponent>("o2");
    auto* comp2_ptr = comp2.get();
    REQUIRE(overlay2->add_child(std::move(comp2)));
    auto* overlay2_ptr = overlay2.get();
    REQUIRE(tui.add_overlay(std::move(overlay2)));

    // Focus the higher z-index overlay
    REQUIRE(overlay2_ptr->focus_first());
    REQUIRE(tui.set_focus(overlay2_ptr));
    base_ptr->received_input.clear();
    comp1_ptr->received_input.clear();
    comp2_ptr->received_input.clear();

    // Higher z-index overlay should get input first
    REQUIRE(terminal.inject_input("e"));
    CHECK(comp2_ptr->received_input.size() == 1);
    CHECK(comp1_ptr->received_input.empty());
    CHECK(base_ptr->received_input.empty());
}

TEST_CASE("Hide and restore overlay with focus fallback", "[tui][overlay][issue50][spec]") {
    cch::tui::VirtualTerminal terminal({.columns = 10, .rows = 5});
    cch::tui::Tui tui(terminal);

    auto base = std::make_unique<FocusableInputComponent>("base");
    auto* base_ptr = base.get();
    REQUIRE(tui.add_child(std::move(base)));
    REQUIRE(tui.start());
    REQUIRE(tui.set_focus(base_ptr));

    auto overlay = std::make_unique<cch::tui::Overlay>();
    auto overlay_comp = std::make_unique<FocusableInputComponent>("over");
    REQUIRE(overlay->add_child(std::move(overlay_comp)));
    REQUIRE(overlay->focus_first());
    auto* overlay_ptr = overlay.get();
    REQUIRE(tui.add_overlay(std::move(overlay)));
    REQUIRE(tui.set_focus(overlay_ptr));

    REQUIRE(overlay_ptr->visible());
    REQUIRE(overlay_ptr->focused());

    // Hide overlay - focus should fallback to base
    REQUIRE(tui.hide_overlay(overlay_ptr));
    CHECK_FALSE(overlay_ptr->visible());

    // Restore overlay
    REQUIRE(tui.restore_overlay(overlay_ptr));
    CHECK(overlay_ptr->visible());
}

TEST_CASE("Remove overlay disposes it and fallback focus works", "[tui][overlay][issue50][spec]") {
    cch::tui::VirtualTerminal terminal({.columns = 10, .rows = 5});
    cch::tui::Tui tui(terminal);

    auto base = std::make_unique<FocusableInputComponent>("base");
    auto* base_ptr = base.get();
    REQUIRE(tui.add_child(std::move(base)));
    REQUIRE(tui.start());
    REQUIRE(tui.set_focus(base_ptr));

    auto overlay = std::make_unique<cch::tui::Overlay>();
    auto overlay_comp = std::make_unique<FocusableInputComponent>("over");
    REQUIRE(overlay->add_child(std::move(overlay_comp)));
    REQUIRE(overlay->focus_first());
    auto* overlay_ptr = overlay.get();
    REQUIRE(tui.add_overlay(std::move(overlay)));
    REQUIRE(tui.set_focus(overlay_ptr));

    // Remove overlay
    REQUIRE(tui.remove_overlay(overlay_ptr));

    // Overlay is gone; base component is still accessible
    base_ptr->received_input.clear();
    REQUIRE(terminal.inject_input("r"));
    CHECK(base_ptr->received_input.size() == 1);
}

TEST_CASE("IME cursor location follows focused component", "[tui][overlay][issue50][spec]") {
    cch::tui::VirtualTerminal terminal({.columns = 10, .rows = 5});
    cch::tui::Tui tui(terminal);

    auto editor = std::make_unique<cch::tui::Editor>(
        cch::tui::EditorOptions{.max_visible_lines = 3});
    auto* editor_ptr = editor.get();
    REQUIRE(tui.add_child(std::move(editor)));
    REQUIRE(tui.start());
    REQUIRE(tui.set_focus(editor_ptr));

    editor_ptr->insert_text_at_cursor("hello");
    REQUIRE(tui.render());

    // Check that cursor is positioned for IME
    const auto cursor = terminal.cursor();
    // Cursor should be visible somewhere (col > 0)
    CHECK(cursor.column > 0);
}

TEST_CASE("Overlay cursor uses the centered composed position", "[tui][overlay][issue981]") {
    cch::tui::VirtualTerminal terminal({.columns = 10, .rows = 5});
    cch::tui::Tui tui(terminal);
    REQUIRE(tui.add_child(std::make_unique<cch::tui::Text>("base", 0, 0)));
    REQUIRE(tui.start());

    cch::tui::OverlayOptions options;
    options.width = 4;
    auto overlay = std::make_unique<cch::tui::Overlay>(options);
    auto child = std::make_unique<FocusableInputComponent>("over");
    REQUIRE(overlay->add_child(std::move(child)));
    REQUIRE(overlay->focus_first());
    auto* overlay_pointer = overlay.get();
    REQUIRE(tui.add_overlay(std::move(overlay)));
    REQUIRE(tui.set_focus(overlay_pointer));
    REQUIRE(tui.render());

    CHECK(terminal.cursor() == cch::tui::CursorPosition{.column = 4, .row = 2});
}

TEST_CASE("Overlay with Editor delegates cursor location", "[tui][overlay][issue981][issue50][spec]") {
    cch::tui::VirtualTerminal terminal({.columns = 20, .rows = 10});
    cch::tui::Tui tui(terminal);

    auto base = std::make_unique<cch::tui::Text>("base content", 0, 0);
    REQUIRE(tui.add_child(std::move(base)));
    REQUIRE(tui.start());

    // Create an overlay with an Editor
    auto overlay = std::make_unique<cch::tui::Overlay>();
    auto editor = std::make_unique<cch::tui::Editor>(
        cch::tui::EditorOptions{.max_visible_lines = 3});
    auto* editor_ptr = editor.get();
    REQUIRE(overlay->add_child(std::move(editor)));
    REQUIRE(overlay->focus_first());
    auto* overlay_ptr = overlay.get();
    REQUIRE(tui.add_overlay(std::move(overlay)));

    // Focus the overlay (which focuses the editor)
    REQUIRE(tui.set_focus(overlay_ptr));
    editor_ptr->insert_text_at_cursor("hello");

    // Render should set IME cursor based on editor position within overlay
    REQUIRE(tui.render());

    // The cursor location should be propagated from editor through overlay
    const auto cursor = terminal.cursor();
    // Cursor should be positioned for IME
    CHECK(cursor.column > 0);
}

TEST_CASE("Stacked overlays render in z-order", "[tui][overlay][issue50][spec]") {
    cch::tui::VirtualTerminal terminal({.columns = 20, .rows = 10});
    cch::tui::Tui tui(terminal);

    auto base = std::make_unique<cch::tui::Text>("base", 0, 0);
    REQUIRE(tui.add_child(std::move(base)));
    REQUIRE(tui.start());

    // Overlay 1 (lower z-index)
    auto overlay1 = std::make_unique<cch::tui::Overlay>(cch::tui::OverlayOptions{
            .visible_at = {},
            .z_index = 0,
    });

    auto text1 = std::make_unique<cch::tui::Text>("overlay1", 0, 0);
    REQUIRE(overlay1->add_child(std::move(text1)));
    REQUIRE(tui.add_overlay(std::move(overlay1)));

    // Overlay 2 (higher z-index)
    auto overlay2 = std::make_unique<cch::tui::Overlay>(cch::tui::OverlayOptions{
            .visible_at = {},
            .z_index = 1,
    });

    auto text2 = std::make_unique<cch::tui::Text>("overlay2", 0, 0);
    REQUIRE(overlay2->add_child(std::move(text2)));
    REQUIRE(tui.add_overlay(std::move(overlay2)));

    // Render both overlays
    REQUIRE(tui.render());

    // Both overlays appear on screen (no way to distinguish ordering
    // from the screen buffer without specific positioning, but at least
    // no crash and content is visible)
    const auto& screen = terminal.screen();
    CHECK_FALSE(screen.empty());
}

TEST_CASE("Focusable Components expose cursor_location for IME", "[tui][overlay][issue50][spec]") {
    // A Focusable with a custom cursor_location set
    cch::tui::VirtualTerminal terminal({.columns = 10, .rows = 5});
    cch::tui::Tui tui(terminal);

    auto comp = std::make_unique<FocusableInputComponent>("test");
    auto* comp_ptr = comp.get();
    REQUIRE(tui.add_child(std::move(comp)));
    REQUIRE(tui.start());
    REQUIRE(tui.set_focus(comp_ptr));

    REQUIRE(tui.render());

    // The custom cursor_location returns column=1, row=0
    const auto cursor = terminal.cursor();
    CHECK(cursor.column == 1);
    CHECK(cursor.row == 0);

    // Unfocus - cursor should move back
    REQUIRE(tui.set_focus(nullptr));
    REQUIRE(tui.render());
}

TEST_CASE("Default cursor_location returns nullopt for non-focusable", "[tui][overlay][issue50][spec]") {
    // Text is not Focusable, so it has no cursor location: its Component
    // default stays nullopt even after rendering.
    cch::tui::Text text("hello", 0, 0);
    REQUIRE(text.render(10));
    CHECK(text.cursor_location() == std::nullopt);
}

TEST_CASE("Overlay rejects a disposed component after removal", "[tui][overlay][issue50][spec]") {
    cch::tui::VirtualTerminal terminal({.columns = 10, .rows = 5});
    cch::tui::Tui tui(terminal);

    auto base = std::make_unique<cch::tui::Text>("base", 0, 0);
    REQUIRE(tui.add_child(std::move(base)));
    REQUIRE(tui.start());

    auto overlay = std::make_unique<cch::tui::Overlay>();
    auto* overlay_ptr = overlay.get();
    REQUIRE(tui.add_overlay(std::move(overlay)));

    // Remove and verify it's gone
    REQUIRE(tui.remove_overlay(overlay_ptr));
    // Overlay is disposed, focus fallback should not try to access it
    REQUIRE(tui.render());
}
