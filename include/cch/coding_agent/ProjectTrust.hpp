#pragma once

#include <cch/util/Error.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace cch::coding_agent {

enum class ProjectTrustDecision {
    Trusted,
    Untrusted,
    Unknown,
};

enum class DefaultProjectTrust {
    Ask,
    Always,
    Never,
};

enum class ProjectTrustSource {
    NoProjectResources,
    CliOverride,
    StoreEntry,
    DefaultAlways,
    DefaultNever,
    DefaultAskNoUi,
    StoreUnavailable,
};

enum class ProjectTrustDiagnosticSeverity {
    Info,
    Warning,
    Error,
};

struct ProjectTrustDiagnostic {
    ProjectTrustDiagnosticSeverity severity{ProjectTrustDiagnosticSeverity::Warning};
    std::string code;
    std::string message;
    std::optional<std::string> path;
};

struct ProjectTrustStoreEntry {
    std::string path;
    ProjectTrustDecision decision{ProjectTrustDecision::Unknown};
};

struct ProjectTrustUpdate {
    std::string path;
    ProjectTrustDecision decision{ProjectTrustDecision::Unknown};
};

struct ProjectTrustResolution {
    ProjectTrustDecision decision{ProjectTrustDecision::Unknown};
    ProjectTrustSource source{ProjectTrustSource::DefaultAskNoUi};
    std::optional<std::string> matched_path;
    std::vector<ProjectTrustDiagnostic> diagnostics;
};

class ProjectTrustStore {
public:
    explicit ProjectTrustStore(std::filesystem::path trust_path);

    [[nodiscard]] const std::filesystem::path& path() const { return trust_path_; }

    [[nodiscard]] util::Expected<std::optional<ProjectTrustStoreEntry>> getEntry(
        const std::filesystem::path& cwd) const;

    [[nodiscard]] util::ExpectedVoid setMany(const std::vector<ProjectTrustUpdate>& updates) const;

private:
    std::filesystem::path trust_path_;
};

[[nodiscard]] std::string to_string(ProjectTrustDecision decision);
[[nodiscard]] std::string to_string(DefaultProjectTrust trust);
[[nodiscard]] std::string to_string(ProjectTrustSource source);

[[nodiscard]] std::optional<DefaultProjectTrust> parse_default_project_trust(const std::string& value);

[[nodiscard]] ProjectTrustResolution resolve_project_trust(
    const std::filesystem::path& cwd,
    bool has_trust_requiring_resources,
    const ProjectTrustStore& trust_store,
    DefaultProjectTrust default_trust,
    std::optional<bool> trust_override);

} // namespace cch::coding_agent
