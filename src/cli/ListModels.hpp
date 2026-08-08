#pragma once

#include <cch/coding_agent/ModelRuntime.hpp>

#include <iosfwd>
#include <optional>
#include <string>

namespace cch::cli {

/// pi `cli/list-models.ts` `listModels`, verbatim output: the six-column
/// `provider model context max-out thinking images` table over the runtime's
/// available (configured-auth) models, `formatTokenCount` K/M formatting,
/// fuzzy `[search]` filtering (`No models matching "<pattern>"` when empty),
/// the yellow models.json warning on stderr, and
/// `formatNoModelsAvailableMessage()` when no models exist. Runs post-runtime
/// pre-stdin; the caller exits 0 after it returns.
void print_list_models(
    const coding_agent::ModelRuntime& runtime,
    const std::optional<std::string>& search,
    std::ostream& output,
    std::ostream& error);

} // namespace cch::cli
