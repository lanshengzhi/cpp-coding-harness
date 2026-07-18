#include "../../third_party/catch2/catch_test_macros.hpp"
#include "../../include/cch/coding_agent/Settings.hpp"
#include "../support/TempWorkspace.hpp"

#include <cstdlib>
#include <fstream>
#include <string>

using namespace cch;

TEST_CASE("SettingsLoader loads provider and model from JSON", "[settings]") {
    tests::TempWorkspace workspace;
    auto settings_path = workspace.path() / "settings.json";
    std::ofstream(settings_path) << R"({"provider":"openai-compatible","model":"gpt-4"})";

    auto settings = coding_agent::SettingsLoader::load(settings_path);
    REQUIRE(settings);
    CHECK(settings->provider == "openai-compatible");
    CHECK(settings->model == "gpt-4");
    CHECK_FALSE(settings->base_url.has_value());
}

TEST_CASE("SettingsLoader returns defaults when file does not exist", "[settings]") {
    auto settings = coding_agent::SettingsLoader::load("/nonexistent/path/settings.json");
    REQUIRE(settings);
    CHECK_FALSE(settings->provider.has_value());
    CHECK_FALSE(settings->model.has_value());
}

TEST_CASE("SettingsLoader returns error for malformed JSON", "[settings]") {
    tests::TempWorkspace workspace;
    auto settings_path = workspace.path() / "settings.json";
    std::ofstream(settings_path) << "{not valid json";

    auto settings = coding_agent::SettingsLoader::load(settings_path);
    CHECK_FALSE(settings.has_value());
}

TEST_CASE("SettingsLoader ignores unknown keys", "[settings]") {
    tests::TempWorkspace workspace;
    auto settings_path = workspace.path() / "settings.json";
    std::ofstream(settings_path) << R"({"provider":"openai-compatible","unknown_key":true})";

    auto settings = coding_agent::SettingsLoader::load(settings_path);
    REQUIRE(settings);
    CHECK(settings->provider == "openai-compatible");
}

TEST_CASE("SettingsLoader handles api_key_env as array", "[settings]") {
    tests::TempWorkspace workspace;
    auto settings_path = workspace.path() / "settings.json";
    std::ofstream(settings_path) << R"({"api_key_env":["CUSTOM_KEY","OPENAI_API_KEY"]})";

    auto settings = coding_agent::SettingsLoader::load(settings_path);
    REQUIRE(settings);
    REQUIRE(settings->api_key_env.has_value());
    CHECK(settings->api_key_env->size() == 2);
    CHECK((*settings->api_key_env)[0] == "CUSTOM_KEY");
    CHECK((*settings->api_key_env)[1] == "OPENAI_API_KEY");
}

TEST_CASE("SettingsLoader handles api_key_env as single string", "[settings]") {
    tests::TempWorkspace workspace;
    auto settings_path = workspace.path() / "settings.json";
    std::ofstream(settings_path) << R"({"api_key_env":"OPENAI_API_KEY"})";

    auto settings = coding_agent::SettingsLoader::load(settings_path);
    REQUIRE(settings);
    REQUIRE(settings->api_key_env.has_value());
    CHECK(settings->api_key_env->size() == 1);
}

TEST_CASE("SettingsLoader resolve_api_key finds first set env var", "[settings]") {
    // Set a known env var for testing
    setenv("CCH_TEST_KEY", "test-value-123", 1);
    auto result = coding_agent::SettingsLoader::resolve_api_key({"CCH_TEST_KEY", "DOES_NOT_EXIST"});
    REQUIRE(result.has_value());
    CHECK(*result == "test-value-123");
    unsetenv("CCH_TEST_KEY");
}

TEST_CASE("SettingsLoader resolve_api_key returns nullopt when none set", "[settings]") {
    auto result = coding_agent::SettingsLoader::resolve_api_key({"DOES_NOT_EXIST_XYZ"});
    CHECK_FALSE(result.has_value());
}

TEST_CASE("SettingsLoader loads project trust defaults", "[settings][project-trust]") {
    tests::TempWorkspace workspace;
    auto settings_path = workspace.path() / "settings.json";
    std::ofstream(settings_path) << R"({"default_project_trust":"always"})";

    auto settings = coding_agent::SettingsLoader::load(settings_path);
    REQUIRE(settings);
    REQUIRE(settings->default_project_trust.has_value());
    CHECK(*settings->default_project_trust == coding_agent::DefaultProjectTrust::Always);
}

TEST_CASE("SettingsLoader loads project resource skill enablement", "[settings][project-resources]") {
    tests::TempWorkspace workspace;
    auto settings_path = workspace.path() / "settings.json";
    std::ofstream(settings_path) << R"({"project_resources":{"skills":"off"}})";

    auto settings = coding_agent::SettingsLoader::load(settings_path);
    REQUIRE(settings);
    REQUIRE(settings->project_skills.has_value());
    CHECK(*settings->project_skills == coding_agent::ResourceEnablement::Off);
}

TEST_CASE("SettingsLoader rejects invalid project trust defaults", "[settings][project-trust]") {
    tests::TempWorkspace workspace;
    auto settings_path = workspace.path() / "settings.json";
    std::ofstream(settings_path) << R"({"default_project_trust":"sometimes"})";

    auto settings = coding_agent::SettingsLoader::load(settings_path);
    CHECK_FALSE(settings.has_value());
}

TEST_CASE("SettingsLoader rejects invalid project resource values", "[settings][project-resources]") {
    tests::TempWorkspace workspace;
    auto settings_path = workspace.path() / "settings.json";
    std::ofstream(settings_path) << R"({"project_resources":{"skills":"enabled"}})";

    auto settings = coding_agent::SettingsLoader::load(settings_path);
    CHECK_FALSE(settings.has_value());
}

TEST_CASE("SettingsLoader loads the pi sessionDir preference", "[settings][session-dir]") {
    tests::TempWorkspace workspace;
    auto settings_path = workspace.path() / "settings.json";
    std::ofstream(settings_path) << R"({"provider":"openai-compatible","sessionDir":"/data/sessions"})";

    auto settings = coding_agent::SettingsLoader::load(settings_path);
    REQUIRE(settings);
    REQUIRE(settings->session_dir.has_value());
    CHECK(*settings->session_dir == "/data/sessions");
    // Existing provider parsing is unaffected by the new key.
    CHECK(settings->provider == "openai-compatible");
}

TEST_CASE("SettingsLoader treats a non-string sessionDir as absent", "[settings][session-dir]") {
    tests::TempWorkspace workspace;
    auto settings_path = workspace.path() / "settings.json";
    std::ofstream(settings_path) << R"({"sessionDir":42,"model":"gpt-4"})";

    auto settings = coding_agent::SettingsLoader::load(settings_path);
    REQUIRE(settings);
    CHECK_FALSE(settings->session_dir.has_value());
    CHECK(settings->model == "gpt-4");
}

TEST_CASE("SettingsLoader defaults sessionDir to absent", "[settings][session-dir]") {
    tests::TempWorkspace workspace;
    auto settings_path = workspace.path() / "settings.json";
    std::ofstream(settings_path) << R"({"provider":"openai-compatible"})";

    auto settings = coding_agent::SettingsLoader::load(settings_path);
    REQUIRE(settings);
    CHECK_FALSE(settings->session_dir.has_value());
}
