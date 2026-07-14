#pragma once

#include <optional>
#include <string_view>
#include <utility>

namespace cch::coding_agent::prompt {

/// Parse a column-zero slash command into a name and arguments.
/// Returns std::nullopt if the input does not begin with '/' or if the name is empty.
[[nodiscard]] std::optional<std::pair<std::string_view, std::string_view>> try_parse_slash_command(
    std::string_view input);

} // namespace cch::coding_agent::prompt
