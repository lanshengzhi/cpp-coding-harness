#pragma once

#include "MessageConversion.hpp"

namespace cch::ai::api {

/// Internal wire payload builder, split out from the public conversion entry
/// point so each family lives in its own translation unit.

[[nodiscard]] support::Expected<support::JsonValue> build_completions_wire_payload(
        const Model& model, const AiContext& context, const PreparedStreamOptions& options);

} // namespace cch::ai::api
