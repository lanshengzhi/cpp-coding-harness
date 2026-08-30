#include "coding_agent/PromptTemplateLoader.hpp"

#include "AsyncTask.hpp"
#include "SkillFrontmatterParser.hpp"
#include "LoaderPath.hpp"
#include "agent/harness/WorkspaceFileSystem.hpp"

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
    auto async_filesystem = detail::make_sync_async_filesystem(fs);
    auto completed = detail::run_sync_bridge(loadPromptTemplateFromFile(
        *async_filesystem, filePath, source_info));
    if (!completed) {
        return PromptTemplateLoadResult{};
    }
    if (!*completed) {
        PromptTemplateLoadResult result;
        result.diagnostics.push_back(PromptTemplateDiagnostic{
            .type = "warning",
            .code = PromptTemplateDiagnosticCode::read_failed,
            .message = completed->error().message,
            .path = filePath,
        });
        return result;
    }
    return std::move(**completed);
}

PromptTemplateLoadResult loadPromptTemplates(
    const harness::WorkspaceFileSystem& fs,
    const std::vector<PromptTemplateDirSpec>& dirs) {
    auto async_filesystem = detail::make_sync_async_filesystem(fs);
    auto completed = detail::run_sync_bridge(loadPromptTemplates(
        *async_filesystem, std::vector<PromptTemplateDirSpec>{dirs.begin(), dirs.end()}));
    if (!completed) {
        return PromptTemplateLoadResult{};
    }
    if (!*completed) {
        PromptTemplateLoadResult result;
        result.diagnostics.push_back(PromptTemplateDiagnostic{
            .type = "warning",
            .code = PromptTemplateDiagnosticCode::list_failed,
            .message = completed->error().message,
            .path = completed->error().path.value_or(std::string{}),
        });
        return result;
    }
    return std::move(**completed);
}

namespace {

[[nodiscard]] harness::FileError async_prompt_aborted_error(std::string path = {}) {
    return harness::FileError{
            .code = harness::FileErrorCode::Aborted,
            .message = "Operation aborted",
            .path = path.empty() ? std::nullopt : std::optional<std::string>{std::move(path)},
    };
}

[[nodiscard]] bool async_prompt_aborted(const harness::FileError& error) {
    return error.code == harness::FileErrorCode::Aborted;
}

[[nodiscard]] std::string async_prompt_read_path(const harness::AsyncFileSystem& fs, std::string_view file_path) {
    const auto relative_path = strip_workspace_root(fs.workspace(), file_path);
    return relative_path.value_or(std::string{file_path});
}

[[nodiscard]] std::string async_prompt_absolute_path(const harness::AsyncFileSystem& fs, std::string_view file_path) {
    const std::filesystem::path path{file_path};
    if (path.is_absolute()) {
        return path.lexically_normal().string();
    }
    return (fs.workspace() / path).lexically_normal().string();
}

[[nodiscard]] detail::AsyncTask<PromptTemplateLoadResult, harness::FileError> load_prompt_template_file_task(
        harness::AsyncFileSystem& fs,
        std::string file_path,
        std::optional<SourceInfo> source_info,
        std::stop_token stop_token) {
    PromptTemplateLoadResult result;
    if (stop_token.stop_requested()) {
        co_return std::unexpected(async_prompt_aborted_error(file_path));
    }
    if (!hasMarkdownExtension(file_path)) {
        result.diagnostics.push_back(PromptTemplateDiagnostic{
                .type = "warning",
                .code = PromptTemplateDiagnosticCode::unsupported_type,
                .message = "prompt template file must use a .md extension",
                .path = file_path,
        });
        co_return result;
    }

    auto content = co_await std::move(fs.readTextFile(async_prompt_read_path(fs, file_path), stop_token));
    if (!content) {
        if (async_prompt_aborted(content.error())) {
            co_return std::unexpected(std::move(content.error()));
        }
        result.diagnostics.push_back(PromptTemplateDiagnostic{
                .type = "warning",
                .code = PromptTemplateDiagnosticCode::read_failed,
                .message = content.error().message,
                .path = file_path,
        });
        co_return result;
    }

    auto parsed = parseFrontmatter(*content);
    if (!parsed) {
        result.diagnostics.push_back(PromptTemplateDiagnostic{
                .type = "warning",
                .code = PromptTemplateDiagnosticCode::parse_failed,
                .message = parsed.error().message,
                .path = file_path,
        });
        co_return result;
    }

    std::optional<std::string> description;
    if (const auto description_it = parsed->fields.find("description");
            description_it != parsed->fields.end() && !description_it->second.empty()) {
        description = description_it->second;
    }
    std::optional<std::string> argument_hint;
    if (const auto hint_it = parsed->fields.find("argument-hint");
            hint_it != parsed->fields.end() && !hint_it->second.empty()) {
        argument_hint = hint_it->second;
    }

    const auto absolute_path = async_prompt_absolute_path(fs, file_path);
    SourceInfo template_source;
    if (source_info) {
        template_source = std::move(*source_info);
        template_source.path = absolute_path;
    }
    result.templates.push_back(PromptTemplate{
            .name = templateNameFromPath(file_path),
            .description = std::move(description),
            .content = std::move(parsed->body),
            .argument_hint = std::move(argument_hint),
            .filePath = absolute_path,
            .sourceInfo = std::move(template_source),
    });
    co_return result;
}

[[nodiscard]] detail::AsyncTask<PromptTemplateLoadResult, harness::FileError> load_prompt_templates_task(
        harness::AsyncFileSystem& fs, std::vector<PromptTemplateDirSpec> dirs, std::stop_token stop_token) {
    PromptTemplateLoadResult result;
    for (const auto& spec : dirs) {
        if (stop_token.stop_requested()) {
            co_return std::unexpected(async_prompt_aborted_error());
        }
        if (spec.is_file) {
            auto file_result = co_await std::move(
                    to_async_result(load_prompt_template_file_task(fs, spec.path, spec.source_info, stop_token)));
            if (!file_result) {
                co_return std::unexpected(std::move(file_result.error()));
            }
            result.templates.insert(result.templates.end(),
                    std::make_move_iterator(file_result->templates.begin()),
                    std::make_move_iterator(file_result->templates.end()));
            result.diagnostics.insert(result.diagnostics.end(),
                    std::make_move_iterator(file_result->diagnostics.begin()),
                    std::make_move_iterator(file_result->diagnostics.end()));
            continue;
        }

        auto entries = co_await std::move(fs.listDir(async_prompt_read_path(fs, spec.path), stop_token));
        if (!entries) {
            if (async_prompt_aborted(entries.error())) {
                co_return std::unexpected(std::move(entries.error()));
            }
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
        std::sort(entries->begin(), entries->end(), [](const auto& left, const auto& right) {
            return left.name < right.name;
        });
        for (const auto& entry : *entries) {
            if (isDotfile(entry.name) || entry.kind != harness::FileKind::File || !hasMarkdownExtension(entry.name)) {
                continue;
            }
            const auto stripped_path = strip_workspace_root(fs.workspace(), entry.path);
            const auto relative_path = stripped_path.value_or(spec.path + "/" + entry.name);
            auto file_result = co_await std::move(
                    to_async_result(load_prompt_template_file_task(fs, relative_path, spec.source_info, stop_token)));
            if (!file_result) {
                co_return std::unexpected(std::move(file_result.error()));
            }
            result.templates.insert(result.templates.end(),
                    std::make_move_iterator(file_result->templates.begin()),
                    std::make_move_iterator(file_result->templates.end()));
            result.diagnostics.insert(result.diagnostics.end(),
                    std::make_move_iterator(file_result->diagnostics.begin()),
                    std::make_move_iterator(file_result->diagnostics.end()));
        }
    }
    std::sort(result.templates.begin(), result.templates.end(), [](const auto& left, const auto& right) {
        return left.name < right.name;
    });
    co_return result;
}

} // namespace

support::AsyncResult<PromptTemplateLoadResult, harness::FileError> loadPromptTemplateFromFile(
        harness::AsyncFileSystem& fs,
        std::string file_path,
        std::optional<SourceInfo> source_info,
        std::stop_token stop_token) {
    return detail::to_async_result(
            load_prompt_template_file_task(fs, std::move(file_path), std::move(source_info), stop_token));
}

support::AsyncResult<PromptTemplateLoadResult, harness::FileError> loadPromptTemplates(
        harness::AsyncFileSystem& fs, std::vector<PromptTemplateDirSpec> dirs, std::stop_token stop_token) {
    return detail::to_async_result(load_prompt_templates_task(fs, std::move(dirs), stop_token));
}

} // namespace cch::coding_agent
