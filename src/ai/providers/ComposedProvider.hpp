#pragma once

#include <cch/ai/Auth.hpp>
#include <cch/ai/Model.hpp>
#include "ai/providers/Provider.hpp"
#include "ai/providers/StreamTransport.hpp"
#include "ai/providers/WebSocketTransport.hpp"

#include <memory>
#include <string>
#include <vector>

namespace cch::ai::providers {

/// Unified provider execution engine composing models, authentication, and
/// the three supported private wire adapters behind the abstract Provider interface.
[[nodiscard]] std::shared_ptr<ai::Provider> make_composed_provider(
    std::string provider_id,
    std::string name,
    std::vector<ai::Model> models,
    ai::ProviderAuth auth,
    std::shared_ptr<StreamTransport> http_transport,
    std::shared_ptr<WebSocketTransport> ws_transport = nullptr,
    CodexWebSocketCacheConfig cache_config = {});

} // namespace cch::ai::providers
