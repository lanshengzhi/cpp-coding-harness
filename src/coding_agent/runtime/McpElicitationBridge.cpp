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
using support::write_json;

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

/// The Upstream's form schema as the projection carries it: text, because
/// `frontend_tui` reads the fields out of the schema itself and this Owner
/// does not interpret it. A schema that will not serialize cannot be
/// rendered, so it projects as empty text and the dialog reports an empty
/// form rather than the user seeing a parse failure.
[[nodiscard]] std::string schema_text(const mcp::ElicitationRequest& request) {
    if (request.form_schema.holds<JsonValue::null_t>()) {
        return {};
    }
    auto serialized = write_json(request.form_schema);
    return serialized ? *serialized : std::string{};
}

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

AsyncResult<mcp::ElicitationAnswer> McpElicitationBridge::ask(
        mcp::ElicitationRequest request, std::stop_token stop_token) {
    using Result = AsyncResult<mcp::ElicitationAnswer>;
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
                                                    .form_schema = schema_text(request),
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
    // Only an accept carries content. A decline or a cancel is the user
    // refusing, so values alongside it would put on the wire data the user
    // did not supply, and the field for a form the user filled in only to
    // then decline is exactly the one that must not travel.
    JsonValue form_content{};
    if (answer.action == McpElicitationAction::Accept && !answer.form_values.empty()) {
        JsonValue content = JsonValue::object_t{};
        for (const auto& [name, value] : answer.form_values) {
            content.get_object().emplace(name, value);
        }
        form_content = std::move(content);
    }
    auto completion = std::move(waiter->completion);
    completion(mcp::ElicitationAnswer{.action = action_of(answer.action),
            .request_id = answer.elicitation_id,
            .form_content = std::move(form_content)});
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
