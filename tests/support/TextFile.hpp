#pragma once

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace cch::tests {

/// Read one file's exact bytes as text. Test-local copies of this helper kept
/// reappearing; the shared one keeps the fixture reads identical.
[[nodiscard]] inline std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

} // namespace cch::tests
