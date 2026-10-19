#pragma once

#include <cch/ai/Models.hpp>

#include <cstdlib>
#include <optional>
#include <string>
#include <utility>

namespace cch::coding_agent {

/// Private helper converting application runtime options into AI gateway startup options.
[[nodiscard]] inline ai::ModelsOptions make_gateway_models_options(
        std::optional<std::string> api_key = std::nullopt, std::string magpie_base_url = {}) {
    ai::ModelsOptions gateway{.api_key = std::move(api_key)};
    if (const char* key = std::getenv("MAGPIE_API_KEY"); key && *key != '\0') {
        gateway.environment_api_key = std::string{key};
    }
    if (!magpie_base_url.empty()) {
        gateway.gateway_url = std::move(magpie_base_url);
    }
    return gateway;
}

} // namespace cch::coding_agent
