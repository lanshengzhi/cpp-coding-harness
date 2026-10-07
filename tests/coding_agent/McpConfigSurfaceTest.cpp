// Spec #882 / ticket #884: the MCP `mcp.json` configuration surface, diffed
// against the frozen pi-v1.0.4 evidence bundle
// (`fixtures/pi-ai/v1.0.4/mcp-codemode/mcp-config-surface.json`). The bundle is
// the acceptance authority (ADR 0066 ruling 3): the validation messages and the
// exposure/alias/`toolExposure` semantics are matched verbatim, not
// self-captured. The separation cases name what a shape-only check would let
// through: a validator that accepts an alias but reports the raw spelling, an
// exact `toolExposure` name that loses to a pattern, and a project override that
// leaks a credential-bearing global field.

#include "coding_agent/mcp/McpConfigFile.hpp"
#include "coding_agent/mcp/McpExposure.hpp"

#include "support/Json.hpp"
#include "support/JsonCompare.hpp"
#include "support/TempWorkspace.hpp"

#include <cch/support/JsonValue.hpp>

#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace cch;

namespace {

using JsonObject = support::JsonValue::object_t;
using JsonArray = support::JsonValue::array_t;

[[nodiscard]] support::JsonValue load_surface() {
    const std::string path =
            std::string{CCH_SOURCE_DIR} + "/fixtures/pi-ai/v1.0.4/mcp-codemode/mcp-config-surface.json";
    std::ifstream input(path, std::ios::binary);
    const std::string text{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    auto parsed = support::read_json(text);
    REQUIRE(parsed.has_value());
    return std::move(*parsed);
}

/// Project a validated config onto pi's `McpServerConfig` JSON shape so the
/// fixture's expected `config` object can be diffed. Only set fields appear,
/// matching pi (an omitted `exposure` stays omitted).
[[nodiscard]] support::JsonValue project_config(const coding_agent::mcp::McpServerConfigVariant& config) {
    const auto& base = std::visit(
            [](const auto& value) -> const coding_agent::mcp::McpServerConfigBase& { return value; }, config);
    support::JsonValue value{JsonObject{}};
    auto& object = value.get_object();
    if (base.exposure) {
        object.emplace("exposure", std::string{coding_agent::mcp::mcp_exposure_name(*base.exposure)});
    }
    if (base.description) {
        object.emplace("description", *base.description);
    }
    if (!base.tool_exposure.empty()) {
        support::JsonValue overrides{JsonObject{}};
        for (const auto& [name, exposure] : base.tool_exposure) {
            overrides.get_object().emplace(name, std::string{coding_agent::mcp::mcp_exposure_name(exposure)});
        }
        object.emplace("toolExposure", std::move(overrides));
    }
    if (base.timeout) {
        object.emplace("timeout", *base.timeout);
    }
    std::visit(
            [&object](const auto& typed) {
                using T = std::decay_t<decltype(typed)>;
                if constexpr (std::is_same_v<T, coding_agent::mcp::McpStdioServerConfig>) {
                    object.emplace("command", typed.command);
                    if (!typed.args.empty()) {
                        support::JsonValue args{JsonArray{}};
                        for (const auto& argument : typed.args) {
                            args.get_array().emplace_back(argument);
                        }
                        object.emplace("args", std::move(args));
                    }
                    if (!typed.env.empty()) {
                        support::JsonValue env{JsonObject{}};
                        for (const auto& [name, header] : typed.env) {
                            env.get_object().emplace(name, header);
                        }
                        object.emplace("env", std::move(env));
                    }
                } else {
                    object.emplace("url", typed.url);
                    if (!typed.headers.empty()) {
                        support::JsonValue headers{JsonObject{}};
                        for (const auto& [name, header] : typed.headers) {
                            headers.get_object().emplace(name, header);
                        }
                        object.emplace("headers", std::move(headers));
                    }
                    // pi carries the `oauth` block through unchanged, so every
                    // field it set appears here in pi's own spelling.
                    if (typed.oauth) {
                        support::JsonValue oauth{JsonObject{}};
                        auto& block = oauth.get_object();
                        if (typed.oauth->client_id) {
                            block.emplace("clientId", *typed.oauth->client_id);
                        }
                        if (typed.oauth->client_secret) {
                            block.emplace("clientSecret", *typed.oauth->client_secret);
                        }
                        if (typed.oauth->callback_port) {
                            block.emplace("callbackPort", static_cast<double>(*typed.oauth->callback_port));
                        }
                        if (typed.oauth->callback_url) {
                            block.emplace("callbackUrl", *typed.oauth->callback_url);
                        }
                        if (typed.oauth->scope) {
                            block.emplace("scope", *typed.oauth->scope);
                        }
                        if (typed.oauth->client_name) {
                            block.emplace("clientName", *typed.oauth->client_name);
                        }
                        if (typed.oauth->client_registration) {
                            block.emplace("clientRegistration",
                                    std::string{*typed.oauth->client_registration ==
                                                                coding_agent::mcp::McpClientRegistration::Cimd
                                                        ? "cimd"
                                                        : "dcr"});
                        }
                        if (typed.oauth->auth_server_metadata_url) {
                            block.emplace("authServerMetadataUrl", *typed.oauth->auth_server_metadata_url);
                        }
                        object.emplace("oauth", std::move(oauth));
                    }
                    if (typed.auth_provider) {
                        object.emplace("auth", support::JsonValue{JsonObject{{"provider", *typed.auth_provider}}});
                    }
                }
            },
            config);
    return value;
}

[[nodiscard]] const JsonObject& expect_object(const support::JsonValue& value, std::string_view key) {
    const auto& object = value.get_object();
    const auto found = object.find(std::string{key});
    REQUIRE(found != object.end());
    const auto* result = found->second.get_if<JsonObject>();
    REQUIRE(result != nullptr);
    return *result;
}

} // namespace

TEST_CASE("validateMcpServerConfig matches the pi-v1.0.4 exposure cases", "[coding_agent][mcp][issue884][spec]") {
    const auto surface = load_surface();
    const auto& cases = expect_object(surface, "exposureValues").at("cases").get_array();
    REQUIRE(cases.size() == 6);
    for (const auto& case_value : cases) {
        const auto& entry = case_value.get_object();
        const std::string exposure = entry.at("exposure").get_string();
        support::JsonValue raw{JsonObject{{"command", "node"}, {"exposure", exposure}}};
        auto validated = coding_agent::mcp::validate_mcp_server_config("demo", raw);
        const auto& expected = entry.at("result").get_object();
        if (const auto error = expected.find("error"); error != expected.end()) {
            REQUIRE_FALSE(validated.has_value());
            CHECK(validated.error().message == error->second.get_string());
            continue;
        }
        REQUIRE(validated.has_value());
        // The alias resolves to the canonical spelling in the returned config.
        const auto mismatch = tests::json_mismatch(expected.at("config"), project_config(*validated));
        CHECK_FALSE(mismatch.has_value());
        if (mismatch) {
            INFO(exposure << " -> " << *mismatch);
        }
    }
}

TEST_CASE(
        "validateMcpServerConfig matches the pi-v1.0.4 server entry examples", "[coding_agent][mcp][issue884][spec]") {
    const auto surface = load_surface();
    const auto& examples = surface.get_object().at("serverEntryExamples").get_array();

    const auto find_example = [&](std::string_view label) -> const support::JsonValue& {
        for (const auto& example : examples) {
            if (example.get_object().at("label").get_string() == label) {
                return example.get_object().at("result");
            }
        }
        FAIL("missing example " << label);
        return examples.front();
    };

    // A round-trip: pi's validated config is the input with aliases resolved, so
    // validating the expected config returns it unchanged.
    for (const std::string_view label : {"stdio", "http", "http+oauth", "http+auth"}) {
        const auto& result = find_example(label);
        const auto& expected = expect_object(result, "config");
        support::JsonValue raw{expected};
        auto validated = coding_agent::mcp::validate_mcp_server_config("demo", raw);
        REQUIRE(validated.has_value());
        const auto mismatch = tests::json_mismatch(support::JsonValue{expected}, project_config(*validated));
        CHECK_FALSE(mismatch.has_value());
        if (mismatch) {
            INFO(label << " -> " << *mismatch);
        }
    }

    // Legacy SSE is rejected with pi's message.
    {
        support::JsonValue raw{JsonObject{{"type", "sse"}, {"url", "https://example.com/mcp"}}};
        auto validated = coding_agent::mcp::validate_mcp_server_config("demo", raw);
        REQUIRE_FALSE(validated.has_value());
        const auto& expected = find_example("legacy-sse").get_object().at("error");
        CHECK(validated.error().message == expected.get_string());
    }

    // An invalid name is rejected before the body is read.
    {
        support::JsonValue raw{JsonObject{{"command", "node"}}};
        auto validated = coding_agent::mcp::validate_mcp_server_config("bad name", raw);
        REQUIRE_FALSE(validated.has_value());
        CHECK(validated.error().message == find_example("invalid-name").get_string());
    }
}

TEST_CASE("getMcpToolExposure matches the pi-v1.0.4 exact-beats-pattern rule", "[coding_agent][mcp][issue884][spec]") {
    using coding_agent::mcp::McpExposure;
    const auto surface = load_surface();
    const auto& expected = expect_object(surface, "exposureValues").at("toolExposure").get_object();
    const auto parse = [](const support::JsonValue& value) {
        auto parsed = coding_agent::mcp::parse_mcp_exposure(value.get_string());
        REQUIRE(parsed.has_value());
        return *parsed;
    };

    // One object: an exact name, a matching pattern, and two patterns whose
    // declaration order decides the winner.
    const std::vector<std::pair<std::string, McpExposure>> overrides = {
            {"read_*", McpExposure::Hidden},
            {"read_file", McpExposure::Direct},
            {"a*", McpExposure::Direct},
            {"*b", McpExposure::Hidden},
    };

    CHECK(coding_agent::mcp::get_mcp_tool_exposure(overrides, McpExposure::Codemode, "read_file") ==
            parse(expected.at("exactWins")));
    CHECK(coding_agent::mcp::get_mcp_tool_exposure(overrides, McpExposure::Codemode, "read_dir") ==
            parse(expected.at("patternMatch")));
    CHECK(coding_agent::mcp::get_mcp_tool_exposure(overrides, McpExposure::Codemode, "ab") ==
            parse(expected.at("firstPatternWins")));
    CHECK(coding_agent::mcp::get_mcp_tool_exposure(overrides, McpExposure::Deferred, "other") ==
            parse(expected.at("fallbackToServer")));
    // With no server exposure the default is `codemode` (pi `?? "codemode"`).
    CHECK(coding_agent::mcp::get_mcp_tool_exposure({}, std::nullopt, "other") == McpExposure::Codemode);
    // A key without `*` that is not an exact match never matches.
    CHECK(coding_agent::mcp::get_mcp_tool_exposure({{"read", McpExposure::Hidden}}, McpExposure::Direct, "read_file") ==
            McpExposure::Direct);
}

TEST_CASE("a project override merges only the pi override keys", "[coding_agent][mcp][issue884][spec]") {
    tests::TempWorkspace workspace;
    workspace.write("home/mcp.json",
            R"({"mcpServers": {"srv": {"command": "my-server", "headers": {"Authorization": "Bearer ${TOKEN}"}, "exposure": "codemode"}}})");
    workspace.write(".pi/mcp.json",
            R"({"mcpServers": {"srv": {"enabled": false, "exposure": "hidden", "toolExposure": {"dangerous": "direct"}}}})");

    const auto trusted =
            coding_agent::mcp::load_mcp_config(workspace.path() / "home", workspace.path(), /* project_trusted */ true);
    REQUIRE(trusted.errors.empty());
    REQUIRE(trusted.servers.size() == 1);
    const auto& entry = trusted.servers.front();
    CHECK_FALSE(entry.enabled);
    REQUIRE(entry.override.has_value());
    CHECK(*entry.override == workspace.path() / ".pi" / "mcp.json");

    // The override replaced exposure and toolExposure but kept the global
    // command (a repository cannot rewrite where the server runs).
    const auto* stdio = std::get_if<coding_agent::mcp::McpStdioServerConfig>(&entry.config);
    REQUIRE(stdio != nullptr);
    CHECK(stdio->command == "my-server");
    REQUIRE(stdio->exposure.has_value());
    CHECK(*stdio->exposure == coding_agent::mcp::McpExposure::Hidden);
    REQUIRE(stdio->tool_exposure.size() == 1);
    CHECK(stdio->tool_exposure.front().first == "dangerous");
    CHECK(stdio->tool_exposure.front().second == coding_agent::mcp::McpExposure::Direct);
}

TEST_CASE(
        "an override naming a field outside the pi override keys is rejected", "[coding_agent][mcp][issue884][spec]") {
    tests::TempWorkspace workspace;
    workspace.write("home/mcp.json", R"({"mcpServers": {"srv": {"command": "my-server"}}})");
    workspace.write(".pi/mcp.json", R"({"mcpServers": {"srv": {"args": ["--evil"]}}})");

    const auto trusted =
            coding_agent::mcp::load_mcp_config(workspace.path() / "home", workspace.path(), /* project_trusted */ true);
    REQUIRE(trusted.errors.size() == 1);
    CHECK(trusted.errors.front().find("an override can only set enabled, exposure, or toolExposure") !=
            std::string::npos);
    // The global entry is kept unchanged rather than half-overridden.
    REQUIRE(trusted.servers.size() == 1);
    const auto* stdio = std::get_if<coding_agent::mcp::McpStdioServerConfig>(&trusted.servers.front().config);
    REQUIRE(stdio != nullptr);
    CHECK(stdio->args.empty());
}

TEST_CASE("autoEnableCodemode is read from the global file and overridden by the project",
        "[coding_agent][mcp][issue884][spec]") {
    tests::TempWorkspace workspace;
    workspace.write("home/mcp.json", R"({"autoEnableCodemode": false, "mcpServers": {}})");
    workspace.write(".pi/mcp.json", R"({"autoEnableCodemode": true, "mcpServers": {}})");

    const auto global = coding_agent::mcp::load_mcp_config(workspace.path() / "home", workspace.path(), false);
    CHECK_FALSE(global.auto_enable_codemode);

    const auto trusted = coding_agent::mcp::load_mcp_config(workspace.path() / "home", workspace.path(), true);
    CHECK(trusted.auto_enable_codemode);
}

TEST_CASE("the mcp.json oauth block matches the pi-v1.0.4 validation rules", "[coding_agent][mcp][issue884][spec]") {
    using coding_agent::mcp::McpClientRegistration;
    const auto validate = [](support::JsonValue oauth) {
        return coding_agent::mcp::validate_mcp_server_config("demo",
                support::JsonValue{JsonObject{{"url", "https://example.com/mcp"}, {"oauth", std::move(oauth)}}});
    };
    const auto expect_error = [&](support::JsonValue oauth, std::string_view message) {
        auto validated = validate(std::move(oauth));
        REQUIRE_FALSE(validated.has_value());
        CHECK(validated.error().message == std::string{"server \"demo\": "} + std::string{message});
    };

    expect_error(support::JsonValue{JsonArray{}}, "oauth must be an object");
    expect_error(support::JsonValue{JsonObject{{"clientId", true}}}, "oauth.clientId must be a string");
    expect_error(support::JsonValue{JsonObject{{"clientSecret", 4}}}, "oauth.clientSecret must be a string");
    expect_error(support::JsonValue{JsonObject{{"callbackPort", 0}}}, "oauth.callbackPort must be a port number");
    expect_error(support::JsonValue{JsonObject{{"callbackPort", 65536}}}, "oauth.callbackPort must be a port number");
    expect_error(support::JsonValue{JsonObject{{"callbackPort", 1.5}}}, "oauth.callbackPort must be a port number");
    expect_error(support::JsonValue{JsonObject{{"callbackUrl", "https://127.0.0.1/callback"}}},
            "oauth.callbackUrl must be an http URI on localhost, 127.0.0.1, or [::1] without query or fragment");
    expect_error(support::JsonValue{JsonObject{{"callbackUrl", "http://127.0.0.1/callback?x=1"}}},
            "oauth.callbackUrl must be an http URI on localhost, 127.0.0.1, or [::1] without query or fragment");
    expect_error(support::JsonValue{JsonObject{{"callbackUrl", "http://example.com/callback"}}},
            "oauth.callbackUrl must be an http URI on localhost, 127.0.0.1, or [::1] without query or fragment");
    expect_error(
            support::JsonValue{JsonObject{{"callbackUrl", "http://127.0.0.1:7777/callback"}, {"callbackPort", 7778}}},
            "oauth.callbackUrl and oauth.callbackPort name different ports");
    expect_error(support::JsonValue{JsonObject{{"scope", 3}}}, "oauth.scope must be a string");
    expect_error(support::JsonValue{JsonObject{{"clientName", "  "}}}, "oauth.clientName must be a non-empty string");
    expect_error(support::JsonValue{JsonObject{{"clientRegistration", "x"}}},
            "oauth.clientRegistration must be \"dcr\" or \"cimd\"");
    expect_error(support::JsonValue{JsonObject{{"clientRegistration", "cimd"}, {"clientId", "id"}}},
            "oauth.clientRegistration \"cimd\" cannot be combined with oauth.clientId or oauth.clientName");
    expect_error(support::JsonValue{JsonObject{{"clientRegistration", "cimd"}, {"clientName", "n"}}},
            "oauth.clientRegistration \"cimd\" cannot be combined with oauth.clientId or oauth.clientName");
    expect_error(
            support::JsonValue{JsonObject{{"clientRegistration", "cimd"}, {"callbackUrl", "http://127.0.0.1:7/cb"}}},
            "oauth.clientRegistration \"cimd\" requires oauth.callbackUrl on localhost or 127.0.0.1 with path "
            "/callback");
    expect_error(
            support::JsonValue{JsonObject{{"clientRegistration", "cimd"}, {"callbackUrl", "http://[::1]:7/callback"}}},
            "oauth.clientRegistration \"cimd\" requires oauth.callbackUrl on localhost or 127.0.0.1 with path "
            "/callback");
    expect_error(support::JsonValue{JsonObject{{"authServerMetadataUrl", "http://example.com/.well-known"}}},
            "oauth.authServerMetadataUrl must be an https URL, or http on localhost, 127.0.0.1, or [::1]");

    // The fixture example round-trips: a pre-registered client with a fixed
    // loopback callback is accepted, and the block is carried in pi's spelling.
    {
        auto validated = validate(support::JsonValue{
                JsonObject{{"clientId", "dummy-client-id"}, {"callbackUrl", "http://127.0.0.1:7777/callback"}}});
        REQUIRE(validated.has_value());
        const auto* http = std::get_if<coding_agent::mcp::McpHttpServerConfig>(&*validated);
        REQUIRE(http != nullptr);
        REQUIRE(http->oauth.has_value());
        REQUIRE(http->oauth->client_id.has_value());
        CHECK(*http->oauth->client_id == "dummy-client-id");
        REQUIRE(http->oauth->callback_url.has_value());
        CHECK(*http->oauth->callback_url == "http://127.0.0.1:7777/callback");
    }
    // `cimd` with a `/callback` path on a loopback host is accepted and recorded.
    {
        auto validated = validate(support::JsonValue{
                JsonObject{{"clientRegistration", "cimd"}, {"callbackUrl", "http://localhost:7/callback"}}});
        REQUIRE(validated.has_value());
        const auto* http = std::get_if<coding_agent::mcp::McpHttpServerConfig>(&*validated);
        REQUIRE(http != nullptr);
        REQUIRE(http->oauth.has_value());
        CHECK(http->oauth->client_registration == McpClientRegistration::Cimd);
    }
}

TEST_CASE("auth is only allowed in the global mcp.json", "[coding_agent][mcp][issue884][spec]") {
    using coding_agent::mcp::McpHttpServerConfig;
    // A global entry may name a `/login` provider, but only over https (or a
    // loopback http URL).
    {
        support::JsonValue raw{JsonObject{
                {"url", "https://example.com/mcp"}, {"auth", support::JsonValue{JsonObject{{"provider", "demo"}}}}}};
        auto validated = coding_agent::mcp::validate_mcp_server_config("demo", raw);
        REQUIRE(validated.has_value());
        const auto* http = std::get_if<McpHttpServerConfig>(&*validated);
        REQUIRE(http != nullptr);
        REQUIRE(http->auth_provider.has_value());
        CHECK(*http->auth_provider == "demo");
    }
    {
        support::JsonValue raw{JsonObject{
                {"url", "http://example.com/mcp"}, {"auth", support::JsonValue{JsonObject{{"provider", "demo"}}}}}};
        auto validated = coding_agent::mcp::validate_mcp_server_config("demo", raw);
        REQUIRE_FALSE(validated.has_value());
        CHECK(validated.error().message ==
                "server \"demo\": auth requires an https URL, or http on localhost, 127.0.0.1, or [::1]");
    }
    {
        support::JsonValue raw{
                JsonObject{{"url", "https://example.com/mcp"}, {"auth", support::JsonValue{JsonObject{}}}}};
        auto validated = coding_agent::mcp::validate_mcp_server_config("demo", raw);
        REQUIRE_FALSE(validated.has_value());
        CHECK(validated.error().message == "server \"demo\": auth.provider must be a provider name");
    }

    // A project file cannot choose where the credential goes: the same entry is
    // rejected there with pi's message, while the global entry still loads.
    tests::TempWorkspace workspace;
    workspace.write("home/mcp.json",
            R"({"mcpServers": {"tools": {"url": "https://example.com/mcp", "auth": {"provider": "demo"}}}})");
    workspace.write(".pi/mcp.json",
            R"({"mcpServers": {"tools": {"url": "https://example.com/mcp", "auth": {"provider": "demo"}}}})");

    const auto trusted =
            coding_agent::mcp::load_mcp_config(workspace.path() / "home", workspace.path(), /* project_trusted */ true);
    REQUIRE(trusted.servers.size() == 1);
    CHECK(trusted.servers.front().name == "tools");
    REQUIRE(trusted.errors.size() == 1);
    CHECK(trusted.errors.front() == (workspace.path() / ".pi" / "mcp.json").string() +
                                            ": server \"tools\": auth is only allowed in the global mcp.json");
}
