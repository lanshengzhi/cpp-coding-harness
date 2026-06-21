#pragma once

#include "CliConfig.hpp"

#include "../../include/cch/util/Error.hpp"

namespace cch::cli {

[[nodiscard]] cch::util::ExpectedVoid preflight_cli_config(const CliConfig& config);

[[nodiscard]] cch::util::ExpectedVoid validate_workspace(const std::filesystem::path& workspace);

[[nodiscard]] std::filesystem::path canonical_workspace(const std::filesystem::path& workspace);

[[nodiscard]] AsyncCliRuntimeConfig to_runtime_config(CliConfig config);

void print_error(const cch::util::Error& error);

} // namespace cch::cli
