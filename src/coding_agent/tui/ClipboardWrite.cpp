#include "ClipboardWrite.hpp"

#include "support/UniqueFd.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace cch::coding_agent::tui {
namespace {

/// pi's writer-command timeout (`copyToClipboard` passes `timeoutMs: 5000`).
constexpr std::chrono::milliseconds kCopyCommandTimeout{5000};
/// pi's `wslpath` timeout in `copyViaWindowsClipboard`.
constexpr std::chrono::milliseconds kWslPathTimeout{1000};
/// pi's `powershell.exe` timeout in `copyViaWindowsClipboard`.
constexpr std::chrono::milliseconds kPowerShellTimeout{5000};

/// One candidate writer in pi's order: the environment gate is applied where
/// the list is built, so a candidate is present only when pi would try it.
struct CopyCommand {
    const char* command{nullptr};
    std::vector<std::string> args{};
};

[[nodiscard]] std::vector<CopyCommand> copy_commands(const ClipboardEnvironment& environment) {
    std::vector<CopyCommand> commands;
    if (!environment.is_linux) return commands;
    if (!environment.termux_version.empty()) commands.push_back({"termux-clipboard-set", {}});
    if (!environment.wayland_display.empty()) commands.push_back({"wl-copy", {}});
    if (!environment.display.empty()) {
        commands.push_back({"xclip", {"-selection", "clipboard"}});
        commands.push_back({"xsel", {"--clipboard", "--input"}});
    }
    return commands;
}

[[nodiscard]] ClipboardCommandRequest writer_request(
        const ClipboardEnvironment& environment, const CopyCommand& command, std::string_view text) {
    return ClipboardCommandRequest{
            .command = command.command,
            .args = command.args,
            .input = std::string{text},
            .timeout = kCopyCommandTimeout,
            .max_buffer_bytes = kClipboardCommandMaxBufferBytes,
            .search_path = environment.search_path,
    };
}

[[nodiscard]] ClipboardCommandRequest reader_request(std::string command,
        std::vector<std::string> args,
        const ClipboardEnvironment& environment,
        std::chrono::milliseconds timeout) {
    return ClipboardCommandRequest{
            .command = std::move(command),
            .args = std::move(args),
            .input = std::nullopt,
            .timeout = timeout,
            .max_buffer_bytes = kClipboardCommandMaxBufferBytes,
            .search_path = environment.search_path,
    };
}

[[nodiscard]] std::string trim(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return std::string{value.substr(first, last - first + 1)};
}

[[nodiscard]] std::string replace_all(std::string_view value, std::string_view from, std::string_view to) {
    std::string replaced;
    replaced.reserve(value.size());
    std::size_t start = 0;
    while (true) {
        const auto at = value.find(from, start);
        if (at == std::string_view::npos) break;
        replaced.append(value.substr(start, at - start));
        replaced.append(to);
        start = at + from.size();
    }
    replaced.append(value.substr(start));
    return replaced;
}

/// Write the whole payload to an owned descriptor; false on the first failure.
[[nodiscard]] bool write_all(const support::UniqueFd& fd, std::string_view bytes) {
    std::size_t written = 0;
    while (written < bytes.size()) {
        const auto count = ::write(fd.get(), bytes.data() + written, bytes.size() - written);
        if (count > 0) {
            written += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

/// One OSC 52 attempt through the terminal that owns the copy. `oversized`
/// distinguishes pi's size-limit failure from "no terminal route was
/// installed", which reports the environment's own failure message instead.
struct Osc52Emission {
    bool emitted{false};
    bool oversized{false};
};

[[nodiscard]] Osc52Emission emit_osc52(std::string_view text, ClipboardWriteOptions& options) {
    const auto sequence = encode_clipboard_osc52(text);
    if (!sequence) return Osc52Emission{.emitted = false, .oversized = true};
    if (!options.write_osc52) return Osc52Emission{};
    // pi `emitOsc52`: write the sequence and report success; the write's own
    // result is not part of the outcome.
    options.write_osc52(*sequence);
    return Osc52Emission{.emitted = true, .oversized = false};
}

/// pi `copyViaWindowsClipboard`: PowerShell reads the text from a UTF-8 file
/// because `clip.exe` and PowerShell stdin decode piped bytes with the console
/// code page, which mangles non-ASCII UTF-8. The temporary file is removed on
/// every path.
[[nodiscard]] bool copy_via_windows_clipboard(const ClipboardEnvironment& environment, std::string_view text) {
    std::error_code temp_error;
    const auto temp_directory = std::filesystem::temp_directory_path(temp_error);
    if (temp_error || temp_directory.empty()) return false;
    const auto temp_file = temp_directory / std::format("pi-wsl-clip-{}.txt", make_clipboard_unique_token());
    // The temporary file never outlives the attempt (pi's `finally` unlink).
    struct TempFileCleanup {
        std::filesystem::path path;
        ~TempFileCleanup() {
            std::error_code remove_error;
            std::filesystem::remove(path, remove_error);
        }
    } cleanup{temp_file};

    support::UniqueFd file{::open(temp_file.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600)};
    if (!file) return false;
    if (!write_all(file, text)) return false;
    if (file.close() != 0) return false;

    const auto converted = run_clipboard_command_blocking(
            reader_request("wslpath", {"-w", temp_file.string()}, environment, kWslPathTimeout));
    if (converted.exit_code != 0) return false;
    const auto windows_path = trim(converted.output);
    if (windows_path.empty()) return false;
    const auto script =
            std::format("Set-Clipboard -Value ([System.IO.File]::ReadAllText('{}', [System.Text.Encoding]::UTF8))",
                    replace_all(windows_path, "'", "''"));
    const auto result = run_clipboard_command_blocking(
            reader_request("powershell.exe", {"-NoProfile", "-Command", script}, environment, kPowerShellTimeout));
    return result.exit_code == 0;
}

/// pi's failure status for the environment that had a route but no working
/// tool.
[[nodiscard]] std::string unavailable_message(const ClipboardEnvironment& environment) {
    if (environment.is_linux) {
        if (!environment.termux_version.empty()) {
            return "Clipboard unavailable: install the Termux:API app and `termux-api` package";
        }
        if (!environment.wayland_display.empty()) {
            return "Clipboard unavailable: install `wl-clipboard` (`wl-copy`) or check Wayland access";
        }
        if (!environment.display.empty()) {
            return "Clipboard unavailable: install `xclip` or `xsel`, or check X11 access";
        }
    }
    return "Clipboard unavailable";
}

} // namespace

ClipboardCopyResult write_clipboard_text(std::string_view text, ClipboardWriteOptions& options) {
    const auto& environment = options.environment;
    bool copied = false;
    for (const auto& command : copy_commands(environment)) {
        const auto request = writer_request(environment, command, text);
        if (run_clipboard_command_blocking(request).exit_code == 0) {
            copied = true;
            break;
        }
    }

    bool osc52_emitted = false;
    if (!copied && environment.is_linux && environment.is_wsl) {
        // Windows Terminal supports OSC 52; prefer it over the slower PowerShell
        // round trip.
        if (!environment.wt_session.empty()) {
            osc52_emitted = emit_osc52(text, options).emitted;
        }
        copied = osc52_emitted || copy_via_windows_clipboard(environment, text);
    }

    // OSC 52 cannot be verified, so a desktop session with a display reports the
    // failure instead (#9618). Without a display the terminal is the only
    // clipboard route (containers, WSL without WSLg), and remote sessions always
    // emit it to reach the client clipboard.
    bool oversized = false;
    if (!osc52_emitted &&
            (clipboard_is_remote_session(environment) || (!copied && clipboard_is_headless(environment)))) {
        const auto emission = emit_osc52(text, options);
        if (emission.emitted) {
            copied = true;
        } else {
            oversized = emission.oversized;
        }
    }

    if (copied) return ClipboardCopyResult{.copied = true, .error = {}};
    if (oversized) {
        return ClipboardCopyResult{
                .copied = false,
                .error = "Clipboard unavailable: text exceeds the OSC 52 size limit",
        };
    }
    return ClipboardCopyResult{.copied = false, .error = unavailable_message(environment)};
}

} // namespace cch::coding_agent::tui
