// Named pi-v1.0.4 acceptance for the CLI TUI-mode surface (#1017). Every
// expectation is read from the verified named bundle through
// `scripts/tui/evidence.py`; nothing here regenerates expectations from Pike.
//
// The frozen `--tui-mode` branch collects diagnostics and keeps parsing, while
// Pike's parser has no diagnostic channel and a rejected mode fails the parse
// carrying the frozen message text. The replay therefore compares a frozen case
// with diagnostics as a failed Pike parse with that text, and keeps every case
// the frozen branch can distinguish. Two divergences are asserted as recorded
// rather than skipped: an unset stored `tuiMode` resolves `regular` in Pike
// until #1024 flips the default, and the inline spelling `--tui-mode=regular`
// reaches Pike's `--name=value` form instead of the frozen unknown-flag bucket.

#include "coding_agent/cli/CliParse.hpp"
#include "coding_agent/cli/FrontendSelection.hpp"
#include "support/PiTuiEvidence.hpp"
#include "support/Json.hpp"
#include "support/TempWorkspace.hpp"

#include <cch/coding_agent/Settings.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using cch::support::JsonValue;

[[nodiscard]] cch::support::Expected<JsonValue> cli_tui_mode_evidence() {
    return cch::tests::read_pi_tui_evidence("cli-tui-mode.json");
}

[[nodiscard]] const JsonValue& scenario_of(const JsonValue& bundle, std::string_view name) {
    for (const auto& scenario : bundle.at("scenarios").get_array()) {
        if (scenario.at("name").get_string() == name) return scenario;
    }
    REQUIRE(false);
    return bundle;
}

/// The frozen `parseArgs` receives argv without the program name; Pike's
/// `parse_args` expects argv[0].
[[nodiscard]] std::vector<std::string> pike_argv_of(const JsonValue& frozen_argv) {
    std::vector<std::string> args{"cpp-harness"};
    for (const auto& token : frozen_argv.get_array())
        args.push_back(token.get_string());
    return args;
}

[[nodiscard]] cch::support::Expected<cch::cli::CliConfig> parse_of(const JsonValue& frozen_argv) {
    auto args = pike_argv_of(frozen_argv);
    std::vector<char*> argv;
    argv.reserve(args.size());
    for (auto& arg : args)
        argv.push_back(arg.data());
    return cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
}

[[nodiscard]] std::optional<cch::coding_agent::TuiMode> tui_mode_of(std::string_view frozen) {
    if (frozen == "regular") return cch::coding_agent::TuiMode::Regular;
    if (frozen == "fullscreen") return cch::coding_agent::TuiMode::Fullscreen;
    return std::nullopt;
}

[[nodiscard]] std::vector<std::string> messages_of(const JsonValue& case_expected) {
    std::vector<std::string> messages;
    for (const auto& message : case_expected.at("messages").get_array())
        messages.push_back(message.get_string());
    return messages;
}

/// A `SettingsManager` whose global document is exactly the frozen
/// `storedFile` (absent document: `{}`).
[[nodiscard]] cch::coding_agent::SettingsManager settings_with(const cch::tests::TempWorkspace& workspace,
        const std::filesystem::path& agent_dir,
        const JsonValue& stored_file) {
    workspace.write("agent/settings.json", stored_file.holds<std::nullptr_t>() ? "{}" : stored_file.get_string());
    return cch::coding_agent::SettingsManager::create(workspace.path(), agent_dir, /* project_trusted */ false);
}

} // namespace

TEST_CASE("frozen CLI --tui-mode parse cases replay through the Pike parser", "[cli][tui-mode][issue1017][compat-pi]") {
    const auto bundle = cli_tui_mode_evidence();
    REQUIRE(bundle);

    std::size_t replayed = 0;
    for (const auto& name : {"tui-mode-flag-accepts-the-two-roots",
                 "tui-mode-flag-rejects-a-missing-value-without-consuming",
                 "tui-mode-flag-rejects-an-unknown-mode-after-consuming-it"}) {
        const auto& scenario = scenario_of(*bundle, name);
        REQUIRE(scenario.at("expected").at("cases").get_array().size() ==
                scenario.at("inputs").at("cases").get_array().size());
        for (const auto& frozen_case : scenario.at("expected").at("cases").get_array()) {
            const auto case_name = frozen_case.at("name").get_string();
            INFO(case_name);
            const auto& frozen = frozen_case.at("expected");
            const auto diagnostics = frozen.at("diagnostics").get_array();
            const auto parsed = parse_of(frozen_case.at("argv"));
            ++replayed;

            if (case_name == "inline-regular-lands-in-unknown-flags") {
                // Recorded divergence: the frozen branch matches the exact token
                // `--tui-mode`, so the inline spelling only fills
                // `unknownFlags` (mode unset, no mode diagnostic). Pike's parser
                // gives every value option a `--name=value` form, so the same
                // token names the same mode.
                REQUIRE(parsed);
                REQUIRE(parsed->tui_mode.has_value());
                CHECK(*parsed->tui_mode == cch::coding_agent::TuiMode::Regular);
                CHECK(frozen.at("unknownFlags").get_object().size() == 1);
                CHECK(frozen.at("tuiMode").holds<std::nullptr_t>());
                CHECK(diagnostics.empty());
                continue;
            }

            if (diagnostics.empty()) {
                REQUIRE(parsed);
                const auto& frozen_mode = frozen.at("tuiMode");
                if (frozen_mode.holds<std::nullptr_t>()) {
                    CHECK_FALSE(parsed->tui_mode.has_value());
                } else {
                    REQUIRE(parsed->tui_mode.has_value());
                    CHECK(*parsed->tui_mode == tui_mode_of(frozen_mode.get_string()));
                }
                CHECK(parsed->messages == messages_of(frozen));
                CHECK(parsed->print == frozen.at("print").get_boolean());
            } else {
                // One frozen diagnostic becomes one failed parse: the `-`-value
                // case reports the missing-value text instead of treating
                // `--print` as the mode, and a consumed unknown value reports the
                // invalid-mode text while the following token stays a message.
                REQUIRE_FALSE(parsed);
                REQUIRE(diagnostics.size() == 1);
                CHECK(parsed.error().message == diagnostics.front().at("message").get_string());
            }
        }
    }
    CHECK(replayed == 10);
}

TEST_CASE("frozen CLI --tui-mode help line is the Pike help line", "[cli][tui-mode][issue1017][compat-pi]") {
    const auto bundle = cli_tui_mode_evidence();
    REQUIRE(bundle);
    const auto& scenario = scenario_of(*bundle, "tui-mode-help-line");
    const auto& lines = scenario.at("expected").at("lines").get_array();
    REQUIRE(lines.size() == 1);

    std::vector<std::string> args{"cpp-harness", "--help"};
    std::vector<char*> argv;
    argv.reserve(args.size());
    for (auto& arg : args)
        argv.push_back(arg.data());
    auto parsed = cch::cli::parse_args(static_cast<int>(argv.size()), argv.data());
    REQUIRE(parsed);
    CHECK(parsed->help_text.find(lines.front().get_string()) != std::string::npos);
}

TEST_CASE("frozen stored tuiMode values resolve through the Pike settings manager",
        "[cli][tui-mode][issue1017][compat-pi]") {
    const auto bundle = cli_tui_mode_evidence();
    REQUIRE(bundle);
    const auto& reads = scenario_of(*bundle, "settings-tui-mode-read-normalization").at("expected").at("reads");
    REQUIRE(reads.get_array().size() == 4);

    // Each read revisits its own stored document: `{}` for the absent one, and
    // an out-of-union value for the last row, which the frozen `getTuiMode`
    // still resolves.

    cch::tests::TempWorkspace workspace;
    const auto agent_dir = workspace.path() / "agent";
    for (const auto& read : reads.get_array()) {
        const auto& stored_file = read.at("storedFile");
        INFO(read.at("resolved").get_string());
        auto settings = settings_with(workspace, agent_dir, stored_file);
        REQUIRE(settings.errors().empty());
        const auto frozen_resolved = tui_mode_of(read.at("resolved").get_string());
        REQUIRE(frozen_resolved.has_value());
        // #1024 flipped the product default: an unset `tuiMode` now resolves
        // fullscreen on Pike as well, matching the frozen resolution.
        CHECK(settings.get_tui_mode() == *frozen_resolved);
    }
}

TEST_CASE("frozen setTuiMode rewrite persists the global key through Pike", "[cli][tui-mode][issue1017][compat-pi]") {
    const auto bundle = cli_tui_mode_evidence();
    REQUIRE(bundle);
    const auto& scenario = scenario_of(*bundle, "settings-tui-mode-write-persists-the-global-key");
    const auto& expected = scenario.at("expected");
    REQUIRE(scenario.at("inputs").at("setTuiMode").get_string() == "regular");

    cch::tests::TempWorkspace workspace;
    const auto agent_dir = workspace.path() / "agent";
    auto settings = settings_with(workspace, agent_dir, scenario.at("inputs").at("initialGlobal"));
    REQUIRE(settings.errors().empty());

    REQUIRE(settings.set_tui_mode(cch::coding_agent::TuiMode::Regular));
    CHECK(settings.errors().empty());
    CHECK(settings.get_tui_mode() == cch::coding_agent::TuiMode::Regular);

    const auto persisted = cch::support::read_json(workspace.read("agent/settings.json"));
    REQUIRE(persisted);
    const auto& frozen_persisted = expected.at("persistedGlobal");
    CHECK(persisted->at("tuiMode").get_string() == frozen_persisted.at("tuiMode").get_string());
    // The frozen rewrite keeps the unrelated key instead of replacing the file.
    CHECK(persisted->at("theme").get_string() == frozen_persisted.at("theme").get_string());
}

TEST_CASE("frozen --tui-mode over stored setting resolves through Pike precedence",
        "[cli][tui-mode][issue1017][compat-pi]") {
    // `options.tuiMode ?? this.settingsManager.getTuiMode()` at
    // `packages/coding-agent/src/modes/interactive/interactive-mode.ts:604`,
    // the resolution the CLI hands to the interactive run assembly.
    const auto bundle = cli_tui_mode_evidence();
    REQUIRE(bundle);
    const auto& scenario = scenario_of(*bundle, "tui-mode-flag-over-settings-precedence");
    const auto& resolutions = scenario.at("expected").at("resolutions");
    REQUIRE(resolutions.get_array().size() == 5);
    REQUIRE(scenario.at("inputs").at("expression").get_string() ==
            "options.tuiMode ?? this.settingsManager.getTuiMode()");
    REQUIRE(scenario.at("inputs").at("expressionEndpoint").get_string() ==
            "packages/coding-agent/src/modes/interactive/interactive-mode.ts:604");

    cch::tests::TempWorkspace workspace;
    const auto agent_dir = workspace.path() / "agent";
    std::size_t default_rows = 0;
    for (const auto& resolution : resolutions.get_array()) {
        const auto& frozen_cli = resolution.at("cliTuiMode");
        const auto& stored_file = resolution.at("storedFile");
        INFO(resolution.at("resolved").get_string());

        cch::cli::CliConfig config;
        const auto parsed = parse_of(resolution.at("argv"));
        if (frozen_cli.holds<std::nullptr_t>()) {
            // A rejected flag contributes no operand: frozen keeps parsing and
            // falls back to the stored document, while Pike's parse already
            // fails (asserted by the parse matrix) so the replay drives the
            // resolution with the absent flag.
            if (resolution.at("argv").get_array().size() != 0) {
                REQUIRE_FALSE(parsed);
            } else {
                REQUIRE(parsed);
                CHECK_FALSE(parsed->tui_mode.has_value());
            }
        } else {
            REQUIRE(parsed);
            REQUIRE(parsed->tui_mode.has_value());
            CHECK(*parsed->tui_mode == tui_mode_of(frozen_cli.get_string()));
            config.tui_mode = *parsed->tui_mode;
        }

        auto settings = settings_with(workspace, agent_dir, stored_file);
        REQUIRE(settings.errors().empty());
        const auto frozen_resolved = tui_mode_of(resolution.at("resolved").get_string());
        REQUIRE(frozen_resolved.has_value());
        const auto selected = cch::cli::select_tui_mode(config, settings);
        if (stored_file.holds<std::nullptr_t>() && frozen_cli.holds<std::nullptr_t>()) {
            // #1024 flipped the one recorded divergence: unset flag over unset
            // setting now resolves the frozen fullscreen default.
            ++default_rows;
            CHECK(selected == cch::coding_agent::TuiMode::Fullscreen);
            CHECK(*frozen_resolved == cch::coding_agent::TuiMode::Fullscreen);
        } else {
            CHECK(selected == *frozen_resolved);
        }
    }
    // The unset/unset row is still exercised; no row may diverge from the
    // frozen resolution now that #1024 flipped the default.
    CHECK(default_rows == 1);
}
