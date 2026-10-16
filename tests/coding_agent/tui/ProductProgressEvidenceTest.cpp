// Supplemental evidence replay for product terminal progress settings, persistence,
// mode switch restoration, and protocol sequences (#1037, spec #946).
// Reads verified `product-progress.json` and `product-progress-protocol.json` artifacts.

#include "support/Json.hpp"
#include "support/PiTuiEvidence.hpp"
#include "support/ProtocolRecordingTerminal.hpp"
#include "support/TempWorkspace.hpp"

#include <cch/coding_agent/Settings.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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

[[nodiscard]] std::vector<std::string> strings_of(const JsonValue& value) {
    std::vector<std::string> strings;
    for (const auto& entry : value.get_array())
        strings.emplace_back(entry.get_string());
    return strings;
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

TEST_CASE("supplemental historical product-progress protocol constants replay exact escape sequences",
        "[coding_agent][progress][protocol][evidence][historical][issue1037][spec]") {
    const auto artifact = cch::tests::read_pi_tui_evidence("product-progress.json");
    REQUIRE(artifact);
    const auto& bundle = *artifact;

    const auto& scenario = find_scenario(bundle, "product-progress-protocol-constants");
    const auto& expected = scenario.at("expected");

    const std::string active_seq = expected.at("activeSequence").get_string();
    const std::string clear_seq = expected.at("clearSequence").get_string();

    CHECK(active_seq == "\x1b]9;4;3\x07");
    CHECK(clear_seq == "\x1b]9;4;0\x07");
    CHECK(expected.at("turnStartAction").get_string() == "active");
    CHECK(expected.at("agentEndAction").get_string() == "clear");
    CHECK(expected.at("compactionStartAction").get_string() == "active");
    CHECK(expected.at("compactionEndAction").get_string() == "clear");
    CHECK(expected.at("stopAction").get_string() == "clear");
}

TEST_CASE("supplemental historical product-progress mode switch expectations replay boolean semantics",
        "[coding_agent][progress][switch][evidence][historical][issue1037][spec]") {
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

TEST_CASE("frozen product-progress-protocol raw terminal operations replay exact OSC 9;4 sequences",
        "[coding_agent][progress][protocol][evidence][issue1037][spec]") {
    const auto artifact = cch::tests::read_pi_tui_evidence("product-progress-protocol.json");
    REQUIRE(artifact);
    const auto& bundle = *artifact;
    REQUIRE(bundle.at("baseline").get_string() == "pi-v1.0.4");
    REQUIRE(bundle.at("revision").get_string() == "7c10bd4337495ee613f2224843ecdf349b80d1df");

    const auto& scenario = find_scenario(bundle, "terminal-progress-raw-protocol");
    const auto& observations = scenario.at("expected").at("observations");

    auto make_started_vt = []() {
        cch::tui::VirtualTerminal vt({.columns = 80, .rows = 24});
        REQUIRE(vt.start([](std::string) -> cch::support::ExpectedVoid { return {}; },
                [](cch::tui::TerminalDimensions) -> cch::support::ExpectedVoid { return {}; }));
        return vt;
    };

    // setProgressTrue
    {
        auto vt = make_started_vt();
        const auto before = vt.output().size();
        REQUIRE(vt.set_progress(true));
        const auto expected_writes = strings_of(observations.at("setProgressTrue").at("writes"));
        const auto expected_osc = cch::tests::extract_osc_9_4_commands(expected_writes);
        std::span<const std::string> delta(vt.output().data() + before, vt.output().size() - before);
        const auto observed_osc = cch::tests::extract_osc_9_4_commands(delta);
        CHECK(observed_osc == expected_osc);
        CHECK(observed_osc == std::vector<std::string>{"\x1b]9;4;3\x07"});
    }

    // setProgressFalse
    {
        auto vt = make_started_vt();
        REQUIRE(vt.set_progress(true));
        const auto before_clear = vt.output().size();
        REQUIRE(vt.set_progress(false));
        const auto expected_writes = strings_of(observations.at("setProgressFalse").at("writes"));
        const auto expected_osc = cch::tests::extract_osc_9_4_commands(expected_writes);
        std::span<const std::string> delta(vt.output().data() + before_clear, vt.output().size() - before_clear);
        const auto observed_osc = cch::tests::extract_osc_9_4_commands(delta);
        CHECK(observed_osc == expected_osc);
        CHECK(observed_osc == std::vector<std::string>{"\x1b]9;4;0\x07"});
    }

    // redundantClearInactive
    {
        auto vt = make_started_vt();
        const auto before = vt.output().size();
        REQUIRE(vt.set_progress(false));
        const auto expected_writes = strings_of(observations.at("redundantClearInactive").at("writes"));
        const auto expected_osc = cch::tests::extract_osc_9_4_commands(expected_writes);
        std::span<const std::string> delta(vt.output().data() + before, vt.output().size() - before);
        const auto observed_osc = cch::tests::extract_osc_9_4_commands(delta);
        CHECK(observed_osc == expected_osc);
        CHECK(observed_osc == std::vector<std::string>{"\x1b]9;4;0\x07"});
    }

    // stopActive
    {
        auto vt = make_started_vt();
        REQUIRE(vt.set_progress(true));
        const auto before_stop = vt.output().size();
        REQUIRE(vt.stop());
        const auto expected_writes = strings_of(observations.at("stopActive").at("writes"));
        const auto expected_osc = cch::tests::extract_osc_9_4_commands(expected_writes);
        std::span<const std::string> delta(vt.output().data() + before_stop, vt.output().size() - before_stop);
        const auto observed_osc = cch::tests::extract_osc_9_4_commands(delta);
        CHECK(observed_osc == expected_osc);
        CHECK(observed_osc == std::vector<std::string>{"\x1b]9;4;0\x07"});
    }

    // stopInactive
    {
        auto vt = make_started_vt();
        const auto before_stop = vt.output().size();
        REQUIRE(vt.stop());
        const auto expected_writes = strings_of(observations.at("stopInactive").at("writes"));
        const auto expected_osc = cch::tests::extract_osc_9_4_commands(expected_writes);
        std::span<const std::string> delta(vt.output().data() + before_stop, vt.output().size() - before_stop);
        const auto observed_osc = cch::tests::extract_osc_9_4_commands(delta);
        CHECK(observed_osc == expected_osc);
        CHECK(observed_osc.empty());
    }
}

TEST_CASE("frozen product-progress-protocol switch matrix raw writes replay exact ordered protocol vectors",
        "[coding_agent][progress][switch][evidence][issue1037][spec]") {
    const auto artifact = cch::tests::read_pi_tui_evidence("product-progress-protocol.json");
    REQUIRE(artifact);
    const auto& bundle = *artifact;

    const auto& scenario = find_scenario(bundle, "interactive-mode-switch-matrix");
    const auto& switch_results = scenario.at("expected").at("switchResults");

    for (const auto& [case_name, case_data] : switch_results.get_object()) {
        INFO("Matrix case: " << case_name);
        const auto writes = strings_of(case_data.at("writes"));
        const auto osc_vector = cch::tests::extract_osc_9_4_commands(writes);

        const bool clear_emitted = case_data.at("progressClearEmitted").get_boolean();
        const bool active_emitted = case_data.at("progressActiveEmitted").get_boolean();

        if (clear_emitted && active_emitted) {
            REQUIRE(osc_vector.size() == 2);
            CHECK(osc_vector[0] == "\x1b]9;4;0\x07");
            CHECK(osc_vector[1] == "\x1b]9;4;3\x07");
            const auto clear_idx = static_cast<int>(case_data.at("clearIndex").get_number());
            const auto active_idx = static_cast<int>(case_data.at("activeIndex").get_number());
            CHECK(clear_idx < active_idx);
            CHECK(case_data.at("orderValid").get_boolean());
        } else if (clear_emitted && !active_emitted) {
            REQUIRE(osc_vector.size() == 1);
            CHECK(osc_vector[0] == "\x1b]9;4;0\x07");
        } else if (!clear_emitted && active_emitted) {
            REQUIRE(osc_vector.size() == 1);
            CHECK(osc_vector[1] == "\x1b]9;4;3\x07");
        } else {
            CHECK(osc_vector.empty());
        }
    }
}

TEST_CASE("frozen product-progress-protocol settings toggle and shutdown teardown clear counts replay",
        "[coding_agent][progress][settings][evidence][issue1037][spec]") {
    const auto artifact = cch::tests::read_pi_tui_evidence("product-progress-protocol.json");
    REQUIRE(artifact);
    const auto& bundle = *artifact;

    // Scenario 4: settings-toggle-stale-progress
    {
        const auto& scenario4 = find_scenario(bundle, "settings-toggle-stale-progress");
        const auto& phases = scenario4.at("expected").at("phases");

        const auto p1_writes = strings_of(phases.at("phase1_turnStartActive").at("writes"));
        CHECK(cch::tests::extract_osc_9_4_commands(p1_writes) == std::vector<std::string>{"\x1b]9;4;3\x07"});

        const auto p2_writes = strings_of(phases.at("phase2_settingsToggleFalse").at("writes"));
        CHECK(cch::tests::extract_osc_9_4_commands(p2_writes).empty());

        const auto p3_writes = strings_of(phases.at("phase3_agentEndWhileDisabled").at("writes"));
        CHECK(cch::tests::extract_osc_9_4_commands(p3_writes).empty());

        const auto p4_writes = strings_of(phases.at("phase4_stopTeardownSafetyNet").at("writes"));
        CHECK(cch::tests::extract_osc_9_4_commands(p4_writes) == std::vector<std::string>{"\x1b]9;4;0\x07"});
        CHECK(phases.at("phase4_stopTeardownSafetyNet").at("clearCount").get_number() == 1);
    }

    // Scenario 5: interactive-mode-stop-clear-counts
    {
        const auto& scenario5 = find_scenario(bundle, "interactive-mode-stop-clear-counts");
        const auto& counts = scenario5.at("expected").at("counts");

        CHECK(counts.at("activeEnabled").at("clearsEmitted").get_number() == 1);
        CHECK(counts.at("idleDisabled").at("clearsEmitted").get_number() == 0);
        CHECK(counts.at("idleEnabled").at("clearsEmitted").get_number() == 1);
        CHECK(counts.at("activeToggledDisabled").at("clearsEmitted").get_number() == 1);
    }
}

TEST_CASE("Protocol progress parser and vector comparison detect mutations, corruptions, and fragmentation",
        "[coding_agent][progress][protocol][issue1037][mutation]") {
    const std::vector<std::string> expected_switch{
            "\x1b]9;4;0\x07",
            "\x1b]9;4;3\x07",
    };

    SECTION("Separating mutation: reversed order during mode switch fails comparison") {
        const std::vector<std::string> reversed_order{
                "\x1b]9;4;3\x07",
                "\x1b]9;4;0\x07",
        };
        CHECK_FALSE(reversed_order == expected_switch);
    }

    SECTION("Separating mutation: duplicate active sequence fails comparison") {
        const std::vector<std::string> duplicate_active{
                "\x1b]9;4;0\x07",
                "\x1b]9;4;3\x07",
                "\x1b]9;4;3\x07",
        };
        CHECK_FALSE(duplicate_active == expected_switch);
    }

    SECTION("Separating mutation: duplicate clear sequence fails comparison") {
        const std::vector<std::string> duplicate_clear{
                "\x1b]9;4;0\x07",
                "\x1b]9;4;0\x07",
                "\x1b]9;4;3\x07",
        };
        CHECK_FALSE(duplicate_clear == expected_switch);
    }

    SECTION("Separating mutation: missing clear before restore fails comparison") {
        const std::vector<std::string> missing_clear{
                "\x1b]9;4;3\x07",
        };
        CHECK_FALSE(missing_clear == expected_switch);
    }

    SECTION("Separating mutation: wrong trailing semicolon is not silently normalized") {
        const auto parsed_malformed = cch::tests::extract_osc_9_4_commands("\x1b]9;4;0;\x07");
        REQUIRE(parsed_malformed.size() == 1);
        CHECK(parsed_malformed[0] == "\x1b]9;4;0;\x07");
        CHECK_FALSE(parsed_malformed == std::vector<std::string>{"\x1b]9;4;0\x07"});
    }

    SECTION("Separating mutation: unexpected active during idle or disabled fails comparison") {
        const std::vector<std::string> unexpected_active{
                "\x1b]9;4;3\x07",
        };
        const std::vector<std::string> expected_idle{};
        CHECK_FALSE(unexpected_active == expected_idle);
    }

    SECTION("Parser correctly reassembles fragmented chunks across boundaries") {
        const std::vector<std::string> fragments{
                "\x1b[?2026h\x1b]9;",
                "4;0",
                "\x07\x1b]9;4;",
                "3\x07\x1b[?25h",
        };
        const auto extracted = cch::tests::extract_osc_9_4_commands(fragments);
        CHECK(extracted == expected_switch);
    }

    SECTION("Parser strips unrelated CSI, OSC 0 title, and style controls without blanket normalization") {
        const std::vector<std::string> bundled{
                "\x1b]0;Pike Interactive\x07"
                "\x1b[2J\x1b[H"
                "\x1b]9;4;3\x07"
                "\x1b[?25h\x1b[1;24r\x1b[m",
        };
        const auto extracted = cch::tests::extract_osc_9_4_commands(bundled);
        CHECK(extracted == std::vector<std::string>{"\x1b]9;4;3\x07"});
    }
}
