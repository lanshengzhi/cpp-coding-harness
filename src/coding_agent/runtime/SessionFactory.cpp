#include "SessionFactory.hpp"

#include "../../../include/cch/ai/ProviderRegistry.hpp"
#include "../../../include/cch/coding_agent/Config.hpp"
#include "../../../include/cch/coding_agent/ProjectResources.hpp"
#include "../../../include/cch/coding_agent/ProjectTrust.hpp"
#include "coding_agent/SkillFormatting.hpp"
#include "coding_agent/SkillLoader.hpp"
#include "coding_agent/PromptTemplateLoader.hpp"
#include "../../../include/cch/tools/ToolFactories.hpp"
#include "../../../include/cch/util/Error.hpp"
#include "../../harness/WorkspaceFileSystem.hpp"
#include "RuntimeServices.hpp"
#include "SessionLifecycle.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <format>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace cch::coding_agent::runtime {
namespace {

[[nodiscard]] SdkDiagnostic make_diag(SdkDiagnostic::Severity severity,
                                       std::string code,
                                       std::string message,
                                       std::optional<std::string> path = std::nullopt) {
    return SdkDiagnostic{severity, std::move(code), std::move(message), std::move(path)};
}

[[nodiscard]] std::string generate_session_id() {
    auto now = std::chrono::system_clock::now();
    auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    return std::format("sdk-{}", ts);
}

[[nodiscard]] std::string make_iso_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);
    std::ostringstream oss;
    oss << std::put_time(std::gmtime(&tt), "%FT%TZ");
    return oss.str();
}

constexpr std::string_view kHostClientProvider = "sdk-host";
constexpr std::string_view kHostClientModel = "host-client";

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
    std::unique_ptr<ai::StreamingChatClient> host_client,
    const std::optional<SdkProviderConfig>& provider_config,
    std::vector<SdkDiagnostic>& diagnostics) {
    if (host_client) {
        diagnostics.push_back(make_diag(
            SdkDiagnostic::Severity::Info,
            "host_client_used",
            "Using host-provided chat client; provider_config metadata is informational only"));
        return host_client;
    }

    if (!provider_config) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "no chat client or provider_config supplied",
            "supply a chat_client or provider_config to create an agent session"));
    }

    const auto& pc = *provider_config;

    std::string resolved_env_name;
    if (pc.api_key_env) {
        if (pc.api_key_env->empty()) {
            return std::unexpected(util::make_error(
                util::ErrorCode::Validation,
                "api_key_env chain is empty",
                "provide at least one environment variable name or supply a host-provided chat client"));
        }

        bool found{false};
        for (const auto& name : *pc.api_key_env) {
            const char* val = std::getenv(name.c_str());
            if (val && val[0] != '\0') {
                resolved_env_name = name;
                found = true;
                break;
            }
        }
        if (!found) {
            std::string chain_str;
            for (size_t i = 0; i < pc.api_key_env->size(); ++i) {
                if (i > 0) chain_str += ", ";
                chain_str += (*pc.api_key_env)[i];
            }
            return std::unexpected(util::make_error(
                util::ErrorCode::Validation,
                std::format("none of the API key env vars [{}] are set or non-empty", chain_str),
                "set one of the environment variables or supply a host-provided chat client"));
        }
    }

    auto registry = ai::make_default_provider_registry();
    if (!registry) {
        return std::unexpected(registry.error());
    }

    ai::ProviderFactoryContext ctx;
    ctx.provider_registry_name = pc.provider;
    ctx.provider = pc.provider;
    ctx.api = pc.provider == "fake" ? "scripted-fake" : "openai-completions";
    ctx.model = pc.model;
    ctx.base_url = pc.base_url.value_or("");
    ctx.api_key_env = resolved_env_name.empty()
        ? (pc.api_key_env && !pc.api_key_env->empty() ? (*pc.api_key_env)[0] : "")
        : resolved_env_name;

    auto client = registry->create(pc.provider, ctx);
    if (!client) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Provider,
            std::format("failed to create provider client '{}': {}", pc.provider, client.error().message),
            client.error().detail));
    }

    return client;
}

} // namespace

util::Expected<CreateAgentSessionResult> SessionFactory::create(
    CreateAgentSessionOptions options) {
    std::vector<SdkDiagnostic> diagnostics;

    const bool has_create = options.session_path.has_value();
    const bool has_resume = options.resume_path.has_value();

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

    bool has_host_client = options.chat_client != nullptr;
    bool build_client_now = has_create || has_host_client || options.provider_config.has_value();

    std::unique_ptr<ai::StreamingChatClient> chat_client;
    std::string provider_name;
    std::string model_name;

    if (build_client_now) {
        auto client_result = build_chat_client(
            std::move(options.chat_client),
            options.provider_config,
            diagnostics);
        if (!client_result) {
            return std::unexpected(client_result.error());
        }
        chat_client = std::move(*client_result);

        if (has_host_client) {
            provider_name = std::string{kHostClientProvider};
            model_name = std::string{kHostClientModel};
        } else if (options.provider_config) {
            provider_name = options.provider_config->provider;
            model_name = options.provider_config->model;
        }
    }

    if (has_create && options.workspace.empty()) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "workspace is required for new sessions",
            "supply a non-empty workspace path"));
    }

    SessionOpenRequest session_req;
    session_req.workspace = options.workspace;

    if (has_resume) {
        session_req.resume_path = *options.resume_path;
        session_req.workspace_explicit = !options.workspace.empty();
    } else {
        session_req.session_path = *options.session_path;
        session_req.session_id = generate_session_id();
        session_req.created_at = make_iso_timestamp();
        session_req.provider = provider_name;
        session_req.model = model_name;
    }

    auto open_result = open_session(session_req);
    if (!open_result) {
        return std::unexpected(open_result.error());
    }

    auto& open = *open_result;

    if (has_resume) {
        auto preload = harness::session::JsonlSessionStore::load(*options.resume_path);
        if (preload) {
            bool has_non_linear{false};
            for (const auto& entry : preload->entries) {
                if (entry.parent_id.has_value() ||
                    entry.kind == harness::session::SessionEntryKind::Compaction ||
                    entry.kind == harness::session::SessionEntryKind::BranchSummary ||
                    entry.kind == harness::session::SessionEntryKind::Label ||
                    entry.kind == harness::session::SessionEntryKind::Leaf) {
                    has_non_linear = true;
                    break;
                }
            }
            if (has_non_linear) {
                return std::unexpected(util::make_error(
                    util::ErrorCode::Session,
                    "unsupported_session_topology",
                    "SDK v1 supports only linear sessions; the resumed session contains branches, "
                    "compactions, or tree metadata that cannot be appended linearly"));
            }
        }
    }

    if (has_resume) {
        if (has_host_client) {
            diagnostics.push_back(make_diag(
                SdkDiagnostic::Severity::Info,
                "host_client_resume",
                "Resumed session with host-provided chat client; stored provider/model metadata may differ"));
        } else if (open.stored_provider && open.stored_model) {
            provider_name = *open.stored_provider;
            model_name = *open.stored_model;

            if (!chat_client) {
                const char* home = std::getenv("HOME");
                if (home) {
                    std::string config_path = std::string(home) + "/.cpp-harness/config.json";
                    auto config = ConfigLoader::load(config_path);
                    if (config && config->provider == provider_name) {
                        SdkProviderConfig pc;
                        pc.provider = provider_name;
                        pc.model = model_name;
                        pc.base_url = config->base_url;
                        pc.api_key_env = config->api_key_env;
                        auto client_result = build_chat_client(nullptr, pc, diagnostics);
                        if (client_result) {
                            chat_client = std::move(*client_result);
                            diagnostics.push_back(make_diag(
                                SdkDiagnostic::Severity::Info,
                                "resume_default_client",
                                std::format("Constructed default provider client from stored metadata ({}/{})",
                                    provider_name, model_name)));
                        }
                    }
                }
                if (!chat_client) {
                    return std::unexpected(util::make_error(
                        util::ErrorCode::Session,
                        "resumed session has no stored provider/model metadata usable for client construction",
                        "supply a provider_config or host chat_client when resuming sessions, "
                        "or ensure ~/.cpp-harness/config.json has matching provider/model/api_key_env"));
                }
            }

            if (options.provider_config) {
                diagnostics.push_back(make_diag(
                    SdkDiagnostic::Severity::Warning,
                    "resume_provider_override",
                    std::format("Resumed session provider/model ({}/{}) overridden by explicit provider_config ({}/{})",
                        provider_name, model_name,
                        options.provider_config->provider, options.provider_config->model)));
                provider_name = options.provider_config->provider;
                model_name = options.provider_config->model;
            }
        } else if (!chat_client) {
            return std::unexpected(util::make_error(
                util::ErrorCode::Session,
                "resumed session has no stored provider/model metadata and no provider_config supplied",
                "supply a provider_config or host chat_client when resuming sessions without stored metadata"));
        }
    }

    std::shared_ptr<harness::AsyncExecutionEnv> exec_env;
    if (options.execution_env) {
        exec_env = options.execution_env;
    } else {
        exec_env = std::make_shared<harness::AsyncLocalExecutionEnv>(
            open.workspace,
            options.builtin_tools.bash,
            options.provider_config && options.provider_config->api_key_env
                ? *options.provider_config->api_key_env
                : std::vector<std::string>{});
    }

    agent::AsyncToolRegistry tools;
    std::set<std::string> builtin_names;

    if (options.builtin_tools.read) {
        builtin_names.insert("read");
        if (auto added = tools.add(tools::make_async_read_file_tool(exec_env)); !added) {
            return std::unexpected(added.error());
        }
    }
    if (options.builtin_tools.write) {
        builtin_names.insert("write");
        if (auto added = tools.add(tools::make_async_write_file_tool(exec_env)); !added) {
            return std::unexpected(added.error());
        }
    }
    if (options.builtin_tools.edit_file) {
        builtin_names.insert("edit_file");
        if (auto added = tools.add(tools::make_async_edit_file_tool(exec_env)); !added) {
            return std::unexpected(added.error());
        }
    }
    if (options.builtin_tools.bash) {
        builtin_names.insert("bash");
        if (auto added = tools.add(tools::make_async_bash_tool(exec_env)); !added) {
            return std::unexpected(added.error());
        }
    }

    if (auto dup = find_duplicate_tool_name(options.custom_tools)) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            std::format("duplicate custom tool name: '{}'", *dup),
            "each custom tool must have a unique name"));
    }
    if (auto collision = find_builtin_custom_collision(builtin_names, options.custom_tools)) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            std::format("custom tool name '{}' collides with built-in tool", *collision),
            "rename the custom tool or disable the conflicting built-in tool"));
    }

    for (auto& tool : options.custom_tools) {
        if (tool) {
            if (auto added = tools.add(std::move(tool)); !added) {
                return std::unexpected(added.error());
            }
        }
    }

    std::vector<Skill> skills = std::move(options.skills);
    std::vector<PromptTemplate> templates = std::move(options.prompt_templates);

    CommandRegistry command_registry;
    {
        std::set<std::string> cmd_names;
        for (auto& cmd : options.commands) {
            if (!cmd_names.insert(cmd.name).second) {
                return std::unexpected(util::make_error(
                    util::ErrorCode::Validation,
                    std::format("duplicate command name: '{}'", cmd.name)));
            }
        }
        for (auto& cmd : options.commands) {
            command_registry.register_command(std::move(cmd.name), std::move(cmd.handler));
        }
    }

    if (options.load_project_resources) {
        auto fs = harness::WorkspaceFileSystem::create(open.workspace);
        if (fs) {
            auto default_trust = options.default_project_trust.value_or(DefaultProjectTrust::Ask);
            auto skill_enablement = options.project_skills_enablement.value_or(ResourceEnablement::Auto);

            ProjectTrustStore trust_store(open.workspace / ".cpp-harness" / "trust.json");

            ProjectResourcePolicy policy;
            policy.project_skills = skill_enablement;

            auto detection = detect_project_resources(*fs);
            bool needs_trust = needs_project_trust_resolution(detection, policy);

            auto trust_resolution = resolve_project_trust(
                open.workspace,
                needs_trust,
                trust_store,
                default_trust,
                std::nullopt);

            auto load_plan = build_project_resource_load_plan(detection, policy, trust_resolution);

            for (const auto& td : trust_resolution.diagnostics) {
                auto sev = SdkDiagnostic::Severity::Warning;
                if (td.severity == ProjectTrustDiagnosticSeverity::Error) {
                    sev = SdkDiagnostic::Severity::Error;
                } else if (td.severity == ProjectTrustDiagnosticSeverity::Info) {
                    sev = SdkDiagnostic::Severity::Info;
                }
                diagnostics.push_back(make_diag(sev, td.code, td.message, td.path));
            }

            for (const auto& rd : load_plan.diagnostics) {
                auto sev = SdkDiagnostic::Severity::Warning;
                if (rd.severity == ResourceDiagnosticSeverity::Error) {
                    sev = SdkDiagnostic::Severity::Error;
                } else if (rd.severity == ResourceDiagnosticSeverity::Info) {
                    sev = SdkDiagnostic::Severity::Info;
                }
                diagnostics.push_back(make_diag(sev, rd.code, rd.message, rd.path));
            }

            if (project_skills_allowed(load_plan)) {
                std::vector<SkillDirSpec> skill_dirs;
                auto skills_dir = open.workspace / ".cpp-harness" / "skills";
                skill_dirs.push_back({.path = skills_dir.string(), .includeRootFiles = false});

                auto skill_load = loadSkills(*fs, skill_dirs);

                std::set<std::string> host_names;
                for (const auto& s : skills) {
                    host_names.insert(s.name);
                }
                for (auto& s : skill_load.skills) {
                    if (host_names.contains(s.name)) {
                        diagnostics.push_back(make_diag(
                            SdkDiagnostic::Severity::Info,
                            "duplicate_skill_skipped",
                            std::format("project skill '{}' skipped: host-provided skill takes precedence", s.name),
                            s.filePath));
                    } else {
                        skills.push_back(std::move(s));
                    }
                }

                for (const auto& sd : skill_load.diagnostics) {
                    diagnostics.push_back(make_diag(
                        SdkDiagnostic::Severity::Warning,
                        "skill_load",
                        sd.message,
                        sd.path));
                }
            }

            if (project_prompts_allowed(load_plan)) {
                std::vector<PromptTemplateDirSpec> prompt_specs;
                auto prompts_dir = open.workspace / ".cpp-harness" / "prompts";
                prompt_specs.push_back({.path = prompts_dir.string(), .is_file = false});

                auto prompt_load = loadPromptTemplates(*fs, prompt_specs);

                std::set<std::string> host_tmpl_names;
                for (const auto& t : templates) {
                    host_tmpl_names.insert(t.name);
                }
                for (auto& t : prompt_load.templates) {
                    if (host_tmpl_names.contains(t.name)) {
                        diagnostics.push_back(make_diag(
                            SdkDiagnostic::Severity::Info,
                            "duplicate_template_skipped",
                            std::format("project template '{}' skipped: host-provided template takes precedence", t.name),
                            std::nullopt));
                    } else {
                        templates.push_back(std::move(t));
                    }
                }

                for (const auto& pd : prompt_load.diagnostics) {
                    diagnostics.push_back(make_diag(
                        SdkDiagnostic::Severity::Warning,
                        "template_load",
                        pd.message,
                        pd.path));
                }
            }
        } else {
            diagnostics.push_back(make_diag(
                SdkDiagnostic::Severity::Warning,
                "workspace_fs_failed",
                "Could not create workspace filesystem for project resource discovery",
                open.workspace.string()));
        }
    }

    RuntimeServices services;
    services.client = std::move(chat_client);
    services.env = std::move(exec_env);
    services.tools = std::move(tools);
    services.skill_load_result.skills = std::move(skills);
    services.prompt_load_result.templates = std::move(templates);

    AgentSessionRuntimeConfig runtime_config;
    runtime_config.max_turns = options.max_turns;
    runtime_config.model = model_name;
    runtime_config.capture_skill_diagnostics = true;

    auto runtime = std::make_unique<AgentSessionRuntime>(
        std::move(services),
        std::move(open),
        std::move(command_registry),
        std::move(runtime_config));

    CreateAgentSessionResult result;
    result.runtime = std::move(runtime);
    result.diagnostics = std::move(diagnostics);
    result.session_id = result.runtime->session_id();
    result.provider = result.runtime->provider();
    result.model = result.runtime->model();
    result.session_path = result.runtime->session_path();
    result.workspace = result.runtime->workspace();

    return result;
}

} // namespace cch::coding_agent::runtime
