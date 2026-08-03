#include "SessionFactory.hpp"

#include <cch/ai/Models.hpp>
#include <cch/coding_agent/AgentConfigDir.hpp>
#include <cch/coding_agent/ModelRuntime.hpp>
#include <cch/coding_agent/ProjectResources.hpp>
#include <cch/coding_agent/ProjectTrust.hpp>
#include <cch/coding_agent/Settings.hpp>
#include <cch/tools/ToolFactories.hpp>
#include <cch/util/Error.hpp>
#include "coding_agent/ProjectResourceLoader.hpp"
#include "coding_agent/SessionPathPolicy.hpp"
#include "coding_agent/prompt/PromptProcessor.hpp"
#include "coding_agent/runtime/AgentSessionRuntime.hpp"
#include "coding_agent/runtime/LocalUserShell.hpp"
#include "coding_agent/runtime/RuntimeServices.hpp"
#include "coding_agent/runtime/SessionLifecycle.hpp"
#include "harness/WorkspaceFileSystem.hpp"
#include "ai/providers/BoostBeastStreamTransport.hpp"
#include "ai/providers/FakeProvider.hpp"
#include "ai/providers/OpenAIProvider.hpp"
#include "util/ExpectedMacros.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>

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

constexpr std::string_view kHostClientProvider = "sdk-host";
constexpr std::string_view kHostClientModel = "host-client";

struct AutomaticNewSessionTarget {
    std::filesystem::path workspace;
    /// Resolved CLI automatic-directory override (--session-dir, then
    /// CCH_CODING_AGENT_SESSION_DIR, then settings sessionDir). The SDK never
    /// supplies one; when absent the workspace-keyed Agent Config Directory
    /// default applies.
    std::optional<std::filesystem::path> directory_override;
};

struct NewSessionTarget {
    std::filesystem::path session_path;
    std::filesystem::path workspace;
};

struct InMemoryNewSessionTarget {
    std::filesystem::path workspace;
};

struct ResumeSessionTarget {
    std::filesystem::path resume_path;
    std::filesystem::path workspace;
    bool workspace_explicit{false};
};

using NormalizedSessionTarget = std::variant<
    AutomaticNewSessionTarget,
    NewSessionTarget,
    InMemoryNewSessionTarget,
    ResumeSessionTarget>;

enum class CreationProfile { Cli, Sdk };

struct AssemblyPlan {
    CreationProfile profile;
    NormalizedSessionTarget target;
    /// The model/auth runtime used by the session. Nullable: a runtime is
    /// default-created from the Agent Config Directory (or `agent_dir`) when
    /// absent (ADR 0029/0030).
    std::shared_ptr<ModelRuntime> model_runtime;
    /// True when the session owns the runtime (default-created, or wrapped by
    /// the factory from injected `ai::Models` in the private test seam) and
    /// must release it on close. A host-injected runtime is never disposed by
    /// the session (ADR 0029: no dispose ceremony; runtimes are reusable
    /// across sessions).
    bool model_runtime_owned{true};
    /// SDK `agentDir` override applied only to a default-created runtime.
    std::optional<std::filesystem::path> agent_dir;
    /// True when the private test seam injected an `ai::Models`; the opaque
    /// `sdk-host`/`host-client` sentinel model resolution applies so focused
    /// session tests keep a deterministic host-facing model.
    bool models_injected{false};
    /// SDK explicit initial model for the public path (pi `model` option).
    std::optional<ai::Model> requested_model;
    /// CLI `--fake` flag: a scripted fake provider is registered into the
    /// runtime. The CLI module later removes the transitional flags.
    bool cli_fake{false};
    /// CLI transitional provider overrides (later CLI module removes these).
    coding_agent::CliProviderOverrides cli_overrides;
    std::shared_ptr<harness::AsyncExecutionEnv> host_execution_env;
    coding_agent::SdkBuiltinTools builtin_tools;
    std::vector<std::unique_ptr<agent::AsyncAgentTool>> custom_tools;
    std::vector<Skill> host_skills;
    std::vector<PromptTemplate> host_prompt_templates;
    std::vector<std::string> prompt_template_paths;
    bool load_project_resources{false};
    std::optional<DefaultProjectTrust> default_project_trust;
    std::optional<ResourceEnablement> project_skills_enablement;
    bool prompt_templates_enabled{true};
    std::optional<std::filesystem::path> trust_store_path;
    std::optional<bool> project_trust_override;
    std::size_t max_queued_messages{agent::kDefaultMaxQueuedMessages};
    std::size_t max_queued_bytes{agent::kDefaultMaxQueuedBytes};
    /// Explicit turn cap carried into the runtime config; std::nullopt (the
    /// default) imposes no cap (ADR 0015).
    std::optional<int> max_turns{std::nullopt};
    /// Native TUI assembly policy: provide the Session-owned User Shell
    /// (ADR 0026). Set only by CLI normalization for the interactive Native
    /// TUI frontend; the SDK profile never sets it.
    bool provide_user_shell{false};
};

[[nodiscard]] SdkDiagnostic make_diag(SdkDiagnostic::Severity severity,
                                       std::string code,
                                       std::string message,
                                       std::optional<std::string> path = std::nullopt) {
    return SdkDiagnostic{severity, std::move(code), std::move(message), std::move(path)};
}

/// One User Settings load per creation attempt. Missing or unreadable settings
/// stay silent (the loader contract); settings that exist but cannot be used
/// fall back to safe defaults with a warning that identifies the problem.
struct UserSettingsSnapshot {
    coding_agent::UserSettings settings;
    std::optional<std::string> fallback_warning;
};

[[nodiscard]] UserSettingsSnapshot load_user_settings_snapshot() {
    auto loaded = coding_agent::SettingsLoader::load(coding_agent::settings_file_path());
    if (loaded) {
        return {std::move(*loaded), std::nullopt};
    }
    std::string warning = "could not load user settings: " + loaded.error().message;
    if (!loaded.error().detail.empty()) {
        warning += ": " + loaded.error().detail;
    }
    warning += "; using safe defaults";
    return {coding_agent::UserSettings{}, std::move(warning)};
}

/// Failed creation after a User Settings fallback keeps the primary error and
/// carries the settings warning through the error context field.
[[nodiscard]] util::Error with_settings_fallback_context(
    util::Error error,
    const UserSettingsSnapshot& snapshot) {
    if (snapshot.fallback_warning) {
        if (error.context && !error.context->empty()) {
            error.context = *error.context + "; " + *snapshot.fallback_warning;
        } else {
            error.context = *snapshot.fallback_warning;
        }
    }
    return error;
}

[[nodiscard]] SdkDiagnostic::Severity to_sdk_severity(ResourceDiagnosticSeverity severity) {
    switch (severity) {
    case ResourceDiagnosticSeverity::Info:
        return SdkDiagnostic::Severity::Info;
    case ResourceDiagnosticSeverity::Warning:
        return SdkDiagnostic::Severity::Warning;
    case ResourceDiagnosticSeverity::Error:
        return SdkDiagnostic::Severity::Error;
    }
    return SdkDiagnostic::Severity::Warning;
}

void add_project_resource_loading_diagnostics(
    std::vector<SdkDiagnostic>& diagnostics,
    const ProjectResourceLoadingResult& loading) {
    for (const auto& diag : loading.diagnostics) {
        diagnostics.push_back(make_diag(
            to_sdk_severity(diag.severity),
            project_resource_loading_diagnostic_code(diag),
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
    const std::vector<std::unique_ptr<agent::AsyncAgentTool>>& tools) {
    std::set<std::string, std::less<>> seen;
    for (const auto& t : tools) {
        if (!t) continue;
        const auto& name = t->definition().name;
        if (!seen.insert(name).second) {
            return name;
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::string> find_builtin_custom_collision(
    const std::set<std::string>& builtin_names,
    const std::vector<std::unique_ptr<agent::AsyncAgentTool>>& custom_tools) {
    for (const auto& t : custom_tools) {
        if (!t) continue;
        if (builtin_names.contains(t->definition().name)) {
            return t->definition().name;
        }
    }
    return std::nullopt;
}

[[nodiscard]] util::ExpectedVoid validate_trust_store_path(
    const std::filesystem::path& trust_path,
    const std::filesystem::path& workspace) {
    if (trust_path.is_absolute() == false) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "trust_store_path must be absolute",
            "supply an absolute filesystem path outside the workspace"));
    }

    std::error_code ec;
    auto trust_canonical = std::filesystem::weakly_canonical(trust_path, ec);
    if (ec) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "trust_store_path cannot be canonicalized",
            trust_path.string()));
    }

    auto ws_canonical = std::filesystem::weakly_canonical(workspace, ec);
    if (ec) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "workspace cannot be canonicalized",
            workspace.string()));
    }

    if (trust_canonical == ws_canonical) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "trust_store_path must be outside the workspace",
            trust_path.string()));
    }

    auto [trust_it, ws_it] = std::mismatch(
        trust_canonical.begin(), trust_canonical.end(),
        ws_canonical.begin(), ws_canonical.end());
    if (ws_it == ws_canonical.end()) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "trust_store_path cannot be inside the workspace",
            trust_path.string()));
    }

    return {};
}

void cleanup_factory_env(bool env_owned, harness::AsyncExecutionEnv* env) {
    if (env_owned && env) {
        boost::asio::io_context io;
        boost::asio::co_spawn(io, env->cleanup(), boost::asio::detached);
        io.run();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Model / runtime resolution
// ─────────────────────────────────────────────────────────────────────────────

/// CLI transitional provider resolution (the CLI module later replaces the
/// `--base-url`/`--api-key-env` flags with the ModelRuntime surface). The
/// openai-compatible adapter remains the CLI default; the `ResolvedProviderSettings`
/// vocabulary is preserved from the removed ProviderConfigResolution module.
[[nodiscard]] std::string provider_default_model(const std::string& registry_name) {
    if (registry_name == "fake") {
        return "fake-model";
    }
    return "gpt-4.1-mini";
}

[[nodiscard]] std::string provider_default_api(const std::string& provider_registry_name) {
    if (provider_registry_name == "fake") {
        return "scripted-fake";
    }
    return "openai-completions";
}

[[nodiscard]] std::vector<std::string> resolve_api_key_env_chain(
    const coding_agent::CliProviderOverrides& overrides,
    const coding_agent::UserSettings& settings) {
    if (overrides.api_key_env && !overrides.api_key_env->empty()) {
        return {*overrides.api_key_env};
    }
    if (settings.api_key_env && !settings.api_key_env->empty()) {
        return *settings.api_key_env;
    }
    return {"OPENAI_API_KEY"};
}


[[nodiscard]] coding_agent::ResolvedProviderSettings resolve_cli_provider_settings(
    std::string_view provider_registry_name,
    const coding_agent::CliProviderOverrides& overrides,
    const coding_agent::UserSettings& settings,
    const std::optional<std::string>& stored_provider,
    const std::optional<std::string>& stored_model) {
    const std::string registry_name{provider_registry_name};

    std::string resolved_provider = registry_name;
    if (registry_name == "fake") {
        resolved_provider = "fake";
    } else if (stored_provider) {
        resolved_provider = *stored_provider;
    } else if (settings.provider) {
        resolved_provider = *settings.provider;
    }

    std::string resolved_model;
    if (overrides.model) {
        resolved_model = *overrides.model;
    } else if (stored_model) {
        resolved_model = *stored_model;
    } else if (settings.model) {
        resolved_model = *settings.model;
    } else {
        resolved_model = provider_default_model(registry_name);
    }

    std::string resolved_base_url;
    if (overrides.base_url) {
        resolved_base_url = *overrides.base_url;
    } else if (settings.base_url) {
        resolved_base_url = *settings.base_url;
    } else {
        resolved_base_url = "https://api.openai.com";
    }

    const auto resolved_api_key_env_chain = resolve_api_key_env_chain(overrides, settings);

    std::string resolved_auth;
    if (overrides.auth) {
        resolved_auth = *overrides.auth;
    } else if (settings.auth) {
        resolved_auth = *settings.auth;
    }

    return coding_agent::ResolvedProviderSettings{
        .provider_registry_name = registry_name,
        .provider = std::move(resolved_provider),
        .api = provider_default_api(registry_name),
        .model = std::move(resolved_model),
        .base_url = std::move(resolved_base_url),
        .api_key_env = resolved_api_key_env_chain.empty() ? "" : resolved_api_key_env_chain.front(),
        .auth = std::move(resolved_auth),
        .api_key_env_chain = std::move(resolved_api_key_env_chain),
    };
}



/// Transitional request Model for the CLI path. Configured provider strings
/// use pi's frozen custom-model defaults instead of claiming catalog-specific
/// capabilities; the private test seam carries its own sentinel Model.
[[nodiscard]] ai::Model make_transitional_request_model(
    const coding_agent::ResolvedProviderSettings& resolved) {
    ai::Model model{
        .id = resolved.model,
        .name = resolved.model,
        .api = resolved.api,
        .provider = resolved.provider,
        .base_url = resolved.base_url,
        .reasoning = false,
        .thinking_level_map = std::nullopt,
        .input = {ai::ModelInput::Text},
        .cost = {},
        .context_window = 128000,
        .max_tokens = 16384,
        .headers = std::nullopt,
        .compat = std::nullopt,
    };
    return model;
}

/// Opaque host-facing Model for the private Models injection seam: provider is
/// the resolved provider identity (settings/stored/`sdk-host`) and the model id
/// is the `host-client` sentinel for new sessions (the caller sets it on
/// `resolved.model`), or the stored model id on resume. Metadata-less sessions
/// keep a truthful, credential-free request Model.
[[nodiscard]] ai::Model make_injected_models_request_model(
    const coding_agent::ResolvedProviderSettings& resolved) {
    ai::Model model{
        .id = resolved.model,
        .name = resolved.model,
        .api = "unknown",
        .provider = resolved.provider,
        .base_url = "",
        .reasoning = false,
        .thinking_level_map = std::nullopt,
        .input = {},
        .cost = {},
        .context_window = 0,
        .max_tokens = 0,
        .headers = std::nullopt,
        .compat = std::nullopt,
    };
    return model;
}

/// Fabricate a Model for resume metadata re-resolution when the stored model no
/// longer exists in the live runtime. Keeps session metadata consistent and
/// lets a legacy/fake provider keep streaming (the TUI auto-selection trigger
/// is a later module).
[[nodiscard]] ai::Model fabricate_metadata_model(
    std::string provider,
    std::string model_id,
    const ModelRuntime& runtime) {
    ai::Model model;
    model.id = std::move(model_id);
    model.name = model.id;
    model.provider = std::move(provider);
    model.api = "unknown";
    const auto selected = runtime.provider(model.provider);
    if (selected) {
        auto available = selected->models();
        if (!available.empty()) {
            model.api = available.front().api;
            model.base_url = available.front().base_url;
        }
    }
    return model;
}

/// Frozen runtime default selection for the public SDK path: the default-model
/// table when the default exists in the runtime, else the first model, else a
/// fabricated model pointing at the first provider, else an empty Model (the
/// Agent then uses pi's internal unknown default). Authentication is resolved
/// at request time; the TUI auto-selection trigger is a later module.
[[nodiscard]] ai::Model runtime_default_model(const ModelRuntime& runtime) {
    for (const auto* provider : {"openai-codex", "kimi-coding"}) {
        if (auto default_id = ModelRuntime::default_model_for_provider(provider)) {
            if (auto model = runtime.model(provider, *default_id); model) {
                return *model;
            }
        }
    }
    auto all = runtime.models();
    if (!all.empty()) {
        return all.front();
    }
    auto providers = runtime.providers();
    if (!providers.empty()) {
        return fabricate_metadata_model(
            std::string{providers.front()->id()}, std::string{kHostClientModel}, runtime);
    }
    return ai::Model{};
}

/// Resolve the SDK public path's initial model: explicit request model, then
/// resume re-resolution against the live runtime, then the runtime default.
[[nodiscard]] ai::Model resolve_sdk_public_model(
    const AssemblyPlan& plan,
    const ModelRuntime& runtime,
    bool is_resume,
    const std::optional<std::string>& stored_provider,
    const std::optional<std::string>& stored_model) {
    if (plan.requested_model) {
        return *plan.requested_model;
    }
    if (is_resume && stored_provider && stored_model) {
        if (auto model = runtime.model(*stored_provider, *stored_model); model) {
            return *model;
        }
        return fabricate_metadata_model(*stored_provider, *stored_model, runtime);
    }
    return runtime_default_model(runtime);
}

/// Build the session's ModelRuntime: the injected/adopted runtime wins; a
/// default-created runtime derives its Agent Config Directory from `agent_dir`.
[[nodiscard]] util::Expected<std::shared_ptr<ModelRuntime>> build_runtime(
    std::shared_ptr<ModelRuntime> injected,
    const std::optional<std::filesystem::path>& agent_dir) {
    if (injected) {
        return injected;
    }
    ModelRuntimeOptions options;
    options.agent_dir = agent_dir.value_or(std::filesystem::path{});
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
[[nodiscard]] util::Expected<std::filesystem::path> resolve_canonical_workspace(
    const std::filesystem::path& workspace) {
    if (workspace.empty()) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "workspace is required for new sessions",
            "supply a non-empty workspace path"));
    }

    std::error_code ec;
    auto resolved = std::filesystem::canonical(workspace, ec);
    if (ec) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "workspace cannot be resolved",
            workspace.string() + ": " + ec.message()));
    }
    if (!std::filesystem::is_directory(resolved, ec) || ec) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "workspace is not a directory",
            workspace.string() + (ec ? ": " + ec.message() : std::string{})));
    }
    return resolved;
}

/// CLI automatic-directory override precedence (pi: --session-dir, then
/// PI_CODING_AGENT_SESSION_DIR, then settings sessionDir, then the
/// workspace-keyed default). The first non-empty value wins and resolves
/// against the final canonical workspace. SDK normalization never consults
/// these inputs.
[[nodiscard]] util::Expected<std::optional<std::filesystem::path>> resolve_cli_session_dir_override(
    const std::optional<std::string>& flag_value,
    const std::optional<std::string>& settings_value,
    const std::filesystem::path& canonical_workspace) {
    std::optional<std::string> value;
    if (flag_value && !flag_value->empty()) {
        value = flag_value;
    }
    if (!value) {
        if (const char* env_value = std::getenv("CCH_CODING_AGENT_SESSION_DIR");
            env_value != nullptr && env_value[0] != '\0') {
            value = std::string{env_value};
        }
    }
    if (!value && settings_value && !settings_value->empty()) {
        value = settings_value;
    }
    if (!value) {
        return std::optional<std::filesystem::path>{};
    }
    auto resolved = session_paths::resolve_session_dir_value(
        *value, canonical_workspace, coding_agent::home_directory());
    if (!resolved) {
        return std::unexpected(resolved.error());
    }
    return std::optional<std::filesystem::path>{std::move(*resolved)};
}

struct SessionTargetNormalizationOptions {
    CreationProfile profile;
    std::filesystem::path workspace;
    bool workspace_explicit{false};
    std::optional<std::string> cli_session_dir{std::nullopt};
    std::optional<std::string> settings_session_dir{std::nullopt};
};

[[nodiscard]] util::Expected<NormalizedSessionTarget> normalize_session_target(
    coding_agent::SessionTarget target,
    SessionTargetNormalizationOptions options) {
    const bool sdk_resume_without_workspace =
        options.profile == CreationProfile::Sdk &&
        std::holds_alternative<ExplicitResumeSessionTarget>(target) &&
        options.workspace.empty();
    if (!sdk_resume_without_workspace) {
        auto workspace = resolve_canonical_workspace(options.workspace);
        if (!workspace) {
            return std::unexpected(workspace.error());
        }
        options.workspace = std::move(*workspace);
    }

    if (std::holds_alternative<DefaultPersistedSessionTarget>(target)) {
        std::optional<std::filesystem::path> directory_override;
        if (options.profile == CreationProfile::Cli) {
            auto resolved = resolve_cli_session_dir_override(
                options.cli_session_dir,
                options.settings_session_dir,
                options.workspace);
            if (!resolved) {
                return std::unexpected(resolved.error());
            }
            directory_override = std::move(*resolved);
        }
        return NormalizedSessionTarget{AutomaticNewSessionTarget{
            .workspace = std::move(options.workspace),
            .directory_override = std::move(directory_override),
        }};
    }
    if (auto* explicit_new = std::get_if<ExplicitNewSessionTarget>(&target)) {
        return NormalizedSessionTarget{NewSessionTarget{
            .session_path = std::move(explicit_new->path),
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
    if (std::holds_alternative<InMemorySessionTarget>(target)) {
        return NormalizedSessionTarget{InMemoryNewSessionTarget{
            .workspace = std::move(options.workspace),
        }};
    }
    return std::unexpected(util::make_error(
        util::ErrorCode::Validation,
        options.profile == CreationProfile::Cli
            ? "unsupported CLI session target"
            : "unsupported SDK session target"));
}

[[nodiscard]] util::Expected<AssemblyPlan> normalize_cli(
    AgentSessionCreationRequest request,
    const coding_agent::UserSettings& settings) {
    AssemblyPlan plan;
    plan.profile = CreationProfile::Cli;
    auto target = normalize_session_target(
        std::move(request.session_target),
        SessionTargetNormalizationOptions{
            .profile = CreationProfile::Cli,
            .workspace = request.workspace,
            .workspace_explicit = request.workspace_explicit,
            .cli_session_dir = request.session_dir,
            .settings_session_dir = settings.session_dir,
        });
    if (!target) {
        return std::unexpected(target.error());
    }
    plan.target = std::move(*target);

    plan.model_runtime = nullptr;
    plan.cli_fake = request.fake;
    plan.cli_overrides = request.provider_overrides;
    plan.host_execution_env = nullptr;
    plan.builtin_tools = coding_agent::SdkBuiltinTools{
        .read = true,
        .write = true,
        .edit_file = true,
        .bash = request.enable_bash,
    };

    plan.load_project_resources = true;
    plan.project_trust_override = request.project_trust_override;
    plan.default_project_trust = settings.default_project_trust.value_or(DefaultProjectTrust::Ask);
    plan.project_skills_enablement = settings.project_skills;
    if (request.disable_project_skills) {
        plan.project_skills_enablement = ResourceEnablement::Off;
    }
    plan.prompt_templates_enabled = !request.disable_prompt_templates;
    plan.prompt_template_paths = request.prompt_template_paths;
    plan.max_queued_messages = request.max_queued_messages;
    plan.max_queued_bytes = request.max_queued_bytes;
    plan.max_turns = request.max_turns;
    plan.provide_user_shell = request.provide_user_shell;

    return plan;
}

[[nodiscard]] util::Expected<AssemblyPlan> normalize_sdk(
    CreateAgentSessionOptions options,
    std::shared_ptr<ModelRuntime> injected_runtime = {},
    bool models_injected = false) {
    AssemblyPlan plan;
    plan.profile = CreationProfile::Sdk;
    const bool workspace_explicit = !options.workspace.empty();
    auto target = normalize_session_target(
        std::move(options.session_target),
        SessionTargetNormalizationOptions{
            .profile = CreationProfile::Sdk,
            .workspace = options.workspace,
            .workspace_explicit = workspace_explicit,
        });
    if (!target) {
        return std::unexpected(target.error());
    }
    plan.target = std::move(*target);

    plan.model_runtime = std::move(injected_runtime);
    if (options.model_runtime) {
        // A host-injected runtime wins and is never disposed by the session.
        plan.model_runtime = std::move(options.model_runtime);
        plan.model_runtime_owned = false;
    }
    plan.models_injected = models_injected;
    plan.requested_model = std::move(options.model);
    plan.agent_dir = std::move(options.agent_dir);
    plan.host_execution_env = std::move(options.execution_env);
    plan.builtin_tools = options.builtin_tools;
    plan.custom_tools = std::move(options.custom_tools);
    plan.host_skills = std::move(options.skills);
    plan.host_prompt_templates = std::move(options.prompt_templates);
    plan.load_project_resources = options.load_project_resources;
    plan.default_project_trust = options.default_project_trust;
    plan.project_skills_enablement = options.project_skills_enablement;
    plan.max_queued_messages = options.max_queued_messages;
    plan.max_queued_bytes = options.max_queued_bytes;
    plan.max_turns = options.max_turns;
    plan.trust_store_path = options.trust_store_path;

    return plan;
}

// ─────────────────────────────────────────────────────────────────────────────
// Assembly
// ─────────────────────────────────────────────────────────────────────────────

/// Register the CLI's transitional provider into the runtime: the scripted fake
/// provider for `--fake`, otherwise the openai-compatible provider composed
/// from the resolved request Model and env chain (the CLI module later removes
/// this surface).
[[nodiscard]] util::ExpectedVoid register_cli_provider(
    ModelRuntime& runtime,
    const coding_agent::ResolvedProviderSettings& resolved,
    const ai::Model& request_model) {
    if (resolved.provider_registry_name == "fake") {
        return runtime.register_native_provider(ai::providers::make_scripted_fake_provider());
    }
    auto provider = ai::providers::make_openai_compatible_provider(
        request_model.provider,
        std::vector<ai::Model>{request_model},
        resolved.api_key_env_chain,
        std::make_shared<ai::providers::BoostBeastStreamTransport>());
    return runtime.register_native_provider(std::move(provider));
}

[[nodiscard]] util::Expected<CreateAgentSessionResult> run_assembly(
    AssemblyPlan plan,
    const UserSettingsSnapshot& snapshot,
    std::unique_ptr<AsyncUserShell> user_shell) {
    std::vector<SdkDiagnostic> diagnostics;

    // 1. Resolve workspace and validate target shape.
    std::filesystem::path workspace;
    PreparedResumeTarget prepared_resume;
    const bool is_resume = std::holds_alternative<ResumeSessionTarget>(plan.target);

    if (is_resume) {
        const auto& target = std::get<ResumeSessionTarget>(plan.target);
        auto prepared = prepare_resume_target(
            target.resume_path, target.workspace, target.workspace_explicit);
        if (!prepared) {
            return std::unexpected(prepared.error());
        }
        if (plan.profile == CreationProfile::Sdk &&
            prepared->resume.topology != harness::session::SessionTopology::Linear) {
            return std::unexpected(util::make_error(
                util::ErrorCode::Session,
                "unsupported_session_topology",
                "SDK v1 supports only linear sessions; the resumed session contains branches, "
                "compactions, or tree metadata that cannot be appended linearly"));
        }
        workspace = prepared->workspace;
        prepared_resume = std::move(*prepared);
    } else if (const auto* target = std::get_if<AutomaticNewSessionTarget>(&plan.target)) {
        workspace = target->workspace;
    } else if (const auto* target = std::get_if<InMemoryNewSessionTarget>(&plan.target)) {
        workspace = target->workspace;
    } else {
        const auto& new_target = std::get<NewSessionTarget>(plan.target);
        if (new_target.workspace.empty()) {
            return std::unexpected(util::make_error(
                util::ErrorCode::Validation,
                "workspace is required for new sessions",
                "supply a non-empty workspace path"));
        }
        workspace = new_target.workspace;
    }

    // 2. The creation attempt's User Settings snapshot supplies defaults and
    // API key chains. A fallback stays observable as a warning diagnostic on
    // the success path and as error context on the failure path.
    if (snapshot.fallback_warning) {
        diagnostics.push_back(make_diag(
            SdkDiagnostic::Severity::Warning,
            "settings:fallback",
            *snapshot.fallback_warning,
            coding_agent::settings_file_path().string()));
    }
    const auto& settings = snapshot.settings;

    // 3. Resolve the model/auth runtime. An injected runtime wins; otherwise a
    // runtime is default-created from the Agent Config Directory (or agentDir).
    std::shared_ptr<ModelRuntime> runtime;
    if (plan.model_runtime) {
        runtime = std::move(plan.model_runtime);
    } else {
        auto built = build_runtime(nullptr, plan.agent_dir);
        if (!built) {
            return std::unexpected(built.error());
        }
        runtime = std::move(*built);
    }

    // 4. Resolve provider/model metadata and the request Model.
    std::optional<std::string> stored_provider;
    std::optional<std::string> stored_model;
    if (is_resume) {
        stored_provider = prepared_resume.resume.metadata.provider;
        stored_model = prepared_resume.resume.metadata.model;
    }

    std::string provider_registry_name;
    if (plan.cli_fake) {
        provider_registry_name = "fake";
    } else if (plan.models_injected) {
        provider_registry_name = std::string{kHostClientProvider};
    } else if (is_resume && stored_provider) {
        provider_registry_name = (*stored_provider == "fake") ? "fake" : "openai-compatible";
    } else {
        provider_registry_name = "openai-compatible";
    }

    std::string resolved_provider;
    std::string resolved_model;
    ai::Model request_model;
    coding_agent::ResolvedProviderSettings resolved;
    if (plan.profile == CreationProfile::Cli) {
        resolved = resolve_cli_provider_settings(
            provider_registry_name, plan.cli_overrides, settings, stored_provider, stored_model);
        request_model = make_transitional_request_model(resolved);
        if (auto registered = register_cli_provider(*runtime, resolved, request_model); !registered) {
            return std::unexpected(registered.error());
        }
        resolved_provider = resolved.provider;
        resolved_model = resolved.model;
    } else if (plan.models_injected) {
        // The private test seam keeps the opaque sentinel model unless an
        // explicit request model is supplied: the resolved provider identity
        // (settings/stored/`sdk-host`) plus the `host-client` sentinel.
        if (plan.requested_model) {
            request_model = *plan.requested_model;
        } else {
            resolved = resolve_cli_provider_settings(
                provider_registry_name,
                coding_agent::CliProviderOverrides{},
                settings,
                stored_provider,
                stored_model);
            if (!is_resume) {
                resolved.model = std::string{kHostClientModel};
            }
            request_model = make_injected_models_request_model(resolved);
        }
        resolved_provider = request_model.provider;
        resolved_model = request_model.id;
    } else {
        // Public SDK path: the initial model comes from the explicit request
        // model, resume re-resolution, or the runtime default.
        request_model = resolve_sdk_public_model(
            plan, *runtime, is_resume, stored_provider, stored_model);
        resolved_provider = request_model.provider;
        resolved_model = request_model.id;
    }

    // Resume: explicit provider/model overrides are allowed but warned.
    if (is_resume) {
        std::optional<std::string> override_provider;
        std::optional<std::string> override_model;
        if (plan.profile == CreationProfile::Cli) {
            override_model = plan.cli_overrides.model;
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
                SdkDiagnostic::Severity::Warning,
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

    // 5. Build the shared stream capability from the runtime. The runtime is
    // the session's canonical ModelRuntime and is held for `model_runtime()`.
    std::shared_ptr<ai::StreamingChatClient> stream = runtime;

    // 6. Resolve execution environment and ownership. Secret environment names
    // come from the runtime's configured models.json apiKey templates plus the
    // settings api_key_env chain (CLI transitional surface).
    std::shared_ptr<harness::AsyncExecutionEnv> exec_env;
    bool env_owned = true;
    std::vector<std::string> secret_environment_names = runtime->configured_api_key_env_names();
    if (settings.api_key_env) {
        for (const auto& name : *settings.api_key_env) {
            if (std::find(secret_environment_names.begin(), secret_environment_names.end(), name) ==
                secret_environment_names.end()) {
                secret_environment_names.push_back(name);
            }
        }
    }
    if (plan.host_execution_env) {
        exec_env = plan.host_execution_env;
        env_owned = false;
    } else {
        exec_env = std::make_shared<harness::AsyncLocalExecutionEnv>(
            workspace,
            plan.builtin_tools.bash,
            std::move(secret_environment_names),
            harness::ShellConfig{
                .shell_path = settings.shell_path,
                .command_prefix = settings.shell_command_prefix,
            });
    }

    auto cleanup_on_failure = [&]() { cleanup_factory_env(env_owned, exec_env.get()); };

    // 7. Build the tool registry from enabled built-ins plus validated custom tools.
    agent::AsyncToolRegistry tools;
    std::set<std::string> builtin_names;

    if (plan.builtin_tools.read) {
        builtin_names.insert("read");
        if (auto added = tools.add(tools::make_async_read_file_tool(exec_env)); !added) {
            cleanup_on_failure();
            return std::unexpected(added.error());
        }
    }
    if (plan.builtin_tools.write) {
        builtin_names.insert("write");
        if (auto added = tools.add(tools::make_async_write_file_tool(exec_env)); !added) {
            cleanup_on_failure();
            return std::unexpected(added.error());
        }
    }
    if (plan.builtin_tools.edit_file) {
        builtin_names.insert("edit_file");
        if (auto added = tools.add(tools::make_async_edit_file_tool(exec_env)); !added) {
            cleanup_on_failure();
            return std::unexpected(added.error());
        }
    }
    if (plan.builtin_tools.bash) {
        builtin_names.insert("bash");
        if (auto added = tools.add(tools::make_async_bash_tool(exec_env)); !added) {
            cleanup_on_failure();
            return std::unexpected(added.error());
        }
    }

    if (auto dup = find_duplicate_tool_name(plan.custom_tools)) {
        cleanup_on_failure();
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            std::format("duplicate custom tool name: '{}'", *dup),
            "each custom tool must have a unique name"));
    }
    if (auto collision = find_builtin_custom_collision(builtin_names, plan.custom_tools)) {
        cleanup_on_failure();
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            std::format("custom tool name '{}' collides with built-in tool", *collision),
            "rename the custom tool or disable the conflicting built-in tool"));
    }

    for (auto& tool : plan.custom_tools) {
        if (tool) {
            if (auto added = tools.add(std::move(tool)); !added) {
                cleanup_on_failure();
                return std::unexpected(added.error());
            }
        }
    }

    // 8. Load project resources if requested.
    std::vector<Skill> skills = std::move(plan.host_skills);
    std::vector<PromptTemplate> templates = std::move(plan.host_prompt_templates);

    std::filesystem::path trust_store_path = plan.trust_store_path.value_or(coding_agent::trust_store_file_path());
    if (plan.profile == CreationProfile::Sdk && plan.trust_store_path) {
        if (auto valid = validate_trust_store_path(*plan.trust_store_path, workspace); !valid) {
            cleanup_on_failure();
            return std::unexpected(valid.error());
        }
    } else if (plan.load_project_resources) {
        // The default user trust store must not become project-controlled when
        // HOME resolves to the workspace. An unavailable store fails closed in
        // ProjectResourceLoader while an explicit same-run trust override can
        // still authorize resources without consulting workspace-local state.
        if (auto valid = validate_trust_store_path(trust_store_path, workspace); !valid) {
            trust_store_path.clear();
        }
    }

    if (plan.load_project_resources) {
        auto fs = harness::WorkspaceFileSystem::create(workspace);
        if (fs) {
            ProjectResourcePolicy resource_policy;
            resource_policy.project_skills = plan.project_skills_enablement.value_or(ResourceEnablement::Auto);

            ProjectResourceLoadingRequest resource_request;
            resource_request.workspace = workspace;
            resource_request.policy = resource_policy;
            resource_request.default_project_trust = plan.default_project_trust.value_or(DefaultProjectTrust::Ask);
            resource_request.project_trust_override = plan.project_trust_override;
            resource_request.prompt_templates_enabled = plan.prompt_templates_enabled;
            resource_request.host_skills = std::move(skills);
            resource_request.host_prompt_templates = std::move(templates);
            if (plan.prompt_templates_enabled) {
                resource_request.explicit_prompt_templates = make_explicit_template_inputs(*fs, plan.prompt_template_paths);
            }

            ProjectTrustStore trust_store{trust_store_path};
            auto resource_loading = load_project_resources(*fs, trust_store, std::move(resource_request));
            if (!resource_loading.fatal_errors.empty()) {
                cleanup_on_failure();
                return std::unexpected(util::make_error(
                    util::ErrorCode::Validation,
                    "explicit resource failed to load",
                    resource_loading.fatal_errors.front().message));
            }
            add_project_resource_loading_diagnostics(diagnostics, resource_loading);
            skills = std::move(resource_loading.resources.skills);
            templates = std::move(resource_loading.resources.prompt_templates);
        } else {
            diagnostics.push_back(make_diag(
                SdkDiagnostic::Severity::Warning,
                "resource:workspace_fs_unavailable",
                fs.error().message,
                workspace.string()));
        }
    }

    // 9. Publish the session only after all fallible prerequisites succeeded.
    OpenSession open;
    if (is_resume) {
        auto published = publish_resume_session(prepared_resume);
        if (!published) {
            cleanup_on_failure();
            return std::unexpected(published.error());
        }
        open = std::move(*published);
    } else {
        NewSessionPublication publication;
        if (const auto* automatic = std::get_if<AutomaticNewSessionTarget>(&plan.target)) {
            publication = AutomaticPublication{
                .workspace = automatic->workspace,
                .directory_override = automatic->directory_override,
            };
        } else if (const auto* in_memory = std::get_if<InMemoryNewSessionTarget>(&plan.target)) {
            publication = InMemoryPublication{
                .workspace = in_memory->workspace,
            };
        } else {
            const auto& explicit_new = std::get<NewSessionTarget>(plan.target);
            publication = ExplicitNewPublication{
                .session_path = explicit_new.session_path,
                .workspace = explicit_new.workspace,
            };
        }
        auto published = publish_session(
            std::move(publication),
            resolved_provider,
            resolved_model);
        if (!published) {
            cleanup_on_failure();
            return std::unexpected(published.error());
        }
        open = std::move(*published);
    }

    // 10. Assemble the runtime. The Native TUI's Session-owned User Shell is
    // an independent capability instance: it shares only the effective
    // user-level Shell configuration with an enabled model Bash Tool and
    // never widens the shared Execution Environment (ADR 0026).
    RuntimeServices services;
    services.stream = std::move(stream);
    services.model_runtime = std::move(runtime);
    services.model_runtime_owned = plan.model_runtime_owned;
    services.env = std::move(exec_env);
    services.env_owned = env_owned;
    services.user_shell = std::move(user_shell);
    if (!services.user_shell && plan.provide_user_shell) {
        services.user_shell = std::make_unique<LocalUserShell>(
            workspace,
            settings.api_key_env.value_or(std::vector<std::string>{}),
            harness::ShellConfig{
                .shell_path = settings.shell_path,
                .command_prefix = settings.shell_command_prefix,
            });
    }
    services.tools = std::move(tools);

    prompt::PromptProcessor prompt_processor{std::move(skills), std::move(templates)};

    AgentSessionRuntimeConfig runtime_config;
    runtime_config.max_queued_messages = plan.max_queued_messages;
    runtime_config.max_queued_bytes = plan.max_queued_bytes;
    runtime_config.max_turns = plan.max_turns;
    runtime_config.model = std::move(request_model);

    const auto session_path = open.store->path();
    const auto metadata = open.metadata;
    auto runtime_handle = std::make_unique<AgentSessionRuntime>(
        std::move(services),
        std::move(open),
        std::move(prompt_processor),
        std::move(runtime_config));

    CreateAgentSessionResult result;
    result.runtime = std::move(runtime_handle);
    result.diagnostics = std::move(diagnostics);
    result.session_id = metadata.session_id;
    result.provider = resolved_provider;
    result.model = resolved_model;
    result.session_path = session_path;
    result.workspace = workspace;
    result.metadata = metadata;
    return result;
}

/// Shared creation tail: normalize produced a plan (or the attempt's first
/// error), assembly runs against the same snapshot, and any failure carries
/// the settings fallback warning through the error context field.
[[nodiscard]] util::Expected<CreateAgentSessionResult> finish_creation(
    util::Expected<AssemblyPlan> plan,
    const UserSettingsSnapshot& snapshot,
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

util::Expected<CreateAgentSessionResult> SessionFactory::create(
    AgentSessionCreationRequest request) {
    const auto snapshot = load_user_settings_snapshot();
    return finish_creation(normalize_cli(std::move(request), snapshot.settings), snapshot);
}

util::Expected<CreateAgentSessionResult> SessionFactory::create(
    CreateAgentSessionOptions options) {
    const auto snapshot = load_user_settings_snapshot();
    return finish_creation(normalize_sdk(std::move(options)), snapshot);
}

util::Expected<CreateAgentSessionResult> SessionFactory::create(
    CreateAgentSessionOptions options,
    std::shared_ptr<ai::Models> models) {
    const auto snapshot = load_user_settings_snapshot();
    if (!models) {
        return finish_creation(normalize_sdk(std::move(options)), snapshot);
    }
    ModelRuntimeOptions wrap_options;
    if (options.agent_dir) {
        wrap_options.agent_dir = *options.agent_dir;
    }
    auto wrapped = ModelRuntime::create_from_models_for_testing(
        std::move(models), std::move(wrap_options));
    if (!wrapped) {
        return std::unexpected(with_settings_fallback_context(wrapped.error(), snapshot));
    }
    return finish_creation(
        normalize_sdk(std::move(options), std::move(*wrapped), true), snapshot);
}

util::Expected<CreateAgentSessionResult> SessionFactory::create(
    CreateAgentSessionOptions options,
    std::shared_ptr<ai::Models> models,
    std::unique_ptr<AsyncUserShell> user_shell) {
    const auto snapshot = load_user_settings_snapshot();
    if (!models) {
        return finish_creation(
            normalize_sdk(std::move(options)), snapshot, std::move(user_shell));
    }
    ModelRuntimeOptions wrap_options;
    if (options.agent_dir) {
        wrap_options.agent_dir = *options.agent_dir;
    }
    auto wrapped = ModelRuntime::create_from_models_for_testing(
        std::move(models), std::move(wrap_options));
    if (!wrapped) {
        return std::unexpected(with_settings_fallback_context(wrapped.error(), snapshot));
    }
    return finish_creation(
        normalize_sdk(std::move(options), std::move(*wrapped), true),
        snapshot,
        std::move(user_shell));
}

util::Expected<CreateAgentSessionResult> SessionFactory::create(
    CreateAgentSessionOptions options,
    std::unique_ptr<AsyncUserShell> user_shell) {
    const auto snapshot = load_user_settings_snapshot();
    return finish_creation(
        normalize_sdk(std::move(options)), snapshot, std::move(user_shell));
}

} // namespace cch::coding_agent::runtime
