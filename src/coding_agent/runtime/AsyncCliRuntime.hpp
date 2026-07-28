#pragma once

#include "cli/CliConfig.hpp"
#include "cli/FrontendSelection.hpp"

namespace cch::cli {

[[nodiscard]] int run_async_cli(
    const CliConfig& config,
    Frontend frontend,
    FrontendEnvironment environment);

} // namespace cch::cli
