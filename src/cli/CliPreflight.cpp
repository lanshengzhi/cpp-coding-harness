#include "CliPreflight.hpp"

#include "../../include/cch/coding_agent/Config.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <utility>

namespace cch::cli {
namespace {

cch::util::Error cli_error(std::string message) {
    return cch::util::make_error(cch::util::ErrorCode::Validation, message, message);
}

std::string timestamp_for_path() {
    auto now = std::chrono::system_clock::now();
    auto seconds = std::chrono::system_clock::to_time_t(now);
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &seconds);
#else
    localtime_r(&seconds, &tm);
#endif
    std::ostringstream out;
    out << std::put_time(&tm, "%Y%m%d-%H%M%S") << '-' << std::setw(3) << std::setfill('0') << milliseconds.count();
    return out.str();
}

std::string random_suffix() {
    std::random_device device;
    std::uniform_int_distribution<unsigned int> distribution(0, 0xFFFFu);
    std::ostringstream out;
    out << std::hex << std::setw(4) << std::setfill('0') << distribution(device);
    return out.str();
}

std::filesystem::path default_session_path() {
    return std::filesystem::current_path() / ".cpp-harness" / "sessions"
        / (timestamp_for_path() + "-" + random_suffix() + ".jsonl");
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
    if (!config.session_path.empty() && std::filesystem::exists(config.session_path, ec)) {
        return std::unexpected(cli_error("session file already exists; use --resume to append"));
    }
    if (config.fake) {
        return {};
    }

    auto config_data = coding_agent::ConfigLoader::load(coding_agent::ConfigLoader::default_config_path());
    if (!config_data) {
        config_data = coding_agent::ConfigData{};
    }

    const auto resolved = coding_agent::resolve_provider_settings(
        "openai-compatible",
        false,
        config.provider_overrides,
        *config_data,
        std::nullopt,
        std::nullopt);
    const auto chain = coding_agent::resolved_api_key_env_chain(config.provider_overrides, *config_data);
    if (!coding_agent::ConfigLoader::resolve_api_key(chain)) {
        return std::unexpected(cli_error(
            "missing API key; set " + resolved.api_key_env + " before real-provider mode"));
    }
    return {};
}

AsyncCliRuntimeConfig to_runtime_config(CliConfig config) {
    const auto run_timestamp = timestamp_for_path();
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
        .session_path = config.session_path.empty() ? default_session_path() : std::move(config.session_path),
        .resume_path = std::move(config.resume_path),
        .session_id = run_timestamp,
        .created_at = run_timestamp,
        .provider_overrides = std::move(config.provider_overrides),
        .prompt = std::move(config.prompt),
    };
}

void print_error(const cch::util::Error& error) {
    std::cerr << (error.detail.empty() ? error.message : error.detail) << '\n';
}

} // namespace cch::cli
