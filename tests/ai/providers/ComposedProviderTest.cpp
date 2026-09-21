#include <cch/ai/Models.hpp>

#include "ai/providers/ComposedProvider.hpp"
#include "support/AsyncResultBridge.hpp"
#include "support/ModelFixture.hpp"
#include "support/StreamAdapterFixture.hpp"

#include <catch2/catch_test_macros.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace cch;

namespace {

using tests::ScriptedTransport;
using tests::TransportAttempt;

/// Runs one composed-provider stream over a scripted transport and returns the
/// single request the adapter sent, so a test can inspect what actually left
/// the provider.
[[nodiscard]] ai::providers::StreamRequest run_stream(
        const std::string& provider_id,
        ai::ProviderStreamOptions options,
        const std::shared_ptr<ScriptedTransport>& transport) {
    auto model = tests::make_model("gpt-5.6-luna", provider_id, "openai-responses");
    model.base_url = "https://example.invalid/v1";

    ai::ProviderAuth auth;
    auto provider = ai::providers::make_composed_provider(
            provider_id, provider_id, std::vector<ai::Model>{model}, std::move(auth), transport);

    const auto sent_model = provider->models().front();
    ai::AiContext context;
    boost::asio::io_context io;
    boost::asio::co_spawn(
            io,
            [&]() -> boost::asio::awaitable<void> {
                auto stream = provider->stream(sent_model, std::move(context), std::move(options));
                static_cast<void>(co_await support::detail::await_async_result(std::move(stream).run(
                        [](const ai::AssistantStreamEvent&) -> support::ExpectedVoid { return {}; })));
                co_return;
            },
            boost::asio::detached);
    io.run();

    REQUIRE(transport->requests.size() == 1);
    return transport->requests.front();
}

[[nodiscard]] std::shared_ptr<ScriptedTransport> transport_with_one_attempt() {
    auto transport = std::make_shared<ScriptedTransport>();
    transport->attempts.push_back(TransportAttempt{.head = {.status_code = 200}, .chunks = {}});
    return transport;
}

[[nodiscard]] std::optional<std::string> affinity_header(const ai::providers::StreamRequest& request) {
    const auto it = request.headers.find("x-opencode-session");
    return it == request.headers.end() ? std::nullopt : std::optional<std::string>{it->second};
}

} // namespace

TEST_CASE("opencode-go injects the session affinity header", "[ai][providers][opencode-go][issue754][spec]") {
    const auto transport = transport_with_one_attempt();
    ai::ProviderStreamOptions options;
    options.max_tokens = 16;
    options.auth.api_key = std::string{"test-key"};
    options.session_id = std::string{"session-abc"};

    const auto request = run_stream("opencode-go", std::move(options), transport);

    REQUIRE(affinity_header(request).has_value());
    CHECK(*affinity_header(request) == "session-abc");
}

TEST_CASE("an affinity header the caller already set is not replaced", "[ai][providers][opencode-go][issue754][spec]") {
    const auto transport = transport_with_one_attempt();
    ai::ProviderStreamOptions options;
    options.max_tokens = 16;
    options.auth.api_key = std::string{"test-key"};
    options.session_id = std::string{"session-abc"};
    options.auth.headers["x-opencode-session"] = "caller-value";

    const auto request = run_stream("opencode-go", std::move(options), transport);

    REQUIRE(affinity_header(request).has_value());
    CHECK(*affinity_header(request) == "caller-value");
}

TEST_CASE("opencode-go without a session id sends no affinity header", "[ai][providers][opencode-go][issue754][spec]") {
    const auto transport = transport_with_one_attempt();
    ai::ProviderStreamOptions options;
    options.max_tokens = 16;
    options.auth.api_key = std::string{"test-key"};

    const auto request = run_stream("opencode-go", std::move(options), transport);

    CHECK_FALSE(affinity_header(request).has_value());
}

TEST_CASE("other providers never receive the affinity header", "[ai][providers][opencode-go][issue754][spec]") {
    const auto transport = transport_with_one_attempt();
    ai::ProviderStreamOptions options;
    options.max_tokens = 16;
    options.auth.api_key = std::string{"test-key"};
    options.session_id = std::string{"session-abc"};

    const auto request = run_stream("openai", std::move(options), transport);

    CHECK_FALSE(affinity_header(request).has_value());
}
