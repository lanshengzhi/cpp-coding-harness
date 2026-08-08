#include "OpenBrowser.hpp"

#include <cstdlib>
#include <utility>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>
#elif defined(_WIN32)
#include <process.h>
#endif

namespace cch::coding_agent::tui {

BrowserLaunchCommand browser_launch_command(std::string target) {
#if defined(__APPLE__)
    return BrowserLaunchCommand{.program = "open", .args = {std::move(target)}};
#elif defined(_WIN32)
    return BrowserLaunchCommand{
        .program = "rundll32",
        .args = {"url.dll,FileProtocolHandler", std::move(target)},
    };
#else
    return BrowserLaunchCommand{.program = "xdg-open", .args = {std::move(target)}};
#endif
}

void open_browser(std::string target) {
    const auto command = browser_launch_command(std::move(target));
#if defined(__unix__) || defined(__APPLE__)
    // Double-fork detach: the intermediate child exits immediately so the
    // launcher is re-parented and never becomes a zombie; stdio is pinned to
    // /dev/null (pi: `stdio: "ignore", detached: true` + `unref()`).
    const auto intermediate = ::fork();
    if (intermediate < 0) return;
    if (intermediate == 0) {
        const auto child = ::fork();
        if (child < 0) ::_exit(0);
        if (child > 0) ::_exit(0);
        (void)::setsid();
        const int null_fd = ::open("/dev/null", O_RDWR);
        if (null_fd >= 0) {
            (void)::dup2(null_fd, STDIN_FILENO);
            (void)::dup2(null_fd, STDOUT_FILENO);
            (void)::dup2(null_fd, STDERR_FILENO);
            if (null_fd > STDERR_FILENO) (void)::close(null_fd);
        }
        std::vector<char*> argv;
        argv.reserve(command.args.size() + 2);
        argv.push_back(const_cast<char*>(command.program.c_str()));
        for (const auto& arg : command.args) argv.push_back(const_cast<char*>(arg.c_str()));
        argv.push_back(nullptr);
        ::execvp(argv[0], argv.data());
        ::_exit(127);
    }
    int status = 0;
    while (::waitpid(intermediate, &status, 0) < 0) {
    }
#elif defined(_WIN32)
    std::vector<const char*> argv;
    argv.reserve(command.args.size() + 2);
    argv.push_back(command.program.c_str());
    for (const auto& arg : command.args) argv.push_back(arg.c_str());
    argv.push_back(nullptr);
    (void)::_spawnvp(_P_DETACH, command.program.c_str(), argv.data());
#else
    (void)command;
#endif
}

} // namespace cch::coding_agent::tui
