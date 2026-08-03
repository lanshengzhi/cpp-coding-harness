#include "SessionFactory.hpp"

#include <cch/ai/Models.hpp>
#include <cch/coding_agent/AgentConfigDir.hpp>
#include <cch/coding_agent/AuthStorage.hpp>
#include <cch/coding_agent/ProjectResources.hpp>
#include <cch/coding_agent/ProjectTrust.hpp>
#include <cch/coding_agent/Settings.hpp>
#include <cch/tools/ToolFactories.hpp>
#include <cch/util/Error.hpp>
#include "coding_agent/ProjectResourceLoader.hpp"
#include "coding_agent/ProviderConfigResolution.hpp"
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
    coding_agent::ProviderRequest provider_request;
    std::shared_ptr<ai::Models> models;
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

class ProcessAuthContext final : public ai::AuthContext {
public:
    [[nodiscard]] boost::asio::awaitable<util::Expected<std::optional<std::string>>> environment(
        std::string name) const override {
        const char* value = std::getenv(name.c_str());
        if (value == nullptr || *value == '\0') {
            co_return std::optional<std::string>{};
        }
        co_return std::optional<std::string>{std::string{value}};
    }

    [[nodiscard]] boost::asio::awaitable<util::Expected<bool>> file_exists(
        std::string path) const override {
        if (path.starts_with("~/")) {
            if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
                path = (std::filesystem::path{home} / path.substr(2)).string();
            }
        }
        std::error_code error;
        const bool exists = std::filesystem::exists(path, error);
        if (error) {
            co_return std::unexpected(util::make_error(
                util::ErrorCode::Auth,
                "could not inspect authentication file",
                error.message()));
        }
        co_return exists;
    }
};

/// Transitional alias projection for the legacy --auth/settings auth name.
/// The value is still read live from AuthStorage for every request; no key is
/// copied into session construction state.
class AliasedCredentialStore final : public ai::CredentialStore {
public:
    AliasedCredentialStore(
        std::shared_ptr<ai::CredentialStore> storage,
        std::string alias)
        : storage_(std::move(storage)), alias_(std::move(alias)) {}

    [[nodiscard]] boost::asio::awaitable<util::Expected<std::optional<ai::Credential>>> read(
        std::string provider_id) override {
        CCH_TRY(credential, co_await storage_->read(key(std::move(provider_id))));
        co_return credential;
    }

    [[nodiscard]] boost::asio::awaitable<util::Expected<std::vector<ai::CredentialInfo>>> list() override {
        CCH_TRY(credentials, co_await storage_->list());
        co_return credentials;
    }

    [[nodiscard]] boost::asio::awaitable<util::Expected<std::optional<ai::Credential>>> modify(
        std::string provider_id,
        ai::CredentialModifyHook modifier) override {
        CCH_TRY(credential, co_await storage_->modify(
            key(std::move(provider_id)), std::move(modifier)));
        co_return credential;
    }

    [[nodiscard]] boost::asio::awaitable<util::ExpectedVoid> remove(
        std::string provider_id) override {
        CCH_TRY_VOID(co_await storage_->remove(key(std::move(provider_id))));
        co_return util::ExpectedVoid{};
    }

private:
    [[nodiscard]] std::string key(std::string provider_id) const {
        return alias_.empty() ? provider_id : alias_;
    }

    std::shared_ptr<ai::CredentialStore> storage_;
    std::string alias_;
};

/// Transitional projection for the legacy string-only session settings. Full
/// catalog and models.json composition belongs to #345. Until that seam lands,
/// configured provider strings use pi's frozen custom-model defaults instead
/// of claiming catalog-specific capabilities, while the private test Models
/// seam carries an explicit unknown protocol/capability sentinel.
[[nodiscard]] ai::Model make_transitional_request_model(
    const coding_agent::ResolvedProviderSettings& resolved,
    bool uses_injected_models) {
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
    if (uses_injected_models) {
        model.api = "unknown";
        model.provider = kHostClientProvider;
        model.base_url.clear();
        model.input.clear();
        model.context_window = 0;
        model.max_tokens = 0;
    }
    return model;
}

[[nodiscard]] util::Expected<std::shared_ptr<ai::Models>> build_models(
    const coding_agent::ResolvedProviderSettings& resolved,
    const ai::Model& model) {
    if (resolved.provider_registry_name == "fake") {
        auto models = ai::providers::make_scripted_fake_models();
        if (!models) {
            return std::unexpected(util::make_error(
                util::ErrorCode::Provider,
                "failed to compose fake Provider"));
        }
        return models;
    }

    std::shared_ptr<ai::CredentialStore> credentials =
        std::make_shared<coding_agent::AuthStorage>(coding_agent::auth_file_path());
    if (!resolved.auth.empty()) {
        credentials = std::make_shared<AliasedCredentialStore>(
            std::move(credentials), resolved.auth);
    }
    auto models = std::make_shared<ai::Models>(
        std::move(credentials),
        std::make_shared<ProcessAuthContext>());
    auto provider = ai::providers::make_openai_compatible_provider(
        model.provider,
        std::vector<ai::Model>{model},
        resolved.api_key_env_chain,
        std::make_shared<ai::providers::BoostBeastStreamTransport>());
    if (auto added = models->set_provider(std::move(provider)); !added) {
        return std::unexpected(added.error());
    }
    return models;
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

    coding_agent::ProviderRequest pr;
    pr.model = request.provider_overrides.model;
    pr.base_url = request.provider_overrides.base_url;
    if (request.provider_overrides.api_key_env) {
        pr.api_key_env = std::vector<std::string>{*request.provider_overrides.api_key_env};
    }
    pr.auth = request.provider_overrides.auth;
    pr.provider = request.fake ? std::optional<std::string>{"fake"} : std::nullopt;
    plan.provider_request = std::move(pr);

    plan.models = nullptr;
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
    std::shared_ptr<ai::Models> models = {}) {
    const bool is_resume =
        std::holds_alternative<ExplicitResumeSessionTarget>(options.session_target);
    const bool has_provider_config = options.provider_config.has_value();

    if (!models && !has_provider_config) {
        if (!is_resume) {
            return std::unexpected(util::make_error(
                util::ErrorCode::Validation,
                "no provider_config supplied",
                "supply provider_config to create an agent session"));
        }
        // Resume may reconstruct a client from stored metadata and user settings.
    }
    if (has_provider_config && options.provider_config->api_key_env.has_value() &&
        options.provider_config->api_key_env->empty()) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "api_key_env chain is empty",
            "provide at least one environment variable name"));
    }

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

    coding_agent::ProviderRequest pr;
    if (options.provider_config) {
        pr.provider = options.provider_config->provider;
        pr.model = options.provider_config->model;
        pr.base_url = options.provider_config->base_url;
        pr.api_key_env = options.provider_config->api_key_env;
    }
    plan.provider_request = std::move(pr);

    plan.models = std::move(models);
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

    // 3. Resolve provider/model metadata.
    std::optional<std::string> stored_provider;
    std::optional<std::string> stored_model;
    if (is_resume) {
        stored_provider = prepared_resume.resume.metadata.provider;
        stored_model = prepared_resume.resume.metadata.model;
    }

    std::string provider_registry_name;
    if (plan.provider_request.provider) {
        provider_registry_name = *plan.provider_request.provider;
    } else if (plan.models) {
        provider_registry_name = std::string{kHostClientProvider};
    } else if (is_resume && stored_provider) {
        provider_registry_name = (*stored_provider == "fake") ? "fake" : "openai-compatible";
    } else {
        provider_registry_name = "openai-compatible";
    }

    auto resolved = coding_agent::resolve_provider_settings(
        provider_registry_name,
        plan.provider_request,
        settings,
        stored_provider,
        stored_model);

    // Metadata-less sessions using the private Models assembly seam record an
    // opaque sentinel. Explicit or stored metadata is preserved.
    if (plan.models && !is_resume && !plan.provider_request.model) {
        resolved.model = std::string{kHostClientModel};
    }

    // Resume: explicit provider/model overrides are allowed but warned.
    if (is_resume) {
        const bool provider_overridden = plan.provider_request.provider.has_value() &&
                                         stored_provider.has_value() &&
                                         *plan.provider_request.provider != *stored_provider;
        const bool model_overridden = plan.provider_request.model.has_value() &&
                                      stored_model.has_value() &&
                                      *plan.provider_request.model != *stored_model;
        if (provider_overridden || model_overridden) {
            diagnostics.push_back(make_diag(
                SdkDiagnostic::Severity::Warning,
                "resume_provider_override",
                std::format(
                    "Resumed session provider/model metadata overridden by explicit request; "
                    "was ({}/{}) now ({}/{})",
                    stored_provider.value_or(""),
                    stored_model.value_or(""),
                    resolved.provider,
                    resolved.model)));
        }
    }

    // 4. Build or adopt the private Models-backed stream capability. Public
    // ModelRuntime injection lands in #345; direct Models injection is limited
    // to the private assembly seam used by focused session tests.
    const bool uses_injected_models =
        plan.models != nullptr &&
        (!plan.provider_request.provider || plan.provider_request.provider->empty() ||
         !plan.provider_request.model || plan.provider_request.model->empty());
    auto request_model = make_transitional_request_model(
        resolved, uses_injected_models);
    std::shared_ptr<ai::Models> models = std::move(plan.models);
    if (!models) {
        auto composed = build_models(resolved, request_model);
        if (!composed) {
            return std::unexpected(composed.error());
        }
        models = std::move(*composed);
    }
    std::shared_ptr<ai::StreamingChatClient> stream = models;

    // 5. Resolve execution environment and ownership.
    std::shared_ptr<harness::AsyncExecutionEnv> exec_env;
    bool env_owned = true;
    if (plan.host_execution_env) {
        exec_env = plan.host_execution_env;
        env_owned = false;
    } else {
        exec_env = std::make_shared<harness::AsyncLocalExecutionEnv>(
            workspace,
            plan.builtin_tools.bash,
            resolved.api_key_env_chain,
            harness::ShellConfig{
                .shell_path = settings.shell_path,
                .command_prefix = settings.shell_command_prefix,
            });
    }

    auto cleanup_on_failure = [&]() { cleanup_factory_env(env_owned, exec_env.get()); };

    // 6. Build the tool registry from enabled built-ins plus validated custom tools.
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

    // 7. Load project resources if requested.
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
            resolved.provider,
            resolved.model);
        if (!published) {
            cleanup_on_failure();
            return std::unexpected(published.error());
        }
        open = std::move(*published);
    }

    // 9. Assemble the runtime. The Native TUI's Session-owned User Shell is
    // an independent capability instance: it shares only the effective
    // user-level Shell configuration with an enabled model Bash Tool and
    // never widens the shared Execution Environment (ADR 0026).
    RuntimeServices services;
    services.stream = std::move(stream);
    services.env = std::move(exec_env);
    services.env_owned = env_owned;
    services.user_shell = std::move(user_shell);
    if (!services.user_shell && plan.provide_user_shell) {
        services.user_shell = std::make_unique<LocalUserShell>(
            workspace,
            resolved.api_key_env_chain,
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
    auto runtime = std::make_unique<AgentSessionRuntime>(
        std::move(services),
        std::move(open),
        std::move(prompt_processor),
        std::move(runtime_config));

    CreateAgentSessionResult result;
    result.runtime = std::move(runtime);
    result.diagnostics = std::move(diagnostics);
    result.session_id = metadata.session_id;
    result.provider = resolved.provider;
    result.model = resolved.model;
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
    return finish_creation(
        normalize_sdk(std::move(options), std::move(models)), snapshot);
}

util::Expected<CreateAgentSessionResult> SessionFactory::create(
    CreateAgentSessionOptions options,
    std::shared_ptr<ai::Models> models,
    std::unique_ptr<AsyncUserShell> user_shell) {
    const auto snapshot = load_user_settings_snapshot();
    return finish_creation(
        normalize_sdk(std::move(options), std::move(models)),
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
