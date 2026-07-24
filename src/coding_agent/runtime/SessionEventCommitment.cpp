#include "SessionEventCommitment.hpp"

#include <variant>

namespace cch::coding_agent::runtime {
namespace {

[[nodiscard]] bool is_incrementally_persisted_message(const ai::MessageVariant& message) {
    return std::holds_alternative<ai::UserMessage>(message) ||
           std::holds_alternative<ai::AssistantMessage>(message) ||
           std::holds_alternative<ai::ToolResultMessage>(message);
}

} // namespace

SessionEventCommitment::SessionEventCommitment(
    harness::session::SessionStore& store)
    : store_(store) {}

agent::AgentEventCommitter SessionEventCommitment::sink() {
    return [this](const agent::AgentLifecycleEvent& event) -> util::ExpectedVoid {
        const auto* end = std::get_if<agent::MessageEndEvent>(&event);
        if (end == nullptr || !is_incrementally_persisted_message(end->message)) {
            return {};
        }

        auto appended = store_.append(end->message);
        if (!appended && !failure_) {
            failure_ = appended.error();
        }
        return appended;
    };
}

util::ExpectedVoid SessionEventCommitment::conclude(
    std::optional<util::ExpectedVoid> agent_result) const {
    if (failure_) {
        return std::unexpected(*failure_);
    }
    if (!agent_result) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Unknown,
            "stateful Agent prompt did not finish"));
    }
    return *agent_result;
}

} // namespace cch::coding_agent::runtime
