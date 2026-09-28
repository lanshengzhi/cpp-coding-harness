#pragma once

#include "CliConfig.hpp"

#include <cch/support/Error.hpp>

namespace cch::cli {

[[nodiscard]] cch::support::Expected<CliConfig> parse_args(int argc, char** argv);

} // namespace cch::cli
