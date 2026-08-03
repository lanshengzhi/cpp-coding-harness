#pragma once

#include <cch/ai/Model.hpp>
#include <cch/ai/Provider.hpp>
#include <cch/ai/providers/StreamTransport.hpp>
#include "ai/providers/OpenAIChatClient.hpp"

#include <memory>
#include <string>
#include <vector>

namespace cch::ai::providers {

/// Compose the transitional OpenAI-compatible Provider around the existing
/// private wire adapter. Authentication is resolved by Models for every call.
[[nodiscard]] std::shared_ptr<ai::Provider> make_openai_compatible_provider(
    std::string provider_id,
    std::vector<ai::Model> models,
    std::vector<std::string> api_key_env,
    std::shared_ptr<StreamTransport> transport,
    OpenAIStreamConfig config = {});

} // namespace cch::ai::providers
