#pragma once

#include <string_view>

namespace cch::tui::detail {

inline bool is_baseline_symbol(char value) {
    constexpr std::string_view kSymbols = "`-=[]\\;',./!@#$%^&*()_+|~{}:<>?";
    return kSymbols.find(value) != std::string_view::npos;
}

} // namespace cch::tui::detail
