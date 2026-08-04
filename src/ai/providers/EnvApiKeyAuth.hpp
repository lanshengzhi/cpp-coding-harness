#pragma once

#include <cch/ai/Auth.hpp>

#include <string>
#include <vector>

namespace cch::ai::providers {

/// Env-chain ApiKeyAuth (pi `envApiKeyAuth`): resolves a stored credential,
/// then the first set environment variable in `environment_names`. Used by the
/// composed kimi-coding built-in and by test provider compositions.
[[nodiscard]] ai::ProviderAuth make_env_api_key_auth(
    std::string provider_name,
    std::vector<std::string> environment_names);

} // namespace cch::ai::providers
