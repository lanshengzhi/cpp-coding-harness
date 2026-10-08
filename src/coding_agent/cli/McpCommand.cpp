// The `pike mcp` CLI subcommand (spec #882, ticket #884). pi source at
// `7c10bd43` (v1.0.4): `packages/coding-agent/src/extensions/mcp/cli.ts`
// (`runMcpCommand`, `add`, `remove`, `list`, `login`, `logout`). The output
// strings, exit codes, and `--json` report shape follow that file; the server
// probe is an injectable seam so the command is testable without a live
// server. `login` drives the `mcp-auth.json`-backed OAuth flow
// (`McpOAuthProvider` + `login_mcp_server`); resolving an `oauth` block's
// endpoints is owned by the OAuth discovery lane, so an entry whose endpoints
// are not already resolved reports that gap explicitly instead of inventing
// one.

#include "coding_agent/cli/McpCommand.hpp"

#include "coding_agent/PrettyJson.hpp"
#include "coding_agent/mcp/McpConfigWrite.hpp"
#include "coding_agent/mcp/McpExposure.hpp"
#include "coding_agent/mcp/McpExtensionToolSource.hpp"
#include "coding_agent/mcp/McpHttpServerConfig.hpp"
#include "coding_agent/mcp/McpOAuthProvider.hpp"
#include "coding_agent/mcp/McpOAuthSignIn.hpp"
#include "coding_agent/mcp/McpOAuthTokenResolver.hpp"
#include "coding_agent/mcp/McpStdioServerConfig.hpp"
#include "support/AsyncResultBridge.hpp"

#include <cch/ai/Auth.hpp>
#include <cch/support/Error.hpp>
#include <cch/support/JsonValue.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/experimental/channel.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <expected>
#include <format>
#include <map>
#include <optional>
#include <sstream>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace cch::cli {
namespace {

using coding_agent::mcp::McpConfigEntry;
using coding_agent::mcp::McpConfigLoad;
using coding_agent::mcp::McpExposure;
using coding_agent::mcp::McpHttpServerConfig;
using coding_agent::mcp::McpStdioServerConfig;

constexpr int kDefaultLoginTimeoutSeconds = 300;

const char* const kHelpHint = "Use \"pike mcp --help\" for usage.";

[[nodiscard]] support::Error mcp_error(std::string message) {
    return support::make_error(support::ErrorCode::Validation, std::move(message));
}

/// pi's `HELP` (`cli.ts`) with the app name substituted.
[[nodiscard]] std::string help_text() {
    return std::string{"Usage:\n"
                       "  pike mcp add <server> [options] -- <command> [args...]\n"
                       "  pike mcp add <server> [options] --url <url>\n"
                       "  pike mcp remove <server> [-l]\n"
                       "  pike mcp list [--json]\n"
                       "  pike mcp login <server> [--timeout <seconds>]\n"
                       "  pike mcp logout <server>\n"
                       "\n"
                       "Configure and check MCP servers and sign in to OAuth servers without starting a session.\n"
                       "Reads ~/.pi/agent/mcp.json and, in trusted projects, .pi/mcp.json.\n"
                       "\n"
                       "Commands:\n"
                       "  add <server>            Add or replace a server in mcp.json\n"
                       "  remove <server>         Remove a server from mcp.json\n"
                       "  list                    Show state, tools, and errors (exits 1 on failure)\n"
                       "  login <server>          Sign in through the browser\n"
                       "  logout <server>         Delete the stored OAuth credentials\n"
                       "\n"
                       "Options for add and remove:\n"
                       "  -l, --local             Use .pi/mcp.json in the current project instead of the global file\n"
                       "\n"
                       "Options for add:\n"
                       "  --url <url>             Streamable HTTP server URL (instead of a command)\n"
                       "  --env <KEY=VALUE>       Environment variable for a stdio server (repeatable)\n"
                       "  --cwd <dir>             Working directory for a stdio server\n"
                       "  --header <KEY=VALUE>    HTTP header (repeatable)\n"
                       "  --bearer-token-env-var <NAME>\n"
                       "                          Send \"Authorization: Bearer ${NAME}\"\n"
                       "  --oauth-client-id <id>  Pre-registered OAuth client id\n"
                       "  --oauth-client-secret <secret>\n"
                       "                          OAuth client secret (may be ${NAME} or !command)\n"
                       "  --oauth-callback-port <port>\n"
                       "                          Fixed OAuth callback port\n"
                       "  --oauth-client-name <name>\n"
                       "                          Client name sent when registering with the OAuth server\n"
                       "  --exposure <mode>       codemode (default), deferred, direct, or hidden\n"
                       "  --description <text>    What the server offers, shown in the system prompt\n"
                       "\n"
                       "Other options:\n"
                       "  --json                  Print the list as JSON\n"
                       "  --timeout <seconds>     How long login waits for the browser (default: 300)"};
}

/// One parsed option value: a flag, a single value, or a repeated list.
enum class OptionKind { Flag, Value, List };

struct ParsedOptions {
    std::vector<std::string> positional;
    std::map<std::string, std::string> values;
    std::map<std::string, std::vector<std::string>> lists;

    [[nodiscard]] bool has_flag(std::string_view name) const { return values.contains(std::string{name}); }
    [[nodiscard]] std::optional<std::string> value(std::string_view name) const {
        const auto found = values.find(std::string{name});
        if (found == values.end()) {
            return std::nullopt;
        }
        return found->second;
    }
    [[nodiscard]] const std::vector<std::string>* list(std::string_view name) const {
        const auto found = lists.find(std::string{name});
        if (found == lists.end()) {
            return nullptr;
        }
        return &found->second;
    }
};

/// pi `parseOptions`: `--name value` options, `-l` aliased to `--local`, and
/// `--` ending the options so a stdio command's own flags pass through.
/// Reaching `max_positionals` does the same. An unknown option is reported and
/// yields `std::nullopt`.
[[nodiscard]] std::optional<ParsedOptions> parse_options(const std::vector<std::string>& args,
        const std::map<std::string, OptionKind>& known,
        std::ostream& error,
        std::size_t max_positionals = static_cast<std::size_t>(-1)) {
    ParsedOptions parsed;
    for (std::size_t index = 0; index < args.size(); ++index) {
        std::string_view arg = args[index];
        if (arg == "-l") {
            arg = "--local";
        }
        if (arg == "--" || parsed.positional.size() >= max_positionals) {
            const std::size_t start = arg == "--" ? index + 1 : index;
            for (std::size_t rest = start; rest < args.size(); ++rest) {
                parsed.positional.push_back(args[rest]);
            }
            break;
        }
        if (!arg.starts_with("--")) {
            parsed.positional.emplace_back(arg);
            continue;
        }
        const std::string name{arg.substr(2)};
        const auto kind = known.find(name);
        if (kind == known.end()) {
            error << "Unknown option " << arg << ".\n" << kHelpHint << '\n';
            return std::nullopt;
        }
        if (kind->second == OptionKind::Flag) {
            parsed.values.emplace(name, std::string{});
            continue;
        }
        if (index + 1 >= args.size()) {
            error << arg << " needs a value.\n";
            return std::nullopt;
        }
        std::string value = args[++index];
        if (kind->second == OptionKind::List) {
            parsed.lists[name].push_back(std::move(value));
        } else {
            parsed.values[name] = std::move(value);
        }
    }
    return parsed;
}

/// pi `parsePairs`: split repeated `KEY=VALUE` options into a record. Returns
/// `std::nullopt` after reporting a pair without a `KEY` part.
[[nodiscard]] std::optional<support::JsonValue::object_t> parse_pairs(
        const std::string& option, const std::vector<std::string>* pairs, std::ostream& error) {
    support::JsonValue::object_t record;
    if (pairs == nullptr) {
        return record;
    }
    for (const auto& pair : *pairs) {
        const auto separator = pair.find('=');
        if (separator == std::string::npos || separator == 0) {
            error << "--" << option << " expects KEY=VALUE, got \"" << pair << "\".\n";
            return std::nullopt;
        }
        record[pair.substr(0, separator)] = support::JsonValue{pair.substr(separator + 1)};
    }
    return record;
}

[[nodiscard]] bool has_authorization_header(const McpHttpServerConfig& config) {
    for (const auto& [key, value] : config.headers) {
        if (key.size() == 13) {
            std::string lowered = key;
            std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char character) {
                return static_cast<char>(std::tolower(character));
            });
            if (lowered == "authorization") {
                return true;
            }
        }
    }
    return false;
}

[[nodiscard]] std::string describe_transport(const McpConfigEntry& entry) {
    if (const auto* http = std::get_if<McpHttpServerConfig>(&entry.config)) {
        return http->url;
    }
    const auto& stdio = std::get<McpStdioServerConfig>(entry.config);
    std::string text = stdio.command;
    for (const auto& argument : stdio.args) {
        text += ' ';
        text += argument;
    }
    return text;
}

[[nodiscard]] std::optional<McpExposure> entry_exposure(const McpConfigEntry& entry) {
    return std::visit([](const auto& config) { return config.exposure; }, entry.config);
}

/// `--oauth-callback-port` value: a number when it is digits only, else the raw
/// string so `validateMcpServerConfig` reports pi's port error.
[[nodiscard]] support::JsonValue parse_port_or_string(const std::string& text) {
    if (text.empty()) {
        return support::JsonValue{text};
    }
    int value = 0;
    for (const char character : text) {
        if (character < '0' || character > '9') {
            return support::JsonValue{text};
        }
        value = value * 10 + (character - '0');
    }
    return support::JsonValue{static_cast<double>(value)};
}

/// The production probe (pi `createConnection`): connect the configured stdio
/// or streamable-HTTP server and list its tools. A server that needs OAuth
/// (the shared `OAuth` error code) is reported as `NeedsAuth`; any other
/// failure is `Failed` with the transport message.
class DefaultMcpServerProbe final : public McpServerProbe {
public:
    [[nodiscard]] boost::asio::awaitable<McpProbeResult> probe(
            const McpConfigEntry& entry, const std::filesystem::path& agent_dir) override {
        if (const auto* http = std::get_if<McpHttpServerConfig>(&entry.config)) {
            std::shared_ptr<coding_agent::mcp::McpRequestAuthSource> request_auth;
            const bool uses_oauth =
                    http->resolved_oauth.has_value() ||
                    (http->oauth.has_value() && !http->auth_provider && !has_authorization_header(*http));
            if (uses_oauth) {
                auto store = std::make_shared<coding_agent::mcp::McpAuthStore>(
                        coding_agent::mcp::McpAuthStore::default_path(agent_dir));
                (void)store->migrate_from_auth_json(agent_dir / "auth.json", http->name, http->url);
                if (http->resolved_oauth) {
                    auto provider = std::make_shared<coding_agent::mcp::McpOAuthProvider>(*http->resolved_oauth);
                    request_auth = std::make_shared<coding_agent::mcp::McpOAuthTokenResolver>(
                            store, http->name, http->url, std::move(provider));
                } else {
                    request_auth = std::make_shared<coding_agent::mcp::McpOAuthFlowTokenResolver>(
                            store, http->name, http->url, *http->oauth);
                }
            }
            auto source =
                    co_await coding_agent::mcp::McpExtensionToolSource::connect_http(*http, std::move(request_auth));
            if (!source) {
                co_return probe_failure(source.error());
            }
            co_return probe_success(**source);
        }
        auto source = co_await coding_agent::mcp::McpExtensionToolSource::connect_stdio(
                std::get<McpStdioServerConfig>(entry.config));
        if (!source) {
            co_return probe_failure(source.error());
        }
        co_return probe_success(**source);
    }

private:
    [[nodiscard]] static McpProbeResult probe_success(const coding_agent::mcp::McpExtensionToolSource& source) {
        McpProbeResult result;
        result.state = McpProbeResult::State::Connected;
        for (const auto& tool : source.tools()) {
            result.tools.push_back(tool.server_tool_name);
        }
        return result;
    }

    [[nodiscard]] static McpProbeResult probe_failure(const support::Error& error) {
        McpProbeResult result;
        result.state = error.code == support::ErrorCode::OAuth ? McpProbeResult::State::NeedsAuth
                                                               : McpProbeResult::State::Failed;
        result.error = error.message;
        return result;
    }
};

/// Run one probe to completion on a private loop.
[[nodiscard]] McpProbeResult run_probe(
        McpServerProbe& probe, const McpConfigEntry& entry, const std::filesystem::path& agent_dir) {
    boost::asio::io_context loop;
    std::optional<McpProbeResult> outcome;
    boost::asio::co_spawn(
            loop,
            [&probe, &entry, &agent_dir, &outcome, &loop]() -> boost::asio::awaitable<void> {
                outcome = co_await probe.probe(entry, agent_dir);
                loop.stop();
            },
            boost::asio::detached);
    loop.run();
    return outcome.value_or(McpProbeResult{});
}

[[nodiscard]] std::string project_config_path(const std::filesystem::path& cwd) {
    return (cwd / coding_agent::mcp::kMcpProjectConfigDir / "mcp.json").string();
}

struct ScopeInfo {
    std::string scope;
    std::string source;
    std::optional<std::string> override_path;
};

[[nodiscard]] ScopeInfo scope_info(const McpConfigEntry& entry, const std::filesystem::path& cwd) {
    ScopeInfo info;
    info.scope = "global";
    info.source = entry.source.string();
    if (entry.override) {
        info.override_path = entry.override->string();
    } else if (entry.source.string() == project_config_path(cwd)) {
        info.scope = "project";
    }
    return info;
}

struct ServerReport {
    McpConfigEntry entry;
    std::string scope;
    std::string source;
    std::optional<std::string> override_path;
    std::string exposure;
    std::string transport;
    std::string state{"disabled"};
    std::vector<std::string> tools;
    std::map<std::string, std::string> tool_exposure;
    std::string error;
};

[[nodiscard]] std::string state_text(const ServerReport& report) {
    if (report.state == "connected") {
        return std::format("connected, {} tool{}", report.tools.size(), report.tools.size() == 1 ? "" : "s");
    }
    if (report.state == "needs-auth") {
        return "needs sign-in";
    }
    return report.state;
}

[[nodiscard]] support::JsonValue report_json(const ServerReport& report) {
    using JsonObject = support::JsonValue::object_t;
    JsonObject object{
            {"name", report.entry.name},
            {"scope", report.scope},
            {"source", report.source},
            {"enabled", report.entry.enabled},
            {"exposure", report.exposure},
            {"transport", report.transport},
            {"state", report.state},
            {"tools", support::JsonValue{support::JsonValue::array_t{}}},
    };
    if (report.override_path) {
        object.emplace("override", *report.override_path);
    }
    auto& tools = object.at("tools").get_array();
    for (const auto& tool : report.tools) {
        tools.emplace_back(tool);
    }
    if (!report.tool_exposure.empty()) {
        JsonObject overrides;
        for (const auto& [name, exposure] : report.tool_exposure) {
            overrides.emplace(name, exposure);
        }
        object.emplace("toolExposure", support::JsonValue{std::move(overrides)});
    }
    if (!report.error.empty()) {
        object.emplace("error", report.error);
    }
    return support::JsonValue{std::move(object)};
}

[[nodiscard]] int add(
        const std::vector<std::string>& args, const McpCommandOptions& options, const std::string& project_config) {
    std::ostream& output = *options.output;
    std::ostream& error = *options.error;
    const std::string usage =
            std::format("Usage: pike mcp add <server> [options] (--url <url> | -- <command> [args...])\n{}", kHelpHint);
    const auto parsed = parse_options(args,
            {
                    {"local", OptionKind::Flag},
                    {"url", OptionKind::Value},
                    {"env", OptionKind::List},
                    {"cwd", OptionKind::Value},
                    {"header", OptionKind::List},
                    {"bearer-token-env-var", OptionKind::Value},
                    {"oauth-client-id", OptionKind::Value},
                    {"oauth-client-secret", OptionKind::Value},
                    {"oauth-callback-port", OptionKind::Value},
                    {"oauth-client-name", OptionKind::Value},
                    {"exposure", OptionKind::Value},
                    {"description", OptionKind::Value},
            },
            error,
            2);
    if (!parsed) {
        return 1;
    }
    const std::string name = parsed->positional.empty() ? std::string{} : parsed->positional[0];
    const std::vector<std::string> command(
            parsed->positional.size() > 1 ? parsed->positional.begin() + 1 : parsed->positional.end(),
            parsed->positional.end());
    const auto url = parsed->value("url");
    const bool http = url.has_value();
    if (name.empty() || (!http) == command.empty()) {
        error << usage << '\n';
        return 1;
    }
    const std::vector<std::string> http_only{"header",
            "bearer-token-env-var",
            "oauth-client-id",
            "oauth-client-secret",
            "oauth-callback-port",
            "oauth-client-name"};
    const std::vector<std::string> stdio_only{"env", "cwd"};
    const auto& misplaced_set = http ? stdio_only : http_only;
    for (const auto& option : misplaced_set) {
        const bool present = parsed->values.contains(option) || parsed->lists.contains(option);
        if (present) {
            error << "--" << option << " only applies to " << (http ? "stdio servers" : "HTTP servers (--url)")
                  << ".\n";
            return 1;
        }
    }

    support::JsonValue::object_t config;
    if (http) {
        auto headers = parse_pairs("header", parsed->list("header"), error);
        if (!headers) {
            return 1;
        }
        if (const auto bearer = parsed->value("bearer-token-env-var"); bearer.has_value()) {
            (*headers)["Authorization"] = support::JsonValue{std::format("Bearer ${{{}}}", *bearer)};
        }
        support::JsonValue::object_t oauth;
        if (const auto value = parsed->value("oauth-client-id"); value.has_value()) {
            oauth["clientId"] = support::JsonValue{*value};
        }
        if (const auto value = parsed->value("oauth-client-secret"); value.has_value()) {
            oauth["clientSecret"] = support::JsonValue{*value};
        }
        if (const auto value = parsed->value("oauth-callback-port"); value.has_value()) {
            oauth["callbackPort"] = parse_port_or_string(*value);
        }
        if (const auto value = parsed->value("oauth-client-name"); value.has_value()) {
            oauth["clientName"] = support::JsonValue{*value};
        }
        config["url"] = support::JsonValue{*url};
        if (!headers->empty()) {
            config["headers"] = support::JsonValue{std::move(*headers)};
        }
        if (!oauth.empty()) {
            config["oauth"] = support::JsonValue{std::move(oauth)};
        }
    } else {
        auto env = parse_pairs("env", parsed->list("env"), error);
        if (!env) {
            return 1;
        }
        config["command"] = support::JsonValue{command.front()};
        if (command.size() > 1) {
            support::JsonValue::array_t rest;
            for (std::size_t index = 1; index < command.size(); ++index) {
                rest.emplace_back(command[index]);
            }
            config["args"] = support::JsonValue{std::move(rest)};
        }
        if (!env->empty()) {
            config["env"] = support::JsonValue{std::move(*env)};
        }
        if (const auto value = parsed->value("cwd"); value.has_value()) {
            config["cwd"] = support::JsonValue{*value};
        }
    }
    if (const auto value = parsed->value("exposure"); value.has_value()) {
        config["exposure"] = support::JsonValue{*value};
    }
    if (const auto value = parsed->value("description"); value.has_value()) {
        config["description"] = support::JsonValue{*value};
    }

    const support::JsonValue entry_value{std::move(config)};
    auto validated = coding_agent::mcp::validate_mcp_server_config(name, entry_value);
    if (!validated) {
        error << validated.error().message << '\n';
        return 1;
    }

    const bool project = parsed->has_flag("local");
    const std::string path = project ? project_config : (options.agent_dir / "mcp.json").string();
    auto added = coding_agent::mcp::add_mcp_server_config(path, name, entry_value);
    if (!added) {
        error << "Could not update " << path << ": " << added.error().message << '\n';
        return 1;
    }
    output << (*added ? "Replaced " : "Added ") << (project ? "project" : "global") << " MCP server \"" << name
           << "\" in " << path << ".\n";
    if (project && !options.project_trusted) {
        output << "The project is not trusted, so " << path << " is ignored until you start pike in the project and "
               << "trust it.\n";
    }
    bool may_need_sign_in = false;
    if (const auto* http_config = std::get_if<McpHttpServerConfig>(&*validated);
            http_config != nullptr && !has_authorization_header(*http_config)) {
        may_need_sign_in = true;
    }
    output << "Check it with: pike mcp list";
    if (may_need_sign_in) {
        output << ". If it requires sign-in: pike mcp login " << name;
    }
    output << ".\n";
    return 0;
}

[[nodiscard]] int remove(
        const std::vector<std::string>& args, const McpCommandOptions& options, const std::string& project_config) {
    std::ostream& output = *options.output;
    std::ostream& error = *options.error;
    const auto parsed = parse_options(args, {{"local", OptionKind::Flag}}, error);
    if (!parsed) {
        return 1;
    }
    const std::string name = parsed->positional.empty() ? std::string{} : parsed->positional[0];
    if (name.empty() || parsed->positional.size() > 1) {
        error << std::format("Usage: pike mcp remove <server> [-l]\n{}", kHelpHint) << '\n';
        return 1;
    }
    const bool project = parsed->has_flag("local");
    const std::string global_config = (options.agent_dir / "mcp.json").string();
    const std::string path = project ? project_config : global_config;
    auto removed = coding_agent::mcp::remove_mcp_server_config(path, name);
    if (!removed) {
        error << "Could not update " << path << ": " << removed.error().message << '\n';
        return 1;
    }
    if (*removed) {
        output << "Removed " << (project ? "project" : "global") << " MCP server \"" << name << "\" from " << path
               << ".\n";
        return 0;
    }
    const McpConfigLoad loaded =
            coding_agent::mcp::load_mcp_config(options.agent_dir, options.cwd, /* project_trusted */ true);
    std::string other;
    for (const auto& entry : loaded.servers) {
        if (entry.name != name) {
            continue;
        }
        const ScopeInfo info = scope_info(entry, options.cwd);
        if (info.scope == (project ? "project" : "global")) {
            continue;
        }
        other = std::format(
                " It is defined in {}{}.", info.source, info.scope == "project" ? "; use --local" : "; omit --local");
        break;
    }
    error << "No " << (project ? "project" : "global") << " MCP server named \"" << name << "\" in " << path << "."
          << other << '\n';
    return 1;
}

[[nodiscard]] std::vector<ServerReport> build_reports(
        const McpConfigLoad& loaded, const McpCommandOptions& options, McpServerProbe& probe) {
    std::vector<ServerReport> reports;
    reports.reserve(loaded.servers.size());
    for (const auto& entry : loaded.servers) {
        ServerReport report;
        report.entry = entry;
        const ScopeInfo info = scope_info(entry, options.cwd);
        report.scope = info.scope;
        report.source = info.source;
        report.override_path = info.override_path;
        const std::optional<McpExposure> exposure = entry_exposure(entry);
        report.exposure = std::string{coding_agent::mcp::mcp_exposure_name(exposure.value_or(McpExposure::Codemode))};
        report.transport = describe_transport(entry);
        if (!entry.enabled) {
            reports.push_back(std::move(report));
            continue;
        }
        const McpProbeResult probed = run_probe(probe, entry, options.agent_dir);
        switch (probed.state) {
        case McpProbeResult::State::Connected:
            report.state = "connected";
            break;
        case McpProbeResult::State::NeedsAuth:
            report.state = "needs-auth";
            break;
        case McpProbeResult::State::Failed:
            report.state = "failed";
            break;
        }
        report.tools = probed.tools;
        if (report.state != "connected") {
            report.error = probed.error;
        }
        for (const auto& tool : report.tools) {
            const std::string override_exposure{
                    coding_agent::mcp::mcp_exposure_name(coding_agent::mcp::get_mcp_tool_exposure(
                            std::visit([](const auto& config) { return config.tool_exposure; }, entry.config),
                            exposure,
                            tool))};
            if (override_exposure != report.exposure) {
                report.tool_exposure.emplace(tool, override_exposure);
            }
        }
        reports.push_back(std::move(report));
    }
    return reports;
}

[[nodiscard]] int list(
        const std::vector<std::string>& args, const McpCommandOptions& options, const std::string& project_config) {
    std::ostream& output = *options.output;
    std::ostream& error = *options.error;
    const auto parsed = parse_options(args, {{"json", OptionKind::Flag}}, error);
    if (!parsed) {
        return 1;
    }
    if (!parsed->positional.empty()) {
        error << std::format("Usage: pike mcp list [--json]\n{}", kHelpHint) << '\n';
        return 1;
    }
    const McpConfigLoad loaded =
            coding_agent::mcp::load_mcp_config(options.agent_dir, options.cwd, options.project_trusted);
    const std::string untrusted_note =
            (!options.project_trusted && std::filesystem::exists(project_config))
                    ? std::format("{} is ignored because the project is not trusted. Start pike in the project to "
                                  "trust it.",
                              project_config)
                    : std::string{};
    DefaultMcpServerProbe default_probe;
    McpServerProbe& probe = options.probe ? *options.probe : default_probe;
    const std::vector<ServerReport> reports = build_reports(loaded, options, probe);
    bool failed = !loaded.errors.empty();
    for (const auto& report : reports) {
        if (report.entry.enabled && report.state != "connected") {
            failed = true;
        }
    }

    if (parsed->has_flag("json")) {
        using JsonObject = support::JsonValue::object_t;
        support::JsonValue::array_t servers;
        for (const auto& report : reports) {
            servers.push_back(report_json(report));
        }
        JsonObject root{
                {"servers", support::JsonValue{std::move(servers)}},
                {"errors", support::JsonValue{support::JsonValue::array_t{}}},
        };
        auto& errors = root.at("errors").get_array();
        for (const auto& config_error : loaded.errors) {
            errors.emplace_back(config_error);
        }
        if (!untrusted_note.empty()) {
            root.emplace("note", untrusted_note);
        }
        auto serialized = coding_agent::detail::serialize_pretty_json(support::JsonValue{std::move(root)}, false);
        if (!serialized) {
            error << serialized.error().message << '\n';
            return 1;
        }
        output << *serialized << '\n';
        return failed ? 1 : 0;
    }

    if (reports.empty() && loaded.errors.empty()) {
        output << "No MCP servers configured. Add them to " << (options.agent_dir / "mcp.json").string()
               << " or .pi/mcp.json.\n";
    }
    for (const auto& report : reports) {
        output << report.entry.name << ": " << state_text(report) << " (" << report.exposure << ", " << report.scope
               << ")\n";
        output << "  " << report.transport << '\n';
        if (report.override_path) {
            output << "  project override: " << *report.override_path << '\n';
        }
        if (report.state == "needs-auth") {
            output << "  sign in with: pike mcp login " << report.entry.name << '\n';
        }
        if (!report.tools.empty()) {
            output << "  tools: ";
            for (std::size_t index = 0; index < report.tools.size(); ++index) {
                if (index != 0) {
                    output << ", ";
                }
                const auto override = report.tool_exposure.find(report.tools[index]);
                output << report.tools[index];
                if (override != report.tool_exposure.end()) {
                    output << " [" << override->second << "]";
                }
            }
            output << '\n';
        }
        if (!report.error.empty()) {
            std::string displayed = report.error;
            std::size_t start = 0;
            while (start <= displayed.size()) {
                const auto newline = displayed.find('\n', start);
                output << "  " << displayed.substr(start, newline - start) << '\n';
                if (newline == std::string::npos) {
                    break;
                }
                start = newline + 1;
            }
        }
    }
    for (const auto& config_error : loaded.errors) {
        output << "config error: " << config_error << '\n';
    }
    if (!untrusted_note.empty()) {
        output << untrusted_note << '\n';
    }
    return failed ? 1 : 0;
}

/// `--oauth-callback-port` value: a number when it parses as a port, else the
/// raw string so `validateMcpServerConfig` reports pi's port error.
[[nodiscard]] const McpConfigEntry* find_entry(const McpConfigLoad& loaded, std::string_view name) {
    for (const auto& entry : loaded.servers) {
        if (entry.name == name) {
            return &entry;
        }
    }
    return nullptr;
}

/// The server's OAuth target URL, or `std::nullopt` when it does not use OAuth
/// (pi `connection.oauthUrl`): a streamable HTTP server without an `Authorization`
/// header and without an `auth.provider`.
[[nodiscard]] std::optional<std::string> oauth_url(const McpConfigEntry& entry) {
    const auto* http = std::get_if<McpHttpServerConfig>(&entry.config);
    if (http == nullptr || http->auth_provider.has_value() || has_authorization_header(*http)) {
        return std::nullopt;
    }
    if (http->url.empty()) {
        return std::nullopt;
    }
    return http->url;
}

[[nodiscard]] support::AsyncResult<void> run_oauth_login(const McpCommandOptions& options,
        const std::shared_ptr<std::move_only_function<void(std::string_view)>>& open_browser,
        const McpConfigEntry& entry,
        const std::string& url) {
    const auto* http = std::get_if<McpHttpServerConfig>(&entry.config);
    auto store = std::make_shared<coding_agent::mcp::McpAuthStore>(
            coding_agent::mcp::McpAuthStore::default_path(options.agent_dir));
    (void)store->migrate_from_auth_json(options.agent_dir / "auth.json", entry.name, url);
    auto provider = std::make_shared<coding_agent::mcp::McpOAuthProvider>(*http->resolved_oauth);
    ai::AuthInteraction interaction;
    interaction.notify = [&options, open_browser, name = entry.name](const ai::AuthEvent& event) {
        std::ostream& output = *options.output;
        if (const auto* progress = std::get_if<ai::AuthProgress>(&event.kind)) {
            output << progress->message << '\n';
        } else if (const auto* auth_url = std::get_if<ai::AuthUrl>(&event.kind)) {
            output << "Sign in to MCP server \"" << name << "\" in your browser:\n" << auth_url->url << '\n';
            if (open_browser && *open_browser) {
                (*open_browser)(auth_url->url);
            }
        }
    };
    interaction.prompt = [](ai::AuthPrompt) -> support::AsyncResult<std::string> {
        // The loopback callback completes the sign-in; the paste fallback is
        // not wired here (the non-interactive path pi also leaves to the
        // callback).
        return support::detail::make_async_result([]() -> boost::asio::awaitable<support::Expected<std::string>> {
            auto executor = co_await boost::asio::this_coro::executor;
            boost::asio::steady_timer timer(executor);
            timer.expires_after(std::chrono::hours{24 * 365 * 10});
            boost::system::error_code receive_error;
            co_await timer.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, receive_error));
            co_return std::unexpected(mcp_error("sign-in prompt woke"));
        });
    };
    // `login_mcp_server` borrows the provider, so keep it alive until the
    // returned operation settles; the shared_ptr ownership is captured by the
    // coroutine below.
    return support::detail::make_async_result(
            [store, provider, name = entry.name, url, interaction = std::move(interaction)]() mutable
                    -> boost::asio::awaitable<support::ExpectedVoid> {
                co_return co_await support::detail::await_async_result(
                        coding_agent::mcp::login_mcp_server(store, name, url, *provider, std::move(interaction)));
            });
}

/// Runs one sign-in operation and keeps its failure, so `login_or_logout`
/// can render pi `login`'s exact lines (`extensions/mcp/cli.ts`): a
/// cancellation names the `--timeout` bound, every other failure carries the
/// flow's message.
[[nodiscard]] std::optional<support::Error> run_result_failure(
        const std::shared_ptr<boost::asio::io_context>& loop, support::AsyncResult<void> operation) {
    // `outcome` is borrowed from this frame, which outlives `run()` on this
    // thread; the coroutine's `loop` copy only shares ownership with the
    // detached paste reader (see `wait_for_redirect_url`).
    std::optional<support::ExpectedVoid> outcome;
    boost::asio::co_spawn(
            *loop,
            [loop, &outcome, operation = std::move(operation)]() mutable -> boost::asio::awaitable<void> {
                outcome = co_await support::detail::await_async_result(std::move(operation));
                loop->stop();
            },
            boost::asio::detached);
    loop->run();
    if (!outcome || *outcome) {
        return std::nullopt;
    }
    return std::optional<support::Error>{std::move(outcome->error())};
}

/// The pasted redirect URL of `pike mcp login` (pi `waitForRedirectUrl`): a
/// terminal gets the paste prompt, everything else waits until the callback or
/// the timeout aborts the prompt. Resolves `std::nullopt` when aborted or when
/// the input ends.
/// `loop` is the shared pump context of the operation that runs this prompt.
/// The detached reader thread below owns a strong reference to it, so a read
/// that finishes after the race is lost posts into a live (no longer run)
/// context whose handler is never drained — posting to a live io_context is
/// always defined — instead of into a destroyed one.
[[nodiscard]] support::AsyncResult<std::optional<std::string>> wait_for_redirect_url(std::istream* input,
        std::ostream* error,
        bool interactive,
        std::shared_ptr<boost::asio::io_context> loop,
        std::stop_token stop) {
    return support::detail::make_async_result(
            [input, error, interactive, loop = std::move(loop), stop]()
                    -> boost::asio::awaitable<support::Expected<std::optional<std::string>>> {
                using Channel =
                        boost::asio::experimental::channel<void(boost::system::error_code, std::optional<std::string>)>;
                auto settled = std::make_shared<Channel>(co_await boost::asio::this_coro::executor, 1);
                if (interactive && input != nullptr) {
                    *error << "If the browser cannot reach this machine, paste the URL it was redirected to: "
                           << std::flush;
                    // Lifetime contract: the detached reader may outlive the
                    // operation (the browser callback or the timeout can win
                    // the race while `std::getline` is still blocked — it
                    // cannot be cancelled portably). It touches only `input`
                    // (process-lifetime stdin in production; a drained string
                    // stream in tests) and posts through the `loop` copy it
                    // owns, which keeps the io_context alive until the post
                    // completes; a still-blocked reader is reaped by process
                    // exit at the end of the command.
                    std::thread reader{[input, loop, settled]() {
                        std::string line;
                        const bool read = static_cast<bool>(std::getline(*input, line));
                        boost::asio::post(*loop, [settled, line = std::move(line), read]() {
                            if (read) {
                                settled->try_send(
                                        boost::system::error_code{}, std::optional<std::string>{std::move(line)});
                            } else {
                                settled->try_send(boost::system::error_code{}, std::optional<std::string>{});
                            }
                        });
                    }};
                    reader.detach();
                }
                // The losing side of the race (the callback or the timeout) aborts
                // this prompt through its stop token. This callback runs on
                // whichever thread requests stop, so it only posts (§6.2).
                std::stop_callback cancel{stop, [loop, settled] {
                                              boost::asio::post(*loop, [settled] {
                                                  settled->try_send(
                                                          boost::system::error_code{}, std::optional<std::string>{});
                                              });
                                          }};
                boost::system::error_code receive_error;
                auto outcome = co_await settled->async_receive(
                        boost::asio::redirect_error(boost::asio::use_awaitable, receive_error));
                if (receive_error || !outcome.has_value()) {
                    co_return std::optional<std::string>{};
                }
                co_return std::move(*outcome);
            });
}

/// `pike mcp login` on an `oauth` block (pi `signInMcpServer`): discovery and
/// registration happen inside the flow, the loopback callback races the pasted
/// redirect URL, and the tokens land in `mcp-auth.json`.
[[nodiscard]] support::AsyncResult<void> run_flow_login(const McpCommandOptions& options,
        const std::shared_ptr<std::move_only_function<void(std::string_view)>>& open_browser,
        const McpHttpServerConfig& http,
        const std::string& url,
        std::chrono::milliseconds timeout,
        std::shared_ptr<boost::asio::io_context> loop) {
    auto store = std::make_shared<coding_agent::mcp::McpAuthStore>(
            coding_agent::mcp::McpAuthStore::default_path(options.agent_dir));
    (void)store->migrate_from_auth_json(options.agent_dir / "auth.json", http.name, url);
    coding_agent::mcp::McpOAuthSignInRequest request;
    request.store = store;
    request.server_name = http.name;
    request.server_url = url;
    request.oauth = *http.oauth;
    request.timeout = timeout;
    request.prompt.show_authorization_url = [output = options.output, open_browser, name = http.name](
                                                    const std::string& authorization_url) {
        *output << "Sign in to MCP server \"" << name << "\" in your browser:\n" << authorization_url << '\n';
        if (open_browser && *open_browser) {
            (*open_browser)(authorization_url);
        }
    };
    if (options.stdin_is_terminal) {
        // Init-capture the fields the prompt reads so no reference to
        // `options` crosses into the stored operation (§6.2); `loop` is the
        // shared pump context the detached paste reader also owns.
        request.prompt.prompt_for_redirect_url = [input = options.input,
                                                         error = options.error,
                                                         interactive = options.stdin_is_terminal,
                                                         loop = std::move(loop)](std::stop_token stop) mutable {
            return wait_for_redirect_url(input, error, interactive, std::move(loop), std::move(stop));
        };
    }
    return coding_agent::mcp::sign_in_mcp_server(std::move(request));
}

[[nodiscard]] int login_or_logout(const std::string& command,
        const std::vector<std::string>& args,
        McpCommandOptions& options,
        const std::string& project_config) {
    std::ostream& output = *options.output;
    std::ostream& error = *options.error;
    const std::string usage = std::format("Usage: pike mcp {} <server>\n{}", command, kHelpHint);
    const auto parsed = parse_options(args,
            command == "login" ? std::map<std::string, OptionKind>{{"timeout", OptionKind::Value}}
                               : std::map<std::string, OptionKind>{},
            error);
    if (!parsed) {
        return 1;
    }
    const std::string name = parsed->positional.empty() ? std::string{} : parsed->positional[0];
    if (name.empty() || parsed->positional.size() > 1) {
        error << usage << '\n';
        return 1;
    }
    const McpConfigLoad loaded =
            coding_agent::mcp::load_mcp_config(options.agent_dir, options.cwd, options.project_trusted);
    const std::string untrusted_note =
            (!options.project_trusted && std::filesystem::exists(project_config))
                    ? std::format(" {} is ignored because the project is not trusted. Start pike in the project to "
                                  "trust it.",
                              project_config)
                    : std::string{};
    const McpConfigEntry* entry = find_entry(loaded, name);
    if (entry == nullptr) {
        std::string configured;
        for (std::size_t index = 0; index < loaded.servers.size(); ++index) {
            if (index != 0) {
                configured += ", ";
            }
            configured += loaded.servers[index].name;
        }
        error << "No MCP server named \"" << name << "\"." << untrusted_note
              << " Configured: " << (configured.empty() ? "none" : configured) << ".\n";
        return 1;
    }
    const auto url = oauth_url(*entry);
    if (!url) {
        error << "MCP server \"" << name
              << "\" does not use OAuth. Only HTTP servers without an Authorization header do.\n";
        return 1;
    }
    auto store = std::make_shared<coding_agent::mcp::McpAuthStore>(
            coding_agent::mcp::McpAuthStore::default_path(options.agent_dir));
    if (command == "logout") {
        auto removed = store->remove(name, *url);
        if (!removed) {
            error << removed.error().message << '\n';
            return 1;
        }
        if (*removed) {
            output << "Signed out of MCP server \"" << name << "\".\n";
        } else {
            output << "No stored credentials for MCP server \"" << name << "\".\n";
        }
        return 0;
    }

    double timeout_seconds = kDefaultLoginTimeoutSeconds;
    if (const auto value = parsed->value("timeout"); value.has_value()) {
        std::istringstream stream{*value};
        stream >> timeout_seconds;
        if (stream.fail() || !stream.eof() || timeout_seconds <= 0) {
            error << "--timeout must be a positive number of seconds.\n";
            return 1;
        }
    }
    const std::chrono::milliseconds timeout{static_cast<std::chrono::milliseconds::rep>(timeout_seconds * 1000.0)};

    DefaultMcpServerProbe default_probe;
    McpServerProbe& probe = options.probe ? *options.probe : default_probe;
    const McpProbeResult before = run_probe(probe, *entry, options.agent_dir);
    if (before.state == McpProbeResult::State::Connected) {
        output << "Already signed in to MCP server \"" << name << "\" (" << before.tools.size() << " tools).\n";
        return 0;
    }
    if (before.state != McpProbeResult::State::NeedsAuth) {
        error << "MCP server \"" << name
              << "\" failed to connect: " << (before.error.empty() ? "unknown error" : before.error) << '\n';
        return 1;
    }
    const auto* http = std::get_if<McpHttpServerConfig>(&entry->config);
    if (http == nullptr) {
        error << "MCP server \"" << name << "\" does not use OAuth. Only HTTP servers without an Authorization "
              << "header do.\n";
        return 1;
    }
    // The pump context is shared (not stack-owned) so the detached paste
    // reader in `wait_for_redirect_url` can keep it alive past this frame; the
    // leftover strong reference is reaped when the reader's read returns.
    auto loop = std::make_shared<boost::asio::io_context>();
    std::optional<support::Error> sign_in_failure;
    if (options.sign_in) {
        sign_in_failure = run_result_failure(loop, options.sign_in(*http, timeout));
    } else {
        auto shared_open_browser =
                std::make_shared<std::move_only_function<void(std::string_view)>>(std::move(options.open_browser));
        if (http->resolved_oauth) {
            sign_in_failure = run_result_failure(loop, run_oauth_login(options, shared_open_browser, *entry, *url));
        } else if (http->oauth) {
            sign_in_failure = run_result_failure(
                    loop, run_flow_login(options, shared_open_browser, *http, *url, timeout, std::move(loop)));
        } else {
            error << "MCP server \"" << name << "\" requires OAuth sign-in, but it has no oauth configuration.\n";
            return 1;
        }
    }
    if (sign_in_failure.has_value()) {
        // pi `login`'s catch (extensions/mcp/cli.ts): the cancelled error —
        // the user aborted, or the `--timeout` lapsed — names the bound;
        // every other failure carries the flow's message after "failed:".
        if (sign_in_failure->code == support::ErrorCode::Cancelled) {
            error << std::format("Sign-in to MCP server \"{}\" was cancelled or not completed within {} seconds.\n",
                    name,
                    std::lround(static_cast<double>(timeout.count()) / 1000.0));
        } else {
            error << std::format("Sign-in to MCP server \"{}\" failed: {}\n", name, sign_in_failure->message);
        }
        return 1;
    }
    const McpProbeResult after = run_probe(probe, *entry, options.agent_dir);
    if (after.state != McpProbeResult::State::Connected) {
        error << "Signed in, but failed to reconnect to MCP server \"" << name << "\".\n";
        return 1;
    }
    output << "Signed in to MCP server \"" << name << "\" (" << after.tools.size() << " tools).\n";
    return 0;
}

} // namespace

std::string mcp_help_text() { return help_text(); }

int run_mcp_command(const std::vector<std::string>& args, McpCommandOptions options) {
    if (!options.output || !options.error) {
        return 1;
    }
    std::ostream& output = *options.output;
    std::ostream& error = *options.error;
    if (args.empty()) {
        output << help_text() << '\n';
        return 0;
    }
    const bool help_requested = std::any_of(
            args.begin(), args.end(), [](const std::string& arg) { return arg == "--help" || arg == "-h"; });
    const std::string command = args.front();
    if (command == "help" || help_requested) {
        output << help_text() << '\n';
        return 0;
    }
    const std::string project_config = project_config_path(options.cwd);
    const std::vector<std::string> rest(args.begin() + 1, args.end());
    if (command == "add") {
        return add(rest, options, project_config);
    }
    if (command == "remove") {
        return remove(rest, options, project_config);
    }
    if (command == "list") {
        return list(rest, options, project_config);
    }
    if (command == "login" || command == "logout") {
        return login_or_logout(command, rest, options, project_config);
    }
    error << "Unknown mcp command \"" << command << "\".\n" << kHelpHint << '\n';
    return 1;
}

} // namespace cch::cli
