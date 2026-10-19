#pragma once

#include <string>

namespace cch::coding_agent {

/// Catalog discovery guidance shared by Session Assembly and --list-models.
[[nodiscard]] inline std::string format_no_models_available_message() {
    return "No models available. Start the local Magpie gateway with `magpie serve`, "
           "then run `/reload`.";
}

} // namespace cch::coding_agent
