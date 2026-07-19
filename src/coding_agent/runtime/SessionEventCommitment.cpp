#include "SessionEventCommitment.hpp"

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
    std::vector<ai::MessageVariant>& live_history,
    std::deque<SubscriberEntry>& subscribers,
    harness::session::SessionStore& store)
    : live_history_(live_history), store_(store) {
    for (auto& sub : subscribers) {
        if (sub.active && sub.sink) {
            snapshot_.push_back(&sub.sink);
        }
    }
}

agent::AgentEventSink SessionEventCommitment::sink() {
    return [this](const agent::AgentLifecycleEvent& event) -> util::ExpectedVoid {
        // Advance Live Session State before any subscriber observes the
        // completed message. This matches pi's state-first event ordering.
        const auto* end = std::get_if<agent::MessageEndEvent>(&event);
        if (end != nullptr) {
            live_history_.push_back(end->message);
        }
        for (auto* subscriber : snapshot_) {
            if (*subscriber) {
                auto delivered = (*subscriber)(event);
                if (!delivered) {
                    failure_ = delivered.error();
                    return delivered;
                }
            }
        }
        if (end != nullptr && is_incrementally_persisted_message(end->message)) {
            auto appended = store_.append(end->message);
            if (!appended) {
                failure_ = appended.error();
                return std::unexpected(appended.error());
            }
        }
        return {};
    };
}

util::ExpectedVoid SessionEventCommitment::conclude(
    std::optional<util::Expected<agent::AsyncAgentRunResult>> loop_result) const {
    if (failure_) {
        return std::unexpected(*failure_);
    }
    if (!loop_result) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Unknown,
            "async loop did not finish"));
    }
    if (!*loop_result) {
        return std::unexpected((*loop_result).error());
    }
    return {};
}

} // namespace cch::coding_agent::runtime
