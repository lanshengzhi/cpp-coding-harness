#include "SessionEventCommitment.hpp"

#include <cch/agent/harness/session/SessionStore.hpp>

#include "SessionPersistence.hpp"

#include <utility>
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
    std::shared_ptr<SessionPersistence> persistence,
    std::shared_ptr<harness::session::SessionStore> store)
    : persistence_(std::move(persistence)), store_(std::move(store)) {}

agent::AgentEventCommitter SessionEventCommitment::sink() {
    return [this](const agent::AgentLifecycleEvent& event) -> support::ExpectedVoid {
        const auto* end = std::get_if<agent::MessageEndEvent>(&event);
        if (end == nullptr || !is_incrementally_persisted_message(end->message)) {
            return {};
        }
        if (persistence_) {
            return persistence_->submit_message_append(end->message);
        }
        // In-memory sessions commit directly into the store's live tree
        // (no disk I/O, so no channel); persisted stores without a channel
        // and store-less sessions keep the successful no-op.
        if (store_ && !store_->path()) {
            return store_->append(end->message);
        }
        return {};
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
