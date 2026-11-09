#pragma once

#include <charconv>
#include <istream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace cch::tests {

[[nodiscard]] inline std::optional<unsigned long long> read_peak_memory(std::istream& status) {
    std::optional<unsigned long long> peak;
    std::string line;
    while (std::getline(status, line)) {
        if (!line.starts_with("VmHWM:")) continue;
        if (peak.has_value()) return std::nullopt;
        std::string_view value(line);
        value.remove_prefix(6);
        const auto start = value.find_first_not_of(" \t");
        if (start == std::string_view::npos) return std::nullopt;
        value.remove_prefix(start);
        unsigned long long kilobytes = 0;
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), kilobytes);
        if (parsed.ec != std::errc{} || parsed.ptr == value.data() || kilobytes == 0 ||
                kilobytes > std::numeric_limits<unsigned long long>::max() / 1024)
            return std::nullopt;
        value.remove_prefix(static_cast<std::size_t>(parsed.ptr - value.data()));
        const auto unit = value.find_first_not_of(" \t");
        if (unit == std::string_view::npos || unit == 0) return std::nullopt;
        value.remove_prefix(unit);
        const auto end = value.find_last_not_of(" \t\r");
        if (end == std::string_view::npos || value.substr(0, end + 1) != "kB") return std::nullopt;
        peak = kilobytes * 1024;
    }
    if (status.bad()) return std::nullopt;
    return peak;
}

} // namespace cch::tests
