#include "MessageConversion.hpp"

#include "PayloadBuilders.hpp"

namespace cch::ai::api {

support::Expected<support::JsonValue> build_openai_completions_payload(
        const Model& model, const AiContext& context, const PreparedStreamOptions& options) {
    if (auto valid = validate_model(model); !valid) {
        return std::unexpected(valid.error());
    }
    return build_completions_wire_payload(model, context, options);
}

} // namespace cch::ai::api
