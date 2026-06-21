#include "../../include/cch/coding_agent/Sdk.hpp"

#include "../../include/cch/ai/ProviderRegistry.hpp"
#include "../../include/cch/coding_agent/Config.hpp"
#include "../../include/cch/coding_agent/ProjectResources.hpp"
#include "../../include/cch/coding_agent/ProjectTrust.hpp"
#include "../../include/cch/coding_agent/SkillFormatting.hpp"
#include "../../include/cch/coding_agent/SkillLoader.hpp"
#include "../../include/cch/coding_agent/PromptTemplateLoader.hpp"
#include "../../include/cch/tools/ToolFactories.hpp"
#include "../../include/cch/util/Error.hpp"
#include "../harness/WorkspaceFileSystem.hpp"
#include "runtime/AgentSessionRunner.hpp"
#include "runtime/RuntimeServices.hpp"
#include "runtime/SessionLifecycle.hpp"

#include <algorithm>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <chrono>
#include <format>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cch::coding_agent {

// ─────────────────────────────────────────────────────────────────────────────
// Pimpl definitions (must be defined before the destructors that use them)
// ─────────────────────────────────────────────────────────────────────────────

struct EventSubscription::Impl {
    int subscriber_index{-1};
    AgentSession::Impl* session{nullptr};
};

struct AgentSession::Impl {
    enum class State { Open, RunningPrompt, Closed };

    State state{State::Closed};

    // ── Session infrastructure ───────────────────────────────────────────
    harness::session::JsonlSessionStore store;
    std::vector<ai::MessageVariant> history;
    std::filesystem::path workspace_path;

    // ── Capabilities ─────────────────────────────────────────────────────
    std::unique_ptr<ai::StreamingChatClient> chat_client;
    std::shared_ptr<harness::AsyncExecutionEnv> execution_env;
    bool owns_execution_env{false};

    // ── Runner ───────────────────────────────────────────────────────────
    std::unique_ptr<runtime::AgentSessionRunner> runner;

    // ── Resources ────────────────────────────────────────────────────────
    std::vector<Skill> skills;
    std::vector<PromptTemplate> templates;
    CommandRegistry command_registry;

    // ── Subscribers ──────────────────────────────────────────────────────
    struct SubscriberEntry {
        agent::AgentEventSink sink;
        bool active{true};
    };
    std::vector<SubscriberEntry> subscribers;
    int next_subscriber_id{0};

    // ── Metadata ─────────────────────────────────────────────────────────
    std::string session_id;
    std::string provider_name;
    std::string model_name;
    std::filesystem::path session_file_path;

    // ── Helpers ──────────────────────────────────────────────────────────

    /// Fan out an event to all currently-active persistent subscribers.
    [[nodiscard]] util::ExpectedVoid fanout(const agent::AgentLifecycleEvent& event) {
        std::vector<agent::AgentEventSink*> active_sinks;
        for (auto& sub : subscribers) {
            if (sub.active && sub.sink) {
                active_sinks.push_back(&sub.sink);
            }
        }
        for (auto* sink : active_sinks) {
            if (*sink) {
                auto result = (*sink)(event);
                if (!result) return result;
            }
        }
        return {};
    }

    /// Build a combined sink: persistent subscribers + per-prompt sink.
    [[nodiscard]] agent::AgentEventSink make_combined_sink(agent::AgentEventSink per_prompt) {
        auto persistent = std::make_shared<std::vector<agent::AgentEventSink*>>();
        for (auto& sub : subscribers) {
            if (sub.active && sub.sink) {
                persistent->push_back(&sub.sink);
            }
        }

        return [persistent = std::move(persistent),
                per_prompt = std::move(per_prompt)](const agent::AgentLifecycleEvent& event) mutable
            -> util::ExpectedVoid {
            for (auto* sink : *persistent) {
                if (*sink) {
                    auto r = (*sink)(event);
                    if (!r) return r;
                }
            }
            if (per_prompt) {
                return per_prompt(event);
            }
            return {};
        };
    }

    /// Process a prompt through the SDK prompt pipeline (silent — no stderr).
    [[nodiscard]] PromptProcessingResult process_sdk_prompt(std::string_view raw_input) {
        auto skill_expansion = expand_skill_command_silent(raw_input, skills);
        std::string expanded = std::move(skill_expansion.expanded);

        if (expanded != raw_input) {
            PromptProcessingResult result;
            result.command_handled = false;
            result.expanded_prompt = std::move(expanded);
            return result;
        }

        CommandContext cmd_ctx{
            .session_id = session_id,
            .workspace_path = workspace_path.string(),
            .provider = provider_name,
            .model = model_name,
            .message_count = history.size(),
        };

        return process_prompt(raw_input, templates, command_registry, cmd_ctx);
    }
};

namespace {

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

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

/// Look up the last assistant message text from committed history.
[[nodiscard]] std::optional<std::string> last_assistant_text_from(
    const std::vector<ai::MessageVariant>& history) {
    for (auto it = history.rbegin(); it != history.rend(); ++it) {
        if (const auto* am = std::get_if<ai::AssistantMessage>(&*it)) {
            return ai::text_from_assistant_content(am->content);
        }
    }
    return std::nullopt;
}

// ─────────────────────────────────────────────────────────────────────────────
// Provider name for host-supplied clients
// ─────────────────────────────────────────────────────────────────────────────

constexpr std::string_view kHostClientProvider = "sdk-host";
constexpr std::string_view kHostClientModel = "host-client";

// ─────────────────────────────────────────────────────────────────────────────
// Duplicate detection helpers
// ─────────────────────────────────────────────────────────────────────────────

/// Check a vector of unique_ptr<AsyncAgentTool> for duplicate names.
/// Returns the first duplicate name, or nullopt if all unique.
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

/// Check that built-in tool names don't collide with custom tool names.
/// Returns the colliding name, or nullopt.
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

// ─────────────────────────────────────────────────────────────────────────────
// Session factory implementation
// ─────────────────────────────────────────────────────────────────────────────

[[nodiscard]] util::Expected<std::unique_ptr<ai::StreamingChatClient>> build_chat_client(
    std::unique_ptr<ai::StreamingChatClient> host_client,
    const std::optional<SdkProviderConfig>& provider_config,
    std::vector<SdkDiagnostic>& diagnostics) {
    // Host-provided client wins
    if (host_client) {
        diagnostics.push_back(make_diag(
            SdkDiagnostic::Severity::Info,
            "host_client_used",
            "Using host-provided chat client; provider_config metadata is informational only"));
        return host_client;
    }

    // Must have provider config
    if (!provider_config) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "no chat client or provider_config supplied",
            "supply a chat_client or provider_config to create an agent session"));
    }

    const auto& pc = *provider_config;

    // Resolve API key from environment
    std::string resolved_key;
    if (pc.api_key_env) {
        auto val = ConfigLoader::resolve_api_key({*pc.api_key_env});
        if (val) {
            resolved_key = *val;
        } else {
            return std::unexpected(util::make_error(
                util::ErrorCode::Validation,
                std::format("API key env var '{}' is not set or empty", *pc.api_key_env),
                "set the environment variable or supply a host-provided chat client"));
        }
    }

    // Build provider client through the registry
    auto registry = ai::make_default_provider_registry();
    if (!registry) {
        return std::unexpected(registry.error());
    }

    ai::ProviderFactoryContext ctx;
    ctx.model = pc.model;
    ctx.base_url = pc.base_url.value_or("");
    ctx.api_key_env = pc.api_key_env.value_or("");

    auto client = registry->create(pc.provider, ctx);
    if (!client) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Provider,
            std::format("failed to create provider client '{}': {}", pc.provider, client.error().message),
            client.error().detail));
    }

    return client;
}

[[nodiscard]] util::Expected<std::unique_ptr<runtime::AgentSessionRunner>> build_runner(
    ai::StreamingChatClient& client,
    agent::AsyncToolRegistry tools,
    const std::vector<PromptTemplate>& templates,
    CommandRegistry& command_registry,
    const std::vector<Skill>& skills,
    int max_turns) {
    agent::AsyncAgentOptions agent_opts;
    agent_opts.max_turns = max_turns > 0 ? max_turns : 30;

    return std::make_unique<runtime::AgentSessionRunner>(
        client,
        std::move(tools),
        std::move(agent_opts),
        templates,
        &command_registry,
        skills);
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// EventSubscription implementation
// ─────────────────────────────────────────────────────────────────────────────

EventSubscription::EventSubscription(EventSubscription&&) noexcept = default;
EventSubscription& EventSubscription::operator=(EventSubscription&&) noexcept = default;
EventSubscription::~EventSubscription() = default;

void EventSubscription::unsubscribe() {
    if (!impl_) return;
    if (impl_->session && impl_->subscriber_index >= 0) {
        auto idx = static_cast<std::size_t>(impl_->subscriber_index);
        if (idx < impl_->session->subscribers.size()) {
            impl_->session->subscribers[idx].active = false;
        }
    }
    impl_.reset();
}

EventSubscription::operator bool() const {
    if (!impl_) return false;
    if (!impl_->session) return false;
    if (impl_->subscriber_index < 0) return false;
    auto idx = static_cast<std::size_t>(impl_->subscriber_index);
    return idx < impl_->session->subscribers.size()
        && impl_->session->subscribers[idx].active;
}

// ─────────────────────────────────────────────────────────────────────────────
// AgentSession implementation
// ─────────────────────────────────────────────────────────────────────────────

AgentSession::AgentSession() = default;
AgentSession::AgentSession(AgentSession&&) noexcept = default;
AgentSession& AgentSession::operator=(AgentSession&&) noexcept = default;
AgentSession::~AgentSession() = default;

util::Expected<PromptResult> AgentSession::prompt(std::string text, PromptOptions options) {
    if (!impl_) {
        return std::unexpected(util::make_error(util::ErrorCode::Validation, "session is not initialized"));
    }
    if (impl_->state == AgentSession::Impl::State::Closed) {
        return std::unexpected(util::make_error(util::ErrorCode::Validation, "session is closed"));
    }
    if (impl_->state == AgentSession::Impl::State::RunningPrompt) {
        return std::unexpected(util::make_error(util::ErrorCode::Validation, "session is busy (prompt already in flight)"));
    }

    impl_->state = AgentSession::Impl::State::RunningPrompt;

    // Process the prompt through SDK pipeline (slash-commands, skill expansion)
    auto processed = impl_->process_sdk_prompt(text);

    PromptResult result;
    if (processed.command_handled) {
        result.success = true;
        result.code = "command_handled";
        result.message = processed.display_text.value_or("");
        result.last_assistant_text = last_assistant_text_from(impl_->history);
        result.message_count = impl_->history.size();
        impl_->state = AgentSession::Impl::State::Open;
        return result;
    }

    // Build combined sink
    auto combined_sink = impl_->make_combined_sink(std::move(options.event_sink));

    // Run the prompt
    auto run_result = impl_->runner->run_prompt(
        impl_->history,
        impl_->store,
        std::move(processed.expanded_prompt),
        std::move(combined_sink));

    if (!run_result.success) {
        result.success = false;
        result.code = run_result.code;
        result.message = run_result.message;
        result.last_assistant_text = last_assistant_text_from(impl_->history);
        result.message_count = impl_->history.size();
        impl_->state = AgentSession::Impl::State::Open;
        return result;
    }

    result.success = true;
    result.code = run_result.code;
    result.message = run_result.message;
    result.last_assistant_text = last_assistant_text_from(impl_->history);
    result.message_count = impl_->history.size();
    impl_->state = AgentSession::Impl::State::Open;
    return result;
}

util::Expected<EventSubscription> AgentSession::subscribe(agent::AgentEventSink sink) {
    if (!impl_) {
        return std::unexpected(util::make_error(util::ErrorCode::Validation, "session is not initialized"));
    }
    if (impl_->state == AgentSession::Impl::State::Closed) {
        return std::unexpected(util::make_error(util::ErrorCode::Validation, "session is closed"));
    }
    if (!sink) {
        return std::unexpected(util::make_error(util::ErrorCode::Validation, "event sink is empty"));
    }

    int index = impl_->next_subscriber_id++;
    impl_->subscribers.push_back({std::move(sink), true});

    auto sub_impl = std::make_unique<EventSubscription::Impl>();
    sub_impl->subscriber_index = index;
    sub_impl->session = impl_.get();

    EventSubscription sub;
    sub.impl_ = std::move(sub_impl);
    return sub;
}

std::size_t AgentSession::message_count() const {
    return impl_ ? impl_->history.size() : 0;
}

std::optional<std::string> AgentSession::last_assistant_text() const {
    return impl_ ? last_assistant_text_from(impl_->history) : std::nullopt;
}

const std::string& AgentSession::session_id() const {
    static const std::string empty;
    return impl_ ? impl_->session_id : empty;
}

const std::filesystem::path& AgentSession::session_path() const {
    static const std::filesystem::path empty;
    return impl_ ? impl_->session_file_path : empty;
}

const std::string& AgentSession::provider() const {
    static const std::string empty;
    return impl_ ? impl_->provider_name : empty;
}

const std::string& AgentSession::model() const {
    static const std::string empty;
    return impl_ ? impl_->model_name : empty;
}

const std::filesystem::path& AgentSession::workspace() const {
    static const std::filesystem::path empty;
    return impl_ ? impl_->workspace_path : empty;
}

util::ExpectedVoid AgentSession::close() {
    if (!impl_) return {};
    if (impl_->state == AgentSession::Impl::State::Closed) return {};

    impl_->state = AgentSession::Impl::State::Closed;

    // Clear subscribers
    for (auto& sub : impl_->subscribers) {
        sub.active = false;
    }
    impl_->subscribers.clear();

    // Release runner and capabilities
    impl_->runner.reset();
    impl_->chat_client.reset();

    // Clean up SDK-owned execution environment
    if (impl_->owns_execution_env && impl_->execution_env) {
        // Best-effort async cleanup; we run a minimal io_context
        boost::asio::io_context io;
        boost::asio::co_spawn(io, impl_->execution_env->cleanup(), boost::asio::detached);
        io.run();
        impl_->execution_env.reset();
    } else {
        impl_->execution_env.reset();
    }

    return {};
}

bool AgentSession::is_open() const {
    return impl_ && impl_->state != AgentSession::Impl::State::Closed;
}

bool AgentSession::is_busy() const {
    return impl_ && impl_->state == AgentSession::Impl::State::RunningPrompt;
}

const std::vector<Skill>& AgentSession::skills() const {
    static const std::vector<Skill> empty;
    return impl_ ? impl_->skills : empty;
}

const std::vector<PromptTemplate>& AgentSession::templates() const {
    static const std::vector<PromptTemplate> empty;
    return impl_ ? impl_->templates : empty;
}

// ─────────────────────────────────────────────────────────────────────────────
// Factory: create_agent_session
// ─────────────────────────────────────────────────────────────────────────────

util::Expected<CreateAgentSessionResult> create_agent_session(
    CreateAgentSessionOptions options) {
    std::vector<SdkDiagnostic> diagnostics;

    // ── Validate session target ──────────────────────────────────────────
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

    // ── Build chat client ────────────────────────────────────────────────
    bool has_host_client = options.chat_client != nullptr;
    auto client_result = build_chat_client(
        std::move(options.chat_client),
        options.provider_config,
        diagnostics);
    if (!client_result) {
        return std::unexpected(client_result.error());
    }
    auto chat_client = std::move(*client_result);

    // Determine provider/model metadata
    std::string provider_name;
    std::string model_name;
    if (has_host_client) {
        provider_name = kHostClientProvider;
        model_name = kHostClientModel;
    } else if (options.provider_config) {
        provider_name = options.provider_config->provider;
        model_name = options.provider_config->model;
    }

    // ── Open or resume session ───────────────────────────────────────────
    runtime::SessionOpenRequest session_req;
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

    auto open_result = runtime::open_session(session_req);
    if (!open_result) {
        return std::unexpected(open_result.error());
    }

    auto& open = *open_result;

    // For resumes, resolve provider/model from session metadata
    if (has_resume) {
        if (has_host_client) {
            // Host client: metadata is informational
            diagnostics.push_back(make_diag(
                SdkDiagnostic::Severity::Info,
                "host_client_resume",
                "Resumed session with host-provided chat client; stored provider/model metadata may differ"));
        } else if (open.stored_provider && open.stored_model) {
            // Use stored metadata for provider/model
            provider_name = *open.stored_provider;
            model_name = *open.stored_model;

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
        } else if (!options.provider_config) {
            return std::unexpected(util::make_error(
                util::ErrorCode::Session,
                "resumed session has no stored provider/model metadata and no provider_config supplied",
                "supply a provider_config or host chat_client when resuming sessions without stored metadata"));
        }
    }

    // ── Build execution environment ──────────────────────────────────────
    std::shared_ptr<harness::AsyncExecutionEnv> exec_env;
    bool owns_env = false;
    if (options.execution_env) {
        exec_env = options.execution_env;
    } else {
        exec_env = std::make_shared<harness::AsyncLocalExecutionEnv>(
            open.workspace,
            options.builtin_tools.bash,
            options.provider_config
                ? std::vector<std::string>{options.provider_config->api_key_env.value_or("")}
                : std::vector<std::string>{});
        owns_env = true;
    }

    // ── Build tool registry ──────────────────────────────────────────────
    agent::AsyncToolRegistry tools;

    // Collect built-in tool names for collision detection
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

    // Check custom tools for duplicates and collisions with built-ins
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

    // Register custom tools
    for (auto& tool : options.custom_tools) {
        if (tool) {
            if (auto added = tools.add(std::move(tool)); !added) {
                return std::unexpected(added.error());
            }
        }
    }

    // ── Assemble resources ───────────────────────────────────────────────
    std::vector<Skill> skills = options.skills;
    std::vector<PromptTemplate> templates = options.prompt_templates;

    // Build command registry from host commands
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

    // ── Optional project resource loading ────────────────────────────────
    if (options.load_project_resources) {
        auto fs = harness::WorkspaceFileSystem::create(open.workspace);
        if (fs) {
            // Trust resolution
            auto default_trust = options.default_project_trust.value_or(DefaultProjectTrust::Ask);
            auto skill_enablement = options.project_skills_enablement.value_or(ResourceEnablement::Auto);

            ProjectTrustStore trust_store(
                open.workspace / ".cpp-harness" / "trust.json");

            ProjectResourcePolicy policy;
            policy.project_skills = skill_enablement;

            auto detection = detect_project_resources(*fs);
            bool needs_trust = needs_project_trust_resolution(detection, policy);

            // Resolve trust (non-interactive: Ask → Untrusted)
            auto trust_resolution = resolve_project_trust(
                open.workspace,
                needs_trust,
                trust_store,
                default_trust,
                std::nullopt); // no CLI override

            auto load_plan = build_project_resource_load_plan(
                detection, policy, trust_resolution);

            // Add trust diagnostics
            for (const auto& td : trust_resolution.diagnostics) {
                auto sev = SdkDiagnostic::Severity::Warning;
                if (td.severity == ProjectTrustDiagnosticSeverity::Error) {
                    sev = SdkDiagnostic::Severity::Error;
                } else if (td.severity == ProjectTrustDiagnosticSeverity::Info) {
                    sev = SdkDiagnostic::Severity::Info;
                }
                diagnostics.push_back(make_diag(sev, td.code, td.message, td.path));
            }

            // Add resource diagnostics
            for (const auto& rd : load_plan.diagnostics) {
                auto sev = SdkDiagnostic::Severity::Warning;
                if (rd.severity == ResourceDiagnosticSeverity::Error) {
                    sev = SdkDiagnostic::Severity::Error;
                } else if (rd.severity == ResourceDiagnosticSeverity::Info) {
                    sev = SdkDiagnostic::Severity::Info;
                }
                diagnostics.push_back(make_diag(sev, rd.code, rd.message, rd.path));
            }

            // Load project skills if allowed
            if (project_skills_allowed(load_plan)) {
                std::vector<SkillDirSpec> skill_dirs;
                auto skills_dir = open.workspace / ".cpp-harness" / "skills";
                skill_dirs.push_back({.path = skills_dir.string(), .includeRootFiles = false});

                auto skill_load = loadSkills(*fs, skill_dirs);

                // Merge: host skills win over project-discovered duplicates
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

                // Forward skill load diagnostics
                for (const auto& sd : skill_load.diagnostics) {
                    diagnostics.push_back(make_diag(
                        SdkDiagnostic::Severity::Warning,
                        "skill_load",
                        sd.message,
                        sd.path));
                }
            }

            // Load project prompt templates if allowed
            if (project_prompts_allowed(load_plan)) {
                std::vector<PromptTemplateDirSpec> prompt_specs;
                auto prompts_dir = open.workspace / ".cpp-harness" / "prompts";
                prompt_specs.push_back({.path = prompts_dir.string(), .is_file = false});

                auto prompt_load = loadPromptTemplates(*fs, prompt_specs);

                // Merge: host templates win over project-discovered duplicates
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

                // Forward template load diagnostics
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

    // ── Build runner ─────────────────────────────────────────────────────
    auto runner_result = build_runner(
        *chat_client,
        std::move(tools),
        templates,
        command_registry,
        skills,
        options.max_turns);
    if (!runner_result) {
        return std::unexpected(runner_result.error());
    }

    // ── Assemble session impl ────────────────────────────────────────────
    auto impl = std::make_unique<AgentSession::Impl>();
    impl->state = AgentSession::Impl::State::Open;
    impl->store = std::move(open.store);
    impl->history = std::move(open.history);
    impl->workspace_path = open.workspace;
    impl->chat_client = std::move(chat_client);
    impl->execution_env = std::move(exec_env);
    impl->owns_execution_env = owns_env;
    impl->runner = std::move(*runner_result);
    impl->skills = std::move(skills);
    impl->templates = std::move(templates);
    impl->command_registry = std::move(command_registry);
    impl->session_id = impl->store.metadata().session_id;
    impl->provider_name = provider_name;
    impl->model_name = model_name;
    impl->session_file_path = impl->store.path();

    // ── Build result ─────────────────────────────────────────────────────
    CreateAgentSessionResult result;
    result.diagnostics = std::move(diagnostics);
    result.session_id = impl->session_id;
    result.provider = impl->provider_name;
    result.model = impl->model_name;
    result.session_path = impl->session_file_path;
    result.workspace = impl->workspace_path;

    auto session = std::make_unique<AgentSession>();
    session->impl_ = std::move(impl);
    result.session = std::move(session);

    return result;
}

} // namespace cch::coding_agent
