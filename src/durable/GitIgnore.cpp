#include "GitIgnore.hpp"

#include "support/UniqueFd.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <limits>
#include <optional>
#include <pwd.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace cch::durable::gitignore {
namespace {

bool is_separator(char c) { return c == '/'; }

// One parsing cursor over the glob text (frozen `globset` Parser).
class GlobParser {
public:
    explicit GlobParser(std::string_view glob) : chars_(glob) {}

    [[nodiscard]] bool parse(std::vector<Token>& out) {
        branches_.emplace_back();
        while (true) {
            const auto next = bump();
            if (!next.has_value()) break;
            const char c = *next;
            if (c == '?') {
                if (!push(Token{.kind = Token::Kind::AnyByte})) return false;
            } else if (c == '*') {
                if (!parse_star()) return false;
            } else if (c == '[' && !found_unclosed_class_) {
                if (!parse_class()) return false;
            } else if (c == '{') {
                alternates_stack_.push_back(branches_.size());
                branches_.emplace_back();
            } else if (c == '}') {
                if (!pop_alternate()) return false;
            } else if (c == ',') {
                // Outside an alternation a comma is a literal; inside it
                // starts a new branch.
                if (alternates_stack_.empty()) {
                    if (!push(literal(','))) return false;
                } else {
                    branches_.emplace_back();
                }
            } else if (c == '\\') {
                const auto escaped = bump();
                if (!escaped.has_value()) return false; // dangling escape
                if (!push(literal(*escaped))) return false;
            } else {
                if (!push(literal(c))) return false;
            }
        }
        if (!alternates_stack_.empty()) return false; // unclosed '{'
        if (branches_.size() != 1) return false;
        out = std::move(branches_.front());
        return true;
    }

private:
    static Token literal(char c) { return Token{.kind = Token::Kind::Literal, .byte = static_cast<unsigned char>(c)}; }

    std::optional<char> bump() {
        if (pos_ >= chars_.size()) return std::nullopt;
        // `prev` is the character before the one this bump returns (frozen
        // `globset` Parser's prev/cur pair).
        if (pos_ > 0) prev_ = chars_[pos_ - 1];
        return chars_[pos_++];
    }

    std::optional<char> peek() const {
        if (pos_ >= chars_.size()) return std::nullopt;
        return chars_[pos_];
    }

    [[nodiscard]] bool have_tokens() const { return !branches_.back().empty(); }

    bool push(Token token) {
        branches_.back().push_back(std::move(token));
        return true;
    }

    Token pop_token() {
        Token token = std::move(branches_.back().back());
        branches_.back().pop_back();
        return token;
    }

    bool pop_alternate() {
        if (alternates_stack_.empty()) return false; // unopened '}'
        const std::size_t start = alternates_stack_.back();
        alternates_stack_.pop_back();
        Token alternates{.kind = Token::Kind::Alternates};
        alternates.branches.assign(std::make_move_iterator(branches_.begin() + static_cast<std::ptrdiff_t>(start)),
                std::make_move_iterator(branches_.end()));
        branches_.erase(branches_.begin() + static_cast<std::ptrdiff_t>(start), branches_.end());
        return push(std::move(alternates));
    }

    bool parse_star() {
        // `prev` at entry is the character before the first `*`; bumping the
        // second star must not lose it.
        const auto entry_prev = prev_;
        if (peek() != '*') return push(Token{.kind = Token::Kind::Star});
        (void)bump();
        if (!have_tokens()) {
            // At the start of a branch `**` is recursive only before a
            // separator or the end; `**name` is two stars.
            const auto after = peek();
            if (after.has_value() && !is_separator(*after)) {
                if (!push(Token{.kind = Token::Kind::Star})) return false;
                return push(Token{.kind = Token::Kind::Star});
            }
            if (!push(Token{.kind = Token::Kind::RecursivePrefix})) return false;
            if (after.has_value()) (void)bump();
            return true;
        }
        if (!entry_prev.has_value() || !is_separator(*entry_prev)) {
            if (branches_.size() <= 1 || (entry_prev != ',' && entry_prev != '{')) {
                if (!push(Token{.kind = Token::Kind::Star})) return false;
                return push(Token{.kind = Token::Kind::Star});
            }
        }
        bool is_suffix;
        const auto after = peek();
        if (!after.has_value()) {
            is_suffix = true;
        } else if ((*after == ',' || *after == '}') && branches_.size() >= 2) {
            is_suffix = true;
        } else if (is_separator(*after)) {
            (void)bump();
            is_suffix = false;
        } else {
            if (!push(Token{.kind = Token::Kind::Star})) return false;
            return push(Token{.kind = Token::Kind::Star});
        }
        // The token before `**` is the separator, which the recursive token
        // replaces; consecutive `**` collapse onto the same recursive kind.
        Token previous = pop_token();
        if (previous.kind == Token::Kind::RecursivePrefix) return push(Token{.kind = Token::Kind::RecursivePrefix});
        if (previous.kind == Token::Kind::RecursiveSuffix) return push(Token{.kind = Token::Kind::RecursiveSuffix});
        return push(Token{.kind = is_suffix ? Token::Kind::RecursiveSuffix : Token::Kind::RecursiveZeroOrMore});
    }

    bool parse_class() {
        const std::size_t saved_pos = pos_;
        const auto saved_prev = prev_;
        std::vector<std::pair<unsigned char, unsigned char>> ranges;
        bool negated = false;
        if (peek() == '!' || peek() == '^') {
            negated = true;
            (void)bump();
        }
        bool first = true;
        bool in_range = false;
        while (true) {
            const auto next = bump();
            if (!next.has_value()) {
                // Unclosed class: the frozen builder falls back to a literal
                // '[' and treats every later '[' as literal too.
                if (found_unclosed_class_ == false) {
                    found_unclosed_class_ = true;
                    pos_ = saved_pos;
                    prev_ = saved_prev;
                    return push(literal('['));
                }
                return false;
            }
            const char c = *next;
            if (c == ']') {
                if (first) {
                    ranges.emplace_back(']', ']');
                } else {
                    break;
                }
            } else if (c == '-') {
                if (first) {
                    ranges.emplace_back('-', '-');
                } else if (in_range) {
                    auto& range = ranges.back();
                    range.second = '-';
                    if (range.second < range.first) return false; // invalid range
                    in_range = false;
                } else {
                    in_range = true;
                }
            } else {
                if (in_range) {
                    auto& range = ranges.back();
                    range.second = static_cast<unsigned char>(c);
                    if (range.second < range.first) return false; // invalid range
                    in_range = false;
                } else {
                    ranges.emplace_back(static_cast<unsigned char>(c), static_cast<unsigned char>(c));
                }
            }
            first = false;
        }
        if (in_range) ranges.emplace_back('-', '-');
        Token token{.kind = Token::Kind::Class, .negated = negated, .ranges = std::move(ranges)};
        return push(std::move(token));
    }

    std::string_view chars_;
    std::size_t pos_{};
    std::optional<char> prev_{};
    bool found_unclosed_class_{};
    std::vector<std::size_t> alternates_stack_;
    std::vector<std::vector<Token>> branches_;
};

// The compiled form of one whole-glob `**`: it matches everything (frozen
// `Tokens::to_regex_with` special case).
bool tokens_match_everything(const std::vector<Token>& tokens) {
    return tokens.size() == 1 && tokens.front().kind == Token::Kind::RecursivePrefix;
}

// One backtracking matcher over the token list and the raw path bytes. The
// recursion depth is bounded by the pattern and path lengths; star retries
// iterate longest-first like the compiled regex.
class TokenMatcher {
public:
    TokenMatcher(const std::vector<Token>& tokens, std::string_view path) : tokens_(tokens), path_(path) {}

    [[nodiscard]] bool match() {
        if (tokens_match_everything(tokens_)) return true;
        return match_from(0, 0);
    }

private:
    bool match_from(std::size_t ti, std::size_t si) {
        if (ti == tokens_.size()) return si == path_.size();
        const Token& token = tokens_[ti];
        switch (token.kind) {
        case Token::Kind::Literal:
            return si < path_.size() && static_cast<unsigned char>(path_[si]) == token.byte &&
                   match_from(ti + 1, si + 1);
        case Token::Kind::AnyByte:
            return si < path_.size() && path_[si] != '/' && match_from(ti + 1, si + 1);
        case Token::Kind::Star: {
            std::size_t end = si;
            while (end < path_.size() && path_[end] != '/')
                ++end;
            while (true) {
                if (match_from(ti + 1, end)) return true;
                if (end == si) return false;
                --end;
            }
        }
        case Token::Kind::Class: {
            if (si >= path_.size()) return false;
            const auto byte = static_cast<unsigned char>(path_[si]);
            bool inside = false;
            for (const auto& [low, high] : token.ranges) {
                if (byte >= low && byte <= high) {
                    inside = true;
                    break;
                }
            }
            if (inside == token.negated) return false;
            return match_from(ti + 1, si + 1);
        }
        case Token::Kind::RecursivePrefix:
            // (?:/?|.*/): zero or more leading components.
            if (match_from(ti + 1, si)) return true;
            for (std::size_t i = si; i < path_.size(); ++i) {
                if (path_[i] == '/' && match_from(ti + 1, i + 1)) return true;
            }
            return false;
        case Token::Kind::RecursiveSuffix:
            // /.*: one slash and everything after it (always last in a
            // gitignore glob after the `/**` -> `/**/*` rewrite).
            return si < path_.size() && path_[si] == '/' && match_from(ti + 1, path_.size());
        case Token::Kind::RecursiveZeroOrMore:
            // (?:/|/.*/): a slash at the current position, then either the
            // rest right after it or anything up to a later slash.
            if (si >= path_.size() || path_[si] != '/') return false;
            if (match_from(ti + 1, si + 1)) return true;
            for (std::size_t i = si + 1; i < path_.size(); ++i) {
                if (path_[i] == '/' && match_from(ti + 1, i + 1)) return true;
            }
            return false;
        case Token::Kind::Alternates:
            for (const auto& branch : token.branches) {
                if (match_branch(branch, 0, ti + 1, si)) return true;
            }
            return false;
        }
        return false;
    }

    bool match_branch(const std::vector<Token>& branch, std::size_t bi, std::size_t rest, std::size_t si) {
        if (bi == branch.size()) return match_from(rest, si);
        const Token& token = branch[bi];
        switch (token.kind) {
        case Token::Kind::Literal:
            return si < path_.size() && static_cast<unsigned char>(path_[si]) == token.byte &&
                   match_branch(branch, bi + 1, rest, si + 1);
        case Token::Kind::AnyByte:
            return si < path_.size() && path_[si] != '/' && match_branch(branch, bi + 1, rest, si + 1);
        case Token::Kind::Star: {
            std::size_t end = si;
            while (end < path_.size() && path_[end] != '/')
                ++end;
            while (true) {
                if (match_branch(branch, bi + 1, rest, end)) return true;
                if (end == si) return false;
                --end;
            }
        }
        case Token::Kind::Class: {
            if (si >= path_.size()) return false;
            const auto byte = static_cast<unsigned char>(path_[si]);
            bool inside = false;
            for (const auto& [low, high] : token.ranges) {
                if (byte >= low && byte <= high) {
                    inside = true;
                    break;
                }
            }
            if (inside == token.negated) return false;
            return match_branch(branch, bi + 1, rest, si + 1);
        }
        default:
            // Recursive tokens and nested alternates do not occur inside a
            // branch for gitignore globs; treat them as no-match.
            return false;
        }
    }

    const std::vector<Token>& tokens_;
    std::string_view path_;
};

bool has_doublestar_prefix(std::string_view actual) { return actual == "**" || actual.starts_with("**/"); }

std::string_view trim_ascii_end(std::string_view line) {
    while (!line.empty()) {
        const char c = line.back();
        if (c != ' ' && c != '\t' && c != '\r' && c != '\v' && c != '\f') break;
        line.remove_suffix(1);
    }
    return line;
}

// Reads a whole small file bounded to `max_bytes`; returns false when the
// file is missing, unreadable or larger than the bound (degraded exactly
// like a missing file, with the read bounded).
bool read_bounded_file(const std::string& path, std::size_t max_bytes, std::string& out) {
    support::UniqueFd fd{::open(path.c_str(), O_RDONLY | O_CLOEXEC)};
    if (!fd) return false;
    std::array<char, 8192> buffer{};
    while (out.size() < max_bytes) {
        const ssize_t n = ::read(fd.get(), buffer.data(), std::min(buffer.size(), max_bytes - out.size()));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        out.append(buffer.data(), static_cast<std::size_t>(n));
    }
    // A file larger than the bound degrades like a missing one.
    if (out.size() >= max_bytes) {
        const ssize_t extra = ::read(fd.get(), buffer.data(), 1);
        if (extra > 0 || (extra < 0 && errno != EINTR)) return false;
    }
    return true;
}

// The frozen `parse_excludes_file` scan: bytes, case-insensitive, the first
// line shaped `excludesFile = <token>` (optionally double-quoted) wins.
bool scan_excludes_file(const std::string& contents, std::string& value) {
    std::size_t start = 0;
    while (start <= contents.size()) {
        std::size_t end = contents.find('\n', start);
        if (end == std::string::npos) end = contents.size();
        std::string_view line(contents.data() + start, end - start);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        std::size_t i = 0;
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
            ++i;
        constexpr std::string_view key = "excludesfile";
        if (line.size() - i >= key.size()) {
            bool matches = true;
            for (std::size_t k = 0; k < key.size(); ++k) {
                const char c = line[i + k];
                const char lowered = c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
                if (lowered != key[k]) {
                    matches = false;
                    break;
                }
            }
            if (matches) {
                i += key.size();
                while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
                    ++i;
                if (i < line.size() && line[i] == '=') {
                    ++i;
                    while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
                        ++i;
                    if (i < line.size() && line[i] == '"') ++i;
                    while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
                        ++i;
                    const std::size_t token_start = i;
                    // The value is a non-space, non-quote token; the frozen
                    // regex allows one surrounding pair of double quotes.
                    while (i < line.size() && line[i] != ' ' && line[i] != '\t' && line[i] != '"')
                        ++i;
                    const std::size_t token_end = i;
                    if (i < line.size() && line[i] == '"') ++i;
                    while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
                        ++i;
                    if (token_end > token_start && i == line.size()) {
                        value = std::string(line.substr(token_start, token_end - token_start));
                        return true;
                    }
                }
            }
        }
        start = end + 1;
    }
    return false;
}

std::string expand_tilde(std::string path) {
    // Only a leading tilde expands, exactly like frozen pi
    // (packages/durable/src/env/node.ts) and the repo's other tilde
    // expansions (WorkspacePathResolution, ShellResolver): a `~` anywhere
    // else in the path stays literal, so backup files like `foo.txt~` or
    // names like `dir~name` are never corrupted (issue #1227).
    if (path != "~" && !path.starts_with("~/")) return path;
    const std::string home = home_dir();
    if (home.empty()) return path;
    if (path == "~") return home;
    return home + path.substr(1);
}

std::optional<std::string> excludes_file_from_config(const char* env_name, const std::string& fallback_path) {
    std::string path;
    if (const char* from_env = ::getenv(env_name); from_env != nullptr && *from_env != '\0')
        path = from_env;
    else if (!fallback_path.empty())
        path = fallback_path;
    else
        return std::nullopt;
    std::string contents;
    if (!read_bounded_file(path, GlobalExcludes::kConfigMaxBytes, contents)) return std::nullopt;
    std::string value;
    if (!scan_excludes_file(contents, value)) return std::nullopt;
    return expand_tilde(std::move(value));
}

} // namespace

support::ExpectedVoid load_rule_file(const std::string& path, RuleFile& rules, RuleBudget& budget) {
    /// One rule file's single line never needs more than this; longer lines
    /// have their tail discarded (a documented C++ bound the frozen walker
    /// does not need, frozen having no rule memory bound at all).
    constexpr std::size_t kRuleLineMaxBytes = 1024 * 1024;
    support::UniqueFd fd{::open(path.c_str(), O_RDONLY | O_CLOEXEC)};
    if (!fd) return {};
    std::string line;
    line.reserve(256);
    std::array<char, 8192> buffer{};
    bool first = true;
    bool saw_any_bytes = false;
    bool discard_tail = false;
    auto flush = [&]() -> support::ExpectedVoid {
        if (auto added = rules.add_line(line, first, budget); !added) return std::unexpected(added.error());
        line.clear();
        first = false;
        return {};
    };
    while (true) {
        const ssize_t n = ::read(fd.get(), buffer.data(), buffer.size());
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        saw_any_bytes = true;
        std::size_t cursor = 0;
        while (cursor < static_cast<std::size_t>(n)) {
            const void* hit = std::memchr(buffer.data() + cursor, '\n', static_cast<std::size_t>(n) - cursor);
            const std::size_t end = hit == nullptr
                                            ? static_cast<std::size_t>(n)
                                            : static_cast<std::size_t>(static_cast<const char*>(hit) - buffer.data());
            if (!discard_tail) {
                const std::size_t room = line.size() < kRuleLineMaxBytes ? kRuleLineMaxBytes - line.size() : 0;
                line.append(buffer.data() + cursor, std::min(end - cursor, room));
                if (end - cursor > room) discard_tail = true;
            }
            if (hit == nullptr) break;
            if (line.ends_with('\r')) line.pop_back();
            if (auto flushed = flush(); !flushed) return std::unexpected(flushed.error());
            discard_tail = false;
            cursor = end + 1;
        }
    }
    // A trailing line without a newline is still a rule line; an empty file
    // contributes nothing.
    if (saw_any_bytes && (!line.empty() || first)) {
        if (line.ends_with('\r')) line.pop_back();
        if (auto flushed = flush(); !flushed) return std::unexpected(flushed.error());
    }
    return {};
}

bool parse_glob(std::string_view glob, std::vector<Token>& tokens) {
    GlobParser parser(glob);
    return parser.parse(tokens);
}

support::ExpectedVoid RuleFile::add_line(std::string_view line, bool first_line, RuleBudget& budget) {
    // A leading UTF-8 BOM is dropped from the first line only (frozen
    // `GitignoreBuilder::add`).
    if (first_line && line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xef &&
            static_cast<unsigned char>(line[1]) == 0xbb && static_cast<unsigned char>(line[2]) == 0xbf)
        line.remove_prefix(3);
    if (!line.empty() && line.front() == '#') return {};
    if (!line.ends_with("\\ ")) line = trim_ascii_end(line);
    if (line.empty()) return {};
    Rule rule;
    bool is_absolute = false;
    if (line.starts_with("\\!") || line.starts_with("\\#")) {
        line.remove_prefix(1);
        is_absolute = !line.empty() && line.front() == '/';
    } else {
        if (!line.empty() && line.front() == '!') {
            rule.whitelist = true;
            line.remove_prefix(1);
        }
        if (!line.empty() && line.front() == '/') {
            line.remove_prefix(1);
            is_absolute = true;
        }
    }
    if (!line.empty() && line.back() == '/') {
        rule.dir_only = true;
        line.remove_suffix(1);
        if (!line.empty() && line.back() == '\\') line.remove_suffix(1);
    }
    std::string actual(line);
    if (!is_absolute && actual.find('/') == std::string::npos && !has_doublestar_prefix(actual))
        actual = "**/" + actual;
    if (actual.size() >= 3 && actual.ends_with("/**")) actual += "/*";
    if (!parse_glob(actual, rule.tokens)) return {}; // unparseable line skipped, like frozen
    if (budget.rules >= budget.max_rules)
        return std::unexpected(support::make_error(support::ErrorCode::Validation,
                std::format("ignore rule count exceeds the {} rule listing budget", budget.max_rules)));
    if (budget.rule_bytes + actual.size() > budget.max_rule_bytes)
        return std::unexpected(support::make_error(support::ErrorCode::Validation,
                std::format("ignore rule text exceeds the {} byte listing budget", budget.max_rule_bytes)));
    budget.rules += 1;
    budget.rule_bytes += actual.size();
    rules_.push_back(std::move(rule));
    return {};
}

Match RuleFile::match(std::string_view path, bool is_dir) const {
    for (auto it = rules_.rbegin(); it != rules_.rend(); ++it) {
        if (it->dir_only && !is_dir) continue;
        TokenMatcher matcher(it->tokens, path);
        if (matcher.match()) return it->whitelist ? Match::Whitelist : Match::Ignore;
    }
    return Match::None;
}

std::string GlobalExcludes::resolve_path() {
    if (auto value = excludes_file_from_config("GIT_CONFIG_GLOBAL", "")) return *value;
    const std::string home = home_dir();
    if (auto value = excludes_file_from_config("", home + "/.gitconfig")) return *value;
    std::string xdg_config = ::getenv("XDG_CONFIG_HOME") != nullptr ? std::string(::getenv("XDG_CONFIG_HOME")) : "";
    if (xdg_config.empty()) xdg_config = home + "/.config";
    if (auto value = excludes_file_from_config("", xdg_config + "/git/config")) return *value;
    const char* system_env = ::getenv("GIT_CONFIG_SYSTEM");
    const std::string system_path =
            system_env != nullptr && *system_env != '\0' ? std::string(system_env) : "/etc/gitconfig";
    if (auto value = excludes_file_from_config("", system_path)) return *value;
    std::string xdg = ::getenv("XDG_CONFIG_HOME") != nullptr ? std::string(::getenv("XDG_CONFIG_HOME")) : "";
    if (xdg.empty()) xdg = home + "/.config";
    return xdg + "/git/ignore";
}

bool resolve_exclude_dir(const std::string& dir, std::string& exclude_dir) {
    const std::string dot_git = dir + "/.git";
    struct stat st{};
    if (::stat(dot_git.c_str(), &st) != 0) return false; // unreadable .git: no exclude rules
    if (!S_ISREG(st.st_mode)) {
        exclude_dir = dot_git;
        return true;
    }
    std::string contents;
    if (!read_bounded_file(dot_git, GlobalExcludes::kConfigMaxBytes, contents)) return false;
    const std::size_t newline = contents.find('\n');
    const std::string first_line = contents.substr(0, newline);
    constexpr std::string_view prefix = "gitdir: ";
    if (!first_line.starts_with(prefix)) return false;
    const std::string git_dir = first_line.substr(prefix.size());
    std::string common_dir = git_dir;
    std::string commondir_contents;
    if (read_bounded_file(git_dir + "/commondir", GlobalExcludes::kConfigMaxBytes, commondir_contents)) {
        const std::size_t commondir_line_end = commondir_contents.find('\n');
        const std::string commondir_line = commondir_contents.substr(0, commondir_line_end);
        if (!commondir_line.empty()) {
            common_dir = commondir_line.front() == '.' ? git_dir + "/" + commondir_line : commondir_line;
        }
    }
    exclude_dir = common_dir;
    return true;
}

bool dir_has_vcs(const std::string& dir) {
    struct stat st{};
    const bool has_git = ::stat((dir + "/.git").c_str(), &st) == 0;
    const bool has_jj = ::stat((dir + "/.jj").c_str(), &st) == 0;
    return has_git || has_jj;
}

std::string utf8_lossy(std::string_view bytes) {
    std::string out;
    out.reserve(bytes.size());
    std::size_t i = 0;
    while (i < bytes.size()) {
        const auto lead = static_cast<unsigned char>(bytes[i]);
        if (lead < 0x80) {
            out.push_back(bytes[i]);
            ++i;
            continue;
        }
        const std::size_t length = lead >= 0xc2 && lead <= 0xdf   ? 2
                                   : lead >= 0xe0 && lead <= 0xef ? 3
                                   : lead >= 0xf0 && lead <= 0xf4 ? 4
                                                                  : 0;
        if (length == 0) {
            out += "\xef\xbf\xbd";
            ++i;
            continue;
        }
        std::size_t valid = 1;
        while (valid < length && i + valid < bytes.size()) {
            const auto byte = static_cast<unsigned char>(bytes[i + valid]);
            if (byte < 0x80 || byte > 0xbf ||
                    (valid == 1 && ((lead == 0xe0 && byte < 0xa0) || (lead == 0xed && byte > 0x9f) ||
                                           (lead == 0xf0 && byte < 0x90) || (lead == 0xf4 && byte > 0x8f))))
                break;
            ++valid;
        }
        if (valid == length) {
            out.append(bytes.substr(i, length));
            i += length;
        } else {
            out += "\xef\xbf\xbd";
            i += valid;
        }
    }
    return out;
}

std::string home_dir() {
    if (const char* home = ::getenv("HOME"); home != nullptr && *home != '\0') return std::string(home);
    if (const passwd* entry = ::getpwuid(::getuid()); entry != nullptr && entry->pw_dir != nullptr)
        return std::string(entry->pw_dir);
    return {};
}

} // namespace cch::durable::gitignore
