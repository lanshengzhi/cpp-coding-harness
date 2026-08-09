#pragma once

#include <cch/ai/Models.hpp>
#include <cch/coding_agent/Settings.hpp>
#include "coding_agent/AgentSession.hpp"
#include "coding_agent/ProjectResourceLoader.hpp"
#include "coding_agent/runtime/AgentSessionCreationRequest.hpp"
#include "coding_agent/runtime/AgentSessionRuntime.hpp"
#include "coding_agent/runtime/AsyncUserShell.hpp"
#include "coding_agent/runtime/SessionLifecycle.hpp"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <optional>
#include <vector>

namespace cch::coding_agent::runtime {

/// Internal result of SessionFactory::create(). The internal
/// create_agent_session() wraps this into the CLI-facing
/// CreateAgentSessionResult.
struct CreateAgentSessionResult {
    /// The assembled runtime for the session.
    std::unique_ptr<AgentSessionRuntime> runtime;
    /// Diagnostics collected during creation (provider fallback,
    /// resource load warnings, etc.).
    std::vector<coding_agent::SessionDiagnostic> diagnostics;
    /// pi `modelFallbackMessage`: set when a persisted session's stored
    /// `model_change` identity could not be restored (interactive boot
    /// warning only; never a stderr diagnostic).
    std::optional<std::string> model_fallback_message;
    /// Theme documents collected by the resource loader (pi
    /// `resourceLoader.getThemes()`): the interactive boot registers and
    /// applies them through the theme controller (`setRegisteredThemes` +
    /// `applyFromSettings`). Parsing stays in the TUI layer.
    std::vector<coding_agent::LoadedThemeResource> theme_resources;
    /// Resolved session metadata.
    std::string session_id;
    std::string provider;
    std::string model;
    std::optional<std::filesystem::path> session_path;
    std::filesystem::path workspace;
    harness::session::SessionMetadata metadata;
};

/// Assembles an AgentSessionRuntime (and its supporting services) from the
/// internal CLI creation request. Each creation attempt loads User Settings
/// at most once and assembles the session from that single snapshot; adapters
/// never supply a settings snapshot.
class SessionFactory {
public:
    [[nodiscard]] static util::Expected<CreateAgentSessionResult> create(
        AgentSessionCreationRequest request);
    /// Private test-support assembly seam for the scripted fake provider: the
    /// injected Models carries the scripted fake provider, and the request
    /// model is fabricated from it (the deterministic provider surface the
    /// `--fake` flag used to drive). Models never appears in the installed
    /// contract surface.
    [[nodiscard]] static util::Expected<CreateAgentSessionResult> create(
        AgentSessionCreationRequest request,
        std::shared_ptr<ai::Models> models);
    /// Private test-support assembly seam with an injected Session-owned User
    /// Shell (User Bash overlap/cancellation tests).
    [[nodiscard]] static util::Expected<CreateAgentSessionResult> create(
        AgentSessionCreationRequest request,
        std::shared_ptr<ai::Models> models,
        std::unique_ptr<AsyncUserShell> user_shell);
};

} // namespace cch::coding_agent::runtime
