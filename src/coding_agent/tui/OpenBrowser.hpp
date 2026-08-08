#pragma once

#include <string>
#include <vector>

namespace cch::coding_agent::tui {

/// One argv browser-launch command (pi `open-browser.ts` platform table).
struct BrowserLaunchCommand {
    std::string program{};
    std::vector<std::string> args{};
};

/// pi `openBrowser`'s platform command selection: `open` on macOS,
/// `rundll32 url.dll,FileProtocolHandler` on Windows, `xdg-open` elsewhere.
[[nodiscard]] BrowserLaunchCommand browser_launch_command(std::string target);

/// pi `openBrowser`: open a URL in the platform browser via direct argv
/// spawn — never through a shell (pi documents the cmd.exe metacharacter
/// injection constraint) — detached and best-effort: launcher failures are
/// ignored and the caller still presents the target to the user.
void open_browser(std::string target);

} // namespace cch::coding_agent::tui
