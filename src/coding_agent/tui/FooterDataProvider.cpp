#include "coding_agent/tui/FooterDataProvider.hpp"

#include "util/UniqueFd.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>

#if defined(__unix__) || defined(__APPLE__)
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace cch::coding_agent::tui {
namespace {

/// Read a small file's trimmed content; empty when unreadable.
[[nodiscard]] std::string read_trimmed(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    std::ostringstream content;
    content << input.rdbuf();
    auto text = content.str();
    while (!text.empty() &&
           (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
        text.pop_back();
    }
    return text;
}

#if defined(__unix__) || defined(__APPLE__)

/// pi `resolveBranchWithGitSync`: `git --no-optional-locks symbolic-ref
/// --quiet --short HEAD` in the repo; null on failure or detached HEAD.
[[nodiscard]] std::optional<std::string> resolve_branch_with_git(
    const std::filesystem::path& repo_dir) {
    int pipe_fds[2];
    if (::pipe(pipe_fds) < 0) return std::nullopt;
    util::UniqueFd read_end{pipe_fds[0]};
    util::UniqueFd write_end{pipe_fds[1]};
    const auto pid = ::fork();
    if (pid < 0) return std::nullopt;
    if (pid == 0) {
        // Child: stdout into the pipe; stdin/stderr ignored (pi
        // `stdio: ["ignore", "pipe", "ignore"]`).
        (void)::dup2(write_end.get(), STDOUT_FILENO);
        (void)read_end.close();
        (void)write_end.close();
        util::UniqueFd null_fd{::open("/dev/null", O_RDWR)};
        if (null_fd) {
            (void)::dup2(null_fd.get(), STDIN_FILENO);
            (void)::dup2(null_fd.get(), STDERR_FILENO);
        }
        (void)::chdir(repo_dir.c_str());
        constexpr std::array<const char*, 5> kArgv{
            "git", "--no-optional-locks", "symbolic-ref", "--quiet", "--short"};
        std::array<char*, 5> argv{
            const_cast<char*>(kArgv[0]),
            const_cast<char*>(kArgv[1]),
            const_cast<char*>(kArgv[2]),
            const_cast<char*>(kArgv[3]),
            const_cast<char*>(kArgv[4]),
        };
        ::execvp("git", argv.data());
        ::_exit(127);
    }
    (void)write_end.close();
    std::string output;
    std::array<char, 4096> buffer{};
    ssize_t count = 0;
    while ((count = ::read(read_end.get(), buffer.data(), buffer.size())) > 0) {
        output.append(buffer.data(), static_cast<std::size_t>(count));
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) continue;
        return std::nullopt;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return std::nullopt;
    while (!output.empty() && output.back() == '\n') output.pop_back();
    return output.empty() ? std::nullopt : std::optional<std::string>{std::move(output)};
}

#else

[[nodiscard]] std::optional<std::string> resolve_branch_with_git(
    const std::filesystem::path& /*repo_dir*/) {
    // Non-POSIX hosts have no fork/exec pipe here; a `.invalid` HEAD falls
    // back to "detached" exactly like a failed git query.
    return std::nullopt;
}

#endif

} // namespace

FooterDataProvider::FooterDataProvider(std::filesystem::path cwd) {
    set_cwd(std::move(cwd));
}

void FooterDataProvider::set_cwd(std::filesystem::path cwd) {
    if (cwd_ == cwd) return;
    cwd_ = std::move(cwd);
    git_paths_ = find_git_paths(cwd_);
    cached_branch_.reset();
}

std::optional<std::string> FooterDataProvider::git_branch() {
    const auto now = std::chrono::steady_clock::now();
    if (cached_branch_ && now - cached_at_ < kCacheTtl) {
        return *cached_branch_;
    }
    cached_branch_ = resolve_branch();
    cached_at_ = now;
    return cached_branch_;
}

std::optional<FooterDataProvider::GitPaths>
FooterDataProvider::find_git_paths(const std::filesystem::path& cwd) const {
    if (cwd.empty()) return std::nullopt;
    auto directory = std::filesystem::absolute(cwd);
    while (true) {
        const auto git_path = directory / ".git";
        std::error_code error;
        const auto status = std::filesystem::status(git_path, error);
        if (!error) {
            if (std::filesystem::is_regular_file(status)) {
                // Linked worktree: `.git` is a file whose content starts with
                // `gitdir: <path>`; the common dir may be pointed to by a
                // `commondir` file inside the linked git dir (pi
                // `findGitPaths`).
                const auto content = read_trimmed(git_path);
                if (content.starts_with("gitdir: ")) {
                    auto git_dir = std::filesystem::absolute(
                        directory / std::string_view{content}.substr(8));
                    const auto head_path = git_dir / "HEAD";
                    if (!std::filesystem::exists(head_path)) return std::nullopt;
                    const auto common_dir_path = git_dir / "commondir";
                    std::filesystem::path common_git_dir = git_dir;
                    if (std::filesystem::is_regular_file(common_dir_path)) {
                        const auto common = read_trimmed(common_dir_path);
                        if (!common.empty()) {
                            common_git_dir = std::filesystem::absolute(
                                git_dir / common);
                        }
                    }
                    return GitPaths{
                        .repo_dir = directory,
                        .common_git_dir = std::move(common_git_dir),
                        .head_path = std::move(head_path),
                    };
                }
                return std::nullopt;
            }
            if (std::filesystem::is_directory(status)) {
                const auto head_path = git_path / "HEAD";
                if (!std::filesystem::exists(head_path)) return std::nullopt;
                return GitPaths{
                    .repo_dir = directory,
                    .common_git_dir = git_path,
                    .head_path = std::move(head_path),
                };
            }
        }
        const auto parent = directory.parent_path();
        if (parent == directory) return std::nullopt;
        directory = parent;
    }
}

std::optional<std::string> FooterDataProvider::resolve_branch() const {
    if (!git_paths_) return std::nullopt;
    // pi `resolveGitBranchSync`: read HEAD; a symbolic ref names the branch,
    // `.invalid` resolves through git itself, and anything else (a raw
    // commit id) is a detached HEAD.
    const auto content = read_trimmed(git_paths_->head_path);
    if (content.empty()) return std::nullopt;
    if (content.starts_with("ref: refs/heads/")) {
        auto branch = std::string{content.substr(std::string_view{"ref: refs/heads/"}.size())};
        if (branch == ".invalid") {
            return resolve_branch_with_git(git_paths_->repo_dir).value_or("detached");
        }
        return branch;
    }
    return std::string{"detached"};
}

} // namespace cch::coding_agent::tui
