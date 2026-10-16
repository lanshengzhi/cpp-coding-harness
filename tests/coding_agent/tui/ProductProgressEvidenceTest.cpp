// Supplemental evidence replay for product terminal progress settings, persistence,
// mode switch restoration, and protocol sequences (#1037, spec #946).
// Reads verified `product-progress.json` artifact for baseline replay.

#include "support/Json.hpp"
#include "support/PiTuiEvidence.hpp"
#include "support/TempWorkspace.hpp"

#include <cch/coding_agent/Settings.hpp>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

namespace {

using cch::support::JsonValue;

[[nodiscard]] const JsonValue& find_scenario(const JsonValue& bundle, std::string_view name) {
    for (const auto& scenario : bundle.at("scenarios").get_array()) {
        if (scenario.at("name").get_string() == name) return scenario;
    }
    REQUIRE(false);
    return bundle;
}

[[nodiscard]] std::string read_file(const std::filesystem::path& path) {
    std::ifstream file(path);
    return std::string{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

} // namespace

TEST_CASE("frozen product-progress settings resolution replays against SettingsManager",
        "[coding_agent][settings][progress][evidence][issue1037][spec]") {
    const auto artifact = cch::tests::read_pi_tui_evidence("product-progress.json");
    REQUIRE(artifact);
    const auto& bundle = *artifact;
    REQUIRE(bundle.at("baseline").get_string() == "pi-v1.0.4");
    REQUIRE(bundle.at("revision").get_string() == "7c10bd4337495ee613f2224843ecdf349b80d1df");

    const auto& scenario = find_scenario(bundle, "product-progress-settings-resolution");
    const auto& results = scenario.at("expected").at("settingsResults");

    auto test_resolution = [&](const std::string& field_name,
                                   std::optional<std::string> global_json,
                                   std::optional<std::string> project_json,
                                   bool trusted) {
        cch::tests::TempWorkspace global_ws;
        cch::tests::TempWorkspace project_ws;
        if (global_json) global_ws.write("settings.json", *global_json);
        if (project_json) project_ws.write(".pi/settings.json", *project_json);

        auto manager = cch::coding_agent::SettingsManager::create(project_ws.path(), global_ws.path(), trusted);

        const bool expected = results.at(field_name).get_boolean();
        INFO("Field name: " << field_name);
        CHECK(manager.get_show_terminal_progress() == expected);
    };

    test_resolution("defaultUnset", std::nullopt, std::nullopt, false);
    test_resolution("explicitGlobalTrue", R"({"terminal": {"showTerminalProgress": true}})", std::nullopt, false);
    test_resolution("explicitGlobalFalse", R"({"terminal": {"showTerminalProgress": false}})", std::nullopt, false);
    test_resolution("projectScopeTrustedWinsTrue",
            R"({"terminal": {"showTerminalProgress": false}})",
            R"({"terminal": {"showTerminalProgress": true}})",
            true);
    test_resolution("projectScopeTrustedWinsFalse",
            R"({"terminal": {"showTerminalProgress": true}})",
            R"({"terminal": {"showTerminalProgress": false}})",
            true);
    test_resolution("projectScopeUntrustedIgnored",
            R"({"terminal": {"showTerminalProgress": true}})",
            R"({"terminal": {"showTerminalProgress": false}})",
            false);
    test_resolution("projectScopeUntrustedGlobalDefault",
            std::nullopt,
            R"({"terminal": {"showTerminalProgress": true}})",
            false);
    test_resolution(
            "projectScopeTrustedGlobalUnset", std::nullopt, R"({"terminal": {"showTerminalProgress": true}})", true);
}

TEST_CASE("frozen product-progress settings persistence replays surgical nested writes",
        "[coding_agent][settings][progress][evidence][issue1037][spec]") {
    const auto artifact = cch::tests::read_pi_tui_evidence("product-progress.json");
    REQUIRE(artifact);
    const auto& bundle = *artifact;

    const auto& scenario = find_scenario(bundle, "product-progress-settings-persistence");
    const auto& expected = scenario.at("expected");

    cch::tests::TempWorkspace global_ws;
    cch::tests::TempWorkspace project_ws;
    global_ws.write("settings.json", R"({"theme": "dark", "terminal": {"clearOnShrink": true, "unknownProp": 42}})");

    auto manager = cch::coding_agent::SettingsManager::create(project_ws.path(), global_ws.path(), true);
    CHECK(manager.get_show_terminal_progress() == expected.at("initialResolved").get_boolean());

    REQUIRE(manager.set_show_terminal_progress(true));
    CHECK(manager.get_show_terminal_progress() == expected.at("afterSetTrue").get_boolean());
    const auto saved1 = read_file(global_ws.path() / "settings.json");
    CHECK(saved1.find("\"showTerminalProgress\": true") != std::string::npos);
    CHECK(saved1.find("\"clearOnShrink\": true") != std::string::npos);
    CHECK(saved1.find("\"unknownProp\": 42") != std::string::npos);
    CHECK(saved1.find("\"theme\": \"dark\"") != std::string::npos);

    REQUIRE(manager.set_show_terminal_progress(false));
    CHECK(manager.get_show_terminal_progress() == expected.at("afterSetFalse").get_boolean());
    const auto saved2 = read_file(global_ws.path() / "settings.json");
    CHECK(saved2.find("\"showTerminalProgress\": false") != std::string::npos);
    CHECK(saved2.find("\"clearOnShrink\": true") != std::string::npos);
    CHECK(saved2.find("\"unknownProp\": 42") != std::string::npos);
}

TEST_CASE("frozen product-progress protocol constants replay exact escape sequences",
        "[coding_agent][progress][protocol][evidence][issue1037][spec]") {
    const auto artifact = cch::tests::read_pi_tui_evidence("product-progress.json");
    REQUIRE(artifact);
    const auto& bundle = *artifact;

    const auto& scenario = find_scenario(bundle, "product-progress-protocol-constants");
    const auto& expected = scenario.at("expected");

    const std::string active_seq = expected.at("activeSequence").get_string();
    const std::string clear_seq = expected.at("clearSequence").get_string();

    CHECK(active_seq == "\x1b]9;4;3\x07");
    CHECK((clear_seq == "\x1b]9;4;0\x07" || clear_seq == "\x1b]9;4;0;\x07"));
    CHECK(expected.at("turnStartAction").get_string() == "active");
    CHECK(expected.at("agentEndAction").get_string() == "clear");
    CHECK(expected.at("compactionStartAction").get_string() == "active");
    CHECK(expected.at("compactionEndAction").get_string() == "clear");
    CHECK(expected.at("stopAction").get_string() == "clear");
}

TEST_CASE("frozen product-progress mode switch expectations match restoration semantics",
        "[coding_agent][progress][switch][evidence][issue1037][spec]") {
    const auto artifact = cch::tests::read_pi_tui_evidence("product-progress.json");
    REQUIRE(artifact);
    const auto& bundle = *artifact;

    const auto& scenario = find_scenario(bundle, "product-progress-mode-switch");
    const auto& switch_results = scenario.at("expected").at("switchResults");

    // Frozen pi observation confirms:
    // streamingEnabledRestore: restores progress (active emitted)
    CHECK(switch_results.at("streamingEnabledRestore").at("progressActiveEmitted").get_boolean() == true);
    // compactingEnabledRestore: restores progress (active emitted)
    CHECK(switch_results.at("compactingEnabledRestore").at("progressActiveEmitted").get_boolean() == true);
    // idleEnabledNoRestore: does NOT restore progress
    CHECK(switch_results.at("idleEnabledNoRestore").at("progressActiveEmitted").get_boolean() == false);
    // streamingDisabledNoRestore: does NOT restore progress
    CHECK(switch_results.at("streamingDisabledNoRestore").at("progressActiveEmitted").get_boolean() == false);
    // streamingEnabledSuppressed: does NOT restore progress when restoreProgress is false
    CHECK(switch_results.at("streamingEnabledSuppressed").at("progressActiveEmitted").get_boolean() == false);
}
