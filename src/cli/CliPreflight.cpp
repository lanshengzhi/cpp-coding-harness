#include "CliPreflight.hpp"

#include "../../include/cch/coding_agent/AgentConfigDir.hpp"
#include "../../include/cch/coding_agent/Settings.hpp"

#include <filesystem>
#include <iostream>
#include <utility>

namespace cch::cli {
namespace {

cch::util::Error cli_error(std::string message) {
    return cch::util::make_error(cch::util::ErrorCode::Validation, message, message);
}

} // namespace

cch::util::ExpectedVoid validate_workspace(const std::filesystem::path& workspace) {
    std::error_code ec;
    if (!std::filesystem::exists(workspace, ec) || !std::filesystem::is_directory(workspace, ec)) {
        return std::unexpected(cli_error("invalid workspace path: " + workspace.string()));
    }
    return {};
}

std::filesystem::path canonical_workspace(const std::filesystem::path& workspace) {
    std::error_code ec;
    auto canonical = std::filesystem::weakly_canonical(workspace, ec);
    return ec ? workspace.lexically_normal() : canonical;
}

cch::util::ExpectedVoid preflight_cli_config(const CliConfig& config) {
    std::error_code ec;
    if (const auto* target = std::get_if<coding_agent::ExplicitNewSessionTarget>(&config.session_target);
        target != nullptr && std::filesystem::exists(target->path, ec)) {
        return std::unexpected(cli_error("session file already exists; use --resume to append"));
    }
    if (config.fake) {
        return {};
    }

    auto settings_data = coding_agent::SettingsLoader::load(coding_agent::settings_file_path());
    if (!settings_data) {
        settings_data = coding_agent::UserSettings{};
    }

    const auto resolved = coding_agent::resolve_provider_settings(
        "openai-compatible",
        false,
        config.provider_overrides,
        *settings_data,
        std::nullopt,
        std::nullopt);
    if (!resolved.api_key.empty()) {
        return {};
    }
    const auto chain = coding_agent::resolved_api_key_env_chain(config.provider_overrides, *settings_data);
    if (!coding_agent::SettingsLoader::resolve_api_key(chain)) {
        return std::unexpected(cli_error(
            "missing API key; set " + resolved.api_key_env + " or configure --auth"));
    }
    return {};
}

AsyncCliRuntimeConfig to_runtime_config(CliConfig config) {
    return AsyncCliRuntimeConfig{
        .fake = config.fake,
        .repl = config.repl,
        .enable_bash = config.enable_bash,
        .project_trust_override = config.project_trust_override,
        .disable_project_skills = config.no_skills,
        .disable_prompt_templates = config.no_prompt_templates,
        .prompt_template_paths = std::move(config.prompt_template_paths),
        .output_mode = config.output_mode,
        .max_turns = config.max_turns,
        .workspace_explicit = config.workspace_explicit,
        .workspace = std::move(config.workspace),
        .session_target = std::move(config.session_target),
        .session_dir = std::move(config.session_dir),
        .provider_overrides = std::move(config.provider_overrides),
        .prompt = std::move(config.prompt),
    };
}

void print_error(const cch::util::Error& error) {
    std::cerr << (error.detail.empty() ? error.message : error.detail) << '\n';
}

} // namespace cch::cli
