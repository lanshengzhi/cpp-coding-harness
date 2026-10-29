#pragma once

#include "durable/FrozenExpectation.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string_view>
#include <utility>

namespace cch::tests::durable {

/// The registered durable-image observation bundle.
inline constexpr std::string_view kImageArtifact = "fixtures/pi-v1.3.0/audit-1090/images.json";

/// Load one image scenario from the registered bundle, failing the test when
/// the fixture is missing.
[[nodiscard]] inline FrozenScenario image_scenario(std::string_view name) {
    auto loaded = FrozenScenario::load(kImageArtifact, name);
    REQUIRE(loaded.has_value());
    return std::move(*loaded);
}

} // namespace cch::tests::durable
