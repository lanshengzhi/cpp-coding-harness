#include "../../third_party/catch2/catch_test_macros.hpp"
#include "../../include/cch/coding_agent/Settings.hpp"
#include "../support/TempWorkspace.hpp"

#include <filesystem>
#include <fstream>
#include <string>

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

TEST_CASE("SettingsManager resolves empty settings without files", "[settings][two-scope]") {
    SettingsDirs dirs;
    auto manager = coding_agent::SettingsManager::create(
        dirs.cwd, dirs.agent_dir, /* project_trusted */ true);

    CHECK(manager.errors().empty());
    CHECK_FALSE(manager.settings().default_provider.has_value());
    CHECK_FALSE(manager.settings().default_model.has_value());
    CHECK_FALSE(manager.settings().theme.has_value());
    CHECK_FALSE(manager.default_project_trust().has_value());
}

TEST_CASE("SettingsManager loads the pi field subset from the global scope", "[settings][two-scope]") {
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

TEST_CASE("SettingsManager ignores legacy and unknown keys", "[settings][two-scope]") {
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

TEST_CASE("SettingsManager deep-merges with the project scope winning", "[settings][two-scope]") {
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

TEST_CASE("SettingsManager ignores defaultProjectTrust from the project scope", "[settings][two-scope][global-only]") {
    SettingsDirs dirs;
    dirs.write_global(R"({"defaultProjectTrust":"never"})");
    dirs.write_project(R"({"defaultProjectTrust":"always"})");

    auto manager = coding_agent::SettingsManager::create(
        dirs.cwd, dirs.agent_dir, /* project_trusted */ true);

    REQUIRE(manager.errors().empty());
    REQUIRE(manager.default_project_trust().has_value());
    CHECK(*manager.default_project_trust() == coding_agent::DefaultProjectTrust::Never);
}

TEST_CASE("SettingsManager loads the project scope only while trusted", "[settings][two-scope][trust]") {
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

TEST_CASE("SettingsManager records a global parse error and suppresses writes", "[settings][two-scope][error]") {
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

TEST_CASE("SettingsManager records a project parse error independently", "[settings][two-scope][error]") {
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

TEST_CASE("SettingsManager rejects an invalid defaultThinkingLevel", "[settings][two-scope][error]") {
    SettingsDirs dirs;
    dirs.write_global(R"({"defaultThinkingLevel":"sometimes"})");

    auto manager = coding_agent::SettingsManager::create(
        dirs.cwd, dirs.agent_dir, /* project_trusted */ true);

    REQUIRE(manager.errors().size() == 1);
    CHECK(manager.errors()[0].message.find("defaultThinkingLevel") != std::string::npos);
}

TEST_CASE("SettingsManager surgical theme write preserves unknown and unmodified fields", "[settings][two-scope][write]") {
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

TEST_CASE("SettingsManager surgical theme write creates a missing file", "[settings][two-scope][write]") {
    SettingsDirs dirs;
    auto manager = coding_agent::SettingsManager::create(
        dirs.cwd, dirs.agent_dir, /* project_trusted */ true);

    REQUIRE(manager.set_theme(coding_agent::SettingsScope::Global, "dark"));

    const auto content = dirs.workspace.read("agent/settings.json");
    CHECK(content.find("\"theme\": \"dark\"") != std::string::npos);
    CHECK(manager.settings().theme == "dark");
}

TEST_CASE("SettingsManager writes the project scope only when trusted", "[settings][two-scope][write][trust]") {
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

TEST_CASE("SettingsManager surgical defaultThinkingLevel write preserves unknown fields and creates the file", "[settings][two-scope][write][issue353]") {
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

TEST_CASE("SettingsManager defaultThinkingLevel write creates a missing file", "[settings][two-scope][write][issue353]") {
    SettingsDirs dirs;
    auto manager = coding_agent::SettingsManager::create(
        dirs.cwd, dirs.agent_dir, /* project_trusted */ true);

    REQUIRE(manager.set_default_thinking_level(
        coding_agent::SettingsScope::Global, "low"));

    const auto content = dirs.workspace.read("agent/settings.json");
    CHECK(content.find("\"defaultThinkingLevel\": \"low\"") != std::string::npos);
    CHECK(manager.settings().default_thinking_level == "low");
}

TEST_CASE("SettingsManager rejects an invalid defaultThinkingLevel write", "[settings][two-scope][write][issue353]") {
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

TEST_CASE("SettingsManager defaultThinkingLevel writes the project scope only when trusted", "[settings][two-scope][write][trust][issue353]") {
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

TEST_CASE("SettingsManager defaultThinkingLevel write is a no-op when unchanged", "[settings][two-scope][write][issue353]") {
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

TEST_CASE("SettingsManager applies pi read-time migrations on load and write", "[settings][two-scope][migration]") {
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

TEST_CASE("SettingsManager reload re-reads both scopes and re-records errors", "[settings][two-scope][reload]") {
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

TEST_CASE("SettingsManager treats an empty agent_dir as no global scope", "[settings][two-scope]") {
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

TEST_CASE("SettingsManager times out acquiring a held settings lock", "[settings][two-scope][lock][issue346]") {
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

TEST_CASE("SettingsManager never loads secrets or secret-reference fields", "[settings][two-scope][secret]") {
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

TEST_CASE("SettingsManager loads and deep-merges the compaction settings object", "[settings][two-scope][issue359]") {
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

TEST_CASE("SettingsManager compaction fields are optional and mistyped values fall back to defaults", "[settings][two-scope][issue359]") {
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

TEST_CASE("SettingsManager rejects a non-object compaction field", "[settings][two-scope][issue359]") {
    SettingsDirs dirs;
    dirs.write_global(R"({"compaction": "enabled"})");

    auto manager = coding_agent::SettingsManager::create(
        dirs.cwd, dirs.agent_dir, /* project_trusted */ true);

    REQUIRE_FALSE(manager.errors().empty());
    CHECK(manager.errors()[0].message.find("compaction") != std::string::npos);
}

TEST_CASE("SettingsManager loads and deep-merges the retry settings object", "[settings][two-scope][issue361]") {
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

TEST_CASE("SettingsManager retry fields are optional and mistyped values fall back to defaults", "[settings][two-scope][issue361]") {
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

TEST_CASE("SettingsManager rejects a non-object retry field", "[settings][two-scope][issue361]") {
    SettingsDirs dirs;
    dirs.write_global(R"({"retry": "enabled"})");

    auto manager = coding_agent::SettingsManager::create(
        dirs.cwd, dirs.agent_dir, /* project_trusted */ true);

    REQUIRE_FALSE(manager.errors().empty());
    CHECK(manager.errors()[0].message.find("retry") != std::string::npos);
}
