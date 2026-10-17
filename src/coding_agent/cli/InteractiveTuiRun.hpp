#pragma once

// The Native TUI intake the CLI composition assembles (#517, #993): the
// Interactive Session Run the boot drives, together with the clipboard
// capabilities the composition boundary observes and installs. The assembly
// lives here rather than inline in the boot entry so a test can drive the
// production wiring — the same run, action sink, and clipboard reader the CLI
// boots — without a real terminal.
//
// Repository-private `cch_coding_agent` implementation header: not part of an
// Owner Interface, not installed, never exported.

#include "coding_agent/cli/CliConfig.hpp"
#include "coding_agent/cli/InitialPrompt.hpp"
#include "coding_agent/tui/ClipboardProcess.hpp"
#include "coding_agent/tui/ExtensionUiHost.hpp"
#include "coding_agent/tui/InteractiveSessionRun.hpp"

#include <cch/tui/NativeClipboard.hpp>

#include <iosfwd>
#include <memory>
#include <utility>

namespace cch::harness {
class RuntimeRoot;
} // namespace cch::harness

namespace cch::coding_agent {
class ModelRuntime;
} // namespace cch::coding_agent

namespace cch::cli {

/// The clipboard capabilities the composition boundary installs. The
/// environment is observed once here (`clipboard_environment_from_process`) and
/// injected, so no clipboard policy reads the process environment; the optional
/// platform helper is the only in-process clipboard provider, and #993 ships
/// none on Linux, where the command path serves the clipboard.
struct InteractiveClipboardInjection {
    coding_agent::tui::ClipboardEnvironment environment{};
    std::unique_ptr<cch::tui::NativeClipboard> native_helper{nullptr};
};

/// Assemble the one Interactive Session Run the Native TUI boots: the
/// CLI-owned facts, the resolved Native TUI root mode
/// (`cch::cli::select_tui_mode`), run intent, host effects, and the clipboard
/// reader and copy policy the injection names.
[[nodiscard]] coding_agent::tui::InteractiveSessionRun assemble_interactive_session_run(InitialMessageResult initial,
        const CliConfig& config,
        coding_agent::TuiMode tui_mode,
        std::shared_ptr<harness::RuntimeRoot> runtime_root,
        std::shared_ptr<coding_agent::ModelRuntime> shared_runtime,
        bool model_runtime_cli_fake,
        std::ostream& error_stream,
        coding_agent::tui::SessionIntentVariant session_intent,
        InteractiveClipboardInjection clipboard = {},
        coding_agent::tui::ExtensionUiRegistrationFactory extension_ui_registration_factory = nullptr,
        std::ostream* output_stream = nullptr);

} // namespace cch::cli
