#include "coding_agent/compat/pi/PiImport.hpp"

#include <cch/coding_agent/AgentConfigDir.hpp>

#include "support/Json.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace cch::coding_agent::compat::pi {
namespace {

struct ImportEntry {
    std::filesystem::path relative_path;
    bool directory{false};
};

[[nodiscard]] support::Error import_error(
        std::string message, const std::filesystem::path& path, std::string detail = {}) {
    if (!detail.empty()) {
        message += ": " + detail;
    }
    return support::make_error(
            support::ErrorCode::Validation, std::move(message), path.empty() ? std::string{} : path.string());
}

[[nodiscard]] support::Expected<std::filesystem::path> absolute_normalized(const std::filesystem::path& path) {
    if (path.empty()) {
        return std::unexpected(import_error("import path is empty", path));
    }
    std::error_code error;
    auto absolute = std::filesystem::absolute(path, error);
    if (error) {
        return std::unexpected(import_error("could not resolve import path", path, error.message()));
    }
    return absolute.lexically_normal();
}

[[nodiscard]] bool path_is_within(const std::filesystem::path& child, const std::filesystem::path& parent) {
    auto child_it = child.begin();
    auto parent_it = parent.begin();
    for (; parent_it != parent.end(); ++parent_it, ++child_it) {
        if (child_it == child.end() || *child_it != *parent_it) {
            return false;
        }
    }
    return child_it != child.end();
}

[[nodiscard]] support::ExpectedVoid validate_pi_session_file(const std::filesystem::path& path) {
    static constexpr std::array<std::string_view, 11> kEntryTypes{
            "message",
            "model_change",
            "thinking_level_change",
            "active_tools_change",
            "custom",
            "custom_message",
            "label",
            "compaction",
            "branch_summary",
            "session_info",
            "leaf",
    };

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::unexpected(import_error("could not read pi session", path));
    }

    std::string line;
    std::size_t line_number = 0;
    bool saw_header = false;
    while (std::getline(input, line)) {
        ++line_number;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        auto parsed = support::read_json(line);
        if (!parsed || !parsed->holds<support::JsonValue::object_t>()) {
            return std::unexpected(
                    import_error("pi session contains malformed JSON", path, "line " + std::to_string(line_number)));
        }
        const auto& object = parsed->get_object();
        const auto type = object.find("type");
        if (type == object.end() || !type->second.holds<std::string>()) {
            return std::unexpected(
                    import_error("pi session entry has no type", path, "line " + std::to_string(line_number)));
        }
        const auto& type_name = type->second.get_string();
        if (!saw_header) {
            if (type_name != "session") {
                return std::unexpected(import_error(
                        "pi session has an unknown header shape", path, "line " + std::to_string(line_number)));
            }
            saw_header = true;
            continue;
        }
        if (std::find(kEntryTypes.begin(), kEntryTypes.end(), type_name) == kEntryTypes.end()) {
            return std::unexpected(import_error("pi session has an unknown entry shape",
                    path,
                    "type " + type_name + " at line " + std::to_string(line_number)));
        }
    }
    if (input.bad()) {
        return std::unexpected(import_error("could not read pi session", path));
    }
    if (!saw_header) {
        return std::unexpected(import_error("pi session is empty", path));
    }
    return {};
}

[[nodiscard]] support::Expected<std::vector<ImportEntry>> collect_entries(const std::filesystem::path& source) {
    std::vector<ImportEntry> entries;
    std::error_code error;
    std::filesystem::recursive_directory_iterator iterator(source, std::filesystem::directory_options::none, error);
    if (error) {
        return std::unexpected(import_error("could not inspect pi state", source, error.message()));
    }

    while (iterator != std::filesystem::recursive_directory_iterator{}) {
        const auto current = iterator->path();
        const auto relative = current.lexically_relative(source);
        const auto status = iterator->symlink_status(error);
        if (error) {
            return std::unexpected(import_error("could not inspect pi state entry", current, error.message()));
        }
        if (std::filesystem::is_symlink(status)) {
            return std::unexpected(import_error("pi import refuses symlinks", current));
        }
        if (std::filesystem::is_directory(status)) {
            entries.push_back(ImportEntry{relative, true});
        } else if (std::filesystem::is_regular_file(status)) {
            if (current.extension() == ".jsonl") {
                if (auto valid = validate_pi_session_file(current); !valid) {
                    return std::unexpected(valid.error());
                }
            }
            entries.push_back(ImportEntry{relative, false});
        } else {
            return std::unexpected(import_error("pi import refuses special files", current));
        }

        iterator.increment(error);
        if (error) {
            return std::unexpected(import_error("could not walk pi state", current, error.message()));
        }
    }

    std::sort(entries.begin(), entries.end(), [](const ImportEntry& first, const ImportEntry& second) {
        return first.relative_path.generic_string() < second.relative_path.generic_string();
    });
    return entries;
}

[[nodiscard]] support::ExpectedVoid set_private_permissions(const std::filesystem::path& path, bool directory) {
    const auto permissions = directory ? std::filesystem::perms::owner_all
                                       : std::filesystem::perms::owner_read | std::filesystem::perms::owner_write;
    std::error_code error;
    std::filesystem::permissions(path, permissions, std::filesystem::perm_options::replace, error);
    if (error) {
        return std::unexpected(import_error("could not set imported state permissions", path, error.message()));
    }
    return {};
}

[[nodiscard]] support::Error cleanup_import_destination(
        const std::filesystem::path& destination, support::Error failure) {
    std::error_code cleanup_error;
    std::filesystem::remove_all(destination, cleanup_error);
    if (cleanup_error) {
        failure.detail += "; cleanup also failed: " + cleanup_error.message();
    }
    return failure;
}

} // namespace

std::filesystem::path default_source_directory() {
    const auto home = home_directory();
    return home.empty() ? std::filesystem::path{} : home / ".pi" / "agent";
}

support::Expected<ImportReport> import_state(ImportOptions options) {
    auto source = absolute_normalized(options.source_directory);
    if (!source) {
        return std::unexpected(source.error());
    }
    auto destination = absolute_normalized(options.destination_directory);
    if (!destination) {
        return std::unexpected(destination.error());
    }

    std::error_code error;
    const auto source_status = std::filesystem::symlink_status(*source, error);
    if (error || !std::filesystem::is_directory(source_status)) {
        return std::unexpected(import_error(
                "pi state directory does not exist", *source, error ? error.message() : "not a directory"));
    }
    if (path_is_within(*destination, *source) || *destination == *source) {
        return std::unexpected(
                import_error("import destination must not be inside the pi state directory", *destination));
    }

    const auto destination_status = std::filesystem::symlink_status(*destination, error);
    if (!error && destination_status.type() != std::filesystem::file_type::not_found) {
        return std::unexpected(
                import_error("import destination already exists; refusing to overwrite it", *destination));
    }
    if (error && error != std::errc::no_such_file_or_directory) {
        return std::unexpected(import_error("could not inspect import destination", *destination, error.message()));
    }

    auto entries = collect_entries(*source);
    if (!entries) {
        return std::unexpected(entries.error());
    }

    const auto parent = destination->parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, error);
        if (error) {
            return std::unexpected(import_error("could not create import destination parent", parent, error.message()));
        }
    }
    std::filesystem::create_directory(*destination, error);
    if (error) {
        return std::unexpected(import_error("could not create import destination", *destination, error.message()));
    }
    if (auto permissions = set_private_permissions(*destination, true); !permissions) {
        return std::unexpected(cleanup_import_destination(*destination, permissions.error()));
    }

    ImportReport report;
    for (const auto& entry : *entries) {
        const auto source_path = *source / entry.relative_path;
        const auto destination_path = *destination / entry.relative_path;
        if (entry.directory) {
            std::filesystem::create_directory(destination_path, error);
            if (error) {
                return std::unexpected(cleanup_import_destination(*destination,
                        import_error("could not create imported directory", destination_path, error.message())));
            }
            if (auto permissions = set_private_permissions(destination_path, true); !permissions) {
                return std::unexpected(cleanup_import_destination(*destination, permissions.error()));
            }
            ++report.directories_copied;
            continue;
        }

        std::filesystem::copy_file(source_path, destination_path, std::filesystem::copy_options::none, error);
        if (error) {
            return std::unexpected(cleanup_import_destination(
                    *destination, import_error("could not copy imported file", source_path, error.message())));
        }
        if (auto permissions = set_private_permissions(destination_path, false); !permissions) {
            return std::unexpected(cleanup_import_destination(*destination, permissions.error()));
        }
        ++report.files_copied;
    }

    return report;
}

} // namespace cch::coding_agent::compat::pi
