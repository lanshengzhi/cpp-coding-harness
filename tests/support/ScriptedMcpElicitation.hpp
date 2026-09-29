#pragma once

#include <cch/mcp/UpstreamElicitation.hpp>
#include <cch/support/AsyncResult.hpp>
#include <cch/support/Error.hpp>
#include "support/ScriptedMcpTransport.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace cch::tests {

/// One Pending Elicitation the scripted port was asked, kept exactly as the
/// MCP Host presented it, so a case can assert on what the user would have
/// been shown rather than on the host's internal state.
struct RecordedElicitation {
    mcp::ElicitationRequest request{};
    /// Whether the wait's stop token had been requested when the answer was
    /// scripted. A port that is asked after the session stopped is a wait that
    /// outlived its call.
    bool stop_requested{false};
};

/// The Pending Elicitation port a test drives, filled from memory.
///
/// It is a **fixture**, never a second production seam: the MCP Host reaches
/// the user through the one `mcp::UpstreamElicitationPort` value, and a test
/// supplies that value here. The port owns no protocol knowledge — it records
/// what it was asked and completes with what the test scripted, which is what
/// makes the assertions about the MRTR loop rather than about a dialog.
class ScriptedMcpElicitation final {
public:
    /// Answer every request inline with `action`, in the order the questions
    /// are asked. A queue shorter than the questions asked runs the last entry
    /// again, so a case only has to script what varies.
    void answer(mcp::ElicitationAction action) {
        answer_with([action](const mcp::ElicitationRequest&) { return answer_for(action); });
    }

    /// Answer from the request, for a case whose answer depends on what was
    /// asked (a URL versus a form, one Server Id versus another).
    void answer_with(std::function<support::Expected<mcp::ElicitationAnswer>(const mcp::ElicitationRequest&)> handler) {
        handler_ = std::move(handler);
        queue_.clear();
    }

    /// Answer the `index`-th question with `action` and leave every other
    /// question unanswered, which is how a case asks for a wait it can drive
    /// to its bound.
    void answer_question(std::size_t index, mcp::ElicitationAction action) {
        queue_.clear();
        queue_.push_back(answer_for(action));
        pending_from_ = index;
    }

    /// Fail the `index`-th question instead of answering it, as a port that
    /// could not ask the user would.
    void fail_question(std::size_t index, support::Error error) {
        queue_.clear();
        queue_.push_back(std::unexpected(std::move(error)));
        pending_from_ = index;
    }

    /// The port the MCP Host is configured with. `delay` bounds the wait, so a
    /// case drives the bound through the same `ScriptedMcpDelay` the
    /// connection's reconnect ladder and cleanup bound already use.
    [[nodiscard]] std::shared_ptr<mcp::UpstreamElicitationPort> port(
            std::chrono::milliseconds timeout = std::chrono::milliseconds{0}) {
        auto port = std::make_shared<mcp::UpstreamElicitationPort>();
        port->timeout = timeout;
        port->prompter = [this](mcp::ElicitationRequest request, std::stop_token stop_token) {
            asked_.push_back(
                    RecordedElicitation{.request = std::move(request), .stop_requested = stop_token.stop_requested()});
            return this->reply(asked_.size() - 1, stop_token);
        };
        return port;
    }

    /// The same port with the supplied timer, so one `ScriptedMcpDelay` bounds
    /// both the connection's own waits and the elicitation wait.
    [[nodiscard]] std::shared_ptr<mcp::UpstreamElicitationPort> port(
            std::chrono::milliseconds timeout, ScriptedMcpDelay& delay) {
        auto port = this->port(timeout);
        port->delay = [&delay](std::chrono::milliseconds requested, std::stop_token stop_token) {
            return delay.request(requested, stop_token);
        };
        return port;
    }

    [[nodiscard]] const std::vector<RecordedElicitation>& asked() const noexcept { return asked_; }

    [[nodiscard]] std::size_t question_count() const noexcept { return asked_.size(); }

    /// Complete the question at `index` now, as a user answering late would:
    /// after the wait was already settled. The loop must discard it.
    void answer_late(std::size_t index, mcp::ElicitationAction action) {
        if (index >= waits_.size()) {
            return;
        }
        if (auto completion = std::move(waits_[index].completion)) {
            completion(answer_for(action));
        }
    }

    /// Whether the question at `index` is still waiting for an answer.
    [[nodiscard]] bool waiting(std::size_t index) const noexcept {
        return index < waits_.size() && static_cast<bool>(waits_[index].completion);
    }

private:
    struct Wait {
        std::stop_token stop_token{};
        support::AsyncCompletion<mcp::ElicitationAnswer, support::Error> completion{};
    };

    static support::Expected<mcp::ElicitationAnswer> answer_for(mcp::ElicitationAction action) {
        return mcp::ElicitationAnswer{.action = action, .request_id = {}, .form_content = support::JsonValue{}};
    }

    [[nodiscard]] support::AsyncResult<mcp::ElicitationAnswer> reply(std::size_t index, std::stop_token stop_token) {
        using Result = support::AsyncResult<mcp::ElicitationAnswer>;
        const std::size_t position = index < pending_from_ ? pending_from_ : index;
        if (handler_) {
            return Result(support::Expected<mcp::ElicitationAnswer>{handler_(asked_.back().request)});
        }
        if (position >= queue_.size()) {
            // Unanswered: the wait stays pending until the port's bound ends
            // it, which is the only thing a suspended call may wait on.
            return Result(Result::producer_type(
                    [this, index, stop_token](support::AsyncCompletion<mcp::ElicitationAnswer, support::Error>
                                    completion) mutable noexcept {
                        while (waits_.size() <= index) {
                            waits_.emplace_back();
                        }
                        waits_[index].stop_token = stop_token;
                        waits_[index].completion = std::move(completion);
                    }));
        }
        auto scripted = queue_[position];
        if (waits_.size() <= index) {
            waits_.resize(index + 1);
        }
        return Result(std::move(scripted));
    }

    std::function<support::Expected<mcp::ElicitationAnswer>(const mcp::ElicitationRequest&)> handler_{};
    /// One scripted answer per question position; a question past the end stays
    /// pending.
    std::vector<support::Expected<mcp::ElicitationAnswer>> queue_{};
    /// The first question the `queue_` answers apply from.
    std::size_t pending_from_{0};
    std::vector<RecordedElicitation> asked_{};
    std::vector<Wait> waits_{};
};

} // namespace cch::tests
