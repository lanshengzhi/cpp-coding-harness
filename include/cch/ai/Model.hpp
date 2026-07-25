#pragma once

#include <string>

namespace cch::ai {

/// First-class model identity (ADR 0019): the one value that answers "which
/// model" across the streaming request, the agent context, and the provider
/// configuration.
///
/// Precedence rule: the StreamChatRequest's Model is authoritative when
/// present; otherwise the provider client's configured Model applies. A call
/// with neither is rejected as a validation error. There is no third slot:
/// AiContext carries conversation state, not model identity.
struct Model {
    std::string id;
};

} // namespace cch::ai
