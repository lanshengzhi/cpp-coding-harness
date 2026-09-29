#include "coding_agent/TrustStoreFile.hpp"

#include "support/Json.hpp"

#include <format>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

#include <sys/stat.h>

// The mechanics are factored out of the project-trust store verbatim, and the
// Upstream MCP Server trust store inherits them: a trust store is owner-only
// state, so a symlink, a non-regular file, or group/other write permission is
// refused rather than followed.
namespace cch::coding_agent {

support::Error trust_store_error(std::string message, std::string detail) {
    if (detail.empty()) {
        detail = message;
    }
    return support::make_error(support::ErrorCode::Validation, std::move(message), std::move(detail));
}

support::Expected<TrustStoreMap> read_trust_store_map(const std::filesystem::path& path, std::string_view store_name) {
    TrustStoreMap data;
    if (path.empty()) {
        return std::unexpected(trust_store_error(std::format("{} path is empty", store_name)));
    }

    std::error_code ec;
    auto status = std::filesystem::symlink_status(path, ec);
    if (ec) {
        if (ec.default_error_condition() == std::errc::no_such_file_or_directory ||
                ec.default_error_condition() == std::errc::not_a_directory) {
            return data;
        }
        return std::unexpected(trust_store_error(std::format("could not inspect {}", store_name), ec.message()));
    }
    if (!std::filesystem::exists(status)) {
        return data;
    }
    if (std::filesystem::is_symlink(status)) {
        return std::unexpected(
                trust_store_error(std::format("refusing to read symlinked {}", store_name), path.string()));
    }
    if (!std::filesystem::is_regular_file(status)) {
        return std::unexpected(trust_store_error(std::format("{} is not a regular file", store_name), path.string()));
    }

    struct stat st{};
    if (::lstat(path.c_str(), &st) == 0) {
        if ((st.st_mode & S_IWGRP) != 0 || (st.st_mode & S_IWOTH) != 0) {
            return std::unexpected(
                    trust_store_error(std::format("{} is writable by group or others", store_name), path.string()));
        }
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::unexpected(trust_store_error(std::format("could not open {}", store_name), path.string()));
    }
    std::stringstream buffer;
    buffer << input.rdbuf();
    auto parsed = support::read_json(buffer.str());
    if (!parsed) {
        return std::unexpected(trust_store_error(std::format("failed to parse {}", store_name), parsed.error().detail));
    }
    if (!parsed->holds<support::JsonValue::object_t>()) {
        return std::unexpected(
                trust_store_error(std::format("invalid {}: expected object", store_name), path.string()));
    }

    for (const auto& [key, value] : parsed->get_object()) {
        if (const auto* flag = value.get_if<bool>()) {
            data[key] = *flag;
        } else if (value.holds<support::JsonValue::null_t>()) {
            data[key] = std::nullopt;
        } else {
            return std::unexpected(
                    trust_store_error(std::format("invalid {} value for key: {}", store_name, key), path.string()));
        }
    }
    return data;
}

support::ExpectedVoid write_trust_store_map(
        const std::filesystem::path& path, const TrustStoreMap& data, std::string_view store_name) {
    if (path.empty()) {
        return std::unexpected(trust_store_error(std::format("{} path is empty", store_name)));
    }
    std::error_code ec;
    auto parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            return std::unexpected(
                    trust_store_error(std::format("could not create {} directory", store_name), ec.message()));
        }
        (void)::chmod(parent.c_str(), 0700);
    }

    auto status = std::filesystem::symlink_status(path, ec);
    if (!ec && std::filesystem::is_symlink(status)) {
        return std::unexpected(
                trust_store_error(std::format("refusing to write symlinked {}", store_name), path.string()));
    }

    support::JsonValue::object_t object;
    for (const auto& [key, value] : data) {
        if (!value.has_value()) {
            continue;
        }
        object.emplace(key, support::JsonValue{*value});
    }
    auto serialized = support::write_json(support::JsonValue{std::move(object)});
    if (!serialized) {
        return std::unexpected(serialized.error());
    }
    serialized->push_back('\n');

    auto tmp = path;
    tmp += ".tmp";
    {
        std::ofstream output(tmp, std::ios::binary | std::ios::trunc);
        if (!output) {
            return std::unexpected(
                    trust_store_error(std::format("could not open temporary {}", store_name), tmp.string()));
        }
        output << *serialized;
        if (!output) {
            return std::unexpected(
                    trust_store_error(std::format("could not write temporary {}", store_name), tmp.string()));
        }
    }
    (void)::chmod(tmp.c_str(), 0600);
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return std::unexpected(trust_store_error(std::format("could not replace {}", store_name), ec.message()));
    }
    (void)::chmod(path.c_str(), 0600);
    return {};
}

} // namespace cch::coding_agent
