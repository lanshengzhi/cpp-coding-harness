#include <cch/durable/Environment.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <format>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;

namespace cch::durable {
namespace {
namespace fs = std::filesystem;

support::Error failure(std::string message, support::ErrorCode code = support::ErrorCode::Workspace) {
    return support::make_error(code, std::move(message));
}
support::Error system_failure(std::string_view operation) {
    return failure(std::string{operation} + ": " + std::strerror(errno));
}
support::Error cancelled() { return failure("aborted", support::ErrorCode::Cancelled); }

class NativeWatch final : public FileWatch {
public:
    NativeWatch(int fd, std::function<void()> callback)
        : fd_(fd), worker_([this, callback = std::move(callback)](std::stop_token stop) {
              std::array<char, 8192> buffer{};
              while (!stop.stop_requested()) {
                  pollfd descriptor{fd_, POLLIN, 0};
                  if (::poll(&descriptor, 1, 50) > 0 && ::read(fd_, buffer.data(), buffer.size()) > 0 &&
                          !stop.stop_requested())
                      callback();
              }
          }) {}
    ~NativeWatch() override { close(); }
    void close() override {
        if (!worker_.joinable()) return;
        worker_.request_stop();
        worker_.join();
        ::close(fd_);
    }

private:
    int fd_;
    std::jthread worker_;
};

class LocalEnvironment final : public Environment {
public:
    explicit LocalEnvironment(std::string cwd) : cwd_(std::move(cwd)) {}
    fs::path resolve(std::string_view path) const {
        fs::path result{path};
        return (result.is_absolute() ? result : cwd_ / result).lexically_normal();
    }
    support::Expected<std::string> read(std::string_view path, std::stop_token stop) override {
        if (stop.stop_requested()) return std::unexpected(cancelled());
        auto name = resolve(path).string();
        int fd = ::open(name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) {
            if (errno == ENOENT)
                return std::unexpected(support::make_error(support::ErrorCode::Workspace,
                        std::format("ENOENT: no such file or directory, open '{}'", name),
                        "not_found"));
            return std::unexpected(system_failure("open"));
        }
        std::string result;
        std::array<char, 8192> buffer{};
        ssize_t n;
        while ((n = ::read(fd, buffer.data(), buffer.size())) > 0) {
            if (stop.stop_requested()) {
                ::close(fd);
                return std::unexpected(cancelled());
            }
            result.append(buffer.data(), static_cast<std::size_t>(n));
        }
        auto error = n < 0 ? std::optional{system_failure("read")} : std::nullopt;
        ::close(fd);
        if (error) return std::unexpected(*error);
        return result;
    }
    support::ExpectedVoid write(std::string_view path, std::string_view content, std::stop_token stop) override {
        if (stop.stop_requested()) return std::unexpected(cancelled());
        auto target = resolve(path);
        std::error_code ec;
        fs::create_directories(target.parent_path(), ec);
        if (ec) return std::unexpected(failure(ec.message()));
        struct stat st{};
        if (::lstat(target.c_str(), &st) == 0 && S_ISLNK(st.st_mode))
            return std::unexpected(failure("refusing to write through final symlink"));
        auto tmp = (target.parent_path() / ".cch-durable-XXXXXX").string();
        int fd = ::mkstemp(tmp.data());
        if (fd < 0) return std::unexpected(system_failure("mkstemp"));
        auto result = write_fd(fd, content, stop);
        if (::close(fd) != 0 && result) result = std::unexpected(system_failure("close"));
        if (result && stop.stop_requested()) result = std::unexpected(cancelled());
        if (result && ::rename(tmp.c_str(), target.c_str()) != 0) result = std::unexpected(system_failure("rename"));
        if (!result) ::unlink(tmp.c_str());
        return result;
    }
    support::ExpectedVoid append(std::string_view path, std::string_view content, std::stop_token stop) override {
        if (stop.stop_requested()) return std::unexpected(cancelled());
        auto name = resolve(path).string();
        int fd = ::open(name.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0) return std::unexpected(system_failure("open"));
        auto result = write_fd(fd, content, stop);
        ::close(fd);
        return result;
    }
    support::Expected<FileInfo> stat(std::string_view path, std::stop_token stop) override {
        if (stop.stop_requested()) return std::unexpected(cancelled());
        auto name = resolve(path);
        struct stat st{};
        if (::lstat(name.c_str(), &st) != 0) return std::unexpected(system_failure("stat"));
        return FileInfo{name.filename().string(),
                name.string(),
                S_ISDIR(st.st_mode)   ? "directory"
                : S_ISLNK(st.st_mode) ? "symlink"
                                      : "file",
                static_cast<std::uintmax_t>(st.st_size)};
    }
    support::Expected<std::vector<FileInfo>> list(std::string_view path, std::stop_token stop) override {
        std::error_code ec;
        fs::directory_iterator it(resolve(path), ec), end;
        if (ec) return std::unexpected(failure(ec.message()));
        std::vector<FileInfo> result;
        while (it != end) {
            auto info = stat(it->path().string(), stop);
            if (!info) return std::unexpected(info.error());
            result.push_back(std::move(*info));
            it.increment(ec);
            if (ec) return std::unexpected(failure(ec.message()));
        }
        return result;
    }
    support::Expected<ShellResult> exec(const ShellCommand& command,
            std::chrono::milliseconds timeout,
            OutputSink output,
            std::stop_token stop) override;
    support::Expected<std::unique_ptr<FileWatch>> watch(
            std::string_view path, std::function<void()> callback, bool recursive) override {
        if (recursive)
            return std::unexpected(failure("recursive watch is not implemented", support::ErrorCode::Validation));
        auto target = resolve(path);
        int fd = ::inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
        if (fd < 0) return std::unexpected(system_failure("inotify_init"));
        std::error_code ec;
        auto watched = fs::is_directory(target, ec) ? target : target.parent_path();
        if (::inotify_add_watch(fd,
                    watched.c_str(),
                    IN_CREATE | IN_MODIFY | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_ATTRIB) < 0) {
            auto err = system_failure("inotify_add_watch");
            ::close(fd);
            return std::unexpected(err);
        }
        return std::unique_ptr<FileWatch>{std::make_unique<NativeWatch>(fd, std::move(callback))};
    }

private:
    static support::ExpectedVoid write_fd(int fd, std::string_view text, std::stop_token stop) {
        while (!text.empty()) {
            if (stop.stop_requested()) return std::unexpected(cancelled());
            auto n = ::write(fd, text.data(), text.size());
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return std::unexpected(system_failure("write"));
            text.remove_prefix(static_cast<std::size_t>(n));
        }
        return {};
    }
    fs::path cwd_;
};

bool secret_name(std::string name) {
    std::ranges::transform(name, name.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    for (std::string_view part : {"API_KEY",
                 "TOKEN",
                 "SECRET",
                 "PASSWORD",
                 "CREDENTIAL",
                 "PRIVATE_KEY",
                 "AUTH",
                 "JWT",
                 "CERTIFICATE",
                 "PASSPHRASE",
                 "OPENAI"})
        if (name.find(part) != std::string::npos) return true;
    return false;
}

support::Expected<ShellResult> LocalEnvironment::exec(
        const ShellCommand& command, std::chrono::milliseconds timeout, OutputSink output, std::stop_token stop) {
    if (stop.stop_requested()) return std::unexpected(cancelled());
    std::vector<std::string> args;
    if (const auto* text = std::get_if<std::string>(&command))
        args = {"/bin/sh", "-c", *text};
    else
        args = std::get<std::vector<std::string>>(command);
    if (args.empty()) return std::unexpected(failure("empty command", support::ErrorCode::Process));
    std::vector<char*> argv;
    for (auto& arg : args)
        argv.push_back(arg.data());
    argv.push_back(nullptr);
    std::vector<std::string> env;
    for (char** p = environ; p && *p; ++p) {
        std::string entry{*p};
        if (!secret_name(entry.substr(0, entry.find('=')))) env.push_back(std::move(entry));
    }
    std::vector<char*> envp;
    for (auto& entry : env)
        envp.push_back(entry.data());
    envp.push_back(nullptr);
    int out[2], err[2];
    if (::pipe2(out, O_CLOEXEC) != 0) return std::unexpected(system_failure("pipe"));
    if (::pipe2(err, O_CLOEXEC) != 0) {
        ::close(out[0]);
        ::close(out[1]);
        return std::unexpected(system_failure("pipe"));
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, out[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, err[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, out[0]);
    posix_spawn_file_actions_addclose(&actions, err[0]);
    posix_spawn_file_actions_addchdir_np(&actions, cwd_.c_str());
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attr, 0);
    pid_t pid{};
    int rc = posix_spawnp(&pid, args[0].c_str(), &actions, &attr, argv.data(), envp.data());
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&actions);
    ::close(out[1]);
    ::close(err[1]);
    if (rc != 0) {
        ::close(out[0]);
        ::close(err[0]);
        return std::unexpected(failure(std::strerror(rc), support::ErrorCode::Process));
    }
    ::fcntl(out[0], F_SETFL, O_NONBLOCK);
    ::fcntl(err[0], F_SETFL, O_NONBLOCK);
    ShellResult result;
    const auto start = std::chrono::steady_clock::now();
    bool done = false, aborted = false, timed = false;
    int status = 0;
    pollfd fds[2]{{out[0], POLLIN, 0}, {err[0], POLLIN, 0}};
    while (!done || fds[0].fd >= 0 || fds[1].fd >= 0) {
        if (!done && (stop.stop_requested() ||
                             (timeout.count() > 0 && std::chrono::steady_clock::now() - start >= timeout))) {
            aborted = stop.stop_requested();
            timed = !aborted;
            ::kill(-pid, SIGKILL);
        }
        (void)::poll(fds, 2, 20);
        for (int i = 0; i < 2; ++i)
            if (fds[i].fd >= 0 && (fds[i].revents & (POLLIN | POLLHUP | POLLERR))) {
                std::array<char, 4096> buf{};
                ssize_t n;
                while ((n = ::read(fds[i].fd, buf.data(), buf.size())) > 0) {
                    std::string_view chunk{buf.data(), static_cast<std::size_t>(n)};
                    result.output.append(chunk);
                    (i == 0 ? result.standard_output : result.standard_error).append(chunk);
                    if (output) output(chunk, i == 1);
                }
                if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
                    ::close(fds[i].fd);
                    fds[i].fd = -1;
                }
            }
        if (!done && ::waitpid(pid, &status, WNOHANG) == pid) done = true;
    }
    if (aborted) return std::unexpected(cancelled());
    if (timed)
        return std::unexpected(failure(
                std::format("timeout:{}", static_cast<double>(timeout.count()) / 1000.0), support::ErrorCode::Timeout));
    result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    return result;
}
} // namespace

std::shared_ptr<Environment> local_environment(std::string cwd) {
    return std::make_shared<LocalEnvironment>(std::move(cwd));
}
} // namespace cch::durable
