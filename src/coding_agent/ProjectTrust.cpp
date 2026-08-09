#include "../../include/cch/coding_agent/ProjectTrust.hpp"
#include "util/Json.hpp"

#include <algorithm>
#include <cerrno>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <system_error>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/stat.h>
#endif

namespace cch::coding_agent {
namespace {

[[nodiscard]] util::Error trust_error(std::string message, std::string detail = {}) {
    return util::make_error(
        util::ErrorCode::Validation,
        std::move(message),
        detail.empty() ? message : std::move(detail));
}

[[nodiscard]] std::filesystem::path canonicalize(const std::filesystem::path& path) {
    std::error_code ec;
    auto canonical = std::filesystem::weakly_canonical(path, ec);
    if (!ec) {
        return canonical.lexically_normal();
    }
    auto absolute = std::filesystem::absolute(path, ec);
    if (!ec) {
        return absolute.lexically_normal();
    }
    return path.lexically_normal();
}

using TrustMap = std::map<std::string, std::optional<bool>>;

[[nodiscard]] util::Expected<TrustMap> read_trust_map(const std::filesystem::path& path) {
    TrustMap data;
    if (path.empty()) {
        return std::unexpected(trust_error("trust store path is empty"));
    }

    std::error_code ec;
    auto status = std::filesystem::symlink_status(path, ec);
    if (ec) {
        if (ec.default_error_condition() == std::errc::no_such_file_or_directory ||
            ec.default_error_condition() == std::errc::not_a_directory) {
            return data;
        }
        return std::unexpected(trust_error("could not inspect trust store", ec.message()));
    }
    if (!std::filesystem::exists(status)) {
        return data;
    }
    if (std::filesystem::is_symlink(status)) {
        return std::unexpected(trust_error("refusing to read symlinked trust store", path.string()));
    }
    if (!std::filesystem::is_regular_file(status)) {
        return std::unexpected(trust_error("trust store is not a regular file", path.string()));
    }

#if defined(__unix__) || defined(__APPLE__)
    struct stat st {};
    if (::lstat(path.c_str(), &st) == 0) {
        if ((st.st_mode & S_IWGRP) != 0 || (st.st_mode & S_IWOTH) != 0) {
            return std::unexpected(trust_error("trust store is writable by group or others", path.string()));
        }
    }
#endif

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::unexpected(trust_error("could not open trust store", path.string()));
    }
    std::stringstream buffer;
    buffer << input.rdbuf();
    auto parsed = util::read_json<util::JsonValue>(buffer.str());
    if (!parsed) {
        return std::unexpected(trust_error("failed to parse trust store", parsed.error().detail));
    }
    if (!parsed->holds<util::JsonValue::object_t>()) {
        return std::unexpected(trust_error("invalid trust store: expected object", path.string()));
    }

    for (const auto& [key, value] : parsed->get_object()) {
        if (const auto* flag = value.get_if<bool>()) {
            data[key] = *flag;
        } else if (value.holds<util::JsonValue::null_t>()) {
            data[key] = std::nullopt;
        } else {
            return std::unexpected(trust_error("invalid trust store value for path: " + key, path.string()));
        }
    }
    return data;
}

[[nodiscard]] util::ExpectedVoid write_trust_map(const std::filesystem::path& path, const TrustMap& data) {
    if (path.empty()) {
        return std::unexpected(trust_error("trust store path is empty"));
    }
    std::error_code ec;
    auto parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            return std::unexpected(trust_error("could not create trust store directory", ec.message()));
        }
#if defined(__unix__) || defined(__APPLE__)
        (void)::chmod(parent.c_str(), 0700);
#endif
    }

    auto status = std::filesystem::symlink_status(path, ec);
    if (!ec && std::filesystem::is_symlink(status)) {
        return std::unexpected(trust_error("refusing to write symlinked trust store", path.string()));
    }

    util::JsonValue::object_t object;
    for (const auto& [key, value] : data) {
        if (!value.has_value()) {
            continue;
        }
        object.emplace(key, util::JsonValue{*value});
    }
    auto serialized = util::write_json(util::JsonValue{std::move(object)});
    if (!serialized) {
        return std::unexpected(serialized.error());
    }
    serialized->push_back('\n');

    auto tmp = path;
    tmp += ".tmp";
    {
        std::ofstream output(tmp, std::ios::binary | std::ios::trunc);
        if (!output) {
            return std::unexpected(trust_error("could not open temporary trust store", tmp.string()));
        }
        output << *serialized;
        if (!output) {
            return std::unexpected(trust_error("could not write temporary trust store", tmp.string()));
        }
    }
#if defined(__unix__) || defined(__APPLE__)
    (void)::chmod(tmp.c_str(), 0600);
#endif
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return std::unexpected(trust_error("could not replace trust store", ec.message()));
    }
#if defined(__unix__) || defined(__APPLE__)
    (void)::chmod(path.c_str(), 0600);
#endif
    return {};
}

[[nodiscard]] std::optional<ProjectTrustStoreEntry> find_nearest(
    const TrustMap& data,
    const std::filesystem::path& cwd) {
    auto current = canonicalize(cwd);
    while (true) {
        const auto key = current.string();
        if (auto it = data.find(key); it != data.end() && it->second.has_value()) {
            return ProjectTrustStoreEntry{
                .path = key,
                .decision = *it->second ? ProjectTrustDecision::Trusted : ProjectTrustDecision::Untrusted,
            };
        }
        auto parent = current.parent_path();
        if (parent == current || parent.empty()) {
            return std::nullopt;
        }
        current = parent;
    }
}

} // namespace

ProjectTrustStore::ProjectTrustStore(std::filesystem::path trust_path)
    : trust_path_(std::move(trust_path)) {}

util::Expected<std::optional<ProjectTrustStoreEntry>> ProjectTrustStore::getEntry(
    const std::filesystem::path& cwd) const {
    auto data = read_trust_map(trust_path_);
    if (!data) {
        return std::unexpected(data.error());
    }
    return find_nearest(*data, cwd);
}

util::ExpectedVoid ProjectTrustStore::setMany(const std::vector<ProjectTrustUpdate>& updates) const {
    auto data = read_trust_map(trust_path_);
    if (!data) {
        return std::unexpected(data.error());
    }
    for (const auto& update : updates) {
        const auto key = canonicalize(update.path).string();
        if (update.decision == ProjectTrustDecision::Unknown) {
            data->erase(key);
        } else {
            (*data)[key] = update.decision == ProjectTrustDecision::Trusted;
        }
    }
    return write_trust_map(trust_path_, *data);
}

std::string to_string(ProjectTrustDecision decision) {
    switch (decision) {
    case ProjectTrustDecision::Trusted:
        return "trusted";
    case ProjectTrustDecision::Untrusted:
        return "untrusted";
    case ProjectTrustDecision::Unknown:
        return "unknown";
    }
    return "unknown";
}

std::string to_string(DefaultProjectTrust trust) {
    switch (trust) {
    case DefaultProjectTrust::Ask:
        return "ask";
    case DefaultProjectTrust::Always:
        return "always";
    case DefaultProjectTrust::Never:
        return "never";
    }
    return "ask";
}

std::string to_string(ProjectTrustSource source) {
    switch (source) {
    case ProjectTrustSource::NoProjectResources:
        return "no_project_resources";
    case ProjectTrustSource::CliOverride:
        return "cli_override";
    case ProjectTrustSource::StoreEntry:
        return "store_entry";
    case ProjectTrustSource::DefaultAlways:
        return "default_always";
    case ProjectTrustSource::DefaultNever:
        return "default_never";
    case ProjectTrustSource::DefaultAskNoUi:
        return "default_ask_no_ui";
    case ProjectTrustSource::StoreUnavailable:
        return "store_unavailable";
    }
    return "unknown";
}

std::optional<DefaultProjectTrust> parse_default_project_trust(const std::string& value) {
    if (value == "ask") {
        return DefaultProjectTrust::Ask;
    }
    if (value == "always") {
        return DefaultProjectTrust::Always;
    }
    if (value == "never") {
        return DefaultProjectTrust::Never;
    }
    return std::nullopt;
}

std::vector<ProjectTrustOption> get_project_trust_options(
    const std::filesystem::path& cwd,
    bool include_session_only) {
    const auto trust_path = canonicalize(cwd).string();
    std::vector<ProjectTrustOption> options;
    options.push_back(ProjectTrustOption{
        .label = "Trust",
        .trusted = true,
        .updates = {{.path = trust_path, .decision = ProjectTrustDecision::Trusted}},
        .saved_path = trust_path,
    });

    // pi `getProjectTrustParentPath`: the parent directory, absent at the
    // filesystem root.
    const auto parent = std::filesystem::path{trust_path}.parent_path();
    if (!parent.empty() && parent != std::filesystem::path{trust_path}) {
        const auto parent_string = parent.string();
        options.push_back(ProjectTrustOption{
            .label = std::format("Trust parent folder ({})", parent_string),
            .trusted = true,
            .updates = {
                {.path = parent_string, .decision = ProjectTrustDecision::Trusted},
                {.path = trust_path, .decision = ProjectTrustDecision::Unknown},
            },
            .saved_path = parent_string,
        });
    }
    if (include_session_only) {
        options.push_back(ProjectTrustOption{
            .label = "Trust (this session only)",
            .trusted = true,
            .updates = {},
            .saved_path = std::nullopt,
        });
    }
    options.push_back(ProjectTrustOption{
        .label = "Do not trust",
        .trusted = false,
        .updates = {{.path = trust_path, .decision = ProjectTrustDecision::Untrusted}},
        .saved_path = trust_path,
    });
    if (include_session_only) {
        options.push_back(ProjectTrustOption{
            .label = "Do not trust (this session only)",
            .trusted = false,
            .updates = {},
            .saved_path = std::nullopt,
        });
    }
    return options;
}

ProjectTrustResolution resolve_project_trust(
    const std::filesystem::path& cwd,
    bool has_trust_requiring_resources,
    const ProjectTrustStore& trust_store,
    DefaultProjectTrust default_trust,
    std::optional<bool> trust_override) {
    if (trust_override.has_value()) {
        return ProjectTrustResolution{
            .decision = *trust_override ? ProjectTrustDecision::Trusted : ProjectTrustDecision::Untrusted,
            .source = ProjectTrustSource::CliOverride,
        };
    }

    if (!has_trust_requiring_resources) {
        return ProjectTrustResolution{
            .decision = ProjectTrustDecision::Trusted,
            .source = ProjectTrustSource::NoProjectResources,
        };
    }

    auto entry = trust_store.getEntry(cwd);
    if (!entry) {
        return ProjectTrustResolution{
            .decision = ProjectTrustDecision::Untrusted,
            .source = ProjectTrustSource::StoreUnavailable,
            .diagnostics = {ProjectTrustDiagnostic{
                .severity = ProjectTrustDiagnosticSeverity::Warning,
                .code = "trust_store_unavailable",
                .message = entry.error().message,
                .path = trust_store.path().string(),
            }},
        };
    }
    if (entry->has_value()) {
        return ProjectTrustResolution{
            .decision = (*entry)->decision,
            .source = ProjectTrustSource::StoreEntry,
            .matched_path = (*entry)->path,
        };
    }

    switch (default_trust) {
    case DefaultProjectTrust::Always:
        return ProjectTrustResolution{
            .decision = ProjectTrustDecision::Trusted,
            .source = ProjectTrustSource::DefaultAlways,
        };
    case DefaultProjectTrust::Never:
        return ProjectTrustResolution{
            .decision = ProjectTrustDecision::Untrusted,
            .source = ProjectTrustSource::DefaultNever,
        };
    case DefaultProjectTrust::Ask:
        return ProjectTrustResolution{
            .decision = ProjectTrustDecision::Untrusted,
            .source = ProjectTrustSource::DefaultAskNoUi,
        };
    }

    return ProjectTrustResolution{
        .decision = ProjectTrustDecision::Untrusted,
        .source = ProjectTrustSource::DefaultAskNoUi,
    };
}

} // namespace cch::coding_agent
