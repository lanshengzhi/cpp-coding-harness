#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <system_error>

TEST_CASE("Formal Catch2 runner isolates HOME and temporary files", "[support][test-runner][issue754][spec]") {
    const auto* home_value = std::getenv("HOME");
    const auto* temporary_value = std::getenv("TMPDIR");

    REQUIRE(home_value != nullptr);
    REQUIRE(temporary_value != nullptr);
    // `XDG_CONFIG_HOME` stays unset so the isolated HOME drives the fixed Agent
    // Config Directory; a test that asserts the XDG precedence sets it itself.
    CHECK(std::getenv("XDG_CONFIG_HOME") == nullptr);

    const std::filesystem::path home{home_value};
    const std::filesystem::path temporary{temporary_value};

    std::error_code home_error;
    std::error_code temporary_error;
    CHECK(std::filesystem::is_directory(home, home_error));
    CHECK_FALSE(home_error);
    CHECK(std::filesystem::is_directory(temporary, temporary_error));
    CHECK_FALSE(temporary_error);
    CHECK(home.parent_path() == temporary.parent_path());
    CHECK(home != temporary);
}
