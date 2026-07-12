#include "SessionFactory.hpp"

#include "AgentSessionRuntime.hpp"
#include "RuntimeServices.hpp"
#include "SessionLifecycle.hpp"

#include "coding_agent/ProjectResourceLoader.hpp"
#include "coding_agent/ProviderConfigResolution.hpp"

#include "../../../include/cch/ai/ProviderRegistry.hpp"
#include "../../../include/cch/coding_agent/Config.hpp"
#include "../../../include/cch/coding_agent/ProjectResources.hpp"
#include "../../../include/cch/coding_agent/ProjectTrust.hpp"
#include "../../../include/cch/tools/ToolFactories.hpp"
#include "../../../include/cch/util/Error.hpp"
#include "../../harness/WorkspaceFileSystem.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <iomanip>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace cch::coding_agent::runtime {
namespace {

constexpr std::string_view kHostClientProvider = "sdk-host";
constexpr std::string_view kHostClientModel = "host-client";

struct NewSessionTarget {
    std::filesystem::path session_path;
    std::filesystem::path workspace;
};

struct ResumeSessionTarget {
    std::filesystem::path resume_path;
    std::filesystem::path workspace;
    bool workspace_explicit{false};
};

using SessionTarget = std::variant<NewSessionTarget, ResumeSessionTarget>;

enum class CreationProfile { Cli, Sdk };

struct AssemblyPlan {
    CreationProfile profile;
    SessionTarget target;
    coding_agent::ProviderRequest provider_request;
    std::unique_ptr<ai::StreamingChatClient> host_client;
    std::shared_ptr<harness::AsyncExecutionEnv> host_execution_env;
    coding_agent::SdkBuiltinTools builtin_tools;
    std::vector<std::unique_ptr<agent::AsyncAgentTool>> custom_tools;
    std::vector<Skill> host_skills;
    std::vector<PromptTemplate> host_prompt_templates;
    std::vector<SdkCommand> commands;
    std::vector<std::string> prompt_template_paths;
    bool load_project_resources{false};
    std::optional<DefaultProjectTrust> default_project_trust;
    std::optional<ResourceEnablement> project_skills_enablement;
    bool prompt_templates_enabled{true};
    std::optional<std::filesystem::path> trust_store_path;
    std::optional<bool> project_trust_override;
    int max_turns{30};
    CommandRegistry command_registry;
    bool capture_skill_diagnostics{false};
};

[[nodiscard]] SdkDiagnostic make_diag(SdkDiagnostic::Severity severity,
                                       std::string code,
                                       std::string message,
                                       std::optional<std::string> path = std::nullopt) {
    return SdkDiagnostic{severity, std::move(code), std::move(message), std::move(path)};
}

[[nodiscard]] std::string generate_session_id() {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    std::random_device device;
    std::uniform_int_distribution<unsigned int> distribution(0, 0xFFFFu);
    return std::format("cch-{}-{:04x}", ms, distribution(device));
}

[[nodiscard]] std::string make_iso_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);
    std::ostringstream oss;
    oss << std::put_time(std::gmtime(&tt), "%FT%TZ");
    return oss.str();
}

[[nodiscard]] std::filesystem::path default_trust_store_path() {
    const char* home = std::getenv("HOME");
    return home ? std::filesystem::path{home} / ".cpp-harness" / "trust.json"
                : std::filesystem::path{};
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

[[nodiscard]] util::Expected<std::unique_ptr<ai::StreamingChatClient>> build_chat_client(
    const coding_agent::ProviderRequest& explicit_request,
    const coding_agent::ResolvedProviderSettings& resolved,
    std::vector<SdkDiagnostic>& diagnostics) {
    (void)diagnostics;

    auto registry = ai::make_default_provider_registry();
    if (!registry) {
        return std::unexpected(registry.error());
    }

    bool should_resolve_env = (resolved.provider_registry_name != "fake");
    if (!should_resolve_env && explicit_request.api_key_env.has_value()) {
        should_resolve_env = true;
    }

    std::string resolved_env_name;
    if (should_resolve_env && resolved.api_key.empty()) {
        bool found{false};
        for (const auto& name : resolved.api_key_env_chain) {
            const char* val = std::getenv(name.c_str());
            if (val && val[0] != '\0') {
                resolved_env_name = name;
                found = true;
                break;
            }
        }
        if (!found) {
            std::string chain_str;
            for (size_t i = 0; i < resolved.api_key_env_chain.size(); ++i) {
                if (i > 0) chain_str += ", ";
                chain_str += resolved.api_key_env_chain[i];
            }
            return std::unexpected(util::make_error(
                util::ErrorCode::Validation,
                std::format("none of the API key env vars [{}] are set or non-empty", chain_str),
                "set one of the environment variables or supply a host-provided chat client"));
        }
    }

    ai::ProviderFactoryContext ctx;
    ctx.provider_registry_name = resolved.provider_registry_name;
    ctx.provider = resolved.provider;
    ctx.api = resolved.api;
    ctx.model = resolved.model;
    ctx.base_url = resolved.base_url;
    ctx.api_key = resolved.api_key;
    ctx.api_key_env = resolved_env_name.empty() ? resolved.api_key_env : resolved_env_name;

    auto client = registry->create(resolved.provider_registry_name, ctx);
    if (!client) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Provider,
            std::format("failed to create provider client '{}': {}", resolved.provider_registry_name, client.error().message),
            client.error().detail));
    }

    return client;
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

[[nodiscard]] util::Expected<AssemblyPlan> normalize_cli(AgentSessionCreationRequest request) {
    // The CLI preflight fills in a default session_path even when resuming,
    // so resume intent is determined by resume_path being non-empty.
    const bool has_resume = !request.resume_path.empty();
    const bool has_create = !has_resume && !request.session_path.empty();

    if (has_create && has_resume) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "both session_path and resume_path are set",
            "set exactly one of session_path (create new) or resume_path (resume existing)"));
    }
    if (!has_create && !has_resume) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "neither session_path nor resume_path is set",
            "set exactly one of session_path (create new) or resume_path (resume existing)"));
    }

    AssemblyPlan plan;
    plan.profile = CreationProfile::Cli;
    if (has_create) {
        plan.target = NewSessionTarget{request.session_path, request.workspace};
    } else {
        plan.target = ResumeSessionTarget{request.resume_path, request.workspace, request.workspace_explicit};
    }

    coding_agent::ProviderRequest pr;
    pr.model = request.provider_overrides.model;
    pr.base_url = request.provider_overrides.base_url;
    if (request.provider_overrides.api_key_env) {
        pr.api_key_env = std::vector<std::string>{*request.provider_overrides.api_key_env};
    }
    pr.auth = request.provider_overrides.auth;
    pr.provider = request.fake ? std::optional<std::string>{"fake"} : std::nullopt;
    plan.provider_request = std::move(pr);

    plan.host_client = nullptr;
    plan.host_execution_env = nullptr;
    plan.builtin_tools = coding_agent::SdkBuiltinTools{
        .read = true,
        .write = true,
        .edit_file = true,
        .bash = request.enable_bash,
    };

    plan.load_project_resources = true;
    plan.project_trust_override = request.project_trust_override;
    plan.default_project_trust = request.config.default_project_trust.value_or(DefaultProjectTrust::Ask);
    plan.project_skills_enablement = request.config.project_skills;
    if (request.disable_project_skills) {
        plan.project_skills_enablement = ResourceEnablement::Off;
    }
    plan.prompt_templates_enabled = !request.disable_prompt_templates;
    plan.prompt_template_paths = request.prompt_template_paths;
    plan.max_turns = request.max_turns;
    plan.command_registry = std::move(request.command_registry);
    plan.capture_skill_diagnostics = false;

    return plan;
}

[[nodiscard]] util::Expected<AssemblyPlan> normalize_sdk(CreateAgentSessionOptions options) {
    const bool has_create = options.session_path.has_value();
    const bool has_resume = options.resume_path.has_value();
    const bool has_chat_client = options.chat_client != nullptr;
    const bool has_provider_config = options.provider_config.has_value();

    if (has_create && has_resume) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "both session_path and resume_path are set",
            "set exactly one of session_path (create new) or resume_path (resume existing)"));
    }
    if (!has_create && !has_resume) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "neither session_path nor resume_path is set",
            "set exactly one of session_path (create new) or resume_path (resume existing)"));
    }
    if (!has_chat_client && !has_provider_config) {
        if (!has_resume) {
            return std::unexpected(util::make_error(
                util::ErrorCode::Validation,
                "no chat client or provider_config supplied",
                "supply a chat_client or provider_config to create an agent session"));
        }
        // Resume may reconstruct a client from stored metadata and user config.
    }
    if (has_provider_config && options.provider_config->api_key_env.has_value() &&
        options.provider_config->api_key_env->empty()) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "api_key_env chain is empty",
            "provide at least one environment variable name or supply a host-provided chat client"));
    }

    AssemblyPlan plan;
    plan.profile = CreationProfile::Sdk;
    if (has_create) {
        plan.target = NewSessionTarget{*options.session_path, options.workspace};
    } else {
        plan.target = ResumeSessionTarget{*options.resume_path, options.workspace, !options.workspace.empty()};
    }

    coding_agent::ProviderRequest pr;
    if (options.provider_config) {
        pr.provider = options.provider_config->provider;
        pr.model = options.provider_config->model;
        pr.base_url = options.provider_config->base_url;
        pr.api_key_env = options.provider_config->api_key_env;
    }
    plan.provider_request = std::move(pr);

    plan.host_client = std::move(options.chat_client);
    plan.host_execution_env = std::move(options.execution_env);
    plan.builtin_tools = options.builtin_tools;
    plan.custom_tools = std::move(options.custom_tools);
    plan.host_skills = std::move(options.skills);
    plan.host_prompt_templates = std::move(options.prompt_templates);
    plan.commands = std::move(options.commands);
    plan.load_project_resources = options.load_project_resources;
    plan.default_project_trust = options.default_project_trust;
    plan.project_skills_enablement = options.project_skills_enablement;
    plan.max_turns = options.max_turns;
    plan.capture_skill_diagnostics = true;
    plan.trust_store_path = options.trust_store_path;

    return plan;
}

// ─────────────────────────────────────────────────────────────────────────────
// Assembly
// ─────────────────────────────────────────────────────────────────────────────

[[nodiscard]] util::Expected<CreateAgentSessionResult> run_assembly(AssemblyPlan plan) {
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
    } else {
        const auto& target = std::get<NewSessionTarget>(plan.target);
        if (target.workspace.empty()) {
            return std::unexpected(util::make_error(
                util::ErrorCode::Validation,
                "workspace is required for new sessions",
                "supply a non-empty workspace path"));
        }
        workspace = target.workspace;
    }

    // 2. Load user configuration for defaults and API key chains.
    auto config = coding_agent::ConfigLoader::load(coding_agent::ConfigLoader::default_config_path());
    if (!config) {
        config = coding_agent::ConfigData{};
    }

    // 3. Resolve provider/model metadata.
    std::optional<std::string> stored_provider;
    std::optional<std::string> stored_model;
    if (is_resume) {
        stored_provider = prepared_resume.resume.metadata.provider;
        stored_model = prepared_resume.resume.metadata.model;
    }

    std::string provider_registry_name;
    if (plan.host_client) {
        provider_registry_name = std::string{kHostClientProvider};
    } else if (plan.provider_request.provider) {
        provider_registry_name = *plan.provider_request.provider;
    } else if (is_resume && stored_provider) {
        provider_registry_name = (*stored_provider == "fake") ? "fake" : "openai-compatible";
    } else {
        provider_registry_name = "openai-compatible";
    }

    auto resolved = coding_agent::resolve_provider_settings(
        provider_registry_name,
        plan.provider_request,
        *config,
        stored_provider,
        stored_model);

    // Metadata-less new sessions with a host client record the documented host
    // sentinel as their model. Explicit or stored metadata is preserved.
    if (plan.host_client && !is_resume && !plan.provider_request.model) {
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

    // 4. Build or adopt the chat client.
    std::unique_ptr<ai::StreamingChatClient> chat_client;
    if (plan.host_client) {
        chat_client = std::move(plan.host_client);
        diagnostics.push_back(make_diag(
            SdkDiagnostic::Severity::Info,
            "host_client_used",
            "Using host-provided chat client; provider_config metadata is informational only"));
        if (is_resume) {
            diagnostics.push_back(make_diag(
                SdkDiagnostic::Severity::Info,
                "host_client_resume",
                "Resumed session with host-provided chat client; stored provider/model metadata may differ"));
        }
    } else {
        auto client_result = build_chat_client(plan.provider_request, resolved, diagnostics);
        if (!client_result) {
            return std::unexpected(client_result.error());
        }
        chat_client = std::move(*client_result);
    }

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
            resolved.api_key_env_chain);
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

    // 7. Validate and register slash-commands.
    CommandRegistry command_registry = std::move(plan.command_registry);
    {
        std::set<std::string> cmd_names;
        for (auto& cmd : plan.commands) {
            if (!cmd_names.insert(cmd.name).second) {
                cleanup_on_failure();
                return std::unexpected(util::make_error(
                    util::ErrorCode::Validation,
                    std::format("duplicate command name: '{}'", cmd.name)));
            }
        }
        for (auto& cmd : plan.commands) {
            command_registry.register_command(std::move(cmd.name), std::move(cmd.handler));
        }
    }

    // 8. Load project resources if requested.
    std::vector<Skill> skills = std::move(plan.host_skills);
    std::vector<PromptTemplate> templates = std::move(plan.host_prompt_templates);

    std::filesystem::path trust_store_path = plan.trust_store_path.value_or(default_trust_store_path());
    if (plan.profile == CreationProfile::Sdk && plan.trust_store_path) {
        if (auto valid = validate_trust_store_path(*plan.trust_store_path, workspace); !valid) {
            cleanup_on_failure();
            return std::unexpected(valid.error());
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
        const auto& target = std::get<NewSessionTarget>(plan.target);
        harness::session::SessionMetadata metadata{
            generate_session_id(),
            make_iso_timestamp(),
            workspace,
            resolved.provider,
            resolved.model,
        };
        auto published = publish_new_session(target.session_path, workspace, std::move(metadata));
        if (!published) {
            cleanup_on_failure();
            return std::unexpected(published.error());
        }
        open = std::move(*published);
    }

    // 10. Assemble the runtime.
    RuntimeServices services;
    services.client = std::move(chat_client);
    services.env = std::move(exec_env);
    services.env_owned = env_owned;
    services.tools = std::move(tools);
    services.skills = std::move(skills);
    services.prompt_templates = std::move(templates);

    AgentSessionRuntimeConfig runtime_config;
    runtime_config.max_turns = plan.max_turns;
    runtime_config.model = resolved.model;
    runtime_config.capture_skill_diagnostics = plan.capture_skill_diagnostics;

    const auto session_path = open.store->path();
    const auto metadata = open.metadata;
    auto runtime = std::make_unique<AgentSessionRuntime>(
        std::move(services),
        std::move(open),
        std::move(command_registry),
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

} // namespace

util::Expected<CreateAgentSessionResult> SessionFactory::create(
    AgentSessionCreationRequest request) {
    auto plan = normalize_cli(std::move(request));
    if (!plan) {
        return std::unexpected(plan.error());
    }
    return run_assembly(std::move(*plan));
}

util::Expected<CreateAgentSessionResult> SessionFactory::create(
    CreateAgentSessionOptions options) {
    auto plan = normalize_sdk(std::move(options));
    if (!plan) {
        return std::unexpected(plan.error());
    }
    return run_assembly(std::move(*plan));
}

} // namespace cch::coding_agent::runtime
