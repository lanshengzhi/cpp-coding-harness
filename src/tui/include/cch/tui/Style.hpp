#pragma once

#include <functional>
#include <string>

namespace cch::tui {

/// Generic terminal-text styling. Hooks may add supported ANSI styling but must
/// preserve the visible width of the supplied text.
using TextStyleHook = std::move_only_function<std::string(std::string)>;
using SelectionStyleHook = std::move_only_function<std::string(std::string, bool)>;

} // namespace cch::tui
