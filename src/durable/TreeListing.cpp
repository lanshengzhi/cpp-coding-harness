#include <cch/durable/Environment.hpp>

#include "GitIgnore.hpp"
#include "support/UniqueFd.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <utility>
#include <vector>

namespace cch::durable {
namespace {
namespace gi = cch::durable::gitignore;

/// The frozen payload ceiling: a listing response frame leaves room for its
/// JSON next to the payload (`frame.rs` MAX_FRAME - 64 KiB).
constexpr std::size_t kMaxPayloadBytes = 16 * 1024 * 1024 - 64 * 1024;
/// Cancellation is observed at least this often, in entries, and around every
/// directory read and sort, so a walk cannot outlive its deadline or a stop
/// request by more than one directory sort plus one entry's rule matching.
constexpr std::size_t kStopCheckInterval = 64;

support::Error listing_failure(std::string message, std::string_view hint = {}) {
    return hint.empty() ? support::make_error(support::ErrorCode::Workspace, std::move(message))
                        : support::make_error(support::ErrorCode::Workspace, std::move(message), std::string{hint});
}

/// The ignore rules of one directory of the chain: its `.gitignore`, its
/// `.git/info/exclude` (through a `gitdir:` file and `commondir` when `.git`
/// is a file), and whether the directory marks a repository. For an ancestor
/// of the listing root, `prefix` is the listing root's path relative to it
/// (the frozen matcher sees entry paths relative to the rule's directory, so
/// an ancestor rule sees the root's own components too).
struct ChainLevel {
    std::string prefix{};
    bool has_git{};
    gi::RuleFile gitignore{};
    gi::RuleFile exclude{};
};

support::ExpectedVoid build_level(const std::string& dir, ChainLevel& level, gi::RuleBudget& budget) {
    level.has_git = gi::dir_has_vcs(dir);
    if (auto loaded = gi::load_rule_file(dir + "/.gitignore", level.gitignore, budget); !loaded)
        return std::unexpected(loaded.error());
    std::string exclude_dir;
    if (gi::resolve_exclude_dir(dir, exclude_dir)) {
        if (auto loaded = gi::load_rule_file(exclude_dir + "/info/exclude", level.exclude, budget); !loaded)
            return std::unexpected(loaded.error());
    }
    return {};
}

/// The combined ignore outcome for one entry, replicating the frozen walker
/// (`ignore` crate `matched_ignore`): the deepest matching rule source wins;
/// per-directory `.gitignore` beats `.git/info/exclude`, which beats the
/// global file; within one file the last matching rule wins; a whitelist
/// suppresses the lower-precedence sources; and a repository boundary stops
/// parent rules above the work-tree root. `levels` is the walker's live
/// in-tree chain (level 0 is the listing root); it grows as the walk
/// descends, and the entry being matched always sits below every level.
class IgnoreChain {
public:
    IgnoreChain(const std::vector<ChainLevel>& levels,
            std::vector<ChainLevel> ancestors,
            gi::RuleFile global,
            bool ancestor_has_git,
            bool enabled)
        : levels_(levels), ancestors_(std::move(ancestors)), global_(std::move(global)),
          ancestor_has_git_(ancestor_has_git), enabled_(enabled) {}

    /// `rel` is the entry path relative to the listing root (raw bytes).
    [[nodiscard]] gi::Match match(std::string_view rel, bool is_dir) const {
        if (!enabled_) return gi::Match::None;
        gi::Match gitignore = gi::Match::None;
        gi::Match exclude = gi::Match::None;
        bool saw_git = false;
        for (std::size_t depth = levels_.size(); depth > 0; --depth) {
            const ChainLevel& level = levels_[depth - 1];
            // Level j sits at depth j; its rules see the components strictly
            // below it, which strips j components from the entry's path. A
            // repository boundary stops the rules above it: the level that
            // has `.git` still contributes its own rules, then blocks the
            // shallower ones (frozen `saw_git`).
            if (!saw_git) {
                const std::string_view below = relative_below(rel, depth - 1);
                if (gitignore == gi::Match::None) gitignore = level.gitignore.match(below, is_dir);
                if (exclude == gi::Match::None) exclude = level.exclude.match(below, is_dir);
            }
            saw_git = saw_git || level.has_git;
        }
        // No directory on the chain marks a repository: no repository rules
        // apply at all, even with `.gitignore` files present (frozen
        // `require_git`).
        if (!saw_git && !ancestor_has_git_) return gi::Match::None;
        for (const ChainLevel& ancestor : ancestors_) {
            if (saw_git) break;
            const std::string ancestor_path =
                    ancestor.prefix.empty() ? std::string(rel) : ancestor.prefix + "/" + std::string(rel);
            if (gitignore == gi::Match::None) gitignore = ancestor.gitignore.match(ancestor_path, is_dir);
            if (exclude == gi::Match::None) exclude = ancestor.exclude.match(ancestor_path, is_dir);
            saw_git = ancestor.has_git;
        }
        // The global file applies whenever any directory on the chain marks a
        // repository, even below a work-tree root.
        if (gitignore != gi::Match::None) return gitignore;
        if (exclude != gi::Match::None) return exclude;
        return global_.empty() ? gi::Match::None : global_.match(rel, is_dir);
    }

private:
    /// Level `depth - 1` is the entry's own directory; level j's rules see the
    /// components strictly below level j.
    [[nodiscard]] static std::string_view relative_below(std::string_view rel, std::size_t depth) {
        for (std::size_t passed = 0; passed < depth; ++passed) {
            const std::size_t slash = rel.find('/');
            if (slash == std::string_view::npos) return {};
            rel.remove_prefix(slash + 1);
        }
        return rel;
    }

    const std::vector<ChainLevel>& levels_;
    std::vector<ChainLevel> ancestors_;
    gi::RuleFile global_;
    bool ancestor_has_git_;
    bool enabled_;
};

struct Frame {
    std::string abs_path;
    std::string rel_path;
    std::vector<std::string> names{};
    std::size_t next_name{};
};

/// Reads one directory's entry names in file-system order (the walker sorts
/// them itself); an unreadable directory reads as empty, like the frozen walk.
std::vector<std::string> read_dir_names(const std::string& abs_path) {
    std::vector<std::string> names;
    DIR* dir = ::opendir(abs_path.c_str());
    if (dir == nullptr) return names;
    while (true) {
        dirent* entry = ::readdir(dir);
        if (entry == nullptr) break;
        const std::string_view name(entry->d_name);
        if (name == "." || name == "..") continue;
        names.emplace_back(name);
    }
    ::closedir(dir);
    return names;
}

class TreeWalker {
public:
    TreeWalker(std::string root, TreeListingOptions options, std::stop_token stop)
        : root_(std::move(root)), options_(std::move(options)), stop_(std::move(stop)) {
        max_bytes_ = std::min(options_.max_bytes, kMaxPayloadBytes);
        deadline_ = std::chrono::steady_clock::now() + options_.timeout;
    }

    support::Expected<TreeListing> run() {
        struct stat st{};
        if (::stat(root_.c_str(), &st) != 0)
            return std::unexpected(listing_failure(
                    std::format("ENOENT: no such file or directory, scandir '{}'", root_), "not_found"));
        if (!S_ISDIR(st.st_mode))
            return std::unexpected(
                    listing_failure(std::format("ENOTDIR: not a directory, scandir '{}'", root_), "not_directory"));

        if (auto prepared = prepare_rules(); !prepared) return std::unexpected(prepared.error());

        std::vector<Frame> stack;
        stack.push_back(Frame{.abs_path = root_, .rel_path = {}});
        while (!stack.empty()) {
            if ((checks_++ % kStopCheckInterval) == 0 && stop_.stop_requested())
                return std::unexpected(support::make_error(support::ErrorCode::Cancelled, "aborted"));
            Frame& frame = stack.back();
            if (frame.names.empty() && frame.next_name == 0) {
                frame.names = read_dir_names(frame.abs_path);
                std::ranges::sort(frame.names);
            }
            if (frame.next_name < frame.names.size()) {
                std::string name = std::move(frame.names[frame.next_name++]);
                auto outcome = handle_entry(frame, name);
                if (!outcome) return std::unexpected(outcome.error());
                if (*outcome == Outcome::Truncated) {
                    listing_.truncated = true;
                    return listing_;
                }
                if (*outcome == Outcome::Descend) stack.push_back(std::move(pending_frame_));
                continue;
            }
            stack.pop_back();
            if (levels_.size() > 1) levels_.pop_back();
        }
        return listing_;
    }

private:
    enum class Outcome { Next, Descend, Truncated };

    support::ExpectedVoid prepare_rules() {
        if (!options_.gitignore) return {};
        // Repository parents of the root, nearest first, each with the root's
        // path relative to it.
        std::vector<ChainLevel> ancestors;
        {
            std::string current = root_;
            std::string prefix;
            while (true) {
                const std::size_t slash = current.find_last_of('/');
                if (slash == std::string::npos) break;
                const std::string base = current.substr(slash + 1);
                current = current.substr(0, slash);
                if (current.empty()) current = "/";
                prefix = prefix.empty() ? base : base + "/" + prefix;
                ChainLevel level{.prefix = prefix};
                if (auto built = build_level(current, level, budget_); !built) return std::unexpected(built.error());
                ancestor_has_git_ = ancestor_has_git_ || level.has_git;
                ancestors.push_back(std::move(level));
                if (current == "/") break;
            }
        }
        ChainLevel root_level;
        if (auto built = build_level(root_, root_level, budget_); !built) return std::unexpected(built.error());
        levels_.push_back(std::move(root_level));
        // The global file is loaded whenever gitignore is enabled; it is only
        // *consulted* for entries whose chain marks a repository (a nested
        // repository below the root qualifies too, so the load cannot be
        // gated on the levels known up front).
        gi::RuleFile global;
        const std::string global_path = gi::GlobalExcludes::resolve_path();
        if (auto loaded = gi::load_rule_file(global_path, global, budget_); !loaded)
            return std::unexpected(loaded.error());
        chain_.emplace(levels_, std::move(ancestors), std::move(global), ancestor_has_git_, options_.gitignore);
        return {};
    }

    support::Expected<Outcome> handle_entry(Frame& frame, const std::string& name) {
        const bool always_excluded =
                name == ".git" || std::ranges::find(options_.exclude, name) != options_.exclude.end();
        if (always_excluded) return Outcome::Next;

        const std::string abs_path = frame.abs_path + "/" + name;
        struct stat st{};
        if (::lstat(abs_path.c_str(), &st) != 0) return Outcome::Next; // vanished, like the frozen walk
        const bool is_dir = S_ISDIR(st.st_mode);
        const char kind = is_dir ? 'd' : S_ISLNK(st.st_mode) ? 'l' : S_ISREG(st.st_mode) ? 'f' : '\0';
        if (kind == '\0') return Outcome::Next; // FIFOs and other kinds are never listed

        const std::string rel = frame.rel_path.empty() ? name : frame.rel_path + "/" + name;
        const gi::Match match = chain_.has_value() ? chain_->match(rel, is_dir) : gi::Match::None;
        if (match == gi::Match::Ignore) return Outcome::Next; // an ignored directory prunes its subtree
        const bool whitelisted = match == gi::Match::Whitelist;
        if (!options_.hidden && !whitelisted && !name.empty() && name.front() == '.') return Outcome::Next;

        // The deadline is checked where the frozen walker checks it: for an
        // entry that survived every filter, just before the bounds.
        if (std::chrono::steady_clock::now() > deadline_) return Outcome::Truncated;
        const std::string lossy = gi::utf8_lossy(rel);
        if (listing_.entries.size() >= options_.max_entries || payload_bytes_ + lossy.size() + 2 > max_bytes_)
            return Outcome::Truncated;
        payload_bytes_ += lossy.size() + 2;
        listing_.entries.push_back(TreeListingEntry{.path = lossy, .kind = kind});
        if (!is_dir) return Outcome::Next;

        ChainLevel level;
        if (auto built = build_level(abs_path, level, budget_); !built) return std::unexpected(built.error());
        levels_.push_back(std::move(level));
        pending_frame_ = Frame{.abs_path = abs_path, .rel_path = rel};
        return Outcome::Descend;
    }

    std::string root_;
    TreeListingOptions options_;
    std::stop_token stop_;
    std::size_t max_bytes_{};
    std::chrono::steady_clock::time_point deadline_{};
    TreeListing listing_{};
    std::size_t payload_bytes_{};
    std::size_t checks_{};
    gi::RuleBudget budget_{};
    std::vector<ChainLevel> levels_;
    std::optional<IgnoreChain> chain_;
    bool ancestor_has_git_{};
    Frame pending_frame_{};
};

} // namespace

support::Expected<TreeListing> walk_tree_listing(std::string root, TreeListingOptions options, std::stop_token stop) {
    return TreeWalker(std::move(root), std::move(options), std::move(stop)).run();
}

} // namespace cch::durable
