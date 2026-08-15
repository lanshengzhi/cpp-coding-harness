#include "OpenBrowser.hpp"

#include <cstdlib>
#include <utility>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace cch::coding_agent::tui {

BrowserLaunchCommand browser_launch_command(std::string target) {
    return BrowserLaunchCommand{.program = "xdg-open", .args = {std::move(target)}};
}

void open_browser(std::string target) {
    const auto command = browser_launch_command(std::move(target));
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
}

} // namespace cch::coding_agent::tui
