#include "coding_agent/SkillLoader.hpp"

#include "SkillFrontmatterParser.hpp"
#include "../harness/WorkspaceFileSystem.hpp"

#include <algorithm>
#include <cctype>
#include <regex>
#include <string>
#include <unordered_set>

namespace cch::coding_agent {

namespace {

constexpr std::size_t kMaxNameLength = 64;
constexpr std::size_t kMaxDescriptionLength = 1024;

/// Validate skill name against Agent Skills standard (with pi leniency).
/// Returns diagnostic messages; empty vector means valid.
[[nodiscard]] std::vector<std::string> validateName(
    const std::string& name,
    const std::string& parentDirName) {
    std::vector<std::string> errors;

    if (name != parentDirName) {
        errors.push_back("name \"" + name + "\" does not match parent directory \"" +
                         parentDirName + "\"");
    }
    if (name.size() > kMaxNameLength) {
        errors.push_back("name exceeds " + std::to_string(kMaxNameLength) +
                         " characters (" + std::to_string(name.size()) + ")");
    }
    // Must be lowercase a-z, 0-9, hyphens only.
    if (!std::all_of(name.begin(), name.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        })) {
        errors.push_back(
            "name contains invalid characters "
            "(must be lowercase a-z, 0-9, hyphens only)");
    }
    if (!name.empty() && name.front() == '-') {
        errors.push_back("name must not start with a hyphen");
    }
    if (!name.empty() && name.back() == '-') {
        errors.push_back("name must not end with a hyphen");
    }
    if (name.find("--") != std::string::npos) {
        errors.push_back("name must not contain consecutive hyphens");
    }

    return errors;
}

/// Validate skill description. Empty/missing → reject; >1024 chars → warn.
/// Returns diagnostic messages; empty vector means valid.
[[nodiscard]] std::vector<std::string> validateDescription(
    const std::string& description) {
    std::vector<std::string> errors;
    if (description.empty()) {
        errors.push_back("description is required");
    } else if (description.size() > kMaxDescriptionLength) {
        errors.push_back("description exceeds " +
                         std::to_string(kMaxDescriptionLength) +
                         " characters (" + std::to_string(description.size()) + ")");
    }
    return errors;
}

/// Extract parent directory basename from a file path.
[[nodiscard]] std::string parentDirName(const std::string& filePath) {
    // Find the last '/' to get the parent directory.
    std::size_t lastSlash = filePath.rfind('/');
    if (lastSlash == std::string::npos || lastSlash == 0) {
        return "";
    }
    // The parent directory name is between the second-to-last '/' and last '/'.
    std::size_t secondLastSlash = filePath.rfind('/', lastSlash - 1);
    std::size_t start = (secondLastSlash == std::string::npos) ? 0 : secondLastSlash + 1;
    return filePath.substr(start, lastSlash - start);
}

} // namespace

// Forward declaration for recursive directory walk.
static void loadSkillsFromDir(
    const harness::WorkspaceFileSystem& fs,
    const std::string& dirPath,
    bool includeRootFiles,
    std::unordered_set<std::string>& seenNames,
    SkillLoadResult& result);

SkillLoadResult loadSkillFromFile(
    const harness::WorkspaceFileSystem& fs,
    const std::string& filePath) {
    SkillLoadResult result;

    // Resolve absolute path for storage and convert to workspace-relative
    // for readTextFile (which requires relative paths).
    std::string readPath = filePath;
    std::string absolutePath = filePath;

    // If the path is absolute, make it relative to the workspace root.
    const auto& root = fs.root();
    std::string rootStr = root.string();
    if (filePath.starts_with("/") && filePath.starts_with(rootStr)) {
        readPath = filePath.substr(rootStr.size());
        if (readPath.starts_with("/")) {
            readPath = readPath.substr(1);
        }
    } else {
        // Relative path: construct absolute for storage.
        absolutePath = (root / filePath).lexically_normal().string();
    }

    auto readResult = fs.readTextFile(readPath);
    if (!readResult.has_value()) {
        result.diagnostics.push_back(SkillDiagnostic{
            .type = "warning",
            .code = SkillDiagnosticCode::read_failed,
            .message = readResult.error().message,
            .path = filePath,
        });
        return result;
    }

    auto parsed = parseFrontmatter(readResult.value());
    if (!parsed.has_value()) {
        result.diagnostics.push_back(SkillDiagnostic{
            .type = "warning",
            .code = SkillDiagnosticCode::parse_failed,
            .message = parsed.error().message,
            .path = filePath,
        });
        return result;
    }

    const auto& fields = parsed->fields;

    // Resolve name: use frontmatter name, or derive from parent dir.
    std::string name;
    auto nameIt = fields.find("name");
    if (nameIt != fields.end()) {
        name = nameIt->second;
    } else {
        name = parentDirName(filePath);
    }

    // Validate name.
    std::string dirName = parentDirName(filePath);
    for (const auto& err : validateName(name, dirName)) {
        result.diagnostics.push_back(SkillDiagnostic{
            .type = "warning",
            .code = SkillDiagnosticCode::invalid_metadata,
            .message = err,
            .path = filePath,
        });
    }

    // Resolve description and trim whitespace.
    std::string description;
    auto descIt = fields.find("description");
    if (descIt != fields.end()) {
        description = descIt->second;
        // Trim leading/trailing whitespace.
        while (!description.empty() && (description.front() == ' ' || description.front() == '\t')) {
            description.erase(0, 1);
        }
        while (!description.empty() && (description.back() == ' ' || description.back() == '\t')) {
            description.pop_back();
        }
    }

    // Validate description. Empty/missing → reject skill.
    auto descErrors = validateDescription(description);
    bool descriptionEmpty = false;
    for (const auto& err : descErrors) {
        if (err == "description is required") {
            descriptionEmpty = true;
        }
        result.diagnostics.push_back(SkillDiagnostic{
            .type = "warning",
            .code = SkillDiagnosticCode::invalid_metadata,
            .message = err,
            .path = filePath,
        });
    }

    if (descriptionEmpty) {
        // Skill rejected — return diagnostics only.
        return result;
    }

    // Resolve disableModelInvocation.
    bool disableModelInvocation = false;
    auto dmiIt = fields.find("disable-model-invocation");
    if (dmiIt != fields.end()) {
        disableModelInvocation = (dmiIt->second == "true");
    }

    result.skills.push_back(Skill{
        .name = std::move(name),
        .description = std::move(description),
        .content = std::move(parsed->body),
        .filePath = absolutePath,
        .disableModelInvocation = disableModelInvocation,
    });

    return result;
}

/// Internal recursive walk: scan a single directory for skills.
void loadSkillsFromDir(
    const harness::WorkspaceFileSystem& fs,
    const std::string& dirPath,
    bool includeRootFiles,
    std::unordered_set<std::string>& seenNames,
    SkillLoadResult& result) {

    auto entriesResult = fs.listDir(dirPath);
    if (!entriesResult.has_value()) {
        result.diagnostics.push_back(SkillDiagnostic{
            .type = "warning",
            .code = SkillDiagnosticCode::list_failed,
            .message = entriesResult.error().message,
            .path = dirPath,
        });
        return;
    }

    // Sort entries by name for deterministic traversal.
    auto entries = std::move(entriesResult.value());
    std::sort(entries.begin(), entries.end(),
              [](const harness::FileInfo& a, const harness::FileInfo& b) {
                  return a.name < b.name;
              });

    // First pass: look for SKILL.md in this directory.
    // Only one SKILL.md per directory (stop recursing after finding one).
    for (const auto& entry : entries) {
        if (entry.name != "SKILL.md") continue;

        // Resolve kind for symlinks.
        if (entry.kind == harness::FileKind::Symlink) {
            auto targetInfo = fs.fileInfo(entry.path); // follow via canonical
            if (!targetInfo.has_value()) continue;
            // Check if the resolved target is a file (canonicalPath handles symlinks).
            auto canonical = fs.canonicalPath(entry.path);
            if (!canonical.has_value()) continue;
            auto resolvedInfo = fs.fileInfo(*canonical);
            if (!resolvedInfo.has_value() || resolvedInfo->kind != harness::FileKind::File) continue;
        } else if (entry.kind != harness::FileKind::File) {
            continue;
        }

        auto fileResult = loadSkillFromFile(fs, entry.path);
        // Deduplicate by name.
        for (auto& skill : fileResult.skills) {
            if (seenNames.contains(skill.name)) {
                result.diagnostics.push_back(SkillDiagnostic{
                    .type = "warning",
                    .code = SkillDiagnosticCode::duplicate_name,
                    .message = "duplicate skill name '" + skill.name + "'",
                    .path = entry.path,
                });
            } else {
                seenNames.insert(skill.name);
                result.skills.push_back(std::move(skill));
            }
        }
        result.diagnostics.insert(result.diagnostics.end(),
                                  std::make_move_iterator(fileResult.diagnostics.begin()),
                                  std::make_move_iterator(fileResult.diagnostics.end()));
        return; // One SKILL.md per directory — stop recursing.
    }

    // Second pass: recurse into subdirectories and load root .md files.
    for (const auto& entry : entries) {
        // Skip hidden entries and special directories.
        if (entry.name.empty() || entry.name[0] == '.') continue;
        if (entry.name == "node_modules") continue;

        // Resolve kind for symlinks.
        harness::FileKind kind = entry.kind;
        std::string entryName = entry.name;
        if (kind == harness::FileKind::Symlink) {
            auto canonical = fs.canonicalPath(entry.path);
            if (!canonical.has_value()) continue;
            auto targetInfo = fs.fileInfo(*canonical);
            if (!targetInfo.has_value()) continue;
            kind = targetInfo->kind;
            entryName = targetInfo->name;
        }

        // Construct relative child path from the parent dir and entry name.
        std::string childPath = dirPath + "/" + entryName;

        if (kind == harness::FileKind::Directory) {
            // Recurse into subdirectory.
            loadSkillsFromDir(fs, childPath, false, seenNames, result);
        } else if (kind == harness::FileKind::File &&
                   includeRootFiles &&
                   entryName.size() > 3 &&
                   entryName.ends_with(".md")) {
            // Root-level .md file treated as a skill (global dir behavior).
            auto fileResult = loadSkillFromFile(fs, childPath);
            for (auto& skill : fileResult.skills) {
                if (seenNames.contains(skill.name)) {
                    result.diagnostics.push_back(SkillDiagnostic{
                        .type = "warning",
                        .code = SkillDiagnosticCode::duplicate_name,
                        .message = "duplicate skill name '" + skill.name + "'",
                        .path = childPath,
                    });
                } else {
                    seenNames.insert(skill.name);
                    result.skills.push_back(std::move(skill));
                }
            }
            result.diagnostics.insert(result.diagnostics.end(),
                                      std::make_move_iterator(fileResult.diagnostics.begin()),
                                      std::make_move_iterator(fileResult.diagnostics.end()));
        }
    }
}

SkillLoadResult loadSkills(
    const harness::WorkspaceFileSystem& fs,
    const std::vector<SkillDirSpec>& dirs) {
    SkillLoadResult result;
    std::unordered_set<std::string> seenNames;

    for (const auto& dirSpec : dirs) {
        // Convert absolute paths to workspace-relative for fileInfo.
        std::string dirPath = dirSpec.path;
        const auto& root = fs.root();
        std::string rootStr = root.string();
        if (dirPath.starts_with("/") && dirPath.starts_with(rootStr)) {
            dirPath = dirPath.substr(rootStr.size());
            if (dirPath.starts_with("/")) dirPath = dirPath.substr(1);
        }
        // If absolute but not under workspace root, skip (can't access).
        if (dirPath.starts_with("/")) {
            continue;
        }

        // Check if the directory exists (skip missing dirs silently).
        auto infoResult = fs.fileInfo(dirPath);
        if (!infoResult.has_value()) {
            if (infoResult.error().code != harness::FileErrorCode::NotFound) {
                result.diagnostics.push_back(SkillDiagnostic{
                    .type = "warning",
                    .code = SkillDiagnosticCode::file_info_failed,
                    .message = infoResult.error().message,
                    .path = dirPath,
                });
            }
            continue;
        }

        // Skip non-directories (symlinks are handled per-entry in the walk).
        if (infoResult->kind != harness::FileKind::Directory) {
            continue;
        }

        // Walk the directory recursively.
        loadSkillsFromDir(fs, dirPath, dirSpec.includeRootFiles, seenNames, result);
    }

    return result;
}

} // namespace cch::coding_agent
