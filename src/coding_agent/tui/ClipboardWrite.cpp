#include "coding_agent/tui/ClipboardWrite.hpp"

#include "util/UniqueFd.hpp"

#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#elif defined(_WIN32)
#include <process.h>
#endif

namespace cch::coding_agent::tui {
namespace {

/// One platform-tool invocation: the program and its arguments; the text is
/// piped to its stdin.
struct ClipboardToolCommand {
    std::string program;
    std::vector<std::string> args;
};

/// The first tool to try for the current platform/env (pi's order: darwin
/// `pbcopy`, win32 `clip`, Linux Termux, then Wayland, then X11).
[[nodiscard]] std::optional<ClipboardToolCommand> primary_clipboard_tool() {
#if defined(__APPLE__)
    return ClipboardToolCommand{.program = "pbcopy", .args = {}};
#elif defined(_WIN32)
    return ClipboardToolCommand{.program = "clip", .args = {}};
#else
    if (const char* termux = std::getenv("TERMUX_VERSION");
        termux != nullptr && termux[0] != '\0') {
        return ClipboardToolCommand{.program = "termux-clipboard-set", .args = {}};
    }
    if (const char* wayland = std::getenv("WAYLAND_DISPLAY");
        wayland != nullptr && wayland[0] != '\0') {
        return ClipboardToolCommand{.program = "wl-copy", .args = {}};
    }
    if (const char* display = std::getenv("DISPLAY");
        display != nullptr && display[0] != '\0') {
        return ClipboardToolCommand{.program = "xclip", .args = {"-selection", "clipboard"}};
    }
    return std::nullopt;
#endif
}

#if defined(__unix__) || defined(__APPLE__)

/// Run one tool with `text` on its stdin; true when the child exits 0.
/// wl-copy keeps the clipboard only while it runs, so the parent waits for
/// its exit code exactly like pi's spawn-and-await (and unlike the X11
/// tools, which daemonize themselves).
[[nodiscard]] bool run_clipboard_tool(
    const ClipboardToolCommand& command,
    std::string_view text) {
    int pipe_fds[2];
    if (::pipe(pipe_fds) < 0) return false;
    // The descriptors are RAII-owned (CODING_STANDARDS 7.8); the pipe's read
    // end feeds the child's stdin.
    util::UniqueFd read_end{pipe_fds[0]};
    util::UniqueFd write_end{pipe_fds[1]};
    const auto pid = ::fork();
    if (pid < 0) return false;
    if (pid == 0) {
        // Child: stdin comes from the pipe; stdout/stderr go to /dev/null
        // (pi `stdio: ["pipe", "ignore", "ignore"]`).
        (void)::dup2(read_end.get(), STDIN_FILENO);
        (void)read_end.close();
        (void)write_end.close();
        util::UniqueFd null_fd{::open("/dev/null", O_RDWR)};
        if (null_fd) {
            (void)::dup2(null_fd.get(), STDOUT_FILENO);
            (void)::dup2(null_fd.get(), STDERR_FILENO);
        }
        // execvp takes `char* const[]`; the argv storage below is owned and
        // the strings are never modified (the OpenBrowser precedent).
        std::vector<char*> argv;
        argv.reserve(command.args.size() + 2);
        argv.push_back(const_cast<char*>(command.program.c_str()));
        for (const auto& arg : command.args) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);
        ::execvp(argv[0], argv.data());
        ::_exit(127);
    }
    // Parent: write the text, close, and reap the child.
    (void)read_end.close();
    std::size_t written = 0;
    while (written < text.size()) {
        const auto count = ::write(
            write_end.get(),
            text.data() + written,
            text.size() - written);
        if (count < 0) {
            if (errno == EINTR) continue;
            break;
        }
        written += static_cast<std::size_t>(count);
    }
    (void)write_end.close();
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) continue;
        return false;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/// pi `copyToX11Clipboard`: try `xclip -selection clipboard`, falling back to
/// `xsel --clipboard --input`; both failures are swallowed and the copy is
/// claimed (pi claims success whenever the tools ran).
[[nodiscard]] bool copy_to_x11_clipboard(std::string_view text) {
    (void)run_clipboard_tool(
        ClipboardToolCommand{.program = "xclip", .args = {"-selection", "clipboard"}},
        text);
    (void)run_clipboard_tool(
        ClipboardToolCommand{.program = "xsel", .args = {"--clipboard", "--input"}},
        text);
    return true;
}

#elif defined(_WIN32)

[[nodiscard]] bool run_clipboard_tool(
    const ClipboardToolCommand& command,
    std::string_view text) {
    // Windows has no fork/pipe here; a bounded temp file feeds `clip`'s
    // stdin through a shell-free redirect (`clip` reads stdin until EOF).
    const auto temp = std::tmpfile();
    if (temp == nullptr) return false;
    const auto written =
        std::fwrite(text.data(), 1, text.size(), temp);
    (void)std::fflush(temp);
    const auto descriptor = ::fileno(temp);
    (void)::_lseeki64(descriptor, 0, SEEK_SET);
    // `clip` does not accept a file argument; run it with stdin redirected.
    std::vector<const char*> argv;
    argv.reserve(command.args.size() + 2);
    argv.push_back(command.program.c_str());
    for (const auto& arg : command.args) argv.push_back(arg.c_str());
    argv.push_back(nullptr);
    const auto pid = ::_spawnvp(_P_NOWAIT, command.program.c_str(), argv.data());
    (void)std::fclose(temp);
    return pid != -1 && written == text.size();
}

#else

[[nodiscard]] bool run_clipboard_tool(
    const ClipboardToolCommand& command,
    std::string_view text) {
    (void)command;
    (void)text;
    return false;
}

#endif

} // namespace

bool write_clipboard_text(std::string_view text) {
#if defined(__unix__) || defined(__APPLE__)
    const auto primary = primary_clipboard_tool();
    if (!primary) return false;
    if (primary->program == "wl-copy") {
        // pi: verify wl-copy exists, then await a clean exit so a failed
        // wl-copy falls through to the X11 tools.
        const auto wayland_ok = run_clipboard_tool(*primary, text);
        if (wayland_ok) return true;
        const char* display = std::getenv("DISPLAY");
        if (display != nullptr && display[0] != '\0') {
            return copy_to_x11_clipboard(text);
        }
        return false;
    }
    if (primary->program == "xclip") {
        return copy_to_x11_clipboard(text);
    }
    return run_clipboard_tool(*primary, text);
#elif defined(_WIN32)
    const auto primary = primary_clipboard_tool();
    if (!primary) return false;
    return run_clipboard_tool(*primary, text);
#else
    (void)text;
    return false;
#endif
}

} // namespace cch::coding_agent::tui
