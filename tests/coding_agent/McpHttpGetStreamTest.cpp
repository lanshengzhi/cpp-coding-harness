// Spec #882, ticket #884: the MCP streamable-http server-to-client GET stream.
// A scripted fake `StreamTransport` (no live network) answers the initialize
// POST, the `notifications/initialized` POST, and each GET from a queue, and
// records every request, so the stream's own behavior is observable without a
// server. The acceptance cases pair each property with a case that separates
// it: 405 (feature absent) vs. a real failure, exhausted retries vs. a clean
// `close()`, and a server `retry:` field vs. the client backoff.

#include "coding_agent/mcp/McpHttpClient.hpp"
#include "coding_agent/mcp/McpHttpServerConfig.hpp"

#include "support/Json.hpp"

#include <cch/support/Error.hpp>
#include <cch/support/JsonValue.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace cch;

namespace {

using namespace std::chrono_literals;

using coding_agent::mcp::McpHttpClient;
using coding_agent::mcp::McpHttpGetStreamOptions;
using coding_agent::mcp::McpHttpServerConfig;

[[nodiscard]] support::Error stream_cancelled() {
    return support::make_error(
            support::ErrorCode::Cancelled, "stream transport cancelled", "transport operation was cancelled");
}

/// One scripted GET-stream attempt: the status/headers returned once the
/// chunks have been delivered, and whether the stream stays open until
/// `close()` aborts it.
struct GetScript {
    int status{200};
    std::map<std::string, std::string> headers{{"content-type", "text/event-stream"}};
    std::vector<std::string> chunks;
    bool hold_open{false};
    std::chrono::milliseconds delay_before_chunks{0};
    std::chrono::milliseconds chunk_interval{0};
};

/// One recorded request, so a test can assert method, headers, ordering, and
/// timing.
struct RecordedRequest {
    std::string method;
    std::string url;
    std::map<std::string, std::string> headers;
    std::string body;
    std::chrono::steady_clock::time_point at{};
};

/// Scripted `StreamTransport`: answers the MCP handshake POSTs, then each GET
/// from a queue (or the default). No network and no TLS.
class ScriptedTransport final : public ai::providers::StreamTransport {
public:
    void push_get(GetScript script) { get_scripts_.push_back(std::move(script)); }
    void set_default_get(GetScript script) { default_get_ = std::move(script); }
    void release() { released_ = true; }

    [[nodiscard]] const std::vector<RecordedRequest>& requests() const { return requests_; }

    [[nodiscard]] std::vector<const RecordedRequest*> get_requests() const {
        std::vector<const RecordedRequest*> result;
        for (const auto& request : requests_) {
            if (request.method == "GET") {
                result.push_back(&request);
            }
        }
        return result;
    }

    [[nodiscard]] int get_call_count() const { return static_cast<int>(get_requests().size()); }

    [[nodiscard]] int delete_call_count() const {
        int count = 0;
        for (const auto& request : requests_) {
            if (request.method == "DELETE") {
                ++count;
            }
        }
        return count;
    }

    [[nodiscard]] const RecordedRequest* last_delete() const {
        for (auto it = requests_.rbegin(); it != requests_.rend(); ++it) {
            if (it->method == "DELETE") {
                return &*it;
            }
        }
        return nullptr;
    }

    /// The POSTed JSON-RPC response for `id`, if the client sent one.
    [[nodiscard]] std::optional<support::JsonValue> posted_response(double id) const {
        for (const auto& request : requests_) {
            if (request.method != "POST") {
                continue;
            }
            auto parsed = support::read_json(request.body);
            if (!parsed) {
                continue;
            }
            const auto* object = parsed->get_if<support::JsonValue::object_t>();
            if (object == nullptr) {
                continue;
            }
            const auto request_id = object->find("id");
            if (request_id == object->end() || !request_id->second.holds<double>() ||
                    request_id->second.get_number() != id) {
                continue;
            }
            if (object->find("result") != object->end() || object->find("error") != object->end()) {
                return *parsed;
            }
        }
        return std::nullopt;
    }

    boost::asio::awaitable<support::Expected<ai::providers::StreamResponse>> async_stream(
            const ai::providers::StreamRequest& request, ai::providers::BodyChunkHandler on_body_chunk) override {
        requests_.push_back(RecordedRequest{
                request.method, request.url, request.headers, request.body, std::chrono::steady_clock::now()});
        const std::stop_token stop = request.stop_token;

        if (request.method == "GET") {
            GetScript script = default_get_;
            if (!get_scripts_.empty()) {
                script = std::move(get_scripts_.front());
                get_scripts_.pop_front();
            }
            auto executor = co_await boost::asio::this_coro::executor;
            if (script.delay_before_chunks > 0ms) {
                boost::asio::steady_timer delay(executor);
                delay.expires_after(script.delay_before_chunks);
                boost::system::error_code error;
                co_await delay.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
            }
            for (std::size_t index = 0; index < script.chunks.size(); ++index) {
                if (stop.stop_requested()) {
                    co_return std::unexpected(stream_cancelled());
                }
                if (on_body_chunk) {
                    if (auto handled = on_body_chunk(script.chunks[index]); !handled) {
                        co_return std::unexpected(handled.error());
                    }
                }
                if (script.chunk_interval > 0ms) {
                    boost::asio::steady_timer timer(executor);
                    timer.expires_after(script.chunk_interval);
                    boost::system::error_code error;
                    co_await timer.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
                }
            }
            if (script.hold_open) {
                boost::asio::steady_timer timer(executor);
                while (!stop.stop_requested() && !released_) {
                    timer.expires_after(1ms);
                    boost::system::error_code error;
                    co_await timer.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
                }
                if (stop.stop_requested()) {
                    co_return std::unexpected(stream_cancelled());
                }
            }
            ai::providers::StreamResponse response;
            response.head.status_code = script.status;
            response.head.headers = std::move(script.headers);
            co_return response;
        }

        auto parsed = support::read_json(request.body);
        std::string method;
        std::optional<double> id;
        if (parsed) {
            if (const auto* object = parsed->get_if<support::JsonValue::object_t>()) {
                if (const auto found = object->find("method");
                        found != object->end() && found->second.holds<std::string>()) {
                    method = found->second.get_string();
                }
                if (const auto found = object->find("id"); found != object->end() && found->second.holds<double>()) {
                    id = found->second.get_number();
                }
            }
        }
        ai::providers::StreamResponse response;
        if (method == "initialize") {
            support::JsonValue result{support::JsonValue::object_t{
                    {"jsonrpc", "2.0"},
                    {"id", id.value_or(0.0)},
                    {"result",
                            support::JsonValue::object_t{
                                    {"protocolVersion", "2025-06-18"},
                                    {"capabilities", support::JsonValue::object_t{}},
                                    {"serverInfo",
                                            support::JsonValue::object_t{
                                                    {"name", "scripted"},
                                                    {"version", "1.0"},
                                            }},
                            }},
            }};
            auto body = support::write_json(result);
            response.head.status_code = 200;
            response.head.headers = {{"content-type", "application/json"}, {"mcp-session-id", "test-session"}};
            response.body = body ? std::move(*body) : std::string{"{}"};
            co_return response;
        }
        response.head.status_code = 202;
        response.head.headers = {{"content-type", "application/json"}};
        co_return response;
    }

private:
    std::deque<GetScript> get_scripts_;
    GetScript default_get_{};
    std::vector<RecordedRequest> requests_;
    bool released_{false};
};

/// A test-driven `io_context`: `run` advances a coroutine to completion and
/// `pump_until` advances the detached background coroutines (the GET stream)
/// until a predicate holds. Both run on the calling thread, so the transport is
/// never driven concurrently.
class Loop final {
public:
    Loop() : work_(boost::asio::make_work_guard(io_)) {}
    Loop(const Loop&) = delete;
    Loop& operator=(const Loop&) = delete;
    ~Loop() {
        work_.reset();
        io_.stop();
    }

    template <typename T> [[nodiscard]] T run(boost::asio::awaitable<T> operation) {
        // Complete through an in-coroutine flag rather than `use_future`: the
        // coroutine resumes and records its result inside the same executor
        // handler, so the loop stops before it drains unrelated handlers (the
        // GET stream a connect posts) and a test can attach its listeners
        // first.
        std::optional<T> result;
        bool done = false;
        boost::asio::co_spawn(
                io_,
                [&result, &done, operation = std::move(operation)]() mutable -> boost::asio::awaitable<void> {
                    result.emplace(co_await std::move(operation));
                    done = true;
                },
                boost::asio::detached);
        while (!done) {
            io_.restart();
            io_.poll_one();
            std::this_thread::sleep_for(1ms);
        }
        return std::move(*result);
    }

    [[nodiscard]] bool pump_until(const std::function<bool()>& predicate, std::chrono::milliseconds budget) {
        const auto deadline = std::chrono::steady_clock::now() + budget;
        while (!predicate() && std::chrono::steady_clock::now() < deadline) {
            io_.restart();
            io_.poll_one();
            std::this_thread::sleep_for(1ms);
        }
        return predicate();
    }

private:
    boost::asio::io_context io_;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work_;
};

[[nodiscard]] McpHttpServerConfig scripted_config() {
    McpHttpServerConfig config;
    config.name = "scripted";
    config.url = "https://scripted.test/mcp";
    return config;
}

[[nodiscard]] std::shared_ptr<McpHttpClient> connect_client(
        Loop& loop, const std::shared_ptr<ScriptedTransport>& transport, McpHttpGetStreamOptions options = {}) {
    auto client = loop.run(McpHttpClient::connect(scripted_config(), transport, nullptr, options));
    REQUIRE(client.has_value());
    return *client;
}

[[nodiscard]] std::string message_event(std::string_view json) {
    return "event: message\ndata: " + std::string{json} + "\n\n";
}

/// A live GET stream that stays open until the client closes it. The first
/// chunk is delayed so the caller can attach its listener before it arrives,
/// as a real server would.
[[nodiscard]] GetScript live_stream(std::vector<std::string> chunks, std::chrono::milliseconds chunk_interval = 0ms) {
    GetScript script;
    script.hold_open = true;
    script.chunks = std::move(chunks);
    script.delay_before_chunks = 30ms;
    script.chunk_interval = chunk_interval;
    return script;
}

} // namespace

TEST_CASE("MCP GET stream: HTTP 405 is feature-absent, not a failure", "[coding_agent][mcp]") {
    auto transport = std::make_shared<ScriptedTransport>();
    transport->push_get(GetScript{.status = 405, .headers = {{"content-type", "text/plain"}}, .chunks = {}});
    Loop loop;
    auto client = connect_client(loop, transport);

    std::vector<support::Error> errors;
    client->set_error_listener([&errors](const support::Error& error) { errors.push_back(error); });

    REQUIRE(loop.pump_until([&] { return transport->get_call_count() >= 1; }, 2s));
    // Give any (wrongly scheduled) retry a chance to appear.
    (void)loop.pump_until([&] { return false; }, 50ms);

    CHECK(transport->get_call_count() == 1);
    CHECK(errors.empty());
    const auto* get = transport->get_requests().front();
    CHECK(get->headers.at("accept") == "text/event-stream");
    CHECK(get->headers.at("mcp-session-id") == "test-session");
    CHECK(get->headers.find("content-type") == get->headers.end());
    CHECK(get->headers.find("last-event-id") == get->headers.end());
    client->close();
}

TEST_CASE("MCP GET stream: a server retry field overrides the reconnect delay", "[coding_agent][mcp]") {
    auto transport = std::make_shared<ScriptedTransport>();
    // The first attempt primes `last-event-id` and asks for a 20 ms reconnect
    // delay, then ends; the second stays open.
    GetScript first;
    first.chunks = {"retry: 20\nid: 1\n" +
                    message_event(R"({"jsonrpc":"2.0","method":"notifications/message","params":{"level":"info"}})")};
    transport->push_get(std::move(first));
    transport->push_get(live_stream({}));

    McpHttpGetStreamOptions options;
    options.initial_delay = 500ms;
    options.max_delay = 500ms;
    Loop loop;
    auto client = connect_client(loop, transport, options);

    REQUIRE(loop.pump_until([&] { return transport->get_call_count() >= 2; }, 5s));
    const auto gets = transport->get_requests();
    CHECK(gets.size() == 2);
    const auto gap = gets[1]->at - gets[0]->at;
    // 20 ms (the server's retry) rather than the 500 ms client backoff.
    CHECK(gap < 200ms);
    CHECK(gets[1]->headers.at("last-event-id") == "1");
    client->close();
}

TEST_CASE("MCP GET stream: exhausted retries report the dropped-stream error", "[coding_agent][mcp]") {
    auto transport = std::make_shared<ScriptedTransport>();
    // A stream that opens and immediately ends, with no event: every attempt is
    // a failed reconnect.
    transport->set_default_get(GetScript{});
    McpHttpGetStreamOptions options;
    options.initial_delay = 1ms;
    options.max_delay = 50ms;
    options.max_retries = 5;
    Loop loop;
    auto client = connect_client(loop, transport, options);

    std::vector<support::Error> errors;
    client->set_error_listener([&errors](const support::Error& error) { errors.push_back(error); });

    REQUIRE(loop.pump_until([&] { return !errors.empty(); }, 5s));
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].message == "MCP server-to-client stream dropped and could not be reopened");
    // pi's loop opens once per attempt plus the initial open: maxRetries + 1.
    CHECK(transport->get_call_count() == 6);
}

TEST_CASE("MCP GET stream: close() aborts the stream without the dropped error", "[coding_agent][mcp]") {
    auto transport = std::make_shared<ScriptedTransport>();
    transport->set_default_get(live_stream({}));
    Loop loop;
    auto client = connect_client(loop, transport);

    std::vector<support::Error> errors;
    client->set_error_listener([&errors](const support::Error& error) { errors.push_back(error); });
    REQUIRE(loop.pump_until([&] { return transport->get_call_count() >= 1; }, 2s));

    client->close();
    REQUIRE(loop.pump_until([&] { return transport->delete_call_count() >= 1; }, 2s));
    (void)loop.pump_until([&] { return false; }, 50ms);

    CHECK(errors.empty());
    CHECK(transport->get_call_count() == 1);
    const auto* deleted = transport->last_delete();
    REQUIRE(deleted != nullptr);
    CHECK(deleted->headers.at("mcp-session-id") == "test-session");
}

TEST_CASE("MCP GET stream: server notifications reach the connection listener", "[coding_agent][mcp]") {
    auto transport = std::make_shared<ScriptedTransport>();
    transport->set_default_get(live_stream({message_event(
            R"({"jsonrpc":"2.0","method":"notifications/message","params":{"level":"info","data":"hello"}})")}));
    Loop loop;
    auto client = connect_client(loop, transport);
    std::vector<std::pair<std::string, support::JsonValue>> received;
    client->set_notification_listener([&received](std::string_view method, const support::JsonValue& params) {
        received.emplace_back(std::string{method}, params);
    });

    REQUIRE(loop.pump_until([&] { return !received.empty(); }, 2s));
    REQUIRE(received.size() == 1);
    CHECK(received[0].first == "notifications/message");
    const auto* object = received[0].second.get_if<support::JsonValue::object_t>();
    REQUIRE(object != nullptr);
    CHECK(object->at("level").get_string() == "info");
    client->close();
}

TEST_CASE("MCP GET stream: server ping is answered with an empty result", "[coding_agent][mcp]") {
    auto transport = std::make_shared<ScriptedTransport>();
    transport->set_default_get(live_stream({message_event(R"({"jsonrpc":"2.0","id":7,"method":"ping"})")}));
    Loop loop;
    auto client = connect_client(loop, transport);

    REQUIRE(loop.pump_until([&] { return transport->posted_response(7).has_value(); }, 2s));
    const auto response = transport->posted_response(7);
    const auto* object = response->get_if<support::JsonValue::object_t>();
    REQUIRE(object != nullptr);
    const auto result = object->find("result");
    REQUIRE(result != object->end());
    CHECK(result->second.get_if<support::JsonValue::object_t>() != nullptr);
    CHECK(result->second.get_if<support::JsonValue::object_t>()->empty());
    client->close();
}

TEST_CASE("MCP GET stream: an unknown server request is answered -32601", "[coding_agent][mcp]") {
    auto transport = std::make_shared<ScriptedTransport>();
    transport->set_default_get(live_stream({message_event(R"({"jsonrpc":"2.0","id":8,"method":"roots/list"})")}));
    Loop loop;
    auto client = connect_client(loop, transport);

    REQUIRE(loop.pump_until([&] { return transport->posted_response(8).has_value(); }, 2s));
    const auto response = transport->posted_response(8);
    const auto* object = response->get_if<support::JsonValue::object_t>();
    REQUIRE(object != nullptr);
    const auto error = object->find("error");
    REQUIRE(error != object->end());
    const auto* error_object = error->second.get_if<support::JsonValue::object_t>();
    REQUIRE(error_object != nullptr);
    CHECK(error_object->at("code").get_number() == -32601);
    CHECK(error_object->at("message").get_string() == "Method not found: roots/list");
    client->close();
}

TEST_CASE("MCP GET stream: notifications/cancelled aborts the in-flight handler", "[coding_agent][mcp]") {
    auto transport = std::make_shared<ScriptedTransport>();
    // The request arrives first; the cancellation follows once the handler has
    // registered (the chunk interval lets the executor start it).
    transport->set_default_get(live_stream(
            {message_event(R"({"jsonrpc":"2.0","id":9,"method":"test/wait"})"),
                    message_event(R"({"jsonrpc":"2.0","method":"notifications/cancelled","params":{"requestId":9}})")},
            20ms));
    Loop loop;
    auto client = connect_client(loop, transport);
    client->set_request_handler("test/wait",
            [](const support::JsonValue&,
                    std::stop_token stop) -> boost::asio::awaitable<support::Expected<support::JsonValue>> {
                auto executor = co_await boost::asio::this_coro::executor;
                boost::asio::steady_timer timer(executor);
                while (!stop.stop_requested()) {
                    timer.expires_after(1ms);
                    boost::system::error_code error;
                    co_await timer.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
                }
                co_return std::unexpected(support::make_error(support::ErrorCode::Cancelled, "MCP request aborted"));
            });

    REQUIRE(loop.pump_until([&] { return transport->posted_response(9).has_value(); }, 3s));
    const auto response = transport->posted_response(9);
    const auto* object = response->get_if<support::JsonValue::object_t>();
    REQUIRE(object != nullptr);
    const auto error = object->find("error");
    REQUIRE(error != object->end());
    const auto* error_object = error->second.get_if<support::JsonValue::object_t>();
    REQUIRE(error_object != nullptr);
    CHECK(error_object->at("code").get_number() == -32603);
    CHECK(error_object->at("message").get_string() == "MCP request aborted");
    client->close();
}
