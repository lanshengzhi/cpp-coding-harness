#pragma once

#include <cch/support/Error.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::durable::gitignore {

/// The tri-state outcome of matching one path against one rule file (frozen
/// `ignore` crate `Match`): no rule matched, an ignore rule matched, or a
/// whitelist (`!`) rule matched.
enum class Match : std::uint8_t {
    None,
    Ignore,
    Whitelist,
};

/// The bounded memory one listing may spend on ignore rules (issue #1198):
/// at most 4 MiB of rule text across every rule file the listing reads, and
/// at most 50,000 rules. Exceeding either fails the listing with a typed
/// Validation error naming the bound; rules are never silently dropped.
struct RuleBudget {
    std::size_t max_rule_bytes{4 * 1024 * 1024};
    std::size_t max_rules{50'000};
    std::size_t rule_bytes{};
    std::size_t rules{};
};

/// One token of a compiled glob (frozen `globset` with `literal_separator`,
/// `backslash_escape` and `allow_unclosed_class`). Matching runs on raw
/// bytes: `*` and `?` never match `/`, character classes are byte ranges, and
/// `**` is recursive only as a whole path component.
struct Token {
    enum class Kind : std::uint8_t {
        Literal,
        AnyByte,
        Star,
        Class,
        RecursivePrefix,
        RecursiveSuffix,
        RecursiveZeroOrMore,
        Alternates,
    };
    Kind kind{Kind::Literal};
    unsigned char byte{};
    bool negated{};
    std::vector<std::pair<unsigned char, unsigned char>> ranges{};
    std::vector<std::vector<Token>> branches{};
};

/// Parses one glob the way the frozen `ignore` crate builds it (the
/// `gitignore.rs` line rewrite, then the `globset` parser). Returns false
/// when the glob is invalid (dangling escape, unopened `}`, invalid range);
/// the caller skips that rule line, exactly like the frozen builder.
[[nodiscard]] bool parse_glob(std::string_view glob, std::vector<Token>& tokens);

/// One `.gitignore`-format rule file rooted at one directory (frozen
/// `Gitignore`): last matching rule wins, a directory-only rule matches only
/// directories, and a whitelist can override an earlier ignore within the
/// same file. The caller owns precedence between files.
class RuleFile {
public:
    /// Adds one rule-file line. Returns a Validation error when the shared
    /// budget is exceeded; an unparseable line is skipped like frozen.
    [[nodiscard]] support::ExpectedVoid add_line(std::string_view line, bool first_line, RuleBudget& budget);

    /// Matches `path` (the `/`-separated relative path, no leading slash)
    /// against the rules, last rule first.
    [[nodiscard]] Match match(std::string_view path, bool is_dir) const;

    [[nodiscard]] bool empty() const noexcept { return rules_.empty(); }

private:
    struct Rule {
        std::vector<Token> tokens{};
        bool whitelist{};
        bool dir_only{};
    };
    std::vector<Rule> rules_;
};

/// Loads one rule file into a matcher, line by line; a missing or unreadable
/// file is an empty matcher (frozen degrades the same way), and a line is
/// capped at `kRuleLineMaxBytes` before the rest of it is discarded. The
/// shared budget fails the load with a typed Validation error.
[[nodiscard]] support::ExpectedVoid load_rule_file(const std::string& path, RuleFile& rules, RuleBudget& budget);

/// The global excludes file resolved the way the frozen `ignore` crate
/// resolves it: `core.excludesFile` scanned (bytes, case-insensitive, first
/// match wins) from `GIT_CONFIG_GLOBAL`, then `$HOME/.gitconfig`, then
/// `$XDG_CONFIG_HOME/git/config`, then `GIT_CONFIG_SYSTEM`/`/etc/gitconfig`;
/// `~` expands against `$HOME`. Config reads are capped at `kConfigMaxBytes`;
/// an oversized, missing or unreadable config degrades to "no value", like a
/// missing file. Returns an empty path when no source names a value, in
/// which case the XDG default (`$XDG_CONFIG_HOME/git/ignore`) applies.
struct GlobalExcludes {
    static constexpr std::size_t kConfigMaxBytes = 1024 * 1024;
    [[nodiscard]] static std::string resolve_path();
};

/// Resolves the exclude-file directory for a work-tree directory: `.git` as
/// a directory is itself; `.git` as a file is read for `gitdir:`, and the
/// gitdir's `commondir` file redirects to the shared directory (frozen
/// `resolve_git_commondir`). Returns false when a `.git` file does not name
/// a gitdir; the caller then applies no exclude rules for the directory.
[[nodiscard]] bool resolve_exclude_dir(const std::string& dir, std::string& exclude_dir);

/// Whether a directory marks a repository (frozen `has_git`): it contains
/// `.git` of any kind (metadata follows a symlink) or a `.jj` directory.
[[nodiscard]] bool dir_has_vcs(const std::string& dir);

/// Renders raw name bytes the way the frozen daemon payload does
/// (`to_string_lossy`): invalid UTF-8 sequences become U+FFFD.
[[nodiscard]] std::string utf8_lossy(std::string_view bytes);

/// The home directory the frozen discovery uses: `$HOME`, then the passwd
/// entry. Empty when neither resolves.
[[nodiscard]] std::string home_dir();

} // namespace cch::durable::gitignore
