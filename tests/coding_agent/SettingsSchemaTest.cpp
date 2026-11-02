// Ticket #1115 (spec #1044, ADR 0075): the published `settings.json` schema
// surface and its diagnostics, compared against the frozen pi-v1.3.0 captures.
//
// Evidence:
//   fixtures/pi-v1.3.0/audit-1049/configuration-schema.json — scenario
//     "settings schema accepts the supported shape and unknown top-level keys"
//     and "settings schema rejects invalid thinking level and negative
//     compaction reserve".
//   fixtures/pi-v1.3.0/audit-1115/settings-diagnostics.json — scenarios
//     "the settings schema rejects an invalid thinking level and a negative
//     compaction reserve naming the key", "the settings schema names the
//     offending path and pi message for each rejected shape", "the compaction
//     consumer rejects negative and fractional token settings naming the field"
//     and "a schema-rejected thinking level loads without a diagnostic and the
//     runtime keeps the raw value".
//
// The published table names only fields a consumer in this build reads, so the
// schema cannot promise behavior the runtime drops. The expected table below is
// written independently of `settings_schema_fields()`: a field added to the
// schema without a reader here, or a reader without a schema entry, fails the
// equality case.

#include <cch/coding_agent/Settings.hpp>

#include "support/Json.hpp"
#include "support/TempWorkspace.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace cch;

namespace {

[[nodiscard]] support::JsonValue parse_document(std::string_view text) {
    auto parsed = support::read_json(text);
    REQUIRE(parsed.has_value());
    return std::move(*parsed);
}

/// The frozen artifact's scenario with this exact name.
[[nodiscard]] support::JsonValue frozen_scenario(std::string_view artifact, std::string_view name) {
    const auto path = std::filesystem::path(CCH_SOURCE_DIR) / "fixtures" / "pi-v1.3.0" / artifact;
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.good());
    std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    auto document = parse_document(text);
    const auto* scenarios = document.get_if<support::JsonValue::object_t>();
    REQUIRE(scenarios != nullptr);
    const auto found = scenarios->find("scenarios");
    REQUIRE(found != scenarios->end());
    for (const auto& scenario : found->second.get_array()) {
        const auto* object = scenario.get_if<support::JsonValue::object_t>();
        REQUIRE(object != nullptr);
        const auto scenario_name = object->find("name");
        REQUIRE(scenario_name != object->end());
        if (scenario_name->second.get<std::string>() == name) {
            const auto expected = object->find("expected");
            REQUIRE(expected != object->end());
            return expected->second;
        }
    }
    FAIL("frozen scenario not found: " << name);
    return {};
}

[[nodiscard]] std::map<std::string, std::string, std::less<>> violations_by_path(
        const std::vector<coding_agent::SettingsSchemaViolation>& violations) {
    std::map<std::string, std::string, std::less<>> by_path;
    for (const auto& violation : violations) {
        by_path.emplace(violation.path, violation.message);
    }
    return by_path;
}

} // namespace

TEST_CASE("settings schema accepts every published field and an unknown top-level key",
        "[coding_agent][settings][schema][issue1115][spec]") {
    // The frozen supported shape (`audit-1049` scenario 1) is a pi key set that
    // includes third-party `packages`/`extensions` and resource-path lists this
    // build does not consume, so the comparison is the property it records: the
    // supported shape validates and `additionalProperties` keeps an unknown
    // top-level key legal.
    auto supported = parse_document(R"({
        "defaultProvider": "magpie",
        "defaultModel": "group/model",
        "defaultThinkingLevel": "high",
        "enabledModels": ["group/a", "group/b"],
        "sessionDir": "/tmp/sessions",
        "defaultProjectTrust": "always",
        "shellPath": "~/.local/bin/shell",
        "shellCommandPrefix": "export READY=1",
        "theme": "dark",
        "tuiMode": "fullscreen",
        "fullscreenExitOutput": "resume-hint",
        "fullscreenScrollbar": "hidden",
        "fullscreenCopyOnSelect": false,
        "fullscreenWheelScrollLines": 5,
        "compaction": {"enabled": true, "reserveTokens": 1234, "keepRecentTokens": 20000},
        "retry": {"enabled": false, "maxRetries": 0, "baseDelayMs": 2000},
        "codemode": {"mode": "only", "inlineBudget": 3000},
        "hideThinkingBlock": true,
        "outputPad": 0,
        "enableSkillCommands": false,
        "showHardwareCursor": true,
        "terminal": {"showTerminalProgress": true},
        "deviceId": "11111111-2222-4333-8444-555555555555"
    })");
    CHECK(coding_agent::validate_settings_schema(supported).empty());

    // pi `additionalProperties: true`: an unknown top-level key is accepted and
    // preserved, so a newer pi settings file cannot break this build.
    auto with_unknown = parse_document(R"({"futureSetting": true, "theme": "dark"})");
    CHECK(coding_agent::validate_settings_schema(with_unknown).empty());

    // An unknown nested member inside a consumed object is accepted too.
    auto with_unknown_nested = parse_document(R"({"compaction": {"futureKnob": 1, "enabled": true}})");
    CHECK(coding_agent::validate_settings_schema(with_unknown_nested).empty());
}

TEST_CASE("settings schema rejects the two frozen invalid shapes with pi's path and message",
        "[coding_agent][settings][schema][issue1115][spec]") {
    // audit-1115 scenario "the settings schema rejects an invalid thinking level
    // and a negative compaction reserve naming the key".
    const auto expected = frozen_scenario("audit-1115/settings-diagnostics.json",
            "the settings schema rejects an invalid thinking level and a negative compaction reserve naming the key");
    const auto* object = expected.get_if<support::JsonValue::object_t>();
    REQUIRE(object != nullptr);

    auto invalid_thinking = parse_document(R"({"defaultThinkingLevel": "bogus"})");
    const auto thinking = violations_by_path(coding_agent::validate_settings_schema(invalid_thinking));
    REQUIRE(thinking.size() == 1);
    CHECK(thinking.count(object->at("invalidThinkingInstancePath").get<std::string>()) == 1);
    CHECK(thinking.begin()->second == object->at("invalidThinkingMessage").get<std::string>());

    auto negative_reserve = parse_document(R"({"compaction": {"reserveTokens": -1}})");
    const auto reserve = violations_by_path(coding_agent::validate_settings_schema(negative_reserve));
    REQUIRE(reserve.size() == 1);
    CHECK(reserve.count(object->at("negativeReserveInstancePath").get<std::string>()) == 1);
    CHECK(reserve.begin()->second == object->at("negativeReserveMessage").get<std::string>());

    CHECK(object->at("unknownTopLevelCheck").get<bool>());
}

TEST_CASE("settings schema names the offending path and pi message for every rejected shape",
        "[coding_agent][settings][schema][issue1115][spec]") {
    // audit-1115 scenario "the settings schema names the offending path and pi
    // message for each rejected shape": the same documents, compared one by one
    // so a message respelling separates.
    const auto expected = frozen_scenario("audit-1115/settings-diagnostics.json",
            "the settings schema names the offending path and pi message for each rejected shape");
    const auto* object = expected.get_if<support::JsonValue::object_t>();
    REQUIRE(object != nullptr);
    const auto expect = [object](std::string_view key, std::string_view document) {
        const auto* entry = object->at(std::string{key}).get_if<support::JsonValue::object_t>();
        REQUIRE(entry != nullptr);
        const auto violations = violations_by_path(coding_agent::validate_settings_schema(parse_document(document)));
        const auto path = entry->at("instancePath").get<std::string>();
        const auto message = entry->at("message").get<std::string>();
        INFO(document);
        REQUIRE(violations.count(path) == 1);
        CHECK(violations.at(path) == message);
    };

    expect("tuiMode", R"({"tuiMode": "bogus"})");
    expect("outputPad", R"({"outputPad": 5})");
    expect("hideThinkingBlock", R"({"hideThinkingBlock": "yes"})");
    expect("reserveTokens", R"({"compaction": {"reserveTokens": "abc"}})");
    expect("enabledModelsElement", R"({"enabledModels": [1]})");
    expect("compactionObject", R"({"compaction": "x"})");
    expect("wheelScrollLines", R"({"fullscreenWheelScrollLines": true})");
}

TEST_CASE("a malformed container reports one violation and no leaf violations",
        "[coding_agent][settings][schema][issue1115][spec]") {
    // The container's own type violation is the whole finding: a leaf below a
    // non-object container is absent, not separately malformed. Reporting both
    // would double-count one bad value and would not match pi's single
    // `/compaction: must be object` entry.
    for (const auto& document : {
                 R"({"compaction": "x"})",
                 R"({"retry": 1})",
                 R"({"codemode": []})",
                 R"({"terminal": "yes"})",
         }) {
        INFO(document);
        const auto violations = coding_agent::validate_settings_schema(parse_document(document));
        REQUIRE(violations.size() == 1);
        CHECK(violations.front().message == "must be object");
    }
}

TEST_CASE("the published settings schema lists exactly the fields this build consumes",
        "[coding_agent][settings][schema][issue1115][spec]") {
    // The published table is the contract of *consumable* fields. This expected
    // list is written by hand, so it separates both directions: a schema key
    // added without a reader here, and a reader added without a schema entry.
    const std::vector<std::pair<std::string, coding_agent::SettingsValueKind>> expected{
            {"defaultProvider", coding_agent::SettingsValueKind::String},
            {"defaultModel", coding_agent::SettingsValueKind::String},
            {"defaultThinkingLevel", coding_agent::SettingsValueKind::ThinkingLevel},
            {"enabledModels", coding_agent::SettingsValueKind::StringArray},
            {"sessionDir", coding_agent::SettingsValueKind::String},
            {"defaultProjectTrust", coding_agent::SettingsValueKind::ProjectTrust},
            {"shellPath", coding_agent::SettingsValueKind::String},
            {"shellCommandPrefix", coding_agent::SettingsValueKind::String},
            {"theme", coding_agent::SettingsValueKind::String},
            {"tuiMode", coding_agent::SettingsValueKind::TuiMode},
            {"fullscreenExitOutput", coding_agent::SettingsValueKind::FullscreenExitOutput},
            {"fullscreenScrollbar", coding_agent::SettingsValueKind::FullscreenScrollbar},
            {"fullscreenCopyOnSelect", coding_agent::SettingsValueKind::Boolean},
            {"fullscreenWheelScrollLines", coding_agent::SettingsValueKind::WheelScrollLines},
            {"compaction", coding_agent::SettingsValueKind::Object},
            {"compaction.enabled", coding_agent::SettingsValueKind::Boolean},
            {"compaction.reserveTokens", coding_agent::SettingsValueKind::NonNegativeInteger},
            {"compaction.keepRecentTokens", coding_agent::SettingsValueKind::NonNegativeInteger},
            {"retry", coding_agent::SettingsValueKind::Object},
            {"retry.enabled", coding_agent::SettingsValueKind::Boolean},
            {"retry.maxRetries", coding_agent::SettingsValueKind::NonNegativeInteger},
            {"retry.baseDelayMs", coding_agent::SettingsValueKind::NonNegativeInteger},
            {"codemode", coding_agent::SettingsValueKind::Object},
            {"codemode.mode", coding_agent::SettingsValueKind::CodemodeMode},
            {"codemode.inlineBudget", coding_agent::SettingsValueKind::NonNegativeInteger},
            {"hideThinkingBlock", coding_agent::SettingsValueKind::Boolean},
            {"outputPad", coding_agent::SettingsValueKind::OutputPad},
            {"enableSkillCommands", coding_agent::SettingsValueKind::Boolean},
            {"showHardwareCursor", coding_agent::SettingsValueKind::Boolean},
            {"terminal", coding_agent::SettingsValueKind::Object},
            {"terminal.showTerminalProgress", coding_agent::SettingsValueKind::Boolean},
            {"deviceId", coding_agent::SettingsValueKind::String},
    };

    const auto published = coding_agent::settings_schema_fields();
    REQUIRE(published.size() == expected.size());
    for (std::size_t index = 0; index < expected.size(); ++index) {
        INFO(index);
        CHECK(published[index].key == expected[index].first);
        CHECK(published[index].kind == expected[index].second);
    }
}

TEST_CASE("a settings document the schema accepts loads into the typed fields it publishes",
        "[coding_agent][settings][schema][issue1115][spec]") {
    // Coupling: every published leaf field, present and well-typed, reaches the
    // typed value its consumer reads. A published field the loader silently
    // drops fails here even though the schema still accepts the document.
    tests::TempWorkspace workspace;
    const auto cwd = workspace.path();
    const auto agent_dir = cwd / "agent";
    workspace.write("agent/settings.json", R"({
        "defaultProvider": "schema-provider",
        "defaultModel": "schema-model",
        "defaultThinkingLevel": "high",
        "enabledModels": ["schema/a"],
        "sessionDir": "/tmp/schema-sessions",
        "defaultProjectTrust": "never",
        "shellPath": "/bin/schema-shell",
        "shellCommandPrefix": "export SCHEMA=1",
        "theme": "schema-theme",
        "tuiMode": "fullscreen",
        "fullscreenExitOutput": "resume-hint",
        "fullscreenScrollbar": "hidden",
        "fullscreenCopyOnSelect": false,
        "fullscreenWheelScrollLines": 5,
        "compaction": {"enabled": false, "reserveTokens": 1234, "keepRecentTokens": 4321},
        "retry": {"enabled": false, "maxRetries": 2, "baseDelayMs": 900},
        "codemode": {"mode": "only", "inlineBudget": 777},
        "hideThinkingBlock": true,
        "outputPad": 0,
        "enableSkillCommands": false,
        "showHardwareCursor": true,
        "terminal": {"showTerminalProgress": true},
        "deviceId": "11111111-2222-4333-8444-555555555555"
    })");

    auto manager = coding_agent::SettingsManager::create(cwd, agent_dir, /* project_trusted */ true);
    REQUIRE(manager.errors().empty());
    const auto& settings = manager.settings();
    CHECK(settings.default_provider == "schema-provider");
    CHECK(settings.default_model == "schema-model");
    CHECK(settings.default_thinking_level == "high");
    REQUIRE(settings.enabled_models.has_value());
    CHECK(*settings.enabled_models == std::vector<std::string>{"schema/a"});
    CHECK(settings.session_dir == "/tmp/schema-sessions");
    REQUIRE(manager.default_project_trust().has_value());
    CHECK(*manager.default_project_trust() == coding_agent::DefaultProjectTrust::Never);
    CHECK(settings.shell_path == "/bin/schema-shell");
    CHECK(settings.shell_command_prefix == "export SCHEMA=1");
    CHECK(settings.theme == "schema-theme");
    CHECK(manager.get_tui_mode() == coding_agent::TuiMode::Fullscreen);
    CHECK(manager.get_fullscreen_exit_output() == coding_agent::FullscreenExitOutput::ResumeHint);
    CHECK(manager.get_fullscreen_scrollbar() == coding_agent::FullscreenScrollbar::Hidden);
    CHECK_FALSE(manager.get_fullscreen_copy_on_select());
    CHECK(manager.get_fullscreen_wheel_scroll_lines() == coding_agent::FullscreenWheelScrollLines{std::size_t{5}});
    REQUIRE(settings.compaction.has_value());
    CHECK(settings.compaction->enabled == false);
    CHECK(settings.compaction->reserve_tokens == 1234);
    CHECK(settings.compaction->keep_recent_tokens == 4321);
    REQUIRE(settings.retry.has_value());
    CHECK(settings.retry->enabled == false);
    CHECK(settings.retry->max_retries == 2);
    CHECK(settings.retry->base_delay_ms == 900);
    CHECK(manager.codemode_mode() == "only");
    REQUIRE(manager.codemode_inline_budget().has_value());
    CHECK(*manager.codemode_inline_budget() == 777);
    CHECK(manager.hide_thinking_block());
    CHECK(manager.output_pad() == 0);
    CHECK_FALSE(manager.get_enable_skill_commands());
    CHECK(manager.get_show_hardware_cursor());
    CHECK(manager.get_show_terminal_progress());
    CHECK(manager.get_or_create_device_id() == "11111111-2222-4333-8444-555555555555");
}

TEST_CASE("a negative or fractional compaction token fails the scope load with pi's text",
        "[coding_agent][settings][schema][issue1115][spec]") {
    // audit-1115 scenario "the compaction consumer rejects negative and
    // fractional token settings naming the field": pi throws this exact text
    // from `getCompactionTokenSetting`. This build has no exception to throw
    // from a getter, so the same validation runs at the load boundary and
    // records the same text as the scope load error.
    const auto expected = frozen_scenario("audit-1115/settings-diagnostics.json",
            "the compaction consumer rejects negative and fractional token settings naming the field");
    const auto* object = expected.get_if<support::JsonValue::object_t>();
    REQUIRE(object != nullptr);

    const std::vector<std::pair<std::string, std::string>> cases{
            {R"({"compaction": {"reserveTokens": -1}})", object->at("negativeReserveMessage").get<std::string>()},
            {R"({"compaction": {"keepRecentTokens": -1}})", object->at("negativeKeepRecentMessage").get<std::string>()},
            {R"({"compaction": {"reserveTokens": 1.5}})", object->at("fractionalReserveMessage").get<std::string>()},
    };
    for (const auto& [document, message] : cases) {
        tests::TempWorkspace workspace;
        const auto cwd = workspace.path();
        workspace.write("agent/settings.json", document);
        auto manager = coding_agent::SettingsManager::create(cwd, cwd / "agent", /* project_trusted */ true);
        INFO(document);
        REQUIRE(manager.errors().size() == 1);
        CHECK(manager.errors().front().scope == coding_agent::SettingsScope::Global);
        CHECK(manager.errors().front().message == message);
        // A failed scope resolves as empty, so the consumer never reads the
        // rejected value and the other scope keeps working (ADR 0031).
        CHECK_FALSE(manager.settings().compaction.has_value());
    }
}

TEST_CASE("a schema-rejected thinking level fails the load instead of reaching the consumer",
        "[coding_agent][settings][schema][issue1115][spec]") {
    // audit-1115 scenario "a schema-rejected thinking level loads without a
    // diagnostic and the runtime keeps the raw value" records the *excluded*
    // upstream runtime-validation bug: pi's manager consumes the raw string.
    // This build does not reproduce it (ticket #1115's owner-approved
    // reassessment), so the load is rejected and the value never reaches a
    // consumer. The discriminating case is the raw value surviving.
    const auto expected = frozen_scenario("audit-1115/settings-diagnostics.json",
            "a schema-rejected thinking level loads without a diagnostic and the runtime keeps the raw value");
    const auto* object = expected.get_if<support::JsonValue::object_t>();
    REQUIRE(object != nullptr);
    CHECK(object->at("schemaCheck").get<bool>() == false);

    tests::TempWorkspace workspace;
    const auto cwd = workspace.path();
    workspace.write("agent/settings.json", R"({"defaultThinkingLevel": "bogus"})");
    auto manager = coding_agent::SettingsManager::create(cwd, cwd / "agent", /* project_trusted */ true);
    REQUIRE(manager.errors().size() == 1);
    CHECK(manager.errors().front().message.find("defaultThinkingLevel") != std::string::npos);
    CHECK_FALSE(manager.settings().default_thinking_level.has_value());
}

TEST_CASE("the settings diagnostic text is pi's file-and-message template",
        "[coding_agent][settings][schema][issue1115][spec]") {
    // audit-1115 scenario "a malformed global settings file yields one warning
    // naming the file path": pi's `collectSettingsDiagnostics` text is
    // `Invalid settings file <path>: <message>`, where the tail is the parser's
    // own error text.
    const auto expected = frozen_scenario("audit-1115/settings-diagnostics.json",
            "a malformed global settings file yields one warning naming the file path");
    const auto* object = expected.get_if<support::JsonValue::object_t>();
    REQUIRE(object != nullptr);
    const auto template_text = object->at("diagnosticTemplate").get<std::string>();
    CHECK(template_text == "Invalid settings file <path>: <message>");

    const auto text = coding_agent::settings_diagnostic_text("/tmp/agent/settings.json", "boom");
    CHECK(text == "Invalid settings file /tmp/agent/settings.json: boom");
}
