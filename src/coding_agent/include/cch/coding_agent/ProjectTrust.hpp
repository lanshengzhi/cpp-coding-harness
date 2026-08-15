#pragma once

#include <cch/support/Error.hpp>

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

/// pi `ProjectTrustOption` (`core/trust-manager.ts`): one `getProjectTrustOptions`
/// choice presented by the boot trust prompt and the `/trust` selector. The
/// option's `updates` persist the decision (empty for the session-only
/// options); `savedPath` marks the store entry the option corresponds to.
struct ProjectTrustOption {
    std::string label;
    bool trusted{false};
    std::vector<ProjectTrustUpdate> updates;
    std::optional<std::string> saved_path;
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

    [[nodiscard]] support::Expected<std::optional<ProjectTrustStoreEntry>> getEntry(
        const std::filesystem::path& cwd) const;

    [[nodiscard]] support::ExpectedVoid setMany(const std::vector<ProjectTrustUpdate>& updates) const;

private:
    std::filesystem::path trust_path_;
};

[[nodiscard]] std::string to_string(ProjectTrustDecision decision);
[[nodiscard]] std::string to_string(DefaultProjectTrust trust);
[[nodiscard]] std::string to_string(ProjectTrustSource source);

[[nodiscard]] std::optional<DefaultProjectTrust> parse_default_project_trust(const std::string& value);

/// pi `getProjectTrustOptions` (`core/trust-manager.ts`): the choices shown
/// by the boot trust prompt and the `/trust` selector — "Trust", "Trust
/// parent folder (<parent>)" (when a parent exists), and "Do not trust",
/// plus the session-only variants when `include_session_only` is set (the
/// boot prompt passes it; the `/trust` selector does not).
[[nodiscard]] std::vector<ProjectTrustOption> get_project_trust_options(
    const std::filesystem::path& cwd,
    bool include_session_only = false);

[[nodiscard]] ProjectTrustResolution resolve_project_trust(
    const std::filesystem::path& cwd,
    bool has_trust_requiring_resources,
    const ProjectTrustStore& trust_store,
    DefaultProjectTrust default_trust,
    std::optional<bool> trust_override);

} // namespace cch::coding_agent
