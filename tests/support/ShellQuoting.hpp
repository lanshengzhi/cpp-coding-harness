#pragma once

#include <filesystem>
#include <string>

namespace cch::tests {

/// Single-quote a path so it survives as one argument inside a shell command string.
[[nodiscard]] inline std::string shell_quote(const std::filesystem::path& path) {
    return "'" + path.string() + "'";
}

} // namespace cch::tests
