#include <cch/ai/Models.hpp>

#include "ai/auth/KimiCodingOAuth.hpp"
#include "ai/auth/OpenAICodexOAuth.hpp"
#include "ai/providers/CodexCatalog.hpp"
#include "ai/providers/EnvApiKeyAuth.hpp"
#include "ai/providers/KimiCatalog.hpp"

#include <utility>
#include <vector>

namespace cch::ai {

std::vector<ProviderDefinition> builtin_provider_definitions() {
    std::vector<ProviderDefinition> definitions;
    definitions.reserve(2);

    ProviderAuth codex_auth;
    codex_auth.oauth = auth::make_openai_codex_oauth_auth();
    definitions.push_back(ProviderDefinition{
            .id = "openai-codex",
            .name = "OpenAI Codex",
            .models = providers::codex_models(),
            .auth = std::move(codex_auth),
    });

    ProviderAuth kimi_auth;
    auto kimi_api_key = providers::make_env_api_key_auth(
            "Kimi API key", {"KIMI_API_KEY"});
    kimi_auth.api_key = std::move(*kimi_api_key.api_key);
    kimi_auth.oauth = auth::make_kimi_coding_oauth_auth();
    definitions.push_back(ProviderDefinition{
            .id = "kimi-coding",
            .name = "Kimi For Coding",
            .models = providers::kimi_coding_models(),
            .auth = std::move(kimi_auth),
    });

    return definitions;
}

} // namespace cch::ai
