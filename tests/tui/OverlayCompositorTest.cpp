#include "tui/OverlayCompositor.hpp"

#include <cch/tui/Overlay.hpp>
#include <cch/tui/Utils.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace cch;

namespace {

class StaticComponent final : public tui::Component {
public:
    explicit StaticComponent(
        std::vector<std::string> lines,
        std::vector<tui::InlineImageRenderRegion> images = {})
        : lines_(std::move(lines)), images_(std::move(images)) {}

    [[nodiscard]] support::Expected<tui::RenderResult> render(std::size_t) override {
        return tui::RenderResult{.lines = lines_, .images = images_};
    }

    void invalidate() override {
        ++invalidation_count;
    }

    std::size_t invalidation_count{0};

private:
    std::vector<std::string> lines_;
    std::vector<tui::InlineImageRenderRegion> images_;
};

class FocusableInputComponent final
    : public tui::Component,
      public tui::InputHandler,
      public tui::Focusable {
public:
    [[nodiscard]] support::Expected<tui::RenderResult> render(std::size_t) override {
        return tui::RenderResult{};
    }

    void invalidate() override {}

    void handle_input(const tui::InputEventVariant& input) override {
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

    std::vector<tui::InputEventVariant> received_input;
    bool accept_key_releases{false};

private:
    bool focused_{false};
};

[[nodiscard]] tui::TerminalCapabilities kitty_capabilities() {
    tui::TerminalCapabilities capabilities;
    capabilities.inline_images = tui::InlineImageProtocol::Kitty;
    capabilities.cell_pixels = tui::CellPixelDimensions{.width = 9, .height = 18};
    return capabilities;
}

[[nodiscard]] tui::InlineImageRenderRegion single_cell_image(
    std::uint64_t resource_id,
    std::size_t column,
    std::size_t row) {
    return tui::InlineImageRenderRegion{
        .resource_id = resource_id,
        .encoded_data = "AAAA",
        .mime_type = "image/png",
        .pixel_width = 9,
        .pixel_height = 18,
        .max_width = 1,
        .max_height = 1,
        .region = tui::CellRegion{.column = column, .row = row, .columns = 1, .rows = 1},
    };
}

[[nodiscard]] tui::OverlayOptions absolute_options(
    std::size_t column,
    std::size_t row,
    std::size_t z_index = 0) {
    tui::OverlayOptions options;
    options.position = tui::OverlayPosition::Absolute;
    options.absolute_column = column;
    options.absolute_row = row;
    options.z_index = z_index;
    return options;
}

} // namespace

TEST_CASE("OverlayCompositor validates overlay lifetime operations", "[tui][overlay]") {
    tui::detail::OverlayCompositor compositor;
    tui::Overlay stranger;

    CHECK_FALSE(compositor.add_overlay(nullptr));
    CHECK(compositor.remove(nullptr));
    CHECK_FALSE(compositor.remove(&stranger));
    CHECK_FALSE(compositor.owns(&stranger));

    auto overlay = std::make_unique<tui::Overlay>();
    auto* overlay_ptr = overlay.get();
    REQUIRE(compositor.add_overlay(std::move(overlay)));
    CHECK(compositor.owns(overlay_ptr));

    REQUIRE(compositor.remove(overlay_ptr));
    CHECK_FALSE(compositor.owns(overlay_ptr));
    CHECK_FALSE(compositor.remove(overlay_ptr));
}

TEST_CASE("OverlayCompositor invalidates every owned overlay", "[tui][overlay]") {
    tui::detail::OverlayCompositor compositor;
    auto overlay = std::make_unique<tui::Overlay>();
    auto child = std::make_unique<StaticComponent>(std::vector<std::string>{"x"});
    auto* child_ptr = child.get();
    REQUIRE(overlay->add_child(std::move(child)));
    REQUIRE(compositor.add_overlay(std::move(overlay)));

    compositor.invalidate_all();
    CHECK(child_ptr->invalidation_count == 1);
}

TEST_CASE("OverlayCompositor splices overlay content at its viewport position", "[tui][overlay]") {
    tui::detail::OverlayCompositor compositor;
    const tui::TerminalDimensions dimensions{.columns = 10, .rows = 4};

    auto options = absolute_options(4, 1);
    options.size_constraints.max_width = 2;
    auto overlay = std::make_unique<tui::Overlay>(options);
    REQUIRE(overlay->add_child(std::make_unique<StaticComponent>(std::vector<std::string>{"XX"})));
    REQUIRE(compositor.add_overlay(std::move(overlay)));

    auto offscreen = std::make_unique<tui::Overlay>(absolute_options(12, 0));
    REQUIRE(offscreen->add_child(std::make_unique<StaticComponent>(std::vector<std::string>{"YY"})));
    REQUIRE(compositor.add_overlay(std::move(offscreen)));

    tui::RenderResult output{.lines = std::vector<std::string>(4, "aaaaaaaaaa")};
    REQUIRE(compositor.composite(dimensions, tui::TerminalCapabilities{}, output));

    CHECK(output.lines[0] == "aaaaaaaaaa");
    CHECK(output.lines[1] == "aaaa\x1b[0mXXaaaa");
    CHECK(output.lines[2] == "aaaaaaaaaa");
    for (const auto& line : output.lines) {
        CHECK(line.find("YY") == std::string::npos);
    }
}

TEST_CASE("OverlayCompositor draws higher z-index overlays on top", "[tui][overlay]") {
    tui::detail::OverlayCompositor compositor;
    const tui::TerminalDimensions dimensions{.columns = 10, .rows = 1};

    auto low_options = absolute_options(0, 0, 1);
    low_options.size_constraints.max_width = 4;
    auto low = std::make_unique<tui::Overlay>(low_options);
    REQUIRE(low->add_child(std::make_unique<StaticComponent>(std::vector<std::string>{"aaaa"})));
    REQUIRE(compositor.add_overlay(std::move(low)));

    auto high_options = absolute_options(0, 0, 2);
    high_options.size_constraints.max_width = 2;
    auto high = std::make_unique<tui::Overlay>(high_options);
    REQUIRE(high->add_child(std::make_unique<StaticComponent>(std::vector<std::string>{"bb"})));
    REQUIRE(compositor.add_overlay(std::move(high)));

    tui::RenderResult output{.lines = std::vector<std::string>{"----------"}};
    REQUIRE(compositor.composite(dimensions, tui::TerminalCapabilities{}, output));
    CHECK(output.lines[0] == "bbaa------");
}

TEST_CASE("OverlayCompositor preserves ANSI styles when splicing line regions", "[tui][overlay]") {
    tui::detail::OverlayCompositor compositor;
    const tui::TerminalDimensions dimensions{.columns = 10, .rows = 1};

    auto options = absolute_options(1, 0);
    options.size_constraints.max_width = 1;
    auto overlay = std::make_unique<tui::Overlay>(options);
    REQUIRE(overlay->add_child(std::make_unique<StaticComponent>(std::vector<std::string>{"X"})));
    REQUIRE(compositor.add_overlay(std::move(overlay)));

    tui::RenderResult output{.lines = std::vector<std::string>{"\x1b[31mRED!"}};
    REQUIRE(compositor.composite(dimensions, tui::TerminalCapabilities{}, output));

    // The prefix keeps its style and reset, and the suffix replays the active
    // red style before "D!" and closes it at the line end.
    CHECK(output.lines[0].starts_with("\x1b[31mR\x1b[0mX\x1b[31mD!\x1b[0m"));
    CHECK(tui::visible_width(output.lines[0]) == 10);
}

TEST_CASE("OverlayCompositor clips base images intersecting overlaid regions", "[tui][overlay]") {
    tui::detail::OverlayCompositor compositor;
    const tui::TerminalDimensions dimensions{.columns = 10, .rows = 4};
    const auto capabilities = kitty_capabilities();

    tui::RenderResult output{
        .lines = std::vector<std::string>(4, "aaaaaaaaaa"),
        .images = {single_cell_image(1, 0, 0), single_cell_image(2, 0, 3)},
    };
    output = tui::detail::OverlayCompositor::materialize_images(
        std::move(output), capabilities, dimensions.columns, 100);
    REQUIRE(output.images.size() == 2);

    auto options = absolute_options(0, 0);
    options.size_constraints.max_width = 1;
    auto overlay = std::make_unique<tui::Overlay>(options);
    REQUIRE(overlay->add_child(std::make_unique<StaticComponent>(std::vector<std::string>{"X"})));
    REQUIRE(compositor.add_overlay(std::move(overlay)));

    REQUIRE(compositor.composite(dimensions, capabilities, output));

    CHECK(output.lines[0].starts_with("X"));
    REQUIRE(output.images.size() == 1);
    CHECK(output.images[0].resource_id == 2);
}

TEST_CASE("OverlayCompositor offsets overlay images into the composed buffer", "[tui][overlay]") {
    tui::detail::OverlayCompositor compositor;
    const tui::TerminalDimensions dimensions{.columns = 10, .rows = 4};
    const auto capabilities = kitty_capabilities();

    auto overlay = std::make_unique<tui::Overlay>(absolute_options(5, 2));
    REQUIRE(overlay->add_child(std::make_unique<StaticComponent>(
        std::vector<std::string>{"X"},
        std::vector{single_cell_image(3, 0, 0)})));
    REQUIRE(compositor.add_overlay(std::move(overlay)));

    tui::RenderResult output{.lines = std::vector<std::string>(4, "aaaaaaaaaa")};
    REQUIRE(compositor.composite(dimensions, capabilities, output));

    // The image's backing line is spliced as blank real estate and the image
    // region is offset by the overlay's viewport position.
    CHECK(output.lines[2] == "aaaaa\x1b[0m aaaa");
    REQUIRE(output.images.size() == 1);
    CHECK(output.images[0].resource_id == 3);
    CHECK(output.images[0].region == tui::CellRegion{.column = 5, .row = 2, .columns = 1, .rows = 1});
}

TEST_CASE("OverlayCompositor substitutes fallback text for clipped overlay images", "[tui][overlay]") {
    tui::detail::OverlayCompositor compositor;
    const tui::TerminalDimensions dimensions{.columns = 10, .rows = 4};
    const auto capabilities = kitty_capabilities();

    auto overlay = std::make_unique<tui::Overlay>(absolute_options(9, 0));
    auto image = single_cell_image(4, 1, 0);
    image.fallback_text = "F";
    REQUIRE(overlay->add_child(std::make_unique<StaticComponent>(
        std::vector<std::string>{"X"},
        std::vector{std::move(image)})));
    REQUIRE(compositor.add_overlay(std::move(overlay)));

    tui::RenderResult output{.lines = std::vector<std::string>(4, "aaaaaaaaaa")};
    REQUIRE(compositor.composite(dimensions, capabilities, output));

    CHECK(output.lines[0] == "aaaaaaaaa\x1b[0mF");
    CHECK(output.images.empty());
}

TEST_CASE("OverlayCompositor routes input to the topmost capturing overlay", "[tui][overlay]") {
    tui::detail::OverlayCompositor compositor;
    const tui::TerminalDimensions dimensions{.columns = 10, .rows = 4};

    auto low = std::make_unique<tui::Overlay>(absolute_options(0, 0, 1));
    auto low_child = std::make_unique<FocusableInputComponent>();
    auto* low_child_ptr = low_child.get();
    REQUIRE(low->add_child(std::move(low_child)));
    REQUIRE(low->focus_first());
    auto* low_ptr = low.get();
    REQUIRE(compositor.add_overlay(std::move(low)));

    auto high = std::make_unique<tui::Overlay>(absolute_options(0, 0, 2));
    auto high_child = std::make_unique<FocusableInputComponent>();
    auto* high_child_ptr = high_child.get();
    REQUIRE(high->add_child(std::move(high_child)));
    REQUIRE(high->focus_first());
    auto* high_ptr = high.get();
    REQUIRE(compositor.add_overlay(std::move(high)));

    CHECK(compositor.dispatch_input(tui::KeyEvent{.key = "q"}, dimensions));
    CHECK(high_child_ptr->received_input.size() == 1);
    CHECK(low_child_ptr->received_input.empty());

    // Hiding the top overlay routes input to the next visible one.
    high_ptr->set_visible(false);
    CHECK(compositor.dispatch_input(tui::KeyEvent{.key = "w"}, dimensions));
    CHECK(low_child_ptr->received_input.size() == 1);

    // Key releases are skipped when the focused child declines them.
    CHECK_FALSE(compositor.dispatch_input(
        tui::KeyEvent{.key = "w", .type = tui::KeyEventType::Release},
        dimensions));
    CHECK(low_child_ptr->received_input.size() == 1);

    // A non-capturing overlay never consumes input.
    REQUIRE(compositor.remove(low_ptr));
    auto passive_options = absolute_options(0, 0, 3);
    passive_options.non_capturing = true;
    auto passive = std::make_unique<tui::Overlay>(passive_options);
    REQUIRE(passive->add_child(std::make_unique<FocusableInputComponent>()));
    REQUIRE(compositor.add_overlay(std::move(passive)));
    high_ptr->set_visible(true);
    CHECK(compositor.dispatch_input(tui::KeyEvent{.key = "e"}, dimensions));
    CHECK(high_child_ptr->received_input.size() == 2);
}

TEST_CASE("OverlayCompositor tracks overlay focus history and restoration targets", "[tui][overlay]") {
    tui::detail::OverlayCompositor compositor;

    FocusableInputComponent base_one;
    FocusableInputComponent base_two;

    auto overlay_one = std::make_unique<tui::Overlay>();
    auto* overlay_one_ptr = overlay_one.get();
    auto overlay_two = std::make_unique<tui::Overlay>();
    auto* overlay_two_ptr = overlay_two.get();
    REQUIRE(compositor.add_overlay(std::move(overlay_one)));
    REQUIRE(compositor.add_overlay(std::move(overlay_two)));

    CHECK(compositor.return_focus(overlay_one_ptr) == nullptr);

    compositor.remember_focus(overlay_one_ptr, &base_one);
    CHECK(compositor.return_focus(overlay_one_ptr) == static_cast<tui::Component*>(&base_one));

    compositor.remember_focus(overlay_two_ptr, overlay_one_ptr);
    CHECK(compositor.return_focus(overlay_two_ptr) == static_cast<tui::Component*>(overlay_one_ptr));

    // Re-remembering an overlay updates its recorded return target.
    compositor.remember_focus(overlay_two_ptr, &base_two);
    CHECK(compositor.return_focus(overlay_two_ptr) == static_cast<tui::Component*>(&base_two));

    // Forgetting an overlay re-points entries that returned focus to it.
    compositor.remember_focus(overlay_two_ptr, overlay_one_ptr);
    compositor.forget_focus(overlay_one_ptr, &base_one);
    CHECK(compositor.return_focus(overlay_one_ptr) == nullptr);
    CHECK(compositor.return_focus(overlay_two_ptr) == static_cast<tui::Component*>(&base_one));

    compositor.clear_focus_history();
    CHECK(compositor.return_focus(overlay_two_ptr) == nullptr);
}

TEST_CASE("OverlayCompositor reports focus target availability and topmost focusable", "[tui][overlay]") {
    tui::detail::OverlayCompositor compositor;
    const tui::TerminalDimensions dimensions{.columns = 10, .rows = 4};

    auto low = std::make_unique<tui::Overlay>(absolute_options(0, 0, 1));
    auto* low_ptr = low.get();
    REQUIRE(compositor.add_overlay(std::move(low)));
    auto high = std::make_unique<tui::Overlay>(absolute_options(0, 0, 2));
    auto* high_ptr = high.get();
    REQUIRE(compositor.add_overlay(std::move(high)));
    tui::Overlay stranger;

    CHECK(compositor.topmost_focusable(dimensions) == high_ptr);
    CHECK(compositor.focus_target_available(low_ptr, dimensions));
    CHECK(compositor.focus_target_available(high_ptr, dimensions));
    CHECK_FALSE(compositor.focus_target_available(nullptr, dimensions));
    CHECK_FALSE(compositor.focus_target_available(&stranger, dimensions));

    // Non-capturing overlays are not focus targets.
    auto passive_options = high_ptr->options();
    passive_options.non_capturing = true;
    high_ptr->set_options(passive_options);
    CHECK(compositor.topmost_focusable(dimensions) == low_ptr);
    CHECK_FALSE(compositor.focus_target_available(high_ptr, dimensions));

    // Hidden overlays are not focus targets either.
    low_ptr->set_visible(false);
    CHECK(compositor.topmost_focusable(dimensions) == nullptr);
    CHECK_FALSE(compositor.focus_target_available(low_ptr, dimensions));
}
