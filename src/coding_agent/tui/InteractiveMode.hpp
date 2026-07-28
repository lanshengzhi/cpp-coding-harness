#pragma once

#include "coding_agent/tui/ClipboardReader.hpp"

#include <cch/tui/Keybindings.hpp>
#include <cch/util/Error.hpp>

#include <boost/asio/awaitable.hpp>

#include <filesystem>
#include <memory>

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
};

/// Run the private Native TUI composition until its exit binding is received.
/// The borrowed Agent Session and Terminal must outlive the returned coroutine.
[[nodiscard]] boost::asio::awaitable<util::ExpectedVoid> run_interactive_mode(
    AgentSession& session,
    cch::tui::Terminal& terminal,
    InteractiveModeConfig config = {});

} // namespace cch::coding_agent::tui
