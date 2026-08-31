#pragma once

#include <cch/coding_agent/PromptTemplate.hpp>
#include <cch/agent/harness/FileSystem.hpp>

#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace cch::coding_agent {

/// Diagnostic severity for prompt template loading.
enum class PromptTemplateDiagnosticCode {
    file_info_failed,
    list_failed,
    read_failed,
    parse_failed,
    unsupported_type,
};

/// Warning produced while loading prompt templates.
struct PromptTemplateDiagnostic {
    std::string_view type{"warning"};
    PromptTemplateDiagnosticCode code;
    std::string message;
    std::string path;
};

/// Result of loading prompt templates from one or more directories.
struct PromptTemplateLoadResult {
    std::vector<PromptTemplate> templates;
    std::vector<PromptTemplateDiagnostic> diagnostics;
};

/// A single directory or file specification for prompt template discovery.
struct PromptTemplateDirSpec {
    /// Absolute or workspace-relative path to scan.
    std::string path;
    /// When true, the path is an explicit file (not a directory).
    bool is_file{false};
    /// pi `SourceInfo` provenance for every template loaded from this spec:
    /// "auto" (discovered project/user) or "cli" (explicit path) with the
    /// resource-root baseDir (pi `createPromptSourceInfo`; #418). Absent
    /// specs carry no sourceInfo.
    std::optional<SourceInfo> source_info{std::nullopt};
};

/// Load a single .md prompt template file through the canonical asynchronous
/// filesystem capability.
[[nodiscard]] support::AsyncResult<PromptTemplateLoadResult, harness::FileError> loadPromptTemplateFromFile(
        harness::AsyncFileSystem& fs,
        std::string file_path,
        std::optional<SourceInfo> source_info = std::nullopt,
        std::stop_token stop_token = {});

/// Discover and load prompt templates from one or more directory specs.
///
/// Each PromptTemplateDirSpec specifies a directory to scan (non-recursively)
/// or an explicit file to load. Directories are scanned for direct .md children.
/// Missing directories are silently skipped. Dot-prefixed entries are skipped.
/// Duplicate names are NOT deduplicated here (pi `loadPromptTemplates` returns
/// the raw list); the resource loader resolves collisions with pi-shaped
/// collision diagnostics and winner/loser paths.
[[nodiscard]] support::AsyncResult<PromptTemplateLoadResult, harness::FileError> loadPromptTemplates(
        harness::AsyncFileSystem& fs, std::vector<PromptTemplateDirSpec> dirs, std::stop_token stop_token = {});

} // namespace cch::coding_agent
