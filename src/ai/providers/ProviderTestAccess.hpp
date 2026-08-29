#pragma once

#include <cch/ai/Models.hpp>
#include "ai/providers/Provider.hpp"
#include "ai/providers/StreamTransport.hpp"
#include "ai/providers/WebSocketTransport.hpp"

#include <memory>

namespace cch::ai::providers {

/// Private test-support access for installing a physical Provider behind the
/// Models seam. Production callers submit ProviderDefinition values instead.
struct ProviderTestAccess final {
    [[nodiscard]] static support::ExpectedVoid install(ai::Models& models, std::shared_ptr<ai::Provider> provider);
    [[nodiscard]] static support::ExpectedVoid replace_transports(ai::Models& models,
            std::shared_ptr<StreamTransport> http_transport,
            std::shared_ptr<WebSocketTransport> ws_transport,
            CodexWebSocketCacheConfig cache_config);
};

} // namespace cch::ai::providers
