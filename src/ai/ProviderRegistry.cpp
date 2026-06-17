#include "../../include/cch/ai/ProviderRegistry.hpp"

#include "../ai/providers/FakeChatClient.hpp"
#include "../../include/cch/ai/providers/BoostBeastStreamTransport.hpp"
#include "../../include/cch/ai/providers/OpenAIChatClient.hpp"

#include <memory>
#include <utility>

namespace cch::ai {

util::ExpectedVoid ProviderRegistry::register_provider(std::string name, ProviderFactory factory) {
    if (name.empty()) {
        return std::unexpected(util::make_error(util::ErrorCode::Provider, "provider name is required"));
    }
    if (!factory) {
        return std::unexpected(util::make_error(util::ErrorCode::Provider, "provider factory is required", name));
    }
    const auto [_, inserted] = factories_.emplace(std::move(name), std::move(factory));
    if (!inserted) {
        return std::unexpected(util::make_error(util::ErrorCode::Provider, "provider is already registered"));
    }
    return {};
}

ProviderFactoryResult ProviderRegistry::create(std::string_view name, const ProviderFactoryContext& context) const {
    const auto found = factories_.find(name);
    if (found == factories_.end()) {
        return std::unexpected(util::make_error(util::ErrorCode::Provider, "unknown provider", std::string{name}));
    }
    return found->second(context);
}

bool ProviderRegistry::contains(std::string_view name) const {
    return factories_.find(name) != factories_.end();
}

std::vector<std::string> ProviderRegistry::provider_names() const {
    std::vector<std::string> names;
    names.reserve(factories_.size());
    for (const auto& [name, _] : factories_) {
        names.push_back(name);
    }
    return names;
}

util::Expected<ProviderRegistry> make_default_provider_registry() {
    ProviderRegistry registry;

    auto openai_registered = registry.register_provider(
        "openai-compatible",
        [](const ProviderFactoryContext& context) -> ProviderFactoryResult {
            providers::OpenAIStreamConfig config;
            config.base_url = context.base_url;
            config.api_key = context.api_key;
            config.api_key_env = context.api_key_env;
            config.model = context.model;
            config.timeout = context.timeout;
            auto transport = std::make_shared<providers::BoostBeastStreamTransport>();
            std::unique_ptr<StreamingChatClient> client =
                std::make_unique<providers::StreamingOpenAIChatClient>(transport, std::move(config));
            return client;
        });
    if (!openai_registered) {
        return std::unexpected(openai_registered.error());
    }

    auto fake_registered = registry.register_provider(
        "fake",
        [](const ProviderFactoryContext&) -> ProviderFactoryResult {
            return providers::make_scripted_fake_chat_client();
        });
    if (!fake_registered) {
        return std::unexpected(fake_registered.error());
    }

    return registry;
}

} // namespace cch::ai
