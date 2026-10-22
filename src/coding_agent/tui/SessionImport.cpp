#include "SessionImport.hpp"

#include <cctype>
#include <system_error>

namespace cch::coding_agent::tui {
namespace {

/// pi `path.parse` (`node:path`): the extension is the last dot of the base
/// name, and a leading-dot file (".bashrc") has no extension.
struct ParsedName {
    std::string name;
    std::string extension;
};

[[nodiscard]] ParsedName parse_file_name(const std::filesystem::path& path) {
    const auto base = path.filename().string();
    const auto dot = base.rfind('.');
    if (dot == std::string::npos || dot == 0) {
        return ParsedName{.name = base, .extension = {}};
    }
    return ParsedName{
            .name = base.substr(0, dot),
            .extension = base.substr(dot),
    };
}

/// Whether two paths name the same file. pi compares the resolved paths
/// (`resolve(destinationPath) === resolvedPath`), so the comparison is
/// lexical-normalized rather than a filesystem identity check.
[[nodiscard]] bool same_path(const std::filesystem::path& left, const std::filesystem::path& right) {
    std::error_code left_ec;
    std::error_code right_ec;
    const auto left_normal = std::filesystem::weakly_canonical(left, left_ec);
    const auto right_normal = std::filesystem::weakly_canonical(right, right_ec);
    if (left_ec || right_ec) {
        return left.lexically_normal() == right.lexically_normal();
    }
    return left_normal == right_normal;
}

} // namespace

std::optional<std::string> path_command_argument(std::string_view argument) {
    // pi `getPathCommandArgument`: the argument text after the command
    // token, with the leading whitespace already trimmed by the router.
    const auto trimmed = argument;
    if (trimmed.empty()) {
        return std::nullopt;
    }
    const auto first = trimmed.front();
    if (first == '"' || first == '\'') {
        const auto closing = trimmed.find(first, 1);
        if (closing == std::string_view::npos) {
            return std::nullopt;
        }
        return std::string{trimmed.substr(1, closing - 1)};
    }
    const auto separator = trimmed.find_first_of(" \t\n\r\f\v");
    if (separator == std::string_view::npos) {
        return std::string{trimmed};
    }
    return std::string{trimmed.substr(0, separator)};
}

std::filesystem::path import_destination_path(
        const std::filesystem::path& session_dir, const std::filesystem::path& resolved_source) {
    const auto base_name = resolved_source.filename();
    auto destination = session_dir / base_name;
    if (same_path(destination, resolved_source)) {
        // The source already lives in the session directory: pi keeps the
        // file where it is and opens it in place.
        return destination;
    }
    const auto parsed = parse_file_name(destination);
    std::size_t suffix = 1;
    std::error_code exists_ec;
    while (std::filesystem::exists(destination, exists_ec)) {
        destination = session_dir / (parsed.name + "-" + std::to_string(suffix++) + parsed.extension);
    }
    return destination;
}

support::Expected<std::filesystem::path> copy_imported_session(
        const std::filesystem::path& session_dir, const std::filesystem::path& resolved_source) {
    std::error_code ec;
    if (!std::filesystem::exists(session_dir, ec)) {
        // pi `mkdirSync(sessionDir, { recursive: true })`.
        std::filesystem::create_directories(session_dir, ec);
        if (ec) {
            return std::unexpected(support::make_error(support::ErrorCode::Session,
                    "could not create the session directory",
                    session_dir.string() + ": " + ec.message()));
        }
    }

    const auto destination = import_destination_path(session_dir, resolved_source);
    if (same_path(destination, resolved_source)) {
        // pi copies only when the source is not already stored at the
        // destination; the file is opened in place instead.
        return destination;
    }

    // pi `copyFileSync(source, destination, COPYFILE_EXCL)`: the copy never
    // overwrites an existing file.
    std::filesystem::copy_file(resolved_source, destination, std::filesystem::copy_options::none, ec);
    if (ec) {
        return std::unexpected(support::make_error(support::ErrorCode::Session,
                "could not copy the imported session file",
                resolved_source.string() + " -> " + destination.string() + ": " + ec.message()));
    }
    return destination;
}

} // namespace cch::coding_agent::tui
