#pragma once

#include <cch/ai/Models.hpp>
#include "ai/providers/Provider.hpp"
#include "ai/providers/StreamTransport.hpp"
#include "ai/providers/WebSocketTransport.hpp"

#include <memory>

namespace cch::ai::providers {

/// Private test-support access for installing a physical Provider behind the
/// Models seam. Production callers submit ProviderDefinition values instead.
///
/// debt: this header is reachable from production code and `cch_ai`'s
/// definitions compile into the library, so the shipped object files carry two
/// symbols whose only callers are tests. The repository's global
/// `-fvisibility=hidden` and the STATIC library kind keep them out of any
/// exported surface, but they are still compiled and linked.
/// Closing this needs a real privileged seam on `Models` for injecting a
/// Provider or transport; none exists, because `apply_provider` builds its own
/// transport. Upgrade when a second test-support accessor is wanted, or when
/// `Models` grows an injectable transport seam for another reason.
struct ProviderTestAccess final {
    [[nodiscard]] static support::ExpectedVoid install(ai::Models& models, std::shared_ptr<ai::Provider> provider);
    [[nodiscard]] static support::ExpectedVoid replace_transports(ai::Models& models,
            std::shared_ptr<StreamTransport> http_transport,
            std::shared_ptr<WebSocketTransport> ws_transport,
            CodexWebSocketCacheConfig cache_config);
};

} // namespace cch::ai::providers
