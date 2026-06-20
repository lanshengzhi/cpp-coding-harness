#include "../../third_party/catch2/catch_test_macros.hpp"
#include "../../include/cch/coding_agent/Config.hpp"
#include "../support/TempWorkspace.hpp"

#include <cstdlib>
#include <fstream>
#include <string>

using namespace cch;

TEST_CASE("ConfigLoader loads provider and model from JSON", "[config]") {
    tests::TempWorkspace workspace;
    auto config_path = workspace.path() / "config.json";
    std::ofstream(config_path) << R"({"provider":"openai-compatible","model":"gpt-4"})";

    auto config = coding_agent::ConfigLoader::load(config_path.string());
    REQUIRE(config);
    CHECK(config->provider == "openai-compatible");
    CHECK(config->model == "gpt-4");
    CHECK_FALSE(config->base_url.has_value());
}

TEST_CASE("ConfigLoader returns defaults when file does not exist", "[config]") {
    auto config = coding_agent::ConfigLoader::load("/nonexistent/path/config.json");
    REQUIRE(config);
    CHECK_FALSE(config->provider.has_value());
    CHECK_FALSE(config->model.has_value());
}

TEST_CASE("ConfigLoader returns error for malformed JSON", "[config]") {
    tests::TempWorkspace workspace;
    auto config_path = workspace.path() / "config.json";
    std::ofstream(config_path) << "{not valid json";

    auto config = coding_agent::ConfigLoader::load(config_path.string());
    CHECK_FALSE(config.has_value());
}

TEST_CASE("ConfigLoader ignores unknown keys", "[config]") {
    tests::TempWorkspace workspace;
    auto config_path = workspace.path() / "config.json";
    std::ofstream(config_path) << R"({"provider":"openai-compatible","unknown_key":true})";

    auto config = coding_agent::ConfigLoader::load(config_path.string());
    REQUIRE(config);
    CHECK(config->provider == "openai-compatible");
}

TEST_CASE("ConfigLoader handles api_key_env as array", "[config]") {
    tests::TempWorkspace workspace;
    auto config_path = workspace.path() / "config.json";
    std::ofstream(config_path) << R"({"api_key_env":["CUSTOM_KEY","OPENAI_API_KEY"]})";

    auto config = coding_agent::ConfigLoader::load(config_path.string());
    REQUIRE(config);
    REQUIRE(config->api_key_env.has_value());
    CHECK(config->api_key_env->size() == 2);
    CHECK((*config->api_key_env)[0] == "CUSTOM_KEY");
    CHECK((*config->api_key_env)[1] == "OPENAI_API_KEY");
}

TEST_CASE("ConfigLoader handles api_key_env as single string", "[config]") {
    tests::TempWorkspace workspace;
    auto config_path = workspace.path() / "config.json";
    std::ofstream(config_path) << R"({"api_key_env":"OPENAI_API_KEY"})";

    auto config = coding_agent::ConfigLoader::load(config_path.string());
    REQUIRE(config);
    REQUIRE(config->api_key_env.has_value());
    CHECK(config->api_key_env->size() == 1);
}

TEST_CASE("ConfigLoader resolve_api_key finds first set env var", "[config]") {
    // Set a known env var for testing
    setenv("CCH_TEST_KEY", "test-value-123", 1);
    auto result = coding_agent::ConfigLoader::resolve_api_key({"CCH_TEST_KEY", "DOES_NOT_EXIST"});
    REQUIRE(result.has_value());
    CHECK(*result == "test-value-123");
    unsetenv("CCH_TEST_KEY");
}

TEST_CASE("ConfigLoader resolve_api_key returns nullopt when none set", "[config]") {
    auto result = coding_agent::ConfigLoader::resolve_api_key({"DOES_NOT_EXIST_XYZ"});
    CHECK_FALSE(result.has_value());
}
