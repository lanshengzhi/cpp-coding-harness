#include "SessionReplacementHost.hpp"

#include "coding_agent/runtime/AgentSessionCreationRequest.hpp"
#include "coding_agent/tui/InteractiveMode.hpp"

#include <utility>

namespace cch::cli {

[[nodiscard]] coding_agent::runtime::AgentSessionCreationRequest
finalize_replacement_session_request(
    coding_agent::runtime::AgentSessionCreationRequest request,
    const coding_agent::tui::InteractiveSessionFacts& facts,
    std::shared_ptr<harness::RuntimeTarget> execution_runtime_target,
    std::shared_ptr<coding_agent::ModelRuntime> model_runtime) {
    request.provide_user_shell = true;
    request.execution_runtime_target = std::move(execution_runtime_target);
    // pi `projectTrustByCwd`: the engine is the session-trust authority. Its
    // resolution already merges the CLI --approve/--no-approve override with
    // the boot prompt decision and the boot-workspace inheritance for
    // in-session replacement, so the host fills in only a request that left
    // the decision unset — overwriting an engine-resolved decision would
    // discard a session-only trust choice (issue #507).
    if (!request.project_trust_override.has_value()) {
        request.project_trust_override = facts.project_trust_override;
    }
    // Pure CLI-owned facts (pi `createRuntime` re-applies the CLI options to
    // each replacement): host-authoritative, load-bearing for the fields
    // `InteractiveEngine::make_session_request` deliberately omits.
    request.no_skills = facts.no_skills;
    request.no_prompt_templates = facts.no_prompt_templates;
    request.prompt_template_paths = facts.prompt_template_paths;
    request.skill_paths = facts.skill_paths;
    request.no_themes = facts.no_themes;
    request.theme_paths = facts.theme_paths;
    request.no_context_files = facts.no_context_files;
    request.system_prompt = facts.system_prompt;
    request.append_system_prompt = facts.append_system_prompt;
    request.provider = facts.provider;
    request.model = facts.model;
    request.models = facts.models;
    request.api_key = facts.api_key;
    if (model_runtime) {
        // The replacement Session reuses the host-shared Models runtime
        // instead of reconstructing one (issue #466); the factory never owns
        // or releases it, so closing a Session keeps the resources the
        // replacement needs.
        request.model_runtime = std::move(model_runtime);
    }
    return request;
}

} // namespace cch::cli
