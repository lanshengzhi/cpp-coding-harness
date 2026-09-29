// Cancelling an in-flight upstream call and the progress it reports while it
// runs (issue #844; spec #833 stories 30 and 31; ADR 0020).
//
// Both behaviors are driven from the one transport seam. Cancellation is a
// caller stop reaching the request, a `notifications/cancelled` reaching the
// wire for that request's own id, and the call settling exactly once — the
// 2026-07-28 revision has no resumability, so the notification is the whole of
// "stop the server-side work" and no replay is attempted. Progress is a
// `notifications/progress` carrying the token the call declared, delivered to
// that call's own sink and to no other.

#include <cch/mcp/UpstreamClient.hpp>
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
using support::JsonValue;

namespace {

constexpr std::string_view kEndpoint{"https://upstream.invalid/mcp"};

/// The `progressToken` a request declared, read off the request the client
/// stack actually framed.
[[nodiscard]] std::optional<std::string> declared_token(const JsonValue& params) {
    const auto* object = params.get_if<JsonValue::object_t>();
    if (object == nullptr) {
        return std::nullopt;
    }
    const auto meta = object->find("_meta");
    if (meta == object->end()) {
        return std::nullopt;
    }
    const auto* meta_object = meta->second.get_if<JsonValue::object_t>();
    if (meta_object == nullptr) {
        return std::nullopt;
    }
    const auto token = meta_object->find("progressToken");
    if (token == meta_object->end()) {
        return std::nullopt;
    }
    const auto* text = token->second.get_if<std::string>();
    return text != nullptr ? std::optional<std::string>{*text} : std::nullopt;
}

/// Every recorded request whose method is `method`, as `params`.
[[nodiscard]] std::vector<JsonValue> requests_of(
        const tests::ScriptedMcpTransport& transport, std::string_view method) {
    std::vector<JsonValue> found;
    for (std::size_t index = 0; index < transport.request_count(); ++index) {
        auto recorded = transport.recorded_method(index);
        REQUIRE(recorded.has_value());
        if (*recorded != method) {
            continue;
        }
        auto params = transport.recorded_params(index);
        REQUIRE(params.has_value());
        found.push_back(std::move(*params));
    }
    return found;
}

/// One connecting client whose Upstream has answered the era probe.
[[nodiscard]] mcp::UpstreamClient connected(
        const std::shared_ptr<tests::ScriptedMcpTransport>& transport) {
    transport->answer("server/discover", {.result = tests::discover_result()});
    transport->answer("tools/list", {.result = tests::tool_list_result({tests::tool_entry("read_issue")})});
    mcp::UpstreamClient client("executor", transport, mcp::UpstreamClientOptions{.url = std::string(kEndpoint)});
    REQUIRE(tests::drive(client.list_tools()).has_value());
    return client;
}

[[nodiscard]] mcp::UpstreamToolCall call() {
    return mcp::UpstreamToolCall{
            .tool = mcp::UpstreamToolDescriptor{.name = "read_issue"},
            .arguments = JsonValue::object_t{},
    };
}

/// One completed call result, so a case can tell an ordinary success from a
/// dropped progress notification.
[[nodiscard]] JsonValue completed() {
    return tests::tool_call_result(JsonValue::array_t{JsonValue::object_t{
            {"type", JsonValue("text")},
            {"text", JsonValue("ABC-1")},
    }});
}

} // namespace

TEST_CASE("a cancelled call tells the Upstream to stop the work the closed stream abandoned",
        "[mcp][cancellation][issue844][spec]") {
    auto transport = std::make_shared<tests::ScriptedMcpTransport>();
    auto client = connected(transport);
    // The Upstream takes the call and goes silent, so the call is still in
    // flight when the caller's stop arrives.
    transport->hold("tools/call");

    std::stop_source caller;
    std::optional<support::Expected<mcp::UpstreamToolCallResult>> outcome;
    std::size_t completions = 0;
    client.call_tool(call(), caller.get_token())
            .start([&](std::expected<mcp::UpstreamToolCallResult, support::Error> value) noexcept {
                ++completions;
                outcome = std::move(value);
            });
    REQUIRE_FALSE(outcome.has_value());

    caller.request_stop();
    REQUIRE(outcome.has_value());
    CHECK_FALSE(*outcome);
    CHECK(outcome->error().code == support::ErrorCode::Cancelled);
    CHECK(completions == 1);

    // The response stream is gone, so the one signal left that reaches the work
    // behind it is the notification — and it names the request the call
    // actually issued, read off that request's own frame.
    const auto cancellations = requests_of(*transport, "notifications/cancelled");
    REQUIRE(cancellations.size() == 1);
    const auto calls = requests_of(*transport, "tools/call");
    REQUIRE(calls.size() == 1);
    const auto* parameters = cancellations.front().get_if<JsonValue::object_t>();
    REQUIRE(parameters != nullptr);
    const auto request_id = parameters->find("requestId");
    REQUIRE(request_id != parameters->end());
    REQUIRE(request_id->second.holds<double>());
    // The client stack minted ids in order, so the call's request is the first
    // one after the probe; matching the value rather than the position keeps the
    // assertion about the notification, not about the counter.
    CHECK(request_id->second.get<double>() > 1.0);
    CHECK(parameters->find("reason") != parameters->end());
}

TEST_CASE("a late answer to a cancelled call never finishes the call a second time",
        "[mcp][cancellation][issue844][spec]") {
    auto transport = std::make_shared<tests::ScriptedMcpTransport>();
    auto client = connected(transport);
    transport->hold("tools/call");

    std::stop_source caller;
    std::optional<support::Expected<mcp::UpstreamToolCallResult>> outcome;
    std::size_t completions = 0;
    client.call_tool(call(), caller.get_token())
            .start([&](std::expected<mcp::UpstreamToolCallResult, support::Error> value) noexcept {
                ++completions;
                outcome = std::move(value);
            });
    caller.request_stop();
    REQUIRE(outcome.has_value());
    REQUIRE_FALSE(*outcome);

    // The Upstream's answer arrives after the stop, which is the whole hazard:
    // a result that arrives late must not finish a call that already settled,
    // and must not turn a cancellation into a success.
    transport->answer("tools/call", {.result = completed()});
    transport->release("tools/call");
    CHECK(completions == 1);
    REQUIRE(outcome.has_value());
    CHECK_FALSE(*outcome);
    CHECK(outcome->error().code == support::ErrorCode::Cancelled);
    // One cancellation, not one per late answer.
    CHECK(requests_of(*transport, "notifications/cancelled").size() == 1);

    // The connection is not poisoned by any of it: the next ordinary call on
    // the same client succeeds.
    auto next = tests::drive(client.call_tool(call()));
    REQUIRE(next.has_value());
    CHECK_FALSE(next->is_error);
}

TEST_CASE("a call made without a progress sink declares no token", "[mcp][progress][issue844][spec]") {
    auto transport = std::make_shared<tests::ScriptedMcpTransport>();
    auto client = connected(transport);
    transport->answer_with("tools/call", [](const JsonValue& params) {
        // The call really did frame no token, so the notification below names
        // one the host never minted and the drop is the token match rather
        // than the absence of a token.
        REQUIRE_FALSE(declared_token(params).has_value());
        return tests::ScriptedMcpAnswer{
                .result = completed(),
                .leading_messages = {tests::progress_notification("2", 1.0)},
        };
    });

    // Nothing is listening for progress, so the call declares no token and the
    // Upstream's notifications have nothing to match against.
    auto outcome = tests::drive(client.call_tool(call()));
    REQUIRE(outcome.has_value());
    CHECK_FALSE(outcome->is_error);
    const auto calls = requests_of(*transport, "tools/call");
    REQUIRE(calls.size() == 1);
    CHECK_FALSE(declared_token(calls.front()).has_value());
}

TEST_CASE("progress an Upstream reports for a call reaches that call's own sink",
        "[mcp][progress][issue844][spec]") {
    auto transport = std::make_shared<tests::ScriptedMcpTransport>();
    auto client = connected(transport);
    // A conforming Upstream echoes the token its request declared, and streams
    // progress ahead of the response it is waiting for.
    transport->answer_with("tools/call", [](const JsonValue& params) {
        const auto token = declared_token(params);
        REQUIRE(token.has_value());
        return tests::ScriptedMcpAnswer{
                .result = completed(),
                .leading_messages = {tests::progress_notification(*token, 3.0, 10.0, "compiling"),
                        tests::progress_notification(*token, 7.0, 10.0, "linking")},
        };
    });

    std::vector<mcp::UpstreamToolProgress> reported;
    auto outcome = tests::drive(
            client.call_tool(call(), {}, mcp::UpstreamProgressSink([&reported](const mcp::UpstreamToolProgress& update) {
                reported.push_back(update);
            })));
    REQUIRE(outcome.has_value());
    CHECK_FALSE(outcome->is_error);

    REQUIRE(reported.size() == 2);
    CHECK(reported[0].progress == 3.0);
    REQUIRE(reported[0].total.has_value());
    CHECK(*reported[0].total == 10.0);
    CHECK(reported[0].message == "compiling");
    CHECK(reported[1].progress == 7.0);
    CHECK(reported[1].message == "linking");
    // The progress reached the display; the result the model sees is still the
    // Upstream's own content and carries none of it.
    CHECK(outcome->content.get_array().at(0).at("text").get_string() == "ABC-1");
}

TEST_CASE("a progress notification naming another call is dropped rather than shown against the running one",
        "[mcp][progress][issue844][spec]") {
    auto transport = std::make_shared<tests::ScriptedMcpTransport>();
    auto client = connected(transport);
    // The first `tools/call` records the token it declared; the second is
    // answered with progress naming the *first* call's token and a token no
    // live call ever declared, which is what a lagging or hostile Upstream
    // produces.
    auto first_token = std::make_shared<std::optional<std::string>>();
    auto served = std::make_shared<std::size_t>(0);
    transport->answer_with("tools/call", [first_token, served](const JsonValue& params) {
        if (*served == 0) {
            *served += 1;
            *first_token = declared_token(params);
            return tests::ScriptedMcpAnswer{.result = completed()};
        }
        *served += 1;
        std::vector<JsonValue> leading;
        if (first_token->has_value()) {
            leading.push_back(tests::progress_notification(**first_token, 99.0, 100.0, "not yours"));
        }
        leading.push_back(tests::progress_notification("9999", 1.0, 2.0, "unknown call"));
        return tests::ScriptedMcpAnswer{.result = completed(), .leading_messages = std::move(leading)};
    });

    std::vector<mcp::UpstreamToolProgress> first_reported;
    auto first = tests::drive(
            client.call_tool(call(), {}, mcp::UpstreamProgressSink([&first_reported](const mcp::UpstreamToolProgress& u) {
                first_reported.push_back(u);
            })));
    REQUIRE(first.has_value());
    REQUIRE(first_token->has_value());

    std::vector<mcp::UpstreamToolProgress> second_reported;
    auto second = tests::drive(
            client.call_tool(call(), {}, mcp::UpstreamProgressSink([&second_reported](const mcp::UpstreamToolProgress& u) {
                second_reported.push_back(u);
            })));
    REQUIRE(second.has_value());
    CHECK_FALSE(second->is_error);

    // The second call's display shows the second call and nothing else: a
    // notification for a finished call and one for a call that never existed
    // are both dropped rather than shown as this call's own progress.
    CHECK(first_reported.empty());
    CHECK(second_reported.empty());
}

TEST_CASE("a progress notification is bounded and redacted before it reaches a display",
        "[mcp][progress][issue844][spec]") {
    auto transport = std::make_shared<tests::ScriptedMcpTransport>();
    auto client = connected(transport);
    // The secret-shaped key is at the *head* of the message and the flood
    // follows it, so a redacted-then-bounded implementation is the only one
    // that can leave a complete `[REDACTED]` marker in a 1 KiB bound, and a
    // bounded-then-redacted one would have cut the key away entirely and
    // proved nothing.
    const std::string flood(4096, 'p');
    transport->answer_with("tools/call", [flood](const JsonValue& params) {
        const auto token = declared_token(params);
        REQUIRE(token.has_value());
        return tests::ScriptedMcpAnswer{
                .result = completed(),
                .leading_messages = {tests::progress_notification(
                        *token, 1.0, 2.0, std::string{" \"api_key\": \"sk-live-must-never-survive\" "} + flood)},
        };
    });

    std::vector<mcp::UpstreamToolProgress> reported;
    auto outcome = tests::drive(
            client.call_tool(call(), {}, mcp::UpstreamProgressSink([&reported](const mcp::UpstreamToolProgress& u) {
                reported.push_back(u);
            })));
    REQUIRE(outcome.has_value());
    REQUIRE(reported.size() == 1);
    // Bounded and redacted, in that order: the secret is gone rather than
    // present inside the first kilobyte.
    CHECK(reported.front().message.size() <= 1024);
    CHECK(reported.front().message.find("sk-live-must-never-survive") == std::string::npos);
    CHECK(reported.front().message.find("[REDACTED]") != std::string::npos);
}

TEST_CASE("a connection close during an in-flight call leaves no suspended upstream call",
        "[mcp][cancellation][issue844][spec]") {
    auto transport = std::make_shared<tests::ScriptedMcpTransport>();
    tests::ScriptedMcpDelay clock;
    mcp::UpstreamConnection connection{"executor",
            transport,
            mcp::UpstreamConnectionOptions{
                    .url = std::string(kEndpoint),
                    .delay = [&clock](std::chrono::milliseconds delay, std::stop_token stop_token) {
                        return clock.request(delay, stop_token);
                    },
    }};
    transport->answer("server/discover", {.result = tests::discover_result()});
    transport->answer("tools/list", {.result = tests::tool_list_result({tests::tool_entry("read_issue")})});
    REQUIRE(tests::drive(connection.connect()).has_value());
    transport->hold("tools/call");

    std::optional<support::Expected<mcp::UpstreamToolCallResult>> outcome;
    connection.call_tool(call()).start([&](std::expected<mcp::UpstreamToolCallResult, support::Error> value) noexcept {
        outcome = std::move(value);
    });
    REQUIRE_FALSE(outcome.has_value());
    REQUIRE(connection.snapshot().in_flight == 1);

    // The close stops admission and requests cancellation of the admitted call,
    // which the transport answers: nothing is left suspended, so the cleanup
    // bound is never needed.
    auto closed = tests::drive(connection.close());
    REQUIRE(closed.has_value());
    CHECK(closed->within_bound);
    CHECK(closed->abandoned_operations == 0);
    CHECK(clock.waiting() == 0);
    CHECK(connection.snapshot().in_flight == 0);

    REQUIRE(outcome.has_value());
    CHECK_FALSE(*outcome);
    CHECK(outcome->error().code == support::ErrorCode::Cancelled);
    // And the Upstream was told, rather than being left to finish work the
    // closed stream abandoned.
    CHECK(requests_of(*transport, "notifications/cancelled").size() == 1);
    // A closed connection admits nothing further, and a repeat close reports the
    // same outcome rather than starting a second cleanup.
    CHECK(tests::drive(connection.call_tool(call())).error().code == support::ErrorCode::Cancelled);
    auto again = tests::drive(connection.close());
    REQUIRE(again.has_value());
    CHECK(again->abandoned_operations == 0);
}
