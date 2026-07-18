#pragma once

#include "../../include/cch/coding_agent/Settings.hpp"

#include <optional>
#include <string>
#include <vector>

namespace cch::coding_agent {

/// Generic explicit provider request used by both CLI and SDK creation paths.
/// Unset fields fall through to configuration and provider defaults.
struct ProviderRequest {
    std::optional<std::string> provider;
    std::optional<std::string> model;
    std::optional<std::string> base_url;
    std::optional<std::vector<std::string>> api_key_env;
    std::optional<std::string> auth;
};

/// Resolve provider settings from explicit request, stored session metadata, and
/// user settings. The provider registry name is always explicit (e.g.
/// "fake", "openai-compatible", or the host-client sentinel).
[[nodiscard]] ResolvedProviderSettings resolve_provider_settings(
    std::string_view provider_registry_name,
    const ProviderRequest& explicit_request,
    const UserSettings& settings,
    const std::optional<std::string>& stored_provider,
    const std::optional<std::string>& stored_model);

} // namespace cch::coding_agent
