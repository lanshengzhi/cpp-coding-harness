#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <string>
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

    const auto* base_value = std::getenv("CCH_TEST_ISOLATION_BASE");
    const auto* inherited_value = std::getenv("CCH_TEST_INHERITED_TMPDIR");
    REQUIRE(base_value != nullptr);
    const std::filesystem::path base{base_value};

    std::error_code home_error;
    std::error_code temporary_error;
    std::error_code base_error;
    CHECK(std::filesystem::is_directory(home, home_error));
    CHECK_FALSE(home_error);
    CHECK(std::filesystem::is_directory(temporary, temporary_error));
    CHECK_FALSE(temporary_error);
    CHECK(std::filesystem::is_directory(base, base_error));
    CHECK_FALSE(base_error);
    CHECK(home.parent_path() == temporary.parent_path());
    CHECK(home != temporary);

    // The isolation tree is a direct child of the resolved base, and that base
    // is the inherited TMPDIR (or /tmp when the caller did not select one).
    const auto root = home.parent_path();
    CHECK(root.parent_path() == base);
    CHECK(root.filename().string().starts_with("cpp-harness-catch2-"));
    if (inherited_value != nullptr) {
        CHECK(base.lexically_normal() == std::filesystem::path{inherited_value}.lexically_normal());
    } else {
        CHECK(base == std::filesystem::path{"/tmp"});
    }
}
