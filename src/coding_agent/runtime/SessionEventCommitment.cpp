#include "SessionEventCommitment.hpp"

#include "SessionPersistence.hpp"

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
    std::shared_ptr<SessionPersistence> persistence)
    : persistence_(std::move(persistence)) {}

agent::AgentEventCommitter SessionEventCommitment::sink() {
    return [this](const agent::AgentLifecycleEvent& event) -> support::ExpectedVoid {
        if (!persistence_) {
            return {};
        }
        const auto* end = std::get_if<agent::MessageEndEvent>(&event);
        if (end == nullptr || !is_incrementally_persisted_message(end->message)) {
            return {};
        }
        return persistence_->submit_message_append(end->message);
    };
}

boost::asio::awaitable<support::ExpectedVoid> SessionEventCommitment::conclude(
    std::optional<support::ExpectedVoid> agent_result) {
    if (persistence_) {
        co_await persistence_->drain();
        if (auto failure = persistence_->failure()) {
            co_return std::unexpected(std::move(*failure));
        }
    }
    if (!agent_result) {
        co_return std::unexpected(support::make_error(
            support::ErrorCode::Unknown,
            "stateful Agent prompt did not finish"));
    }
    co_return *agent_result;
}

} // namespace cch::coding_agent::runtime
