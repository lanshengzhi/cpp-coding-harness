#include "coding_agent/SkillLoader.hpp"

#include "coding_agent/GitIgnoreMatcher.hpp"
#include "LoaderPath.hpp"
#include "coding_agent/SkillFrontmatterParser.hpp"
#include "../harness/WorkspaceFileSystem.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <regex>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace cch::coding_agent {

namespace {

constexpr std::size_t kMaxNameLength = 64;
constexpr std::size_t kMaxDescriptionLength = 1024;

/// The `.gitignore`/`.ignore`/`.fdignore` filenames read at every directory
/// of the walk (pi `IGNORE_FILE_NAMES`).
constexpr std::array<std::string_view, 3> kIgnoreFileNames{
    ".gitignore",
    ".ignore",
    ".fdignore",
};

/// Validate skill name against Agent Skills standard (pi leniency — no
/// parent-directory match requirement, matching `core/skills.ts`
/// `validateName`). Returns diagnostic messages; empty vector means valid.
[[nodiscard]] std::vector<std::string> validateName(
    const std::string& name) {
    std::vector<std::string> errors;

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

/// The parent directory basename of a slash-separated path.
[[nodiscard]] std::string parentDirName(const std::string& filePath) {
    const std::size_t lastSlash = filePath.rfind('/');
    if (lastSlash == std::string::npos || lastSlash == 0) {
        return "";
    }
    const std::size_t secondLastSlash = filePath.rfind('/', lastSlash - 1);
    const std::size_t start =
        (secondLastSlash == std::string::npos) ? 0 : secondLastSlash + 1;
    return filePath.substr(start, lastSlash - start);
}

/// pi `SourceScope`/`SourceInfo` construction for a discovered skill
/// (`core/skills.ts` `createSkillSourceInfo` re-expressed through the loader
/// subset's final `sourceInfo` shape).
[[nodiscard]] SourceInfo make_source_info(
    const std::string& filePath,
    const SkillSourceContext& context) {
    return SourceInfo{
        .path = filePath,
        .source = context.source,
        .scope = context.scope,
        .origin = SourceOrigin::TopLevel,
        .base_dir = context.base_dir,
    };
}

/// pi `ResourceDiagnostic` collision for a duplicate skill name: the first
/// loaded skill wins and the loser carries the winner/loser paths.
[[nodiscard]] SkillDiagnostic skill_collision_diagnostic(
    const std::string& name,
    const std::string& winner_path,
    const std::string& loser_path) {
    return SkillDiagnostic{
        .type = "collision",
        .code = SkillDiagnosticCode::collision,
        .message = "name \"" + name + "\" collision",
        .path = loser_path,
        .collision = ResourceCollision{
            .resource_type = ResourceCollisionResourceType::Skill,
            .name = name,
            .winner_path = winner_path,
            .loser_path = loser_path,
            .winner_source = std::nullopt,
            .loser_source = std::nullopt,
        },
    };
}

/// pi `loadSkills` `addSkills` dedupe: the same real file reached twice (via
/// symlink) is skipped silently; a duplicate name drops the loser with the
/// winner/loser collision diagnostic.
void append_unique_skill(
    std::unordered_map<std::string, std::string>& seenNames,
    std::unordered_set<std::string>& seenRealPaths,
    SkillLoadResult& result,
    Skill skill,
    std::optional<std::string> real_path) {
    if (auto it = seenNames.find(skill.name); it != seenNames.end()) {
        result.diagnostics.push_back(skill_collision_diagnostic(
            skill.name,
            it->second,
            skill.filePath));
        return;
    }
    // The real path joins the set only when the skill is kept (pi `addSkills`
    // records it in the else branch), so a collision loser never shadows a
    // later distinct-named skill that shares its real file.
    if (real_path && !seenRealPaths.insert(*real_path).second) {
        return;
    }
    seenNames.emplace(skill.name, skill.filePath);
    result.skills.push_back(std::move(skill));
}

/// The posix path of `path` relative to `root`, both slash-separated and
/// relative to the same filesystem root. `path` must live under `root`.
[[nodiscard]] std::string relative_posix(std::string_view root, std::string_view path) {
    if (path == root) {
        return {};
    }
    if (root == ".") {
        return path.size() >= 2 && path.starts_with("./") ? std::string{path.substr(2)}
                                                          : std::string{path};
    }
    if (path.starts_with(root) && path.size() > root.size() && path[root.size()] == '/') {
        return std::string{path.substr(root.size() + 1)};
    }
    return std::string{path};
}

/// pi `addIgnoreRules(ig, dir, root)`: read every ignore file in `dir` and
/// add its rules prefixed with the directory's path relative to the scan
/// root. Read failures are silent (pi `try/catch {}`).
void add_ignore_rules(
    const harness::WorkspaceFileSystem& fs,
    const std::string& dirPath,
    std::string_view root,
    IgnoreMatcher& matcher) {
    const auto prefix = [&]() -> std::string {
        const auto rel = relative_posix(root, dirPath);
        if (rel.empty()) {
            return {};
        }
        return rel + "/";
    }();

    // The addressed read path needs the directory prefix without a leading
    // `./` (the scan root is `"."`).
    const std::string dir = dirPath == "." ? std::string{} : dirPath + "/";
    for (const auto filename : kIgnoreFileNames) {
        const auto content = fs.readTextFile(dir + std::string{filename});
        if (content) {
            matcher.add_rules(*content, prefix);
        }
    }
}

} // namespace

// Forward declaration for the recursive directory walk.
static void loadSkillsFromDir(
    const harness::WorkspaceFileSystem& fs,
    const std::string& dirPath,
    const SkillDirSpec& spec,
    std::string_view root,
    IgnoreMatcher& matcher,
    std::unordered_map<std::string, std::string>& seenNames,
    std::unordered_set<std::string>& seenRealPaths,
    SkillLoadResult& result);

SkillLoadResult loadSkillFromFile(
    const harness::WorkspaceFileSystem& fs,
    const std::string& filePath,
    SkillSourceContext source_context) {
    SkillLoadResult result;

    // Resolve absolute path for storage and convert paths beneath the
    // workspace to the relative form required by readTextFile.
    const auto& root = fs.root();
    const auto relative_path = strip_workspace_root(root, filePath);
    const std::string readPath = relative_path.value_or(filePath);
    const std::string absolutePath = std::filesystem::path{filePath}.is_absolute()
        ? filePath
        : (root / filePath).lexically_normal().string();

    auto readResult = fs.readTextFile(readPath);
    if (!readResult.has_value()) {
        result.diagnostics.push_back(SkillDiagnostic{
            .type = "warning",
            .code = SkillDiagnosticCode::read_failed,
            .message = readResult.error().message,
            .path = filePath,
            .collision = std::nullopt,
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
            .collision = std::nullopt,
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
    for (const auto& err : validateName(name)) {
        result.diagnostics.push_back(SkillDiagnostic{
            .type = "warning",
            .code = SkillDiagnosticCode::invalid_metadata,
            .message = err,
            .path = filePath,
            .collision = std::nullopt,
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
            .collision = std::nullopt,
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

    // The skill directory: the base against which the file's relative
    // references resolve (pi `Skill.baseDir`). Root-level .md files get the
    // scan directory itself.
    const std::string baseDir = std::filesystem::path{absolutePath}.parent_path().string();

    result.skills.push_back(Skill{
        .name = std::move(name),
        .description = std::move(description),
        .filePath = absolutePath,
        .baseDir = baseDir,
        .sourceInfo = make_source_info(absolutePath, source_context),
        .disableModelInvocation = disableModelInvocation,
    });

    return result;
}

/// Internal recursive walk: scan a single directory for skills.
///
/// `dirPath` is addressed relative to the filesystem root; `root` is the
/// scan-root spec path the ignore matcher computes rel paths against; the
/// matcher accumulates ignore rules as the walk descends (pi `addIgnoreRules`
/// per directory).
void loadSkillsFromDir(
    const harness::WorkspaceFileSystem& fs,
    const std::string& dirPath,
    const SkillDirSpec& spec,
    std::string_view root,
    IgnoreMatcher& matcher,
    std::unordered_map<std::string, std::string>& seenNames,
    std::unordered_set<std::string>& seenRealPaths,
    SkillLoadResult& result) {

    add_ignore_rules(fs, dirPath, root, matcher);

    auto entriesResult = fs.listDir(dirPath);
    if (!entriesResult.has_value()) {
        result.diagnostics.push_back(SkillDiagnostic{
            .type = "warning",
            .code = SkillDiagnosticCode::list_failed,
            .message = entriesResult.error().message,
            .path = dirPath,
            .collision = std::nullopt,
        });
        return;
    }

    // Sort entries by name for deterministic traversal.
    auto entries = std::move(entriesResult.value());
    std::sort(entries.begin(), entries.end(),
              [](const harness::FileInfo& a, const harness::FileInfo& b) {
                  return a.name < b.name;
              });

    const auto real_path_of = [&](const harness::FileInfo& entry)
        -> std::optional<std::string> {
        // pi `canonicalizePath(skill.filePath)` — used to detect the same
        // real file reached twice via symlinks.
        auto canonical = fs.canonicalPath(entry.path);
        return canonical ? std::optional<std::string>{*canonical} : std::nullopt;
    };

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

        // The ignore matcher prunes ignored SKILL.md files (pi
        // `ig.ignores(relPath)`), which falls through to the recursion pass.
        const auto rel_path = relative_posix(root, dirPath + "/" + entry.name);
        if (matcher.ignores(rel_path, /* is_dir */ false)) continue;

        auto fileResult = loadSkillFromFile(fs, entry.path, spec.source_context);
        // Deduplicate by real path, then by name (first wins; collisions
        // carry winner/loser paths).
        for (auto& skill : fileResult.skills) {
            append_unique_skill(seenNames, seenRealPaths, result, std::move(skill), real_path_of(entry));
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

        const auto rel_path = relative_posix(root, childPath);

        if (kind == harness::FileKind::Directory) {
            // The matcher prunes ignored directories (pi checks dirs with a
            // trailing slash), skipping their entire subtree.
            if (matcher.ignores(rel_path, /* is_dir */ true)) continue;
            // Recurse into subdirectory.
            loadSkillsFromDir(fs, childPath, spec, root, matcher, seenNames, seenRealPaths, result);
        } else if (kind == harness::FileKind::File &&
                   dirPath == root &&
                   spec.include_root_files &&
                   entryName.size() > 3 &&
                   entryName.ends_with(".md")) {
            // Root-level .md file treated as a skill (pi "pi" discovery mode,
            // scan root only).
            if (matcher.ignores(rel_path, /* is_dir */ false)) continue;
            auto fileResult = loadSkillFromFile(fs, childPath, spec.source_context);
            for (auto& skill : fileResult.skills) {
                append_unique_skill(seenNames, seenRealPaths, result, std::move(skill), real_path_of(entry));
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
    std::unordered_map<std::string, std::string> seenNames;
    std::unordered_set<std::string> seenRealPaths;

    for (const auto& dirSpec : dirs) {
        // Convert absolute paths beneath the workspace to relative paths for
        // fileInfo. Absolute paths outside the workspace are inaccessible.
        auto dir_path = strip_workspace_root(fs.root(), dirSpec.path);
        if (!dir_path) {
            continue;
        }

        // Check if the directory exists (skip missing dirs silently).
        auto infoResult = fs.fileInfo(*dir_path);
        if (!infoResult.has_value()) {
            if (infoResult.error().code != harness::FileErrorCode::NotFound) {
                result.diagnostics.push_back(SkillDiagnostic{
                    .type = "warning",
                    .code = SkillDiagnosticCode::file_info_failed,
                    .message = infoResult.error().message,
                    .path = *dir_path,
                    .collision = std::nullopt,
                });
            }
            continue;
        }

        // pi `loadSkills` explicit-path handling: a file spec loads only
        // when it is a `.md` file (anything else is a warning); symlinks are
        // resolved through their canonical target.
        harness::FileKind kind = infoResult->kind;
        std::string specPath = *dir_path;
        if (kind == harness::FileKind::Symlink) {
            if (auto canonical = fs.canonicalPath(specPath); canonical) {
                if (auto target = fs.fileInfo(*canonical); target) {
                    kind = target->kind;
                    specPath = *canonical;
                }
            } else {
                continue;
            }
        }

        if (kind == harness::FileKind::File) {
            if (!specPath.ends_with(".md")) {
                result.diagnostics.push_back(SkillDiagnostic{
                    .type = "warning",
                    .code = SkillDiagnosticCode::invalid_metadata,
                    .message = "skill path is not a markdown file",
                    .path = *dir_path,
                    .collision = std::nullopt,
                });
                continue;
            }
            auto fileResult = loadSkillFromFile(fs, specPath, dirSpec.source_context);
            auto canonical = fs.canonicalPath(specPath);
            for (auto& skill : fileResult.skills) {
                append_unique_skill(
                    seenNames, seenRealPaths, result, std::move(skill),
                    canonical ? std::optional<std::string>{*canonical} : std::nullopt);
            }
            result.diagnostics.insert(
                result.diagnostics.end(),
                std::make_move_iterator(fileResult.diagnostics.begin()),
                std::make_move_iterator(fileResult.diagnostics.end()));
            continue;
        }

        // Skip non-directories (symlinks are handled per-entry in the walk).
        if (kind != harness::FileKind::Directory) {
            continue;
        }

        // Each scan root gets a fresh ignore matcher (pi builds one per
        // `loadSkillsFromDirInternal` call); the walk then accumulates the
        // root's and each descendant's ignore rules.
        IgnoreMatcher matcher;
        loadSkillsFromDir(fs, *dir_path, dirSpec, *dir_path, matcher, seenNames, seenRealPaths, result);
    }

    return result;
}

} // namespace cch::coding_agent
