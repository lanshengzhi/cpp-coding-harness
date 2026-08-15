#pragma once

#include <cch/ai/CredentialStore.hpp>
#include <cch/support/Error.hpp>

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace cch::coding_agent {

/// In-memory runtime API key overlay on a CredentialStore (pi
/// `RuntimeCredentials`). `read`/`list` consult process-lifetime overrides
/// before the backing store, giving runtime API keys the highest precedence in
/// Request Authentication without persisting anything. `modify`/`remove`
/// delegate to the backing store so OAuth login and logout persist normally.
class RuntimeApiKeyOverlay final : public ai::CredentialStore {
public:
    explicit RuntimeApiKeyOverlay(std::shared_ptr<ai::CredentialStore> base);

    void set_runtime_api_key(std::string provider_id, std::string api_key);
    void remove_runtime_api_key(std::string_view provider_id);
    [[nodiscard]] bool has_runtime_api_key(std::string_view provider_id) const;
    [[nodiscard]] std::vector<std::string> runtime_api_key_providers() const;

    // ── CredentialStore ──────────────────────────────────────────────────

    [[nodiscard]] cch::support::AsyncResult<std::optional<ai::Credential>> read(
        std::string provider_id) override;
    [[nodiscard]] cch::support::AsyncResult<std::vector<ai::CredentialInfo>> list() override;
    [[nodiscard]] cch::support::AsyncResult<std::optional<ai::Credential>> modify(
        std::string provider_id,
        ai::CredentialModifyHook modifier) override;
    [[nodiscard]] cch::support::AsyncResult<void> remove(
        std::string provider_id) override;

private:
    std::shared_ptr<ai::CredentialStore> base_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::string> overrides_;
};

} // namespace cch::coding_agent
