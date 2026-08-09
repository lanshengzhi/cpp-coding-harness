#pragma once

#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

namespace cch::coding_agent {

/// The `.gitignore`/`.ignore`/`.fdignore` walk matcher implementing pi's
/// prefix/negation semantics (`core/package-manager.ts` `prefixIgnorePattern`
/// + `addIgnoreRules` over the npm `ignore` package).
///
/// Rules are added per directory as the loader walk descends: each rule is
/// prefixed with the directory's posix path relative to the scan root, so a
/// subdirectory ignore file's patterns apply beneath that subdirectory.
/// Patterns are evaluated in order and the last matching rule wins, with `!`
/// negation re-including; a pattern without a slash matches the basename at
/// any depth, and a trailing `/` matches directories only. This is a strict
/// subset of gitignore glob syntax: `*` (within a segment), `**` (across
/// segments), `?`, `[...]` character classes, and `\` escapes.
class IgnoreMatcher {
public:
    /// Add the rules of one ignore-file content, prefixing every pattern with
    /// `prefix` (the directory's posix path relative to the scan root, with
    /// a trailing `/`; empty for the scan root itself). Mirrors pi
    /// `addIgnoreRules` line handling: blank lines and `#` comments (unless
    /// `\#`-escaped) are skipped, `!` negates, `\!` escapes a literal bang,
    /// and a leading `/` is stripped (the prefix re-anchors it).
    void add_rules(std::string_view content, std::string_view prefix);

    /// True when `rel_path` (posix, relative to the scan root, no leading
    /// `./`) is ignored; `is_dir` selects the trailing-slash directory form
    /// (pi checks directories as `relPath + "/"`). Last matching rule wins;
    /// no match means not ignored.
    [[nodiscard]] bool ignores(std::string_view rel_path, bool is_dir) const;

private:
    struct Rule {
        std::regex pattern;
        bool negated{false};
        bool dir_only{false};
        bool basename_match{false};
    };

    std::vector<Rule> rules_;
};

/// pi `prefixIgnorePattern`: transform one ignore-file line into a scan-root
/// anchored pattern, or return nullopt when the line carries no rule (blank
/// or comment). `prefix` is the directory's posix path relative to the scan
/// root with a trailing `/` (empty for the scan root).
[[nodiscard]] std::optional<std::string> prefix_ignore_pattern(
    std::string_view line,
    std::string_view prefix);

} // namespace cch::coding_agent
