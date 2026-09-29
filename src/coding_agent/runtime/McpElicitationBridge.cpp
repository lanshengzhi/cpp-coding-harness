#include "coding_agent/runtime/McpElicitationBridge.hpp"

#include "support/Json.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::coding_agent::runtime {
namespace {

using support::AsyncCompletion;
using support::AsyncResult;
using support::Error;
using support::ErrorCode;
using support::JsonValue;
using support::make_error;

[[nodiscard]] McpElicitationMode project(mcp::ElicitationMode mode) noexcept {
    switch (mode) {
    case mcp::ElicitationMode::Form:
        return McpElicitationMode::Form;
    case mcp::ElicitationMode::Url:
        return McpElicitationMode::Url;
    }
    return McpElicitationMode::Url;
}

[[nodiscard]] mcp::ElicitationAction action_of(McpElicitationAction action) noexcept {
    switch (action) {
    case McpElicitationAction::Accept:
        return mcp::ElicitationAction::Accept;
    case McpElicitationAction::Decline:
        return mcp::ElicitationAction::Decline;
    case McpElicitationAction::Cancel:
        return mcp::ElicitationAction::Cancel;
    }
    return mcp::ElicitationAction::Cancel;
}

} // namespace

std::string_view to_string(McpElicitationAction action) noexcept {
    switch (action) {
    case McpElicitationAction::Accept:
        return "accept";
    case McpElicitationAction::Decline:
        return "decline";
    case McpElicitationAction::Cancel:
        return "cancel";
    }
    return "cancel";
}

std::shared_ptr<mcp::UpstreamElicitationPort> McpElicitationBridge::port() {
    auto port = std::make_shared<mcp::UpstreamElicitationPort>();
    port->prompter = [self = shared_from_this()](mcp::ElicitationRequest request, std::stop_token stop_token) {
        return self->ask(std::move(request), stop_token);
    };
    return port;
}

Error McpElicitationBridge::unpresentable(std::string_view server_id) {
    return make_error(ErrorCode::Validation,
            "the Upstream MCP Server asked a Pending Elicitation question this build cannot present",
            "Server Id \"" + std::string(server_id) +
                    "\" asked in a mode the Native UI has no dialog for, so the question was never put in front of "
                    "the user and the one tool call failed instead");
}

AsyncResult<mcp::ElicitationAnswer> McpElicitationBridge::ask(
        mcp::ElicitationRequest request, std::stop_token stop_token) {
    using Result = AsyncResult<mcp::ElicitationAnswer>;
    // A mode this build cannot render is refused before the wait exists: the
    // Upstream learns its call failed, the user is never asked a question the
    // session cannot collect an answer to, and nothing is left suspended. This
    // is the one place form mode is turned on (issue #846).
    if (request.mode == mcp::ElicitationMode::Form) {
        return Result(std::unexpected(unpresentable(request.server_id)));
    }
    auto self = shared_from_this();
    return Result(
            Result::producer_type([self = std::move(self), request = std::move(request), stop_token](
                                          AsyncCompletion<mcp::ElicitationAnswer, Error> completion) mutable noexcept {
                auto waiter = std::make_shared<Waiter>();
                std::string id;
                {
                    const std::scoped_lock lock(self->mutex_);
                    if (self->closed_) {
                        completion(std::unexpected(make_error(
                                ErrorCode::Cancelled, "this session's Pending Elicitation surface is closed")));
                        return;
                    }
                    self->next_id_ += 1;
                    id = request.server_id + "#" + std::to_string(self->next_id_);
                    if (stop_token.stop_possible()) {
                        waiter->stop.emplace(stop_token, StopForward{.bridge = self, .elicitation_id = id});
                    }
                    self->pending_.emplace(id,
                            Pending{.projection =
                                            McpPendingElicitation{
                                                    .elicitation_id = id,
                                                    .server_id = request.server_id,
                                                    .tool_name = request.tool_name,
                                                    .mode = project(request.mode),
                                                    .request_id = request.request_id,
                                                    .message = request.message,
                                                    .url = request.url,
                                            },
                                    .waiter = waiter});
                }
                // The completion is stored after the registration, so a stop
                // that fires the instant the question is asked finds a waiter
                // that can still be completed exactly once.
                waiter->completion = std::move(completion);
            }));
}

void McpElicitationBridge::abandon(const std::string& elicitation_id) {
    std::shared_ptr<Waiter> waiter;
    {
        const std::scoped_lock lock(mutex_);
        const auto found = pending_.find(elicitation_id);
        if (found == pending_.end()) {
            return; // already answered or already ended: the stop lost the race
        }
        waiter = found->second.waiter;
        pending_.erase(found);
    }
    if (waiter && waiter->completion) {
        auto completion = std::move(waiter->completion);
        completion(std::unexpected(
                make_error(ErrorCode::Cancelled, "the Pending Elicitation wait was stopped before it was answered")));
    }
}

std::vector<McpPendingElicitation> McpElicitationBridge::pending() const {
    const std::scoped_lock lock(mutex_);
    std::vector<McpPendingElicitation> rows;
    rows.reserve(pending_.size());
    for (const auto& [id, entry] : pending_) {
        (void)id;
        rows.push_back(entry.projection);
    }
    return rows;
}

support::ExpectedVoid McpElicitationBridge::answer(McpElicitationAnswer answer) {
    std::shared_ptr<Waiter> waiter;
    {
        const std::scoped_lock lock(mutex_);
        const auto found = pending_.find(answer.elicitation_id);
        if (found == pending_.end()) {
            return std::unexpected(make_error(ErrorCode::Validation,
                    "this session has no Pending Elicitation awaiting that answer",
                    "elicitation \"" + answer.elicitation_id +
                            "\" is unknown, already answered, or was already ended"));
        }
        waiter = found->second.waiter;
        // The registration is dropped before the answer leaves the broker, so
        // a stop that raced the answer cannot complete the same wait twice.
        pending_.erase(found);
    }
    if (!waiter || !waiter->completion) {
        return std::unexpected(make_error(ErrorCode::Validation,
                "this session has no Pending Elicitation awaiting that answer",
                "elicitation \"" + answer.elicitation_id + "\" was already settled"));
    }
    JsonValue content = JsonValue::object_t{};
    for (const auto& [name, value] : answer.form_values) {
        content.get_object().emplace(name, JsonValue(value));
    }
    auto completion = std::move(waiter->completion);
    completion(mcp::ElicitationAnswer{.action = action_of(answer.action),
            .request_id = answer.elicitation_id,
            .form_content = answer.form_values.empty() ? JsonValue{} : std::move(content)});
    return {};
}

void McpElicitationBridge::close() noexcept {
    std::map<std::string, Pending, std::less<>> ended;
    {
        const std::scoped_lock lock(mutex_);
        if (closed_) {
            return;
        }
        closed_ = true;
        ended.swap(pending_);
    }
    // The completions run outside the lock: a completion that answers, or
    // that re-enters the bridge, must not find the mutex held.
    for (auto& [id, entry] : ended) {
        (void)id;
        if (entry.waiter && entry.waiter->completion) {
            auto completion = std::move(entry.waiter->completion);
            completion(std::unexpected(
                    make_error(ErrorCode::Cancelled, "the session ended before the question was answered")));
        }
    }
}

bool McpElicitationBridge::closed() const {
    const std::scoped_lock lock(mutex_);
    return closed_;
}

} // namespace cch::coding_agent::runtime
