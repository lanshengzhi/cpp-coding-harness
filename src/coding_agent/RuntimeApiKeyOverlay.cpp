#include "RuntimeApiKeyOverlay.hpp"

#include <algorithm>
#include <expected>
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

cch::support::AsyncResult<std::optional<ai::Credential>>
RuntimeApiKeyOverlay::read(std::string provider_id) {
    {
        std::scoped_lock lock(mutex_);
        if (const auto found = overrides_.find(provider_id);
            found != overrides_.end()) {
            ai::ApiKeyCredential credential{.key = found->second};
            return cch::support::AsyncResult<std::optional<ai::Credential>>(
                std::expected<std::optional<ai::Credential>, cch::support::Error>{
                    std::optional<ai::Credential>{std::move(credential)}});
        }
    }
    return base_->read(std::move(provider_id));
}

cch::support::AsyncResult<std::vector<ai::CredentialInfo>>
RuntimeApiKeyOverlay::list() {
    std::vector<std::string> runtime = runtime_api_key_providers();
    // Weak capture: the producer must not hold the last strong reference to
    // the backing store, which could otherwise be destroyed on the store's own
    // worker thread and self-join during teardown (ADR 0040 / #454).
    std::weak_ptr<ai::CredentialStore> base = base_;
    return cch::support::AsyncResult<std::vector<ai::CredentialInfo>>(
        [base = std::move(base), runtime = std::move(runtime)](
            cch::support::AsyncCompletion<std::vector<ai::CredentialInfo>, cch::support::Error> completion) mutable noexcept {
            auto locked = base.lock();
            if (!locked) {
                completion(std::unexpected(support::make_error(
                    support::ErrorCode::Unknown,
                    "credential store is unavailable")));
                return;
            }
            locked->list().start(
                [completion = std::move(completion), runtime = std::move(runtime)](
                    std::expected<std::vector<ai::CredentialInfo>, cch::support::Error> entries) mutable noexcept {
                    if (!entries) {
                        completion(std::move(entries));
                        return;
                    }
                    for (const auto& provider_id : runtime) {
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
                    completion(std::move(entries));
                });
        });
}

cch::support::AsyncResult<std::optional<ai::Credential>>
RuntimeApiKeyOverlay::modify(
    std::string provider_id,
    ai::CredentialModifyHook modifier) {
    return base_->modify(std::move(provider_id), std::move(modifier));
}

cch::support::AsyncResult<void> RuntimeApiKeyOverlay::remove(
    std::string provider_id) {
    remove_runtime_api_key(provider_id);
    return base_->remove(std::move(provider_id));
}

} // namespace cch::coding_agent
