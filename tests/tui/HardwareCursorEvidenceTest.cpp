// Named pi-v1.0.4 acceptance for hardware cursor preference and IME positioning (#1037).
// Every expectation is read from the verified `hardware-cursor.json` artifact;
// nothing here is derived from Pike assertions alone.

#include <cch/tui/AltScreen.hpp>
#include <cch/tui/Component.hpp>
#include <cch/tui/Container.hpp>
#include <cch/tui/ITui.hpp>
#include <cch/tui/Text.hpp>
#include <cch/tui/Tui.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include "support/PiTuiEvidence.hpp"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using cch::support::JsonValue;

class ReplayCursorChild final : public cch::tui::Component, public cch::tui::Focusable {
public:
    ReplayCursorChild(
            std::vector<std::string> lines, std::optional<cch::tui::CursorPosition> cursor = std::nullopt)
        : lines_(std::move(lines)), cursor_(cursor) {}

    [[nodiscard]] cch::support::Expected<cch::tui::RenderResult> render(std::size_t) override {
        return cch::tui::RenderResult{.lines = lines_};
    }

    void invalidate() override {}

    void set_cursor(std::optional<cch::tui::CursorPosition> cursor) { cursor_ = cursor; }

    [[nodiscard]] std::optional<cch::tui::CursorPosition> cursor_location() const override {
        return focused_ ? cursor_ : std::nullopt;
    }

    void set_focused(bool focused) override { focused_ = focused; }
    [[nodiscard]] bool focused() const override { return focused_; }

private:
    std::vector<std::string> lines_;
    std::optional<cch::tui::CursorPosition> cursor_;
    bool focused_{false};
};

[[nodiscard]] std::size_t size_of(const JsonValue& value) {
    return static_cast<std::size_t>(value.get_number());
}

[[nodiscard]] const JsonValue& find_scenario(const JsonValue& bundle, std::string_view name) {
    for (const auto& scenario : bundle.at("scenarios").get_array()) {
        if (scenario.at("name").get_string() == name) return scenario;
    }
    REQUIRE(false);
    return bundle;
}

} // namespace

TEST_CASE("frozen hardware-cursor observations replay across MainScreen and AltScreen",
        "[tui][hardware-cursor][issue1037][compat-pi][spec]") {
    const auto bundle = cch::tests::read_pi_tui_evidence("hardware-cursor.json");
    REQUIRE(bundle.has_value());
    REQUIRE(bundle->at("scenarios").get_array().size() == 13);

    SECTION("main-screen-default-hidden") {
        const auto& scenario = find_scenario(*bundle, "main-screen-default-hidden");
        const auto cols = size_of(scenario.at("dimensions").at("columns"));
        const auto rows = size_of(scenario.at("dimensions").at("rows"));
        cch::tui::VirtualTerminal terminal(cch::tui::VirtualTerminalOptions{.columns = cols, .rows = rows});
        cch::tui::MainScreen main_screen{terminal};

        auto child = std::make_unique<ReplayCursorChild>(
                std::vector<std::string>{"test text"}, cch::tui::CursorPosition{.column = 4, .row = 0});
        auto* child_pointer = child.get();
        REQUIRE(main_screen.add_child(std::move(child)));
        REQUIRE(main_screen.start());
        REQUIRE(main_screen.set_focus(child_pointer));
        REQUIRE(main_screen.render());

        const auto& step = scenario.at("expected").at("steps").get_array()[0];
        CHECK(terminal.cursor().column == size_of(step.at("cursor").at("col")));
        CHECK(terminal.cursor().row == size_of(step.at("cursor").at("row")));
        CHECK(terminal.modes().cursor_visible == step.at("cursorVisible").get_boolean());
        CHECK(main_screen.show_hardware_cursor() == step.at("showHardwareCursor").get_boolean());

        REQUIRE(main_screen.stop());
    }

    SECTION("main-screen-enabled-visible") {
        const auto& scenario = find_scenario(*bundle, "main-screen-enabled-visible");
        const auto cols = size_of(scenario.at("dimensions").at("columns"));
        const auto rows = size_of(scenario.at("dimensions").at("rows"));
        cch::tui::VirtualTerminal terminal(cch::tui::VirtualTerminalOptions{.columns = cols, .rows = rows});
        cch::tui::MainScreen main_screen{terminal, true};

        auto child = std::make_unique<ReplayCursorChild>(
                std::vector<std::string>{"test text"}, cch::tui::CursorPosition{.column = 4, .row = 0});
        auto* child_pointer = child.get();
        REQUIRE(main_screen.add_child(std::move(child)));
        REQUIRE(main_screen.start());
        REQUIRE(main_screen.set_focus(child_pointer));
        REQUIRE(main_screen.render());

        const auto& step = scenario.at("expected").at("steps").get_array()[0];
        CHECK(terminal.cursor().column == size_of(step.at("cursor").at("col")));
        CHECK(terminal.cursor().row == size_of(step.at("cursor").at("row")));
        CHECK(terminal.modes().cursor_visible == step.at("cursorVisible").get_boolean());
        CHECK(main_screen.show_hardware_cursor() == step.at("showHardwareCursor").get_boolean());

        REQUIRE(main_screen.stop());
    }

    SECTION("main-screen-disabled-explicit") {
        const auto& scenario = find_scenario(*bundle, "main-screen-disabled-explicit");
        const auto cols = size_of(scenario.at("dimensions").at("columns"));
        const auto rows = size_of(scenario.at("dimensions").at("rows"));
        cch::tui::VirtualTerminal terminal(cch::tui::VirtualTerminalOptions{.columns = cols, .rows = rows});
        cch::tui::MainScreen main_screen{terminal, false};

        auto child = std::make_unique<ReplayCursorChild>(
                std::vector<std::string>{"test text"}, cch::tui::CursorPosition{.column = 4, .row = 0});
        auto* child_pointer = child.get();
        REQUIRE(main_screen.add_child(std::move(child)));
        REQUIRE(main_screen.start());
        REQUIRE(main_screen.set_focus(child_pointer));
        REQUIRE(main_screen.render());

        const auto& step = scenario.at("expected").at("steps").get_array()[0];
        CHECK(terminal.cursor().column == size_of(step.at("cursor").at("col")));
        CHECK(terminal.cursor().row == size_of(step.at("cursor").at("row")));
        CHECK(terminal.modes().cursor_visible == step.at("cursorVisible").get_boolean());
        CHECK(main_screen.show_hardware_cursor() == step.at("showHardwareCursor").get_boolean());

        REQUIRE(main_screen.stop());
    }

    SECTION("main-screen-toggle") {
        const auto& scenario = find_scenario(*bundle, "main-screen-toggle");
        const auto cols = size_of(scenario.at("dimensions").at("columns"));
        const auto rows = size_of(scenario.at("dimensions").at("rows"));
        cch::tui::VirtualTerminal terminal(cch::tui::VirtualTerminalOptions{.columns = cols, .rows = rows});
        cch::tui::MainScreen main_screen{terminal, false};

        auto child = std::make_unique<ReplayCursorChild>(
                std::vector<std::string>{"test text"}, cch::tui::CursorPosition{.column = 4, .row = 0});
        auto* child_pointer = child.get();
        REQUIRE(main_screen.add_child(std::move(child)));
        REQUIRE(main_screen.start());
        REQUIRE(main_screen.set_focus(child_pointer));

        const auto& steps = scenario.at("expected").at("steps").get_array();

        // Step 1: default-false
        REQUIRE(main_screen.render());
        CHECK(terminal.cursor().column == size_of(steps[0].at("cursor").at("col")));
        CHECK(terminal.cursor().row == size_of(steps[0].at("cursor").at("row")));
        CHECK(terminal.modes().cursor_visible == steps[0].at("cursorVisible").get_boolean());
        CHECK(main_screen.show_hardware_cursor() == steps[0].at("showHardwareCursor").get_boolean());

        // Step 2: toggle-true
        main_screen.set_show_hardware_cursor(true);
        REQUIRE(main_screen.render());
        CHECK(terminal.cursor().column == size_of(steps[1].at("cursor").at("col")));
        CHECK(terminal.cursor().row == size_of(steps[1].at("cursor").at("row")));
        CHECK(terminal.modes().cursor_visible == steps[1].at("cursorVisible").get_boolean());
        CHECK(main_screen.show_hardware_cursor() == steps[1].at("showHardwareCursor").get_boolean());

        // Step 3: toggle-false
        main_screen.set_show_hardware_cursor(false);
        REQUIRE(main_screen.render());
        CHECK(terminal.cursor().column == size_of(steps[2].at("cursor").at("col")));
        CHECK(terminal.cursor().row == size_of(steps[2].at("cursor").at("row")));
        CHECK(terminal.modes().cursor_visible == steps[2].at("cursorVisible").get_boolean());
        CHECK(main_screen.show_hardware_cursor() == steps[2].at("showHardwareCursor").get_boolean());

        REQUIRE(main_screen.stop());
    }

    SECTION("main-screen-invalid-cursor") {
        const auto& scenario = find_scenario(*bundle, "main-screen-invalid-cursor");
        const auto cols = size_of(scenario.at("dimensions").at("columns"));
        const auto rows = size_of(scenario.at("dimensions").at("rows"));
        cch::tui::VirtualTerminal terminal(cch::tui::VirtualTerminalOptions{.columns = cols, .rows = rows});
        cch::tui::MainScreen main_screen{terminal, true};

        auto child = std::make_unique<ReplayCursorChild>(
                std::vector<std::string>{"no cursor line"}, std::nullopt);
        auto* child_pointer = child.get();
        REQUIRE(main_screen.add_child(std::move(child)));
        REQUIRE(main_screen.start());
        REQUIRE(main_screen.set_focus(child_pointer));
        REQUIRE(main_screen.render());

        const auto& step = scenario.at("expected").at("steps").get_array()[0];
        CHECK(terminal.modes().cursor_visible == step.at("cursorVisible").get_boolean());
        CHECK(main_screen.show_hardware_cursor() == step.at("showHardwareCursor").get_boolean());

        REQUIRE(main_screen.stop());
    }

    SECTION("main-screen-nested") {
        const auto& scenario = find_scenario(*bundle, "main-screen-nested");
        const auto cols = size_of(scenario.at("dimensions").at("columns"));
        const auto rows = size_of(scenario.at("dimensions").at("rows"));
        cch::tui::VirtualTerminal terminal(cch::tui::VirtualTerminalOptions{.columns = cols, .rows = rows});
        cch::tui::MainScreen main_screen{terminal, false};

        auto container = std::make_unique<cch::tui::Container>();
        REQUIRE(container->add_child(std::make_unique<cch::tui::Text>("header line", 0, 0)));
        auto box = std::make_unique<cch::tui::Box>(2, 1);
        auto child = std::make_unique<ReplayCursorChild>(
                std::vector<std::string>{"nested input"}, cch::tui::CursorPosition{.column = 3, .row = 0});
        auto* child_pointer = child.get();
        REQUIRE(box->add_child(std::move(child)));
        REQUIRE(container->add_child(std::move(box)));

        REQUIRE(main_screen.add_child(std::move(container)));
        REQUIRE(main_screen.start());
        REQUIRE(main_screen.set_focus(child_pointer));

        const auto& steps = scenario.at("expected").at("steps").get_array();

        // Step 1: preference-false
        REQUIRE(main_screen.render());
        CHECK(terminal.cursor().column == size_of(steps[0].at("cursor").at("col")));
        CHECK(terminal.cursor().row == size_of(steps[0].at("cursor").at("row")));
        CHECK(terminal.modes().cursor_visible == steps[0].at("cursorVisible").get_boolean());
        CHECK(main_screen.show_hardware_cursor() == steps[0].at("showHardwareCursor").get_boolean());

        // Step 2: preference-true
        main_screen.set_show_hardware_cursor(true);
        REQUIRE(main_screen.render());
        CHECK(terminal.cursor().column == size_of(steps[1].at("cursor").at("col")));
        CHECK(terminal.cursor().row == size_of(steps[1].at("cursor").at("row")));
        CHECK(terminal.modes().cursor_visible == steps[1].at("cursorVisible").get_boolean());
        CHECK(main_screen.show_hardware_cursor() == steps[1].at("showHardwareCursor").get_boolean());

        REQUIRE(main_screen.stop());
    }

    SECTION("alt-screen-default-hidden") {
        const auto& scenario = find_scenario(*bundle, "alt-screen-default-hidden");
        const auto cols = size_of(scenario.at("dimensions").at("columns"));
        const auto rows = size_of(scenario.at("dimensions").at("rows"));
        cch::tui::VirtualTerminal terminal(cch::tui::VirtualTerminalOptions{.columns = cols, .rows = rows});
        cch::tui::AltScreen alt_screen{terminal};

        auto child = std::make_unique<ReplayCursorChild>(
                std::vector<std::string>{"line 0", "line 1"}, cch::tui::CursorPosition{.column = 6, .row = 1});
        auto* child_pointer = child.get();
        REQUIRE(alt_screen.add_child(std::move(child)));
        REQUIRE(alt_screen.start());
        REQUIRE(alt_screen.set_focus(child_pointer));
        REQUIRE(alt_screen.render());

        const auto& step = scenario.at("expected").at("steps").get_array()[0];
        CHECK(terminal.cursor().column == size_of(step.at("cursor").at("col")));
        CHECK(terminal.cursor().row == size_of(step.at("cursor").at("row")));
        CHECK(terminal.modes().cursor_visible == step.at("cursorVisible").get_boolean());
        CHECK(alt_screen.show_hardware_cursor() == step.at("showHardwareCursor").get_boolean());

        REQUIRE(alt_screen.stop());
    }

    SECTION("alt-screen-enabled-visible") {
        const auto& scenario = find_scenario(*bundle, "alt-screen-enabled-visible");
        const auto cols = size_of(scenario.at("dimensions").at("columns"));
        const auto rows = size_of(scenario.at("dimensions").at("rows"));
        cch::tui::VirtualTerminal terminal(cch::tui::VirtualTerminalOptions{.columns = cols, .rows = rows});
        cch::tui::AltScreen alt_screen{terminal, true};

        auto child = std::make_unique<ReplayCursorChild>(
                std::vector<std::string>{"line 0", "line 1"}, cch::tui::CursorPosition{.column = 6, .row = 1});
        auto* child_pointer = child.get();
        REQUIRE(alt_screen.add_child(std::move(child)));
        REQUIRE(alt_screen.start());
        REQUIRE(alt_screen.set_focus(child_pointer));
        REQUIRE(alt_screen.render());

        const auto& step = scenario.at("expected").at("steps").get_array()[0];
        CHECK(terminal.cursor().column == size_of(step.at("cursor").at("col")));
        CHECK(terminal.cursor().row == size_of(step.at("cursor").at("row")));
        CHECK(terminal.modes().cursor_visible == step.at("cursorVisible").get_boolean());
        CHECK(alt_screen.show_hardware_cursor() == step.at("showHardwareCursor").get_boolean());

        REQUIRE(alt_screen.stop());
    }

    SECTION("alt-screen-disabled-explicit") {
        const auto& scenario = find_scenario(*bundle, "alt-screen-disabled-explicit");
        const auto cols = size_of(scenario.at("dimensions").at("columns"));
        const auto rows = size_of(scenario.at("dimensions").at("rows"));
        cch::tui::VirtualTerminal terminal(cch::tui::VirtualTerminalOptions{.columns = cols, .rows = rows});
        cch::tui::AltScreen alt_screen{terminal, false};

        auto child = std::make_unique<ReplayCursorChild>(
                std::vector<std::string>{"line 0", "line 1"}, cch::tui::CursorPosition{.column = 6, .row = 1});
        auto* child_pointer = child.get();
        REQUIRE(alt_screen.add_child(std::move(child)));
        REQUIRE(alt_screen.start());
        REQUIRE(alt_screen.set_focus(child_pointer));
        REQUIRE(alt_screen.render());

        const auto& step = scenario.at("expected").at("steps").get_array()[0];
        CHECK(terminal.cursor().column == size_of(step.at("cursor").at("col")));
        CHECK(terminal.cursor().row == size_of(step.at("cursor").at("row")));
        CHECK(terminal.modes().cursor_visible == step.at("cursorVisible").get_boolean());
        CHECK(alt_screen.show_hardware_cursor() == step.at("showHardwareCursor").get_boolean());

        REQUIRE(alt_screen.stop());
    }

    SECTION("alt-screen-toggle") {
        const auto& scenario = find_scenario(*bundle, "alt-screen-toggle");
        const auto cols = size_of(scenario.at("dimensions").at("columns"));
        const auto rows = size_of(scenario.at("dimensions").at("rows"));
        cch::tui::VirtualTerminal terminal(cch::tui::VirtualTerminalOptions{.columns = cols, .rows = rows});
        cch::tui::AltScreen alt_screen{terminal, false};

        auto child = std::make_unique<ReplayCursorChild>(
                std::vector<std::string>{"line 0", "line 1"}, cch::tui::CursorPosition{.column = 6, .row = 1});
        auto* child_pointer = child.get();
        REQUIRE(alt_screen.add_child(std::move(child)));
        REQUIRE(alt_screen.start());
        REQUIRE(alt_screen.set_focus(child_pointer));

        const auto& steps = scenario.at("expected").at("steps").get_array();

        // Step 1: default-false
        REQUIRE(alt_screen.render());
        CHECK(terminal.cursor().column == size_of(steps[0].at("cursor").at("col")));
        CHECK(terminal.cursor().row == size_of(steps[0].at("cursor").at("row")));
        CHECK(terminal.modes().cursor_visible == steps[0].at("cursorVisible").get_boolean());
        CHECK(alt_screen.show_hardware_cursor() == steps[0].at("showHardwareCursor").get_boolean());

        // Step 2: toggle-true
        alt_screen.set_show_hardware_cursor(true);
        REQUIRE(alt_screen.render());
        CHECK(terminal.cursor().column == size_of(steps[1].at("cursor").at("col")));
        CHECK(terminal.cursor().row == size_of(steps[1].at("cursor").at("row")));
        CHECK(terminal.modes().cursor_visible == steps[1].at("cursorVisible").get_boolean());
        CHECK(alt_screen.show_hardware_cursor() == steps[1].at("showHardwareCursor").get_boolean());

        // Step 3: toggle-false
        alt_screen.set_show_hardware_cursor(false);
        REQUIRE(alt_screen.render());
        CHECK(terminal.cursor().column == size_of(steps[2].at("cursor").at("col")));
        CHECK(terminal.cursor().row == size_of(steps[2].at("cursor").at("row")));
        CHECK(terminal.modes().cursor_visible == steps[2].at("cursorVisible").get_boolean());
        CHECK(alt_screen.show_hardware_cursor() == steps[2].at("showHardwareCursor").get_boolean());

        REQUIRE(alt_screen.stop());
    }

    SECTION("alt-screen-invalid-cursor") {
        const auto& scenario = find_scenario(*bundle, "alt-screen-invalid-cursor");
        const auto cols = size_of(scenario.at("dimensions").at("columns"));
        const auto rows = size_of(scenario.at("dimensions").at("rows"));
        cch::tui::VirtualTerminal terminal(cch::tui::VirtualTerminalOptions{.columns = cols, .rows = rows});
        cch::tui::AltScreen alt_screen{terminal, true};

        auto child = std::make_unique<ReplayCursorChild>(
                std::vector<std::string>{"no cursor line"}, std::nullopt);
        auto* child_pointer = child.get();
        REQUIRE(alt_screen.add_child(std::move(child)));
        REQUIRE(alt_screen.start());
        REQUIRE(alt_screen.set_focus(child_pointer));
        REQUIRE(alt_screen.render());

        const auto& step = scenario.at("expected").at("steps").get_array()[0];
        CHECK(terminal.modes().cursor_visible == step.at("cursorVisible").get_boolean());
        CHECK(alt_screen.show_hardware_cursor() == step.at("showHardwareCursor").get_boolean());

        REQUIRE(alt_screen.stop());
    }

    SECTION("alt-screen-nested") {
        const auto& scenario = find_scenario(*bundle, "alt-screen-nested");
        const auto cols = size_of(scenario.at("dimensions").at("columns"));
        const auto rows = size_of(scenario.at("dimensions").at("rows"));
        cch::tui::VirtualTerminal terminal(cch::tui::VirtualTerminalOptions{.columns = cols, .rows = rows});
        cch::tui::AltScreen alt_screen{terminal, false};

        auto container = std::make_unique<cch::tui::Container>();
        REQUIRE(container->add_child(std::make_unique<cch::tui::Text>("first row", 0, 0)));
        auto box = std::make_unique<cch::tui::Box>(3, 1);
        auto child = std::make_unique<ReplayCursorChild>(
                std::vector<std::string>{"alt nested"}, cch::tui::CursorPosition{.column = 4, .row = 0});
        auto* child_pointer = child.get();
        REQUIRE(box->add_child(std::move(child)));
        REQUIRE(container->add_child(std::move(box)));

        REQUIRE(alt_screen.add_child(std::move(container)));
        REQUIRE(alt_screen.start());
        REQUIRE(alt_screen.set_focus(child_pointer));

        const auto& steps = scenario.at("expected").at("steps").get_array();

        // Step 1: preference-false
        REQUIRE(alt_screen.render());
        CHECK(terminal.cursor().column == size_of(steps[0].at("cursor").at("col")));
        CHECK(terminal.cursor().row == size_of(steps[0].at("cursor").at("row")));
        CHECK(terminal.modes().cursor_visible == steps[0].at("cursorVisible").get_boolean());
        CHECK(alt_screen.show_hardware_cursor() == steps[0].at("showHardwareCursor").get_boolean());

        // Step 2: preference-true
        alt_screen.set_show_hardware_cursor(true);
        REQUIRE(alt_screen.render());
        CHECK(terminal.cursor().column == size_of(steps[1].at("cursor").at("col")));
        CHECK(terminal.cursor().row == size_of(steps[1].at("cursor").at("row")));
        CHECK(terminal.modes().cursor_visible == steps[1].at("cursorVisible").get_boolean());
        CHECK(alt_screen.show_hardware_cursor() == steps[1].at("showHardwareCursor").get_boolean());

        REQUIRE(alt_screen.stop());
    }

    SECTION("alt-screen-clipped") {
        const auto& scenario = find_scenario(*bundle, "alt-screen-clipped");
        const auto cols = size_of(scenario.at("dimensions").at("columns"));
        const auto rows = size_of(scenario.at("dimensions").at("rows"));
        cch::tui::VirtualTerminal terminal(cch::tui::VirtualTerminalOptions{.columns = cols, .rows = rows});
        cch::tui::AltScreen alt_screen{terminal, true};

        std::vector<std::string> lines;
        for (int i = 0; i < 8; ++i)
            lines.push_back(std::format("line {}", i));
        auto child = std::make_unique<ReplayCursorChild>(
                lines, cch::tui::CursorPosition{.column = 5, .row = 2});
        auto* child_pointer = child.get();
        REQUIRE(alt_screen.add_child(std::move(child)));
        REQUIRE(alt_screen.start());
        REQUIRE(alt_screen.set_focus(child_pointer));

        const auto& steps = scenario.at("expected").at("steps").get_array();

        // Step 1: following-end
        REQUIRE(alt_screen.render());
        CHECK(terminal.modes().cursor_visible == steps[0].at("cursorVisible").get_boolean());

        // Step 2: scroll-to-top
        alt_screen.scroll_to_top();
        REQUIRE(alt_screen.render());
        CHECK(terminal.cursor().column == size_of(steps[1].at("cursor").at("col")));
        CHECK(terminal.cursor().row == size_of(steps[1].at("cursor").at("row")));
        CHECK(terminal.modes().cursor_visible == steps[1].at("cursorVisible").get_boolean());

        // Step 3: scroll-by-1
        alt_screen.scroll_by(1);
        REQUIRE(alt_screen.render());
        CHECK(terminal.cursor().column == size_of(steps[2].at("cursor").at("col")));
        CHECK(terminal.cursor().row == size_of(steps[2].at("cursor").at("row")));
        CHECK(terminal.modes().cursor_visible == steps[2].at("cursorVisible").get_boolean());

        // Step 4: scroll-to-bottom
        alt_screen.scroll_to_bottom();
        REQUIRE(alt_screen.render());
        CHECK(terminal.modes().cursor_visible == steps[3].at("cursorVisible").get_boolean());

        REQUIRE(alt_screen.stop());
    }
}
