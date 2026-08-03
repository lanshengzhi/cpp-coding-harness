#pragma once

#include <cch/ai/Context.hpp>
#include <cch/ai/Model.hpp>
#include <cch/ai/Provider.hpp>
#include <cch/util/Error.hpp>
#include <cch/util/JsonValue.hpp>

namespace cch::ai::api {

enum class AdapterKind {
    OpenAICodexResponses,
    OpenAIResponses,
    AnthropicMessages,
};

/// Build one transport-independent wire payload using the shared normalized
/// history and streamSimple values. No provider SDK DTO crosses this boundary.
[[nodiscard]] util::Expected<util::JsonValue> build_adapter_payload(
    AdapterKind adapter,
    const Model& model,
    const AiContext& context,
    const ProviderStreamOptions& options);

} // namespace cch::ai::api
