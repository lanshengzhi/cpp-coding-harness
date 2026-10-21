#pragma once

#include "ai/api/PreparedStreamOptions.hpp"
#include <cch/ai/Context.hpp>
#include <cch/ai/Model.hpp>
#include <cch/support/Error.hpp>
#include <cch/support/JsonValue.hpp>

namespace cch::ai::api {

/// Validate the Model and build the gateway's Chat Completions wire payload
/// from normalized history and prepared stream options. Model validation
/// errors are returned unchanged.
[[nodiscard]] support::Expected<support::JsonValue> build_completions_payload(
        const Model& model, const AiContext& context, const PreparedStreamOptions& options);

} // namespace cch::ai::api
