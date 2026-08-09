#pragma once

#include "../../third_party/catch2/catch_test_macros.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

namespace cch::tests {

/// The theme fixture files' source root (`tests/fixtures/themes`).
[[nodiscard]] inline std::filesystem::path theme_fixture_dir() {
    return std::filesystem::path(CCH_SOURCE_DIR) / "tests" / "fixtures" / "themes";
}

/// A theme JSON document derived from the dark fixture with a distinct name
/// and accent color (the shared shape of the controller/loader theme
/// tests).
[[nodiscard]] inline std::string fixture_theme(std::string_view name, std::string_view accent) {
    std::ifstream input(theme_fixture_dir() / "dark.json");
    std::ostringstream content;
    content << input.rdbuf();
    auto json = content.str();
    const auto replace_once = [&json](std::string_view old_text, std::string_view new_text) {
        const auto position = json.find(old_text);
        REQUIRE(position != std::string::npos);
        json.replace(position, old_text.size(), new_text);
    };
    replace_once(
        "\"name\": \"dark\"",
        std::string{"\"name\": \""} + std::string(name) + "\"");
    replace_once(
        "\"accent\": \"accent\"",
        std::string{"\"accent\": \""} + std::string(accent) + "\"");
    return json;
}

} // namespace cch::tests
