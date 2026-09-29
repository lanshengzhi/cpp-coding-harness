// The bounded reconnect ladder and the deterministic two-phase close of one
// Upstream MCP Server connection (issue #839), driven through the one
// injected transport seam and a scripted connection timer: a flapping server
// cannot spin, no reconnect fires after a close, and the whole cleanup is
// bounded however badly the transport behaves.

#include <cch/mcp/UpstreamConnection.hpp>
#include "support/ScriptedMcpTransport.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace cch;

namespace {

constexpr std::string_view kEndpoint{"https://upstream.invalid/mcp"};

struct Fixture {
    std::shared_ptr<tests::ScriptedMcpTransport> transport = std::make_shared<tests::ScriptedMcpTransport>();
    tests::ScriptedMcpDelay clock;
    mcp::UpstreamConnection connection{
            "executor",
            transport,
            mcp::UpstreamConnectionOptions{
                    .url = std::string(kEndpoint),
                    .delay = [this](std::chrono::milliseconds delay, std::stop_token stop_token) {
                        return clock.request(delay, stop_token);
                    },
            }};

    void answer_conforming_upstream() {
        transport->answer("server/discover", {.result = tests::discover_result()});
        transport->answer("tools/list", {.result = tests::tool_list_result({tests::tool_entry("read_issue")})});
    }

    /// A transport that has lost the Upstream entirely: every method fails at
    /// the transport, which is what a dead server looks like from here.
    void answer_unreachable_upstream() {
        const auto failure = support::make_error(
                support::ErrorCode::Network, "the Upstream MCP Server could not be reached");
        transport->answer("server/discover", {.transport_error = failure});
        transport->answer("tools/list", {.transport_error = failure});
        transport->answer("tools/call", {.transport_error = failure});
    }

    [[nodiscard]] std::size_t probes() const { return transport->request_count("server/discover"); }

    [[nodiscard]] mcp::UpstreamToolCall call() const {
        return mcp::UpstreamToolCall{.tool = mcp::UpstreamToolDescriptor{.name = "read_issue"},
                .arguments = support::JsonValue::object_t{}};
    }
};

} // namespace

TEST_CASE("a flapping Upstream is not reconnected once per failure", "[mcp][connection][backoff][issue839][spec]") {
    Fixture fixture;
    fixture.answer_conforming_upstream();
    REQUIRE(tests::drive(fixture.connection.connect()).has_value());

    fixture.answer_unreachable_upstream();
    std::size_t failed_calls = 0;
    for (int attempt = 0; attempt < 50; ++attempt) {
        if (!tests::drive(fixture.connection.call_tool(fixture.call()))) {
            ++failed_calls;
        }
    }
    for (int notice = 0; notice < 100; ++notice) {
        fixture.connection.notify_transport_closed("the response stream was closed");
    }

    // Fifty failed calls and a hundred closure notifications produced one
    // armed reconnect, not one hundred and fifty of them.
    CHECK(failed_calls == 50);
    CHECK(fixture.clock.waiting() == 1);
    CHECK(fixture.connection.status() == mcp::UpstreamConnectionStatus::Failed);

    const auto elapsed = fixture.clock.elapse_all();
    const std::vector<std::chrono::milliseconds> expected_delays{std::chrono::milliseconds{250},
            std::chrono::milliseconds{500},
            std::chrono::milliseconds{1000},
            std::chrono::milliseconds{2000},
            std::chrono::milliseconds{4000},
            std::chrono::milliseconds{8000},
            std::chrono::milliseconds{16000},
            std::chrono::milliseconds{30000},
            std::chrono::milliseconds{30000},
            std::chrono::milliseconds{30000}};
    CHECK(fixture.clock.delays() == expected_delays);
    CHECK(elapsed == expected_delays.size());

    // The ladder doubles from the first rung, is capped, and is spent: the
    // owner's own connection attempt plus at most ten reconnects, which is
    // eleven probes of a dead endpoint however hard it is asked.
    CHECK(fixture.probes() == 11);
    CHECK(fixture.connection.snapshot().consecutive_failures == 10);
    CHECK(fixture.connection.snapshot().next_reconnect_delay == std::chrono::milliseconds{0});
    for (int notice = 0; notice < 100; ++notice) {
        fixture.connection.notify_transport_closed("the response stream was closed");
    }
    CHECK(fixture.probes() == 11);
}

TEST_CASE("a spent ladder starts over only when the owner asks for a connection again",
        "[mcp][connection][backoff][issue839][spec]") {
    Fixture fixture;
    fixture.answer_conforming_upstream();
    REQUIRE(tests::drive(fixture.connection.connect()).has_value());
    fixture.answer_unreachable_upstream();
    REQUIRE_FALSE(tests::drive(fixture.connection.connect()).has_value());
    fixture.clock.elapse_all();
    const auto spent = fixture.probes();
    REQUIRE(spent > 0);

    // The Upstream came back, and the owner is the one who asks again.
    fixture.answer_conforming_upstream();
    REQUIRE(tests::drive(fixture.connection.connect()).has_value());
    CHECK(fixture.probes() == spent + 1);
    CHECK(fixture.connection.snapshot().consecutive_failures == 0);
    CHECK(fixture.connection.status() == mcp::UpstreamConnectionStatus::Connected);
}

TEST_CASE("no reconnect fires after a close that happened during the backoff", "[mcp][connection][close][issue839][spec]") {
    Fixture fixture;
    fixture.answer_unreachable_upstream();
    REQUIRE_FALSE(tests::drive(fixture.connection.connect()).has_value());
    REQUIRE(fixture.clock.waiting() == 1);

    auto closed = tests::drive(fixture.connection.close());
    REQUIRE(closed.has_value());
    CHECK(closed->within_bound);
    CHECK(closed->abandoned_operations == 0);

    const auto probes_at_close = fixture.probes();
    // The armed delay has been cancelled; letting it elapse is exactly what a
    // runtime would do, and it must reconnect nothing.
    CHECK(fixture.clock.elapse_oldest());
    CHECK(fixture.probes() == probes_at_close);
    CHECK(fixture.clock.waiting() == 0);

    // A close is idempotent and reports the same outcome again.
    auto repeated = tests::drive(fixture.connection.close());
    REQUIRE(repeated.has_value());
    CHECK(repeated->within_bound);
    CHECK(repeated->abandoned_operations == 0);
    CHECK(tests::drive(fixture.connection.list_tools()).error().code == support::ErrorCode::Cancelled);
}

TEST_CASE("a close during an in-flight call reaches quiescence with nothing abandoned",
        "[mcp][connection][close][issue839][spec]") {
    Fixture fixture;
    fixture.answer_conforming_upstream();
    REQUIRE(tests::drive(fixture.connection.connect()).has_value());
    fixture.transport->hold("tools/call");

    bool call_completed = false;
    fixture.connection.call_tool(fixture.call())
            .start([&call_completed](std::expected<mcp::UpstreamToolCallResult, support::Error>) noexcept {
                call_completed = true;
            });
    REQUIRE_FALSE(call_completed);
    CHECK(fixture.connection.snapshot().in_flight == 1);

    // The caller's cancellation reaches the request, and the transport answers
    // it, so the cleanup bound is never needed.
    auto closed = tests::drive(fixture.connection.close());
    REQUIRE(closed.has_value());
    CHECK(closed->within_bound);
    CHECK(closed->abandoned_operations == 0);
    CHECK(call_completed);
    CHECK(fixture.clock.waiting() == 0);
    CHECK(fixture.connection.snapshot().in_flight == 0);
}

TEST_CASE("a call that ignores cancellation is released at the cleanup bound",
        "[mcp][connection][close][issue839][spec]") {
    Fixture fixture;
    fixture.answer_conforming_upstream();
    REQUIRE(tests::drive(fixture.connection.connect()).has_value());
    fixture.transport->hold("tools/call", tests::McpHold::Ignored);

    bool call_completed = false;
    fixture.connection.call_tool(fixture.call())
            .start([&call_completed](std::expected<mcp::UpstreamToolCallResult, support::Error>) noexcept {
                call_completed = true;
            });
    REQUIRE_FALSE(call_completed);

    // The first phase of the close returns without waiting.
    std::optional<mcp::UpstreamCloseOutcome> outcome;
    fixture.connection.close().start([&outcome](std::expected<mcp::UpstreamCloseOutcome, support::Error> value) noexcept {
        if (value) {
            outcome = *value;
        }
    });
    CHECK_FALSE(outcome.has_value());

    // The second phase is bounded: the connection asked for exactly the
    // 1 s cleanup bound and released the connection when it expired.
    REQUIRE(fixture.clock.waiting() == 1);
    CHECK(fixture.clock.next_delay() == std::chrono::milliseconds{1000});
    CHECK(fixture.clock.elapse_oldest());

    REQUIRE(outcome.has_value());
    CHECK_FALSE(outcome->within_bound);
    CHECK(outcome->abandoned_operations == 1);
    CHECK_FALSE(call_completed);
    CHECK(fixture.clock.waiting() == 0);
}

TEST_CASE("a close requested from a status change does not wait on the change that made it",
        "[mcp][connection][close][issue839][spec]") {
    auto transport = std::make_shared<tests::ScriptedMcpTransport>();
    tests::ScriptedMcpDelay clock;
    std::optional<mcp::UpstreamCloseOutcome> outcome;
    std::shared_ptr<mcp::UpstreamConnection> connection;
    connection = std::make_shared<mcp::UpstreamConnection>("executor",
            transport,
            mcp::UpstreamConnectionOptions{
                    .url = std::string(kEndpoint),
                    .status_sink = [&connection, &outcome](const mcp::UpstreamConnectionSnapshot& snapshot) {
                        if (snapshot.status != mcp::UpstreamConnectionStatus::Failed) {
                            return;
                        }
                        connection->close().start([&outcome](
                                                          std::expected<mcp::UpstreamCloseOutcome, support::Error> value) noexcept {
                            if (value) {
                                outcome = *value;
                            }
                        });
                    },
                    .delay = [&clock](std::chrono::milliseconds delay, std::stop_token stop_token) {
                        return clock.request(delay, stop_token);
                    },
            });
    transport->answer("server/discover",
            {.result = tests::discover_result()});
    REQUIRE(tests::drive(connection->connect()).has_value());

    transport->answer("tools/call",
            {.transport_error = support::make_error(
                    support::ErrorCode::Network, "the Upstream MCP Server could not be reached")});
    REQUIRE_FALSE(tests::drive(connection->call_tool(mcp::UpstreamToolCall{
                                         .tool = mcp::UpstreamToolDescriptor{.name = "read_issue"},
                                         .arguments = support::JsonValue::object_t{}}))
                            .has_value());

    REQUIRE(outcome.has_value());
    CHECK(outcome->within_bound);
    CHECK(outcome->abandoned_operations == 0);
    CHECK(clock.waiting() == 0); // the armed reconnect was cancelled by the close
}

TEST_CASE("a connection that was never connected still closes deterministically", "[mcp][connection][close][issue839][spec]") {
    Fixture fixture;
    auto closed = tests::drive(fixture.connection.close());
    REQUIRE(closed.has_value());
    CHECK(closed->within_bound);
    CHECK(closed->abandoned_operations == 0);
    CHECK(fixture.clock.waiting() == 0);
    CHECK(fixture.probes() == 0);
}
