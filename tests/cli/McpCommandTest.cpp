// The `pike mcp` CLI subcommand (spec #882, ticket #884): pi's `runMcpCommand`
// (`extensions/mcp/cli.ts`). The cases pin the verbatim output strings, the
// `mcp.json` write half, the `--json` report shape, and the exit codes; the
// connection probe is a scripted seam so `list`/`login` are deterministic.

#include "coding_agent/cli/McpCommand.hpp"
#include "support/Json.hpp"
#include "support/CliRunFixture.hpp"
#include "support/TempWorkspace.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cch/support/JsonValue.hpp>

#include <boost/asio/awaitable.hpp>

#include <filesystem>
#include <fstream>
#include <istream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace cch;

namespace {

[[nodiscard]] std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

/// Run `pike mcp ...` in-process with `XDG_CONFIG_HOME` pinned at `xdg_config`
/// so the global `mcp.json` lands in `<xdg_config>/pike/agent/mcp.json`.
[[nodiscard]] tests::CliRunResult run_pike(
        const std::filesystem::path& cwd, std::vector<std::string> args, const std::filesystem::path& xdg_config) {
    return tests::run_cli(tests::CliRunOptions{
            .args = std::move(args),
            .cwd = cwd,
            .env = {{"XDG_CONFIG_HOME", xdg_config.string()}},
            .stdin_text = {},
            .stdin_is_terminal = false,
            .stdout_is_terminal = false,
            .resume_picker = {},
    });
}

/// A scripted `McpServerProbe`: returns one preset outcome for every server.
class ScriptedProbe final : public cli::McpServerProbe {
public:
    explicit ScriptedProbe(cli::McpProbeResult result) : result_(std::move(result)) {}

    [[nodiscard]] boost::asio::awaitable<cli::McpProbeResult> probe(
            const coding_agent::mcp::McpConfigEntry&, const std::filesystem::path&) override {
        co_return result_;
    }

private:
    cli::McpProbeResult result_;
};

struct DirectRun {
    int exit_code{0};
    std::string stdout_text;
    std::string stderr_text;
};

[[nodiscard]] DirectRun run_direct(const std::filesystem::path& agent_dir,
        const std::filesystem::path& cwd,
        std::vector<std::string> args,
        std::shared_ptr<cli::McpServerProbe> probe = nullptr) {
    std::istringstream input;
    std::ostringstream output;
    std::ostringstream error;
    cli::McpCommandOptions options;
    options.cwd = cwd;
    options.agent_dir = agent_dir;
    options.output = &output;
    options.error = &error;
    options.input = &input;
    options.probe = std::move(probe);
    const int exit_code = cli::run_mcp_command(args, std::move(options));
    return DirectRun{exit_code, output.str(), error.str()};
}

constexpr const char* kHelpPrefix = "Usage:\n  pike mcp add <server> [options] -- <command> [args...]";

} // namespace

TEST_CASE("mcp add writes a streamable HTTP server to the global mcp.json", "[cli][mcp][issue884]") {
    tests::TempWorkspace home;
    tests::TempWorkspace cwd;
    const auto agent_dir = home.path() / ".config" / "pike" / "agent";

    const auto run =
            run_pike(cwd.path(), {"mcp", "add", "notion", "--url", "https://example.com/mcp"}, home.path() / ".config");

    REQUIRE(run.exit_code == 0);
    const auto config_path = agent_dir / "mcp.json";
    REQUIRE(std::filesystem::exists(config_path));
    CHECK(run.stdout_text ==
            "Added global MCP server \"notion\" in " + config_path.string() +
                    ".\nCheck it with: pike mcp list. If it requires sign-in: pike mcp login notion.\n");

    auto parsed = support::read_json(read_text(config_path));
    REQUIRE(parsed.has_value());
    const auto* root = parsed->get_if<support::JsonValue::object_t>();
    REQUIRE(root != nullptr);
    const auto servers = root->find("mcpServers");
    REQUIRE(servers != root->end());
    const auto* entries = servers->second.get_if<support::JsonValue::object_t>();
    REQUIRE(entries != nullptr);
    const auto entry = entries->find("notion");
    REQUIRE(entry != entries->end());
    const auto* fields = entry->second.get_if<support::JsonValue::object_t>();
    REQUIRE(fields != nullptr);
    REQUIRE(fields->find("url") != fields->end());
    CHECK(fields->at("url").get_string() == "https://example.com/mcp");
}

TEST_CASE("mcp add -- writes a stdio server with env, args, and cwd", "[cli][mcp][issue884]") {
    tests::TempWorkspace home;
    tests::TempWorkspace cwd;
    const auto agent_dir = home.path() / ".config" / "pike" / "agent";

    const auto run = run_pike(cwd.path(),
            {"mcp", "add", "echo", "--env", "FOO=bar", "--cwd", "/tmp/work", "--", "python3", "server.py", "--flag"},
            home.path() / ".config");

    REQUIRE(run.exit_code == 0);
    CHECK(run.stdout_text == "Added global MCP server \"echo\" in " + (agent_dir / "mcp.json").string() +
                                     ".\nCheck it with: pike mcp list.\n");

    auto parsed = support::read_json(read_text(agent_dir / "mcp.json"));
    REQUIRE(parsed.has_value());
    const auto* entries = parsed->at("mcpServers").get_if<support::JsonValue::object_t>();
    REQUIRE(entries != nullptr);
    const auto* fields = entries->at("echo").get_if<support::JsonValue::object_t>();
    REQUIRE(fields != nullptr);
    CHECK(fields->at("command").get_string() == "python3");
    REQUIRE(fields->at("args").get_array().size() == 2);
    CHECK(fields->at("args").get_array()[0].get_string() == "server.py");
    CHECK(fields->at("args").get_array()[1].get_string() == "--flag");
    CHECK(fields->at("env").get_object().at("FOO").get_string() == "bar");
    CHECK(fields->at("cwd").get_string() == "/tmp/work");
}

TEST_CASE("mcp add rejects options that do not match the transport", "[cli][mcp][issue884]") {
    tests::TempWorkspace home;
    tests::TempWorkspace cwd;
    const auto run = run_pike(cwd.path(),
            {"mcp", "add", "echo", "--env", "FOO=bar", "--url", "https://example.com/mcp"},
            home.path() / ".config");
    REQUIRE(run.exit_code == 1);
    CHECK(run.stderr_text.find("--env only applies to stdio servers.") == 0);
}

TEST_CASE("mcp add reports an unknown option and the help hint", "[cli][mcp][issue884]") {
    tests::TempWorkspace home;
    tests::TempWorkspace cwd;
    const auto run = run_pike(cwd.path(), {"mcp", "add", "echo", "--nope"}, home.path() / ".config");
    REQUIRE(run.exit_code == 1);
    CHECK(run.stderr_text == "Unknown option --nope.\n" + std::string{"Use \"pike mcp --help\" for usage."} + "\n");
}

TEST_CASE("mcp add requires exactly one of --url or a command", "[cli][mcp][issue884]") {
    tests::TempWorkspace home;
    tests::TempWorkspace cwd;
    const auto run = run_pike(cwd.path(), {"mcp", "add", "echo"}, home.path() / ".config");
    REQUIRE(run.exit_code == 1);
    CHECK(run.stderr_text.find("Usage: pike mcp add <server> [options] (--url <url> | -- <command> [args...])") == 0);
}

TEST_CASE("mcp add validates the exposure spelling", "[cli][mcp][issue884]") {
    tests::TempWorkspace home;
    tests::TempWorkspace cwd;
    const auto run = run_pike(cwd.path(),
            {"mcp", "add", "echo", "--url", "https://example.com/mcp", "--exposure", "bogus"},
            home.path() / ".config");
    REQUIRE(run.exit_code == 1);
    CHECK(run.stderr_text.find("exposure must be one of \"codemode\", \"deferred\", \"direct\", \"hidden\"") !=
            std::string::npos);
}

TEST_CASE("mcp remove deletes the entry and reports the scope", "[cli][mcp][issue884]") {
    tests::TempWorkspace home;
    tests::TempWorkspace cwd;
    const auto agent_dir = home.path() / ".config" / "pike" / "agent";

    REQUIRE(run_pike(cwd.path(), {"mcp", "add", "notion", "--url", "https://example.com/mcp"}, home.path() / ".config")
                    .exit_code == 0);
    const auto removed = run_pike(cwd.path(), {"mcp", "remove", "notion"}, home.path() / ".config");
    REQUIRE(removed.exit_code == 0);
    CHECK(removed.stdout_text ==
            "Removed global MCP server \"notion\" from " + (agent_dir / "mcp.json").string() + ".\n");

    const auto missing = run_pike(cwd.path(), {"mcp", "remove", "notion"}, home.path() / ".config");
    REQUIRE(missing.exit_code == 1);
    CHECK(missing.stderr_text ==
            "No global MCP server named \"notion\" in " + (agent_dir / "mcp.json").string() + ".\n");
}

TEST_CASE("mcp prints help for the bare, help, and --help forms", "[cli][mcp][issue884]") {
    tests::TempWorkspace home;
    tests::TempWorkspace cwd;
    for (const auto& args : {std::vector<std::string>{"mcp"},
                 std::vector<std::string>{"mcp", "help"},
                 std::vector<std::string>{"mcp", "--help"},
                 std::vector<std::string>{"mcp", "add", "--help"},
                 std::vector<std::string>{"mcp", "list", "-h"}}) {
        const auto run = run_pike(cwd.path(), args, home.path() / ".config");
        REQUIRE(run.exit_code == 0);
        CHECK(run.stdout_text.starts_with(kHelpPrefix));
    }
}

TEST_CASE("mcp reports an unknown command", "[cli][mcp][issue884]") {
    tests::TempWorkspace home;
    tests::TempWorkspace cwd;
    const auto run = run_pike(cwd.path(), {"mcp", "bogus"}, home.path() / ".config");
    REQUIRE(run.exit_code == 1);
    CHECK(run.stderr_text == "Unknown mcp command \"bogus\".\nUse \"pike mcp --help\" for usage.\n");
}

TEST_CASE("mcp list prints pi's empty-configuration line", "[cli][mcp][issue884]") {
    tests::TempWorkspace agent;
    tests::TempWorkspace cwd;
    const auto run = run_direct(agent.path(), cwd.path(), {"list"});
    REQUIRE(run.exit_code == 0);
    CHECK(run.stdout_text ==
            "No MCP servers configured. Add them to " + (agent.path() / "mcp.json").string() + " or .pi/mcp.json.\n");
}

TEST_CASE("mcp list reports a connected server and exits 0", "[cli][mcp][issue884]") {
    tests::TempWorkspace agent;
    tests::TempWorkspace cwd;
    agent.write("mcp.json", R"({"mcpServers":{"echo":{"command":"python3","args":["server.py"]}}})");

    cli::McpProbeResult connected;
    connected.state = cli::McpProbeResult::State::Connected;
    connected.tools = {"echo", "sum"};
    const auto run = run_direct(agent.path(), cwd.path(), {"list"}, std::make_shared<ScriptedProbe>(connected));

    REQUIRE(run.exit_code == 0);
    CHECK(run.stdout_text == "echo: connected, 2 tools (codemode, global)\n  python3 server.py\n  tools: echo, sum\n");
}

TEST_CASE("mcp list exits 1 when an enabled server is not connected", "[cli][mcp][issue884]") {
    tests::TempWorkspace agent;
    tests::TempWorkspace cwd;
    agent.write("mcp.json", R"({"mcpServers":{"notion":{"url":"https://example.com/mcp"}}})");

    cli::McpProbeResult needs_auth;
    needs_auth.state = cli::McpProbeResult::State::NeedsAuth;
    needs_auth.error = "MCP server 'notion' rejected the request (HTTP 401)";
    const auto run = run_direct(agent.path(), cwd.path(), {"list"}, std::make_shared<ScriptedProbe>(needs_auth));

    REQUIRE(run.exit_code == 1);
    CHECK(run.stdout_text == "notion: needs sign-in (codemode, global)\n  https://example.com/mcp\n"
                             "  sign in with: pike mcp login notion\n"
                             "  MCP server 'notion' rejected the request (HTTP 401)\n");
}

TEST_CASE("mcp list --json emits the server report shape", "[cli][mcp][issue884]") {
    tests::TempWorkspace agent;
    tests::TempWorkspace cwd;
    agent.write("mcp.json", R"({"mcpServers":{"echo":{"command":"python3"}}})");

    cli::McpProbeResult connected;
    connected.state = cli::McpProbeResult::State::Connected;
    connected.tools = {"echo"};
    const auto run =
            run_direct(agent.path(), cwd.path(), {"list", "--json"}, std::make_shared<ScriptedProbe>(connected));

    REQUIRE(run.exit_code == 0);
    auto parsed = support::read_json(run.stdout_text);
    REQUIRE(parsed.has_value());
    const auto* root = parsed->get_if<support::JsonValue::object_t>();
    REQUIRE(root != nullptr);
    const auto servers = root->find("servers");
    REQUIRE(servers != root->end());
    REQUIRE(servers->second.get_array().size() == 1);
    const auto* report = servers->second.get_array()[0].get_if<support::JsonValue::object_t>();
    REQUIRE(report != nullptr);
    CHECK(report->at("name").get_string() == "echo");
    CHECK(report->at("state").get_string() == "connected");
    CHECK(report->at("exposure").get_string() == "codemode");
    CHECK(report->at("scope").get_string() == "global");
    REQUIRE(report->at("tools").get_array().size() == 1);
    CHECK(report->at("tools").get_array()[0].get_string() == "echo");
}

TEST_CASE("mcp logout deletes stored credentials or reports none", "[cli][mcp][issue884]") {
    tests::TempWorkspace agent;
    tests::TempWorkspace cwd;
    agent.write("mcp.json", R"({"mcpServers":{"notion":{"url":"https://example.com/mcp"}}})");

    const auto none = run_direct(agent.path(), cwd.path(), {"logout", "notion"});
    REQUIRE(none.exit_code == 0);
    CHECK(none.stdout_text == "No stored credentials for MCP server \"notion\".\n");
}

TEST_CASE("mcp logout rejects a server that does not use OAuth", "[cli][mcp][issue884]") {
    tests::TempWorkspace agent;
    tests::TempWorkspace cwd;
    agent.write("mcp.json", R"({"mcpServers":{"echo":{"command":"python3"}}})");

    const auto run = run_direct(agent.path(), cwd.path(), {"logout", "echo"});
    REQUIRE(run.exit_code == 1);
    CHECK(run.stderr_text ==
            "MCP server \"echo\" does not use OAuth. Only HTTP servers without an Authorization header do.\n");
}

TEST_CASE("mcp login reports an unknown server and the configured names", "[cli][mcp][issue884]") {
    tests::TempWorkspace agent;
    tests::TempWorkspace cwd;
    agent.write("mcp.json", R"({"mcpServers":{"notion":{"url":"https://example.com/mcp"}}})");

    const auto run = run_direct(agent.path(), cwd.path(), {"login", "missing"});
    REQUIRE(run.exit_code == 1);
    CHECK(run.stderr_text == "No MCP server named \"missing\". Configured: notion.\n");
}

TEST_CASE("mcp login reports an already signed-in server", "[cli][mcp][issue884]") {
    tests::TempWorkspace agent;
    tests::TempWorkspace cwd;
    agent.write("mcp.json", R"({"mcpServers":{"notion":{"url":"https://example.com/mcp"}}})");

    cli::McpProbeResult connected;
    connected.state = cli::McpProbeResult::State::Connected;
    connected.tools = {"search", "fetch"};
    const auto run =
            run_direct(agent.path(), cwd.path(), {"login", "notion"}, std::make_shared<ScriptedProbe>(connected));
    REQUIRE(run.exit_code == 0);
    CHECK(run.stdout_text == "Already signed in to MCP server \"notion\" (2 tools).\n");
}

TEST_CASE("mcp login reports an unresolved OAuth server", "[cli][mcp][issue884]") {
    tests::TempWorkspace agent;
    tests::TempWorkspace cwd;
    agent.write("mcp.json", R"({"mcpServers":{"notion":{"url":"https://example.com/mcp","oauth":{}}}})");

    cli::McpProbeResult needs_auth;
    needs_auth.state = cli::McpProbeResult::State::NeedsAuth;
    const auto run =
            run_direct(agent.path(), cwd.path(), {"login", "notion"}, std::make_shared<ScriptedProbe>(needs_auth));
    REQUIRE(run.exit_code == 1);
    CHECK(run.stderr_text == "MCP server \"notion\" requires OAuth sign-in, but its authorization-server endpoints "
                             "are not resolved yet.\n");
}

TEST_CASE("mcp login rejects a non-positive timeout", "[cli][mcp][issue884]") {
    tests::TempWorkspace agent;
    tests::TempWorkspace cwd;
    agent.write("mcp.json", R"({"mcpServers":{"notion":{"url":"https://example.com/mcp"}}})");

    const auto run = run_direct(agent.path(), cwd.path(), {"login", "notion", "--timeout", "0"});
    REQUIRE(run.exit_code == 1);
    CHECK(run.stderr_text == "--timeout must be a positive number of seconds.\n");
}
