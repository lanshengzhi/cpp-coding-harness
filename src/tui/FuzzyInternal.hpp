#pragma once

#include <string>
#include <string_view>

namespace cch::tui::detail {

/// pi's `String.prototype.toLowerCase` for UTF-8 text, including dotted-I
/// expansion and context-sensitive final sigma. Full case folding is not used.
[[nodiscard]] std::string lowercase_text(std::string_view text);

} // namespace cch::tui::detail
