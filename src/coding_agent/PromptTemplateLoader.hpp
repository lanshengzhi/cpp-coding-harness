#pragma once

#include <cch/coding_agent/PromptTemplate.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace cch::harness {
class WorkspaceFileSystem;
}

namespace cch::coding_agent {

/// Diagnostic severity for prompt template loading.
enum class PromptTemplateDiagnosticCode {
    file_info_failed,
    list_failed,
    read_failed,
    parse_failed,
    duplicate_name,
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
};

/// Load a single .md prompt template file.
///
/// Reads the file via the filesystem, parses YAML frontmatter using the
/// existing SkillFrontmatterParser, and returns a PromptTemplate with
/// name (from filename), optional description, optional argument_hint,
/// and body content. A non-Markdown path returns an `unsupported_type`
/// diagnostic instead of being silently ignored.
[[nodiscard]] PromptTemplateLoadResult loadPromptTemplateFromFile(
    const harness::WorkspaceFileSystem& fs,
    const std::string& filePath);

/// Discover and load prompt templates from one or more directory specs.
///
/// Each PromptTemplateDirSpec specifies a directory to scan (non-recursively)
/// or an explicit file to load. Directories are scanned for direct .md children.
/// Missing directories are silently skipped. Duplicate template names produce
/// diagnostics (first found wins). Dot-prefixed entries are skipped.
[[nodiscard]] PromptTemplateLoadResult loadPromptTemplates(
    const harness::WorkspaceFileSystem& fs,
    const std::vector<PromptTemplateDirSpec>& dirs);

} // namespace cch::coding_agent
