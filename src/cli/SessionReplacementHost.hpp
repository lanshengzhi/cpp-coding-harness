#pragma once

#include <memory>

namespace cch::coding_agent {
class ModelRuntime;
} // namespace cch::coding_agent

namespace cch::coding_agent::runtime {
struct AgentSessionCreationRequest;
} // namespace cch::coding_agent::runtime

namespace cch::coding_agent::tui {
struct InteractiveSessionFacts;
} // namespace cch::coding_agent::tui

namespace cch::harness {
class RuntimeTarget;
} // namespace cch::harness

namespace cch::cli {

/// The interactive composition host's finalization of one engine-built
/// session-creation request (pi `createAgentSessionRuntime`'s `createRuntime`
/// closure), shared by the boot path's deferred boot-session creation and
/// every in-session session replacement delivered as `ReplaceSessionAction`.
///
/// Field ownership seam (issue #507):
/// - Session trust is engine-authoritative: `project_trust_override` arrives
///   resolved (the CLI `--approve`/`--no-approve` override, the boot prompt
///   decision, or the boot-workspace inheritance — pi `projectTrustByCwd`),
///   so the host fills it from the CLI facts only when the engine left it
///   unset; overwriting an engine-resolved decision would discard a
///   session-only trust choice.
/// - The pure CLI-owned resource and model facts are host-authoritative:
///   re-applied to every replacement request. Load-bearing for the fields
///   `InteractiveEngine::make_session_request` deliberately omits
///   (`no_themes`, `theme_paths`, `no_context_files`, `system_prompt`,
///   `append_system_prompt`) and an idempotent mirror of the engine-set
///   ones — both sides read the same `CliConfig` values.
/// - Host-only capabilities are always host-set: the interactive Session's
///   independent User Shell (ADR 0026), the CLI Runtime root's target, and
///   the host-shared Models runtime when one was created (issue #466); a
///   null shared runtime leaves the request's `model_runtime` untouched so
///   the factory default-creates a Session-owned one.
[[nodiscard]] coding_agent::runtime::AgentSessionCreationRequest
finalize_replacement_session_request(
    coding_agent::runtime::AgentSessionCreationRequest request,
    const coding_agent::tui::InteractiveSessionFacts& facts,
    std::shared_ptr<harness::RuntimeTarget> execution_runtime_target,
    std::shared_ptr<coding_agent::ModelRuntime> model_runtime);

} // namespace cch::cli
