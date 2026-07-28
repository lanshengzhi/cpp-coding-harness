#pragma once

#include <cch/ai/Content.hpp>
#include <cch/util/Error.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace cch::cli {

struct PreparedInitialPrompt {
    std::string text;
    std::vector<ai::ImageContent> images;
};

[[nodiscard]] util::Expected<PreparedInitialPrompt> prepare_initial_prompt(
    std::string_view prompt,
    const std::vector<std::string>& file_arguments,
    const std::filesystem::path& working_directory);

} // namespace cch::cli
