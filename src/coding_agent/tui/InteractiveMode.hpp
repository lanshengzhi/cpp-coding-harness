#pragma once

#include "coding_agent/tui/ClipboardReader.hpp"

#include "coding_agent/AgentSession.hpp"
#include <cch/tui/Keybindings.hpp>
#include <cch/util/Error.hpp>

#include <boost/asio/awaitable.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace cch::coding_agent {
class AgentSession;
} // namespace cch::coding_agent

namespace cch::tui {
class Terminal;
} // namespace cch::tui

namespace cch::coding_agent::tui {

struct InteractiveModeConfig {
    std::filesystem::path agent_config_directory;
    cch::tui::KeybindingPlatform platform{cch::tui::native_keybinding_platform()};
    std::unique_ptr<AsyncClipboardReader> clipboard_reader{nullptr};
    std::optional<std::string> initial_prompt{std::nullopt};
    PromptOptions initial_prompt_options{};
    /// pi `modelFallbackMessage` (sdk.ts `createAgentSession`): shown as a
    /// `Warning: <message>` boot line in the chat container (pi
    /// `interactive-mode.ts` `showWarning`). Absent in print mode.
    std::optional<std::string> model_fallback_message{std::nullopt};
    /// Browser opening for the login dialog's auth-URL view (pi
    /// `openBrowser`: detached argv spawn, never through a shell, best-effort).
    /// Null installs the real platform spawn; tests inject a recorder.
    std::move_only_function<void(std::string)> open_browser_sink{nullptr};
};

/// Run the private Native TUI composition until its exit binding is received.
/// The borrowed Agent Session and Terminal must outlive the returned coroutine.
[[nodiscard]] boost::asio::awaitable<util::ExpectedVoid> run_interactive_mode(
    AgentSession& session,
    cch::tui::Terminal& terminal,
    InteractiveModeConfig config = {});

} // namespace cch::coding_agent::tui
