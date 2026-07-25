#include "coding_agent/runtime/AsyncCliRuntime.hpp"

#include "cli/InteractiveCliFrontend.hpp"
#include "cli/JsonCliRenderer.hpp"
#include "cli/TextCliRenderer.hpp"
#include "coding_agent/AgentSessionBridge.hpp"
#include "coding_agent/runtime/RpcMode.hpp"

#include <iostream>
#include <string>
#include <variant>

namespace cch::cli {

int run_async_cli(const CliConfig& config) {
    coding_agent::runtime::AgentSessionCreationRequest request;
    request.fake = config.fake;
    request.enable_bash = config.enable_bash;
    request.project_trust_override = config.project_trust_override;
    request.disable_project_skills = config.no_skills;
    request.disable_prompt_templates = config.no_prompt_templates;
    request.prompt_template_paths = config.prompt_template_paths;
    request.workspace_explicit = config.workspace_explicit;
    request.max_turns = config.max_turns;
    request.workspace = config.workspace;
    request.session_target = config.session_target;
    request.session_dir = config.session_dir;
    request.provider_overrides = config.provider_overrides;

    auto created = coding_agent::create_agent_session(std::move(request));
    if (!created) {
        // The adapter adds create/resume presentation context from the
        // original CLI intent only; failure semantics stay in the factory's
        // error value and are never classified by message text.
        const auto& error = created.error();
        std::cerr << (std::holds_alternative<coding_agent::ExplicitResumeSessionTarget>(config.session_target)
                          ? "could not resume session: "
                          : "could not create session: ")
                  << error.message;
        if (!error.detail.empty() && error.detail != error.message) {
            std::cerr << ": " << error.detail;
        }
        std::cerr << '\n';
        if (error.context && !error.context->empty()) {
            std::cerr << "note: " << *error.context << '\n';
        }
        return 2;
    }

    for (const auto& diag : created->diagnostics) {
        const char* severity = "info";
        switch (diag.severity) {
        case coding_agent::SdkDiagnostic::Severity::Info:
            severity = "info";
            break;
        case coding_agent::SdkDiagnostic::Severity::Warning:
            severity = "warn";
            break;
        case coding_agent::SdkDiagnostic::Severity::Error:
            severity = "error";
            break;
        }
        std::string category = "session";
        std::string code = diag.code;
        if (const auto split = code.find(':'); split != std::string::npos) {
            category = code.substr(0, split);
            code = code.substr(split + 1);
        }
        std::cerr << '[' << category << ':' << severity << "] " << code << ": " << diag.message;
        if (diag.path) {
            std::cerr << " (" << *diag.path << ')';
        }
        std::cerr << '\n';
    }

    auto& session = *created->session;
    if (config.output_mode == OutputMode::Rpc) {
        return coding_agent::runtime::run_rpc_mode(coding_agent::runtime::RpcModeConfig{
            std::cin,
            std::cout,
            session,
            created->provider,
            created->model,
            created->workspace,
        });
    }

    InteractiveCliFrontendConfig frontend_config{
        .input = std::cin,
        .output = std::cout,
        .error = std::cerr,
        .repl = config.repl,
        .prompt = config.prompt,
    };

    auto run_frontend = [&](CliRenderer& renderer) {
        InteractiveCliFrontend frontend{
            session,
            renderer,
            created->metadata,
            std::move(frontend_config)};
        return exit_code_for(frontend.run());
    };

    if (config.output_mode == OutputMode::Json) {
        JsonCliRenderer renderer{std::cout, std::cerr};
        return run_frontend(renderer);
    }

    TextCliRenderer renderer{std::cout, std::cerr};
    return run_frontend(renderer);
}

} // namespace cch::cli
