#include "RuntimeApiKeyOverlay.hpp"

#include <algorithm>
#include <utility>

namespace cch::coding_agent {

RuntimeApiKeyOverlay::RuntimeApiKeyOverlay(std::shared_ptr<ai::CredentialStore> base)
    : base_(std::move(base)) {}

void RuntimeApiKeyOverlay::set_runtime_api_key(
    std::string provider_id,
    std::string api_key) {
    std::scoped_lock lock(mutex_);
    overrides_[std::move(provider_id)] = std::move(api_key);
}

void RuntimeApiKeyOverlay::remove_runtime_api_key(std::string_view provider_id) {
    std::scoped_lock lock(mutex_);
    overrides_.erase(std::string{provider_id});
}

bool RuntimeApiKeyOverlay::has_runtime_api_key(std::string_view provider_id) const {
    std::scoped_lock lock(mutex_);
    return overrides_.contains(std::string{provider_id});
}

std::vector<std::string> RuntimeApiKeyOverlay::runtime_api_key_providers() const {
    std::vector<std::string> providers;
    std::scoped_lock lock(mutex_);
    providers.reserve(overrides_.size());
    for (const auto& [provider_id, _] : overrides_) {
        providers.push_back(provider_id);
    }
    return providers;
}

boost::asio::awaitable<util::Expected<std::optional<ai::Credential>>>
RuntimeApiKeyOverlay::read(std::string provider_id) {
    {
        std::scoped_lock lock(mutex_);
        if (const auto found = overrides_.find(provider_id);
            found != overrides_.end()) {
            ai::ApiKeyCredential credential{.key = found->second};
            co_return std::optional<ai::Credential>{std::move(credential)};
        }
    }
    co_return co_await base_->read(std::move(provider_id));
}

boost::asio::awaitable<util::Expected<std::vector<ai::CredentialInfo>>>
RuntimeApiKeyOverlay::list() {
    auto entries = co_await base_->list();
    if (!entries) {
        co_return entries;
    }
    std::vector<std::string> runtime_providers = runtime_api_key_providers();
    for (const auto& provider_id : runtime_providers) {
        const auto existing = std::find_if(
            entries->begin(),
            entries->end(),
            [&provider_id](const ai::CredentialInfo& entry) {
                return entry.provider_id == provider_id;
            });
        if (existing != entries->end()) {
            existing->type = "api_key";
        } else {
            entries->push_back(ai::CredentialInfo{
                .provider_id = provider_id,
                .type = "api_key",
            });
        }
    }
    co_return entries;
}

boost::asio::awaitable<util::Expected<std::optional<ai::Credential>>>
RuntimeApiKeyOverlay::modify(
    std::string provider_id,
    ai::CredentialModifyHook modifier) {
    co_return co_await base_->modify(std::move(provider_id), std::move(modifier));
}

boost::asio::awaitable<util::ExpectedVoid> RuntimeApiKeyOverlay::remove(
    std::string provider_id) {
    remove_runtime_api_key(provider_id);
    co_return co_await base_->remove(std::move(provider_id));
}

} // namespace cch::coding_agent
