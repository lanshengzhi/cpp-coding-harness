#include <cch/coding_agent/ProjectTrust.hpp>

#include "coding_agent/TrustStoreFile.hpp"

#include <algorithm>
#include <filesystem>
#include <format>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace cch::coding_agent {
namespace {

/// The project-trust store's name in its own file diagnostics.
constexpr std::string_view kTrustStoreName = "trust store";

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

[[nodiscard]] std::optional<ProjectTrustStoreEntry> find_nearest(
        const TrustStoreMap& data, const std::filesystem::path& cwd) {
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

support::Expected<std::optional<ProjectTrustStoreEntry>> ProjectTrustStore::getEntry(
    const std::filesystem::path& cwd) const {
    auto data = read_trust_store_map(trust_path_, kTrustStoreName);
    if (!data) {
        return std::unexpected(data.error());
    }
    return find_nearest(*data, cwd);
}

support::ExpectedVoid ProjectTrustStore::setMany(const std::vector<ProjectTrustUpdate>& updates) const {
    auto data = read_trust_store_map(trust_path_, kTrustStoreName);
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
    return write_trust_store_map(trust_path_, *data, kTrustStoreName);
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
            .matched_path = {},
            .diagnostics = {},
        };
    }

    if (!has_trust_requiring_resources) {
        return ProjectTrustResolution{
            .decision = ProjectTrustDecision::Trusted,
            .source = ProjectTrustSource::NoProjectResources,
            .matched_path = {},
            .diagnostics = {},
        };
    }

    auto entry = trust_store.getEntry(cwd);
    if (!entry) {
        return ProjectTrustResolution{
            .decision = ProjectTrustDecision::Untrusted,
            .source = ProjectTrustSource::StoreUnavailable,
            .matched_path = {},
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
            .diagnostics = {},
        };
    }

    switch (default_trust) {
    case DefaultProjectTrust::Always:
        return ProjectTrustResolution{
            .decision = ProjectTrustDecision::Trusted,
            .source = ProjectTrustSource::DefaultAlways,
            .matched_path = {},
            .diagnostics = {},
        };
    case DefaultProjectTrust::Never:
        return ProjectTrustResolution{
            .decision = ProjectTrustDecision::Untrusted,
            .source = ProjectTrustSource::DefaultNever,
            .matched_path = {},
            .diagnostics = {},
        };
    case DefaultProjectTrust::Ask:
        return ProjectTrustResolution{
            .decision = ProjectTrustDecision::Untrusted,
            .source = ProjectTrustSource::DefaultAskNoUi,
            .matched_path = {},
            .diagnostics = {},
        };
    }

    return ProjectTrustResolution{
        .decision = ProjectTrustDecision::Untrusted,
        .source = ProjectTrustSource::DefaultAskNoUi,
        .matched_path = {},
        .diagnostics = {},
    };
}

} // namespace cch::coding_agent
