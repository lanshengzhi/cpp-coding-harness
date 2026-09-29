// The MCP Host's credential store over pi's shared `auth.json`
// (issue #838, spec #833 story 33).
//
// The `cch_mcp` package owns upstream credential handling; this file covers
// the coding-agent half of the handoff — that one Server Id is one
// `mcp.<server-id>` key in the shared store, that a write goes through the
// store's only write path without disturbing another record, and that a record
// of another type is never replaced by a bearer.

#include <cch/ai/CredentialStore.hpp>
#include <cch/coding_agent/AuthStorage.hpp>
#include "coding_agent/McpCredentialStore.hpp"
#include "support/AsyncResultBridge.hpp"
#include "support/TempWorkspace.hpp"

#include <catch2/catch_test_macros.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

/// The token the bearer path persists. The store never reports it anywhere but
/// the value a trusted caller reads back.
constexpr std::string_view kToken{"pike-mcp-upstream-bearer-0123456789abcdef"};

[[nodiscard]] std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.is_open());
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

template <typename T, typename Action>
T run_async(Action action) {
    boost::asio::io_context io;
    std::optional<T> result;
    boost::asio::co_spawn(
            io,
            [&]() -> boost::asio::awaitable<void> {
                result.emplace(co_await cch::support::detail::await_async_result(action()));
                co_return;
            },
            boost::asio::detached);
    io.run();
    REQUIRE(result.has_value());
    return std::move(*result);
}

[[nodiscard]] std::shared_ptr<cch::coding_agent::AuthStorage> open(const std::filesystem::path& path) {
    return std::make_shared<cch::coding_agent::AuthStorage>(path);
}

} // namespace

TEST_CASE("the MCP credential store persists an Upstream bearer under its mcp.<server-id> key",
        "[coding_agent][credentials][mcp][auth][issue838][spec]") {
    cch::tests::TempWorkspace workspace;
    const auto path = workspace.path() / "auth.json";
    auto storage = open(path);
    cch::coding_agent::McpCredentialStore store(storage);

    // Nothing is stored yet, and an absent key is not a failure.
    const auto absent = run_async<cch::support::Expected<std::optional<std::string>>>([&]() {
        return store.read_bearer("executor");
    });
    REQUIRE(absent);
    CHECK_FALSE(absent->has_value());

    const auto written = run_async<cch::support::Expected<void>>([&]() {
        return store.write_bearer("executor", std::string{kToken});
    });
    REQUIRE(written);

    // The record is one ordinary API-key record under the `mcp.<server-id>`
    // key, so the store's key map stays a plain provider-id map and an MCP
    // credential can never collide with a provider's.
    const auto record = run_async<cch::support::Expected<std::optional<cch::ai::Credential>>>([&]() {
        return storage->read("mcp.executor");
    });
    REQUIRE(record);
    REQUIRE(record->has_value());
    const auto* const api_key = std::get_if<cch::ai::ApiKeyCredential>(&**record);
    REQUIRE(api_key != nullptr);
    REQUIRE(api_key->key.has_value());
    CHECK(*api_key->key == kToken);

    // The read path answers with the same value, and a second store over the
    // same file sees it: the credential is durable, not per-process.
    const auto resolved = run_async<cch::support::Expected<std::optional<std::string>>>([&]() {
        return cch::coding_agent::McpCredentialStore(open(path)).read_bearer("executor");
    });
    REQUIRE(resolved);
    REQUIRE(resolved->has_value());
    CHECK(*resolved == kToken);

    // `list()` is metadata-only and reports the key without resolving it.
    const auto listed = run_async<cch::support::Expected<std::vector<cch::ai::CredentialInfo>>>([&]() {
        return storage->list();
    });
    REQUIRE(listed);
    REQUIRE(listed->size() == 1);
    CHECK(listed->front().provider_id == "mcp.executor");
    CHECK(listed->front().type == "api_key");
    CHECK(read_text(path).find("mcp.executor") != std::string::npos);
}

TEST_CASE("the MCP credential store leaves a provider's credential and a record of another type alone",
        "[coding_agent][credentials][mcp][auth][issue838][spec]") {
    cch::tests::TempWorkspace workspace;
    const auto path = workspace.path() / "auth.json";
    auto storage = open(path);
    cch::coding_agent::McpCredentialStore store(storage);

    // A provider credential and an OAuth record, as `cch_ai` OAuth (#849) will
    // leave under an MCP key.
    const auto seeded_provider = run_async<cch::support::Expected<std::optional<cch::ai::Credential>>>([&]() {
        return storage->modify(
                "anthropic",
                [](std::optional<cch::ai::Credential>)
                    -> cch::support::AsyncResult<std::optional<cch::ai::Credential>> {
                    return cch::support::AsyncResult<std::optional<cch::ai::Credential>>(
                            std::expected<std::optional<cch::ai::Credential>, cch::support::Error>{
                                    std::optional<cch::ai::Credential>{cch::ai::ApiKeyCredential{
                                            .key = "provider-key", .env = {}}}});
                });
    });
    REQUIRE(seeded_provider);
    const auto seeded_oauth = run_async<cch::support::Expected<std::optional<cch::ai::Credential>>>([&]() {
        return storage->modify(
                "mcp.executor",
                [](std::optional<cch::ai::Credential>)
                    -> cch::support::AsyncResult<std::optional<cch::ai::Credential>> {
                    return cch::support::AsyncResult<std::optional<cch::ai::Credential>>(
                            std::expected<std::optional<cch::ai::Credential>, cch::support::Error>{
                                    std::optional<cch::ai::Credential>{cch::ai::OAuthCredential{
                                            .refresh = "refresh", .access = "access", .expires = 0}}});
                });
    });
    REQUIRE(seeded_oauth);

    // An OAuth record is not a bearer credential, so it neither reads as one
    // nor is replaced by one.
    const auto not_a_bearer = run_async<cch::support::Expected<std::optional<std::string>>>([&]() {
        return store.read_bearer("executor");
    });
    REQUIRE(not_a_bearer);
    CHECK_FALSE(not_a_bearer->has_value());

    const auto refused = run_async<cch::support::Expected<void>>([&]() {
        return store.write_bearer("executor", std::string{kToken});
    });
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == cch::support::ErrorCode::Auth);

    const auto untouched = run_async<cch::support::Expected<std::optional<cch::ai::Credential>>>([&]() {
        return storage->read("mcp.executor");
    });
    REQUIRE(untouched);
    REQUIRE(untouched->has_value());
    CHECK(std::get_if<cch::ai::OAuthCredential>(&**untouched) != nullptr);

    // The provider's own credential is untouched as well.
    const auto provider = run_async<cch::support::Expected<std::optional<cch::ai::Credential>>>([&]() {
        return storage->read("anthropic");
    });
    REQUIRE(provider);
    REQUIRE(provider->has_value());
    const auto* const provider_key = std::get_if<cch::ai::ApiKeyCredential>(&**provider);
    REQUIRE(provider_key != nullptr);
    REQUIRE(provider_key->key.has_value());
    CHECK(*provider_key->key == "provider-key");
}

TEST_CASE("the MCP credential store replaces the bearer a Server Id already holds",
        "[coding_agent][credentials][mcp][auth][issue838][spec]") {
    cch::tests::TempWorkspace workspace;
    const auto path = workspace.path() / "auth.json";
    auto storage = open(path);
    cch::coding_agent::McpCredentialStore store(storage);

    REQUIRE(run_async<cch::support::Expected<void>>([&]() { return store.write_bearer("docs", "first-token"); }));
    REQUIRE(run_async<cch::support::Expected<void>>([&]() { return store.write_bearer("docs", "rotated-token"); }));

    const auto resolved = run_async<cch::support::Expected<std::optional<std::string>>>([&]() {
        return store.read_bearer("docs");
    });
    REQUIRE(resolved);
    REQUIRE(resolved->has_value());
    CHECK(*resolved == "rotated-token");

    // Two Server Ids never share a key.
    REQUIRE(run_async<cch::support::Expected<void>>([&]() { return store.write_bearer("executor", "other-token"); }));
    const auto other = run_async<cch::support::Expected<std::optional<std::string>>>([&]() {
        return store.read_bearer("executor");
    });
    REQUIRE(other);
    REQUIRE(other->has_value());
    CHECK(*other == "other-token");
    CHECK(read_text(path).find("first-token") == std::string::npos);
}
