#pragma once

#include "support/Json.hpp"

#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>

namespace cch::tests::audit_1047 {

/// Read one frozen `fixtures/pi-v1.3.0/audit-1047/<artifact>` file.
///
/// The helper is read-only on purpose: a comparison case drives the artifact's
/// own `inputs` and asserts against its own `expected` members, so no frozen
/// value is copied into C++.
[[nodiscard]] inline support::JsonValue read_artifact(std::string_view artifact) {
    const std::string path = std::string{CCH_SOURCE_DIR} + "/fixtures/pi-v1.3.0/audit-1047/" + std::string{artifact};
    std::ifstream input(path, std::ios::binary);
    const std::string text{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    auto parsed = support::read_json(text);
    REQUIRE(parsed.has_value());
    return std::move(*parsed);
}

/// Find one scenario by its exact frozen name.
[[nodiscard]] inline const support::JsonValue* find_scenario(
        const support::JsonValue& artifact, std::string_view name) {
    for (const auto& scenario : artifact.get_object().at("scenarios").get_array()) {
        if (scenario.get_object().at("name").get_string() == name) {
            return &scenario;
        }
    }
    return nullptr;
}

/// The named member of a JSON object; a missing member is a broken fixture.
[[nodiscard]] inline const support::JsonValue& member(const support::JsonValue& object, std::string_view name) {
    const auto& fields = object.get_object();
    const auto found = fields.find(name);
    REQUIRE(found != fields.end());
    return found->second;
}

} // namespace cch::tests::audit_1047
