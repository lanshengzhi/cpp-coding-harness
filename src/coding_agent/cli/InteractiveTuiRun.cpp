#include "InteractiveTuiRun.hpp"

#include "agent/harness/RuntimeRoot.hpp"
#include "coding_agent/tui/LinuxClipboard.hpp"

#include <cch/coding_agent/AgentConfigDir.hpp>
#include <cch/coding_agent/ModelRuntime.hpp>

#include <optional>
#include <string>
#include <utility>

namespace cch::cli {

coding_agent::tui::InteractiveSessionRun assemble_interactive_session_run(InitialMessageResult initial,
        const CliConfig& config,
        std::shared_ptr<harness::RuntimeRoot> runtime_root,
        std::shared_ptr<coding_agent::ModelRuntime> shared_runtime,
        bool model_runtime_cli_fake,
        std::ostream& error_stream,
        coding_agent::tui::SessionIntentVariant session_intent,
        InteractiveClipboardInjection clipboard,
        coding_agent::tui::ExtensionUiRegistrationFactory extension_ui_registration_factory) {
    // One observed environment feeds both halves of the clipboard capability:
    // the reader the run installs as its clipboard and the copy policy the
    // engine runs. Nothing below the composition boundary reads the process
    // environment.
    const auto environment = clipboard.environment;
    return coding_agent::tui::InteractiveSessionRunBuilder{}
            .with_session_facts(config.session_facts)
            .with_agent_config_directory(coding_agent::agent_config_dir())
            .with_initial_prompt(initial.initial_message.empty()
                                         ? std::nullopt
                                         : std::optional<std::string>{std::move(initial.initial_message)})
            .with_initial_prompt_options(coding_agent::PromptOptions{
                    .expand_prompt_templates = true,
                    .images = std::move(initial.initial_images),
            })
            .with_session_intent(std::move(session_intent))
            .with_runtime_root(std::move(runtime_root))
            .with_shared_runtime(std::move(shared_runtime))
            .with_model_runtime_cli_fake(model_runtime_cli_fake)
            .with_error_stream(&error_stream)
            .with_native_clipboard(
                    coding_agent::tui::make_linux_clipboard_reader(environment, std::move(clipboard.native_helper)))
            .with_clipboard_write_options(coding_agent::tui::ClipboardWriteOptions{.environment = environment})
            .with_extension_ui_registration_factory(std::move(extension_ui_registration_factory))
            .build();
}

} // namespace cch::cli
