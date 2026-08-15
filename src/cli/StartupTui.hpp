#pragma once

#include "coding_agent/tui/SessionSelector.hpp"

#include <cch/tui/Keybindings.hpp>
#include <cch/tui/Terminal.hpp>
#include <cch/support/Error.hpp>

#include <boost/asio/awaitable.hpp>

#include <filesystem>
#include <optional>
#include <string>

namespace cch::cli {

/// pi `startup-ui.ts` `createStartupTui` options (subset): the agent config
/// directory (keybindings.json read like pi `KeybindingsManager.create()`
/// and the boot theme load) and the raw global-scope `theme` setting for the
/// startup theme init. Theme registration stays skipped: pi's
/// `setRegisteredThemes(loadStartupThemes(...))` is absent per the G5 record
/// — the startup TUI resolves the settings theme (slash values read as
/// unset) or the COLORFGBG env default through the G5 controller default
/// (`init_boot_theme`) and registers nothing.
struct StartupTuiOptions {
    std::filesystem::path agent_config_directory;
    std::optional<std::string> theme_setting;
    cch::tui::KeybindingPlatform platform{
        cch::tui::native_keybinding_platform()};
};

/// pi `session-picker.ts` `selectSession` outcome.
enum class StartupPickerOutcome {
    Selected,
    Cancelled,
    Exited,
};

struct StartupPickerResult {
    StartupPickerOutcome outcome{StartupPickerOutcome::Cancelled};
    /// The picked session path; absent unless `outcome == Selected` (pi
    /// `selectSession` returns `string | null`).
    std::optional<std::filesystem::path> session_path;
};

/// pi `session-picker.ts` `selectSession` subset: the minimal pi-shaped
/// startup TUI — the Tui root in pi `TuiMainScreen`'s role hosting the
/// session selector (pi `SessionSelectorComponent` with
/// `showRenameHint: false`, so rename stays hidden) — over the effective
/// session space loaders (pi `SessionManager.list`/`listAll` closures).
/// Selection resolves with the picked session path; cancel and exit resolve
/// without one (pi's `selectSession` cancel → null, exit → process exit 0).
/// The host stops and clears the screen before returning (pi
/// `clearStartupTui` + `ui.stop()`).
[[nodiscard]] boost::asio::awaitable<support::Expected<StartupPickerResult>>
run_startup_session_picker(
    cch::tui::Terminal& terminal,
    StartupTuiOptions options,
    coding_agent::tui::SessionListLoader current_loader,
    coding_agent::tui::SessionListLoader all_loader);

/// pi `startup-ui.ts` `showStartupSelector` subset for the boot missing-cwd
/// prompt: the generic string-list selector over pi's verbatim
/// `formatMissingSessionCwdPrompt` text with the Continue/Cancel options.
/// Returns true on Continue, false on Cancel (pi main.ts exits 0 on cancel).
[[nodiscard]] boost::asio::awaitable<support::Expected<bool>>
run_startup_missing_cwd_prompt(
    cch::tui::Terminal& terminal,
    StartupTuiOptions options,
    std::string title);

/// The ProcessTerminal-backed picker host (the real CLI, pi main.ts
/// `selectSession` before the main TUI): runs the startup TUI on its own
/// terminal + io_context and returns the picked session path, or nullopt on
/// cancel/exit. Fails cleanly when the descriptors are not terminals (pi's
/// ProcessTerminal throws on piped stdin the same way).
[[nodiscard]] support::Expected<std::optional<std::filesystem::path>>
run_process_terminal_resume_picker(
    StartupTuiOptions options,
    coding_agent::tui::SessionListLoader current_loader,
    coding_agent::tui::SessionListLoader all_loader);

/// The ProcessTerminal-backed boot missing-cwd Continue/Cancel prompt (pi
/// main.ts `promptForMissingSessionCwd` → `showStartupSelector`).
[[nodiscard]] support::Expected<bool> run_process_terminal_missing_cwd_prompt(
    StartupTuiOptions options,
    std::string title);

} // namespace cch::cli
