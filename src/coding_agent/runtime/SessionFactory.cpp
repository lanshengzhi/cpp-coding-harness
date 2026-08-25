#include "SessionFactory.hpp"

#include <cch/ai/Models.hpp>
#include <cch/agent/AgentContext.hpp>
#include <cch/coding_agent/AgentConfigDir.hpp>
#include <cch/coding_agent/AuthGuidance.hpp>
#include <cch/coding_agent/ModelRuntime.hpp>
#include <cch/coding_agent/ProjectResources.hpp>
#include <cch/coding_agent/ProjectTrust.hpp>
#include <cch/coding_agent/Settings.hpp>
#include <cch/agent/harness/session/SessionStore.hpp>
#include <cch/agent/tools/ToolFactories.hpp>
#include <cch/support/Error.hpp>
#include "support/AsyncResultBridge.hpp"
#include "coding_agent/ProjectResourceLoader.hpp"
#include "coding_agent/SessionCwd.hpp"
#include "coding_agent/SessionDiscovery.hpp"
#include "coding_agent/SessionPathPolicy.hpp"
#include "coding_agent/runtime/AgentSessionRuntime.hpp"
#include "coding_agent/runtime/LocalUserShell.hpp"
#include "coding_agent/runtime/RuntimeServices.hpp"
#include "coding_agent/runtime/SessionLifecycle.hpp"
#include "agent/harness/WorkspaceFileSystem.hpp"
#include "ai/providers/FakeProvider.hpp"
#include "support/ExpectedMacros.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace cch::coding_agent::runtime {
namespace {

struct AutomaticNewSessionTarget {
    std::filesystem::path workspace;
    /// Resolved CLI automatic-directory override (--session-dir, then
    /// PI_CODING_AGENT_SESSION_DIR, then settings sessionDir). When absent
    /// the workspace-keyed Agent Config Directory default applies.
    std::optional<std::filesystem::path> directory_override;
    /// pi `--session-id` warn-create: the new session's exact id.
    std::optional<std::string> session_id;
};

/// pi `SessionManager.open`: open-or-create at an exact path. The assembly
/// resumes the file when it exists with content and creates a new session
/// there otherwise.
struct OpenOrCreateSessionTarget {
    std::filesystem::path session_path;
    std::filesystem::path workspace;
};

struct InMemoryNewSessionTarget {
    std::filesystem::path workspace;
    /// pi `--session-id` + `--no-session`: the in-memory session's id.
    std::optional<std::string> session_id;
};

struct ResumeSessionTarget {
    std::filesystem::path resume_path;
    std::filesystem::path workspace;
    bool workspace_explicit{false};
};

/// pi `SessionManager.forkFrom`: the new session inherits the source's
/// history and is published in the effective session directory.
struct ForkTarget {
    std::filesystem::path source_path;
    std::optional<std::string> session_id;
    std::optional<std::filesystem::path> directory_override;
    std::filesystem::path workspace;
};

/// pi `SessionManager.continueRecent`: resume the most recent session in the
/// effective session directory (cwd-filtered only when a custom override is
/// in effect), or create a new persisted session when none exists.
struct ContinueRecentTarget {
    std::optional<std::filesystem::path> directory_override;
    std::filesystem::path workspace;
};

using NormalizedSessionTarget = std::variant<
    AutomaticNewSessionTarget,
    OpenOrCreateSessionTarget,
    InMemoryNewSessionTarget,
    ResumeSessionTarget,
    ForkTarget,
    ContinueRecentTarget>;

struct AssemblyPlan {
    NormalizedSessionTarget target;
    /// The model/auth runtime used by the session. Nullable: a runtime is
    /// default-created from the Agent Config Directory when absent (ADR
    /// 0029/0030).
    std::shared_ptr<ModelRuntime> model_runtime;
    /// True when the session owns the runtime (default-created, or wrapped by
    /// the factory from injected `ai::Models` in the private test seam) and
    /// must release it on close. A host-injected runtime is never disposed by
    /// the session (ADR 0029: no dispose ceremony; runtimes are reusable
    /// across sessions).
    bool model_runtime_owned{true};
    /// Private test seam: an explicit request Model supplied through the
    /// creation request (focused session tests with deterministic models).
    std::optional<ai::Model> requested_model;
    /// CLI-path fake-provider seam: the test-suite injected `ai::Models`
    /// carries the scripted fake provider, and the request model is fabricated
    /// from it (the request surface the deleted fake-provider CLI flag used to
    /// drive).
    bool cli_fake{false};
    /// pi CLI model selection (`--provider`, `--model`, `--models`, `--api-key`).
    struct CliModelSelection {
        std::optional<std::string> provider;
        std::optional<std::string> model;
        std::vector<std::string> models;
        std::optional<std::string> api_key;
    };
    CliModelSelection cli_selection;
    /// Private test seam: custom tools registered alongside the fixed built-in
    /// tool set (retry-continuation tests).
    std::vector<agent::Tool> custom_tools;
    /// Private test seam: the shared live PI_* facts holder wired into the
    /// model Bash Tool (live-refresh tests).
    std::shared_ptr<tools::BashSessionEnvironment> bash_session_environment;
    std::vector<std::string> prompt_template_paths;
    std::vector<std::string> skill_paths;
    std::optional<DefaultProjectTrust> default_project_trust;
    bool no_skills{false};
    bool no_prompt_templates{false};
    /// pi `--no-themes` and repeatable `--theme` paths (file or directory,
    /// resolved against the workspace).
    bool no_themes{false};
    std::vector<std::string> theme_paths;
    /// pi `--no-context-files` and the raw `--system-prompt` /
    /// `--append-system-prompt` values (P20).
    bool no_context_files{false};
    std::optional<std::string> system_prompt;
    std::vector<std::string> append_system_prompt;
    std::optional<bool> project_trust_override;
    std::size_t max_queued_messages{agent::kDefaultMaxQueuedMessages};
    std::size_t max_queued_bytes{agent::kDefaultMaxQueuedBytes};
    /// Interactive frontend assembly policy: provide the Session-owned User
    /// Shell (ADR 0026). Set only by CLI normalization for the interactive
    /// frontend.
    bool provide_user_shell{false};
    /// One target of the CLI-owned Runtime root. It is shared by the Session's
    /// filesystem and Shell capabilities and survives Session replacement.
    std::shared_ptr<harness::RuntimeTarget> execution_runtime_target;
    /// pi `switchSession` cwdOverride (in-session resume only): bind the
    /// resumed session's runtime to this cwd even when the header stores a
    /// different (missing) cwd.
    std::optional<std::filesystem::path> resume_cwd_override;
    /// Private in-session seam: the pi in-memory `createBranchedSession`
    /// seed (meaningful only with `InMemorySessionTarget`).
    std::optional<runtime::InMemoryBranchSeed> in_memory_branch_seed;
    /// pi `--name`: the session display name appended as a `session_info`
    /// entry after publication (pi appendSessionInfo).
    std::optional<std::string> session_name;
};

[[nodiscard]] SessionDiagnostic make_diag(SessionDiagnostic::Severity severity,
                                          std::string code,
                                          std::string message,
                                          std::optional<std::string> path = std::nullopt) {
    return SessionDiagnostic{severity, std::move(code), std::move(message), std::move(path)};
}

/// One Settings load per creation attempt. A bootstrap SettingsManager starts
/// with the project scope untrusted; resource loading later flips it, reloading
/// the project scope so model/Shell defaults honor project settings only while
/// the project is trusted. Scope load errors surface as warning diagnostics.
struct SettingsSnapshot {
    coding_agent::SettingsManager manager;
};

[[nodiscard]] SettingsSnapshot load_settings_snapshot(
    const std::filesystem::path& workspace) {
    return SettingsSnapshot{
        coding_agent::SettingsManager::create(
            workspace,
            coding_agent::agent_config_dir(),
            /* project_trusted */ false),
    };
}

/// Failed creation keeps the primary error and carries any settings load
/// warnings through the error context field.
[[nodiscard]] support::Error with_settings_fallback_context(
    support::Error error,
    const SettingsSnapshot& snapshot) {
    for (const auto& settings_error : snapshot.manager.errors()) {
        std::string warning = std::string{"could not load "} +
            (settings_error.scope == SettingsScope::Global
                 ? "global settings"
                 : "project settings") +
            ": " + settings_error.message;
        if (error.context && !error.context->empty()) {
            error.context = *error.context + "; " + warning;
        } else {
            error.context = warning;
        }
    }
    return error;
}

[[nodiscard]] SessionDiagnostic::Severity to_session_severity(ResourceDiagnosticType type) {
    switch (type) {
    case ResourceDiagnosticType::Warning:
        return SessionDiagnostic::Severity::Warning;
    case ResourceDiagnosticType::Error:
        return SessionDiagnostic::Severity::Error;
    case ResourceDiagnosticType::Collision:
        return SessionDiagnostic::Severity::Warning;
    }
    return SessionDiagnostic::Severity::Warning;
}

/// pi `ResourceDiagnostic` values carry no machine-readable code; the session
/// seam derives a stable code from the type (and collision resource type).
[[nodiscard]] std::string resource_diagnostic_code(const ResourceDiagnostic& diagnostic) {
    switch (diagnostic.type) {
    case ResourceDiagnosticType::Warning:
        return "resource:warning";
    case ResourceDiagnosticType::Error:
        return "resource:error";
    case ResourceDiagnosticType::Collision:
        if (diagnostic.collision) {
            switch (diagnostic.collision->resource_type) {
            case ResourceCollisionResourceType::Skill:
                return "resource:skill_collision";
            case ResourceCollisionResourceType::Prompt:
                return "resource:prompt_collision";
            case ResourceCollisionResourceType::Theme:
                return "resource:theme_collision";
            }
        }
        return "resource:collision";
    }
    return "resource:warning";
}

void add_project_resource_loading_diagnostics(
    std::vector<SessionDiagnostic>& diagnostics,
    const ProjectResourceLoadingResult& loading) {
    for (const auto& diag : loading.diagnostics) {
        diagnostics.push_back(make_diag(
            to_session_severity(diag.type),
            resource_diagnostic_code(diag),
            diag.message,
            diag.path));
    }
}

[[nodiscard]] std::vector<ExplicitPromptTemplateInput> make_explicit_template_inputs(
    const harness::WorkspaceFileSystem& fs,
    const std::vector<std::string>& paths) {
    std::vector<ExplicitPromptTemplateInput> inputs;
    inputs.reserve(paths.size());
    for (const auto& path : paths) {
        bool is_file = true;
        if (auto info = fs.fileInfo(path); info && info->kind == harness::FileKind::Directory) {
            is_file = false;
        }
        inputs.push_back(ExplicitPromptTemplateInput{
            .path = path,
            .is_file = is_file,
        });
    }
    return inputs;
}

[[nodiscard]] std::optional<std::string> find_duplicate_tool_name(
    const std::vector<agent::Tool>& tools) {
    std::set<std::string, std::less<>> seen;
    for (const auto& t : tools) {
        if (!t.execute) continue;
        const auto& name = t.definition.name;
        if (!seen.insert(name).second) {
            return name;
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::string> find_builtin_custom_collision(
    const std::set<std::string>& builtin_names,
    const std::vector<agent::Tool>& custom_tools) {
    for (const auto& t : custom_tools) {
        if (!t.execute) continue;
        if (builtin_names.contains(t.definition.name)) {
            return t.definition.name;
        }
    }
    return std::nullopt;
}

[[nodiscard]] support::ExpectedVoid validate_trust_store_path(
    const std::filesystem::path& trust_path,
    const std::filesystem::path& workspace) {
    if (trust_path.is_absolute() == false) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "trust_store_path must be absolute",
            "supply an absolute filesystem path outside the workspace"));
    }

    std::error_code ec;
    auto trust_canonical = std::filesystem::weakly_canonical(trust_path, ec);
    if (ec) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "trust_store_path cannot be canonicalized",
            trust_path.string()));
    }

    auto ws_canonical = std::filesystem::weakly_canonical(workspace, ec);
    if (ec) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "workspace cannot be canonicalized",
            workspace.string()));
    }

    if (trust_canonical == ws_canonical) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "trust_store_path must be outside the workspace",
            trust_path.string()));
    }

    auto [trust_it, ws_it] = std::mismatch(
        trust_canonical.begin(), trust_canonical.end(),
        ws_canonical.begin(), ws_canonical.end());
    if (ws_it == ws_canonical.end()) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "trust_store_path cannot be inside the workspace",
            trust_path.string()));
    }

    return {};
}

void cleanup_factory_env(harness::AsyncExecutionEnv* env) {
    if (env) {
        std::move(env->cleanup()).start(
            [](std::expected<void, harness::FileError>) noexcept {});
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Model / runtime resolution
// ─────────────────────────────────────────────────────────────────────────────

/// Fabricate the request Model for the CLI-path fake-provider seam: the
/// injected `ai::Models` carries the scripted fake provider whose catalog is
/// empty (deterministic test surface), so the request model is a truthful,
/// credential-free value pointing at it.
[[nodiscard]] ai::Model make_fake_request_model() {
    return ai::Model{
        .id = "fake-model",
        .name = "fake-model",
        .api = "scripted-fake",
        .provider = "fake",
        .base_url = "",
        .reasoning = false,
        .thinking_level_map = std::nullopt,
        .input = {ai::ModelInput::Text},
        .cost = {},
        .context_window = 128000,
        .max_tokens = 16384,
        .headers = std::nullopt,
        .compat = std::nullopt,
    };
}

[[nodiscard]] std::string lowercase(std::string value) {
    for (char& character : value) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return value;
}

/// Resolve a CLI `--model` pattern (with optional `--provider`) against the
/// live runtime catalog using all models (not only pre-configured ones), so
/// `--api-key` can enable first-time setup (pi `resolveCliModel` subset).
/// Supports `--provider <name> --model <pattern>` and `--model <provider>/<pattern>`;
/// exact id / `provider/id` matches win, then partial id/name matches.
[[nodiscard]] support::Expected<ai::Model> resolve_cli_model_pattern(
    const ModelRuntime& runtime,
    const std::optional<std::string>& cli_provider,
    std::string_view cli_model) {
    const auto all_models = runtime.models();
    if (all_models.empty()) {
        return std::unexpected(support::make_error(
            support::ErrorCode::ModelValidation,
            "No models available. Check your installation or add models to models.json."));
    }

    // Canonical provider lookup (case-insensitive).
    std::map<std::string, std::string, std::less<>> provider_map;
    for (const auto& model : all_models) {
        provider_map.emplace(lowercase(model.provider), model.provider);
    }

    std::optional<std::string> provider = cli_provider;
    std::string pattern{cli_model};
    const bool inferred_provider =
        !provider && pattern.find('/') != std::string::npos;
    if (!provider) {
        const auto slash = pattern.find('/');
        if (slash != std::string::npos) {
            const auto maybe_provider = lowercase(pattern.substr(0, slash));
            if (const auto found = provider_map.find(maybe_provider);
                found != provider_map.end()) {
                provider = found->second;
                pattern = pattern.substr(slash + 1);
            }
        }
    }
    if (cli_provider) {
        const auto found = provider_map.find(lowercase(*cli_provider));
        if (found == provider_map.end()) {
            return std::unexpected(support::make_error(
                support::ErrorCode::ModelValidation,
                "Unknown provider \"" + *cli_provider +
                    "\". Use --list-models to see available providers/models."));
        }
        provider = found->second;
        // Tolerate --model <provider>/<pattern> alongside --provider.
        const auto prefix = *provider + "/";
        if (lowercase(pattern).starts_with(lowercase(prefix))) {
            pattern = pattern.substr(prefix.size());
        }
    }

    // Exact id / provider+id matches without provider inference.
    const auto exact_match = [&](const ai::Model& model) {
        return lowercase(model.id) == lowercase(std::string{cli_model}) ||
               lowercase(model.provider + "/" + model.id) == lowercase(std::string{cli_model});
    };
    if (!provider) {
        for (const auto& model : all_models) {
            if (exact_match(model)) {
                return model;
            }
        }
    }

    // Exact id match within the resolved provider.
    for (const auto& model : all_models) {
        if ((!provider || model.provider == *provider) &&
            model.id == pattern) {
            return model;
        }
    }
    // Partial id/name match within the resolved provider.
    for (const auto& model : all_models) {
        if ((!provider || model.provider == *provider) &&
            (model.id.find(pattern) != std::string::npos ||
             model.name.find(pattern) != std::string::npos)) {
            return model;
        }
    }
    // When a provider was inferred from the slash, retry the full input as a
    // raw model id across all models (OpenRouter-style ids).
    if (inferred_provider) {
        for (const auto& model : all_models) {
            if (model.id.find(cli_model) != std::string::npos) {
                return model;
            }
        }
    }

    return std::unexpected(support::make_error(
        support::ErrorCode::ModelValidation,
        "Unknown model \"" + std::string{cli_model} +
            "\". Use --list-models to see available models."));
}

/// Resolve `--models` / settings `enabledModels` patterns into the scoped
/// model set for cycling (pi `resolveModelScope` over the live availability
/// snapshot — scoping never includes providers without configured auth). An
/// empty pattern list yields an empty scope.
[[nodiscard]] std::vector<cch::coding_agent::ScopedModel> resolve_model_scope(
    const std::vector<std::string>& patterns,
    const ModelRuntime& runtime) {
    return cch::coding_agent::resolve_model_scope(patterns, runtime.get_available_snapshot());
}

/// Run the side-effect-free availability coroutine to completion on a
/// temporary executor so the synchronous session-assembly path can consult
/// live configured auth (pi `refreshAvailability`, which session creation and
/// `findInitialModel` both run). The per-provider checks never hit the
/// network and OAuth credentials are never refreshed; a refresh failure keeps
/// the last-known snapshot (pi keeps the previous list on failure).
void refresh_availability_sync(
    const std::shared_ptr<ModelRuntime>& runtime) {
    boost::asio::io_context io;
    auto future = boost::asio::co_spawn(
            io,
            [runtime]() -> boost::asio::awaitable<void> {
                (void)co_await support::detail::await_async_result(runtime->get_available());
                co_return;
            },
            boost::asio::use_future);
    io.run();
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    try {
#endif
        future.get();
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    } catch (...) {
        // Availability failures keep the last-known snapshot.
    }
#endif
}

/// Frozen runtime default selection (pi `findInitialModel` step 4): the
/// default-model table over the available (configured-auth) models, then the
/// first available model, then the concrete unknown `kDefaultModel` (pi
/// `DEFAULT_MODEL`) — streaming fails through normal provider lookup, so no
/// construction-time default silently wins. The availability snapshot must
/// be live: the factory refreshes it before the resolution chain runs.
[[nodiscard]] ai::Model runtime_default_model(const ModelRuntime& runtime) {
    const auto available = runtime.get_available_snapshot();
    for (const auto* provider : {"openai-codex", "kimi-coding"}) {
        if (auto default_id = ModelRuntime::default_model_for_provider(provider)) {
            for (const auto& model : available) {
                if (model.provider == provider && model.id == *default_id) {
                    return model;
                }
            }
        }
    }
    if (!available.empty()) {
        return available.front();
    }
    return agent::detail::kDefaultModel;
}

/// pi `sdk.ts` `createAgentSession` restore check: the stored `model_change`
/// identity must resolve in the live runtime catalog AND its provider must
/// have configured auth (`restoredModel && hasConfiguredAuth`). Returns true
/// when the stored identity cannot be restored.
[[nodiscard]] bool resume_model_restore_failed(
    const ModelRuntime& runtime,
    const std::string& provider,
    const std::string& model_id) {
    auto restored = runtime.model(provider, model_id);
    if (!restored) {
        return true;
    }
    return !runtime.has_configured_auth(restored->provider);
}
/// Build the session's ModelRuntime: the injected/adopted runtime wins; a
/// default-created runtime derives its Agent Config Directory from the
/// environment default.
[[nodiscard]] support::Expected<std::shared_ptr<ModelRuntime>> build_runtime(
    std::shared_ptr<ModelRuntime> injected) {
    if (injected) {
        return injected;
    }
    ModelRuntimeOptions options;
    auto created = ModelRuntime::create(std::move(options));
    if (!created) {
        return std::unexpected(created.error());
    }
    return std::move(*created);
}

// ─────────────────────────────────────────────────────────────────────────────
// Normalization
// ─────────────────────────────────────────────────────────────────────────────

/// Resolve one canonical physical workspace directory shared by execution,
/// Session Metadata, and default storage so symbolic-link aliases collapse to
/// a single identity.
[[nodiscard]] support::Expected<std::filesystem::path> resolve_canonical_workspace(
    const std::filesystem::path& workspace) {
    if (workspace.empty()) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "workspace is required for new sessions",
            "supply a non-empty workspace path"));
    }

    std::error_code ec;
    auto resolved = std::filesystem::canonical(workspace, ec);
    if (ec) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "workspace cannot be resolved",
            workspace.string() + ": " + ec.message()));
    }
    if (!std::filesystem::is_directory(resolved, ec) || ec) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "workspace is not a directory",
            workspace.string() + (ec ? ": " + ec.message() : std::string{})));
    }
    return resolved;
}

/// CLI automatic-directory override precedence (pi: --session-dir, then
/// PI_CODING_AGENT_SESSION_DIR, then settings sessionDir, then the
/// workspace-keyed default). The first non-empty value wins and resolves
/// against the final canonical workspace.
[[nodiscard]] support::Expected<std::optional<std::filesystem::path>> resolve_cli_session_dir_override(
    const std::optional<std::string>& flag_value,
    const std::optional<std::string>& settings_value,
    const std::filesystem::path& canonical_workspace) {
    std::optional<std::string> env_value;
    if (const char* env = std::getenv("PI_CODING_AGENT_SESSION_DIR");
        env != nullptr && env[0] != '\0') {
        env_value = std::string{env};
    }
    return session_paths::resolve_effective_session_dir(
        flag_value, env_value, settings_value, canonical_workspace,
        coding_agent::home_directory());
}

struct SessionTargetNormalizationOptions {
    std::filesystem::path workspace;
    bool workspace_explicit{false};
    std::optional<std::string> cli_session_dir{std::nullopt};
    std::optional<std::string> settings_session_dir{std::nullopt};
};

[[nodiscard]] support::Expected<NormalizedSessionTarget> normalize_session_target(
    coding_agent::SessionTarget target,
    SessionTargetNormalizationOptions options) {
    auto workspace = resolve_canonical_workspace(options.workspace);
    if (!workspace) {
        return std::unexpected(workspace.error());
    }
    options.workspace = std::move(*workspace);

    if (const auto* default_target =
            std::get_if<DefaultPersistedSessionTarget>(&target)) {
        std::optional<std::filesystem::path> directory_override;
        auto resolved = resolve_cli_session_dir_override(
            options.cli_session_dir,
            options.settings_session_dir,
            options.workspace);
        if (!resolved) {
            return std::unexpected(resolved.error());
        }
        directory_override = std::move(*resolved);
        return NormalizedSessionTarget{AutomaticNewSessionTarget{
            .workspace = std::move(options.workspace),
            .directory_override = std::move(directory_override),
            .session_id = default_target->session_id,
        }};
    }
    if (auto* open_or_create =
            std::get_if<ExplicitOpenOrCreateSessionTarget>(&target)) {
        return NormalizedSessionTarget{OpenOrCreateSessionTarget{
            .session_path = std::move(open_or_create->path),
            .workspace = std::move(options.workspace),
        }};
    }
    if (auto* explicit_resume = std::get_if<ExplicitResumeSessionTarget>(&target)) {
        return NormalizedSessionTarget{ResumeSessionTarget{
            .resume_path = std::move(explicit_resume->path),
            .workspace = std::move(options.workspace),
            .workspace_explicit = options.workspace_explicit,
        }};
    }
    if (auto* fork = std::get_if<ForkSessionTarget>(&target)) {
        std::optional<std::filesystem::path> directory_override;
        auto resolved = resolve_cli_session_dir_override(
            options.cli_session_dir,
            options.settings_session_dir,
            options.workspace);
        if (!resolved) {
            return std::unexpected(resolved.error());
        }
        directory_override = std::move(*resolved);
        return NormalizedSessionTarget{ForkTarget{
            .source_path = std::move(fork->source_path),
            .session_id = std::move(fork->session_id),
            .directory_override = std::move(directory_override),
            .workspace = std::move(options.workspace),
        }};
    }
    if (std::holds_alternative<ContinueRecentSessionTarget>(target)) {
        std::optional<std::filesystem::path> directory_override;
        auto resolved = resolve_cli_session_dir_override(
            options.cli_session_dir,
            options.settings_session_dir,
            options.workspace);
        if (!resolved) {
            return std::unexpected(resolved.error());
        }
        directory_override = std::move(*resolved);
        return NormalizedSessionTarget{ContinueRecentTarget{
            .directory_override = std::move(directory_override),
            .workspace = std::move(options.workspace),
        }};
    }
    if (const auto* in_memory = std::get_if<InMemorySessionTarget>(&target)) {
        return NormalizedSessionTarget{InMemoryNewSessionTarget{
            .workspace = std::move(options.workspace),
            .session_id = in_memory->session_id,
        }};
    }
    return std::unexpected(support::make_error(
        support::ErrorCode::Validation,
        "unsupported session target"));
}

[[nodiscard]] support::Expected<AssemblyPlan> normalize_cli(
    AgentSessionCreationRequest request,
    const SettingsManager& settings) {
    AssemblyPlan plan;
    const auto merged = settings.settings();
    auto target = normalize_session_target(
        std::move(request.session_target),
        SessionTargetNormalizationOptions{
            .workspace = request.workspace,
            // The workspace is always the launch directory (pi
            // `workspace := cwd`); an explicit workspace override no longer
            // exists, so the resume mismatch check never sees one.
            .workspace_explicit = false,
            .cli_session_dir = request.session_dir,
            .settings_session_dir = merged.session_dir,
        });
    if (!target) {
        return std::unexpected(target.error());
    }
    plan.target = std::move(*target);

    plan.model_runtime = std::move(request.model_runtime);
    // A host-shared ModelRuntime (ADR 0029/0030, issue #466) is never owned
    // or released by the Session: closing one Session must not stop the
    // shared Models resources the replacement Session needs. The private test
    // seam keeps the same rule for its host-injected runtime.
    if (plan.model_runtime) {
        plan.model_runtime_owned = false;
    }
    plan.cli_selection = AssemblyPlan::CliModelSelection{
        .provider = std::move(request.session_facts.provider),
        .model = std::move(request.session_facts.model),
        .models = std::move(request.session_facts.models),
        .api_key = std::move(request.session_facts.api_key),
    };
    // Private test seams: an explicit request Model and custom tools flow
    // through the plan unchanged.
    plan.requested_model = std::move(request.request_model);
    plan.custom_tools = std::move(request.custom_tools);
    plan.bash_session_environment = std::move(request.bash_session_environment);
    plan.project_trust_override = request.project_trust_override.has_value()
        ? request.project_trust_override
        : request.session_facts.project_trust_override;
    plan.default_project_trust =
        settings.default_project_trust().value_or(DefaultProjectTrust::Ask);
    plan.no_skills = request.session_facts.no_skills;
    plan.no_prompt_templates = request.session_facts.no_prompt_templates;
    plan.prompt_template_paths = std::move(request.session_facts.prompt_template_paths);
    plan.skill_paths = std::move(request.session_facts.skill_paths);
    plan.no_themes = request.session_facts.no_themes;
    plan.theme_paths = std::move(request.session_facts.theme_paths);
    plan.no_context_files = request.session_facts.no_context_files;
    plan.system_prompt = std::move(request.session_facts.system_prompt);
    plan.append_system_prompt = std::move(request.session_facts.append_system_prompt);
    plan.max_queued_messages = request.max_queued_messages;
    plan.max_queued_bytes = request.max_queued_bytes;
    plan.provide_user_shell = request.provide_user_shell;
    plan.execution_runtime_target = std::move(request.execution_runtime_target);
    plan.session_name = std::move(request.session_name);
    plan.resume_cwd_override = std::move(request.resume_cwd_override);
    plan.in_memory_branch_seed = std::move(request.in_memory_branch_seed);

    return plan;
}

// ─────────────────────────────────────────────────────────────────────────────
// Assembly
// ─────────────────────────────────────────────────────────────────────────────

/// Resolve the CLI path's request Model through the pi chain: `--model`
/// (with `--provider`) wins; `--models`/`enabledModels` scope the selection
/// for new sessions; a resume re-resolves the stored `model_change` against
/// the live runtime; then settings `defaultProvider`/`defaultModel`; then the
/// runtime default. `out_scoped_models` receives the resolved `--models`/
/// settings `enabledModels` scope (pi `scopedModels` carried into the
/// session for Ctrl+P cycling; pi main.ts `parsed.models ??
/// settingsManager.getEnabledModels()`), and `out_scoped_thinking_level` the
/// first scoped entry's explicit `:level` when the scope selects the initial
/// model (pi main.ts `buildSessionOptions`), regardless of which chain step
/// selected the model.
[[nodiscard]] support::Expected<ai::Model> resolve_cli_request_model(
    const AssemblyPlan& plan,
    const ModelRuntime& runtime,
    bool is_resume,
    const std::optional<std::string>& stored_provider,
    const std::optional<std::string>& stored_model,
    bool resume_restore_failed,
    const coding_agent::UserSettings& settings,
    std::vector<cch::coding_agent::ScopedModel>* out_scoped_models = nullptr,
    std::optional<std::string>* out_scoped_thinking_level = nullptr) {
    const auto& selection = plan.cli_selection;

    // 0. `--models` / settings `enabledModels` scope the session's Ctrl+P
    // cycling set (pi main.ts `parsed.models ?? getEnabledModels()`; a resume
    // re-resolves its stored identity for the initial model but keeps the
    // scope). The scope resolves from the live availability snapshot, so
    // keyless providers are never in scope.
    std::vector<cch::coding_agent::ScopedModel> scoped;
    std::vector<std::string> patterns = selection.models;
    if (patterns.empty() && settings.enabled_models) {
        patterns = *settings.enabled_models;
    }
    if (!patterns.empty()) {
        scoped = resolve_model_scope(patterns, runtime);
        if (out_scoped_models != nullptr) *out_scoped_models = scoped;
    }

    // 1. CLI --model (with optional --provider) wins.
    if (selection.model) {
        return resolve_cli_model_pattern(runtime, selection.provider, *selection.model);
    }

    // 2. The resolved scope selects the initial model for new sessions when
    // no explicit --model was given.

    // 3. Resume: re-resolve the stored model_change identity against the live
    // runtime catalog and require configured auth (pi `restoreModelFromSession`:
    // `restoredModel && hasConfiguredAuth`). A restored model wins immediately;
    // a missing/unauthenticated model continues through settings and runtime
    // defaults without silent substitution — the boot fallback message is
    // assembled by run_assembly after the chain lands.
    if (!resume_restore_failed && stored_provider && stored_model) {
        if (auto restored = runtime.model(*stored_provider, *stored_model);
            restored && runtime.has_configured_auth(restored->provider)) {
            return *restored;
        }
    }

    // 4. Scoped models: the saved default when it is in scope, else the first
    // scoped model (pi buildSessionOptions). The matching entry's explicit
    // `:level` seeds the initial thinking level (pi main.ts
    // `savedInScope.thinkingLevel` / `scopedModels[0].thinkingLevel`).
    if (!scoped.empty() && !is_resume) {
        if (settings.default_provider && settings.default_model) {
            for (const auto& scoped_model : scoped) {
                const auto& model = scoped_model.model;
                if (model.provider == *settings.default_provider &&
                    model.id == *settings.default_model) {
                    if (out_scoped_thinking_level != nullptr) {
                        *out_scoped_thinking_level = scoped_model.thinking_level;
                    }
                    return model;
                }
            }
        }
        if (out_scoped_thinking_level != nullptr) {
            *out_scoped_thinking_level = scoped.front().thinking_level;
        }
        return scoped.front().model;
    }

    // 5. Settings defaultProvider/defaultModel — only when the provider has
    // configured auth (pi findInitialModel: `found && hasConfiguredAuth`). An
    // unauthenticated settings default falls through to the runtime default.
    if (settings.default_provider && settings.default_model) {
        if (auto model = runtime.model(
                *settings.default_provider, *settings.default_model);
            model && runtime.has_configured_auth(model->provider)) {
            return *model;
        }
    }

    // 6. Runtime default.
    return runtime_default_model(runtime);
}

/// pi `session-cwd.ts` `MissingSessionCwdError` + `formatMissingSessionCwdError`,
/// verbatim: the non-interactive boot error for a resumed session whose stored
/// header cwd no longer exists. The CLI prints the message alone (no
/// "could not resume session:" prefix) and exits 1; the interactive
/// Continue/Cancel prompt lands with the startup-TUI host.
[[nodiscard]] support::Error missing_session_cwd_error(
    const std::filesystem::path& session_file,
    const std::filesystem::path& session_cwd,
    const std::filesystem::path& fallback_cwd) {
    // pi `MissingSessionCwdError.message` (session-cwd.ts), verbatim.
    return support::make_error(
        support::ErrorCode::MissingSessionCwd,
        format_missing_session_cwd_error(MissingSessionCwdIssue{
            .session_file = session_file,
            .session_cwd = session_cwd,
            .fallback_cwd = fallback_cwd,
        }));
}

[[nodiscard]] support::Expected<coding_agent::CreateAgentSessionResult> run_assembly(
    AssemblyPlan plan,
    SettingsSnapshot& snapshot,
    std::unique_ptr<AsyncUserShell> user_shell) {
    std::vector<SessionDiagnostic> diagnostics;

    // 1. Resolve workspace and validate target shape. Resume-shaped targets
    // (explicit resume, open-or-create on an existing file, fork, continue
    // with a most-recent session) prepare their resume view first so the
    // model chain below re-resolves the stored identity.
    std::filesystem::path workspace;
    const auto launch_cwd = std::visit(
        [](const auto& target) { return target.workspace; }, plan.target);
    PreparedResumeTarget prepared_resume;
    std::optional<NewSessionPublication> new_publication;
    bool is_resume = false;

    if (const auto* target = std::get_if<ResumeSessionTarget>(&plan.target)) {
        auto prepared = prepare_resume_target(
            target->resume_path, target->workspace, target->workspace_explicit,
            plan.resume_cwd_override);
        if (!prepared) {
            return std::unexpected(prepared.error());
        }
        workspace = prepared->workspace;
        prepared_resume = std::move(*prepared);
        is_resume = true;
    } else if (const auto* target =
                   std::get_if<OpenOrCreateSessionTarget>(&plan.target)) {
        workspace = target->workspace;
        // pi `SessionManager.open`: an existing non-empty file resumes (cwd
        // from the header); a missing or empty regular file creates a new
        // session at the exact path (pi rewrites an empty file with a fresh
        // header). A non-regular existing entry is refused.
        std::error_code ec;
        const bool exists = std::filesystem::exists(target->session_path, ec);
        const bool regular =
            exists && std::filesystem::is_regular_file(target->session_path, ec);
        if (exists && !regular) {
            return std::unexpected(support::make_error(
                support::ErrorCode::Session,
                "session target is not a regular file",
                target->session_path.string()));
        }
        if (regular && std::filesystem::file_size(target->session_path, ec) > 0) {
            auto prepared = prepare_resume_target(
                target->session_path, workspace, false,
                plan.resume_cwd_override);
            if (!prepared) {
                return std::unexpected(prepared.error());
            }
            workspace = prepared->workspace;
            prepared_resume = std::move(*prepared);
            is_resume = true;
        } else {
            if (exists) {
                std::error_code remove_ec;
                if (!std::filesystem::remove(target->session_path, remove_ec) ||
                    remove_ec) {
                    return std::unexpected(support::make_error(
                        support::ErrorCode::Session,
                        "could not initialize session file",
                        target->session_path.string() +
                            (remove_ec ? ": " + remove_ec.message()
                                       : std::string{})));
                }
            }
            new_publication = ExplicitNewPublication{
                target->session_path, workspace};
        }
    } else if (const auto* target = std::get_if<ForkTarget>(&plan.target)) {
        workspace = target->workspace;
        auto prepared = prepare_fork_target(
            target->source_path,
            workspace,
            target->directory_override,
            target->session_id);
        if (!prepared) {
            return std::unexpected(prepared.error());
        }
        prepared_resume = std::move(*prepared);
        is_resume = true;
    } else if (const auto* target =
                   std::get_if<ContinueRecentTarget>(&plan.target)) {
        workspace = target->workspace;
        // pi `continueRecent`: the most recent session in the effective
        // session directory, cwd-filtered only when a custom override differs
        // from the workspace-keyed default; a new persisted session when
        // none exists.
        const auto sessions_root = coding_agent::sessions_root_path();
        const auto default_directory =
            sessions_root / session_paths::encode_workspace_key(workspace);
        const auto directory =
            target->directory_override.value_or(default_directory);
        const std::optional<std::filesystem::path> cwd_filter =
            (target->directory_override &&
             *target->directory_override != default_directory)
                ? std::optional<std::filesystem::path>{workspace}
                : std::nullopt;
        if (auto most_recent = session_discovery::find_most_recent_session(
                directory, cwd_filter)) {
            auto prepared = prepare_resume_target(
                most_recent->path, workspace, false);
            if (!prepared) {
                return std::unexpected(prepared.error());
            }
            workspace = prepared->workspace;
            prepared_resume = std::move(*prepared);
            is_resume = true;
        } else {
            new_publication = AutomaticPublication{
                workspace, target->directory_override, std::nullopt};
        }
    } else if (const auto* target =
                   std::get_if<AutomaticNewSessionTarget>(&plan.target)) {
        workspace = target->workspace;
        new_publication = AutomaticPublication{
            workspace, target->directory_override, target->session_id};
    } else if (const auto* target =
                   std::get_if<InMemoryNewSessionTarget>(&plan.target)) {
        workspace = target->workspace;
        new_publication = InMemoryPublication{workspace, target->session_id};
    } else {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "unsupported session target"));
    }

    // pi main.ts missing-cwd recovery (session-cwd.ts): a resumed session
    // whose stored header cwd no longer exists fails the boot with pi's
    // MissingSessionCwdError text. The check engages only when the header
    // cwd replaced the launch cwd (an empty header keeps the launch cwd, and
    // forks always target the launch cwd). The interactive Continue/Cancel
    // prompt lands with the startup-TUI host; until then both frontends
    // surface the stderr error and exit 1.
    if (is_resume && workspace != launch_cwd) {
        std::error_code exists_ec;
        if (!std::filesystem::exists(workspace, exists_ec)) {
            return std::unexpected(missing_session_cwd_error(
                prepared_resume.resume_path, workspace, launch_cwd));
        }
    }

    // pi main.ts: cwd-bound services (settings, resources, provider
    // registrations, models) resolve against the target session cwd, not the
    // process cwd — a resumed session from another project binds the project
    // settings scope to the session header cwd. The project scope stays
    // untrusted until the trust decision below reloads it.
    if (is_resume && snapshot.manager.cwd() != workspace) {
        snapshot.manager = coding_agent::SettingsManager::create(
            workspace,
            coding_agent::agent_config_dir(),
            /* project_trusted */ false);
    }

    // 2. Settings load errors stay observable as warning diagnostics. Global
    // errors are known at bootstrap; project-scope errors surface after the
    // project trust decision below reloads the project scope.
    const auto add_settings_diagnostics = [&]() {
        for (const auto& settings_error : snapshot.manager.errors()) {
            diagnostics.push_back(make_diag(
                SessionDiagnostic::Severity::Warning,
                "settings:" +
                    (settings_error.scope == SettingsScope::Global
                         ? std::string{"global"}
                         : std::string{"project"}),
                "could not load " +
                    (settings_error.scope == SettingsScope::Global
                         ? std::string{"global settings"}
                         : std::string{"project settings"}) +
                    ": " + settings_error.message,
                (settings_error.scope == SettingsScope::Global
                     ? snapshot.manager.global_path()
                     : snapshot.manager.project_path())
                    .string()));
        }
    };
    add_settings_diagnostics();

    // 3. Load project resources and resolve project trust first, so the
    // project settings scope loads only when the project is trusted and model /
    // Shell defaults honor project settings while the project is trusted. The
    // former SDK host-supplied skill/template injection is gone: skills and
    // templates come only from project discovery.
    std::vector<Skill> skills;
    std::vector<PromptTemplate> templates;
    std::vector<LoadedThemeResource> theme_documents;
    // P20 loader outputs that flow into the System Prompt: Project Context
    // Files, the custom system prompt, and the append strings.
    std::vector<prompt::ProjectContextFile> agents_files;
    std::optional<std::string> system_prompt_text;
    std::vector<std::string> append_system_prompt_texts;
    // Loaded-resources presentation + `/reload` inputs (pi
    // `getSystemPromptSource()`/`getAppendSystemPromptSources()` and the
    // per-kind diagnostics): dropped from the runtime today, must flow into
    // the runtime config (#418).
    std::optional<std::string> system_prompt_source;
    std::vector<std::string> append_system_prompt_sources;
    std::vector<ResourceDiagnostic> skill_diagnostics;
    std::vector<ResourceDiagnostic> prompt_diagnostics;
    std::vector<ResourceDiagnostic> theme_diagnostics;
    /// The resolved loading request, retained for the runtime's `/reload`
    /// re-run (pi's retained `DefaultResourceLoader` options, #418).
    std::optional<ProjectResourceLoadingRequest> retained_resource_request;
    std::filesystem::path trust_store_path =
        coding_agent::trust_store_file_path();
    if (auto valid = validate_trust_store_path(trust_store_path, workspace); !valid) {
        trust_store_path.clear();
    }

    bool project_trusted = false;
    {
        auto fs = harness::WorkspaceFileSystem::create(workspace);
        if (fs) {
            ProjectResourceLoadingRequest resource_request;
            resource_request.workspace = workspace;
            resource_request.agent_config_directory = coding_agent::agent_config_dir();
            resource_request.default_project_trust = plan.default_project_trust.value_or(DefaultProjectTrust::Ask);
            resource_request.project_trust_override = plan.project_trust_override;
            resource_request.no_skills = plan.no_skills;
            resource_request.no_prompt_templates = plan.no_prompt_templates;
            resource_request.skill_paths = plan.skill_paths;
            resource_request.no_themes = plan.no_themes;
            resource_request.theme_paths = plan.theme_paths;
            resource_request.no_context_files = plan.no_context_files;
            resource_request.system_prompt = plan.system_prompt;
            resource_request.append_system_prompt = plan.append_system_prompt;
            resource_request.explicit_prompt_templates = make_explicit_template_inputs(*fs, plan.prompt_template_paths);

            ProjectTrustStore trust_store{trust_store_path};
            // Retained for `/reload` (pi's `DefaultResourceLoader` instance
            // re-runs the same discovery options); the loader takes a copy so
            // the retained request survives.
            const auto retained_request_copy = resource_request;
            auto resource_loading = load_project_resources(*fs, trust_store, std::move(resource_request));
            if (!resource_loading.fatal_errors.empty()) {
                return std::unexpected(support::make_error(
                    support::ErrorCode::Validation,
                    "explicit resource failed to load",
                    resource_loading.fatal_errors.front().message));
            }
            add_project_resource_loading_diagnostics(diagnostics, resource_loading);
            project_trusted = resource_loading.trust.decision == ProjectTrustDecision::Trusted;
            skills = std::move(resource_loading.resources.skills);
            templates = std::move(resource_loading.resources.prompt_templates);
            theme_documents = std::move(resource_loading.resources.themes);
            agents_files = std::move(resource_loading.resources.agents_files);
            system_prompt_text = std::move(resource_loading.resources.system_prompt);
            append_system_prompt_texts =
                std::move(resource_loading.resources.append_system_prompt);
            system_prompt_source =
                std::move(resource_loading.resources.system_prompt_source);
            append_system_prompt_sources =
                std::move(resource_loading.resources.append_system_prompt_sources);
            skill_diagnostics = std::move(resource_loading.skill_diagnostics);
            prompt_diagnostics = std::move(resource_loading.prompt_diagnostics);
            theme_diagnostics = std::move(resource_loading.theme_diagnostics);
            retained_resource_request.emplace(retained_request_copy);
        } else {
            diagnostics.push_back(make_diag(
                SessionDiagnostic::Severity::Warning,
                "resource:workspace_fs_unavailable",
                fs.error().message,
                workspace.string()));
        }
    }
    if (auto trusted = snapshot.manager.set_project_trusted(project_trusted); !trusted) {
        return std::unexpected(trusted.error());
    }
    // A project-scope load error recorded during the trust flip must surface on
    // the success path too, not only through the failure-path context.
    add_settings_diagnostics();
    const auto& settings = snapshot.manager.settings();

    // 4. Resolve the model/auth runtime. An injected runtime wins; otherwise a
    // runtime is default-created from the Agent Config Directory.
    std::shared_ptr<ModelRuntime> runtime;
    if (plan.model_runtime) {
        runtime = std::move(plan.model_runtime);
    } else {
        auto built = build_runtime(nullptr);
        if (!built) {
            return std::unexpected(built.error());
        }
        runtime = std::move(*built);
    }

    // 5. Resolve provider/model metadata and the request Model. Resume stores
    // only `model_change {provider, modelId}`; the recorded identity is
    // re-resolved against the live runtime catalog. Refresh live availability
    // first so the resolution chain consults real configured auth (pi
    // `refreshAvailability` at runtime creation and inside `findInitialModel`):
    // `has_configured_auth` and the available snapshot then reflect actual
    // credential resolution, not the structural provider composition.
    refresh_availability_sync(runtime);

    std::optional<std::string> stored_provider;
    std::optional<std::string> stored_model;
    if (is_resume) {
        stored_provider = prepared_resume.resume.provider;
        stored_model = prepared_resume.resume.model;
    }

    // Resume: record whether the stored `model_change {provider, modelId}`
    // can be restored against the live runtime (pi `sdk.ts` `createAgentSession`:
    // the restored model must exist AND its provider must have configured
    // auth). An explicit `--model`/request model skips the restore check and
    // never produces the fallback message.
    bool resume_restore_failed = false;
    if (is_resume && stored_provider && stored_model &&
        !plan.cli_selection.model && !plan.requested_model && !plan.cli_fake) {
        resume_restore_failed = resume_model_restore_failed(
            *runtime, *stored_provider, *stored_model);
    }

    std::string resolved_provider;
    std::string resolved_model;
    ai::Model request_model;
    std::vector<cch::coding_agent::ScopedModel> scoped_models;
    std::optional<std::string> scoped_thinking_level;
    if (plan.requested_model) {
        // Private test seam: an explicit request Model wins directly.
        request_model = *plan.requested_model;
    } else if (plan.cli_fake) {
        // The scripted fake provider's catalog is empty; the request model
        // is fabricated. Explicit --model, then the stored resume identity,
        // then the settings default supply the observable model id.
        request_model = make_fake_request_model();
        if (plan.cli_selection.model) {
            request_model.id = *plan.cli_selection.model;
            request_model.name = *plan.cli_selection.model;
        } else if (is_resume && stored_model) {
            request_model.id = *stored_model;
            request_model.name = *stored_model;
        } else if (settings.default_model) {
            request_model.id = *settings.default_model;
            request_model.name = *settings.default_model;
        }
    } else {
        auto resolved = resolve_cli_request_model(
            plan, *runtime, is_resume, stored_provider, stored_model,
            resume_restore_failed, settings, &scoped_models, &scoped_thinking_level);
        if (!resolved) {
            return std::unexpected(resolved.error());
        }
        request_model = std::move(*resolved);
    }
    if (plan.cli_selection.api_key) {
        // In-memory runtime API key override, never persisted; requires an
        // explicit model (enforced at parse time too).
        if (auto override_set = runtime->set_runtime_api_key(
                request_model.provider, *plan.cli_selection.api_key);
            !override_set) {
            return std::unexpected(override_set.error());
        }
    }
    resolved_provider = request_model.provider;
    resolved_model = request_model.id;

    // pi `sdk.ts` `createAgentSession` `modelFallbackMessage`: when the stored
    // identity cannot be restored, the boot warning names the resolved
    // fallback (`. Using <provider>/<model>`); when nothing is available the
    // message is pi's `formatNoModelsAvailableMessage()` instead — including
    // for fresh sessions whose resolution chain lands on the unknown
    // placeholder (pi `if (!model)`). Never a stderr diagnostic: the
    // interactive frontend surfaces it as a boot warning and print mode
    // drops it.
    std::optional<std::string> model_fallback_message;
    const bool landed_on_placeholder =
        request_model.provider == agent::detail::kDefaultModel.provider &&
        request_model.id == agent::detail::kDefaultModel.id;
    if (resume_restore_failed) {
        if (landed_on_placeholder) {
            model_fallback_message = coding_agent::format_no_models_available_message(
                std::filesystem::path{coding_agent::kDefaultAuthGuidanceDocsPath});
        } else {
            model_fallback_message = std::format(
                "Could not restore model {}/{}. Using {}/{}",
                *stored_provider, *stored_model,
                request_model.provider, request_model.id);
        }
    } else if (landed_on_placeholder) {
        model_fallback_message = coding_agent::format_no_models_available_message(
            std::filesystem::path{coding_agent::kDefaultAuthGuidanceDocsPath});
    }

    // Resume: explicit provider/model overrides are allowed but warned.
    if (is_resume) {
        std::optional<std::string> override_provider;
        std::optional<std::string> override_model;
        if (plan.cli_selection.model || plan.cli_selection.provider) {
            override_model = plan.cli_selection.model;
            override_provider = plan.cli_selection.provider;
        } else if (plan.requested_model) {
            override_provider = plan.requested_model->provider;
            override_model = plan.requested_model->id;
        }
        const bool provider_overridden = override_provider.has_value() &&
                                         stored_provider.has_value() &&
                                         *override_provider != *stored_provider;
        const bool model_overridden = override_model.has_value() &&
                                      stored_model.has_value() &&
                                      *override_model != *stored_model;
        if (provider_overridden || model_overridden) {
            diagnostics.push_back(make_diag(
                SessionDiagnostic::Severity::Warning,
                "resume_provider_override",
                std::format(
                    "Resumed session provider/model metadata overridden by explicit request; "
                    "was ({}/{}) now ({}/{})",
                    stored_provider.value_or(""),
                    stored_model.value_or(""),
                    resolved_provider,
                    resolved_model)));
        }
    }

    // 6. The runtime is the session's canonical ModelRuntime, held for
    // `model_runtime()` and injected as the Agent's sole stream seam (#326).

    // 7. Resolve execution environment. Secret environment names come from
    // the runtime's configured models.json apiKey templates. The session
    // always owns its local execution environment; the former SDK
    // host-provided environment injection is gone.
    std::vector<std::string> secret_environment_names = runtime->configured_api_key_env_names();
    auto exec_env = std::make_shared<harness::AsyncLocalExecutionEnv>(
        plan.execution_runtime_target,
        workspace,
        /* bash_available */ true,
        std::move(secret_environment_names),
        harness::ShellConfig{
            .shell_path = settings.shell_path,
            .command_prefix = settings.shell_command_prefix,
        });
    auto cleanup_on_failure = [&]() { cleanup_factory_env(exec_env.get()); };

    // 8. Build the tool registry: the fixed #331 built-in set (read, write,
    // edit, bash — always available) plus the private test-seam custom tools.
    agent::ToolRegistry tools;
    std::set<std::string> builtin_names;

    builtin_names.insert("read");
    if (auto added = tools.add(tools::make_async_read_file_tool(exec_env)); !added) {
        cleanup_on_failure();
        return std::unexpected(added.error());
    }
    builtin_names.insert("write");
    if (auto added = tools.add(tools::make_async_write_file_tool(exec_env)); !added) {
        cleanup_on_failure();
        return std::unexpected(added.error());
    }
    builtin_names.insert("edit");
    if (auto added = tools.add(tools::make_async_edit_tool(exec_env)); !added) {
        cleanup_on_failure();
        return std::unexpected(added.error());
    }
    builtin_names.insert("bash");
    // Live PI_* session facts for the model Bash Tool (pi
    // `resolveSpawnContext`): the runtime refreshes this holder as the model
    // and thinking level change. The private test seam's holder wins;
    // production assembly creates one.
    auto bash_session_environment = plan.bash_session_environment
        ? plan.bash_session_environment
        : std::make_shared<tools::BashSessionEnvironment>();
    if (auto added = tools.add(tools::make_async_bash_tool(
            exec_env, bash_session_environment));
        !added) {
        cleanup_on_failure();
        return std::unexpected(added.error());
    }

    if (auto dup = find_duplicate_tool_name(plan.custom_tools)) {
        cleanup_on_failure();
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            std::format("duplicate custom tool name: '{}'", *dup),
            "each custom tool must have a unique name"));
    }
    if (auto collision = find_builtin_custom_collision(builtin_names, plan.custom_tools)) {
        cleanup_on_failure();
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            std::format("custom tool name '{}' collides with built-in tool", *collision),
            "rename the custom tool or disable the conflicting built-in tool"));
    }

    for (auto& tool : plan.custom_tools) {
        if (tool.execute) {
            if (auto added = tools.add(std::move(tool)); !added) {
                cleanup_on_failure();
                return std::unexpected(added.error());
            }
        }
    }

    // pi sdk.ts `createAgentSession` thinking restore: the effective
    // pre-clamp level is the resumed `thinking_level_change` entry (when the
    // session has one), else the settings `defaultThinkingLevel`, else pi's
    // DEFAULT_THINKING_LEVEL ("medium"). An in-memory branch seed restores
    // like a resume: the branch path's level wins over the settings default,
    // so the appended entry and the Agent's live state never diverge. The
    // Agent clamps the request at construction (ADR 0034 / #352), so the
    // persisted initial entries below carry the same clamped value.
    const std::string effective_thinking_level = ai::clamp_thinking_level_string(
        request_model,
        is_resume && prepared_resume.resume.has_thinking_level_entry
            ? prepared_resume.resume.thinking_level
            : plan.in_memory_branch_seed &&
                    plan.in_memory_branch_seed->context.has_thinking_level_entry
                ? plan.in_memory_branch_seed->context.thinking_level
                : settings.default_thinking_level.value_or("medium"));

    // 9. Publish the session only after all fallible prerequisites succeeded.
    OpenSession open;
    if (is_resume) {
        auto published = publish_resume_session(prepared_resume);
        if (!published) {
            cleanup_on_failure();
            return std::unexpected(published.error());
        }
        open = std::move(*published);
        // pi sdk.ts: a resumed session without a `thinking_level_change`
        // entry gets the restored level appended so a later resume restores
        // it; a session that already carries the entry stays untouched.
        if (!prepared_resume.resume.has_thinking_level_entry) {
            if (auto appended = open.store->append_thinking_level_change(
                    std::nullopt, effective_thinking_level);
                !appended) {
                cleanup_on_failure();
                return std::unexpected(appended.error());
            }
        }
    } else {
        auto published = publish_session(
            std::move(*new_publication),
            resolved_provider,
            resolved_model);
        if (!published) {
            cleanup_on_failure();
            return std::unexpected(published.error());
        }
        open = std::move(*published);
        // Private in-session seam: pi in-memory `createBranchedSession` — the
        // branch's message projection and derived state seed the new
        // in-memory session. The seed messages are also committed to the
        // store so its live tree mirrors the live context exactly (pi's
        // non-persisting SessionManager keeps the branch's entries). The
        // header parent pointer comes from the seed; the model/thinking
        // restore rides the branch context like a resumed session.
        if (plan.in_memory_branch_seed) {
            const auto& seed = *plan.in_memory_branch_seed;
            open.metadata.parent_session = seed.parent_session;
            open.history = seed.context.messages;
            open.context_model = seed.context.model;
            open.context_thinking_level =
                seed.context.thinking_level == "off" &&
                    !seed.context.has_thinking_level_entry
                ? std::nullopt
                : std::optional<std::string>{seed.context.thinking_level};
            open.topology = harness::session::SessionTopology::Branched;
            for (const auto& message : open.history) {
                if (auto appended = open.store->append(message); !appended) {
                    cleanup_on_failure();
                    return std::unexpected(appended.error());
                }
            }
        }
        // Persist the session's `model_change {provider, modelId}` as the first
        // content entry (pi `setModel` → `appendModelChange`) — skipped for the
        // unknown placeholder, exactly like pi's `if (model)` guard, so a
        // zero-model session never records a spurious unknown/unknown identity
        // that a later resume would warn about. Resume re-resolves this
        // identity against the live runtime catalog; no baseUrl, key-source,
        // or authentication material ever enters the session file (ADR 0031).
        const bool placeholder_model =
            resolved_provider == agent::detail::kDefaultModel.provider &&
            resolved_model == agent::detail::kDefaultModel.id;
        if (!placeholder_model) {
            if (auto appended = open.store->append_model_change(
                    std::nullopt, resolved_provider, resolved_model);
                !appended) {
                cleanup_on_failure();
                return std::unexpected(appended.error());
            }
        }
        // pi sdk.ts: new sessions persist the initial thinking level as
        // the second entry so a later resume restores it. In-memory sessions
        // keep both entries in the store's live tree only (nothing durable
        // to resume from).
        if (auto appended = open.store->append_thinking_level_change(
                std::nullopt, effective_thinking_level);
            !appended) {
            cleanup_on_failure();
            return std::unexpected(appended.error());
        }
    }

    // pi main.ts `--name`: appendSessionInfo after session selection. The
    // stored name is sanitized exactly like pi (CR/LF runs become one space,
    // then trimmed); in-memory sessions have no session_info surface.
    if (plan.session_name) {
        auto sanitized = sanitize_session_name(*plan.session_name);
        if (auto appended = open.store->append_session_info(
                std::nullopt, std::move(sanitized));
            !appended) {
            cleanup_on_failure();
            return std::unexpected(appended.error());
        }
    }

    // 10. Assemble the runtime. The Native TUI's Session-owned User Shell is
    // an independent capability instance: it shares only the effective
    // user-level Shell configuration with an enabled model Bash Tool and
    // never widens the shared Execution Environment (ADR 0026).
    // Capture the merged settings values first: the manager moves into the
    // runtime below so it can persist later defaults with the same
    // project-trust state (pi `AgentSession.settingsManager`).
    AgentSessionRuntimeConfig runtime_config;
    runtime_config.max_queued_messages = plan.max_queued_messages;
    runtime_config.max_queued_bytes = plan.max_queued_bytes;
    runtime_config.model = std::move(request_model);
    runtime_config.scoped_models = std::move(scoped_models);
    // pi main.ts `buildSessionOptions`: a scoped entry's explicit `:level`
    // seeds the new-session thinking level (CLI `--thinking` precedence
    // lands with the flag's session plumbing).
    runtime_config.default_thinking_level =
        scoped_thinking_level ? scoped_thinking_level : settings.default_thinking_level;
    // pi `_rebuildSystemPrompt` inputs resolved by the resource loader (P20):
    // the custom prompt, the append strings, and the Project Context Files.
    runtime_config.custom_prompt = std::move(system_prompt_text);
    runtime_config.append_system_prompt = std::move(append_system_prompt_texts);
    runtime_config.context_files = std::move(agents_files);
    // Loaded-resources presentation + `/reload` (pi `resourceLoader`
    // getters): the SYSTEM/APPEND sources, the per-kind diagnostics, and the
    // retained loading request (#418).
    runtime_config.system_prompt_source = std::move(system_prompt_source);
    runtime_config.append_system_prompt_sources =
        std::move(append_system_prompt_sources);
    runtime_config.skill_diagnostics = std::move(skill_diagnostics);
    runtime_config.prompt_diagnostics = std::move(prompt_diagnostics);
    runtime_config.theme_diagnostics = std::move(theme_diagnostics);
    runtime_config.resource_loading_request =
        std::move(retained_resource_request);
    const auto shell_path = settings.shell_path;
    const auto shell_command_prefix = settings.shell_command_prefix;

    RuntimeServices services;
    services.model_runtime = std::move(runtime);
    services.model_runtime_owned = plan.model_runtime_owned;
    services.settings_manager = std::move(snapshot.manager);
    services.env = std::move(exec_env);
    services.env_owned = true;
    services.runtime_target = plan.execution_runtime_target;
    services.user_shell = std::move(user_shell);
    if (!services.user_shell && plan.provide_user_shell) {
        services.user_shell = std::make_unique<LocalUserShell>(
            workspace,
            services.model_runtime->configured_api_key_env_names(),
            harness::ShellConfig{
                .shell_path = shell_path,
                .command_prefix = shell_command_prefix,
            });
    }
    services.bash_session_environment = std::move(bash_session_environment);
    services.tools = std::move(tools);

    const auto session_path = open.store->path();
    const auto metadata = open.metadata;
    auto runtime_handle = std::make_unique<AgentSessionRuntime>(
        std::move(services),
        std::move(open),
        std::move(skills),
        std::move(templates),
        std::move(runtime_config));

    coding_agent::ResolvedSessionIdentity identity{
        .provider = resolved_provider,
        .model = resolved_model,
        .session_id = metadata.session_id,
        .session_path = session_path,
        .workspace = workspace,
        .metadata = metadata,
    };
    return SessionFactory::publish(
        std::move(runtime_handle),
        std::move(session_path),
        std::move(diagnostics),
        std::move(model_fallback_message),
        std::move(theme_documents),
        std::move(identity));
}

/// Shared creation tail: normalize produced a plan (or the attempt's first
/// error), assembly runs against the same snapshot, and any failure carries
/// the settings load warnings through the error context field.
[[nodiscard]] support::Expected<coding_agent::CreateAgentSessionResult> finish_creation(
    support::Expected<AssemblyPlan> plan,
    SettingsSnapshot& snapshot,
    std::unique_ptr<AsyncUserShell> user_shell = {}) {
    if (!plan) {
        return std::unexpected(with_settings_fallback_context(plan.error(), snapshot));
    }
    auto result = run_assembly(
        std::move(*plan), snapshot, std::move(user_shell));
    if (!result) {
        return std::unexpected(with_settings_fallback_context(result.error(), snapshot));
    }
    return result;
}

} // namespace

coding_agent::CreateAgentSessionResult SessionFactory::publish(
    std::unique_ptr<AgentSessionRuntime> runtime,
    std::optional<std::filesystem::path> session_path,
    std::vector<coding_agent::SessionDiagnostic> diagnostics,
    std::optional<std::string> model_fallback_message,
    std::vector<coding_agent::LoadedThemeResource> theme_resources,
    coding_agent::ResolvedSessionIdentity identity) {
    return coding_agent::CreateAgentSessionResult{
        .session = coding_agent::AgentSession::bind_runtime(
            std::move(runtime), std::move(session_path)),
        .diagnostics = std::move(diagnostics),
        .model_fallback_message = std::move(model_fallback_message),
        .theme_resources = std::move(theme_resources),
        .resolved_identity = std::move(identity),
    };
}

void SessionFactory::apply_cli_facts(
    AgentSessionCreationRequest& request,
    const InteractiveSessionFacts& facts) {
    // pi `projectTrustByCwd`: the engine is the session-trust authority. Its
    // resolution already merges the CLI --approve/--no-approve override with
    // the boot prompt decision and the boot-workspace inheritance for
    // in-session replacement, so the merge fills in only a request that left
    // the decision unset — overwriting an engine-resolved decision would
    // discard a session-only trust choice (issue #507).
    if (!request.project_trust_override.has_value()) {
        request.project_trust_override = facts.project_trust_override;
    }
    // Pure CLI-owned facts (pi `createRuntime` re-applies the CLI options to
    // each replacement): host-authoritative, load-bearing for the fields
    // `InteractiveEngine::make_session_request` deliberately omits.
    request.session_facts = facts;
}

support::Expected<coding_agent::CreateAgentSessionResult> SessionFactory::create(
    AgentSessionCreationRequest request,
    std::optional<InteractiveSessionFacts> session_facts,
    AssemblyOverrides overrides) {
    // One door (map #508 D5/D7): an engine-built replacement request crosses
    // with the CLI-owned facts, re-applied before any assembly step reads the
    // request.
    if (session_facts.has_value()) {
        apply_cli_facts(request, *session_facts);
    }
    auto snapshot = load_settings_snapshot(request.workspace);
    if (overrides.models) {
        // The assembly override contributes concrete Providers only. The
        // SessionFactory owns the concrete ModelRuntime that receives them;
        // no alternate Models Runtime construction path exists.
        ModelRuntimeOptions wrap_options;
        wrap_options.models_path = std::filesystem::path{};
        auto wrapped = ModelRuntime::create(std::move(wrap_options));
        if (!wrapped) {
            return std::unexpected(
                with_settings_fallback_context(wrapped.error(), snapshot));
        }
        const auto incoming_providers = overrides.models->providers();
        for (auto provider : incoming_providers) {
            if (auto registered = (*wrapped)->register_native_provider(std::move(provider)); !registered) {
                return std::unexpected(with_settings_fallback_context(registered.error(), snapshot));
            }
        }
        auto plan = normalize_cli(std::move(request), snapshot.manager);
        if (!plan) {
            return std::unexpected(
                with_settings_fallback_context(plan.error(), snapshot));
        }
        // A host-injected runtime (the private E2E seam, already marked as
        // never-owned by normalize_cli) wins; otherwise the incoming concrete
        // Providers serve the deterministic CLI fake path, whose request Model
        // remains fabricated to preserve the deleted fake-provider flag's
        // behavior.
        if (!plan->model_runtime) {
            plan->model_runtime = std::move(*wrapped);
            plan->model_runtime_owned = true;
            plan->cli_fake = true;
        }
        return finish_creation(
            std::move(plan), snapshot, std::move(overrides.user_shell));
    }
    return finish_creation(
        normalize_cli(std::move(request), snapshot.manager),
        snapshot,
        std::move(overrides.user_shell));
}

} // namespace cch::coding_agent::runtime
