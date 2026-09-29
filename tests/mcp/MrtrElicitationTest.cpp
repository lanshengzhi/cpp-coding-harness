// The MCP Host's Multi Round-Trip continuation loop (issue #845; spec #833
// stories 27-29, and the story-32 regression half of the defensive matrix).
//
// Every case drives the full client stack above the one injected transport
// seam (`tests::ScriptedMcpTransport`) and answers Pending Elicitations through
// the one elicitation port (`tests::ScriptedMcpElicitation`), with the wait
// bound on the same `ScriptedMcpDelay` the connection's own waits use. The
// assertions are on wire bytes and on the tool-call outcome, never on the
// loop's internal state.
//
// The four claims this file exists to pin:
//   * the retried request carries a NEW JSON-RPC id, the ORIGINAL arguments,
//     the user's answer, and the opaque `requestState` byte-for-byte;
//   * a multi-round exchange is bounded and each round re-arms the wait;
//   * decline, cancel, the elicitation bound, and the session's stop token
//     all end the call with NO re-send, and a late answer is discarded;
//   * an undeclared input-request type fails exactly one call and never asks
//     the user (ADR 0008).

#include <cch/mcp/UpstreamClient.hpp>
#include <cch/mcp/UpstreamConnection.hpp>
#include "mcp/JsonRpc.hpp"
#include "mcp/Protocol.hpp"
#include "mcp/WireDto.hpp"
#include "support/Json.hpp"
#include "support/ScriptedMcpElicitation.hpp"
#include "support/ScriptedMcpTransport.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

using namespace cch;
using support::JsonValue;
using tests::ScriptedMcpAnswer;

namespace {

/// A `tools/call` result that suspends the call for one input request.
[[nodiscard]] JsonValue input_required_result(
        std::string request_state, std::vector<JsonValue> input_requests, int status = 200) {
    (void)status;
    JsonValue::object_t result{
            {"resultType", JsonValue("input_required")},
            {"requestState", JsonValue(std::move(request_state))},
            {"inputRequests", JsonValue(std::move(input_requests))},
    };
    return JsonValue(std::move(result));
}

/// One URL-mode input request.
[[nodiscard]] JsonValue url_request(std::string id = "r1",
        std::string url = "https://executor.invalid/mcp/approve/abc",
        std::string message = "Approve this action") {
    return JsonValue::object_t{
            {"id", JsonValue(std::move(id))},
            {"type", JsonValue("url")},
            {"message", JsonValue(std::move(message))},
            {"url", JsonValue(std::move(url))},
    };
}

/// One form-mode input request.
[[nodiscard]] JsonValue form_request(std::string id = "r1") {
    return JsonValue::object_t{
            {"id", JsonValue(std::move(id))},
            {"type", JsonValue("form")},
            {"message", JsonValue("Confirm the scope")},
            {"schema", JsonValue::object_t{{"type", JsonValue("object")}}},
    };
}

/// A connecting Upstream whose `tools/call` is answered by `script`, with the
/// elicitation port and timer the case supplies. Everything the assertions
/// need is on the transport: the exact bytes each `tools/call` carried.
struct ElicitationFixture {
    std::shared_ptr<tests::ScriptedMcpTransport> transport{std::make_shared<tests::ScriptedMcpTransport>()};
    tests::ScriptedMcpDelay delay{};
    tests::ScriptedMcpElicitation elicitation{};
    std::shared_ptr<mcp::UpstreamConnection> connection{};

    ElicitationFixture(std::shared_ptr<mcp::UpstreamElicitationPort> port) {
        transport->answer("server/discover", ScriptedMcpAnswer{.result = tests::discover_result()});
        transport->answer("tools/list", ScriptedMcpAnswer{.result = tests::tool_list_result({})});
        connection = std::make_shared<mcp::UpstreamConnection>("executor",
                transport,
                mcp::UpstreamConnectionOptions{
                        .url = "https://mcp.example/mcp",
                        .delay = [this](std::chrono::milliseconds wait,
                                         std::stop_token stop_token) { return delay.request(wait, stop_token); },
                        .elicitation = std::move(port),
                });
        const auto connected = drive(connection->connect());
        REQUIRE(connected.has_value());
    }

    template <typename T> [[nodiscard]] static support::Expected<T> drive(support::AsyncResult<T> operation) {
        return tests::drive(std::move(operation));
    }

    /// The `tools/call` request bodies, in order, as the Upstream received
    /// them.
    [[nodiscard]] std::vector<std::string> call_bodies() const {
        std::vector<std::string> bodies;
        for (std::size_t index = 0; index < transport->request_count(); ++index) {
            const auto method = transport->recorded_method(index);
            REQUIRE(method.has_value());
            if (*method == "tools/call") {
                bodies.push_back(transport->requests().at(index).body);
            }
        }
        return bodies;
    }

    /// One recorded `tools/call`'s JSON-RPC id, so a fresh id per round is
    /// observable rather than inferred.
    [[nodiscard]] double call_id(std::size_t call_index) const {
        const auto bodies = call_bodies();
        REQUIRE(call_index < bodies.size());
        const auto parsed = support::read_json(bodies[call_index]);
        REQUIRE(parsed.has_value());
        return parsed->at("id").get<double>();
    }
};

/// A `tools/call` the fixture can issue.
[[nodiscard]] mcp::UpstreamToolCall post_message() {
    return mcp::UpstreamToolCall{
            .tool = mcp::UpstreamToolDescriptor{.name = "post_message"},
            .arguments = JsonValue::object_t{{"channel", JsonValue("ops")}},
    };
}

} // namespace

TEST_CASE("a URL elicitation is asked, answered, and the original call continues", "[mcp][issue845][spec]") {
    auto elicitation = std::make_shared<tests::ScriptedMcpElicitation>();
    elicitation->answer(mcp::ElicitationAction::Accept);
    std::size_t call_round = 0;
    // First round suspends; the retry completes. The port answers inline, so
    // the whole exchange settles before the call returns.
    auto fixture_transport = std::make_shared<tests::ScriptedMcpTransport>();
    fixture_transport->answer("server/discover", ScriptedMcpAnswer{.result = tests::discover_result()});
    fixture_transport->answer("tools/list", ScriptedMcpAnswer{.result = tests::tool_list_result({})});
    fixture_transport->answer_with("tools/call", [call_round = &call_round](const JsonValue& params) mutable {
        (void)params;
        if ((*call_round)++ == 0) {
            return ScriptedMcpAnswer{
                    .result = input_required_result(R"({"token":"opaque-1","seq":7})", {url_request()})};
        }
        return ScriptedMcpAnswer{.result = tests::tool_call_result(JsonValue::object_t{
                                         {"type", JsonValue("text")}, {"text", JsonValue("approved")}})};
    });

    tests::ScriptedMcpDelay delay;
    auto connection = std::make_shared<mcp::UpstreamConnection>("executor",
            fixture_transport,
            mcp::UpstreamConnectionOptions{
                    .url = "https://mcp.example/mcp",
                    .delay = [&delay](std::chrono::milliseconds wait,
                                     std::stop_token stop_token) { return delay.request(wait, stop_token); },
                    .elicitation = elicitation->port(std::chrono::seconds{30}, delay),
            });
    REQUIRE(tests::drive(connection->connect()).has_value());

    const auto called = tests::drive(connection->call_tool(post_message()));
    REQUIRE(called.has_value());
    CHECK_FALSE(called->is_error);
    CHECK(called->diagnostic.empty());
    // The model sees the Upstream's own completed content, not the
    // elicitation that preceded it.
    CHECK(support::write_json(called->content).value_or("").find("approved") != std::string::npos);

    // The user was asked exactly one question, and it was a URL-mode one that
    // showed the address to visit.
    REQUIRE(elicitation->question_count() == 1);
    const auto& question = elicitation->asked().front().request;
    CHECK(question.mode == mcp::ElicitationMode::Url);
    CHECK(question.url == "https://executor.invalid/mcp/approve/abc");
    CHECK(question.message == "Approve this action");
    CHECK(question.server_id == "executor");
    CHECK(question.tool_name == "post_message");
    CHECK(question.request_id == "r1");
    // The opaque token is never put in front of the user: it is the server's
    // own bookkeeping, and the host does not interpret it.
    CHECK(question.url.find("opaque-1") == std::string::npos);

    // Two `tools/call` requests: the original and the continuation.
    const auto bodies = [&fixture_transport] {
        std::vector<std::string> recorded;
        for (std::size_t index = 0; index < fixture_transport->request_count(); ++index) {
            const auto method = fixture_transport->recorded_method(index);
            REQUIRE(method.has_value());
            if (*method == "tools/call") {
                recorded.push_back(fixture_transport->requests().at(index).body);
            }
        }
        return recorded;
    }();
    REQUIRE(bodies.size() == 2);
    // A fresh JSON-RPC id per round, and the original name and arguments.
    const auto first_id = support::read_json(bodies[0])->at("id").get<double>();
    const auto second_id = support::read_json(bodies[1])->at("id").get<double>();
    CHECK(second_id != first_id);
    const auto retried = support::read_json(bodies[1]);
    REQUIRE(retried.has_value());
    const auto& params = retried->at("params").get_object();
    CHECK(params.at("name").get<std::string>() == "post_message");
    CHECK(params.at("arguments").get_object().at("channel").get<std::string>() == "ops");
    // The answer, keyed by the request the Upstream named.
    const auto& responses = params.at("inputResponses").get_array();
    REQUIRE(responses.size() == 1);
    CHECK(responses.front().at("action").get<std::string>() == "accept");
    CHECK(responses.front().at("id").get<std::string>() == "r1");
}

TEST_CASE("the opaque requestState is echoed byte-for-byte on every retry", "[mcp][issue845][spec]") {
    auto elicitation = std::make_shared<tests::ScriptedMcpElicitation>();
    elicitation->answer(mcp::ElicitationAction::Accept);
    // A token chosen so that parsing and re-serializing it would change the
    // bytes: members out of the order a value tree would sort them into, a
    // number a double would reformat, an escape a serializer would rewrite,
    // and whitespace no serializer emits.
    // The bytes on the wire are written out by hand, because a value tree
    // cannot express them: a serializer would sort the members, reformat the
    // numbers, and drop the whitespace, so a retry built by re-serializing the
    // token would carry different bytes than this one.
    const std::string token = R"({ "z" : 1 , "a" : [ 1.50 , 1e2 ] , "s" : "line\nbreak é" })";
    const auto suspension = [&token](std::size_t round) {
        return R"({"resultType":"input_required","requestState":)" + token + R"(,"inputRequests":[{"id":"r)" +
               std::to_string(round) +
               R"(","type":"url","message":"Approve",)"
               R"("url":"https://executor.invalid/approve"}]})";
    };
    std::size_t round = 0;
    auto transport = std::make_shared<tests::ScriptedMcpTransport>();
    transport->answer("server/discover", ScriptedMcpAnswer{.result = tests::discover_result()});
    transport->answer("tools/list", ScriptedMcpAnswer{.result = tests::tool_list_result({})});
    transport->answer_with("tools/call", [&round, suspension](const JsonValue&) mutable {
        if (round++ < 2) {
            return ScriptedMcpAnswer{.raw_result = suspension(round)};
        }
        return ScriptedMcpAnswer{.result = tests::tool_call_result(JsonValue::object_t{{"text", JsonValue("done")}})};
    });

    tests::ScriptedMcpDelay delay;
    auto connection = std::make_shared<mcp::UpstreamConnection>("executor",
            transport,
            mcp::UpstreamConnectionOptions{
                    .url = "https://mcp.example/mcp",
                    .delay = [&delay](std::chrono::milliseconds wait,
                                     std::stop_token stop_token) { return delay.request(wait, stop_token); },
                    .elicitation = elicitation->port(std::chrono::seconds{30}, delay),
            });
    REQUIRE(tests::drive(connection->connect()).has_value());
    const auto called = tests::drive(connection->call_tool(post_message()));
    REQUIRE(called.has_value());
    CHECK_FALSE(called->is_error);

    std::vector<std::string> bodies;
    for (std::size_t index = 0; index < transport->request_count(); ++index) {
        const auto method = transport->recorded_method(index);
        REQUIRE(method.has_value());
        if (*method == "tools/call") {
            bodies.push_back(transport->requests().at(index).body);
        }
    }
    // Three rounds: the original plus two continuations, and the token is on
    // the wire in exactly the bytes the server sent.
    REQUIRE(bodies.size() == 3);
    for (const auto& body : bodies) {
        if (body.find("requestState") == std::string::npos) {
            continue;
        }
        CHECK(body.find("\"requestState\":" + token) != std::string::npos);
    }
    // A fresh id per round, monotonically, and never a reused one.
    CHECK(support::read_json(bodies[1])->at("id").get<double>() !=
            support::read_json(bodies[0])->at("id").get<double>());
    CHECK(support::read_json(bodies[2])->at("id").get<double>() !=
            support::read_json(bodies[1])->at("id").get<double>());
    // The original arguments survive every round untouched.
    for (const auto& body : bodies) {
        const auto parsed = support::read_json(body);
        REQUIRE(parsed.has_value());
        CHECK(parsed->at("params").at("arguments").at("channel").get<std::string>() == "ops");
    }
    // Both questions were asked, one per round.
    CHECK(elicitation->question_count() == 2);
}

TEST_CASE("a multi-request suspension is asked in order and every answer is echoed", "[mcp][issue845][spec]") {
    auto elicitation = std::make_shared<tests::ScriptedMcpElicitation>();
    elicitation->answer_with([](const mcp::ElicitationRequest& request) {
        return support::Expected<mcp::ElicitationAnswer>{mcp::ElicitationAnswer{
                .action = request.mode == mcp::ElicitationMode::Url ? mcp::ElicitationAction::Accept
                                                                    : mcp::ElicitationAction::Decline,
                .request_id = request.request_id,
                .form_content = request.mode == mcp::ElicitationMode::Form
                                        ? JsonValue::object_t{{"scope", JsonValue("read")}}
                                        : JsonValue{},
        }};
    });
    std::size_t round = 0;
    auto transport = std::make_shared<tests::ScriptedMcpTransport>();
    transport->answer("server/discover", ScriptedMcpAnswer{.result = tests::discover_result()});
    transport->answer("tools/list", ScriptedMcpAnswer{.result = tests::tool_list_result({})});
    transport->answer_with("tools/call", [&round](const JsonValue&) mutable {
        if (round++ == 0) {
            return ScriptedMcpAnswer{.result = input_required_result(
                                             R"({"token":"two"})", {url_request("url-1"), form_request("form-1")})};
        }
        return ScriptedMcpAnswer{.result = tests::tool_call_result(JsonValue::object_t{{"text", JsonValue("done")}})};
    });

    tests::ScriptedMcpDelay delay;
    auto connection = std::make_shared<mcp::UpstreamConnection>("executor",
            transport,
            mcp::UpstreamConnectionOptions{
                    .url = "https://mcp.example/mcp",
                    .delay = [&delay](std::chrono::milliseconds wait,
                                     std::stop_token stop_token) { return delay.request(wait, stop_token); },
                    .elicitation = elicitation->port(std::chrono::seconds{30}, delay),
            });
    REQUIRE(tests::drive(connection->connect()).has_value());
    const auto called = tests::drive(connection->call_tool(post_message()));
    REQUIRE(called.has_value());
    CHECK_FALSE(called->is_error);

    // Both questions were asked, in the order the result declared them, and
    // the form request carried the server's own schema uninterpreted.
    REQUIRE(elicitation->question_count() == 2);
    CHECK(elicitation->asked()[0].request.mode == mcp::ElicitationMode::Url);
    CHECK(elicitation->asked()[1].request.mode == mcp::ElicitationMode::Form);
    CHECK(elicitation->asked()[1].request.form_schema.at("type").get<std::string>() == "object");

    // One continuation, carrying both answers in the declared order.
    std::optional<std::string> retry;
    for (std::size_t index = 0; index < transport->request_count(); ++index) {
        const auto method = transport->recorded_method(index);
        REQUIRE(method.has_value());
        if (*method == "tools/call" && index > 0) {
            retry = transport->requests().at(index).body;
        }
    }
    REQUIRE(retry.has_value());
    const auto parsed = support::read_json(*retry);
    REQUIRE(parsed.has_value());
    const auto& responses = parsed->at("params").at("inputResponses").get_array();
    REQUIRE(responses.size() == 2);
    CHECK(responses[0].at("id").get<std::string>() == "url-1");
    CHECK(responses[0].at("action").get<std::string>() == "accept");
    CHECK(responses[1].at("id").get<std::string>() == "form-1");
    CHECK(responses[1].at("action").get<std::string>() == "decline");
    CHECK(responses[1].at("content").at("scope").get<std::string>() == "read");
}

TEST_CASE("a declined elicitation is still an answer the Upstream is told about", "[mcp][issue845][spec]") {
    auto elicitation = std::make_shared<tests::ScriptedMcpElicitation>();
    elicitation->answer(mcp::ElicitationAction::Decline);
    std::size_t round = 0;
    auto transport = std::make_shared<tests::ScriptedMcpTransport>();
    transport->answer("server/discover", ScriptedMcpAnswer{.result = tests::discover_result()});
    transport->answer("tools/list", ScriptedMcpAnswer{.result = tests::tool_list_result({})});
    transport->answer_with("tools/call", [&round](const JsonValue&) mutable {
        if (round++ == 0) {
            return ScriptedMcpAnswer{.result = input_required_result(R"({"token":"d"})", {url_request()})};
        }
        return ScriptedMcpAnswer{.result = tests::tool_call_result(JsonValue::object_t{{"text", JsonValue("denied")}})};
    });
    tests::ScriptedMcpDelay delay;
    auto connection = std::make_shared<mcp::UpstreamConnection>("executor",
            transport,
            mcp::UpstreamConnectionOptions{
                    .url = "https://mcp.example/mcp",
                    .delay = [&delay](std::chrono::milliseconds wait,
                                     std::stop_token stop_token) { return delay.request(wait, stop_token); },
                    .elicitation = elicitation->port(std::chrono::seconds{30}, delay),
            });
    REQUIRE(tests::drive(connection->connect()).has_value());
    const auto called = tests::drive(connection->call_tool(post_message()));
    REQUIRE(called.has_value());
    CHECK_FALSE(called->is_error);
    // The server was told the user declined, rather than the call being
    // abandoned: a decline is an answer, and the server's flow needs it.
    const auto bodies = [&transport] {
        std::vector<std::string> recorded;
        for (std::size_t index = 0; index < transport->request_count(); ++index) {
            if (*transport->recorded_method(index) == "tools/call") {
                recorded.push_back(transport->requests().at(index).body);
            }
        }
        return recorded;
    }();
    REQUIRE(bodies.size() == 2);
    CHECK(bodies[1].find(R"("action":"decline")") != std::string::npos);
}

TEST_CASE("a cancelled elicitation is answered as a cancel and the call continues", "[mcp][issue845][spec]") {
    auto elicitation = std::make_shared<tests::ScriptedMcpElicitation>();
    elicitation->answer(mcp::ElicitationAction::Cancel);
    std::size_t round = 0;
    auto transport = std::make_shared<tests::ScriptedMcpTransport>();
    transport->answer("server/discover", ScriptedMcpAnswer{.result = tests::discover_result()});
    transport->answer("tools/list", ScriptedMcpAnswer{.result = tests::tool_list_result({})});
    transport->answer_with("tools/call", [&round](const JsonValue&) mutable {
        if (round++ == 0) {
            return ScriptedMcpAnswer{.result = input_required_result(R"({"token":"c"})", {url_request()})};
        }
        return ScriptedMcpAnswer{
                .result = tests::tool_call_result(JsonValue::object_t{{"text", JsonValue("cancelled")}})};
    });
    tests::ScriptedMcpDelay delay;
    auto connection = std::make_shared<mcp::UpstreamConnection>("executor",
            transport,
            mcp::UpstreamConnectionOptions{
                    .url = "https://mcp.example/mcp",
                    .delay = [&delay](std::chrono::milliseconds wait,
                                     std::stop_token stop_token) { return delay.request(wait, stop_token); },
                    .elicitation = elicitation->port(std::chrono::seconds{30}, delay),
            });
    REQUIRE(tests::drive(connection->connect()).has_value());
    const auto called = tests::drive(connection->call_tool(post_message()));
    REQUIRE(called.has_value());
    CHECK_FALSE(called->is_error);
    // A user who cancels is telling the server something; the difference from
    // a *stopped* wait is that a stopped wait sends nothing at all.
    const auto bodies = [&transport] {
        std::vector<std::string> recorded;
        for (std::size_t index = 0; index < transport->request_count(); ++index) {
            if (*transport->recorded_method(index) == "tools/call") {
                recorded.push_back(transport->requests().at(index).body);
            }
        }
        return recorded;
    }();
    REQUIRE(bodies.size() == 2);
    CHECK(bodies[1].find(R"("action":"cancel")") != std::string::npos);
}

TEST_CASE(
        "an unanswered elicitation reaches its bound, fails one call, and re-sends nothing", "[mcp][issue845][spec]") {
    auto elicitation = std::make_shared<tests::ScriptedMcpElicitation>();
    // Nothing is scripted, so the port leaves the wait pending.
    auto transport = std::make_shared<tests::ScriptedMcpTransport>();
    transport->answer("server/discover", ScriptedMcpAnswer{.result = tests::discover_result()});
    transport->answer("tools/list", ScriptedMcpAnswer{.result = tests::tool_list_result({})});
    transport->answer(
            "tools/call", ScriptedMcpAnswer{.result = input_required_result(R"({"token":"t"})", {url_request()})});
    tests::ScriptedMcpDelay delay;
    auto connection = std::make_shared<mcp::UpstreamConnection>("executor",
            transport,
            mcp::UpstreamConnectionOptions{
                    .url = "https://mcp.example/mcp",
                    .delay = [&delay](std::chrono::milliseconds wait,
                                     std::stop_token stop_token) { return delay.request(wait, stop_token); },
                    .elicitation = elicitation->port(std::chrono::seconds{30}, delay),
            });
    REQUIRE(tests::drive(connection->connect()).has_value());

    support::Expected<mcp::UpstreamToolCallResult> outcome =
            std::unexpected(support::make_error(support::ErrorCode::Busy, "the call never completed"));
    auto call = connection->call_tool(post_message());
    call.start([&outcome](std::expected<mcp::UpstreamToolCallResult, support::Error> value) mutable noexcept {
        outcome = std::move(value);
    });
    // The wait is pending on the port, and the bound is armed on the same
    // scripted timer: the user was asked and has not answered.
    REQUIRE(elicitation->question_count() == 1);
    CHECK(elicitation->waiting(0));
    CHECK(delay.waiting() == 1);
    CHECK(delay.next_delay() == std::chrono::seconds{30});

    // The bound elapses.
    REQUIRE(delay.elapse_oldest());
    REQUIRE(outcome.has_value());
    CHECK(outcome->is_error);
    CHECK(outcome->diagnostic.find("not answered") != std::string::npos);
    // Exactly one tools/call: the suspended call is over and nothing was sent
    // in the user's name.
    CHECK(transport->request_count("tools/call") == 1);

    // A late answer is discarded rather than completing anything a second
    // time: the call is already settled, and no re-send follows it.
    elicitation->answer_late(0, mcp::ElicitationAction::Accept);
    CHECK(transport->request_count("tools/call") == 1);
}

TEST_CASE("stopping the call during an elicitation leaves no suspended call and no re-send", "[mcp][issue845][spec]") {
    auto elicitation = std::make_shared<tests::ScriptedMcpElicitation>();
    auto transport = std::make_shared<tests::ScriptedMcpTransport>();
    transport->answer("server/discover", ScriptedMcpAnswer{.result = tests::discover_result()});
    transport->answer("tools/list", ScriptedMcpAnswer{.result = tests::tool_list_result({})});
    transport->answer(
            "tools/call", ScriptedMcpAnswer{.result = input_required_result(R"({"token":"s"})", {url_request()})});
    tests::ScriptedMcpDelay delay;
    auto connection = std::make_shared<mcp::UpstreamConnection>("executor",
            transport,
            mcp::UpstreamConnectionOptions{
                    .url = "https://mcp.example/mcp",
                    .delay = [&delay](std::chrono::milliseconds wait,
                                     std::stop_token stop_token) { return delay.request(wait, stop_token); },
                    .elicitation = elicitation->port(std::chrono::seconds{30}, delay),
            });
    REQUIRE(tests::drive(connection->connect()).has_value());

    // The two-phase close is what stops a session; the wait it leaves behind
    // is the case under test.
    std::stop_source session_stop;
    support::Expected<mcp::UpstreamToolCallResult> outcome =
            std::unexpected(support::make_error(support::ErrorCode::Busy, "the call never completed"));
    auto call = connection->call_tool(post_message(), session_stop.get_token());
    call.start([&outcome](std::expected<mcp::UpstreamToolCallResult, support::Error> value) mutable noexcept {
        outcome = std::move(value);
    });
    REQUIRE(elicitation->question_count() == 1);
    CHECK(elicitation->waiting(0));
    CHECK(delay.waiting() == 1);

    support::Expected<mcp::UpstreamCloseOutcome> closed =
            std::unexpected(support::make_error(support::ErrorCode::Busy, "the close never completed"));
    auto closing = connection->close();
    closing.start([&closed](std::expected<mcp::UpstreamCloseOutcome, support::Error> value) mutable noexcept {
        closed = std::move(value);
    });
    // The close stopped the call and armed its own cleanup bound, so two waits
    // stand on the scripted timer; the elicitation wait is the older of them.
    REQUIRE(delay.waiting() == 2);
    REQUIRE(delay.elapse_oldest());
    // The close reached the wait and the call settled inside the connection's
    // own bound: no suspended call is left behind, and the elicitation bound
    // is not what ended it.
    REQUIRE(closed.has_value());
    CHECK(closed->within_bound);
    CHECK(closed->abandoned_operations == 0);
    CHECK(outcome.has_value());
    CHECK(outcome->is_error);
    CHECK(transport->request_count("tools/call") == 1);
    // The cleanup bound was not reached: the call quiesced on the stop itself.
    CHECK(delay.waiting() == 1);

    // A late answer after the close is discarded and sends nothing.
    elicitation->answer_late(0, mcp::ElicitationAction::Accept);
    CHECK(transport->request_count("tools/call") == 1);
}

TEST_CASE("an undeclared input-request type fails one call and never asks the user", "[mcp][issue845][spec]") {
    auto elicitation = std::make_shared<tests::ScriptedMcpElicitation>();
    elicitation->answer(mcp::ElicitationAction::Accept);
    auto transport = std::make_shared<tests::ScriptedMcpTransport>();
    transport->answer("server/discover", ScriptedMcpAnswer{.result = tests::discover_result()});
    transport->answer("tools/list", ScriptedMcpAnswer{.result = tests::tool_list_result({})});
    // The `type` this build does not declare, and a second `tools/call` that
    // is an ordinary call, so the failure's cost is measurable.
    transport->answer_with("tools/call", [](const JsonValue&) {
        static std::size_t round = 0;
        if (round++ == 0) {
            return ScriptedMcpAnswer{
                    .result = input_required_result(R"({"token":"u"})",
                            {JsonValue::object_t{{"id", JsonValue("r1")}, {"type", JsonValue("websocket")}}})};
        }
        return ScriptedMcpAnswer{
                .result = tests::tool_call_result(JsonValue::object_t{{"text", JsonValue("still works")}})};
    });
    tests::ScriptedMcpDelay delay;
    auto connection = std::make_shared<mcp::UpstreamConnection>("executor",
            transport,
            mcp::UpstreamConnectionOptions{
                    .url = "https://mcp.example/mcp",
                    .delay = [&delay](std::chrono::milliseconds wait,
                                     std::stop_token stop_token) { return delay.request(wait, stop_token); },
                    .elicitation = elicitation->port(std::chrono::seconds{30}, delay),
            });
    REQUIRE(tests::drive(connection->connect()).has_value());

    const auto refused = tests::drive(connection->call_tool(post_message()));
    REQUIRE(refused.has_value());
    CHECK(refused->is_error);
    CHECK(refused->diagnostic.find("does not declare") != std::string::npos);
    // The user was never asked: an undeclared type is not a mode this build
    // guesses at, so nothing was put in front of anyone.
    CHECK(elicitation->question_count() == 0);

    // The connection is untouched: the next ordinary call still succeeds, and
    // the failure cost no re-probe and no re-list.
    const auto ordinary = tests::drive(connection->call_tool(post_message()));
    REQUIRE(ordinary.has_value());
    CHECK_FALSE(ordinary->is_error);
    CHECK(transport->request_count("server/discover") == 1);
    CHECK(transport->request_count("tools/call") == 2);
}

TEST_CASE("an unanswerable input_required result fails one call and never asks the user", "[mcp][issue845][spec]") {
    auto elicitation = std::make_shared<tests::ScriptedMcpElicitation>();
    elicitation->answer(mcp::ElicitationAction::Accept);
    using JsonValue = support::JsonValue;
    // The three shapes this build refuses rather than answers: no
    // continuation token, no input requests, and an input request that is not
    // an object. Each is a protocol violation, and each is local.
    const std::vector<JsonValue> broken{
            JsonValue::object_t{
                    {"resultType", JsonValue("input_required")}, {"inputRequests", JsonValue::array_t{url_request()}}},
            JsonValue::object_t{{"resultType", JsonValue("input_required")},
                    {"requestState", JsonValue("t")},
                    {"inputRequests", JsonValue::array_t{}}},
            JsonValue::object_t{{"resultType", JsonValue("input_required")},
                    {"requestState", JsonValue("t")},
                    {"inputRequests", JsonValue::array_t{JsonValue("not-an-object")}}},
    };
    auto transport = std::make_shared<tests::ScriptedMcpTransport>();
    transport->answer("server/discover", ScriptedMcpAnswer{.result = tests::discover_result()});
    transport->answer("tools/list", ScriptedMcpAnswer{.result = tests::tool_list_result({})});
    transport->answer("tools/call", ScriptedMcpAnswer{.result = broken.front()});
    tests::ScriptedMcpDelay delay;
    auto connection = std::make_shared<mcp::UpstreamConnection>("executor",
            transport,
            mcp::UpstreamConnectionOptions{
                    .url = "https://mcp.example/mcp",
                    .delay = [&delay](std::chrono::milliseconds wait,
                                     std::stop_token stop_token) { return delay.request(wait, stop_token); },
                    .elicitation = elicitation->port(std::chrono::seconds{30}, delay),
            });
    REQUIRE(tests::drive(connection->connect()).has_value());

    for (const auto& result : broken) {
        transport->answer("tools/call", ScriptedMcpAnswer{.result = result});
        const auto refused = tests::drive(connection->call_tool(post_message()));
        REQUIRE(refused.has_value());
        CHECK(refused->is_error);
        CHECK_FALSE(refused->diagnostic.empty());
    }
    CHECK(elicitation->question_count() == 0);
    // One failed call per refusal and no continuation for any of them.
    CHECK(transport->request_count("tools/call") == broken.size());
}

TEST_CASE("a server that keeps suspending is bounded in rounds", "[mcp][issue845][spec]") {
    auto elicitation = std::make_shared<tests::ScriptedMcpElicitation>();
    elicitation->answer(mcp::ElicitationAction::Accept);
    auto transport = std::make_shared<tests::ScriptedMcpTransport>();
    transport->answer("server/discover", ScriptedMcpAnswer{.result = tests::discover_result()});
    transport->answer("tools/list", ScriptedMcpAnswer{.result = tests::tool_list_result({})});
    transport->answer_with("tools/call", [](const JsonValue&) {
        static std::size_t round = 0;
        return ScriptedMcpAnswer{.result = input_required_result(R"({"token":")" + std::to_string(round++) + R"("})",
                                         {url_request("r" + std::to_string(round))})};
    });
    tests::ScriptedMcpDelay delay;
    auto connection = std::make_shared<mcp::UpstreamConnection>("executor",
            transport,
            mcp::UpstreamConnectionOptions{
                    .url = "https://mcp.example/mcp",
                    .delay = [&delay](std::chrono::milliseconds wait,
                                     std::stop_token stop_token) { return delay.request(wait, stop_token); },
                    .elicitation = elicitation->port(std::chrono::seconds{30}, delay),
            });
    REQUIRE(tests::drive(connection->connect()).has_value());

    const auto outcome = tests::drive(connection->call_tool(post_message()));
    REQUIRE(outcome.has_value());
    CHECK(outcome->is_error);
    CHECK(outcome->diagnostic.find("Multi Round-Trip bound") != std::string::npos);
    // Bounded rounds: the original plus exactly the declared cap, and no more.
    CHECK(transport->request_count("tools/call") == 9);
    CHECK(elicitation->question_count() == 8);
}

TEST_CASE("a port that cannot ask fails one call rather than waiting forever", "[mcp][issue845][spec]") {
    auto port = std::make_shared<mcp::UpstreamElicitationPort>();
    // The port is present but empty: a session that wired a value and never
    // filled it. A suspended call must still end.
    auto transport = std::make_shared<tests::ScriptedMcpTransport>();
    transport->answer("server/discover", ScriptedMcpAnswer{.result = tests::discover_result()});
    transport->answer("tools/list", ScriptedMcpAnswer{.result = tests::tool_list_result({})});
    transport->answer(
            "tools/call", ScriptedMcpAnswer{.result = input_required_result(R"({"token":"p"})", {url_request()})});
    tests::ScriptedMcpDelay delay;
    auto connection = std::make_shared<mcp::UpstreamConnection>("executor",
            transport,
            mcp::UpstreamConnectionOptions{
                    .url = "https://mcp.example/mcp",
                    .delay = [&delay](std::chrono::milliseconds wait,
                                     std::stop_token stop_token) { return delay.request(wait, stop_token); },
                    .elicitation = port,
            });
    REQUIRE(tests::drive(connection->connect()).has_value());
    const auto outcome = tests::drive(connection->call_tool(post_message()));
    REQUIRE(outcome.has_value());
    CHECK(outcome->is_error);
    CHECK(transport->request_count("tools/call") == 1);
}

TEST_CASE("a continuation token past the host's bound is refused, not truncated", "[mcp][issue845][spec]") {
    auto elicitation = std::make_shared<tests::ScriptedMcpElicitation>();
    elicitation->answer(mcp::ElicitationAction::Accept);
    auto transport = std::make_shared<tests::ScriptedMcpTransport>();
    transport->answer("server/discover", ScriptedMcpAnswer{.result = tests::discover_result()});
    transport->answer("tools/list", ScriptedMcpAnswer{.result = tests::tool_list_result({})});
    // Past kMaxRequestStateBytes: a truncated token would be a *different*
    // token, so the only safe response is to refuse this one call.
    const std::string huge = "\"" + std::string(64 * 1024, 'x') + "\"";
    transport->answer("tools/call", ScriptedMcpAnswer{.result = input_required_result(huge, {url_request()})});
    tests::ScriptedMcpDelay delay;
    auto connection = std::make_shared<mcp::UpstreamConnection>("executor",
            transport,
            mcp::UpstreamConnectionOptions{
                    .url = "https://mcp.example/mcp",
                    .delay = [&delay](std::chrono::milliseconds wait,
                                     std::stop_token stop_token) { return delay.request(wait, stop_token); },
                    .elicitation = elicitation->port(std::chrono::seconds{30}, delay),
            });
    REQUIRE(tests::drive(connection->connect()).has_value());
    const auto outcome = tests::drive(connection->call_tool(post_message()));
    REQUIRE(outcome.has_value());
    CHECK(outcome->is_error);
    CHECK(outcome->diagnostic.find("too long") != std::string::npos);
    CHECK(elicitation->question_count() == 0);
    CHECK(transport->request_count("tools/call") == 1);
}

TEST_CASE("an elicitation the raw member reader cannot frame is a failed call, not a guessed token",
        "[mcp][issue845][spec]") {
    // The decoder is the guard: a result whose `requestState` is present but
    // whose source text cannot be recovered verbatim is refused outright.
    JsonValue::object_t result{
            {"resultType", JsonValue("input_required")},
            {"requestState", JsonValue("opaque")},
            {"inputRequests", JsonValue::array_t{url_request()}},
    };
    // The source text is deliberately not the object's own text, so the raw
    // member reader finds nothing to recover.
    const auto decoded = mcp::dto::read_input_required_result(JsonValue(result), R"({"resultType":"input_required"})");
    REQUIRE(!decoded.has_value());
    CHECK(decoded.error().detail.find("verbatim") != std::string::npos);
}
