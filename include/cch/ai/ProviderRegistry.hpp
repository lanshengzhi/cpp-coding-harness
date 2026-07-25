#pragma once

#include "ChatClient.hpp"
#include "providers/OpenAICompletionsCompat.hpp"

#include "../util/Error.hpp"

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace cch::ai {

// ProviderFactoryContext captures static construction-time provider configuration.
// OAuth, subscription-provider, and dynamic per-call API-key resolution are not
// supported yet; api_key_env only reads a static environment variable.
struct ProviderFactoryContext {
    // Registry key used to construct the adapter. This may differ from provider
    // identity for OpenAI-compatible custom providers.
    std::string provider_registry_name{"openai-compatible"};
    // Provider/API identity written into assistant messages.
    std::string provider{"openai-compatible"};
    std::string api{"openai-completions"};
    // Configured default model identity; the request's Model takes precedence
    // (see StreamChatRequest::model, ADR 0019).
    Model model;
    std::string base_url{"https://api.openai.com"};
    std::string api_key;
    std::string api_key_env{"OPENAI_API_KEY"};
    std::chrono::milliseconds timeout{30000};
    cch::ai::providers::OpenAICompletionsCompat openai_compat;
};

using ProviderClient = std::unique_ptr<StreamingChatClient>;
using ProviderFactoryResult = cch::util::Expected<ProviderClient>;
using ProviderFactory = std::function<ProviderFactoryResult(const ProviderFactoryContext&)>;

class ProviderRegistry {
public:
    [[nodiscard]] cch::util::ExpectedVoid register_provider(std::string name, ProviderFactory factory);
    [[nodiscard]] ProviderFactoryResult create(
        std::string_view name,
        const ProviderFactoryContext& context) const;
    [[nodiscard]] bool contains(std::string_view name) const;
    [[nodiscard]] std::vector<std::string> provider_names() const;

private:
    std::map<std::string, ProviderFactory, std::less<>> factories_;
};

[[nodiscard]] cch::util::Expected<ProviderRegistry> make_default_provider_registry();

} // namespace cch::ai
