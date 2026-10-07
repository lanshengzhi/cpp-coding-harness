// Spec #882 / ticket #884: pi's `mcp-auth.json` credential store, diffed
// against the frozen pi-v1.0.4 evidence bundle
// (`fixtures/pi-ai/v1.0.4/mcp-codemode/mcp-protocol-surface.json`
// `credentialStore`). The bundle pins the file name, the key format
// (`mcp__<server>|<url>`), and the stored-state shape. The separation cases
// name what a shape-only check would let through: a legacy bare-URL key that is
// read but never taken over, a migration that overwrites newer credentials, and
// a malformed store that is silently replaced by an empty one.

#include "coding_agent/mcp/McpAuthStore.hpp"

#include "support/Json.hpp"
#include "support/TempWorkspace.hpp"

#include <cch/support/JsonValue.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>

using namespace cch;

namespace {

using JsonObject = support::JsonValue::object_t;

[[nodiscard]] support::JsonValue load_protocol_surface() {
    const std::string path =
            std::string{CCH_SOURCE_DIR} + "/fixtures/pi-ai/v1.0.4/mcp-codemode/mcp-protocol-surface.json";
    std::ifstream input(path, std::ios::binary);
    const std::string text{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    auto parsed = support::read_json(text);
    REQUIRE(parsed.has_value());
    return std::move(*parsed);
}

[[nodiscard]] std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

[[nodiscard]] support::JsonValue parse_file(const std::filesystem::path& path) {
    auto parsed = support::read_json(read_file(path));
    REQUIRE(parsed.has_value());
    return std::move(*parsed);
}

[[nodiscard]] coding_agent::mcp::McpOAuthState sample_state(const std::string& url) {
    coding_agent::mcp::McpOAuthState state;
    state.server_url = url;
    coding_agent::mcp::McpOAuthTokens tokens;
    tokens.access_token = "dummy-access-token";
    tokens.token_type = "Bearer";
    tokens.refresh_token = "dummy-refresh-token";
    state.tokens = std::move(tokens);
    state.tokens_expire_at = 0;
    return state;
}

} // namespace

TEST_CASE("the mcp-auth.json key format and file shape match the pi-v1.0.4 bundle",
        "[coding_agent][mcp][issue884][spec]") {
    const auto surface = load_protocol_surface();
    const auto& credential_store = surface.get_object().at("credentialStore").get_object();
    CHECK(credential_store.at("file").get_string() == "mcp-auth.json");
    CHECK(credential_store.at("keyFormat").get_string() == "mcp__<server>|<serverUrl>");

    // The bundle's sample key is exactly `store_key(name, url)`.
    const auto& sample = credential_store.at("sampleFile").get_object();
    REQUIRE(sample.size() == 1);
    const std::string sample_key = sample.begin()->first;
    CHECK(coding_agent::mcp::McpAuthStore::store_key("filesystem", "https://example.com/mcp") == sample_key);

    tests::TempWorkspace workspace;
    coding_agent::mcp::McpAuthStore store(workspace.path() / "mcp-auth.json");
    REQUIRE(store.save("filesystem", "https://example.com/mcp", sample_state("https://example.com/mcp")).has_value());

    // The stored value carries the bundle's state shape: serverUrl, tokens, and
    // tokensExpireAt; the file is pretty-printed with a trailing newline.
    const auto document = parse_file(workspace.path() / "mcp-auth.json");
    const auto* states = document.get_if<JsonObject>();
    REQUIRE(states != nullptr);
    REQUIRE(states->contains(sample_key));
    const auto* state = states->at(sample_key).get_if<JsonObject>();
    REQUIRE(state != nullptr);
    CHECK(state->at("serverUrl").get_string() == "https://example.com/mcp");
    const auto* tokens = state->at("tokens").get_if<JsonObject>();
    REQUIRE(tokens != nullptr);
    CHECK(tokens->at("access_token").get_string() == "dummy-access-token");
    CHECK(tokens->at("token_type").get_string() == "Bearer");
    CHECK(tokens->at("refresh_token").get_string() == "dummy-refresh-token");
    CHECK(state->at("tokensExpireAt").get_number() == 0);
    CHECK(read_file(workspace.path() / "mcp-auth.json").ends_with("}\n"));
}

TEST_CASE("a legacy bare-URL key is taken over on the first load", "[coding_agent][mcp][issue884][spec]") {
    tests::TempWorkspace workspace;
    const auto path = workspace.path() / "mcp-auth.json";
    // Older versions keyed the state by the URL alone.
    workspace.write("mcp-auth.json",
            R"({"https://example.com/mcp": {"serverUrl": "https://example.com/mcp", "tokens": {"access_token": "dummy-legacy", "token_type": "Bearer"}}})");

    coding_agent::mcp::McpAuthStore store(path);
    auto loaded = store.load("filesystem", "https://example.com/mcp");
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->has_value());
    REQUIRE((*loaded)->tokens.has_value());
    CHECK((*loaded)->tokens->access_token == "dummy-legacy");

    // The load moved the state to the namespaced key and removed the legacy one.
    const auto document = parse_file(path);
    const auto* states = document.get_if<JsonObject>();
    REQUIRE(states != nullptr);
    CHECK(states->contains("mcp__filesystem|https://example.com/mcp"));
    CHECK_FALSE(states->contains("https://example.com/mcp"));
}

TEST_CASE("tokens reads a legacy key without taking it over", "[coding_agent][mcp][issue884][spec]") {
    tests::TempWorkspace workspace;
    const auto path = workspace.path() / "mcp-auth.json";
    workspace.write("mcp-auth.json",
            R"({"https://example.com/mcp": {"serverUrl": "https://example.com/mcp", "tokens": {"access_token": "dummy-legacy", "token_type": "Bearer"}}})");

    coding_agent::mcp::McpAuthStore store(path);
    auto tokens = store.tokens("filesystem", "https://example.com/mcp");
    REQUIRE(tokens.has_value());
    REQUIRE(tokens->has_value());
    CHECK((*tokens)->access_token == "dummy-legacy");

    // The legacy key is still there: reading tokens does not migrate.
    const auto document = parse_file(path);
    CHECK(document.get_object().contains("https://example.com/mcp"));
}

TEST_CASE("remove deletes the namespaced or legacy key", "[coding_agent][mcp][issue884][spec]") {
    tests::TempWorkspace workspace;
    const auto path = workspace.path() / "mcp-auth.json";
    coding_agent::mcp::McpAuthStore store(path);

    // Nothing stored is not an error.
    auto absent = store.remove("filesystem", "https://example.com/mcp");
    REQUIRE(absent.has_value());
    CHECK_FALSE(*absent);

    REQUIRE(store.save("filesystem", "https://example.com/mcp", sample_state("https://example.com/mcp")).has_value());
    auto removed = store.remove("filesystem", "https://example.com/mcp");
    REQUIRE(removed.has_value());
    CHECK(*removed);
    const auto document = parse_file(path);
    CHECK_FALSE(document.get_object().contains("mcp__filesystem|https://example.com/mcp"));

    // A legacy-only record is removed too.
    workspace.write("mcp-auth.json",
            R"({"https://example.com/mcp": {"serverUrl": "https://example.com/mcp", "tokens": {"access_token": "x", "token_type": "Bearer"}}})");
    auto removed_legacy = store.remove("filesystem", "https://example.com/mcp");
    REQUIRE(removed_legacy.has_value());
    CHECK(*removed_legacy);
}

TEST_CASE("an auth.json mcp__<server> record migrates into mcp-auth.json", "[coding_agent][mcp][issue884][spec]") {
    tests::TempWorkspace workspace;
    // The #875 shape: one `mcp__<server>` OAuth record in the shared auth.json.
    workspace.write("auth.json",
            R"({"mcp__radius": {"type": "oauth", "refresh": "dummy-refresh-token", "access": "dummy-access-token", "expires": 1234}, "other": {"type": "api_key", "key": "dummy-key"}})");

    coding_agent::mcp::McpAuthStore store(workspace.path() / "mcp-auth.json");
    auto migrated = store.migrate_from_auth_json(workspace.path() / "auth.json", "radius", "https://example.com/mcp");
    REQUIRE(migrated.has_value());
    CHECK(*migrated);

    // The state now lives in mcp-auth.json under the namespaced key.
    const auto states = parse_file(workspace.path() / "mcp-auth.json");
    const auto* root = states.get_if<JsonObject>();
    REQUIRE(root != nullptr);
    REQUIRE(root->contains("mcp__radius|https://example.com/mcp"));
    const auto* state = root->at("mcp__radius|https://example.com/mcp").get_if<JsonObject>();
    REQUIRE(state != nullptr);
    CHECK(state->at("serverUrl").get_string() == "https://example.com/mcp");
    CHECK(state->at("tokens").get_object().at("access_token").get_string() == "dummy-access-token");
    CHECK(state->at("tokens").get_object().at("refresh_token").get_string() == "dummy-refresh-token");
    CHECK(state->at("tokensExpireAt").get_number() == 1234);

    // The record left auth.json; the unrelated record stayed.
    const auto auth = parse_file(workspace.path() / "auth.json");
    const auto* auth_root = auth.get_if<JsonObject>();
    REQUIRE(auth_root != nullptr);
    CHECK_FALSE(auth_root->contains("mcp__radius"));
    CHECK(auth_root->contains("other"));

    // A second migration is a no-op (the state was already taken over).
    auto again = store.migrate_from_auth_json(workspace.path() / "auth.json", "radius", "https://example.com/mcp");
    REQUIRE(again.has_value());
    CHECK_FALSE(*again);
}

TEST_CASE("migration does not overwrite an existing stored state", "[coding_agent][mcp][issue884][spec]") {
    tests::TempWorkspace workspace;
    workspace.write(
            "auth.json", R"({"mcp__radius": {"type": "oauth", "refresh": "r", "access": "old-access", "expires": 1}})");
    coding_agent::mcp::McpAuthStore store(workspace.path() / "mcp-auth.json");
    auto state = sample_state("https://example.com/mcp");
    state.tokens->access_token = "new-access";
    REQUIRE(store.save("radius", "https://example.com/mcp", state).has_value());

    auto migrated = store.migrate_from_auth_json(workspace.path() / "auth.json", "radius", "https://example.com/mcp");
    REQUIRE(migrated.has_value());
    CHECK_FALSE(*migrated);

    auto loaded = store.load("radius", "https://example.com/mcp");
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->has_value());
    CHECK((*loaded)->tokens->access_token == "new-access");
    // The un-migrated auth.json record is left alone, not deleted.
    CHECK(parse_file(workspace.path() / "auth.json").get_object().contains("mcp__radius"));
}

TEST_CASE("a malformed mcp-auth.json is an explicit error, never silently replaced",
        "[coding_agent][mcp][issue884][spec]") {
    tests::TempWorkspace workspace;
    const auto path = workspace.path() / "mcp-auth.json";
    workspace.write("mcp-auth.json", "{ not json");

    coding_agent::mcp::McpAuthStore store(path);
    auto loaded = store.load("filesystem", "https://example.com/mcp");
    REQUIRE_FALSE(loaded.has_value());
    auto saved = store.save("filesystem", "https://example.com/mcp", sample_state("https://example.com/mcp"));
    REQUIRE_FALSE(saved.has_value());
    // The file was not overwritten with a fresh store.
    CHECK(read_file(path) == "{ not json");
}
