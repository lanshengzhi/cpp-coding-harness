#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <system_error>

TEST_CASE(
    "Formal Catch2 runner isolates HOME, Agent Config Directory, and temporary files",
    "[support][test-runner][issue442]") {
    const auto* home_value = std::getenv("HOME");
    const auto* agent_config_value = std::getenv("PI_CODING_AGENT_DIR");
    const auto* temporary_value = std::getenv("TMPDIR");

    REQUIRE(home_value != nullptr);
    REQUIRE(agent_config_value != nullptr);
    REQUIRE(temporary_value != nullptr);

    const std::filesystem::path home{home_value};
    const std::filesystem::path agent_config{agent_config_value};
    const std::filesystem::path temporary{temporary_value};

    std::error_code home_error;
    std::error_code agent_config_error;
    std::error_code temporary_error;
    CHECK(std::filesystem::is_directory(home, home_error));
    CHECK_FALSE(home_error);
    CHECK(std::filesystem::is_directory(agent_config, agent_config_error));
    CHECK_FALSE(agent_config_error);
    CHECK(std::filesystem::is_directory(temporary, temporary_error));
    CHECK_FALSE(temporary_error);
    CHECK(home.parent_path() == agent_config.parent_path());
    CHECK(home.parent_path() == temporary.parent_path());
    CHECK(home != agent_config);
    CHECK(home != temporary);
    CHECK(agent_config != temporary);
    CHECK(std::getenv("PI_CODING_AGENT_SESSION_DIR") == nullptr);
}
