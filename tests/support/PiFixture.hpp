#pragma once

#include "util/Json.hpp"

#include <cch/util/Error.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace cch::tests {

[[nodiscard]] inline util::Expected<util::JsonValue> read_pi_fixture(
    std::string_view relative_path) {
    const auto path = std::filesystem::path{CCH_SOURCE_DIR} / "fixtures/pi-ai" /
                      relative_path;
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Unknown,
            "Failed to open pi fixture: " + path.string()));
    }
    const std::string json{
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{}};
    return util::read_json(json);
}

[[nodiscard]] inline util::Expected<std::string> read_pi_fixture_text(
    std::string_view relative_path) {
    const auto path = std::filesystem::path{CCH_SOURCE_DIR} / "fixtures/pi-ai" /
                      relative_path;
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Unknown,
            "Failed to open pi fixture: " + path.string()));
    }
    return std::string{
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{}};
}

} // namespace cch::tests
