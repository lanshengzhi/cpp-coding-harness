#pragma once

#include <cch/ai/Context.hpp>
#include <cch/ai/Model.hpp>
#include "PreparedStreamOptions.hpp"
#include <cch/support/Error.hpp>
#include <cch/support/JsonValue.hpp>

namespace cch::ai::api {

/// Build the OpenAI Chat Completions wire payload using normalized history
/// and the prepared stream options. No provider SDK DTO crosses this boundary.
[[nodiscard]] support::Expected<support::JsonValue> build_openai_completions_payload(
        const Model& model, const AiContext& context, const PreparedStreamOptions& options);

} // namespace cch::ai::api
