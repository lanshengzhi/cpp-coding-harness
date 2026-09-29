// The session-to-session half of the tool-catalog cache (issue #848): two
// consecutive `McpSessionHost`s that share one cache, and the second session's
// Upstream is never asked for its tools again.
//
// The whole case is driven through the one MCP Host seam the wiring already
// has — the injected `tests::ScriptedMcpTransport` — plus the cache value the
// caller supplies. No second seam is added, and nothing here reaches past the
// transport: what the Upstream was asked for is read back off it.
//
// The boundary this file also records: `McpSessionHostOptions::catalog_cache`
// is a seam the *production* caller must fill, and the long-lived owner of
// that instance is the tool-publication ticket's decision (#842), not this
// one's. The evidence below is therefore about the wiring accepting and
// sharing the value, not about the composition root doing it.

#include <cch/coding_agent/AuthStorage.hpp>
#include "coding_agent/McpCredentialStore.hpp"
#include "coding_agent/runtime/McpSessionHost.hpp"
#include "support/PumpUntil.hpp"
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
