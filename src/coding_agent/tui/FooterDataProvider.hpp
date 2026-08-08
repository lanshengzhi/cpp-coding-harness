#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>

namespace cch::coding_agent::tui {

/// pi `footer-data-provider.ts` subset: the git branch for the footer's
/// pwd line, resolved by reading git metadata walking up from the session
/// cwd. No watchers (the C++ subset re-reads on a short cache TTL; pi's
/// HEAD/reftable watchers are an efficiency mechanism, not observable
/// behavior), no extension statuses (extensions are out of the subset).
///
/// `git_branch()` returns `std::nullopt` outside a git repo and `"detached"`
/// on a detached HEAD, exactly like pi's `getGitBranch`.
class FooterDataProvider {
public:
    explicit FooterDataProvider(std::filesystem::path cwd);

    /// Current git branch for the provider's cwd; nullopt when the cwd is
    /// not inside a git repo (or the metadata cannot be read).
    [[nodiscard]] std::optional<std::string> git_branch();

    /// Re-bind the provider to a new cwd (pi `setCwd`; the runtime cwd
    /// follows the session header cwd).
    void set_cwd(std::filesystem::path cwd);

private:
    struct GitPaths {
        std::filesystem::path repo_dir;
        std::filesystem::path common_git_dir;
        std::filesystem::path head_path;
    };

    [[nodiscard]] std::optional<GitPaths> find_git_paths(
        const std::filesystem::path& cwd) const;
    [[nodiscard]] std::optional<std::string> resolve_branch() const;

    std::filesystem::path cwd_;
    /// Cached git metadata paths for the current cwd (nullopt when the cwd
    /// is outside a repo).
    mutable std::optional<GitPaths> git_paths_{std::nullopt};
    /// Branch cache with a short TTL; re-reads after it expires so branch
    /// switches appear in the footer without a watcher.
    mutable std::optional<std::string> cached_branch_{std::nullopt};
    mutable std::chrono::steady_clock::time_point cached_at_{};
    inline static constexpr std::chrono::milliseconds kCacheTtl{1000};
};

} // namespace cch::coding_agent::tui
