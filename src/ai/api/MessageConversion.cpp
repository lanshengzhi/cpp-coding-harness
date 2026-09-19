#include "MessageConversion.hpp"

#include "PayloadBuilders.hpp"

namespace cch::ai::api {

support::Expected<support::JsonValue> build_adapter_payload(
    AdapterKind adapter,
    const Model& model,
    const AiContext& context,
    const ProviderStreamOptions& options) {
    if (adapter == AdapterKind::AnthropicMessages) {
        return build_anthropic_payload(model, context, options);
    }
    return build_responses_payload(adapter, model, context, options);
}

} // namespace cch::ai::api
