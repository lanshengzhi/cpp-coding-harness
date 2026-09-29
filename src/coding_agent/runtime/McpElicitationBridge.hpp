#pragma once

#include <cch/coding_agent/McpElicitation.hpp>
#include <cch/mcp/UpstreamElicitation.hpp>
#include <cch/support/AsyncResult.hpp>
#include <cch/support/Error.hpp>

#include <atomic>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace cch::coding_agent::runtime {

/// The session's Pending Elicitation seam (issue #845, ADR 0065): the value
/// that lets the MCP Host ask this session a question and this session's
/// frontend answer it, without either of them naming the other.
///
/// It is a passive broker. `cch_mcp` reaches it only through the one
/// `mcp::ElicitationPrompter` port and never learns what answered; the
/// frontend reaches it only through the `cch_coding_agent` projection values
/// and never learns a protocol type. Every access is under one mutex, because
/// the MCP Host asks on the Runtime domain and the frontend answers on its
/// own.
class McpElicitationBridge final : public std::enable_shared_from_this<McpElicitationBridge> {
public:
    /// The port the MCP Host asks through. Handed to every connection the
    /// session owns, so one broker serves the whole session.
    [[nodiscard]] std::shared_ptr<mcp::UpstreamElicitationPort> port();

    /// Ask the user, through the one port. The returned operation completes
    /// when the answer arrives, when the wait's own `stop_token` fires, or
    /// when the session closes — never on its own, so a suspended call cannot
    /// outlive the session that started it.
    ///
    /// Both modes are asked. Form mode carries the Upstream's schema into the
    /// projection for `frontend_tui` to render (issue #846); the wait, the
    /// bound, and the retry are the shared Multi Round-Trip loop's, so no
    /// part of this differs between the two.
    [[nodiscard]] cch::support::AsyncResult<mcp::ElicitationAnswer> ask(
            mcp::ElicitationRequest request, std::stop_token stop_token);

    /// Every elicitation this session is currently blocked on, in the order
    /// they were asked. The whole read model a presentation surface renders.
    [[nodiscard]] std::vector<McpPendingElicitation> pending() const;

    /// Answer one pending elicitation. An identifier that is not pending — an
    /// elicitation already answered, cancelled, or never asked — is refused,
    /// so a second answer for a settled question is dropped rather than
    /// completing a suspended call twice.
    [[nodiscard]] support::ExpectedVoid answer(McpElicitationAnswer answer);

    /// End every pending wait as cancelled, so session Close leaves no
    /// waiter and no suspended call. Idempotent.
    void close() noexcept;

    [[nodiscard]] bool closed() const;

private:
    /// Ends one wait when the MCP call's stop token fires, so a session that
    /// stops ends the question even when nobody answers it. A named type
    /// because `std::stop_callback` takes a concrete callback, not
    /// `std::function`.
    struct StopForward {
        std::shared_ptr<McpElicitationBridge> bridge;
        std::string elicitation_id;
        void operator()() const { bridge->abandon(elicitation_id); }
    };

    /// One awaiting answer.
    struct Waiter {
        std::optional<std::stop_callback<StopForward>> stop{};
        cch::support::AsyncCompletion<mcp::ElicitationAnswer, cch::support::Error> completion{};
    };

    struct Pending {
        McpPendingElicitation projection{};
        std::shared_ptr<Waiter> waiter{};
    };

    /// End one wait as cancelled, dropping it from the pending set first so
    /// the registration is gone before its callback can fire.
    void abandon(const std::string& elicitation_id);

    mutable std::mutex mutex_;
    std::map<std::string, Pending, std::less<>> pending_{};
    std::size_t next_id_{0};
    bool closed_{false};
};

} // namespace cch::coding_agent::runtime
