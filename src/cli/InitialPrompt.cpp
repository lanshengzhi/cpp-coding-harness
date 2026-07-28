#include "InitialPrompt.hpp"

#include "coding_agent/ImageInput.hpp"

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <span>
#include <system_error>
#include <utility>

namespace cch::cli {
namespace {

[[nodiscard]] util::Error file_error(
    std::string message,
    const std::filesystem::path& path,
    std::string detail = {}) {
    if (detail.empty()) detail = message;
    return util::make_error(
        util::ErrorCode::Validation,
        std::move(message),
        std::move(detail),
        path.string());
}

[[nodiscard]] util::Expected<std::filesystem::path> resolve_file_path(
    std::string_view argument,
    const std::filesystem::path& working_directory) {
    if (argument.empty()) {
        return std::unexpected(file_error("initial file path is empty", {}));
    }

    std::filesystem::path path{argument};
    if (argument == "~" || argument.starts_with("~/")) {
        const char* home = std::getenv("HOME");
        if (home == nullptr || *home == '\0') {
            return std::unexpected(file_error(
                "could not expand initial file path",
                path,
                "HOME is unavailable"));
        }
        path = argument == "~"
            ? std::filesystem::path{home}
            : std::filesystem::path{home} / std::string(argument.substr(2));
    }
    if (path.is_relative()) path = working_directory / path;

    std::error_code absolute_error;
    path = std::filesystem::absolute(path, absolute_error);
    if (absolute_error) {
        return std::unexpected(file_error(
            "could not resolve initial file path",
            path,
            absolute_error.message()));
    }
    return path.lexically_normal();
}

[[nodiscard]] std::string text_file_reference(
    const std::filesystem::path& path,
    std::span<const std::uint8_t> bytes) {
    std::string reference = "<file name=\"" + path.string() + "\">\n";
    reference.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    reference += "\n</file>\n";
    return reference;
}

[[nodiscard]] std::string image_file_reference(
    const std::filesystem::path& path,
    const coding_agent::ProcessedImageInput& processed) {
    std::string reference = "<file name=\"" + path.string() + "\">";
    if (processed.image) {
        for (std::size_t index = 0; index < processed.hints.size(); ++index) {
            if (index > 0) reference.push_back('\n');
            reference += processed.hints[index];
        }
    } else if (processed.omission) {
        reference += *processed.omission;
    }
    reference += "</file>\n";
    return reference;
}

[[nodiscard]] util::Expected<std::vector<std::uint8_t>> read_file_bytes(
    const std::filesystem::path& path) {
    std::error_code status_error;
    const auto status = std::filesystem::status(path, status_error);
    if (status_error || !std::filesystem::exists(status)) {
        return std::unexpected(file_error(
            "initial file not found: " + path.string(),
            path,
            status_error ? status_error.message() : "file does not exist"));
    }
    if (!std::filesystem::is_regular_file(status)) {
        return std::unexpected(file_error(
            "initial file is not a regular file: " + path.string(),
            path));
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::unexpected(file_error(
            "could not read initial file: " + path.string(),
            path));
    }
    std::vector<std::uint8_t> bytes;
    for (std::istreambuf_iterator<char> iterator{input}, end; iterator != end; ++iterator) {
        bytes.push_back(static_cast<std::uint8_t>(static_cast<unsigned char>(*iterator)));
    }
    if (input.bad()) {
        return std::unexpected(file_error(
            "could not read initial file: " + path.string(),
            path));
    }
    return bytes;
}

} // namespace

util::Expected<PreparedInitialPrompt> prepare_initial_prompt(
    std::string_view prompt,
    const std::vector<std::string>& file_arguments,
    const std::filesystem::path& working_directory) {
    PreparedInitialPrompt prepared;
    for (const auto& argument : file_arguments) {
        auto resolved = resolve_file_path(argument, working_directory);
        if (!resolved) return std::unexpected(std::move(resolved.error()));

        auto bytes = read_file_bytes(*resolved);
        if (!bytes) return std::unexpected(std::move(bytes.error()));
        if (bytes->empty()) continue;

        const auto mime_type = coding_agent::sniff_supported_image_mime_type(*bytes);
        if (!mime_type) {
            prepared.text += text_file_reference(*resolved, *bytes);
            continue;
        }

        auto processed = coding_agent::process_image_input(*bytes, *mime_type);
        if (!processed) return std::unexpected(std::move(processed.error()));
        prepared.text += image_file_reference(*resolved, *processed);
        if (processed->image) prepared.images.push_back(std::move(*processed->image));
    }
    prepared.text += prompt;
    return prepared;
}

} // namespace cch::cli
