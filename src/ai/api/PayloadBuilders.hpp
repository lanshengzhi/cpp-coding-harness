#pragma once

#include "MessageConversion.hpp"

namespace cch::ai::api {

/// The two wire payload builders behind build_adapter_payload; split out so
/// each family lives in its own translation unit.

[[nodiscard]] support::Expected<support::JsonValue> build_responses_payload(
        AdapterKind adapter, const Model& model, const AiContext& context, const ProviderStreamOptions& options);

[[nodiscard]] support::Expected<support::JsonValue> build_anthropic_payload(
        const Model& model, const AiContext& context, const ProviderStreamOptions& options);

[[nodiscard]] support::Expected<support::JsonValue> build_completions_payload(
        const Model& model, const AiContext& context, const ProviderStreamOptions& options);

} // namespace cch::ai::api
