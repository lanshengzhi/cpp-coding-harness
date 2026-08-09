#include "coding_agent/PromptTemplateLoader.hpp"

#include "SkillFrontmatterParser.hpp"
#include "LoaderPath.hpp"
#include "../harness/WorkspaceFileSystem.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>

namespace cch::coding_agent {

namespace {

[[nodiscard]] bool hasMarkdownExtension(std::string_view path) {
    if (path.size() < 3) {
        return false;
    }
    const auto suffix = path.substr(path.size() - 3);
    return suffix[0] == '.' &&
           (suffix[1] == 'm' || suffix[1] == 'M') &&
           (suffix[2] == 'd' || suffix[2] == 'D');
}

/// Strip .md extension from a filename to derive the template name.
[[nodiscard]] std::string templateNameFromPath(const std::string& path) {
    // Find the last path separator to extract the filename.
    auto slash = path.rfind('/');
    std::string_view filename = (slash == std::string::npos)
        ? std::string_view(path)
        : std::string_view(path).substr(slash + 1);

    // Strip .md extension (case-insensitive).
    if (hasMarkdownExtension(filename)) {
        filename = filename.substr(0, filename.size() - 3);
    }
    return std::string(filename);
}

/// Check if a filename starts with a dot.
[[nodiscard]] bool isDotfile(std::string_view name) {
    return !name.empty() && name.front() == '.';
}

} // namespace

PromptTemplateLoadResult loadPromptTemplateFromFile(
    const harness::WorkspaceFileSystem& fs,
    const std::string& filePath,
    const std::optional<SourceInfo>& source_info) {
    PromptTemplateLoadResult result;

    // Only load .md files. Directory discovery filters other entries before
    // calling this function; an explicit non-Markdown input must report why it
    // could not be loaded so session assembly can fail consistently.
    if (!hasMarkdownExtension(filePath)) {
        result.diagnostics.push_back(PromptTemplateDiagnostic{
            .type = "warning",
            .code = PromptTemplateDiagnosticCode::unsupported_type,
            .message = "prompt template file must use a .md extension",
            .path = filePath,
        });
        return result;
    }

    // Read the file.
    auto content = fs.readTextFile(filePath);
    if (!content) {
        result.diagnostics.push_back(PromptTemplateDiagnostic{
            .type = "warning",
            .code = PromptTemplateDiagnosticCode::read_failed,
            .message = content.error().message,
            .path = filePath,
        });
        return result;
    }

    // Parse frontmatter.
    auto parsed = parseFrontmatter(*content);
    if (!parsed) {
        result.diagnostics.push_back(PromptTemplateDiagnostic{
            .type = "warning",
            .code = PromptTemplateDiagnosticCode::parse_failed,
            .message = parsed.error().message,
            .path = filePath,
        });
        return result;
    }

    const auto& fm = *parsed;

    // Extract name from filename.
    std::string name = templateNameFromPath(filePath);

    // Extract description from frontmatter fields.
    std::optional<std::string> description;
    auto descIt = fm.fields.find("description");
    if (descIt != fm.fields.end() && !descIt->second.empty()) {
        description = descIt->second;
    }

    // Extract argument-hint from frontmatter fields.
    std::optional<std::string> argument_hint;
    auto hintIt = fm.fields.find("argument-hint");
    if (hintIt != fm.fields.end() && !hintIt->second.empty()) {
        argument_hint = hintIt->second;
    }

    const std::filesystem::path absolute_path = std::filesystem::path{filePath}.is_absolute()
        ? std::filesystem::path{filePath}
        : (fs.root() / filePath);
    const auto normalized_path = absolute_path.lexically_normal().string();
    // pi `createPromptSourceInfo`: the spec's provenance with the per-file
    // path (pi `SourceInfo.path`); absent specs carry an empty SourceInfo.
    SourceInfo template_source;
    if (source_info) {
        template_source = *source_info;
        template_source.path = normalized_path;
    }

    result.templates.push_back(PromptTemplate{
        .name = std::move(name),
        .description = std::move(description),
        .content = std::move(fm.body),
        .argument_hint = std::move(argument_hint),
        .filePath = normalized_path,
        .sourceInfo = std::move(template_source),
    });

    return result;
}

PromptTemplateLoadResult loadPromptTemplates(
    const harness::WorkspaceFileSystem& fs,
    const std::vector<PromptTemplateDirSpec>& dirs) {
    PromptTemplateLoadResult result;

    for (const auto& spec : dirs) {
        if (spec.is_file) {
            // Explicit file path — load directly.
            auto file_result = loadPromptTemplateFromFile(fs, spec.path, spec.source_info);
            result.templates.insert(
                result.templates.end(),
                std::make_move_iterator(file_result.templates.begin()),
                std::make_move_iterator(file_result.templates.end()));
            result.diagnostics.insert(result.diagnostics.end(),
                std::make_move_iterator(file_result.diagnostics.begin()),
                std::make_move_iterator(file_result.diagnostics.end()));
            continue;
        }

        // Directory — list direct children.
        auto entries = fs.listDir(spec.path);
        if (!entries) {
            // Missing directory is silent (not an error).
            if (entries.error().code != harness::FileErrorCode::NotFound) {
                result.diagnostics.push_back(PromptTemplateDiagnostic{
                    .type = "warning",
                    .code = PromptTemplateDiagnosticCode::list_failed,
                    .message = entries.error().message,
                    .path = spec.path,
                });
            }
            continue;
        }

        // Sort entries by name for deterministic order.
        std::sort(entries->begin(), entries->end(),
            [](const harness::FileInfo& a, const harness::FileInfo& b) { return a.name < b.name; });

        for (const auto& entry : *entries) {
            // Skip dot-prefixed entries and non-files.
            if (isDotfile(entry.name)) continue;
            if (entry.kind != harness::FileKind::File) continue;
            // Only load .md files.
            if (!hasMarkdownExtension(entry.name)) continue;

            // listDir returns absolute entry paths; convert paths beneath the
            // workspace to the relative form required by the file loader.
            const auto stripped_path = strip_workspace_root(fs.root(), entry.path);
            const auto relative_path = stripped_path.value_or(spec.path + "/" + entry.name);

            auto file_result = loadPromptTemplateFromFile(fs, relative_path, spec.source_info);

            result.templates.insert(
                result.templates.end(),
                std::make_move_iterator(file_result.templates.begin()),
                std::make_move_iterator(file_result.templates.end()));
            result.diagnostics.insert(result.diagnostics.end(),
                std::make_move_iterator(file_result.diagnostics.begin()),
                std::make_move_iterator(file_result.diagnostics.end()));
        }
    }

    // Sort templates by name for deterministic output.
    std::sort(result.templates.begin(), result.templates.end(),
        [](const PromptTemplate& a, const PromptTemplate& b) { return a.name < b.name; });

    return result;
}

} // namespace cch::coding_agent
