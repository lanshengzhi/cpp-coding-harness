#pragma once

#include "EnvVarGuard.hpp"

#include <string>

namespace cch::tests {

/// Save, scrub, and restore every env variable the pi detectCapabilities env
/// rules read (terminal-image.ts at baseline 83114817), so capability
/// detection tests are hermetic regardless of the runner's own terminal
/// emulator. `set(name, value)` applies one override after the scrub.
class ImageEnvironmentGuard final {
public:
    ImageEnvironmentGuard() {
        scrub();
    }
    ImageEnvironmentGuard(const ImageEnvironmentGuard&) = delete;
    ImageEnvironmentGuard& operator=(const ImageEnvironmentGuard&) = delete;

    void set(std::string name, std::string value) {
        variable(name)->set(value);
    }

private:
    EnvVarGuard* variable(const std::string& name) {
        if (name == "TERM_PROGRAM") return &term_program;
        if (name == "TERMINAL_EMULATOR") return &terminal_emulator;
        if (name == "TERM") return &term;
        if (name == "TMUX") return &tmux;
        if (name == "KITTY_WINDOW_ID") return &kitty_window_id;
        if (name == "GHOSTTY_RESOURCES_DIR") return &ghostty_resources_dir;
        if (name == "WEZTERM_PANE") return &wezterm_pane;
        if (name == "WARP_SESSION_ID") return &warp_session_id;
        if (name == "WARP_TERMINAL_SESSION_UUID") return &warp_terminal_session_uuid;
        if (name == "ITERM_SESSION_ID") return &iterm_session_id;
        if (name == "WT_SESSION") return &wt_session;
        return &cmux_workspace_id;
    }

    void scrub() {
        term_program.unset();
        terminal_emulator.unset();
        term.unset();
        tmux.unset();
        kitty_window_id.unset();
        ghostty_resources_dir.unset();
        wezterm_pane.unset();
        warp_session_id.unset();
        warp_terminal_session_uuid.unset();
        iterm_session_id.unset();
        wt_session.unset();
        cmux_workspace_id.unset();
    }

    EnvVarGuard term_program{"TERM_PROGRAM"};
    EnvVarGuard terminal_emulator{"TERMINAL_EMULATOR"};
    EnvVarGuard term{"TERM"};
    EnvVarGuard tmux{"TMUX"};
    EnvVarGuard kitty_window_id{"KITTY_WINDOW_ID"};
    EnvVarGuard ghostty_resources_dir{"GHOSTTY_RESOURCES_DIR"};
    EnvVarGuard wezterm_pane{"WEZTERM_PANE"};
    EnvVarGuard warp_session_id{"WARP_SESSION_ID"};
    EnvVarGuard warp_terminal_session_uuid{"WARP_TERMINAL_SESSION_UUID"};
    EnvVarGuard iterm_session_id{"ITERM_SESSION_ID"};
    EnvVarGuard wt_session{"WT_SESSION"};
    EnvVarGuard cmux_workspace_id{"CMUX_WORKSPACE_ID"};
};

} // namespace cch::tests
