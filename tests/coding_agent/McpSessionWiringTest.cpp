// The MCP Host's session wiring (issue #841): trust-gated startup, the five
// Upstream Connection Status states, and the bearer credential chain.
//
// These cases cross the one production door — `create_agent_session_async` —
// for everything the product does at startup, and drive the wiring value
// (`McpSessionHost`) directly where a case is about one connection's state
// machine rather than about session assembly. The only injected seam is the
// MCP Host's transport (`tests::ScriptedMcpTransport`) and the connection
// timer's value (`tests::ScriptedMcpDelay`); the credential store under test
// is the production one over a real `auth.json`.

#include <cch/coding_agent/AgentConfigDir.hpp>
#include <cch/coding_agent/AuthStorage.hpp>
#include "coding_agent/AgentSession.hpp"
#include "coding_agent/McpCredentialStore.hpp"
#include "coding_agent/runtime/AgentSessionCreationRequest.hpp"
#include "coding_agent/runtime/McpSessionHost.hpp"
#include "coding_agent/runtime/SessionFactory.hpp"
#include "support/AgentRootFixture.hpp"
#include "support/EnvVarGuard.hpp"
#include "support/PumpUntil.hpp"
#include "support/RuntimeFixture.hpp"
#include "support/ScriptedMcpTransport.hpp"
#include "support/TempWorkspace.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace cch;

namespace {

namespace runtime_ns = cch::coding_agent::runtime;
using coding_agent::McpUpstreamState;
using tests::ScriptedMcpAnswer;

/// How long `drive_on` waits for a scripted operation before reporting it as
/// never completed. It is a cap, not a wait: a scripted transport and a
/// scripted timer make every case here settle in one pump.
constexpr std::chrono::milliseconds kOperationBudget{5000};

/// A conforming Modern Era Upstream: the era probe, then an empty catalog.
void answer_conforming_upstream(tests::ScriptedMcpTransport& transport) {
    transport.answer("server/discover", ScriptedMcpAnswer{.result = tests::discover_result()});
    transport.answer("tools/list", ScriptedMcpAnswer{.result = tests::tool_list_result({})});
}

[[nodiscard]] const coding_agent::McpUpstreamStatus& row_for(
        const std::vector<coding_agent::McpUpstreamStatus>& rows, std::string_view server_id) {
    const auto found = std::ranges::find_if(rows, [server_id](const coding_agent::McpUpstreamStatus& row) {
        return row.server_id == server_id;
    });
    REQUIRE(found != rows.end());
    return *found;
}

/// One host-level fixture: a temp Agent Config Directory for the trust store
/// and the credential store, a loop this test owns, and the scripted
/// transport every connection shares.
struct WiringFixture {
    tests::TempWorkspace workspace;
    std::filesystem::path agent_dir;
    std::filesystem::path trust_store;
    std::shared_ptr<tests::ScriptedMcpTransport> transport{std::make_shared<tests::ScriptedMcpTransport>()};
    tests::ScriptedMcpDelay delay;
    boost::asio::io_context loop;

    WiringFixture() {
        agent_dir = workspace.path() / "agent";
        std::filesystem::create_directories(agent_dir);
        trust_store = agent_dir / "mcp-trust.json";
    }

    void write_trust_store(std::string_view content) const {
        std::ofstream output(trust_store, std::ios::binary | std::ios::trunc);
        output << content;
    }

    void write_credentials(std::string_view content) const {
        std::ofstream output(agent_dir / "auth.json", std::ios::binary | std::ios::trunc);
        output << content;
    }

    [[nodiscard]] runtime_ns::McpSessionHostOptions options(
            std::vector<coding_agent::UserMcpServerSettings> servers) {
        runtime_ns::McpSessionHostOptions options;
        options.servers = std::move(servers);
        options.trust_store_path = trust_store;
        options.credentials =
                std::make_shared<coding_agent::McpCredentialStore>(std::make_shared<coding_agent::AuthStorage>(
                        agent_dir / "auth.json"));
        options.executor = loop.get_executor();
        options.transport = transport;
        options.delay = [this](std::chrono::milliseconds wait, std::stop_token stop_token) {
            return delay.request(wait, stop_token);
        };
        return options;
    }

    [[nodiscard]] coding_agent::UserMcpServerSettings server(std::string server_id) const {
        return coding_agent::UserMcpServerSettings{
                .server_id = std::move(server_id),
                .url = "https://mcp.example/mcp",
        };
    }

    /// Drain the loop so every co-spawned connect has run. This is
    /// `tests::drain_ready` and not `run()`: a connect still waiting on an
    /// Upstream that never answers holds the loop's work count, so `run()`
    /// would not return — which is exactly the state the "never answers"
    /// cases hold on purpose, and the same reason every other fixture in
    /// this repository drains instead of running.
    void pump() { tests::drain_ready(loop); }
};

/// Drive one `AsyncResult` to its terminal outcome on the fixture's loop.
template <typename T> [[nodiscard]] support::Expected<T> drive_on(WiringFixture& fixture, support::AsyncResult<T> op) {
    auto outcome = std::make_shared<support::Expected<T>>(std::unexpected(
            support::make_error(support::ErrorCode::Busy, "the operation never completed")));
    auto done = std::make_shared<std::atomic<bool>>(false);
    std::move(op).start([outcome, done](std::expected<T, support::Error> value) mutable noexcept {
        *outcome = std::move(value);
        done->store(true, std::memory_order_release);
    });
    (void)tests::pump_until(fixture.loop, [done] { return done->load(std::memory_order_acquire); },
            kOperationBudget);
    return std::move(*outcome);
}

/// One session-assembly fixture: a temp workspace with its own Agent Config
/// Directory, guarded so no ambient provider key resolves as configured.
struct AssemblyFixture {
    tests::TempWorkspace workspace;
    std::filesystem::path agent_dir;
    tests::EnvVarGuard home_guard{"HOME"};
    tests::EnvVarGuard kimi_guard{"KIMI_API_KEY"};
    tests::EnvVarGuard deepseek_guard{"DEEPSEEK_API_KEY"};
    tests::RuntimeFixture runtime;
    std::shared_ptr<tests::ScriptedMcpTransport> transport{std::make_shared<tests::ScriptedMcpTransport>()};
    tests::ScriptedMcpDelay delay;

    AssemblyFixture() {
        home_guard.set(workspace.path().string());
        agent_dir = tests::agent_root_under_home(workspace.path());
        std::filesystem::create_directories(agent_dir);
        kimi_guard.unset();
        deepseek_guard.unset();
    }

    void write_settings(std::string_view content) const {
        std::ofstream output(agent_dir / "settings.json", std::ios::binary | std::ios::trunc);
        output << content;
    }

    void write_trust_store(std::string_view content) const {
        std::ofstream output(agent_dir / "mcp-trust.json", std::ios::binary | std::ios::trunc);
        output << content;
    }

    [[nodiscard]] runtime_ns::AgentSessionCreationRequest make_request() {
        runtime_ns::AgentSessionCreationRequest request;
        request.execution_runtime_target = runtime.make_target();
        request.session_facts.no_skills = true;
        request.session_facts.no_prompt_templates = true;
        request.workspace = workspace.path();
        request.session_target = coding_agent::InMemorySessionTarget{};
        request.mcp_transport = transport;
        request.mcp_delay = [this](std::chrono::milliseconds wait, std::stop_token stop_token) {
            return delay.request(wait, stop_token);
        };
        return request;
    }

    [[nodiscard]] support::Expected<coding_agent::CreateAgentSessionResult> create(
            runtime_ns::AgentSessionCreationRequest request) {
        return runtime.run(coding_agent::create_agent_session_async(std::move(request), std::nullopt, {}));
    }
};

constexpr std::string_view kOneServerSettings =
        R"({"mcpServers": {"executor": {"url": "https://mcp.example/mcp"}}})";

} // namespace

TEST_CASE("a session is usable while an Upstream that never answers is still pending",
        "[mcp][issue841][spec]") {
    AssemblyFixture fixture;
    fixture.write_settings(kOneServerSettings);
    fixture.write_trust_store(R"({"executor": true})");
    // The Upstream takes the era probe and goes silent: the one case that
    // proves startup does not wait for discovery, rather than merely that a
    // fast server starts quickly.
    fixture.transport->hold("server/discover");

    auto created = fixture.create(fixture.make_request());
    REQUIRE(created.has_value());
    auto& session = fixture.runtime.adopt_session(std::move(created->session));

    CHECK(session.is_open());
    const auto rows = session.mcp_upstream_status();
    REQUIRE(rows.size() == 1);
    CHECK(row_for(rows, "executor").state == McpUpstreamState::Pending);
    // The attempt reached the Upstream; the session did not wait for it.
    CHECK(fixture.transport->request_count("server/discover") == 1);

    // Session close releases the silent attempt: the deterministic two-phase
    // close cancels it rather than waiting out the per-call deadline.
    session.close();
}

TEST_CASE("a server whose first-enable consent is outstanding is disabled and never contacted",
        "[mcp][issue841][spec]") {
    AssemblyFixture fixture;
    fixture.write_settings(kOneServerSettings);

    auto created = fixture.create(fixture.make_request());
    REQUIRE(created.has_value());
    auto& session = fixture.runtime.adopt_session(std::move(created->session));

    CHECK(fixture.transport->request_count() == 0);
    const auto rows = session.mcp_upstream_status();
    REQUIRE(rows.size() == 1);
    CHECK(row_for(rows, "executor").state == McpUpstreamState::Disabled);
    CHECK(row_for(rows, "executor").status_message == "awaiting first-enable consent");
    REQUIRE(session.pending_mcp_server_trust_requests().size() == 1);
    CHECK(session.pending_mcp_server_trust_requests().front().server_id == "executor");
}

TEST_CASE("a declined server is disabled and never contacted", "[mcp][issue841][spec]") {
    AssemblyFixture fixture;
    fixture.write_settings(kOneServerSettings);
    fixture.write_trust_store(R"({"executor": false})");

    auto created = fixture.create(fixture.make_request());
    REQUIRE(created.has_value());
    auto& session = fixture.runtime.adopt_session(std::move(created->session));

    CHECK(fixture.transport->request_count() == 0);
    const auto rows = session.mcp_upstream_status();
    REQUIRE(rows.size() == 1);
    // Declined is not failed: nothing was ever attempted, so there is no
    // failure to report and nothing to retry.
    CHECK(row_for(rows, "executor").state == McpUpstreamState::Disabled);
    CHECK(row_for(rows, "executor").status_message == "declined");
    CHECK(session.pending_mcp_server_trust_requests().empty());
}

TEST_CASE("the first upstream request happens only after the user consents",
        "[mcp][issue841][spec]") {
    WiringFixture fixture;
    answer_conforming_upstream(*fixture.transport);
    // A prompter that holds its completion: the consent is outstanding for as
    // long as the test says it is, which is the case a "connect on startup,
    // prompt later" wiring would get wrong.
    struct HeldPrompt {
        std::optional<support::AsyncCompletion<coding_agent::McpServerTrustAnswer, support::Error>> completion;

        [[nodiscard]] coding_agent::McpServerTrustPrompter prompter() {
            auto held = std::make_shared<HeldPrompt>();
            held_state = held;
            return [held](coding_agent::McpServerTrustPromptRequest, std::stop_token) {
                return support::AsyncResult<coding_agent::McpServerTrustAnswer>(
                        support::AsyncProducer<coding_agent::McpServerTrustAnswer, support::Error>{
                                [held](support::AsyncCompletion<coding_agent::McpServerTrustAnswer, support::Error>
                                               completion) mutable noexcept {
                                    held->completion.emplace(std::move(completion));
                                }});
            };
        }
        std::shared_ptr<HeldPrompt> held_state;
    };
    HeldPrompt prompt;
    auto options = fixture.options({fixture.server("executor")});
    options.trust_prompter = prompt.prompter();
    auto host = runtime_ns::McpSessionHost::start(std::move(options));
    REQUIRE(host != nullptr);
    fixture.pump();

    auto answered = std::make_shared<support::Expected<coding_agent::McpServerTrustResolution>>();
    host->ask_trust("executor")
            .start([answered](std::expected<coding_agent::McpServerTrustResolution, support::Error> outcome) mutable
                           noexcept { *answered = std::move(outcome); });
    fixture.pump();

    // The prompt is open and unanswered: the Upstream is still untouched, and
    // its row still reads disabled rather than a failure that never happened.
    REQUIRE(prompt.held_state->completion.has_value());
    CHECK(fixture.transport->request_count() == 0);
    CHECK(row_for(host->upstream_status(), "executor").state == McpUpstreamState::Disabled);

    auto completion = std::move(*prompt.held_state->completion);
    prompt.held_state->completion.reset();
    completion(std::expected<coding_agent::McpServerTrustAnswer, support::Error>{
            coding_agent::McpServerTrustAnswer::Accepted});
    fixture.pump();

    REQUIRE(answered->has_value());
    CHECK((*answered)->enables_server());
    CHECK(fixture.transport->request_count("server/discover") == 1);
    CHECK(row_for(host->upstream_status(), "executor").state == McpUpstreamState::Connected);
}

TEST_CASE("an accepted consent is what connects an Upstream", "[mcp][issue841][spec]") {
    WiringFixture fixture;
    answer_conforming_upstream(*fixture.transport);
    auto options = fixture.options({fixture.server("executor")});
    options.trust_prompter = [](coding_agent::McpServerTrustPromptRequest, std::stop_token) {
        return support::AsyncResult<coding_agent::McpServerTrustAnswer>(
                support::Expected<coding_agent::McpServerTrustAnswer>{coding_agent::McpServerTrustAnswer::Accepted});
    };
    auto host = runtime_ns::McpSessionHost::start(std::move(options));
    REQUIRE(host != nullptr);
    fixture.pump();
    CHECK(fixture.transport->request_count() == 0);

    const auto answered = drive_on(fixture, host->ask_trust("executor"));
    fixture.pump();
    REQUIRE(answered.has_value());
    CHECK(answered->enables_server());
    CHECK(fixture.transport->request_count("server/discover") == 1);
    CHECK(row_for(host->upstream_status(), "executor").state == McpUpstreamState::Connected);
}

TEST_CASE("a declined consent leaves the server uncontacted", "[mcp][issue841][spec]") {
    WiringFixture fixture;
    answer_conforming_upstream(*fixture.transport);
    auto options = fixture.options({fixture.server("executor")});
    options.trust_prompter = [](coding_agent::McpServerTrustPromptRequest, std::stop_token) {
        return support::AsyncResult<coding_agent::McpServerTrustAnswer>(
                support::Expected<coding_agent::McpServerTrustAnswer>{coding_agent::McpServerTrustAnswer::Declined});
    };
    auto host = runtime_ns::McpSessionHost::start(std::move(options));
    REQUIRE(host != nullptr);
    fixture.pump();

    const auto answered = drive_on(fixture, host->ask_trust("executor"));
    REQUIRE(answered.has_value());
    CHECK_FALSE(answered->enables_server());
    fixture.pump();
    CHECK(fixture.transport->request_count() == 0);
    CHECK(row_for(host->upstream_status(), "executor").state == McpUpstreamState::Disabled);
}

TEST_CASE("a bearer-authenticated Upstream connects with the credential its reference names",
        "[mcp][issue841][spec]") {
    WiringFixture fixture;
    tests::EnvVarGuard token_guard{"MCP_EXECUTOR_TOKEN", "session-token-value"};
    answer_conforming_upstream(*fixture.transport);
    fixture.write_trust_store(R"({"executor": true})");
    auto server = fixture.server("executor");
    server.bearer_env_var = "MCP_EXECUTOR_TOKEN";

    auto host = runtime_ns::McpSessionHost::start(fixture.options({server}));
    REQUIRE(host != nullptr);
    fixture.pump();

    REQUIRE(fixture.transport->request_count("server/discover") == 1);
    const auto& recorded = fixture.transport->requests().front();
    const auto authorization = std::ranges::find_if(recorded.headers, [](const auto& header) {
        return header.first == "Authorization";
    });
    REQUIRE(authorization != recorded.headers.end());
    CHECK(authorization->second == "Bearer session-token-value");
    CHECK(row_for(host->upstream_status(), "executor").state == McpUpstreamState::Connected);
}

TEST_CASE("a server whose credential cannot be resolved waits for one instead of asking anyway",
        "[mcp][issue841][spec]") {
    WiringFixture fixture;
    tests::EnvVarGuard token_guard{"MCP_EXECUTOR_TOKEN", std::nullopt};
    answer_conforming_upstream(*fixture.transport);
    auto server = fixture.server("executor");
    server.bearer_env_var = "MCP_EXECUTOR_TOKEN";
    fixture.write_trust_store(R"({"executor": true})");

    auto host = runtime_ns::McpSessionHost::start(fixture.options({server}));
    REQUIRE(host != nullptr);
    fixture.pump();

    // Neither the environment nor the store holds a credential, so the
    // exchange is refused before the wire: an unauthenticated request is
    // never sent (issue #838). The connection machinery classifies that as
    // `needs_auth`, and the projection carries it through unchanged — a
    // missing credential is a credential the session still owes, not a
    // transport failure the reconnect ladder could retry away.
    CHECK(fixture.transport->request_count() == 0);
    CHECK(row_for(host->upstream_status(), "executor").state == McpUpstreamState::NeedsAuth);
}

TEST_CASE("a flapping server shows the bounded reconnect ladder in its status row",
        "[mcp][issue841][spec]") {
    WiringFixture fixture;
    fixture.write_trust_store(R"({"executor": true})");
    // Every attempt fails at the transport, which is a transport-closure
    // signal: the connection arms exactly one bounded reconnect (issue #839)
    // and the row reports where on that ladder it stands.
    fixture.transport->answer("server/discover", ScriptedMcpAnswer{.transport_error =
            support::make_error(support::ErrorCode::Network, "connection refused")});

    auto servers = std::vector{coding_agent::UserMcpServerSettings{}};
    servers = {fixture.server("executor")};
    auto host = runtime_ns::McpSessionHost::start(fixture.options(servers));
    REQUIRE(host != nullptr);
    fixture.pump();

    const auto rows = host->upstream_status();
    const auto& row = row_for(rows, "executor");
    CHECK(row.state == McpUpstreamState::Failed);
    CHECK(row.consecutive_failures >= 1);
    CHECK(fixture.delay.waiting() == 1);
    CHECK(fixture.delay.next_delay() == std::chrono::milliseconds{250});

    // The next attempt re-probes exactly once more; the ladder advances only
    // when a delay elapses, so elapsing it doubles the rung.
    REQUIRE(fixture.delay.elapse_oldest());
    fixture.pump();
    CHECK(fixture.transport->request_count("server/discover") == 2);
    CHECK(fixture.delay.next_delay() == std::chrono::milliseconds{500});
}

TEST_CASE("an Upstream that answers 401 waits for the user instead of retrying",
        "[mcp][issue841][spec]") {
    WiringFixture fixture;
    fixture.write_trust_store(R"({"executor": true})");
    fixture.transport->answer("server/discover", ScriptedMcpAnswer{.status_code = 401});

    auto host = runtime_ns::McpSessionHost::start(fixture.options({fixture.server("executor")}));
    REQUIRE(host != nullptr);
    fixture.pump();

    CHECK(row_for(host->upstream_status(), "executor").state == McpUpstreamState::NeedsAuth);
    // No reconnect is armed for a credential the session cannot obtain.
    CHECK(fixture.delay.waiting() == 0);
}

TEST_CASE("a lost transport moves a connected Upstream to failed and arms one bounded reconnect",
        "[mcp][issue841][spec]") {
    WiringFixture fixture;
    answer_conforming_upstream(*fixture.transport);
    fixture.write_trust_store(R"({"executor": true})");
    auto host = runtime_ns::McpSessionHost::start(fixture.options({fixture.server("executor")}));
    REQUIRE(host != nullptr);
    fixture.pump();
    REQUIRE(row_for(host->upstream_status(), "executor").state == McpUpstreamState::Connected);

    auto* connection = host->connection("executor");
    REQUIRE(connection != nullptr);
    connection->notify_transport_closed("the transport went away");
    fixture.pump();

    CHECK(row_for(host->upstream_status(), "executor").state == McpUpstreamState::Failed);
    CHECK(fixture.delay.waiting() == 1);
    CHECK(fixture.delay.next_delay() == std::chrono::milliseconds{250});
}

TEST_CASE("the five Upstream Connection Status states are the five the glossary names",
        "[mcp][issue841][spec]") {
    // The projection is the session's only view of the MCP Host (ADR 0065),
    // so the states it can name are the whole contract `/mcp` renders.
    CHECK(coding_agent::to_string(McpUpstreamState::Pending) == "pending");
    CHECK(coding_agent::to_string(McpUpstreamState::Connected) == "connected");
    CHECK(coding_agent::to_string(McpUpstreamState::Failed) == "failed");
    CHECK(coding_agent::to_string(McpUpstreamState::NeedsAuth) == "needs_auth");
    CHECK(coding_agent::to_string(McpUpstreamState::Disabled) == "disabled");
}

TEST_CASE("closing the session's MCP Host cancels its in-flight work", "[mcp][issue841][spec]") {
    WiringFixture fixture;
    fixture.write_trust_store(R"({"executor": true})");
    fixture.transport->hold("server/discover", tests::McpHold::UntilStopped);

    auto host = runtime_ns::McpSessionHost::start(fixture.options({fixture.server("executor")}));
    REQUIRE(host != nullptr);
    fixture.pump();
    REQUIRE(fixture.transport->request_count("server/discover") == 1);

    const auto closed = drive_on(fixture, host->close());
    REQUIRE(closed.has_value());
}
