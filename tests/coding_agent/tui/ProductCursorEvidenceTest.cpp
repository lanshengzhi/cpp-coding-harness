// Named pi-v1.0.4 acceptance for product hardware cursor settings and live switch (#1037).
// Every expectation is read from the verified `product-cursor.json` artifact;
// nothing here is derived from Pike assertions alone.

#include "support/EnvVarGuard.hpp"
#include "support/Json.hpp"
#include "support/PiTuiEvidence.hpp"
#include "support/ProtocolRecordingTerminal.hpp"
#include "support/TempWorkspace.hpp"

#include <cch/coding_agent/Settings.hpp>
#include <cch/tui/AltScreen.hpp>
#include <cch/tui/Component.hpp>
#include <cch/tui/Container.hpp>
#include <cch/tui/ITui.hpp>
#include <cch/tui/Tui.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using cch::support::JsonValue;

class ReplayCursorChild final : public cch::tui::Component, public cch::tui::Focusable {
public:
    ReplayCursorChild(std::vector<std::string> lines, std::optional<cch::tui::CursorPosition> cursor = std::nullopt)
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

[[nodiscard]] std::size_t size_of(const JsonValue& value) { return static_cast<std::size_t>(value.get_number()); }

[[nodiscard]] const JsonValue& find_scenario(const JsonValue& bundle, std::string_view name) {
    for (const auto& scenario : bundle.at("scenarios").get_array()) {
        if (scenario.at("name").get_string() == name) return scenario;
    }
    REQUIRE(false);
    return bundle;
}

[[nodiscard]] std::vector<std::string> strings_of(const JsonValue& value) {
    std::vector<std::string> strings;
    for (const auto& entry : value.get_array())
        strings.emplace_back(entry.get_string());
    return strings;
}

[[nodiscard]] std::vector<std::string> visible_rows(std::vector<std::string> rows) {
    for (auto& row : rows) {
        while (!row.empty() && row.back() == ' ')
            row.pop_back();
    }
    return rows;
}

} // namespace

TEST_CASE("frozen product-cursor settings resolution replays against SettingsManager",
        "[coding_agent][settings][cursor][issue1037][spec]") {
    const auto artifact = cch::tests::read_pi_tui_evidence("product-cursor.json");
    REQUIRE(artifact);
    const auto& bundle = *artifact;
    REQUIRE(bundle.at("baseline").get_string() == "pi-v1.0.4");
    REQUIRE(bundle.at("revision").get_string() == "7c10bd4337495ee613f2224843ecdf349b80d1df");

    const auto& scenario = find_scenario(bundle, "product-settings-resolution");
    const auto& results = scenario.at("expected").at("settingsResults");

    cch::tests::EnvVarGuard env_guard{"PI_HARDWARE_CURSOR"};

    auto test_resolution = [&](const std::string& field_name,
                                   std::optional<std::string> global_json,
                                   std::optional<std::string> project_json,
                                   bool trusted,
                                   std::optional<std::string> env_val) {
        if (env_val)
            env_guard.set(*env_val);
        else
            env_guard.unset();

        cch::tests::TempWorkspace global_ws;
        cch::tests::TempWorkspace project_ws;
        if (global_json) global_ws.write("settings.json", *global_json);
        if (project_json) project_ws.write(".pi/settings.json", *project_json);

        auto manager = cch::coding_agent::SettingsManager::create(project_ws.path(), global_ws.path(), trusted);

        const bool expected = results.at(field_name).get_boolean();
        INFO("Field name: " << field_name);
        CHECK(manager.get_show_hardware_cursor() == expected);
    };

    test_resolution("defaultUnset", std::nullopt, std::nullopt, false, std::nullopt);
    test_resolution("envFallbackActive", std::nullopt, std::nullopt, false, "1");
    test_resolution("envFallbackInactiveZero", std::nullopt, std::nullopt, false, "0");
    test_resolution("envFallbackInactiveTrue", std::nullopt, std::nullopt, false, "true");
    test_resolution("explicitFalseOverridesEnv", R"({"showHardwareCursor": false})", std::nullopt, false, "1");
    test_resolution("explicitTrue", R"({"showHardwareCursor": true})", std::nullopt, false, std::nullopt);
    test_resolution("projectScopeTrustedWins",
            R"({"showHardwareCursor": false})",
            R"({"showHardwareCursor": true})",
            true,
            std::nullopt);
    test_resolution("projectScopeUntrustedIgnored",
            R"({"showHardwareCursor": false})",
            R"({"showHardwareCursor": true})",
            false,
            std::nullopt);
}

TEST_CASE("frozen product-cursor live switch scenarios replay across MainScreen and AltScreen",
        "[coding_agent][tui][cursor][renderer-switch][issue1037][spec]") {
    const auto artifact = cch::tests::read_pi_tui_evidence("product-cursor.json");
    REQUIRE(artifact);
    const auto& bundle = *artifact;
    REQUIRE(bundle.at("baseline").get_string() == "pi-v1.0.4");
    REQUIRE(bundle.at("revision").get_string() == "7c10bd4337495ee613f2224843ecdf349b80d1df");

    const std::vector<std::string> scenario_names = {
            "switch-regular-to-fullscreen-live-true",
            "switch-fullscreen-to-regular-live-false",
            "switch-regular-to-fullscreen-live-false",
            "switch-fullscreen-to-regular-live-true",
            "switch-null-cursor-hides",
            "switch-nested-box-padding",
    };

    for (const auto& name : scenario_names) {
        DYNAMIC_SECTION("Replay scenario: " << name) {
            const auto& scenario = find_scenario(bundle, name);
            const auto cols = size_of(scenario.at("dimensions").at("columns"));
            const auto rows = size_of(scenario.at("dimensions").at("rows"));
            const auto& inputs = scenario.at("inputs");
            const auto& expected = scenario.at("expected");

            const std::string initial_mode = inputs.at("initialMode").get_string();
            const std::string target_mode = inputs.at("targetMode").get_string();
            const bool initial_pref = inputs.at("initialPreference").get_boolean();
            const bool live_pref = inputs.at("livePreference").get_boolean();
            const auto lines = strings_of(inputs.at("lines"));

            std::optional<cch::tui::CursorPosition> cursor_pos;
            if (!inputs.at("cursorCol").holds<std::nullptr_t>() && !inputs.at("cursorRow").holds<std::nullptr_t>()) {
                cursor_pos = cch::tui::CursorPosition{
                        .column = size_of(inputs.at("cursorCol")),
                        .row = size_of(inputs.at("cursorRow")),
                };
            }

            const auto pad_x = size_of(inputs.at("paddingX"));
            const auto pad_y = size_of(inputs.at("paddingY"));

            cch::tui::VirtualTerminal vt({.columns = cols, .rows = rows});
            cch::tests::ProtocolRecordingTerminal terminal(vt);

            // Step 1: Build initial root
            std::unique_ptr<cch::tui::ITui> initial_root;
            if (initial_mode == "regular") {
                initial_root = std::make_unique<cch::tui::MainScreen>(terminal, initial_pref);
            } else {
                initial_root = std::make_unique<cch::tui::AltScreen>(terminal, initial_pref);
            }

            auto child = std::make_unique<ReplayCursorChild>(lines, cursor_pos);
            auto* child_ptr = child.get();

            std::unique_ptr<cch::tui::Component> mounted;
            if (pad_x > 0 || pad_y > 0) {
                auto box = std::make_unique<cch::tui::Box>(pad_x, pad_y);
                REQUIRE(box->add_child(std::move(child)));
                mounted = std::move(box);
            } else {
                mounted = std::move(child);
            }

            REQUIRE(initial_root->add_child(std::move(mounted)));
            REQUIRE(initial_root->set_focus(child_ptr));
            REQUIRE(initial_root->start());
            REQUIRE(initial_root->render());

            // Step 2: Live preference update
            initial_root->set_show_hardware_cursor(live_pref);
            REQUIRE(initial_root->render());

            // Step 3: Switch TUI mode transferring live preference
            auto* const focus = initial_root->focus();
            const bool transferred_pref = initial_root->show_hardware_cursor();
            CHECK(transferred_pref == live_pref);

            auto children = initial_root->take_children();
            REQUIRE(initial_root->stop(cch::tui::TuiStopOptions{.preserve_screen = true}));
            REQUIRE(initial_root->set_focus(nullptr));

            std::unique_ptr<cch::tui::ITui> target_root;
            if (target_mode == "fullscreen") {
                target_root = std::make_unique<cch::tui::AltScreen>(terminal, transferred_pref);
            } else {
                target_root = std::make_unique<cch::tui::MainScreen>(terminal, transferred_pref);
            }

            terminal.reset_recording();
            for (auto& ch : children) {
                REQUIRE(target_root->add_child(std::move(ch)));
            }
            REQUIRE(target_root->set_focus(focus));
            REQUIRE(target_root->start());
            REQUIRE(target_root->render());

            // Step 4: Verify against exact frozen expectations
            CHECK(target_root->show_hardware_cursor() == expected.at("showHardwareCursor").get_boolean());
            CHECK(vt.modes().cursor_visible == expected.at("cursorVisible").get_boolean());

            const auto& expected_cursor = expected.at("cursor");
            CHECK(vt.cursor().column == size_of(expected_cursor.at("col")));
            CHECK(vt.cursor().row == size_of(expected_cursor.at("row")));

            const auto expected_screen = visible_rows(strings_of(expected.at("screen")));
            CHECK(visible_rows(vt.screen()) == expected_screen);

            // Protocol assertions:
            // 1. Verify movement and visibility order: cursor positioning must precede final visibility update
            std::size_t last_movement_or_cursor_op = std::string::npos;
            std::size_t last_visibility_op = std::string::npos;
            bool last_visible_state = false;

            const auto& ops = terminal.ops();
            for (std::size_t i = 0; i < ops.size(); ++i) {
                const auto& op = ops[i];
                if (op.kind == cch::tests::ProtocolRecordingTerminal::OpKind::Write) {
                    if (cch::tests::is_cursor_movement_sequence(op.write_bytes)) {
                        last_movement_or_cursor_op = i;
                    }
                } else if (op.kind == cch::tests::ProtocolRecordingTerminal::OpKind::SetCursor) {
                    last_movement_or_cursor_op = i;
                } else if (op.kind == cch::tests::ProtocolRecordingTerminal::OpKind::SetCursorVisible) {
                    last_visibility_op = i;
                    last_visible_state = op.visible;
                }
            }

            if (expected.at("cursorVisible").get_boolean()) {
                REQUIRE(last_visibility_op != std::string::npos);
                CHECK(last_visible_state == true);
                if (last_movement_or_cursor_op != std::string::npos) {
                    CHECK(last_movement_or_cursor_op <= last_visibility_op);
                }
            } else {
                if (last_visibility_op != std::string::npos) {
                    CHECK(last_visible_state == false);
                }
            }

            // 2. Check for matching movement bytes from frozen artifact
            for (const auto& ev : expected.at("terminalEvents").get_array()) {
                if (ev.at("op").get_string() == "write") {
                    const auto bytes = ev.at("bytes").get_string();
                    if (cch::tests::is_cursor_movement_sequence(bytes)) {
                        bool found = false;
                        for (const auto& op : ops) {
                            if (op.kind == cch::tests::ProtocolRecordingTerminal::OpKind::Write &&
                                    op.write_bytes == bytes) {
                                found = true;
                                break;
                            }
                        }
                        CHECK(found);
                    }
                }
            }

            REQUIRE(target_root->stop());
        }
    }
}
