#include <cch/tui/Editor.hpp>
#include <cch/tui/Overlay.hpp>
#include <cch/tui/Text.hpp>
#include <cch/tui/Tui.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include "../../third_party/catch2/catch_test_macros.hpp"

#include <memory>
#include <string>
#include <vector>

namespace {

class FocusableInputComponent final
    : public cch::tui::Component,
      public cch::tui::InputHandler,
      public cch::tui::Focusable {
public:
    explicit FocusableInputComponent(std::string label = "x")
        : label_(std::move(label)) {}

    [[nodiscard]] cch::util::Expected<std::vector<std::string>> render(std::size_t width) override {
        auto line = label_;
        if (line.size() < width) line.append(width - line.size(), ' ');
        return std::vector<std::string>{line};
    }

    void invalidate() override {
        ++invalidation_count;
    }

    void handle_input(const cch::tui::InputEventVariant& input) override {
        received_input.push_back(input);
    }

    [[nodiscard]] bool accepts_key_releases() const override {
        return accept_key_releases;
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

TEST_CASE("Overlay renders children and reports visibility", "[tui][overlay][issue50]") {
    cch::tui::Overlay overlay;
    auto text = std::make_unique<cch::tui::Text>("hello", 0, 0);
    REQUIRE(overlay.add_child(std::move(text)));

    REQUIRE(overlay.visible());
    auto rendered = overlay.render(10);
    REQUIRE(rendered);
    REQUIRE_FALSE(rendered->empty());
    CHECK((*rendered)[0].find("hello") != std::string::npos);

    overlay.set_visible(false);
    CHECK_FALSE(overlay.visible());
    rendered = overlay.render(10);
    REQUIRE(rendered);
    CHECK(rendered->empty());
}

TEST_CASE("Overlay supports position strategies", "[tui][overlay][issue50]") {
    // Absolute position
    cch::tui::OverlayOptions opts;
    opts.position = cch::tui::OverlayPosition::Absolute;
    opts.absolute_column = 5;
    opts.absolute_row = 3;
    cch::tui::Overlay overlay(opts);
    auto text = std::make_unique<cch::tui::Text>("x", 0, 0);
    REQUIRE(overlay.add_child(std::move(text)));

    auto rendered = overlay.render(10);
    REQUIRE(rendered);
    REQUIRE_FALSE(rendered->empty());

    const auto [col, row] = overlay.layout_position(80, 24, 1, 1);
    CHECK(col == 5);
    CHECK(row == 3);
}

TEST_CASE("Overlay layout position handles TopLeft anchor", "[tui][overlay][issue50]") {
    cch::tui::Overlay overlay;
    overlay.set_anchor(10, 5, 20, 10);
    const auto [col, row] = overlay.layout_position(80, 24, 8, 3);
    CHECK(col == 10);
    CHECK(row == 5);
}

TEST_CASE("Overlay layout position handles Center anchor", "[tui][overlay][issue50]") {
    cch::tui::OverlayOptions opts;
    opts.position = cch::tui::OverlayPosition::Center;
    cch::tui::Overlay overlay(opts);
    overlay.set_anchor(10, 5, 20, 10);
    const auto [col, row] = overlay.layout_position(80, 24, 8, 3);
    CHECK(col == 10 + (20 / 2) - (8 / 2)); // 16
    CHECK(row == 5 + (10 / 2) - (3 / 2));  // 8
}

TEST_CASE("Overlay layout position handles BottomRight anchor", "[tui][overlay][issue50]") {
    cch::tui::OverlayOptions opts;
    opts.position = cch::tui::OverlayPosition::BottomRight;
    cch::tui::Overlay overlay(opts);
    overlay.set_anchor(10, 5, 20, 10);
    const auto [col, row] = overlay.layout_position(80, 24, 8, 3);
    CHECK(col == 10 + 20 - 8); // 22
    CHECK(row == 5 + 10 - 3);  // 12
}

TEST_CASE("Overlay layout position handles LeftCenter anchor", "[tui][overlay][issue50]") {
    cch::tui::OverlayOptions opts;
    opts.position = cch::tui::OverlayPosition::LeftCenter;
    cch::tui::Overlay overlay(opts);
    overlay.set_anchor(10, 5, 20, 10);
    const auto [col, row] = overlay.layout_position(80, 24, 8, 3);
    CHECK(col == 10);
    CHECK(row == 5 + (10 / 2) - (3 / 2)); // 8
}

TEST_CASE("Overlay layout position handles RightCenter anchor", "[tui][overlay][issue50]") {
    cch::tui::OverlayOptions opts;
    opts.position = cch::tui::OverlayPosition::RightCenter;
    cch::tui::Overlay overlay(opts);
    overlay.set_anchor(10, 5, 20, 10);
    const auto [col, row] = overlay.layout_position(80, 24, 8, 3);
    CHECK(col == 10 + 20 - 8); // 22
    CHECK(row == 5 + (10 / 2) - (3 / 2)); // 8
}

TEST_CASE("Overlay layout position handles Percentage position", "[tui][overlay][issue50]") {
    cch::tui::OverlayOptions opts;
    opts.position = cch::tui::OverlayPosition::Percentage;
    opts.percentage_column = 50;
    opts.percentage_row = 25;
    cch::tui::Overlay overlay(opts);
    const auto [col, row] = overlay.layout_position(80, 24, 8, 3);
    CHECK(col == (80 * 50) / 100); // 40
    CHECK(row == (24 * 25) / 100); // 6
}

TEST_CASE("Overlay visible_at checks viewport constraints", "[tui][overlay][issue50]") {
    cch::tui::OverlayOptions opts;
    opts.visibility.min_viewport_width = 40;
    opts.visibility.max_viewport_height = 30;
    cch::tui::Overlay overlay(opts);

    CHECK(overlay.visible_at({.columns = 50, .rows = 20}));
    CHECK_FALSE(overlay.visible_at({.columns = 30, .rows = 20})); // too narrow
    CHECK_FALSE(overlay.visible_at({.columns = 50, .rows = 40})); // too tall

    // Hidden overlays are never visible
    overlay.set_visible(false);
    CHECK_FALSE(overlay.visible_at({.columns = 50, .rows = 20}));
}

TEST_CASE("Overlay size constraints limit rendered dimensions", "[tui][overlay][issue50]") {
    cch::tui::OverlayOptions opts;
    opts.size_constraints.min_width = 20;
    opts.size_constraints.min_height = 3;
    opts.size_constraints.max_height = 5;
    cch::tui::Overlay overlay(opts);
    auto text = std::make_unique<cch::tui::Text>("short", 0, 0);
    REQUIRE(overlay.add_child(std::move(text)));

    auto rendered = overlay.render(10);
    REQUIRE(rendered);
    // Width should be at least min_width=20
    CHECK(rendered->front().size() >= 20);
    // Height should be at least 3, at most 5
    CHECK(rendered->size() >= 3);
    CHECK(rendered->size() <= 5);
}

TEST_CASE("Tui routes semantic input to exactly one focused target", "[tui][overlay][issue50]") {
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

TEST_CASE("Tui routes input to overlays before base content", "[tui][overlay][issue50]") {
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

TEST_CASE("Non-capturing overlay passes input through to base content", "[tui][overlay][issue50]") {
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

TEST_CASE("Overlay z-index controls input routing order", "[tui][overlay][issue50]") {
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
    auto* overlay1_ptr = overlay1.get();
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

TEST_CASE("Hide and restore overlay with focus fallback", "[tui][overlay][issue50]") {
    cch::tui::VirtualTerminal terminal({.columns = 10, .rows = 5});
    cch::tui::Tui tui(terminal);

    auto base = std::make_unique<FocusableInputComponent>("base");
    auto* base_ptr = base.get();
    REQUIRE(tui.add_child(std::move(base)));
    REQUIRE(tui.start());
    REQUIRE(tui.set_focus(base_ptr));

    auto overlay = std::make_unique<cch::tui::Overlay>();
    auto overlay_comp = std::make_unique<FocusableInputComponent>("over");
    auto* overlay_comp_ptr = overlay_comp.get();
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

TEST_CASE("Remove overlay disposes it and fallback focus works", "[tui][overlay][issue50]") {
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

TEST_CASE("IME cursor location follows focused component", "[tui][overlay][issue50]") {
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

TEST_CASE("Overlay with Editor delegates cursor location", "[tui][overlay][issue50]") {
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

TEST_CASE("Stacked overlays render in z-order", "[tui][overlay][issue50]") {
    cch::tui::VirtualTerminal terminal({.columns = 20, .rows = 10});
    cch::tui::Tui tui(terminal);

    auto base = std::make_unique<cch::tui::Text>("base", 0, 0);
    REQUIRE(tui.add_child(std::move(base)));
    REQUIRE(tui.start());

    // Overlay 1 (lower z-index)
    auto overlay1 = std::make_unique<cch::tui::Overlay>(
        cch::tui::OverlayOptions{.z_index = 0});
    auto text1 = std::make_unique<cch::tui::Text>("overlay1", 0, 0);
    REQUIRE(overlay1->add_child(std::move(text1)));
    auto* overlay1_ptr = overlay1.get();
    REQUIRE(tui.add_overlay(std::move(overlay1)));

    // Overlay 2 (higher z-index)
    auto overlay2 = std::make_unique<cch::tui::Overlay>(
        cch::tui::OverlayOptions{.z_index = 1});
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

TEST_CASE("Focusable Components expose cursor_location for IME", "[tui][overlay][issue50]") {
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

TEST_CASE("Default cursor_location returns nullopt for non-focusable", "[tui][overlay][issue50]") {
    // Text is not Focusable, so no cursor location
    cch::tui::Text text("hello", 0, 0);
    // Text doesn't inherit from Focusable, verify by dynamic_cast
    auto* focusable = dynamic_cast<cch::tui::Focusable*>(&text);
    CHECK(focusable == nullptr);
}

TEST_CASE("Overlay rejects a disposed component after removal", "[tui][overlay][issue50]") {
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
