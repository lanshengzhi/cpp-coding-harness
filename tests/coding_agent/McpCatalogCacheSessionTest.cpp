// The session-to-session half of the tool-catalog cache (issue #848, ADR
// 0067): the wiring accepting and sharing one cache, and the production
// composition root actually supplying it, so a repeated session does not
// re-walk `tools/list` (spec #833 story 17).
//
// The first two cases are driven through the one MCP Host seam the wiring
// already has — the injected `tests::ScriptedMcpTransport` — plus the cache
// value the caller supplies. The later cases cross the production door,
// `create_agent_session_async`, carrying the cache the interactive host
// builds, which is what makes the reuse production behaviour rather than a
// property of a test. No second seam is added, and nothing here reaches past
// the transport: what the Upstream was asked for is read back off it.

#include <cch/coding_agent/AuthStorage.hpp>
#include "coding_agent/McpCredentialStore.hpp"
#include "coding_agent/runtime/AgentSessionCreationRequest.hpp"
#include "coding_agent/runtime/McpSessionHost.hpp"
#include "coding_agent/runtime/SessionFactory.hpp"
#include "support/AgentRootFixture.hpp"
#include "support/EnvVarGuard.hpp"
#include "support/ModelsFixture.hpp"
#include "support/PumpUntil.hpp"
#include "support/RuntimeFixture.hpp"
#include "support/RuntimeLoopDriver.hpp"
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
#include <thread>
#include <utility>
#include <vector>

using namespace cch;

namespace {

namespace runtime_ns = cch::coding_agent::runtime;

/// How long `drive_on` waits for a scripted operation before reporting it as
/// never completed. A cap, not a wait.
constexpr std::chrono::milliseconds kOperationBudget{5000};

/// One session's MCP Host over a temp Agent Config Directory, with the
/// scripted transport and timer the wiring already takes as test seams.
struct SessionFixture {
    tests::TempWorkspace workspace;
    std::filesystem::path agent_dir;
    std::shared_ptr<tests::ScriptedMcpTransport> transport{std::make_shared<tests::ScriptedMcpTransport>()};
    tests::ScriptedMcpDelay delay;
    boost::asio::io_context loop;

    SessionFixture() {
        agent_dir = workspace.path() / "agent";
        std::filesystem::create_directories(agent_dir);
        std::ofstream trust(agent_dir / "mcp-trust.json", std::ios::binary | std::ios::trunc);
        trust << R"({"executor": true})";
    }

    /// A conforming Upstream whose catalog is fresh for a minute.
    void answer_catalog() {
        transport->answer("server/discover", {.result = tests::discover_result()});
        transport->answer("tools/list",
                {.result = tests::tool_list_result(
                        {tests::tool_entry("read_issue"), tests::tool_entry("write_file")}, std::nullopt, 60000.0)});
    }

    [[nodiscard]] runtime_ns::McpSessionHostOptions options(
            std::shared_ptr<mcp::UpstreamCatalogCache> catalog_cache) {
        runtime_ns::McpSessionHostOptions host_options;
        host_options.servers = {coding_agent::UserMcpServerSettings{
                .server_id = "executor",
                .url = "https://mcp.example/mcp",
        }};
        host_options.trust_store_path = agent_dir / "mcp-trust.json";
        host_options.credentials =
                std::make_shared<coding_agent::McpCredentialStore>(std::make_shared<coding_agent::AuthStorage>(
                        agent_dir / "auth.json"));
        host_options.executor = loop.get_executor();
        host_options.transport = transport;
        host_options.delay = [this](std::chrono::milliseconds wait, std::stop_token stop_token) {
            return delay.request(wait, stop_token);
        };
        host_options.catalog_cache = std::move(catalog_cache);
        return host_options;
    }

    void pump() { tests::drain_ready(loop); }
};

template <typename T> [[nodiscard]] support::Expected<T> drive_on(SessionFixture& fixture, support::AsyncResult<T> op) {
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

[[nodiscard]] std::vector<std::string> names_of(const mcp::UpstreamCatalog& catalog) {
    std::vector<std::string> names;
    names.reserve(catalog.tools.size());
    for (const auto& tool : catalog.tools) {
        names.push_back(tool.name);
    }
    return names;
}

} // namespace

TEST_CASE("a second session sharing the catalog cache asks the Upstream for nothing",
        "[mcp][catalog][cache][issue848][spec]") {
    auto cache = std::make_shared<mcp::UpstreamCatalogCache>();
    const std::vector<std::string> declared{"read_issue", "write_file"};

    // First session: a cold cache, so the Upstream is walked once.
    SessionFixture first;
    first.answer_catalog();
    auto first_host = runtime_ns::McpSessionHost::start(first.options(cache));
    REQUIRE(first_host != nullptr);
    first.pump();
    REQUIRE(first_host->connection("executor") != nullptr);
    const auto cold = drive_on(first, first_host->connection("executor")->list_tools());
    REQUIRE(cold.has_value());
    CHECK(names_of(*cold) == declared);
    CHECK(first.transport->request_count("tools/list") == 1);
    CHECK(first_host->upstream_status().at(0).state == coding_agent::McpUpstreamState::Connected);
    REQUIRE(drive_on(first, first_host->close()).has_value());

    // Second session: a completely separate host, its own connections, its
    // own trust gate — handed the first session's cache. The whole of
    // acceptance criterion 1 is that this session issues no `tools/list` at
    // all, and it gets the identical catalog for nothing.
    SessionFixture second;
    second.answer_catalog();
    auto second_host = runtime_ns::McpSessionHost::start(second.options(cache));
    REQUIRE(second_host != nullptr);
    second.pump();
    REQUIRE(second_host->connection("executor") != nullptr);
    const auto warm = drive_on(second, second_host->connection("executor")->list_tools());
    REQUIRE(warm.has_value());
    CHECK(names_of(*warm) == declared);
    CHECK(second.transport->request_count("tools/list") == 0);
    CHECK(second.transport->request_count("server/discover") == 1);
    REQUIRE(drive_on(second, second_host->close()).has_value());
}

TEST_CASE("a session with no shared cache still asks the Upstream for its catalog",
        "[mcp][catalog][cache][issue848][spec]") {
    // The other half of the seam's meaning: a null cache is a session-scoped
    // cache, which is correct within the session and worth nothing across
    // sessions. Without this, "a shared cache is faster" would also be
    // "a shared cache is the only way a catalog is ever used".
    SessionFixture first;
    first.answer_catalog();
    auto first_host = runtime_ns::McpSessionHost::start(first.options(nullptr));
    REQUIRE(first_host != nullptr);
    first.pump();
    REQUIRE(drive_on(first, first_host->connection("executor")->list_tools()).has_value());
    CHECK(first.transport->request_count("tools/list") == 1);
    REQUIRE(drive_on(first, first_host->close()).has_value());

    SessionFixture second;
    second.answer_catalog();
    auto second_host = runtime_ns::McpSessionHost::start(second.options(nullptr));
    REQUIRE(second_host != nullptr);
    second.pump();
    REQUIRE(drive_on(second, second_host->connection("executor")->list_tools()).has_value());
    CHECK(second.transport->request_count("tools/list") == 1);
    REQUIRE(drive_on(second, second_host->close()).has_value());
}

namespace {

/// A whole Agent Session over a temp Agent Config Directory, with the MCP
/// Host's transport and timer injected: the production door, crossed once per
/// "session" this file runs in sequence.
struct ProductionFixture {
    tests::TempWorkspace workspace;
    std::filesystem::path agent_dir;
    tests::EnvVarGuard home_guard{"HOME"};
    tests::EnvVarGuard kimi_guard{"KIMI_API_KEY"};
    tests::EnvVarGuard deepseek_guard{"DEEPSEEK_API_KEY"};
    tests::RuntimeFixture runtime;
    tests::RuntimeLoopDriver driver;
    std::shared_ptr<tests::ScriptedMcpTransport> transport{std::make_shared<tests::ScriptedMcpTransport>()};
    tests::ScriptedMcpDelay delay;
    /// The cache the interactive host builds and hands to every session it
    /// creates (ADR 0067).
    std::shared_ptr<mcp::UpstreamCatalogCache> cache = coding_agent::runtime::make_host_catalog_cache();

    ProductionFixture() : driver(runtime) {
        home_guard.set(workspace.path().string());
        agent_dir = tests::agent_root_under_home(workspace.path());
        std::filesystem::create_directories(agent_dir);
        kimi_guard.unset();
        deepseek_guard.unset();
        std::ofstream trust(agent_dir / "mcp-trust.json", std::ios::binary | std::ios::trunc);
        trust << R"({"executor": true})";
    }

    /// One configured Upstream, pointed at `url`, declared `eager` so the
    /// session walks its catalog as soon as it connects.
    void configure(std::string url) const {
        std::ofstream settings(agent_dir / "settings.json", std::ios::binary | std::ios::trunc);
        settings << R"({"mcpServers": {"executor": {"url": ")" << url << R"(", "activation": "eager"}}})";
    }

    /// A conforming Upstream whose catalog is fresh for a minute and, when
    /// `process_wide`, says so: only an Upstream that declares the wider scope
    /// may be reused across sessions.
    void answer_catalog(bool process_wide) {
        transport->answer("server/discover", {.result = tests::discover_result()});
        transport->answer("tools/list",
                {.result = tests::tool_list_result({tests::tool_entry("read_issue")},
                         std::nullopt,
                         60000.0,
                         process_wide ? std::optional<std::string>{"process"} : std::nullopt)});
    }

    /// Create and close one session against the shared cache, the way the
    /// interactive host does, and report what the Upstream was asked for.
    void run_session() {
        coding_agent::runtime::AgentSessionCreationRequest request;
        request.execution_runtime_target = runtime.make_target();
        request.session_facts.no_skills = true;
        request.session_facts.no_prompt_templates = true;
        request.workspace = workspace.path();
        request.session_target = coding_agent::InMemorySessionTarget{};
        request.mcp_transport = transport;
        request.mcp_delay = [this](std::chrono::milliseconds wait, std::stop_token stop_token) {
            return delay.request(wait, stop_token);
        };
        request.request_model = tests::scripted_request_model("fake", "fake-model");
        auto overrides = tests::cli_fake_overrides(tests::models_from_provider(tests::make_scripted_fake_provider()));
        overrides.catalog_cache = cache;
        auto created = runtime.run(
                coding_agent::create_agent_session_async(std::move(request), std::nullopt, std::move(overrides)));
        REQUIRE(created.has_value());
        auto& session = runtime.adopt_session(std::move(created->session));
        session.close();
    }

    /// Every wait here is a cap, not a wait: a passing case returns as soon as
    /// its condition holds.
    template <typename Condition> [[nodiscard]] static bool wait_until(Condition condition) {
        const auto deadline = std::chrono::steady_clock::now() + kOperationBudget;
        while (!condition() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
        }
        return condition();
    }

    [[nodiscard]] std::size_t listings() const { return transport->request_count("tools/list"); }
};

} // namespace

TEST_CASE("the production host shares one warm cache between two consecutive sessions",
        "[mcp][catalog][cache][session][issue848][spec]") {
    ProductionFixture fixture;
    fixture.configure("https://mcp.example/mcp");
    // The Upstream opts into the wider scope, which is the only way its
    // catalog may be reused by a session that did not fetch it.
    fixture.answer_catalog(/* process_wide */ true);

    fixture.run_session();
    REQUIRE(ProductionFixture::wait_until([&fixture] { return fixture.listings() == 1; }));

    fixture.run_session();
    // The second session issued no `tools/list` at all: the whole of spec
    // #833 story 17, on the production path.
    REQUIRE(ProductionFixture::wait_until(
            [&fixture] { return fixture.transport->request_count("server/discover") == 2; }));
    CHECK(fixture.listings() == 1);
}

TEST_CASE("a session-scoped Upstream is still walked by every session",
        "[mcp][catalog][cache][session][issue848][spec]") {
    ProductionFixture fixture;
    fixture.configure("https://mcp.example/mcp");
    // The Upstream says nothing about a wider scope, so it stays in the
    // narrowest one and the cross-session cache must not serve it (ADR 0067).
    fixture.answer_catalog(/* process_wide */ false);

    fixture.run_session();
    REQUIRE(ProductionFixture::wait_until([&fixture] { return fixture.listings() == 1; }));
    fixture.run_session();
    REQUIRE(ProductionFixture::wait_until([&fixture] { return fixture.listings() == 2; }));
    CHECK(fixture.cache->size() == 0);
}

TEST_CASE("a Server Id re-pointed at another endpoint is not served the previous one's catalog",
        "[mcp][catalog][cache][session][issue848][spec]") {
    ProductionFixture fixture;
    fixture.configure("https://mcp.example/mcp");
    fixture.answer_catalog(/* process_wide */ true);

    fixture.run_session();
    REQUIRE(ProductionFixture::wait_until([&fixture] { return fixture.listings() == 1; }));

    // The user re-points the Server Id. The entry the first session left is
    // keyed by the endpoint it was walked from, so the new session walks its
    // own catalog rather than publishing the old Upstream's descriptors
    // (ADR 0067).
    fixture.configure("https://mcp-moved.example/mcp");
    fixture.run_session();
    REQUIRE(ProductionFixture::wait_until([&fixture] { return fixture.listings() == 2; }));
}
