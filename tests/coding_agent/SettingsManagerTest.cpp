#include <cch/coding_agent/Settings.hpp>

#include "support/EnvVarGuard.hpp"
#include "support/TempWorkspace.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <variant>

using namespace cch;

namespace {

struct SettingsDirs {
    tests::TempWorkspace workspace;
    std::filesystem::path cwd;
    std::filesystem::path agent_dir;

    SettingsDirs() : cwd(workspace.path()), agent_dir(workspace.path() / "agent") {}

    void write_global(std::string content) const {
        workspace.write("agent/settings.json", content);
    }

    void write_project(std::string content) const {
        workspace.write(".pi/settings.json", content);
    }
};

} // namespace

TEST_CASE("SettingsManager resolves empty settings without files", "[settings][two-scope][spec]") {
    SettingsDirs dirs;
    auto manager = coding_agent::SettingsManager::create(
        dirs.cwd, dirs.agent_dir, /* project_trusted */ true);

    CHECK(manager.errors().empty());
    CHECK_FALSE(manager.settings().default_provider.has_value());
    CHECK_FALSE(manager.settings().default_model.has_value());
    CHECK_FALSE(manager.settings().theme.has_value());
    CHECK_FALSE(manager.default_project_trust().has_value());
}

TEST_CASE("SettingsManager loads the pi field subset from the global scope", "[settings][two-scope][spec]") {
    SettingsDirs dirs;
    dirs.write_global(R"({
        "defaultProvider": "deepseek",
        "defaultModel": "deepseek-v4-flash",
        "defaultThinkingLevel": "high",
        "enabledModels": ["deepseek-v4-flash", "deepseek-r1"],
        "sessionDir": "/data/sessions",
        "defaultProjectTrust": "always",
        "shellPath": "~/.local/bin/shell",
        "shellCommandPrefix": "export READY=1",
        "theme": "solarized"
    })");

    auto manager = coding_agent::SettingsManager::create(
        dirs.cwd, dirs.agent_dir, /* project_trusted */ true);

    REQUIRE(manager.errors().empty());
    const auto& settings = manager.settings();
    CHECK(settings.default_provider == "deepseek");
    CHECK(settings.default_model == "deepseek-v4-flash");
    CHECK(settings.default_thinking_level == "high");
    REQUIRE(settings.enabled_models.has_value());
    CHECK(settings.enabled_models->size() == 2);
    CHECK(settings.session_dir == "/data/sessions");
    CHECK(settings.shell_path == "~/.local/bin/shell");
    CHECK(settings.shell_command_prefix == "export READY=1");
    CHECK(settings.theme == "solarized");
    REQUIRE(manager.default_project_trust().has_value());
    CHECK(*manager.default_project_trust() == coding_agent::DefaultProjectTrust::Always);
}

TEST_CASE("SettingsManager ignores legacy and unknown keys", "[settings][two-scope][spec]") {
    SettingsDirs dirs;
    dirs.write_global(R"({"provider":"old","model":"old-m","base_url":"https://x",
        "api_key_env":["OLD_KEY"],"auth":"entry","project_resources":{"skills":"off"},
        "unknown_future":true,"defaultModel":"deepseek-v4-flash"})");

    auto manager = coding_agent::SettingsManager::create(
        dirs.cwd, dirs.agent_dir, /* project_trusted */ true);

    REQUIRE(manager.errors().empty());
    const auto& settings = manager.settings();
    // Removed harness-private fields never surface.
    CHECK_FALSE(settings.default_provider.has_value());
    CHECK(settings.default_model == "deepseek-v4-flash");
    // defaultProjectTrust stays global-only even when a legacy key is present.
    CHECK_FALSE(manager.default_project_trust().has_value());
}

TEST_CASE("SettingsManager deep-merges with the project scope winning", "[settings][two-scope][spec]") {
    SettingsDirs dirs;
    dirs.write_global(R"({
        "defaultProvider": "openai-codex",
        "defaultModel": "gpt-5.5",
        "theme": "dark",
        "shellPath": "/bin/global-shell"
    })");
    dirs.write_project(R"({
        "defaultModel": "kimi-for-coding",
        "theme": "light"
    })");

    auto manager = coding_agent::SettingsManager::create(
        dirs.cwd, dirs.agent_dir, /* project_trusted */ true);

    REQUIRE(manager.errors().empty());
    const auto& settings = manager.settings();
    // Project wins on overlap.
    CHECK(settings.default_model == "kimi-for-coding");
    CHECK(settings.theme == "light");
    // Global-only fields survive the merge.
    CHECK(settings.default_provider == "openai-codex");
    CHECK(settings.shell_path == "/bin/global-shell");
    CHECK(manager.global_settings().default_model == "gpt-5.5");
    CHECK(manager.project_settings().default_model == "kimi-for-coding");
}

TEST_CASE("SettingsManager ignores defaultProjectTrust from the project scope",
        "[settings][two-scope][global-only][spec]") {
    SettingsDirs dirs;
    dirs.write_global(R"({"defaultProjectTrust":"never"})");
    dirs.write_project(R"({"defaultProjectTrust":"always"})");

    auto manager = coding_agent::SettingsManager::create(
        dirs.cwd, dirs.agent_dir, /* project_trusted */ true);

    REQUIRE(manager.errors().empty());
    REQUIRE(manager.default_project_trust().has_value());
    CHECK(*manager.default_project_trust() == coding_agent::DefaultProjectTrust::Never);
}

TEST_CASE("SettingsManager loads the project scope only while trusted", "[settings][two-scope][trust][spec]") {
    SettingsDirs dirs;
    dirs.write_project(R"({"defaultModel":"project-model","theme":"project-theme"})");

    auto untrusted = coding_agent::SettingsManager::create(
        dirs.cwd, dirs.agent_dir, /* project_trusted */ false);
    CHECK_FALSE(untrusted.project_settings().default_model.has_value());
    CHECK_FALSE(untrusted.settings().default_model.has_value());
    CHECK_FALSE(untrusted.is_project_trusted());

    REQUIRE(untrusted.set_project_trusted(true));
    CHECK(untrusted.is_project_trusted());
    CHECK(untrusted.project_settings().default_model == "project-model");
    CHECK(untrusted.settings().theme == "project-theme");

    REQUIRE(untrusted.set_project_trusted(false));
    CHECK_FALSE(untrusted.project_settings().default_model.has_value());
    CHECK_FALSE(untrusted.settings().default_model.has_value());
}

TEST_CASE("SettingsManager records a global parse error and suppresses writes", "[settings][two-scope][error][spec]") {
    SettingsDirs dirs;
    dirs.write_global("{not valid json");

    auto manager = coding_agent::SettingsManager::create(
        dirs.cwd, dirs.agent_dir, /* project_trusted */ true);

    REQUIRE(manager.errors().size() == 1);
    CHECK(manager.errors()[0].scope == coding_agent::SettingsScope::Global);
    CHECK(manager.errors()[0].message.find("invalid JSON") != std::string::npos);
    CHECK_FALSE(manager.settings().default_model.has_value());

    // Writes to a scope whose load failed are suppressed (file left untouched).
    REQUIRE(manager.set_theme(coding_agent::SettingsScope::Global, "light"));
    CHECK(dirs.workspace.read("agent/settings.json") == "{not valid json");
}

TEST_CASE("SettingsManager records a project parse error independently", "[settings][two-scope][error][spec]") {
    SettingsDirs dirs;
    dirs.write_global(R"({"defaultModel":"global-model"})");
    dirs.write_project("{broken");

    auto manager = coding_agent::SettingsManager::create(
        dirs.cwd, dirs.agent_dir, /* project_trusted */ true);

    REQUIRE(manager.errors().size() == 1);
    CHECK(manager.errors()[0].scope == coding_agent::SettingsScope::Project);
    // The global scope still resolves.
    CHECK(manager.settings().default_model == "global-model");
}

TEST_CASE("SettingsManager rejects an invalid defaultThinkingLevel", "[settings][two-scope][error][spec]") {
    SettingsDirs dirs;
    dirs.write_global(R"({"defaultThinkingLevel":"sometimes"})");

    auto manager = coding_agent::SettingsManager::create(
        dirs.cwd, dirs.agent_dir, /* project_trusted */ true);

    REQUIRE(manager.errors().size() == 1);
    CHECK(manager.errors()[0].message.find("defaultThinkingLevel") != std::string::npos);
}

TEST_CASE("SettingsManager surgical theme write preserves unknown and unmodified fields",
        "[settings][two-scope][write][spec]") {
    SettingsDirs dirs;
    dirs.write_global(R"({
        "provider": "legacy",
        "model": "legacy-m",
        "future": {"enabled": true},
        "shellPath": "/bin/custom-shell",
        "shellCommandPrefix": "export READY=1",
        "theme": "dark"
    })");

    auto manager = coding_agent::SettingsManager::create(
        dirs.cwd, dirs.agent_dir, /* project_trusted */ true);
    REQUIRE(manager.errors().empty());

    REQUIRE(manager.set_theme(coding_agent::SettingsScope::Global, "light"));

    const auto content = dirs.workspace.read("agent/settings.json");
    CHECK(content.find("\"theme\": \"light\"") != std::string::npos);
    // Unmodified / unknown fields are preserved.
    CHECK(content.find("\"provider\": \"legacy\"") != std::string::npos);
    CHECK(content.find("\"model\": \"legacy-m\"") != std::string::npos);
    CHECK(content.find("\"future\"") != std::string::npos);
    CHECK(content.find("\"shellPath\": \"/bin/custom-shell\"") != std::string::npos);
    CHECK(content.find("\"shellCommandPrefix\": \"export READY=1\"") != std::string::npos);

    // The in-memory view updates too.
    CHECK(manager.settings().theme == "light");
    CHECK(manager.settings().shell_path == "/bin/custom-shell");
}

TEST_CASE("SettingsManager surgical theme write creates a missing file", "[settings][two-scope][write][spec]") {
    SettingsDirs dirs;
    auto manager = coding_agent::SettingsManager::create(
        dirs.cwd, dirs.agent_dir, /* project_trusted */ true);

    REQUIRE(manager.set_theme(coding_agent::SettingsScope::Global, "dark"));

    const auto content = dirs.workspace.read("agent/settings.json");
    CHECK(content.find("\"theme\": \"dark\"") != std::string::npos);
    CHECK(manager.settings().theme == "dark");
}

TEST_CASE("SettingsManager writes the project scope only when trusted", "[settings][two-scope][write][trust][spec]") {
    SettingsDirs dirs;
    dirs.write_project(R"({"theme":"dark"})");

    auto manager = coding_agent::SettingsManager::create(
        dirs.cwd, dirs.agent_dir, /* project_trusted */ false);

    // Project-scope writes are refused while the project is untrusted.
    auto refused = manager.set_theme(coding_agent::SettingsScope::Project, "light");
    REQUIRE_FALSE(refused);
    CHECK(refused.error().message.find("not trusted") != std::string::npos);

    REQUIRE(manager.set_project_trusted(true));
    REQUIRE(manager.set_theme(coding_agent::SettingsScope::Project, "light"));

    const auto content = dirs.workspace.read(".pi/settings.json");
    CHECK(content.find("\"theme\": \"light\"") != std::string::npos);
    CHECK(manager.settings().theme == "light");
}

TEST_CASE("SettingsManager surgical defaultThinkingLevel write preserves unknown fields and creates the file",
        "[settings][two-scope][write][issue353][spec]") {
    SettingsDirs dirs;
    dirs.write_global(R"({
        "defaultProvider": "alpha",
        "defaultModel": "alpha-1",
        "future": {"enabled": true},
        "theme": "dark"
    })");

    auto manager = coding_agent::SettingsManager::create(
        dirs.cwd, dirs.agent_dir, /* project_trusted */ true);
    REQUIRE(manager.errors().empty());

    REQUIRE(manager.set_default_thinking_level(
        coding_agent::SettingsScope::Global, "high"));

    const auto content = dirs.workspace.read("agent/settings.json");
    CHECK(content.find("\"defaultThinkingLevel\": \"high\"") != std::string::npos);
    // Unmodified / unknown fields are preserved.
    CHECK(content.find("\"defaultProvider\": \"alpha\"") != std::string::npos);
    CHECK(content.find("\"defaultModel\": \"alpha-1\"") != std::string::npos);
    CHECK(content.find("\"future\"") != std::string::npos);
    CHECK(content.find("\"theme\": \"dark\"") != std::string::npos);

    // The in-memory view updates too.
    CHECK(manager.settings().default_thinking_level == "high");
    CHECK(manager.settings().default_provider == "alpha");
}

TEST_CASE("SettingsManager defaultThinkingLevel write creates a missing file",
        "[settings][two-scope][write][issue353][spec]") {
    SettingsDirs dirs;
    auto manager = coding_agent::SettingsManager::create(
        dirs.cwd, dirs.agent_dir, /* project_trusted */ true);

    REQUIRE(manager.set_default_thinking_level(
        coding_agent::SettingsScope::Global, "low"));

    const auto content = dirs.workspace.read("agent/settings.json");
    CHECK(content.find("\"defaultThinkingLevel\": \"low\"") != std::string::npos);
    CHECK(manager.settings().default_thinking_level == "low");
}

TEST_CASE("SettingsManager surgical enabledModels write preserves unknown fields and clears on nullopt",
        "[settings][two-scope][write][issue407][spec]") {
    SettingsDirs dirs;
    dirs.write_global(R"({
        "defaultProvider": "alpha",
        "enabledModels": ["alpha-1"],
        "future": {"enabled": true}
    })");

    auto manager = coding_agent::SettingsManager::create(
        dirs.cwd, dirs.agent_dir, /* project_trusted */ true);
    REQUIRE(manager.errors().empty());
    REQUIRE(manager.settings().enabled_models.has_value());

    // Surgical write replaces the field, preserving every other field.
    REQUIRE(manager.set_enabled_models(std::vector<std::string>{"alpha-1", "beta-1"}));
    auto content = dirs.workspace.read("agent/settings.json");
    CHECK(content.find("\"enabledModels\": [\n    \"alpha-1\",\n    \"beta-1\"\n  ]") != std::string::npos);
    CHECK(content.find("\"defaultProvider\": \"alpha\"") != std::string::npos);
    CHECK(content.find("\"future\"") != std::string::npos);
    REQUIRE(manager.settings().enabled_models.has_value());
    CHECK(manager.settings().enabled_models->size() == 2);

    // nullopt removes the field (pi writes `undefined`, which its serializer
    // drops) and preserves the other fields.
    REQUIRE(manager.set_enabled_models(std::nullopt));
    content = dirs.workspace.read("agent/settings.json");
    CHECK(content.find("enabledModels") == std::string::npos);
    CHECK(content.find("\"defaultProvider\": \"alpha\"") != std::string::npos);
    CHECK_FALSE(manager.settings().enabled_models.has_value());

    // A no-op write reports success without rewriting.
    REQUIRE(manager.set_enabled_models(std::nullopt));
}

TEST_CASE("SettingsManager rejects an invalid defaultThinkingLevel write",
        "[settings][two-scope][write][issue353][spec]") {
    SettingsDirs dirs;
    auto manager = coding_agent::SettingsManager::create(
        dirs.cwd, dirs.agent_dir, /* project_trusted */ true);

    auto rejected = manager.set_default_thinking_level(
        coding_agent::SettingsScope::Global, "sometimes");
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().message.find("defaultThinkingLevel") != std::string::npos);
    // Nothing was written and the in-memory view is unchanged.
    CHECK_FALSE(std::filesystem::exists(dirs.workspace.path() / "agent" / "settings.json"));
    CHECK_FALSE(manager.settings().default_thinking_level.has_value());
}

TEST_CASE("SettingsManager defaultThinkingLevel writes the project scope only when trusted",
        "[settings][two-scope][write][trust][issue353][spec]") {
    SettingsDirs dirs;
    dirs.write_project(R"({"defaultThinkingLevel":"high"})");

    auto manager = coding_agent::SettingsManager::create(
        dirs.cwd, dirs.agent_dir, /* project_trusted */ false);

    auto refused = manager.set_default_thinking_level(
        coding_agent::SettingsScope::Project, "low");
    REQUIRE_FALSE(refused);
    CHECK(refused.error().message.find("not trusted") != std::string::npos);

    REQUIRE(manager.set_project_trusted(true));
    REQUIRE(manager.set_default_thinking_level(
        coding_agent::SettingsScope::Project, "low"));

    const auto content = dirs.workspace.read(".pi/settings.json");
    CHECK(content.find("\"defaultThinkingLevel\": \"low\"") != std::string::npos);
    // Project scope wins over the global scope in the merged view.
    CHECK(manager.settings().default_thinking_level == "low");
}

TEST_CASE("SettingsManager defaultThinkingLevel write is a no-op when unchanged",
        "[settings][two-scope][write][issue353][spec]") {
    SettingsDirs dirs;
    dirs.write_global(R"({"defaultThinkingLevel":"high"})");

    auto manager = coding_agent::SettingsManager::create(
        dirs.cwd, dirs.agent_dir, /* project_trusted */ true);

    REQUIRE(manager.set_default_thinking_level(
        coding_agent::SettingsScope::Global, "high"));
    // The file is untouched (no timestamp churn) and the view is unchanged.
    CHECK(dirs.workspace.read("agent/settings.json") == R"({"defaultThinkingLevel":"high"})");
    CHECK(manager.settings().default_thinking_level == "high");
}

TEST_CASE(
        "SettingsManager applies pi read-time migrations on load and write", "[settings][two-scope][migration][spec]") {
    SettingsDirs dirs;
    dirs.write_global(R"({
        "queueMode": "all",
        "websockets": true,
        "retry": {"maxDelayMs": 5000},
        "skills": {"enableSkillCommands": true, "customDirectories": ["/x", "/y"]},
        "defaultModel": "gpt-5.5"
    })");

    auto manager = coding_agent::SettingsManager::create(
        dirs.cwd, dirs.agent_dir, /* project_trusted */ true);
    REQUIRE(manager.errors().empty());
    CHECK(manager.settings().default_model == "gpt-5.5");

    REQUIRE(manager.set_theme(coding_agent::SettingsScope::Global, "light"));

    const auto content = dirs.workspace.read("agent/settings.json");
    // queueMode -> steeringMode
    CHECK(content.find("\"queueMode\"") == std::string::npos);
    CHECK(content.find("\"steeringMode\": \"all\"") != std::string::npos);
    // websockets boolean -> transport enum
    CHECK(content.find("\"websockets\"") == std::string::npos);
    CHECK(content.find("\"transport\": \"websocket\"") != std::string::npos);
    // retry.maxDelayMs -> retry.provider.maxRetryDelayMs
    CHECK(content.find("\"maxDelayMs\"") == std::string::npos);
    CHECK(content.find("\"maxRetryDelayMs\": 5000") != std::string::npos);
    // skills object -> array form
    CHECK(content.find("\"enableSkillCommands\": true") != std::string::npos);
    CHECK(content.find("\"/x\"") != std::string::npos);
    // The theme write applied surgically alongside the migration.
    CHECK(content.find("\"theme\": \"light\"") != std::string::npos);
}

TEST_CASE("SettingsManager reload re-reads both scopes and re-records errors", "[settings][two-scope][reload][spec]") {
    SettingsDirs dirs;
    auto manager = coding_agent::SettingsManager::create(
        dirs.cwd, dirs.agent_dir, /* project_trusted */ true);
    CHECK(manager.errors().empty());

    dirs.write_global(R"({"defaultModel":"new-model"})");
    REQUIRE(manager.reload());
    CHECK(manager.settings().default_model == "new-model");

    dirs.write_global("{broken");
    REQUIRE(manager.reload());
    REQUIRE(manager.errors().size() == 1);
    CHECK(manager.errors()[0].scope == coding_agent::SettingsScope::Global);
    CHECK_FALSE(manager.settings().default_model.has_value());
}

TEST_CASE("SettingsManager treats an empty agent_dir as no global scope", "[settings][two-scope][spec]") {
    tests::TempWorkspace workspace;
    auto manager = coding_agent::SettingsManager::create(
        workspace.path(), /* agent_dir */ {}, /* project_trusted */ true);

    CHECK(manager.global_path().empty());
    CHECK(manager.errors().empty());
    CHECK_FALSE(manager.settings().default_model.has_value());
    // A no-op write succeeds against the absent scope.
    REQUIRE(manager.set_theme(coding_agent::SettingsScope::Global, "x"));
    CHECK_FALSE(manager.settings().theme.has_value());
}

TEST_CASE("SettingsManager times out acquiring a held settings lock", "[settings][two-scope][lock][issue346][spec]") {
    SettingsDirs dirs;
    dirs.write_global(R"({"theme":"dark"})");

    // Simulate a competing proper-lockfile holder: a fresh `<path>.lock` dir.
    const auto lock_path = dirs.workspace.path() / "agent" / "settings.json.lock";
    std::filesystem::create_directories(lock_path);

    auto manager = coding_agent::SettingsManager::create(
        dirs.cwd, dirs.agent_dir, /* project_trusted */ true);
    REQUIRE(manager.errors().empty());

    auto saved = manager.set_theme(coding_agent::SettingsScope::Global, "light");
    REQUIRE_FALSE(saved);
    CHECK(saved.error().message.find("locked") != std::string::npos);
    // The in-memory view was not advanced and the file was not modified.
    CHECK(manager.settings().theme == "dark");
    CHECK(dirs.workspace.read("agent/settings.json") == R"({"theme":"dark"})");
    std::filesystem::remove_all(lock_path);
}

TEST_CASE("SettingsManager never loads secrets or secret-reference fields", "[settings][two-scope][secret][spec]") {
    SettingsDirs dirs;
    dirs.write_global(R"({
        "apiKey": "sk-super-secret-value",
        "defaultModel": "gpt-5.5"
    })");

    auto manager = coding_agent::SettingsManager::create(
        dirs.cwd, dirs.agent_dir, /* project_trusted */ true);

    // The unknown apiKey field is preserved on write but never exposed.
    REQUIRE(manager.set_theme(coding_agent::SettingsScope::Global, "dark"));
    const auto content = dirs.workspace.read("agent/settings.json");
    CHECK(content.find("sk-super-secret-value") != std::string::npos);
    CHECK(manager.settings().default_model == "gpt-5.5");
}

TEST_CASE("SettingsManager loads and deep-merges the compaction settings object",
        "[settings][two-scope][issue359][spec]") {
    SettingsDirs dirs;
    dirs.write_global(R"({
        "compaction": { "enabled": false, "reserveTokens": 8192 }
    })");
    dirs.write_project(R"({
        "compaction": { "keepRecentTokens": 4096 }
    })");

    auto manager = coding_agent::SettingsManager::create(
        dirs.cwd, dirs.agent_dir, /* project_trusted */ true);

    REQUIRE(manager.errors().empty());
    REQUIRE(manager.settings().compaction.has_value());
    // Per-field deep merge: the project scope wins per field, global fields
    // the project scope does not set are kept.
    CHECK(manager.settings().compaction->enabled == false);
    CHECK(manager.settings().compaction->reserve_tokens == 8192);
    CHECK(manager.settings().compaction->keep_recent_tokens == 4096);
    // The project scope alone carries only its own field.
    REQUIRE(manager.project_settings().compaction.has_value());
    CHECK_FALSE(manager.project_settings().compaction->enabled.has_value());
    CHECK_FALSE(manager.project_settings().compaction->reserve_tokens.has_value());
    CHECK(manager.project_settings().compaction->keep_recent_tokens == 4096);
}

TEST_CASE("SettingsManager compaction fields are optional and mistyped values fall back to defaults",
        "[settings][two-scope][issue359][spec]") {
    SettingsDirs dirs;
    dirs.write_global(R"({
        "compaction": { "enabled": "yes", "reserveTokens": -1, "keepRecentTokens": 10000 }
    })");

    auto manager = coding_agent::SettingsManager::create(
        dirs.cwd, dirs.agent_dir, /* project_trusted */ true);

    REQUIRE(manager.errors().empty());
    REQUIRE(manager.settings().compaction.has_value());
    // Mistyped fields are ignored, leaving them absent (the pi default at
    // resolution); the valid field is kept.
    CHECK_FALSE(manager.settings().compaction->enabled.has_value());
    CHECK_FALSE(manager.settings().compaction->reserve_tokens.has_value());
    CHECK(manager.settings().compaction->keep_recent_tokens == 10000);
}

TEST_CASE("SettingsManager rejects a non-object compaction field", "[settings][two-scope][issue359][spec]") {
    SettingsDirs dirs;
    dirs.write_global(R"({"compaction": "enabled"})");

    auto manager = coding_agent::SettingsManager::create(
        dirs.cwd, dirs.agent_dir, /* project_trusted */ true);

    REQUIRE_FALSE(manager.errors().empty());
    CHECK(manager.errors()[0].message.find("compaction") != std::string::npos);
}

TEST_CASE("SettingsManager loads and deep-merges the retry settings object", "[settings][two-scope][issue361][spec]") {
    SettingsDirs dirs;
    dirs.write_global(R"({
        "retry": { "enabled": false, "maxRetries": 5 }
    })");
    dirs.write_project(R"({
        "retry": { "baseDelayMs": 100 }
    })");

    auto manager = coding_agent::SettingsManager::create(
        dirs.cwd, dirs.agent_dir, /* project_trusted */ true);

    REQUIRE(manager.errors().empty());
    REQUIRE(manager.settings().retry.has_value());
    // Per-field deep merge: the project scope wins per field, global fields
    // the project scope does not set are kept.
    CHECK(manager.settings().retry->enabled == false);
    CHECK(manager.settings().retry->max_retries == 5);
    CHECK(manager.settings().retry->base_delay_ms == 100);
    // The project scope alone carries only its own field.
    REQUIRE(manager.project_settings().retry.has_value());
    CHECK_FALSE(manager.project_settings().retry->enabled.has_value());
    CHECK_FALSE(manager.project_settings().retry->max_retries.has_value());
    CHECK(manager.project_settings().retry->base_delay_ms == 100);
}

TEST_CASE("SettingsManager retry fields are optional and mistyped values fall back to defaults",
        "[settings][two-scope][issue361][spec]") {
    SettingsDirs dirs;
    dirs.write_global(R"({
        "retry": { "enabled": "yes", "maxRetries": -1, "baseDelayMs": 500 }
    })");

    auto manager = coding_agent::SettingsManager::create(
        dirs.cwd, dirs.agent_dir, /* project_trusted */ true);

    REQUIRE(manager.errors().empty());
    REQUIRE(manager.settings().retry.has_value());
    // Mistyped fields are ignored, leaving them absent (the pi default at
    // resolution); the valid field is kept.
    CHECK_FALSE(manager.settings().retry->enabled.has_value());
    CHECK_FALSE(manager.settings().retry->max_retries.has_value());
    CHECK(manager.settings().retry->base_delay_ms == 500);
}

TEST_CASE("SettingsManager rejects a non-object retry field", "[settings][two-scope][issue361][spec]") {
    SettingsDirs dirs;
    dirs.write_global(R"({"retry": "enabled"})");

    auto manager = coding_agent::SettingsManager::create(
        dirs.cwd, dirs.agent_dir, /* project_trusted */ true);

    REQUIRE_FALSE(manager.errors().empty());
    CHECK(manager.errors()[0].message.find("retry") != std::string::npos);
}

TEST_CASE("SettingsManager loads the graduated render settings with pi defaults",
        "[settings][two-scope][issue408][spec]") {
    SettingsDirs dirs;
    dirs.write_global(R"({
        "hideThinkingBlock": true,
        "outputPad": 0
    })");

    auto manager = coding_agent::SettingsManager::create(
        dirs.cwd, dirs.agent_dir, /* project_trusted */ true);

    REQUIRE(manager.errors().empty());
    const auto& settings = manager.settings();
    REQUIRE(settings.hide_thinking_block.has_value());
    CHECK(*settings.hide_thinking_block == true);
    REQUIRE(settings.output_pad.has_value());
    CHECK(*settings.output_pad == 0);
    // Resolved accessors carry the pi defaults when the fields are absent.
    SettingsDirs empty_dirs;
    auto defaults = coding_agent::SettingsManager::create(
        empty_dirs.cwd, empty_dirs.agent_dir, /* project_trusted */ true);
    CHECK_FALSE(defaults.hide_thinking_block());
    CHECK(defaults.output_pad() == 1);
}

TEST_CASE("SettingsManager outputPad resolves every non-zero value as 1 like pi",
        "[settings][two-scope][issue408][spec]") {
    SettingsDirs dirs;
    dirs.write_global(R"({"outputPad": 7})");

    auto manager = coding_agent::SettingsManager::create(
        dirs.cwd, dirs.agent_dir, /* project_trusted */ true);

    REQUIRE(manager.errors().empty());
    // pi `getOutputPad`: `settings.outputPad === 0 ? 0 : 1`.
    CHECK(manager.settings().output_pad == 1);
    CHECK(manager.output_pad() == 1);
}

TEST_CASE("SettingsManager deep-merges the graduated render settings with the project scope winning",
        "[settings][two-scope][issue408][spec]") {
    SettingsDirs dirs;
    dirs.write_global(R"({
        "hideThinkingBlock": true,
        "outputPad": 0
    })");
    dirs.write_project(R"({
        "hideThinkingBlock": false
    })");

    auto manager = coding_agent::SettingsManager::create(
        dirs.cwd, dirs.agent_dir, /* project_trusted */ true);

    REQUIRE(manager.errors().empty());
    // Project scope wins per field; fields the project scope does not set
    // keep the global scope value.
    CHECK(manager.hide_thinking_block() == false);
    CHECK(manager.output_pad() == 0);
}

TEST_CASE("SettingsManager hideThinkingBlock write is a surgical global-scope merge",
        "[settings][two-scope][write][issue408][spec]") {
    SettingsDirs dirs;
    dirs.write_global(R"({"theme":"dark","future":true})");

    auto manager = coding_agent::SettingsManager::create(
        dirs.cwd, dirs.agent_dir, /* project_trusted */ true);
    auto saved = manager.set_hide_thinking_block(true);
    REQUIRE(saved);

    CHECK(manager.hide_thinking_block() == true);
    CHECK(manager.settings().theme == "dark");
    const auto content = dirs.workspace.read("agent/settings.json");
    CHECK(content.find("\"hideThinkingBlock\": true") != std::string::npos);
    // Unknown and unmodified fields survive the surgical write.
    CHECK(content.find("\"future\": true") != std::string::npos);

    // Reloading re-reads the persisted value (survives persistence).
    auto reloaded = coding_agent::SettingsManager::create(
        dirs.cwd, dirs.agent_dir, /* project_trusted */ true);
    CHECK(reloaded.hide_thinking_block() == true);
}

TEST_CASE("SettingsManager hideThinkingBlock write creates the file and is a no-op when unchanged",
        "[settings][two-scope][write][issue408][spec]") {
    SettingsDirs dirs;

    auto manager = coding_agent::SettingsManager::create(
        dirs.cwd, dirs.agent_dir, /* project_trusted */ true);
    REQUIRE(manager.set_hide_thinking_block(true));
    const auto content = dirs.workspace.read("agent/settings.json");
    CHECK(content.find("\"hideThinkingBlock\": true") != std::string::npos);

    // The unchanged value is a no-op: the file is not rewritten.
    const auto before = dirs.workspace.read("agent/settings.json");
    REQUIRE(manager.set_hide_thinking_block(true));
    CHECK(dirs.workspace.read("agent/settings.json") == before);
}

TEST_CASE("SettingsManager outputPad write is a surgical global-scope merge and validates the value",
        "[settings][two-scope][write][issue408][spec]") {
    SettingsDirs dirs;
    dirs.write_global(R"({"theme":"dark"})");

    auto manager = coding_agent::SettingsManager::create(
        dirs.cwd, dirs.agent_dir, /* project_trusted */ true);
    REQUIRE(manager.set_output_pad(0));
    CHECK(manager.output_pad() == 0);
    CHECK(manager.settings().theme == "dark");
    const auto content = dirs.workspace.read("agent/settings.json");
    CHECK(content.find("\"outputPad\": 0") != std::string::npos);
    CHECK(content.find("\"theme\": \"dark\"") != std::string::npos);

    auto reloaded = coding_agent::SettingsManager::create(
        dirs.cwd, dirs.agent_dir, /* project_trusted */ true);
    CHECK(reloaded.output_pad() == 0);

    // Only 0 and 1 exist (pi `outputPad: 0 | 1`).
    auto rejected = manager.set_output_pad(2);
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().message.find("outputPad") != std::string::npos);
    CHECK(manager.output_pad() == 0);
}

TEST_CASE("SettingsManager render-setting writes suppress on a global load failure",
        "[settings][two-scope][write][error][issue408][spec]") {
    SettingsDirs dirs;
    dirs.write_global("{");

    auto manager = coding_agent::SettingsManager::create(
        dirs.cwd, dirs.agent_dir, /* project_trusted */ true);
    REQUIRE_FALSE(manager.errors().empty());
    // A scope whose load failed suppresses writes to that scope (pi).
    REQUIRE(manager.set_hide_thinking_block(true));
    REQUIRE(manager.set_output_pad(0));
    CHECK_FALSE(manager.hide_thinking_block());
    CHECK(manager.output_pad() == 1);
}

TEST_CASE("SettingsManager defaultProjectTrust write is a surgical global-scope merge",
        "[settings][two-scope][write][issue408][spec]") {
    SettingsDirs dirs;
    dirs.write_global(R"({"theme":"dark","future":true})");

    auto manager = coding_agent::SettingsManager::create(
        dirs.cwd, dirs.agent_dir, /* project_trusted */ true);
    REQUIRE(manager.set_default_project_trust(coding_agent::DefaultProjectTrust::Never));
    REQUIRE(manager.default_project_trust().has_value());
    CHECK(*manager.default_project_trust() == coding_agent::DefaultProjectTrust::Never);
    CHECK(manager.settings().theme == "dark");
    const auto content = dirs.workspace.read("agent/settings.json");
    CHECK(content.find("\"defaultProjectTrust\": \"never\"") != std::string::npos);
    CHECK(content.find("\"future\": true") != std::string::npos);

    auto reloaded = coding_agent::SettingsManager::create(
        dirs.cwd, dirs.agent_dir, /* project_trusted */ true);
    REQUIRE(reloaded.default_project_trust().has_value());
    CHECK(*reloaded.default_project_trust() == coding_agent::DefaultProjectTrust::Never);
}

TEST_CASE(
        "SettingsManager loads enableSkillCommands with the pi default true", "[settings][two-scope][issue412][spec]") {
    SettingsDirs dirs;
    dirs.write_global(R"({"enableSkillCommands": false})");

    auto manager = coding_agent::SettingsManager::create(
        dirs.cwd, dirs.agent_dir, /* project_trusted */ true);

    REQUIRE(manager.errors().empty());
    // pi `getEnableSkillCommands`: `settings.enableSkillCommands ?? true`.
    CHECK(manager.settings().enable_skill_commands == false);
    CHECK(manager.get_enable_skill_commands() == false);

    SettingsDirs empty_dirs;
    auto defaults = coding_agent::SettingsManager::create(
        empty_dirs.cwd, empty_dirs.agent_dir, /* project_trusted */ true);
    CHECK(defaults.get_enable_skill_commands() == true);
}

TEST_CASE("SettingsManager deep-merges enableSkillCommands with the project scope winning",
        "[settings][two-scope][issue412][spec]") {
    SettingsDirs dirs;
    dirs.write_global(R"({"enableSkillCommands": true})");
    dirs.write_project(R"({"enableSkillCommands": false})");

    auto manager = coding_agent::SettingsManager::create(
        dirs.cwd, dirs.agent_dir, /* project_trusted */ true);

    REQUIRE(manager.errors().empty());
    CHECK(manager.get_enable_skill_commands() == false);
}

TEST_CASE("SettingsManager enableSkillCommands write is a surgical global-scope merge",
        "[settings][two-scope][write][issue412][spec]") {
    SettingsDirs dirs;
    dirs.write_global(R"({"theme":"dark","future":true})");

    auto manager = coding_agent::SettingsManager::create(
        dirs.cwd, dirs.agent_dir, /* project_trusted */ true);
    auto saved = manager.set_enable_skill_commands(false);
    REQUIRE(saved);

    CHECK(manager.get_enable_skill_commands() == false);
    CHECK(manager.settings().theme == "dark");
    const auto content = dirs.workspace.read("agent/settings.json");
    CHECK(content.find("\"enableSkillCommands\": false") != std::string::npos);
    // Unknown and unmodified fields survive the surgical write.
    CHECK(content.find("\"future\": true") != std::string::npos);

    // Reloading re-reads the persisted value (survives persistence).
    auto reloaded = coding_agent::SettingsManager::create(
        dirs.cwd, dirs.agent_dir, /* project_trusted */ true);
    CHECK(reloaded.get_enable_skill_commands() == false);
}

TEST_CASE("SettingsManager enableSkillCommands write is a no-op when unchanged",
        "[settings][two-scope][write][issue412][spec]") {
    SettingsDirs dirs;
    dirs.write_global(R"({"enableSkillCommands": true})");

    auto manager = coding_agent::SettingsManager::create(
        dirs.cwd, dirs.agent_dir, /* project_trusted */ true);
    const auto before = dirs.workspace.read("agent/settings.json");
    REQUIRE(manager.set_enable_skill_commands(true));
    CHECK(dirs.workspace.read("agent/settings.json") == before);
}

TEST_CASE("SettingsManager getOrCreateDeviceId creates persists and reuses a UUIDv4",
        "[settings][two-scope][write][issue862][spec]") {
    SettingsDirs dirs;
    dirs.write_global(R"({"theme": "dark"})");

    auto manager = coding_agent::SettingsManager::create(
        dirs.cwd, dirs.agent_dir, /* project_trusted */ true);
    const auto device_id = manager.get_or_create_device_id();
    REQUIRE(device_id.size() == 36);
    CHECK(device_id[8] == '-');
    CHECK(device_id[13] == '-');
    CHECK(device_id[14] == '4');
    CHECK(device_id[18] == '-');
    CHECK((device_id[19] == '8' || device_id[19] == '9' || device_id[19] == 'a' || device_id[19] == 'b'));

    // Same manager returns the same ID; the global file now carries it while
    // unmodified and unknown fields survive.
    CHECK(manager.get_or_create_device_id() == device_id);
    const auto content = dirs.workspace.read("agent/settings.json");
    CHECK(content.find("\"deviceId\": \"" + device_id + "\"") != std::string::npos);
    CHECK(content.find("\"theme\": \"dark\"") != std::string::npos);

    // A fresh manager reloads the persisted ID.
    auto reloaded = coding_agent::SettingsManager::create(
        dirs.cwd, dirs.agent_dir, /* project_trusted */ true);
    CHECK(reloaded.get_or_create_device_id() == device_id);
}

TEST_CASE("SettingsManager getOrCreateDeviceId ignores a committed project-scope deviceId",
        "[settings][two-scope][write][issue862][spec]") {
    SettingsDirs dirs;
    dirs.write_project(R"({"deviceId": "123e4567-e89b-42d3-a456-426614174000"})");

    auto manager = coding_agent::SettingsManager::create(
        dirs.cwd, dirs.agent_dir, /* project_trusted */ true);
    const auto device_id = manager.get_or_create_device_id();
    CHECK(device_id != "123e4567-e89b-42d3-a456-426614174000");
    // The write landed in the global scope only.
    CHECK(dirs.workspace.read("agent/settings.json").find("\"deviceId\"") != std::string::npos);
    CHECK(dirs.workspace.read(".pi/settings.json").find("\"deviceId\": \"" + device_id + "\"") ==
            std::string::npos);
}

TEST_CASE("SettingsManager resolves pi's codemode mode and inline budget",
        "[settings][two-scope][codemode][issue885][spec]") {
    SettingsDirs dirs;
    auto empty = coding_agent::SettingsManager::create(dirs.cwd, dirs.agent_dir, /* project_trusted */ true);
    // pi `getCodemodeMode` default: "on"; pi `getCodemodeInlineBudget`: unset
    // (the caller falls back to DEFAULT_CODEMODE_INLINE_BUDGET = 3000).
    CHECK(empty.codemode_mode() == "on");
    CHECK_FALSE(empty.codemode_inline_budget().has_value());

    dirs.write_global(R"({"codemode": {"mode": "only", "inlineBudget": 1200}})");
    auto global = coding_agent::SettingsManager::create(dirs.cwd, dirs.agent_dir, /* project_trusted */ true);
    CHECK(global.codemode_mode() == "only");
    REQUIRE(global.codemode_inline_budget().has_value());
    CHECK(*global.codemode_inline_budget() == 1200);

    // Project scope wins per field, pi deep-merge semantics.
    dirs.write_project(R"({"codemode": {"mode": "on"}})");
    auto project = coding_agent::SettingsManager::create(dirs.cwd, dirs.agent_dir, /* project_trusted */ true);
    CHECK(project.codemode_mode() == "on");
    REQUIRE(project.codemode_inline_budget().has_value());
    CHECK(*project.codemode_inline_budget() == 1200);

    // An unknown mode is ignored rather than rejected, so a newer pi setting
    // cannot break the load and resolution falls back to "on".
    SettingsDirs other;
    other.write_global(R"({"codemode": {"mode": "sometimes"}})");
    auto unknown = coding_agent::SettingsManager::create(other.cwd, other.agent_dir, /* project_trusted */ true);
    CHECK(unknown.errors().empty());
    CHECK(unknown.codemode_mode() == "on");
}

TEST_CASE("SettingsManager resolves tuiMode with the frozen unset default fullscreen",
        "[settings][two-scope][issue1017][issue1024][spec]") {
    SettingsDirs dirs;
    dirs.write_global(R"({"theme":"dark"})");
    auto manager = coding_agent::SettingsManager::create(dirs.cwd, dirs.agent_dir, /* project_trusted */ true);

    // #1024 (T78) flipped the unset default: the resolution now matches the
    // frozen `getTuiMode` behavior, fullscreen.
    CHECK_FALSE(manager.settings().tui_mode.has_value());
    CHECK(manager.get_tui_mode() == coding_agent::TuiMode::Fullscreen);

    for (const auto& [stored, expected] : std::array{
                 std::pair<std::string_view, coding_agent::TuiMode>{"regular", coding_agent::TuiMode::Regular},
                 std::pair<std::string_view, coding_agent::TuiMode>{"fullscreen", coding_agent::TuiMode::Fullscreen},
                 // pi does not validate the stored value; resolution normalizes
                 // everything that is not "regular" to fullscreen.
                 std::pair<std::string_view, coding_agent::TuiMode>{"bogus", coding_agent::TuiMode::Fullscreen},
         }) {
        dirs.write_global(std::format(R"({{"tuiMode":"{}"}})", stored));
        auto scoped = coding_agent::SettingsManager::create(dirs.cwd, dirs.agent_dir, /* project_trusted */ true);
        CHECK(scoped.errors().empty());
        CHECK(scoped.get_tui_mode() == expected);
    }
}

TEST_CASE("SettingsManager tuiMode write is a surgical global-scope merge and a no-op when unchanged",
        "[settings][two-scope][write][issue1017][spec]") {
    SettingsDirs dirs;
    dirs.write_global(R"({"theme":"dark","future":true})");

    auto manager = coding_agent::SettingsManager::create(dirs.cwd, dirs.agent_dir, /* project_trusted */ true);
    REQUIRE(manager.set_tui_mode(coding_agent::TuiMode::Fullscreen));

    CHECK(manager.get_tui_mode() == coding_agent::TuiMode::Fullscreen);
    CHECK(manager.settings().theme == "dark");
    const auto content = dirs.workspace.read("agent/settings.json");
    CHECK(content.find("\"tuiMode\": \"fullscreen\"") != std::string::npos);
    // Unknown and unmodified fields survive the surgical write.
    CHECK(content.find("\"future\": true") != std::string::npos);

    auto reloaded = coding_agent::SettingsManager::create(dirs.cwd, dirs.agent_dir, /* project_trusted */ true);
    CHECK(reloaded.get_tui_mode() == coding_agent::TuiMode::Fullscreen);

    // The unchanged value is a no-op: the file is not rewritten.
    const auto before = dirs.workspace.read("agent/settings.json");
    REQUIRE(manager.set_tui_mode(coding_agent::TuiMode::Fullscreen));
    CHECK(dirs.workspace.read("agent/settings.json") == before);

    // Writing regular over an unset key persists it (pi `setTuiMode` writes the
    // key rather than relying on the resolution default).
    SettingsDirs empty;
    auto unset = coding_agent::SettingsManager::create(empty.cwd, empty.agent_dir, /* project_trusted */ true);
    REQUIRE(unset.set_tui_mode(coding_agent::TuiMode::Regular));
    CHECK(empty.workspace.read("agent/settings.json").find("\"tuiMode\": \"regular\"") != std::string::npos);
}

TEST_CASE("SettingsManager tuiMode write suppresses on a global load failure",
        "[settings][two-scope][write][error][issue1017][spec]") {
    SettingsDirs dirs;
    dirs.write_global("{");

    auto manager = coding_agent::SettingsManager::create(dirs.cwd, dirs.agent_dir, /* project_trusted */ true);
    REQUIRE_FALSE(manager.errors().empty());
    REQUIRE(manager.set_tui_mode(coding_agent::TuiMode::Fullscreen));
    // A scope whose load failed suppresses writes to that scope (pi), and the
    // in-memory view does not advance either: the resolution falls back to the
    // #1024 fullscreen default.
    CHECK(manager.get_tui_mode() == coding_agent::TuiMode::Fullscreen);
}

TEST_CASE("SettingsManager loads showHardwareCursor with explicit false vs env fallback",
        "[settings][two-scope][issue1037][spec]") {
    tests::EnvVarGuard guard{"PI_HARDWARE_CURSOR"};

    // 1. Env unset, empty settings => default false
    guard.unset();
    SettingsDirs empty_dirs;
    auto manager =
            coding_agent::SettingsManager::create(empty_dirs.cwd, empty_dirs.agent_dir, /* project_trusted */ true);
    REQUIRE(manager.errors().empty());
    CHECK(manager.settings().show_hardware_cursor == std::nullopt);
    CHECK(manager.get_show_hardware_cursor() == false);

    // 2. Env "1", empty settings => fallback true
    guard.set("1");
    CHECK(manager.get_show_hardware_cursor() == true);

    // 3. Env "0", "true", "yes", "" => fallback false
    guard.set("0");
    CHECK(manager.get_show_hardware_cursor() == false);
    guard.set("true");
    CHECK(manager.get_show_hardware_cursor() == false);
    guard.set("yes");
    CHECK(manager.get_show_hardware_cursor() == false);
    guard.set("");
    CHECK(manager.get_show_hardware_cursor() == false);

    // 4. Explicit false wins over env "1"
    guard.set("1");
    SettingsDirs false_dirs;
    false_dirs.write_global(R"({"showHardwareCursor": false})");
    auto false_manager =
            coding_agent::SettingsManager::create(false_dirs.cwd, false_dirs.agent_dir, /* project_trusted */ true);
    REQUIRE(false_manager.errors().empty());
    CHECK(false_manager.settings().show_hardware_cursor == false);
    CHECK(false_manager.get_show_hardware_cursor() == false);

    // 5. Explicit true wins over env "0"
    guard.set("0");
    SettingsDirs true_dirs;
    true_dirs.write_global(R"({"showHardwareCursor": true})");
    auto true_manager =
            coding_agent::SettingsManager::create(true_dirs.cwd, true_dirs.agent_dir, /* project_trusted */ true);
    REQUIRE(true_manager.errors().empty());
    CHECK(true_manager.settings().show_hardware_cursor == true);
    CHECK(true_manager.get_show_hardware_cursor() == true);
}

TEST_CASE("SettingsManager deep-merges showHardwareCursor with the project scope winning",
        "[settings][two-scope][issue1037][spec]") {
    tests::EnvVarGuard guard{"PI_HARDWARE_CURSOR"};
    guard.unset();

    // 1. Global true, Project false (trusted) => Project false wins
    SettingsDirs dirs1;
    dirs1.write_global(R"({"showHardwareCursor": true})");
    dirs1.write_project(R"({"showHardwareCursor": false})");
    auto manager1 = coding_agent::SettingsManager::create(dirs1.cwd, dirs1.agent_dir, /* project_trusted */ true);
    REQUIRE(manager1.errors().empty());
    CHECK(manager1.get_show_hardware_cursor() == false);

    // 2. Global false, Project true (trusted) => Project true wins
    SettingsDirs dirs2;
    dirs2.write_global(R"({"showHardwareCursor": false})");
    dirs2.write_project(R"({"showHardwareCursor": true})");
    auto manager2 = coding_agent::SettingsManager::create(dirs2.cwd, dirs2.agent_dir, /* project_trusted */ true);
    REQUIRE(manager2.errors().empty());
    CHECK(manager2.get_show_hardware_cursor() == true);

    // 3. Global true, Project false (untrusted) => Global true wins (untrusted ignored)
    SettingsDirs dirs3;
    dirs3.write_global(R"({"showHardwareCursor": true})");
    dirs3.write_project(R"({"showHardwareCursor": false})");
    auto manager3 = coding_agent::SettingsManager::create(dirs3.cwd, dirs3.agent_dir, /* project_trusted */ false);
    REQUIRE(manager3.errors().empty());
    CHECK(manager3.get_show_hardware_cursor() == true);

    // 4. Global unset, Project false (trusted), env "1" => Project false wins over env
    guard.set("1");
    SettingsDirs dirs4;
    dirs4.write_project(R"({"showHardwareCursor": false})");
    auto manager4 = coding_agent::SettingsManager::create(dirs4.cwd, dirs4.agent_dir, /* project_trusted */ true);
    REQUIRE(manager4.errors().empty());
    CHECK(manager4.get_show_hardware_cursor() == false);
}

TEST_CASE("SettingsManager showHardwareCursor write is a surgical global-scope merge",
        "[settings][two-scope][write][issue1037][spec]") {
    tests::EnvVarGuard guard{"PI_HARDWARE_CURSOR"};
    guard.unset();

    SettingsDirs dirs;
    dirs.write_global(R"({"theme":"dark","future":true})");

    auto manager = coding_agent::SettingsManager::create(dirs.cwd, dirs.agent_dir, /* project_trusted */ true);
    REQUIRE(manager.errors().empty());
    CHECK(manager.get_show_hardware_cursor() == false);

    auto saved = manager.set_show_hardware_cursor(true);
    REQUIRE(saved);

    CHECK(manager.get_show_hardware_cursor() == true);
    CHECK(manager.settings().theme == "dark");
    const auto content = dirs.workspace.read("agent/settings.json");
    CHECK(content.find("\"showHardwareCursor\": true") != std::string::npos);
    CHECK(content.find("\"future\": true") != std::string::npos);

    // Reloading re-reads the persisted value
    auto reloaded = coding_agent::SettingsManager::create(dirs.cwd, dirs.agent_dir, /* project_trusted */ true);
    CHECK(reloaded.get_show_hardware_cursor() == true);

    // The unchanged value is a no-op
    const auto before = dirs.workspace.read("agent/settings.json");
    REQUIRE(manager.set_show_hardware_cursor(true));
    CHECK(dirs.workspace.read("agent/settings.json") == before);
}

TEST_CASE("SettingsManager resolves terminal.showTerminalProgress default false and merges project scope",
        "[settings][two-scope][terminal-progress][issue1037][spec]") {
    // 1. Missing setting => default false
    SettingsDirs empty_dirs;
    auto manager =
            coding_agent::SettingsManager::create(empty_dirs.cwd, empty_dirs.agent_dir, /* project_trusted */ true);
    REQUIRE(manager.errors().empty());
    CHECK(manager.settings().show_terminal_progress == std::nullopt);
    CHECK(manager.get_show_terminal_progress() == false);

    // 2. Global true, Project unset => true
    SettingsDirs dirs_gt;
    dirs_gt.write_global(R"({"terminal": {"showTerminalProgress": true}})");
    auto manager_gt = coding_agent::SettingsManager::create(dirs_gt.cwd, dirs_gt.agent_dir, /* project_trusted */ true);
    REQUIRE(manager_gt.errors().empty());
    CHECK(manager_gt.settings().show_terminal_progress == true);
    CHECK(manager_gt.get_show_terminal_progress() == true);

    // 3. Global true, Project false (trusted) => Project false wins
    SettingsDirs dirs_pf;
    dirs_pf.write_global(R"({"terminal": {"showTerminalProgress": true}})");
    dirs_pf.write_project(R"({"terminal": {"showTerminalProgress": false}})");
    auto manager_pf = coding_agent::SettingsManager::create(dirs_pf.cwd, dirs_pf.agent_dir, /* project_trusted */ true);
    REQUIRE(manager_pf.errors().empty());
    CHECK(manager_pf.get_show_terminal_progress() == false);

    // 4. Global false, Project true (trusted) => Project true wins
    SettingsDirs dirs_pt;
    dirs_pt.write_global(R"({"terminal": {"showTerminalProgress": false}})");
    dirs_pt.write_project(R"({"terminal": {"showTerminalProgress": true}})");
    auto manager_pt = coding_agent::SettingsManager::create(dirs_pt.cwd, dirs_pt.agent_dir, /* project_trusted */ true);
    REQUIRE(manager_pt.errors().empty());
    CHECK(manager_pt.get_show_terminal_progress() == true);

    // 5. Global true, Project false (untrusted) => Global true wins
    SettingsDirs dirs_untrusted;
    dirs_untrusted.write_global(R"({"terminal": {"showTerminalProgress": true}})");
    dirs_untrusted.write_project(R"({"terminal": {"showTerminalProgress": false}})");
    auto manager_untrusted = coding_agent::SettingsManager::create(
            dirs_untrusted.cwd, dirs_untrusted.agent_dir, /* project_trusted */ false);
    REQUIRE(manager_untrusted.errors().empty());
    CHECK(manager_untrusted.get_show_terminal_progress() == true);

    // 6. Global unset, Project true (trusted) => Project true wins
    SettingsDirs dirs_proj_only;
    dirs_proj_only.write_project(R"({"terminal": {"showTerminalProgress": true}})");
    auto manager_proj_only = coding_agent::SettingsManager::create(
            dirs_proj_only.cwd, dirs_proj_only.agent_dir, /* project_trusted */ true);
    REQUIRE(manager_proj_only.errors().empty());
    CHECK(manager_proj_only.get_show_terminal_progress() == true);

    // 7. Global unset, Project true (untrusted) => Global default (false) wins
    SettingsDirs dirs_proj_untrusted;
    dirs_proj_untrusted.write_project(R"({"terminal": {"showTerminalProgress": true}})");
    auto manager_proj_untrusted = coding_agent::SettingsManager::create(
            dirs_proj_untrusted.cwd, dirs_proj_untrusted.agent_dir, /* project_trusted */ false);
    REQUIRE(manager_proj_untrusted.errors().empty());
    CHECK(manager_proj_untrusted.get_show_terminal_progress() == false);
}

TEST_CASE("SettingsManager set_show_terminal_progress performs surgical global write into nested terminal object",
        "[settings][two-scope][write][terminal-progress][issue1037][spec]") {
    SettingsDirs dirs;
    dirs.write_global(R"({"theme":"dark","terminal":{"clearOnShrink":true,"unknownField":"preserved"}})");

    auto manager = coding_agent::SettingsManager::create(dirs.cwd, dirs.agent_dir, /* project_trusted */ true);
    REQUIRE(manager.errors().empty());
    CHECK(manager.get_show_terminal_progress() == false);

    auto saved = manager.set_show_terminal_progress(true);
    REQUIRE(saved);

    CHECK(manager.get_show_terminal_progress() == true);
    CHECK(manager.settings().theme == "dark");
    const auto content = dirs.workspace.read("agent/settings.json");
    CHECK(content.find("\"showTerminalProgress\": true") != std::string::npos);
    CHECK(content.find("\"clearOnShrink\": true") != std::string::npos);
    CHECK(content.find("\"unknownField\": \"preserved\"") != std::string::npos);
    CHECK(content.find("\"theme\": \"dark\"") != std::string::npos);

    // Reloading re-reads the persisted value
    auto reloaded = coding_agent::SettingsManager::create(dirs.cwd, dirs.agent_dir, /* project_trusted */ true);
    CHECK(reloaded.get_show_terminal_progress() == true);

    // Changing to false updates the nested field while preserving siblings
    auto saved_false = manager.set_show_terminal_progress(false);
    REQUIRE(saved_false);
    CHECK(manager.get_show_terminal_progress() == false);
    const auto content_false = dirs.workspace.read("agent/settings.json");
    CHECK(content_false.find("\"showTerminalProgress\": false") != std::string::npos);
    CHECK(content_false.find("\"clearOnShrink\": true") != std::string::npos);
    CHECK(content_false.find("\"unknownField\": \"preserved\"") != std::string::npos);

    // The unchanged value is a no-op
    const auto before = dirs.workspace.read("agent/settings.json");
    REQUIRE(manager.set_show_terminal_progress(false));
    CHECK(dirs.workspace.read("agent/settings.json") == before);
}

TEST_CASE("SettingsManager resolves the fullscreen settings pi defaults", "[coding_agent][settings][issue1020][spec]") {
    SettingsDirs dirs;
    auto manager = coding_agent::SettingsManager::create(dirs.cwd, dirs.agent_dir, /* project_trusted */ true);

    REQUIRE(manager.errors().empty());
    CHECK_FALSE(manager.settings().fullscreen_scrollbar.has_value());
    CHECK_FALSE(manager.settings().fullscreen_copy_on_select.has_value());
    CHECK_FALSE(manager.settings().fullscreen_wheel_scroll_lines.has_value());
    // pi `getFullscreenScrollbar`/`getFullscreenCopyOnSelect`/
    // `getFullscreenWheelScrollLines` defaults: auto/true/auto.
    CHECK(manager.get_fullscreen_scrollbar() == coding_agent::FullscreenScrollbar::Auto);
    CHECK(manager.get_fullscreen_copy_on_select());
    CHECK(std::holds_alternative<coding_agent::AutoWheelScrollLines>(manager.get_fullscreen_wheel_scroll_lines()));
}

TEST_CASE("SettingsManager resolves fullscreenScrollbar and keeps invalid project values shadowing",
        "[coding_agent][settings][issue1020][spec]") {
    SettingsDirs dirs;
    // pi `getFullscreenScrollbar`: only the exact stored strings "always" and
    // "hidden" resolve to themselves; every other value resolves as "auto".
    const auto cases = std::array{
            std::pair<std::string_view, coding_agent::FullscreenScrollbar>{
                    "\"always\"", coding_agent::FullscreenScrollbar::Always},
            std::pair<std::string_view, coding_agent::FullscreenScrollbar>{
                    "\"hidden\"", coding_agent::FullscreenScrollbar::Hidden},
            std::pair<std::string_view, coding_agent::FullscreenScrollbar>{
                    "\"auto\"", coding_agent::FullscreenScrollbar::Auto},
            std::pair<std::string_view, coding_agent::FullscreenScrollbar>{
                    "\"bogus\"", coding_agent::FullscreenScrollbar::Auto},
            std::pair<std::string_view, coding_agent::FullscreenScrollbar>{
                    "\"\"", coding_agent::FullscreenScrollbar::Auto},
            std::pair<std::string_view, coding_agent::FullscreenScrollbar>{
                    "42", coding_agent::FullscreenScrollbar::Auto},
            std::pair<std::string_view, coding_agent::FullscreenScrollbar>{
                    "true", coding_agent::FullscreenScrollbar::Auto},
            std::pair<std::string_view, coding_agent::FullscreenScrollbar>{
                    "null", coding_agent::FullscreenScrollbar::Auto},
            std::pair<std::string_view, coding_agent::FullscreenScrollbar>{
                    "[\"always\"]", coding_agent::FullscreenScrollbar::Auto},
    };
    for (const auto& [stored, expected] : cases) {
        dirs.write_global(std::format(R"({{"fullscreenScrollbar":{}}})", stored));
        auto manager = coding_agent::SettingsManager::create(dirs.cwd, dirs.agent_dir, /* project_trusted */ true);
        REQUIRE(manager.errors().empty());
        CHECK(manager.settings().fullscreen_scrollbar == expected);
        CHECK(manager.get_fullscreen_scrollbar() == expected);
    }

    // pi's deep merge gives a present project value priority over global even
    // when it is invalid, so the invalid value resolves as "auto".
    for (const auto& [project_value, expected] : std::array{
                 std::pair<std::string_view, coding_agent::FullscreenScrollbar>{
                         "\"bogus\"", coding_agent::FullscreenScrollbar::Auto},
                 std::pair<std::string_view, coding_agent::FullscreenScrollbar>{
                         "42", coding_agent::FullscreenScrollbar::Auto},
                 std::pair<std::string_view, coding_agent::FullscreenScrollbar>{
                         "null", coding_agent::FullscreenScrollbar::Auto},
                 std::pair<std::string_view, coding_agent::FullscreenScrollbar>{
                         "\"hidden\"", coding_agent::FullscreenScrollbar::Hidden},
         }) {
        dirs.write_global(R"({"fullscreenScrollbar":"always"})");
        dirs.write_project(std::format(R"({{"fullscreenScrollbar":{}}})", project_value));
        auto manager = coding_agent::SettingsManager::create(dirs.cwd, dirs.agent_dir, /* project_trusted */ true);
        REQUIRE(manager.errors().empty());
        CHECK(manager.get_fullscreen_scrollbar() == expected);
    }
}

TEST_CASE("SettingsManager resolves fullscreenCopyOnSelect with pi's nullish and truthiness reads",
        "[coding_agent][settings][issue1020][spec]") {
    SettingsDirs dirs;
    // pi `getFullscreenCopyOnSelect` is `settings.fullscreenCopyOnSelect ??
    // true` and the AltScreen consumer keeps JavaScript truthiness, so null is
    // true while false, 0, and "" disable copy-on-select.
    const auto cases = std::array{
            std::pair<std::string_view, bool>{"true", true},
            std::pair<std::string_view, bool>{"false", false},
            std::pair<std::string_view, bool>{"1", true},
            std::pair<std::string_view, bool>{"0", false},
            std::pair<std::string_view, bool>{"\"false\"", true},
            std::pair<std::string_view, bool>{"\"0\"", true},
            std::pair<std::string_view, bool>{"\"\"", false},
            std::pair<std::string_view, bool>{"null", true},
            std::pair<std::string_view, bool>{"[]", true},
            std::pair<std::string_view, bool>{"{}", true},
    };
    for (const auto& [stored, expected] : cases) {
        dirs.write_global(std::format(R"({{"fullscreenCopyOnSelect":{}}})", stored));
        auto manager = coding_agent::SettingsManager::create(dirs.cwd, dirs.agent_dir, /* project_trusted */ true);
        REQUIRE(manager.errors().empty());
        CHECK(manager.settings().fullscreen_copy_on_select == expected);
        CHECK(manager.get_fullscreen_copy_on_select() == expected);
    }

    // A present project value wins over global under pi's deep merge, so a
    // falsy wrong-typed project value can disable copy-on-select and a nullish
    // one falls back to the true default rather than to the global value.
    for (const auto& [global_value, project_value, expected] : std::array{
                 std::tuple<std::string_view, std::string_view, bool>{"true", "0", false},
                 std::tuple<std::string_view, std::string_view, bool>{"true", "\"\"", false},
                 std::tuple<std::string_view, std::string_view, bool>{"false", "\"false\"", true},
                 std::tuple<std::string_view, std::string_view, bool>{"false", "null", true},
                 std::tuple<std::string_view, std::string_view, bool>{"true", "false", false},
         }) {
        dirs.write_global(std::format(R"({{"fullscreenCopyOnSelect":{}}})", global_value));
        dirs.write_project(std::format(R"({{"fullscreenCopyOnSelect":{}}})", project_value));
        auto manager = coding_agent::SettingsManager::create(dirs.cwd, dirs.agent_dir, /* project_trusted */ true);
        REQUIRE(manager.errors().empty());
        CHECK(manager.get_fullscreen_copy_on_select() == expected);
    }
}

TEST_CASE("SettingsManager resolves fullscreenWheelScrollLines with pi's finite floor and clamp",
        "[coding_agent][settings][issue1020][spec]") {
    SettingsDirs dirs;
    // pi `getFullscreenWheelScrollLines`: only a finite number is a line count
    // and it is floored and clamped to 1..100; everything else is "auto".
    const auto cases = std::array{
            std::pair<std::string_view, std::optional<std::size_t>>{"\"auto\"", std::nullopt},
            std::pair<std::string_view, std::optional<std::size_t>>{"1", 1},
            std::pair<std::string_view, std::optional<std::size_t>>{"5", 5},
            std::pair<std::string_view, std::optional<std::size_t>>{"2.9", 2},
            std::pair<std::string_view, std::optional<std::size_t>>{"0", 1},
            std::pair<std::string_view, std::optional<std::size_t>>{"-4", 1},
            std::pair<std::string_view, std::optional<std::size_t>>{"100", 100},
            std::pair<std::string_view, std::optional<std::size_t>>{"100.5", 100},
            std::pair<std::string_view, std::optional<std::size_t>>{"250", 100},
            std::pair<std::string_view, std::optional<std::size_t>>{"\"5\"", std::nullopt},
            std::pair<std::string_view, std::optional<std::size_t>>{"true", std::nullopt},
            std::pair<std::string_view, std::optional<std::size_t>>{"null", std::nullopt},
            std::pair<std::string_view, std::optional<std::size_t>>{"[]", std::nullopt},
    };
    for (const auto& [stored, expected] : cases) {
        dirs.write_global(std::format(R"({{"fullscreenWheelScrollLines":{}}})", stored));
        auto manager = coding_agent::SettingsManager::create(dirs.cwd, dirs.agent_dir, /* project_trusted */ true);
        REQUIRE(manager.errors().empty());
        const auto resolved = manager.get_fullscreen_wheel_scroll_lines();
        if (expected.has_value()) {
            REQUIRE(std::holds_alternative<std::size_t>(resolved));
            CHECK(std::get<std::size_t>(resolved) == *expected);
        } else {
            CHECK(std::holds_alternative<coding_agent::AutoWheelScrollLines>(resolved));
        }
    }

    // A present project value always shadows global, including wrong types and
    // values that resolve as auto.
    for (const auto& [project_value, expected] : std::array{
                 std::pair<std::string_view, std::optional<std::size_t>>{"2", 2},
                 std::pair<std::string_view, std::optional<std::size_t>>{"2.9", 2},
                 std::pair<std::string_view, std::optional<std::size_t>>{"\"5\"", std::nullopt},
                 std::pair<std::string_view, std::optional<std::size_t>>{"null", std::nullopt},
                 std::pair<std::string_view, std::optional<std::size_t>>{"\"auto\"", std::nullopt},
         }) {
        dirs.write_global(R"({"fullscreenWheelScrollLines":10})");
        dirs.write_project(std::format(R"({{"fullscreenWheelScrollLines":{}}})", project_value));
        auto manager = coding_agent::SettingsManager::create(dirs.cwd, dirs.agent_dir, /* project_trusted */ true);
        REQUIRE(manager.errors().empty());
        const auto resolved = manager.get_fullscreen_wheel_scroll_lines();
        if (expected.has_value()) {
            REQUIRE(std::holds_alternative<std::size_t>(resolved));
            CHECK(std::get<std::size_t>(resolved) == *expected);
        } else {
            CHECK(std::holds_alternative<coding_agent::AutoWheelScrollLines>(resolved));
        }
    }
}

TEST_CASE("SettingsManager reload re-reads the fullscreen settings",
        "[coding_agent][settings][issue1020][reload][spec]") {
    SettingsDirs dirs;
    dirs.write_global(R"({
        "fullscreenScrollbar":"always",
        "fullscreenCopyOnSelect":false,
        "fullscreenWheelScrollLines":10
    })");
    auto manager = coding_agent::SettingsManager::create(dirs.cwd, dirs.agent_dir, /* project_trusted */ true);
    REQUIRE(manager.errors().empty());
    CHECK(manager.get_fullscreen_scrollbar() == coding_agent::FullscreenScrollbar::Always);
    CHECK_FALSE(manager.get_fullscreen_copy_on_select());
    REQUIRE(std::holds_alternative<std::size_t>(manager.get_fullscreen_wheel_scroll_lines()));
    CHECK(std::get<std::size_t>(manager.get_fullscreen_wheel_scroll_lines()) == 10);

    // A removed key falls back to the pi default after reload.
    dirs.write_global(R"({"fullscreenWheelScrollLines":"auto"})");
    REQUIRE(manager.reload());
    CHECK(manager.get_fullscreen_scrollbar() == coding_agent::FullscreenScrollbar::Auto);
    CHECK(manager.get_fullscreen_copy_on_select());
    CHECK(std::holds_alternative<coding_agent::AutoWheelScrollLines>(manager.get_fullscreen_wheel_scroll_lines()));

    // A project value shadows global on reload too.
    dirs.write_global(R"({"fullscreenScrollbar":"always"})");
    dirs.write_project(R"({"fullscreenScrollbar":"hidden"})");
    REQUIRE(manager.reload());
    CHECK(manager.get_fullscreen_scrollbar() == coding_agent::FullscreenScrollbar::Hidden);
}

TEST_CASE("SettingsManager set_fullscreen_scrollbar writes the global scope only and is a no-op when unchanged",
        "[coding_agent][settings][issue1020][write][spec]") {
    SettingsDirs dirs;
    dirs.write_global(R"({"theme":"dark","future":true})");

    auto manager = coding_agent::SettingsManager::create(dirs.cwd, dirs.agent_dir, /* project_trusted */ true);
    REQUIRE(manager.set_fullscreen_scrollbar(coding_agent::FullscreenScrollbar::Hidden));
    CHECK(manager.get_fullscreen_scrollbar() == coding_agent::FullscreenScrollbar::Hidden);
    CHECK(manager.settings().fullscreen_scrollbar == coding_agent::FullscreenScrollbar::Hidden);
    CHECK(manager.settings().theme == "dark");
    const auto content = dirs.workspace.read("agent/settings.json");
    CHECK(content.find("\"fullscreenScrollbar\": \"hidden\"") != std::string::npos);
    // Unknown and unmodified fields survive the surgical write.
    CHECK(content.find("\"future\": true") != std::string::npos);

    auto reloaded = coding_agent::SettingsManager::create(dirs.cwd, dirs.agent_dir, /* project_trusted */ true);
    CHECK(reloaded.get_fullscreen_scrollbar() == coding_agent::FullscreenScrollbar::Hidden);

    // The unchanged value is a no-op: the file is not rewritten.
    const auto before = dirs.workspace.read("agent/settings.json");
    REQUIRE(manager.set_fullscreen_scrollbar(coding_agent::FullscreenScrollbar::Hidden));
    CHECK(dirs.workspace.read("agent/settings.json") == before);

    // The write creates a missing global file.
    SettingsDirs empty;
    auto fresh = coding_agent::SettingsManager::create(empty.cwd, empty.agent_dir, /* project_trusted */ true);
    REQUIRE(fresh.set_fullscreen_scrollbar(coding_agent::FullscreenScrollbar::Always));
    CHECK(empty.workspace.read("agent/settings.json").find("\"fullscreenScrollbar\": \"always\"") != std::string::npos);

    // pi always writes `globalSettings`, so a project value still shadows it.
    SettingsDirs shadow;
    shadow.write_global(R"({"theme":"dark"})");
    shadow.write_project(R"({"fullscreenScrollbar":"always"})");
    auto shadowed = coding_agent::SettingsManager::create(shadow.cwd, shadow.agent_dir, /* project_trusted */ true);
    REQUIRE(shadowed.set_fullscreen_scrollbar(coding_agent::FullscreenScrollbar::Hidden));
    CHECK(shadowed.get_fullscreen_scrollbar() == coding_agent::FullscreenScrollbar::Always);
    CHECK(shadow.workspace.read("agent/settings.json").find("\"fullscreenScrollbar\": \"hidden\"") !=
            std::string::npos);
}

TEST_CASE("SettingsManager set_fullscreen_copy_on_select writes the global scope only and is a no-op when unchanged",
        "[coding_agent][settings][issue1020][write][spec]") {
    SettingsDirs dirs;
    dirs.write_global(R"({"theme":"dark","future":true})");

    auto manager = coding_agent::SettingsManager::create(dirs.cwd, dirs.agent_dir, /* project_trusted */ true);
    REQUIRE(manager.set_fullscreen_copy_on_select(false));
    CHECK_FALSE(manager.get_fullscreen_copy_on_select());
    CHECK(manager.settings().fullscreen_copy_on_select == false);
    CHECK(manager.settings().theme == "dark");
    const auto content = dirs.workspace.read("agent/settings.json");
    CHECK(content.find("\"fullscreenCopyOnSelect\": false") != std::string::npos);
    CHECK(content.find("\"future\": true") != std::string::npos);

    auto reloaded = coding_agent::SettingsManager::create(dirs.cwd, dirs.agent_dir, /* project_trusted */ true);
    CHECK_FALSE(reloaded.get_fullscreen_copy_on_select());

    // The unchanged value is a no-op: the file is not rewritten.
    const auto before = dirs.workspace.read("agent/settings.json");
    REQUIRE(manager.set_fullscreen_copy_on_select(false));
    CHECK(dirs.workspace.read("agent/settings.json") == before);

    // pi always writes `globalSettings`, so a project value still shadows it.
    SettingsDirs shadow;
    shadow.write_global(R"({"theme":"dark"})");
    shadow.write_project(R"({"fullscreenCopyOnSelect":true})");
    auto shadowed = coding_agent::SettingsManager::create(shadow.cwd, shadow.agent_dir, /* project_trusted */ true);
    REQUIRE(shadowed.set_fullscreen_copy_on_select(false));
    CHECK(shadowed.get_fullscreen_copy_on_select());
    CHECK(shadow.workspace.read("agent/settings.json").find("\"fullscreenCopyOnSelect\": false") != std::string::npos);
}

TEST_CASE("SettingsManager set_fullscreen_wheel_scroll_lines clamps and persists auto",
        "[coding_agent][settings][issue1020][write][spec]") {
    SettingsDirs dirs;
    dirs.write_global(R"({"theme":"dark","future":true})");

    auto manager = coding_agent::SettingsManager::create(dirs.cwd, dirs.agent_dir, /* project_trusted */ true);
    // pi `setFullscreenWheelScrollLines` writes the number floored and clamped
    // to 1..100.
    REQUIRE(manager.set_fullscreen_wheel_scroll_lines(std::size_t{250}));
    REQUIRE(std::holds_alternative<std::size_t>(manager.get_fullscreen_wheel_scroll_lines()));
    CHECK(std::get<std::size_t>(manager.get_fullscreen_wheel_scroll_lines()) == 100);
    auto content = dirs.workspace.read("agent/settings.json");
    CHECK(content.find("\"fullscreenWheelScrollLines\": 100") != std::string::npos);
    CHECK(content.find("\"future\": true") != std::string::npos);

    REQUIRE(manager.set_fullscreen_wheel_scroll_lines(std::size_t{0}));
    REQUIRE(std::holds_alternative<std::size_t>(manager.get_fullscreen_wheel_scroll_lines()));
    CHECK(std::get<std::size_t>(manager.get_fullscreen_wheel_scroll_lines()) == 1);
    CHECK(dirs.workspace.read("agent/settings.json").find("\"fullscreenWheelScrollLines\": 1") != std::string::npos);

    // `"auto"` is persisted as the string pi writes.
    REQUIRE(manager.set_fullscreen_wheel_scroll_lines(coding_agent::AutoWheelScrollLines{}));
    CHECK(std::holds_alternative<coding_agent::AutoWheelScrollLines>(manager.get_fullscreen_wheel_scroll_lines()));
    CHECK(dirs.workspace.read("agent/settings.json").find("\"fullscreenWheelScrollLines\": \"auto\"") !=
            std::string::npos);

    // The unchanged value is a no-op: the file is not rewritten.
    const auto before = dirs.workspace.read("agent/settings.json");
    REQUIRE(manager.set_fullscreen_wheel_scroll_lines(coding_agent::AutoWheelScrollLines{}));
    CHECK(dirs.workspace.read("agent/settings.json") == before);

    auto reloaded = coding_agent::SettingsManager::create(dirs.cwd, dirs.agent_dir, /* project_trusted */ true);
    CHECK(std::holds_alternative<coding_agent::AutoWheelScrollLines>(reloaded.get_fullscreen_wheel_scroll_lines()));

    // pi always writes `globalSettings`, so a project value still shadows it.
    SettingsDirs shadow;
    shadow.write_global(R"({"fullscreenWheelScrollLines":10})");
    shadow.write_project(R"({"fullscreenWheelScrollLines":2})");
    auto shadowed = coding_agent::SettingsManager::create(shadow.cwd, shadow.agent_dir, /* project_trusted */ true);
    REQUIRE(shadowed.set_fullscreen_wheel_scroll_lines(coding_agent::AutoWheelScrollLines{}));
    REQUIRE(std::holds_alternative<std::size_t>(shadowed.get_fullscreen_wheel_scroll_lines()));
    CHECK(std::get<std::size_t>(shadowed.get_fullscreen_wheel_scroll_lines()) == 2);
    CHECK(shadow.workspace.read("agent/settings.json").find("\"fullscreenWheelScrollLines\": \"auto\"") !=
            std::string::npos);
}

TEST_CASE("SettingsManager fullscreen setting writes suppress on a global load failure",
        "[coding_agent][settings][issue1020][write][error][spec]") {
    SettingsDirs dirs;
    dirs.write_global("{");

    auto manager = coding_agent::SettingsManager::create(dirs.cwd, dirs.agent_dir, /* project_trusted */ true);
    REQUIRE_FALSE(manager.errors().empty());
    // A scope whose load failed suppresses writes to that scope (pi), and the
    // in-memory view does not advance either.
    REQUIRE(manager.set_fullscreen_scrollbar(coding_agent::FullscreenScrollbar::Hidden));
    REQUIRE(manager.set_fullscreen_copy_on_select(false));
    REQUIRE(manager.set_fullscreen_wheel_scroll_lines(std::size_t{5}));
    CHECK(manager.get_fullscreen_scrollbar() == coding_agent::FullscreenScrollbar::Auto);
    CHECK(manager.get_fullscreen_copy_on_select());
    CHECK(std::holds_alternative<coding_agent::AutoWheelScrollLines>(manager.get_fullscreen_wheel_scroll_lines()));
    CHECK_FALSE(manager.settings().fullscreen_scrollbar.has_value());
    CHECK_FALSE(manager.settings().fullscreen_copy_on_select.has_value());
    CHECK_FALSE(manager.settings().fullscreen_wheel_scroll_lines.has_value());
    CHECK(dirs.workspace.read("agent/settings.json") == "{");
}

TEST_CASE("SettingsManager set_fullscreen_scrollbar canonicalizes a non-canonical global value and then no-ops",
        "[coding_agent][settings][issue1020][write][spec]") {
    SettingsDirs dirs;
    // `"bogus"` is not the canonical wire spelling but resolves as auto; the
    // setter must rewrite the field instead of leaving the malformed value on
    // disk, while unknown siblings survive the surgical write.
    dirs.write_global(R"({"fullscreenScrollbar":"bogus","future":true,"theme":"dark"})");

    auto manager = coding_agent::SettingsManager::create(dirs.cwd, dirs.agent_dir, /* project_trusted */ true);
    REQUIRE(manager.errors().empty());
    CHECK(manager.get_fullscreen_scrollbar() == coding_agent::FullscreenScrollbar::Auto);

    REQUIRE(manager.set_fullscreen_scrollbar(coding_agent::FullscreenScrollbar::Auto));
    CHECK(dirs.workspace.read("agent/settings.json") == "{\n"
                                                        "  \"fullscreenScrollbar\": \"auto\",\n"
                                                        "  \"future\": true,\n"
                                                        "  \"theme\": \"dark\"\n"
                                                        "}\n");

    // A repeated call against the canonical wire value is a house no-op.
    const auto before = dirs.workspace.read("agent/settings.json");
    REQUIRE(manager.set_fullscreen_scrollbar(coding_agent::FullscreenScrollbar::Auto));
    CHECK(dirs.workspace.read("agent/settings.json") == before);

    // Reload recomputes the canonical wire state from the rewritten file.
    REQUIRE(manager.reload());
    REQUIRE(manager.set_fullscreen_scrollbar(coding_agent::FullscreenScrollbar::Auto));
    CHECK(dirs.workspace.read("agent/settings.json") == before);

    // A trusted project override still leaves the setter canonicalizing the
    // global wire value while the project value keeps shadowing the reads.
    SettingsDirs shadow;
    shadow.write_global(R"({"fullscreenScrollbar":"bogus"})");
    shadow.write_project(R"({"fullscreenScrollbar":"always"})");
    auto shadowed = coding_agent::SettingsManager::create(shadow.cwd, shadow.agent_dir, /* project_trusted */ true);
    REQUIRE(shadowed.errors().empty());
    CHECK(shadowed.get_fullscreen_scrollbar() == coding_agent::FullscreenScrollbar::Always);

    REQUIRE(shadowed.set_fullscreen_scrollbar(coding_agent::FullscreenScrollbar::Auto));
    CHECK(shadow.workspace.read("agent/settings.json") == "{\n  \"fullscreenScrollbar\": \"auto\"\n}\n");
    CHECK(shadow.workspace.read(".pi/settings.json") == R"({"fullscreenScrollbar":"always"})");
    CHECK(shadowed.get_fullscreen_scrollbar() == coding_agent::FullscreenScrollbar::Always);

    const auto shadow_before = shadow.workspace.read("agent/settings.json");
    REQUIRE(shadowed.set_fullscreen_scrollbar(coding_agent::FullscreenScrollbar::Auto));
    CHECK(shadow.workspace.read("agent/settings.json") == shadow_before);
}

TEST_CASE("SettingsManager set_fullscreen_copy_on_select canonicalizes a non-canonical global value and then no-ops",
        "[coding_agent][settings][issue1020][write][spec]") {
    SettingsDirs dirs;
    // The string `"false"` is truthy, so it resolves as enabled; the setter
    // must still rewrite it as the canonical JSON boolean.
    dirs.write_global(R"({"fullscreenCopyOnSelect":"false","future":true})");

    auto manager = coding_agent::SettingsManager::create(dirs.cwd, dirs.agent_dir, /* project_trusted */ true);
    REQUIRE(manager.errors().empty());
    CHECK(manager.get_fullscreen_copy_on_select());

    REQUIRE(manager.set_fullscreen_copy_on_select(true));
    CHECK(dirs.workspace.read("agent/settings.json") == "{\n"
                                                        "  \"fullscreenCopyOnSelect\": true,\n"
                                                        "  \"future\": true\n"
                                                        "}\n");

    const auto before = dirs.workspace.read("agent/settings.json");
    REQUIRE(manager.set_fullscreen_copy_on_select(true));
    CHECK(dirs.workspace.read("agent/settings.json") == before);

    // The falsy numeric spelling resolves as disabled and is rewritten as the
    // canonical boolean too.
    dirs.write_global(R"({"fullscreenCopyOnSelect":0})");
    REQUIRE(manager.reload());
    CHECK_FALSE(manager.get_fullscreen_copy_on_select());

    REQUIRE(manager.set_fullscreen_copy_on_select(false));
    CHECK(dirs.workspace.read("agent/settings.json") == "{\n  \"fullscreenCopyOnSelect\": false\n}\n");
    const auto disabled = dirs.workspace.read("agent/settings.json");
    REQUIRE(manager.set_fullscreen_copy_on_select(false));
    CHECK(dirs.workspace.read("agent/settings.json") == disabled);

    // A trusted project override keeps shadowing the reads while the setter
    // canonicalizes the global wire value.
    SettingsDirs shadow;
    shadow.write_global(R"({"fullscreenCopyOnSelect":"false"})");
    shadow.write_project(R"({"fullscreenCopyOnSelect":false})");
    auto shadowed = coding_agent::SettingsManager::create(shadow.cwd, shadow.agent_dir, /* project_trusted */ true);
    REQUIRE(shadowed.errors().empty());
    CHECK_FALSE(shadowed.get_fullscreen_copy_on_select());

    REQUIRE(shadowed.set_fullscreen_copy_on_select(true));
    CHECK(shadow.workspace.read("agent/settings.json") == "{\n  \"fullscreenCopyOnSelect\": true\n}\n");
    CHECK_FALSE(shadowed.get_fullscreen_copy_on_select());

    const auto shadow_before = shadow.workspace.read("agent/settings.json");
    REQUIRE(shadowed.set_fullscreen_copy_on_select(true));
    CHECK(shadow.workspace.read("agent/settings.json") == shadow_before);
}

TEST_CASE(
        "SettingsManager set_fullscreen_wheel_scroll_lines canonicalizes a non-canonical global value and then no-ops",
        "[coding_agent][settings][issue1020][write][spec]") {
    SettingsDirs dirs;
    // `250` resolves as the clamped line count 100; the setter must rewrite
    // the canonical clamped number.
    dirs.write_global(R"({"fullscreenWheelScrollLines":250,"future":true})");

    auto manager = coding_agent::SettingsManager::create(dirs.cwd, dirs.agent_dir, /* project_trusted */ true);
    REQUIRE(manager.errors().empty());
    REQUIRE(std::holds_alternative<std::size_t>(manager.get_fullscreen_wheel_scroll_lines()));
    CHECK(std::get<std::size_t>(manager.get_fullscreen_wheel_scroll_lines()) == 100);

    REQUIRE(manager.set_fullscreen_wheel_scroll_lines(std::size_t{100}));
    CHECK(dirs.workspace.read("agent/settings.json") == "{\n"
                                                        "  \"fullscreenWheelScrollLines\": 100,\n"
                                                        "  \"future\": true\n"
                                                        "}\n");

    const auto before = dirs.workspace.read("agent/settings.json");
    REQUIRE(manager.set_fullscreen_wheel_scroll_lines(std::size_t{100}));
    CHECK(dirs.workspace.read("agent/settings.json") == before);

    // The wrong-typed string `"5"` resolves as auto and is rewritten as the
    // canonical string.
    dirs.write_global(R"({"fullscreenWheelScrollLines":"5"})");
    REQUIRE(manager.reload());
    CHECK(std::holds_alternative<coding_agent::AutoWheelScrollLines>(manager.get_fullscreen_wheel_scroll_lines()));

    REQUIRE(manager.set_fullscreen_wheel_scroll_lines(coding_agent::AutoWheelScrollLines{}));
    CHECK(dirs.workspace.read("agent/settings.json") == "{\n  \"fullscreenWheelScrollLines\": \"auto\"\n}\n");
    const auto auto_wire = dirs.workspace.read("agent/settings.json");
    REQUIRE(manager.set_fullscreen_wheel_scroll_lines(coding_agent::AutoWheelScrollLines{}));
    CHECK(dirs.workspace.read("agent/settings.json") == auto_wire);

    // A trusted project override keeps shadowing the reads while the setter
    // canonicalizes the global wire value.
    SettingsDirs shadow;
    shadow.write_global(R"({"fullscreenWheelScrollLines":250})");
    shadow.write_project(R"({"fullscreenWheelScrollLines":5})");
    auto shadowed = coding_agent::SettingsManager::create(shadow.cwd, shadow.agent_dir, /* project_trusted */ true);
    REQUIRE(shadowed.errors().empty());
    REQUIRE(std::holds_alternative<std::size_t>(shadowed.get_fullscreen_wheel_scroll_lines()));
    CHECK(std::get<std::size_t>(shadowed.get_fullscreen_wheel_scroll_lines()) == 5);

    REQUIRE(shadowed.set_fullscreen_wheel_scroll_lines(std::size_t{100}));
    CHECK(shadow.workspace.read("agent/settings.json") == "{\n  \"fullscreenWheelScrollLines\": 100\n}\n");
    REQUIRE(std::holds_alternative<std::size_t>(shadowed.get_fullscreen_wheel_scroll_lines()));
    CHECK(std::get<std::size_t>(shadowed.get_fullscreen_wheel_scroll_lines()) == 5);

    const auto shadow_before = shadow.workspace.read("agent/settings.json");
    REQUIRE(shadowed.set_fullscreen_wheel_scroll_lines(std::size_t{100}));
    CHECK(shadow.workspace.read("agent/settings.json") == shadow_before);
}
