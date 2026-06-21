#pragma once

#include "CliConfig.hpp"

#include "../../include/cch/util/Error.hpp"

namespace cch::cli {

[[nodiscard]] cch::util::Expected<CliConfig> parse_args(int argc, char** argv);

} // namespace cch::cli
