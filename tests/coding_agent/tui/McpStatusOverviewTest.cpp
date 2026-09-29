// The `/mcp` Upstream Connection Status overview (issue #841, spec #833
// story 8): the chat block the Native TUI renders, and the router entry that
// reaches it.
//
// The block is built from the session's `cch_coding_agent` projection only.
// The formatter takes `std::span<const cch::coding_agent::McpUpstreamStatus>`
// and cannot name an `cch_mcp` type at all, so the headless-no-frontend
// invariant and the ADR 0065 Owner boundary hold by construction here: a new
// state invented in the MCP Host package would not compile into this file.
//
// Coverage:
// - exactly the five Upstream Connection Status states render, one per row,
//   in configuration order;
// - a trust-gated-off server reads `disabled`, never `failed`;
// - a flapping server shows the bounded reconnect ladder the connection
//   machinery published (failure count and the delay before the next
//   attempt) rather than a bare failure;
// - a session with no configured server renders the empty-state line.

#include "coding_agent/tui/SlashCommandEffects.hpp"
#include "coding_agent/tui/SlashCommandRouter.hpp"

#include <cch/coding_agent/McpUpstreamStatus.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>
#include <vector>

using namespace cch;

namespace {

using coding_agent::McpUpstreamState;
using coding_agent::McpUpstreamStatus;

[[nodiscard]] std::string render(std::vector<McpUpstreamStatus> rows) {
    return coding_agent::tui::format_mcp_status(rows);
}

[[nodiscard]] std::size_t occurrences(std::string_view text, std::string_view needle) {
    std::size_t count = 0;
    for (std::size_t at = text.find(needle); at != std::string_view::npos; at = text.find(needle, at + 1)) {
        ++count;
    }
    return count;
}

} // namespace

TEST_CASE("the /mcp overview renders each Upstream MCP Server in exactly one of the five states",
        "[coding_agent][tui][commands][mcp][issue841][spec]") {
    const std::string block = render({
            McpUpstreamStatus{.server_id = "executor", .state = McpUpstreamState::Connected},
            McpUpstreamStatus{.server_id = "docs", .state = McpUpstreamState::Pending},
            McpUpstreamStatus{
                    .server_id = "registry",
                    .state = McpUpstreamState::Failed,
                    .status_message = "the upstream refused the connection",
            },
            McpUpstreamStatus{.server_id = "sso", .state = McpUpstreamState::NeedsAuth},
            McpUpstreamStatus{
                    .server_id = "local-tools",
                    .state = McpUpstreamState::Disabled,
                    .status_message = "declined",
            },
    });

    CHECK(block.find("executor: connected") != std::string::npos);
    CHECK(block.find("docs: pending") != std::string::npos);
    CHECK(block.find("registry: failed") != std::string::npos);
    CHECK(block.find("sso: needs_auth") != std::string::npos);
    // A server the trust gate never enabled is disabled, not failed: nothing
    // was attempted, so there is no failure to report and nothing to retry.
    CHECK(block.find("local-tools: disabled") != std::string::npos);
    CHECK(block.find("local-tools: failed") == std::string::npos);

    // The bounded, redacted diagnostic the connection published is shown, and
    // only for the row that carries one.
    CHECK(block.find("the upstream refused the connection") != std::string::npos);
    CHECK(occurrences(block, "declined") == 1);
    CHECK(block.find("executor: connected") < block.find("docs: pending"));
}

TEST_CASE("the /mcp overview shows the bounded reconnect ladder of a flapping server",
        "[coding_agent][tui][commands][mcp][issue841][spec]") {
    const std::string block = render({McpUpstreamStatus{
            .server_id = "executor",
            .state = McpUpstreamState::Failed,
            .status_message = "the transport closed",
            .consecutive_failures = 3,
            .next_reconnect_delay = std::chrono::milliseconds{1000},
    }});

    // The ladder is what a flapping server's row adds over a bare failure: how
    // many attempts have failed and how long until the next one (issue #839).
    CHECK(block.find("executor: failed") != std::string::npos);
    CHECK(block.find("3 failure(s), next attempt in 1000ms") != std::string::npos);
}

TEST_CASE("the /mcp overview renders the empty state for a session with no configured server",
        "[coding_agent][tui][commands][mcp][issue841][spec]") {
    const std::string block = render({});
    CHECK(block.find("No Upstream MCP Servers are configured.") != std::string::npos);
}

TEST_CASE("/mcp routes as an immediate read-only command", "[coding_agent][tui][commands][mcp][issue841][spec]") {
    using coding_agent::tui::SlashCommandExecutionContext;
    using coding_agent::tui::SlashCommandId;
    using coding_agent::tui::SlashCommandImmediateResult;
    using coding_agent::tui::SlashCommandInvocation;
    using coding_agent::tui::SlashCommandParseResultVariant;
    using coding_agent::tui::SlashCommandRouteVariant;
    using coding_agent::tui::SlashCommandRouter;

    std::vector<SlashCommandId> executed;
    SlashCommandExecutionContext context;
    context.execute_immediate = [&executed](const SlashCommandInvocation& invocation) {
        executed.push_back(invocation.command);
        return support::ExpectedVoid{};
    };
    SlashCommandRouter router;

    const auto parsed = SlashCommandRouter::parse("/mcp");
    const auto* invocation = std::get_if<SlashCommandInvocation>(&parsed);
    REQUIRE(invocation != nullptr);
    CHECK(invocation->command == SlashCommandId::Mcp);
    CHECK(invocation->argument.empty());

    const auto routed = router.route("/mcp", context);
    CHECK(std::get_if<SlashCommandImmediateResult>(&routed) != nullptr);
    REQUIRE(executed.size() == 1);
    CHECK(executed.front() == SlashCommandId::Mcp);
}
