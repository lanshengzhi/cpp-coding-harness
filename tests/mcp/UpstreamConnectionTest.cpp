// The per-Upstream connection machinery driven through the one injected
// transport seam and a scripted connection timer (issue #839): the five
// Upstream Connection Status states, non-blocking startup, and health inferred
// from request outcomes and transport closure rather than from a heartbeat.
//
// No case here reaches past the transport: a status change is observed through
// the published snapshot, and every request the connection made is read back
// off the scripted transport.

#include <cch/mcp/UpstreamConnection.hpp>
#include "support/ScriptedMcpTransport.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace cch;

namespace {

constexpr std::string_view kEndpoint{"https://upstream.invalid/mcp"};

/// One connection under test, with the transport and the timer it drives
/// exposed so a case can script the Upstream and read back what it was asked
/// for.
struct Fixture {
    std::shared_ptr<tests::ScriptedMcpTransport> transport = std::make_shared<tests::ScriptedMcpTransport>();
    tests::ScriptedMcpDelay clock;
    std::vector<mcp::UpstreamConnectionSnapshot> published;
    mcp::UpstreamConnection connection{"executor",
            transport,
            mcp::UpstreamConnectionOptions{
                    .url = std::string(kEndpoint),
                    .status_sink =
                            [this](const mcp::UpstreamConnectionSnapshot& snapshot) { published.push_back(snapshot); },
                    .delay = [this](std::chrono::milliseconds delay,
                                     std::stop_token stop_token) { return clock.request(delay, stop_token); },
            }};

    /// An Upstream that answers the era probe and one tool page.
    void answer_conforming_upstream() {
        transport->answer("server/discover", {.result = tests::discover_result()});
        transport->answer("tools/list", {.result = tests::tool_list_result({tests::tool_entry("read_issue")})});
        transport->answer("tools/call", {.result = tests::tool_call_result(support::JsonValue::array_t{})});
    }

    void answer_failing_probe(support::ErrorCode code = support::ErrorCode::Network) {
        transport->answer("server/discover",
                {.transport_error = support::make_error(code, "the Upstream MCP Server could not be reached")});
    }

    [[nodiscard]] std::size_t probes() const { return transport->request_count("server/discover"); }
};

[[nodiscard]] bool status_in(
        const std::vector<mcp::UpstreamConnectionSnapshot>& published, mcp::UpstreamConnectionStatus status) {
    for (const auto& snapshot : published) {
        if (snapshot.status == status) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("a connection starts pending and reports connected once the era probe answers",
        "[mcp][connection][status][issue839][spec]") {
    Fixture fixture;
    CHECK(fixture.connection.status() == mcp::UpstreamConnectionStatus::Pending);
    CHECK(fixture.connection.snapshot().server_id == "executor");

    fixture.answer_conforming_upstream();
    auto probed = tests::drive(fixture.connection.connect());
    REQUIRE(probed.has_value());
    CHECK(probed->name == "executor");
    CHECK(fixture.connection.status() == mcp::UpstreamConnectionStatus::Connected);
    CHECK(fixture.published.back().status == mcp::UpstreamConnectionStatus::Connected);
    CHECK(status_in(fixture.published, mcp::UpstreamConnectionStatus::Pending));
    CHECK(fixture.published.back().diagnostic.empty());
}

TEST_CASE("the five Upstream Connection Status states are exactly the ones the glossary names",
        "[mcp][connection][status][issue839][spec]") {
    const std::vector<std::pair<mcp::UpstreamConnectionStatus, std::string_view>> states{
            {mcp::UpstreamConnectionStatus::Pending, "pending"},
            {mcp::UpstreamConnectionStatus::Connected, "connected"},
            {mcp::UpstreamConnectionStatus::Failed, "failed"},
            {mcp::UpstreamConnectionStatus::NeedsAuth, "needs_auth"},
            {mcp::UpstreamConnectionStatus::Disabled, "disabled"},
    };
    CHECK(states.size() == 5);
    for (const auto& [status, name] : states) {
        CHECK(mcp::to_string(status) == name);
    }
}

TEST_CASE("startup never waits for an Upstream that never answers", "[mcp][connection][startup][issue839][spec]") {
    Fixture fixture;
    fixture.transport->hold("server/discover");

    bool completed = false;
    bool catalog_result = true;
    // The connection attempt is handed back while the Upstream is still
    // silent: this is the whole of a session start as far as MCP is
    // concerned, and it must not have waited for the answer.
    auto attempt = fixture.connection.connect();
    attempt.start([&completed](std::expected<mcp::UpstreamServerInfo, support::Error>) noexcept { completed = true; });

    CHECK_FALSE(completed);
    CHECK(fixture.connection.status() == mcp::UpstreamConnectionStatus::Pending);
    CHECK(fixture.connection.snapshot().in_flight == 1);
    // The one reading published so far is the attempt itself: the connection
    // is `pending` with one operation in flight, and nothing has failed yet.
    REQUIRE(fixture.published.size() == 1);
    CHECK(fixture.published.front().status == mcp::UpstreamConnectionStatus::Pending);
    CHECK(fixture.published.front().in_flight == 1);
    CHECK(fixture.published.front().diagnostic.empty());

    // The session keeps going: a tool call against a connection that is not
    // connected fails immediately rather than waiting on the silent Upstream.
    auto catalog = fixture.connection.list_tools();
    catalog.start([&catalog_result](std::expected<mcp::UpstreamCatalog, support::Error> outcome) noexcept {
        catalog_result = outcome.has_value();
    });
    CHECK_FALSE(catalog_result);
    CHECK(fixture.connection.status() == mcp::UpstreamConnectionStatus::Pending);

    // Closing releases the silent call rather than leaving it in flight.
    auto closed = tests::drive(fixture.connection.close());
    REQUIRE(closed.has_value());
    CHECK(closed->abandoned_operations == 0);
}

TEST_CASE("a failed connection reports failed and a transport failure from a request reports it too",
        "[mcp][connection][health][issue839][spec]") {
    Fixture fixture;
    fixture.answer_failing_probe();

    auto probed = tests::drive(fixture.connection.connect());
    REQUIRE_FALSE(probed.has_value());
    CHECK(fixture.connection.status() == mcp::UpstreamConnectionStatus::Failed);
    CHECK(fixture.connection.snapshot().diagnostic.find("could not be reached") != std::string::npos);
    CHECK(fixture.published.back().consecutive_failures == 1);
    // The failure armed exactly one reconnect rather than reconnecting per
    // failure.
    CHECK(fixture.clock.waiting() == 1);
}

TEST_CASE("a request that reaches the Upstream is not health evidence against it",
        "[mcp][connection][health][issue839][spec]") {
    Fixture fixture;
    fixture.answer_conforming_upstream();
    REQUIRE(tests::drive(fixture.connection.connect()).has_value());

    // A protocol violation is the Upstream's behavior, not the connection's
    // health, so it must not move the connection out of `connected`.
    fixture.transport->answer("tools/list", {.error_code = -32000, .error_message = "malformed page"});
    auto catalog = tests::drive(fixture.connection.list_tools());
    REQUIRE_FALSE(catalog.has_value());
    CHECK(catalog.error().code == support::ErrorCode::Validation);
    CHECK(fixture.connection.status() == mcp::UpstreamConnectionStatus::Connected);
    CHECK(fixture.clock.waiting() == 0);
}

TEST_CASE("a lost transport moves a connected connection to failed and re-probes on reconnect",
        "[mcp][connection][health][issue839][spec]") {
    Fixture fixture;
    fixture.answer_conforming_upstream();
    REQUIRE(tests::drive(fixture.connection.connect()).has_value());

    fixture.connection.notify_transport_closed("the response stream was closed");
    CHECK(fixture.connection.status() == mcp::UpstreamConnectionStatus::Failed);
    CHECK(fixture.clock.waiting() == 1);

    // The era belongs to the connection that probed it, so the reconnect is a
    // new connection that probes again.
    REQUIRE(fixture.clock.elapse_oldest());
    CHECK(fixture.probes() == 2);
    CHECK(fixture.connection.status() == mcp::UpstreamConnectionStatus::Connected);
    CHECK(fixture.connection.snapshot().consecutive_failures == 0);
}

TEST_CASE("an authentication challenge waits for the user instead of for another attempt",
        "[mcp][connection][health][issue839][spec]") {
    Fixture fixture;
    fixture.transport->answer("server/discover", {.status_code = 401});
    fixture.transport->answer("tools/list", {.status_code = 401});

    auto probed = tests::drive(fixture.connection.connect());
    REQUIRE_FALSE(probed.has_value());
    CHECK(probed.error().code == support::ErrorCode::Auth);
    CHECK(fixture.connection.status() == mcp::UpstreamConnectionStatus::NeedsAuth);
    CHECK(fixture.clock.waiting() == 0); // no reconnect can supply a credential

    // Further transport-closure notifications are absorbed rather than
    // turning a credential wait into a reconnect loop.
    fixture.connection.notify_transport_closed("the response stream was closed");
    CHECK(fixture.connection.status() == mcp::UpstreamConnectionStatus::NeedsAuth);
    CHECK(fixture.clock.waiting() == 0);

    // A fresh attempt is what clears it, which is what `/mcp auth` does once
    // the user has authorized the Upstream.
    fixture.answer_conforming_upstream();
    REQUIRE(tests::drive(fixture.connection.connect()).has_value());
    CHECK(fixture.connection.status() == mcp::UpstreamConnectionStatus::Connected);
}

TEST_CASE("a disabled connection admits nothing and re-enabling it connects again",
        "[mcp][connection][status][issue839][spec]") {
    Fixture fixture;
    fixture.answer_conforming_upstream();
    REQUIRE(tests::drive(fixture.connection.connect()).has_value());

    fixture.connection.disable("the user has not enabled this Upstream");
    CHECK(fixture.connection.status() == mcp::UpstreamConnectionStatus::Disabled);
    CHECK(tests::drive(fixture.connection.list_tools()).error().code == support::ErrorCode::Cancelled);
    CHECK(fixture.clock.waiting() == 0);

    // Disabling twice is not an error and changes nothing.
    fixture.connection.disable("still disabled");
    CHECK(fixture.published.back().diagnostic == "the user has not enabled this Upstream");

    fixture.connection.enable();
    CHECK(fixture.connection.status() == mcp::UpstreamConnectionStatus::Connected);
    CHECK(fixture.probes() == 2);
    CHECK(tests::drive(fixture.connection.list_tools()).has_value());
}

TEST_CASE("a tool call is bounded by the per-call deadline and capped at the containment bound",
        "[mcp][connection][limits][issue839][spec]") {
    Fixture fixture;
    fixture.answer_conforming_upstream();
    REQUIRE(tests::drive(fixture.connection.connect()).has_value());

    auto catalog = tests::drive(fixture.connection.list_tools());
    REQUIRE(catalog.has_value());
    auto outcome = tests::drive(fixture.connection.call_tool(mcp::UpstreamToolCall{
            .tool = catalog->tools.front(),
            .arguments = support::JsonValue::object_t{},
    }));
    REQUIRE(outcome.has_value());

    for (std::size_t index = 0; index < fixture.transport->request_count(); ++index) {
        CHECK(fixture.transport->recorded_timeout(index) == std::chrono::seconds{30});
    }

    // A caller asking for ten minutes gets the 300 s cap, never ten minutes.
    Fixture greedy;
    greedy.answer_conforming_upstream();
    mcp::UpstreamConnection capped("executor",
            greedy.transport,
            mcp::UpstreamConnectionOptions{
                    .url = std::string(kEndpoint),
                    .request_timeout = std::chrono::minutes{10},
                    .delay =
                            [](std::chrono::milliseconds, std::stop_token) {
                                return cch::support::AsyncResult<void>(cch::support::ExpectedVoid{});
                            },
            });
    REQUIRE(tests::drive(capped.connect()).has_value());
    REQUIRE(tests::drive(capped.list_tools()).has_value());
    REQUIRE(greedy.transport->request_count() > 0);
    CHECK(greedy.transport->recorded_timeout(greedy.transport->request_count() - 1) == std::chrono::seconds{300});
}

TEST_CASE("a cancelled call is the caller's decision and leaves the connection connected",
        "[mcp][connection][cancellation][issue839][spec]") {
    Fixture fixture;
    fixture.answer_conforming_upstream();
    REQUIRE(tests::drive(fixture.connection.connect()).has_value());

    fixture.transport->hold("tools/call");
    std::stop_source caller;
    bool completed = false;
    auto call = fixture.connection.call_tool(
            mcp::UpstreamToolCall{.tool = mcp::UpstreamToolDescriptor{.name = "read_issue"},
                    .arguments = support::JsonValue::object_t{}},
            caller.get_token());
    call.start([&completed](std::expected<mcp::UpstreamToolCallResult, support::Error>) noexcept { completed = true; });
    CHECK_FALSE(completed);

    caller.request_stop();
    CHECK(completed);
    CHECK(fixture.connection.status() == mcp::UpstreamConnectionStatus::Connected);
    CHECK(fixture.clock.waiting() == 0);
}
