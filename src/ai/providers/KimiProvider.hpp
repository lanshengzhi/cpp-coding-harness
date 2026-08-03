#pragma once

#include <cch/ai/Provider.hpp>
#include <cch/ai/providers/StreamTransport.hpp>

#include <memory>

namespace cch::ai::providers {

/// Private Kimi composition seam. The caller supplies request-time auth (the
/// api-key shape or `auth::make_kimi_coding_oauth_auth`); #345 makes the
/// catalog user-reachable.
[[nodiscard]] std::shared_ptr<ai::Provider> make_kimi_coding_provider(
    ProviderAuth auth,
    std::shared_ptr<StreamTransport> transport);

} // namespace cch::ai::providers
