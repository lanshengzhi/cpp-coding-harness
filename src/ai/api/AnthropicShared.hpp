#pragma once

#include "ai/providers/Provider.hpp"
#include <cch/ai/StreamTransport.hpp>

#include <cch/ai/Context.hpp>
#include <cch/ai/Model.hpp>
#include <cch/support/Error.hpp>

namespace cch::ai::api {

/// One Anthropic Messages request: frozen payload plus URL and header
/// defaults. Split out of the adapter so `stream()` stays wiring-only.
[[nodiscard]] support::Expected<providers::StreamRequest> build_anthropic_stream_request(
        const Model& model, const AiContext& context, ProviderStreamOptions& options);

} // namespace cch::ai::api
