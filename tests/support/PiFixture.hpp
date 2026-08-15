#pragma once

#include "support/Json.hpp"

#include <cch/support/Error.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace cch::tests {

[[nodiscard]] inline support::Expected<support::JsonValue> read_pi_fixture(
    std::string_view relative_path) {
    const auto path = std::filesystem::path{CCH_SOURCE_DIR} / "fixtures/pi-ai" /
                      relative_path;
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Unknown,
            "Failed to open pi fixture: " + path.string()));
    }
    const std::string json{
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{}};
    return support::read_json(json);
}

[[nodiscard]] inline support::Expected<std::string> read_pi_fixture_text(
    std::string_view relative_path) {
    const auto path = std::filesystem::path{CCH_SOURCE_DIR} / "fixtures/pi-ai" /
                      relative_path;
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Unknown,
            "Failed to open pi fixture: " + path.string()));
    }
    return std::string{
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{}};
}

} // namespace cch::tests
