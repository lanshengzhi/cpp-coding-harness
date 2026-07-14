#pragma once

#include "../../cli/CliRuntimeConfig.hpp"

#include <iosfwd>

namespace cch::coding_agent {
struct PromptResult;
}

namespace cch::cli {

/// Write the successful text-frontend presentation for one prompt result.
void write_text_prompt_result(
    const coding_agent::PromptResult& result,
    std::ostream& output);

int run_async_cli(const AsyncCliRuntimeConfig& config);

} // namespace cch::cli
