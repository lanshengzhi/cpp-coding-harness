// Ticket #1064 (spec #1044, ADR 0075): the reusable `cch_codemode` owner. This
// is a public caller, not an application session: it includes only the codemode
// Owner Interface headers, parses the frozen source/declaration observations,
// and executes a real script against an injected nested tool.
//
// Acceptance discipline: a compiled-but-inert header move would pass an
// existence check while the library still needed an application Session or had
// lost the tool bridge. The script below therefore calls the injected tool at
// runtime, and the failure case proves a rejected nested call reaches the
// script while prior output survives.

#include <cch/codemode/Declarations.hpp>
#include <cch/codemode/Sandbox.hpp>
#include <cch/codemode/Source.hpp>

#include "support/AsyncResultBridge.hpp"
#include "support/Json.hpp"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_message.hpp>

#include <algorithm>

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace cch;

namespace {

[[nodiscard]] std::filesystem::path audit_root() {
    return std::filesystem::path{CCH_SOURCE_DIR} / "fixtures" / "pi-v1.3.0" / "audit-1053";
}

[[nodiscard]] std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

[[nodiscard]] support::JsonValue read_artifact(std::string_view artifact) {
    auto parsed = support::read_json(read_text(audit_root() / artifact));
    REQUIRE(parsed.has_value());
    return std::move(*parsed);
}

[[nodiscard]] const support::JsonValue* find_scenario(const support::JsonValue& artifact, std::string_view name) {
    for (const auto& scenario : artifact.get_object().at("scenarios").get_array()) {
        if (scenario.get_object().at("name").get_string() == name) return &scenario;
    }
    return nullptr;
}

[[nodiscard]] const support::JsonValue& member(const support::JsonValue& object, std::string_view name) {
    const auto& fields = object.get_object();
    const auto found = fields.find(std::string{name});
    REQUIRE(found != fields.end());
    return found->second;
}

void check_options(const codemode::CodemodeSourceOptions& options, const support::JsonValue& expected) {
    const auto& fields = expected.get_object();
    if (const auto found = fields.find("timeoutMs"); found == fields.end()) {
        CHECK_FALSE(options.timeout_ms.has_value());
    } else {
        CHECK(options.timeout_ms.has_value());
        if (options.timeout_ms.has_value()) {
            CHECK(*options.timeout_ms == static_cast<std::int64_t>(found->second.get_number()));
        }
    }
    if (const auto found = fields.find("maxOutputTokens"); found == fields.end()) {
        CHECK_FALSE(options.max_output_tokens.has_value());
    } else {
        CHECK(options.max_output_tokens.has_value());
        if (options.max_output_tokens.has_value()) {
            CHECK(*options.max_output_tokens == static_cast<std::int64_t>(found->second.get_number()));
        }
    }
}

void check_parsed_source(std::string_view input, const support::JsonValue& expected) {
    auto parsed = codemode::parse_codemode_source(input);
    CHECK(parsed.has_value());
    if (!parsed) return;
    CHECK(parsed->code == member(expected, "code").get_string());
    check_options(parsed->options, member(expected, "options"));
}

template <typename T> [[nodiscard]] std::expected<T, support::Error> run_async(support::AsyncResult<T> operation) {
    boost::asio::io_context io;
    auto future = boost::asio::co_spawn(
            io,
            [](support::AsyncResult<T> pending) -> boost::asio::awaitable<std::expected<T, support::Error>> {
                co_return co_await support::detail::await_async_result(std::move(pending));
            }(std::move(operation)),
            boost::asio::use_future);
    io.run();
    return future.get();
}

[[nodiscard]] std::unique_ptr<codemode::CodemodeSandbox> make_sandbox(
        std::vector<codemode::CodemodeGlobal> globals = {}) {
    auto sandbox = codemode::CodemodeSandbox::create(codemode::default_codemode_guest_wasm_path(), std::move(globals));
    REQUIRE(sandbox.has_value());
    return std::move(*sandbox);
}

[[nodiscard]] codemode::CodemodeLimits limits() {
    codemode::CodemodeLimits bounds;
    bounds.timeout = std::chrono::seconds{30};
    return bounds;
}

} // namespace

TEST_CASE("the public codemode source parser keeps only a first-line options command",
        "[codemode][source][issue1064][spec]") {
    const auto artifact = read_artifact("source-options.json");

    // The frozen capture's accepted forms: plain code, first-line options,
    // leading whitespace/CRLF, and option-like text that is not the first line.
    const std::string_view accepted[] = {
            "source keeps plain JavaScript and a leading ordinary comment unchanged",
            "first-line options are parsed and replaced by an empty line to preserve line numbers",
            "options accept leading whitespace, CRLF, an empty object and a zero token budget",
            "only the first line is an options line",
    };
    for (const auto name : accepted) {
        const auto* scenario = find_scenario(artifact, name);
        REQUIRE(scenario != nullptr);
        INFO("scenario: " + std::string{name});
        const auto& captured = member(*scenario, "inputs");
        const auto& expected = member(*scenario, "expected");
        if (const auto found = captured.get_object().find("input"); found != captured.get_object().end()) {
            INFO("input: " + found->second.get_string());
            check_parsed_source(found->second.get_string(), expected);
            continue;
        }
        const auto& input_values = member(captured, "inputs").get_array();
        const auto& expected_values = expected.get_array();
        REQUIRE(input_values.size() == expected_values.size());
        for (std::size_t i = 0; i < input_values.size(); ++i) {
            INFO("input[" << i << "]: " << input_values[i].get_string());
            check_parsed_source(input_values[i].get_string(), expected_values[i]);
        }
    }

    const auto* constants = find_scenario(artifact, "the public options prefix and grammar constants are fixed");
    REQUIRE(constants != nullptr);
    const auto& exported = member(*constants, "expected");
    CHECK(codemode::kCodemodeOptionsPrefix == member(exported, "prefix").get_string());
    CHECK(codemode::kCodemodeSourceGrammar == member(exported, "grammar").get_string());
}

TEST_CASE("the public codemode source parser rejects a misplaced options line with its line number",
        "[codemode][source][issue1100][spec]") {
    // Frozen pi v1.3.0 (`source-options.json`, "a misplaced options line is
    // rejected with its line number"): a `// @options:` line anywhere past line
    // 1 is a typed error naming the line — including after a valid options line.
    const auto artifact = read_artifact("source-options.json");
    const auto* scenario = find_scenario(artifact, "a misplaced options line is rejected with its line number");
    REQUIRE(scenario != nullptr);
    const auto& inputs = member(member(*scenario, "inputs"), "inputs").get_array();
    const auto& expected = member(*scenario, "expected").get_array();
    REQUIRE(inputs.size() == expected.size());
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        INFO("input[" << i << "]: " << inputs[i].get_string());
        auto parsed = codemode::parse_codemode_source(inputs[i].get_string());
        CHECK_FALSE(parsed.has_value());
        if (parsed) continue;
        CHECK(parsed.error().code == support::ErrorCode::Validation);
        CHECK(parsed.error().message == member(expected[i], "message").get_string());
    }
}

TEST_CASE("the public codemode source parser rejects invalid options and empty input with typed errors",
        "[codemode][source][issue1100][spec]") {
    // Frozen pi v1.3.0 (`source-options.json`, "invalid options and empty
    // input are rejected with typed errors"): every case is a Validation error
    // with the captured message — never a silent fallback to "no options".
    const auto artifact = read_artifact("source-options.json");
    const auto* scenario = find_scenario(artifact, "invalid options and empty input are rejected with typed errors");
    REQUIRE(scenario != nullptr);
    const auto& inputs = member(member(*scenario, "inputs"), "inputs").get_array();
    const auto& expected = member(*scenario, "expected").get_array();
    REQUIRE(inputs.size() == expected.size());
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        const auto& input = member(expected[i], "input").get_string();
        INFO("input[" << i << "]: " << input);
        auto parsed = codemode::parse_codemode_source(input);
        CHECK_FALSE(parsed.has_value());
        if (parsed) continue;
        CHECK(parsed.error().code == support::ErrorCode::Validation);
        CHECK(parsed.error().message == member(member(expected[i], "error"), "message").get_string());
    }
}

TEST_CASE("the public codemode declaration preserves the relocated tool identifiers",
        "[codemode][declarations][issue1064][spec]") {
    const auto artifact = read_artifact("declarations.json");
    const auto* scenario =
            find_scenario(artifact, "tool identifiers replace invalid characters and preserve MCP names");
    REQUIRE(scenario != nullptr);

    const auto& names = member(member(*scenario, "inputs"), "names").get_array();
    const auto& expected = member(*scenario, "expected").get_array();
    REQUIRE(names.size() == expected.size());
    // The frozen identifier capture's last case (`naïve`) exercises a
    // multi-byte character: one invalid code point becomes one `_`.
    for (std::size_t i = 0; i < names.size(); ++i) {
        INFO("name: " << names[i].get_string());
        CHECK(codemode::to_codemode_identifier(names[i].get_string()) == expected[i].get_string());
    }
}

TEST_CASE("the public codemode declaration renders frozen schema types", "[codemode][declarations][issue1064][spec]") {
    const auto artifact = read_artifact("declarations.json");
    const auto* scenario =
            find_scenario(artifact, "schema primitives, literals, unions and references render as TypeScript types");
    REQUIRE(scenario != nullptr);

    const auto& schemas = member(member(*scenario, "inputs"), "schemas").get_array();
    const auto& expected = member(*scenario, "expected").get_array();
    REQUIRE(schemas.size() == expected.size());
    for (std::size_t i = 0; i < schemas.size(); ++i) {
        CHECK(codemode::schema_to_type(schemas[i], 16'000) == expected[i].get_string());
    }
}

TEST_CASE("the public codemode declaration covers the frozen schema renderings",
        "[codemode][declarations][issue1100][spec]") {
    // Every frozen declarations scenario rendered through schema_to_type: one-
    // line property sorting and quoting, doc comments, constraint tags,
    // optional-null branches, draft-04 exclusive bounds, escaped terminators,
    // tag dropping under the character budget, and $ref resolution.
    const auto artifact = read_artifact("declarations.json");
    const auto schemas_scenario = [&](std::string_view name) -> const support::JsonValue* {
        return find_scenario(artifact, name);
    };

    for (const auto name : {
                 "object properties are sorted on one line with quoted non-identifier keys",
         }) {
        const auto* scenario = schemas_scenario(name);
        REQUIRE(scenario != nullptr);
        INFO("scenario: " + std::string{name});
        const auto& schemas = member(member(*scenario, "inputs"), "schemas").get_array();
        const auto& expected = member(*scenario, "expected").get_array();
        REQUIRE(schemas.size() == expected.size());
        for (std::size_t i = 0; i < schemas.size(); ++i) {
            CHECK(codemode::schema_to_type(schemas[i], codemode::kDefaultInputSchemaMaxChars) ==
                    expected[i].get_string());
        }
    }

    // Single-schema scenarios whose expected value is the rendered object.
    for (const auto name : {
                 "property descriptions become comment lines",
                 "constraint tags render integer bounds, defaults, patterns and deprecated markers",
                 "optional non-null branches keep their constraint tags",
                 "draft-04 boolean exclusive bounds become exclusive tags",
         }) {
        const auto* scenario = schemas_scenario(name);
        REQUIRE(scenario != nullptr);
        INFO("scenario: " + std::string{name});
        const auto& schema = member(member(*scenario, "inputs"), "schema");
        CHECK(codemode::schema_to_type(schema, codemode::kDefaultInputSchemaMaxChars) ==
                member(*scenario, "expected").get_string());
    }

    {
        const auto* scenario = schemas_scenario(
                "long tags are dropped, line terminators are escaped, and line breaks stay in comments");
        REQUIRE(scenario != nullptr);
        const auto& schemas = member(member(*scenario, "inputs"), "schemas").get_array();
        const auto& expected = member(*scenario, "expected").get_array();
        REQUIRE(schemas.size() == expected.size());
        for (std::size_t i = 0; i < schemas.size(); ++i) {
            CHECK(codemode::schema_to_type(schemas[i], codemode::kDefaultInputSchemaMaxChars) ==
                    expected[i].get_string());
        }
    }

    {
        const auto* scenario = schemas_scenario("types over the character budget drop tags, then become unknown");
        REQUIRE(scenario != nullptr);
        const auto& schema = member(member(*scenario, "inputs"), "schema");
        const auto& budgets = member(member(*scenario, "inputs"), "budgets").get_array();
        const auto& expected = member(*scenario, "expected").get_array();
        REQUIRE(budgets.size() == expected.size());
        for (std::size_t i = 0; i < budgets.size(); ++i) {
            const std::size_t budget = budgets[i].holds<support::JsonValue::null_t>()
                                               ? std::size_t{0}
                                               : static_cast<std::size_t>(budgets[i].get_number());
            CHECK(codemode::schema_to_type(schema, budget) == expected[i].get_string());
        }
    }

    {
        const auto* scenario =
                schemas_scenario("local references resolve and recursive or remote references render unknown");
        REQUIRE(scenario != nullptr);
        const auto& schema = member(member(*scenario, "inputs"), "schema");
        CHECK(codemode::schema_to_type(schema, codemode::kDefaultInputSchemaMaxChars) ==
                member(*scenario, "expected").get_string());
    }
}

TEST_CASE("the public codemode declaration renders frozen tool signatures samples and blocks",
        "[codemode][declarations][issue1100][spec]") {
    const auto artifact = read_artifact("declarations.json");
    const auto* scenario =
            find_scenario(artifact, "tool signatures and samples use the output schema and input budget");
    REQUIRE(scenario != nullptr);
    const auto& captured = member(*scenario, "inputs");
    ai::Tool tool;
    tool.name = member(member(captured, "tool"), "name").get_string();
    tool.description = member(member(captured, "tool"), "description").get_string();
    tool.parameters = member(member(captured, "tool"), "inputSchema");
    tool.output_schema = member(member(captured, "tool"), "outputSchema");
    const auto& expected = member(*scenario, "expected");

    CHECK(codemode::render_tool_declaration(tool) == member(expected, "signature").get_string());
    CHECK(codemode::render_tool_sample(tool) == member(expected, "sample").get_string());
    CHECK(codemode::render_tool_declaration(tool, 10) == member(expected, "budgeted").get_string());

    // A tool without an output schema promises `unknown`.
    auto unknown = tool;
    unknown.output_schema.reset();
    const std::string unknown_declaration = codemode::render_tool_declaration(unknown);
    CHECK(unknown_declaration.ends_with(": Promise<unknown>;"));
    CHECK(member(expected, "unknownOutput").get_string() == "unknown");
    // `output` is the output schema's own rendering.
    CHECK(codemode::schema_to_type(*tool.output_schema, codemode::kDefaultInputSchemaMaxChars) ==
            "{ results: Array<string>; }");
    CHECK(codemode::schema_to_type(support::JsonValue{support::JsonValue::object_t{{"type", "object"}}},
                  codemode::kDefaultInputSchemaMaxChars) == member(expected, "output").get_string());
}

TEST_CASE("the public codemode declaration groups tools globals and namespaces with doc comments",
        "[codemode][declarations][issue1100][spec]") {
    const auto artifact = read_artifact("declarations.json");
    const auto* scenario =
            find_scenario(artifact, "renderDeclarations groups tools, globals and namespaces with doc comments");
    REQUIRE(scenario != nullptr);
    const auto& captured = member(*scenario, "inputs");
    std::vector<ai::Tool> tools;
    for (const auto& entry : member(captured, "tools").get_array()) {
        ai::Tool tool;
        tool.name = member(entry, "name").get_string();
        tool.description = member(entry, "description").get_string();
        tool.parameters = member(entry, "inputSchema");
        tool.output_schema = member(entry, "outputSchema");
        tools.push_back(std::move(tool));
    }
    std::vector<codemode::CodemodeGlobalDeclaration> globals;
    for (const auto& entry : member(captured, "globals").get_array()) {
        codemode::CodemodeGlobalDeclaration global;
        global.name = member(entry, "name").get_string();
        if (const auto found = entry.get_object().find("description"); found != entry.get_object().end()) {
            global.description = found->second.get_string();
        }
        global.signature = member(entry, "signature").get_string();
        globals.push_back(std::move(global));
    }
    CHECK(codemode::render_declarations(tools, globals) == member(*scenario, "expected").get_string());
}

TEST_CASE("the public codemode limits are the frozen public constants", "[codemode][limits][issue1100][spec]") {
    const auto artifact = read_artifact("declarations.json");
    const auto* scenario = find_scenario(artifact, "default and stored limits are public constants");
    REQUIRE(scenario != nullptr);
    const auto& expected = member(*scenario, "expected");
    CHECK(codemode::kDefaultInputSchemaMaxChars ==
            static_cast<std::size_t>(member(expected, "inputSchemaMaxChars").get_number()));
    CHECK(codemode::kMaxOutputChars == static_cast<std::size_t>(member(expected, "outputChars").get_number()));
    CHECK(codemode::kMaxOutputItems == static_cast<std::size_t>(member(expected, "outputItems").get_number()));
    CHECK(codemode::kMaxStoreTotalChars == static_cast<std::size_t>(member(expected, "storeTotalChars").get_number()));
    CHECK(codemode::kMaxStoreValueChars == static_cast<std::size_t>(member(expected, "storeValueChars").get_number()));
    CHECK(codemode::kDefaultInterruptBudget ==
            static_cast<std::size_t>(member(expected, "interruptBudget").get_number()));
}

TEST_CASE("a public codemode caller executes a script against an injected nested tool",
        "[codemode][sandbox][issue1064][spec]") {
    auto sandbox = make_sandbox();
    std::vector<std::string> calls;
    const std::vector<codemode::CodemodeToolDescriptor> tools{
            {.name = "echo", .js_name = "echo", .description = "Echo the argument."},
    };
    codemode::CodemodeToolCallHandler handler = [&calls](std::string_view name,
                                                        std::string_view arguments,
                                                        std::stop_token) {
        calls.push_back(std::format("{}:{}", name, arguments));
        return support::AsyncResult<std::string>{support::Expected<std::string>{std::string{R"({"value":"pong"})"}}};
    };

    auto result = run_async(sandbox->run("text('before');\n"
                                         "const echoed = await tools.echo({ x: 1 });\n"
                                         "text('after:' + echoed.value);\n"
                                         "return echoed;",
            tools,
            limits(),
            {},
            std::move(handler)));

    REQUIRE(result.has_value());
    CHECK_FALSE(result->error.has_value());
    REQUIRE(result->value_json.has_value());
    CHECK(*result->value_json == R"({"value":"pong"})");
    REQUIRE(result->output.size() == 2);
    CHECK(result->output[0].kind == codemode::CodemodeOutputItem::Kind::Text);
    CHECK(result->output[0].data == "before");
    CHECK(result->output[1].data == "after:pong");
    REQUIRE(calls.size() == 1);
    CHECK(calls.front() == R"(echo:{"x":1})");
}

TEST_CASE("a public codemode caller keeps prior output when an injected tool rejects",
        "[codemode][sandbox][issue1064][spec]") {
    auto sandbox = make_sandbox();
    const std::vector<codemode::CodemodeToolDescriptor> tools{
            {.name = "fail", .js_name = "fail", .description = "Fail the call."},
    };
    codemode::CodemodeToolCallHandler handler = [](std::string_view, std::string_view, std::stop_token) {
        return support::AsyncResult<std::string>{support::Expected<std::string>{
                std::unexpected(support::make_error(support::ErrorCode::Validation, "nested tool rejected the call"))}};
    };

    auto result = run_async(sandbox->run(
            "text('before');\nawait tools.fail({});\ntext('after');", tools, limits(), {}, std::move(handler)));

    REQUIRE(result.has_value());
    REQUIRE(result->error.has_value());
    CHECK(result->error->kind == codemode::CodemodeError::Kind::Script);
    CHECK(result->error->message.find("nested tool rejected the call") != std::string::npos);
    REQUIRE(result->output.size() == 1);
    CHECK(result->output[0].data == "before");
}

TEST_CASE("a public codemode caller executes discovery-returned tools with namespace-consistent names",
        "[codemode][discovery][globals][issue1091][spec]") {
    // #1091: the script discovers a tool through a spread global, then executes
    // the returned raw name through the tools surface. The raw name and its
    // normalized identifier must reach the same host call — an empty catalog
    // or a name-mangled call would not be discovery.
    auto sandbox = make_sandbox({
            {.name = "searchTools", .spread = true},
            {.name = "describeTool", .spread = true},
    });
    std::vector<std::string> calls;
    const std::vector<codemode::CodemodeToolDescriptor> tools{
            {.name = "counter.add", .js_name = "counter_add", .description = "Add two numbers."},
    };
    codemode::CodemodeToolCallHandler handler = [&calls](std::string_view name,
                                                        std::string_view arguments,
                                                        std::stop_token) {
        calls.push_back(std::format("{}:{}", name, arguments));
        if (name == "searchTools") {
            return support::AsyncResult<std::string>{support::Expected<std::string>{
                    std::string{R"([{"name":"counter.add","description":"Add two numbers."}])"}}};
        }
        if (name == "describeTool") {
            return support::AsyncResult<std::string>{support::Expected<std::string>{std::string{
                    R"({"name":"counter.add","declaration":"declare const tools: { counter_add(args: {a: number, b: number}): Promise<unknown> };"})"}}};
        }
        // Sum the arguments so the raw-name call and the normalized-name call
        // prove they each executed with their own argument.
        double sum = 0.0;
        if (auto args = support::read_json(arguments)) {
            if (const auto* fields = args->get_if<support::JsonValue::object_t>()) {
                for (const char* key : {"a", "b"}) {
                    if (const auto it = fields->find(key); it != fields->end()) {
                        if (const auto* number = it->second.get_if<double>()) sum += *number;
                    }
                }
            }
        }
        return support::AsyncResult<std::string>{
                support::Expected<std::string>{std::format("{{\"value\":{:.0f}}}", sum)}};
    };

    auto result = run_async(sandbox->run("const found = await searchTools('add');\n"
                                         "const target = found[0].name;\n"
                                         "const described = await describeTool(target);\n"
                                         "const viaRaw = await tools[target]({ a: 2, b: 3 });\n"
                                         "const viaJs = await tools.counter_add({ a: 1, b: 1 });\n"
                                         "text(described.declaration);\n"
                                         "return [target, viaRaw.value, viaJs.value];",
            tools,
            limits(),
            {},
            std::move(handler)));

    REQUIRE(result.has_value());
    CHECK_FALSE(result->error.has_value());
    REQUIRE(result->value_json.has_value());
    CHECK(*result->value_json == R"(["counter.add",5,2])");
    REQUIRE(result->output.size() == 1);
    CHECK(result->output[0].data ==
            "declare const tools: { counter_add(args: {a: number, b: number}): Promise<unknown> };");
    // namespace/name consistency: discovery, the raw-name call, and the
    // normalized-identifier call all observe the registered name.
    REQUIRE(calls.size() == 4);
    CHECK(calls[0] == R"(searchTools:["add"])");
    CHECK(calls[1] == R"(describeTool:["counter.add"])");
    CHECK(calls[2] == R"(counter.add:{"a":2,"b":3})");
    CHECK(calls[3] == R"(counter.add:{"a":1,"b":1})");
}

TEST_CASE("a public codemode caller cannot execute a discovery name the host does not provide",
        "[codemode][discovery][issue1091][spec]") {
    // Acceptance discriminating case (#1091 "an empty catalog does not count"):
    // discovery must reflect the actually-callable set. A catalog name the
    // host did not register must not be executable, so a discovery surface
    // that advertises tools the callable set lacks is observable to the script.
    auto sandbox = make_sandbox({
            {.name = "searchTools", .spread = true},
    });
    const std::vector<codemode::CodemodeToolDescriptor> tools{
            {.name = "counter.add", .js_name = "counter_add", .description = "Add two numbers."},
    };
    codemode::CodemodeToolCallHandler handler = [](std::string_view name, std::string_view, std::stop_token) {
        if (name == "searchTools") {
            // The double claims a tool the host never registered.
            return support::AsyncResult<std::string>{support::Expected<std::string>{
                    std::string{R"([{"name":"ghost.tool","description":"Not registered."}])"}}};
        }
        return support::AsyncResult<std::string>{support::Expected<std::string>{std::string{"null"}}};
    };

    auto result = run_async(sandbox->run("const found = await searchTools('ghost');\n"
                                         "try {\n"
                                         "  await tools[found[0].name]({});\n"
                                         "  return 'executed';\n"
                                         "} catch (error) {\n"
                                         "  return [error.name, error.message];\n"
                                         "}",
            tools,
            limits(),
            {},
            std::move(handler)));

    REQUIRE(result.has_value());
    CHECK_FALSE(result->error.has_value());
    REQUIRE(result->value_json.has_value());
    CHECK(*result->value_json ==
            R"(["TypeError","tools.ghost.tool does not exist. Available: counter_add. ALL_TOOLS lists every tool; searchTools(query) finds tools by topic. Check for a member with \"ghost.tool\" in tools."])");
}

TEST_CASE("a public codemode caller collects mixed output in order with the console marker and image MIME",
        "[codemode][output][issue1092][spec]") {
    // Frozen pi v1.3.0 (`fixtures/pi-v1.3.0/audit-1053/script-execution.json`,
    // "a script returns a JSON value after top-level await and collects output
    // in order"): `text()` and `console.*` both produce `type: "text"` items,
    // console items carry `console: true`, and `image()` passes the base64 data
    // and detected MIME through. The data URI is the same 1x1 PNG pi used.
    auto sandbox = make_sandbox();

    auto result = run_async(sandbox->run("text('plain'); console.log('log', 2); console.error('err'); "
                                         "image('data:image/png;base64,"
                                         "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAFgwJ/"
                                         "lYQ5WQAAAABJRU5ErkJggg=='); "
                                         "return { a: 1 }",
            {},
            limits(),
            {},
            nullptr));

    REQUIRE(result.has_value());
    CHECK_FALSE(result->error.has_value());
    REQUIRE(result->value_json.has_value());
    CHECK(*result->value_json == R"({"a":1})");
    const auto& output = result->output;
    REQUIRE(output.size() == 4);
    CHECK(output[0].kind == codemode::CodemodeOutputItem::Kind::Text);
    CHECK(output[0].data == "plain");
    CHECK_FALSE(output[0].console);
    CHECK(output[1].kind == codemode::CodemodeOutputItem::Kind::Text);
    CHECK(output[1].data == "log 2");
    CHECK(output[1].console);
    CHECK(output[2].data == "err");
    CHECK(output[2].console);
    CHECK(output[3].kind == codemode::CodemodeOutputItem::Kind::Image);
    CHECK(output[3].data ==
            "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAFgwJ/lYQ5WQAAAABJRU5ErkJggg==");
    CHECK(output[3].mime_type == "image/png");
    CHECK_FALSE(output[3].console);
}

TEST_CASE("a public codemode caller reports a script error with its name message stack and prior output",
        "[codemode][errors][issue1092][spec]") {
    // Frozen pi v1.3.0 (`script-execution.json`, "a script failure keeps prior
    // output and reports the script error with its line"): kind `script`, the
    // error's name/message, the guest stack with the script's line, and the
    // output emitted before the throw.
    auto sandbox = make_sandbox();

    auto result = run_async(sandbox->run("text(\"before\");\nthrow new Error(\"boom\")", {}, limits(), {}, nullptr));

    REQUIRE(result.has_value());
    REQUIRE(result->error.has_value());
    CHECK(result->error->kind == codemode::CodemodeError::Kind::Script);
    CHECK(result->error->name == "Error");
    CHECK(result->error->message == "boom");
    REQUIRE(result->error->stack.has_value());
    CHECK(*result->error->stack == "Error: boom\n    at <anonymous> (codemode.js:2:11)");
    REQUIRE(result->output.size() == 1);
    CHECK(result->output[0].data == "before");
    CHECK_FALSE(result->value_json.has_value());
    CHECK(result->calls.empty());
    CHECK_FALSE(result->store_writes.has_value());
}

TEST_CASE("a public codemode caller reports non-Error throws BigInt returns and syntax errors as script errors",
        "[codemode][errors][issue1092][spec]") {
    // Frozen pi v1.3.0 (`script-execution.json`, "non-Error throws, syntax
    // errors and non-serializable returns are script errors"): a non-Error
    // throw carries only `message`; a non-serializable return surfaces the
    // guest stringify TypeError with its stack; a syntax error surfaces with
    // the guest's line report.
    auto sandbox = make_sandbox();

    auto plain = run_async(sandbox->run("throw \"plain\"", {}, limits(), {}, nullptr));
    REQUIRE(plain.has_value());
    REQUIRE(plain->error.has_value());
    CHECK(plain->error->kind == codemode::CodemodeError::Kind::Script);
    CHECK(plain->error->message == "plain");
    CHECK_FALSE(plain->error->name.has_value());
    CHECK_FALSE(plain->error->stack.has_value());
    CHECK(plain->output.empty());

    auto bigint = run_async(sandbox->run("return 1n", {}, limits(), {}, nullptr));
    REQUIRE(bigint.has_value());
    REQUIRE(bigint->error.has_value());
    CHECK(bigint->error->name == "TypeError");
    CHECK(bigint->error->message == "BigInt are forbidden in JSON.stringify");
    REQUIRE(bigint->error->stack.has_value());
    CHECK(bigint->error->stack->starts_with("TypeError: BigInt are forbidden in JSON.stringify"));

    auto syntax = run_async(sandbox->run("text(", {}, limits(), {}, nullptr));
    REQUIRE(syntax.has_value());
    REQUIRE(syntax->error.has_value());
    CHECK(syntax->error->name == "SyntaxError");
    CHECK(syntax->error->message == "unexpected token in expression: '}'");
    REQUIRE(syntax->error->stack.has_value());
    CHECK(syntax->error->stack->starts_with("SyntaxError: unexpected token in expression: '}'"));
}

TEST_CASE("a public codemode caller treats exit() as success with prior output and the write set",
        "[codemode][output][issue1092][spec]") {
    // Frozen pi v1.3.0 (`script-execution.json`, "exit() succeeds with prior
    // output and store writes and stops later output"): `exit()` ends the run
    // as success, keeps the output emitted before it, stops later output, and
    // reports the pending store writes.
    auto sandbox = make_sandbox();

    auto result = run_async(sandbox->run(
            "text(\"before\"); store(\"a\", {x: 1}); store(\"b\", 2); store(\"b\", undefined); exit(); text(\"after\")",
            {},
            limits(),
            {},
            nullptr));

    REQUIRE(result.has_value());
    CHECK_FALSE(result->error.has_value());
    CHECK_FALSE(result->value_json.has_value());
    REQUIRE(result->output.size() == 1);
    CHECK(result->output[0].data == "before");
    REQUIRE(result->store_writes.has_value());
    REQUIRE(result->store_writes->set.size() == 1);
    const auto* a_value = result->store_writes->set.at("a").get_if<support::JsonValue::object_t>();
    REQUIRE(a_value != nullptr);
    const auto x = a_value->find("x");
    REQUIRE(x != a_value->end());
    const auto* x_value = x->second.get_if<double>();
    REQUIRE(x_value != nullptr);
    CHECK(*x_value == 1.0);
    REQUIRE(result->store_writes->deleted.size() == 1);
    CHECK(result->store_writes->deleted[0] == "b");
    CHECK(result->calls.empty());
}

TEST_CASE("a public codemode caller reports guest memory exhaustion as an out-of-memory script error",
        "[codemode][limits][issue1092][spec]") {
    // Frozen pi v1.3.0 (`script-execution.json`, "a memory-limited VM reports
    // out of memory as a script error", upstream portable.test.ts "caps the
    // interrupt budget and memory at the remote's limits"): with the guest
    // memory capped, an allocation past the cap fails the run as a script
    // error named InternalError with message "out of memory".
    auto sandbox = make_sandbox();
    auto memory_limits = limits();
    memory_limits.memory_limit_bytes = 8u * 1024u * 1024u;

    // The allocation pattern is the frozen upstream case (pi
    // packages/codemode/test/portable.test.ts:93-105): each iteration builds a
    // unique 1 MiB string, so QuickJS cannot intern or rope its way out of
    // actually allocating; the 8 MiB cap exhausts within a handful of rounds.
    auto result =
            run_async(sandbox->run("const parts = [];\nfor (;;) parts.push(\"x\".repeat(1 << 20) + parts.length);",
                    {},
                    memory_limits,
                    {},
                    nullptr));

    REQUIRE(result.has_value());
    REQUIRE(result->error.has_value());
    CHECK(result->error->kind == codemode::CodemodeError::Kind::Script);
    CHECK(result->error->name == "InternalError");
    CHECK(result->error->message == "out of memory");
    CHECK(result->output.empty());
    CHECK(result->calls.empty());
}

TEST_CASE("a public codemode caller fails the run when the guest hits its output item limit",
        "[codemode][limits][issue1092][spec]") {
    // The frozen prelude enforces pi's output limits (16 MiB of characters or
    // 100000 text/image/console items) guest-side: past the limit the script
    // fails with the frozen RangeError, and the host reports it as a script
    // error. One over-limit text() is the cheap discriminating case; the
    // 100000-item variant differs only in which clause trips first.
    auto sandbox = make_sandbox();

    auto result =
            run_async(sandbox->run("text('x'.repeat(16777217)); return 'unreachable';", {}, limits(), {}, nullptr));

    REQUIRE(result.has_value());
    REQUIRE(result->error.has_value());
    CHECK(result->error->kind == codemode::CodemodeError::Kind::Script);
    CHECK(result->error->name == "RangeError");
    CHECK(result->error->message ==
            "script output exceeded the limit of 16777216 characters or 100000 text(), image(), and console calls. "
            "Print a summary instead, or write large data to a file with a tool.");
    CHECK_FALSE(result->value_json.has_value());
}

TEST_CASE("a public codemode caller fails the run when the guest hits its store value limit",
        "[codemode][limits][issue1092][spec]") {
    // The frozen prelude enforces pi's store limits guest-side (262144 JSON
    // characters per value, 1048576 total): past a limit the script fails with
    // the frozen RangeError and the host reports it as a script error.
    auto sandbox = make_sandbox();

    auto result = run_async(
            sandbox->run("store('big', 'x'.repeat(262145)); return 'unreachable';", {}, limits(), {}, nullptr));

    REQUIRE(result.has_value());
    REQUIRE(result->error.has_value());
    CHECK(result->error->kind == codemode::CodemodeError::Kind::Script);
    CHECK(result->error->name == "RangeError");
    CHECK(result->error->message.starts_with(
            "store(\"big\") value has 262147 characters of JSON, more than the limit of 262144."));
    CHECK_FALSE(result->value_json.has_value());
    CHECK_FALSE(result->store_writes.has_value());
}

TEST_CASE("a public codemode caller returns its value and text output from a successful budgeted run",
        "[codemode][inline][issue1097][spec]") {
    // Frozen pi v1.3.0 (`inline-interrupt.json`, "a successful inline execution
    // returns its value and text output"): a budgeted execution that finishes
    // reports ok with the value, the text output, no calls, and a present (but
    // empty) write set.
    auto sandbox = make_sandbox();
    auto budget_limits = limits();
    budget_limits.interrupt_budget = codemode::kDefaultInterruptBudget;

    auto result = run_async(sandbox->run("text('inline'); return 7", {}, budget_limits, {}, nullptr));

    REQUIRE(result.has_value());
    CHECK_FALSE(result->error.has_value());
    REQUIRE(result->value_json.has_value());
    CHECK(*result->value_json == "7");
    REQUIRE(result->output.size() == 1);
    CHECK(result->output[0].data == "inline");
    CHECK(result->calls.empty());
    REQUIRE(result->store_writes.has_value());
    CHECK(result->store_writes->set.empty());
    CHECK(result->store_writes->deleted.empty());
}

TEST_CASE("a public codemode caller stops pure computation with the interrupt budget, not the deadline",
        "[codemode][inline][issue1097][spec]") {
    // Frozen pi v1.3.0 (`inline-interrupt.json`, "an inline script that
    // computes without awaiting is stopped by its interrupt budget, not the
    // deadline"): both a tight loop and a tight await-loop exceed the budget
    // (polls counted across the whole execution) and fail as a timeout with
    // the frozen budget message — even with an earlier deadline pending.
    //
    // The budget is scaled to this harness's pinned guest (#874 quickjs.wasi
    // 3.2): it polls its interrupt handler about two orders of magnitude less
    // often than pi v1.3.0's guest, so frozen pi's 1000-poll capture would take
    // over 20 s here. The frozen contract under test is that polls are counted
    // across the whole execution and reported with this exact message shape.
    auto sandbox = make_sandbox();
    auto budget_limits = limits();
    budget_limits.timeout = std::chrono::seconds{30};
    budget_limits.interrupt_budget = 20;

    for (const char* script : {"while (true) {}", "while (true) await null"}) {
        auto result = run_async(sandbox->run(script, {}, budget_limits, {}, nullptr));
        REQUIRE(result.has_value());
        REQUIRE(result->error.has_value());
        CHECK(result->error->kind == codemode::CodemodeError::Kind::Timeout);
        CHECK(result->error->message == "Execution exceeded its interrupt budget of 20");
    }
}

TEST_CASE("a public codemode caller counts the interrupt budget across tool waits",
        "[codemode][inline][issue1097][spec]") {
    // Frozen pi v1.3.0 (`inline-interrupt.json`, "the interrupt budget counts
    // across awaits on tools"): a loop that alternates compute and awaited
    // tool calls keeps spending the same budget; the run fails with the
    // budget message after the calls that fit. (The frozen capture's exact
    // call count depends on pi's poll rate; the contract here is the error
    // plus completed settled calls.)
    auto sandbox = make_sandbox();
    auto budget_limits = limits();
    budget_limits.timeout = std::chrono::seconds{30};
    budget_limits.interrupt_budget = 20;
    const std::vector<codemode::CodemodeToolDescriptor> tools{
            {.name = "tick", .js_name = "tick", .description = "Count one tick."},
    };
    codemode::CodemodeToolCallHandler handler = [](std::string_view, std::string_view, std::stop_token) {
        return support::AsyncResult<std::string>{support::Expected<std::string>{std::string{"null"}}};
    };

    auto result =
            run_async(sandbox->run("let n = 0;\nfor (;;) { for (let i = 0; i < 100; i++) n += i; await tools.tick(); }",
                    tools,
                    budget_limits,
                    {},
                    std::move(handler)));

    REQUIRE(result.has_value());
    REQUIRE(result->error.has_value());
    CHECK(result->error->kind == codemode::CodemodeError::Kind::Timeout);
    CHECK(result->error->message == "Execution exceeded its interrupt budget of 20");
    CHECK(result->calls.size() >= 2);
    // Every settled call is `ok`; the call in flight when the budget ran out is
    // reported cancelled by the finish sweep.
    const auto settled_calls = result->calls.size() - 1;
    CHECK(std::ranges::all_of(result->calls.begin(),
            result->calls.begin() + static_cast<std::ptrdiff_t>(settled_calls),
            [](const codemode::CodemodeCallRecord& call) {
                return call.status == codemode::CodemodeCallRecord::Status::Ok;
            }));
}

TEST_CASE("a public codemode caller reports the deadline when a tool waits past it and keeps partial output",
        "[codemode][inline][issue1097][spec]") {
    // Frozen pi v1.3.0 (`inline-interrupt.json`, "an inline deadline while a
    // tool waits reports the deadline, not the interrupt budget"): the run
    // fails with the deadline message, the output emitted before the call is
    // kept, and the in-flight call is cancelled with a duration.
    auto sandbox = make_sandbox();
    auto deadline_limits = limits();
    deadline_limits.timeout = std::chrono::milliseconds{40};
    const std::vector<codemode::CodemodeToolDescriptor> tools{
            {.name = "hang", .js_name = "hang", .description = "Hang until cancelled."},
    };
    std::atomic<bool> tool_aborted{false};
    codemode::CodemodeToolCallHandler handler = [&tool_aborted](std::string_view,
                                                        std::string_view,
                                                        std::stop_token signal) -> support::AsyncResult<std::string> {
        return support::detail::make_async_result(
                [&tool_aborted, signal]() -> boost::asio::awaitable<support::Expected<std::string>> {
                    auto executor = co_await boost::asio::this_coro::executor;
                    boost::asio::steady_timer timer{executor};
                    while (!signal.stop_requested()) {
                        timer.expires_after(std::chrono::milliseconds{5});
                        co_await timer.async_wait(boost::asio::use_awaitable);
                    }
                    tool_aborted.store(true);
                    co_return std::unexpected(support::make_error(support::ErrorCode::Cancelled, "aborted"));
                });
    };

    auto result = run_async(sandbox->run("text('partial'); await tools.hang(); return 'unreachable';",
            tools,
            deadline_limits,
            {},
            std::move(handler)));

    REQUIRE(result.has_value());
    REQUIRE(result->error.has_value());
    CHECK(result->error->kind == codemode::CodemodeError::Kind::Timeout);
    CHECK(result->error->message == "Execution timed out after 40 ms");
    REQUIRE(result->output.size() == 1);
    CHECK(result->output[0].data == "partial");
    REQUIRE(result->calls.size() == 1);
    CHECK(result->calls[0].status == codemode::CodemodeCallRecord::Status::Cancelled);
    CHECK(result->calls[0].duration_ms.has_value());
    CHECK(tool_aborted.load());
}

TEST_CASE("a public codemode caller aborting while a tool waits keeps partial output and cancels the call",
        "[codemode][inline][issue1097][spec]") {
    // Frozen pi v1.3.0 (`inline-interrupt.json`, "caller abort during an
    // inline tool wait aborts the tool and keeps partial output"): the run
    // settles aborted with the reason's message, the output before the call is
    // kept, and the in-flight call is cancelled with a duration.
    auto sandbox = make_sandbox();
    const std::vector<codemode::CodemodeToolDescriptor> tools{
            {.name = "hang", .js_name = "hang", .description = "Hang until cancelled."},
    };
    std::atomic<bool> tool_aborted{false};
    std::stop_source cancel;
    codemode::CodemodeToolCallHandler handler = [&tool_aborted, &cancel](std::string_view,
                                                        std::string_view,
                                                        std::stop_token signal) -> support::AsyncResult<std::string> {
        return support::detail::make_async_result(
                [&tool_aborted, &cancel, signal]() -> boost::asio::awaitable<support::Expected<std::string>> {
                    // The caller aborts while the tool is in flight.
                    cancel.request_stop();
                    auto executor = co_await boost::asio::this_coro::executor;
                    boost::asio::steady_timer timer{executor};
                    while (!signal.stop_requested()) {
                        timer.expires_after(std::chrono::milliseconds{5});
                        co_await timer.async_wait(boost::asio::use_awaitable);
                    }
                    tool_aborted.store(true);
                    co_return std::unexpected(support::make_error(support::ErrorCode::Cancelled, "aborted"));
                });
    };

    auto result = run_async(sandbox->run("text('partial'); await tools.hang(); return 'unreachable';",
            tools,
            limits(),
            cancel.get_token(),
            std::move(handler),
            {},
            std::string{"cancelled"}));

    REQUIRE(result.has_value());
    REQUIRE(result->error.has_value());
    CHECK(result->error->kind == codemode::CodemodeError::Kind::Aborted);
    CHECK(result->error->message == "cancelled");
    REQUIRE(result->output.size() == 1);
    CHECK(result->output[0].data == "partial");
    REQUIRE(result->calls.size() == 1);
    CHECK(result->calls[0].status == codemode::CodemodeCallRecord::Status::Cancelled);
    CHECK(result->calls[0].duration_ms.has_value());
    CHECK(tool_aborted.load());
    CHECK_FALSE(result->value_json.has_value());
}

TEST_CASE("a public codemode caller reports a pre-aborted run with the abort reason and never starts it",
        "[codemode][inline][issue1097][spec]") {
    // Frozen pi v1.3.0 (`inline-interrupt.json`, "an inline execution with an
    // already-aborted signal never creates a VM"): the run settles as aborted
    // with the reason's message, with no output and no calls.
    auto sandbox = make_sandbox();
    std::stop_source cancel;
    cancel.request_stop();

    auto result = run_async(sandbox->run(
            "text('unreachable'); return 1", {}, limits(), cancel.get_token(), nullptr, {}, std::string{"early"}));

    REQUIRE(result.has_value());
    REQUIRE(result->error.has_value());
    CHECK(result->error->kind == codemode::CodemodeError::Kind::Aborted);
    CHECK(result->error->message == "early");
    CHECK(result->output.empty());
    CHECK(result->calls.empty());
    CHECK_FALSE(result->value_json.has_value());
}

TEST_CASE("a public codemode caller reads a store snapshot across calls and drops failed writes",
        "[codemode][store][issue1091][spec]") {
    auto sandbox = make_sandbox();
    std::map<std::string, support::JsonValue> snapshot;
    snapshot.emplace("seed", support::JsonValue{7});
    snapshot.emplace("stale", support::JsonValue{1});

    auto first = run_async(sandbox->run("store(\"counter\", 41);\n"
                                        "store(\"stale\", undefined);\n"
                                        "return load(\"seed\");",
            {},
            limits(),
            {},
            {},
            snapshot));

    REQUIRE(first.has_value());
    CHECK_FALSE(first->error.has_value());
    REQUIRE(first->value_json.has_value());
    CHECK(*first->value_json == "7");
    // Frozen rule: a successful run reports exactly its writes — one update
    // and one delete; the untouched snapshot key is not a write.
    REQUIRE(first->store_writes.has_value());
    REQUIRE(first->store_writes->set.size() == 1);
    const auto counter = first->store_writes->set.find("counter");
    REQUIRE(counter != first->store_writes->set.end());
    const auto* counter_value = counter->second.get_if<double>();
    REQUIRE(counter_value != nullptr);
    CHECK(*counter_value == 41.0);
    CHECK(first->store_writes->deleted == std::vector<std::string>{"stale"});

    // Cross-call snapshot: the caller merges the reported writes into the
    // next run's snapshot, so load() sees the update and the delete.
    for (const auto& key : first->store_writes->deleted)
        snapshot.erase(key);
    for (auto& [key, value] : first->store_writes->set)
        snapshot[key] = value;

    auto second = run_async(
            sandbox->run("return [load(\"counter\"), load(\"stale\")];", {}, limits(), {}, {}, std::move(snapshot)));

    REQUIRE(second.has_value());
    CHECK_FALSE(second->error.has_value());
    REQUIRE(second->value_json.has_value());
    CHECK(*second->value_json == "[41,null]");

    // Frozen rule: only successful executions report writes.
    auto failed = run_async(sandbox->run("store(\"lost\", 1);\nthrow new Error(\"x\");", {}, limits()));
    REQUIRE(failed.has_value());
    REQUIRE(failed->error.has_value());
    CHECK(failed->error->kind == codemode::CodemodeError::Kind::Script);
    CHECK_FALSE(failed->store_writes.has_value());
}

TEST_CASE("a public codemode caller records an unawaited call as cancelled and aborts it",
        "[codemode][calls][issue1091][spec]") {
    // #1091 frozen rule: a call the script never awaited is aborted when the
    // script returns, and its record is cancelled with a duration.
    auto sandbox = make_sandbox();
    bool tool_aborted = false;
    const std::vector<codemode::CodemodeToolDescriptor> tools{
            {.name = "hang", .js_name = "hang", .description = "Hang until cancelled."},
    };
    codemode::CodemodeToolCallHandler handler = [&tool_aborted](
                                                        std::string_view, std::string_view, std::stop_token signal) {
        return support::detail::make_async_result(
                [&tool_aborted, signal]() mutable -> boost::asio::awaitable<support::Expected<std::string>> {
                    auto executor = co_await boost::asio::this_coro::executor;
                    boost::asio::steady_timer timer(executor);
                    while (!signal.stop_requested()) {
                        timer.expires_after(std::chrono::milliseconds{5});
                        const auto [error] =
                                co_await timer.async_wait(boost::asio::as_tuple(boost::asio::use_awaitable));
                        (void)error;
                    }
                    tool_aborted = true;
                    co_return std::unexpected(support::make_error(support::ErrorCode::Validation, "tool aborted"));
                });
    };

    auto result = run_async(sandbox->run("tools.hang();\nreturn \"done\";", tools, limits(), {}, std::move(handler)));

    REQUIRE(result.has_value());
    CHECK_FALSE(result->error.has_value());
    REQUIRE(result->value_json.has_value());
    CHECK(*result->value_json == "\"done\"");
    REQUIRE(result->calls.size() == 1);
    CHECK(result->calls[0].name == "hang");
    CHECK(result->calls[0].status == codemode::CodemodeCallRecord::Status::Cancelled);
    CHECK(result->calls[0].duration_ms.has_value());
    // The frozen scenario asserts the host call observed the abort.
    CHECK(tool_aborted);
}

TEST_CASE("a public codemode caller records settled calls with ok and error statuses",
        "[codemode][calls][issue1091][spec]") {
    auto sandbox = make_sandbox();
    const std::vector<codemode::CodemodeToolDescriptor> tools{
            {.name = "ok", .js_name = "ok", .description = "Succeed."},
            {.name = "fail", .js_name = "fail", .description = "Reject the call."},
    };
    codemode::CodemodeToolCallHandler handler = [](std::string_view name, std::string_view, std::stop_token) {
        if (name == "ok") {
            return support::AsyncResult<std::string>{support::Expected<std::string>{std::string{"1"}}};
        }
        return support::AsyncResult<std::string>{support::Expected<std::string>{
                std::unexpected(support::make_error(support::ErrorCode::Validation, "boom"))}};
    };

    auto result = run_async(sandbox->run("const one = await tools.ok();\n"
                                         "let caught = \"\";\n"
                                         "try { await tools.fail(); } catch (error) { caught = error.message; }\n"
                                         "return [one, caught];",
            tools,
            limits(),
            {},
            std::move(handler)));

    REQUIRE(result.has_value());
    CHECK_FALSE(result->error.has_value());
    REQUIRE(result->value_json.has_value());
    CHECK(*result->value_json == R"([1,"boom"])");
    REQUIRE(result->calls.size() == 2);
    CHECK(result->calls[0].name == "ok");
    CHECK(result->calls[0].status == codemode::CodemodeCallRecord::Status::Ok);
    CHECK(result->calls[0].duration_ms.has_value());
    CHECK(result->calls[1].name == "fail");
    CHECK(result->calls[1].status == codemode::CodemodeCallRecord::Status::Error);
    CHECK(result->calls[1].duration_ms.has_value());
}

TEST_CASE("a public codemode caller binds a dotted global into its frozen namespace and spreads arguments",
        "[codemode][globals][issue1091][spec]") {
    // pi `globalsJson`: a dotted global binds as a member of a frozen namespace
    // object; with `spread` the guest passes the whole argument array instead of
    // the first argument.
    auto sandbox = make_sandbox({
            {.name = "models.classify", .spread = true},
    });
    std::atomic<bool> called{false};
    codemode::CodemodeToolCallHandler handler = [&called](std::string_view name,
                                                        std::string_view arguments,
                                                        std::stop_token) {
        called.store(true);
        CHECK(name == "models.classify");
        CHECK(arguments == R"([{"input":"x"}])");
        return support::AsyncResult<std::string>{support::Expected<std::string>{std::string{R"("classified")"}}};
    };

    auto result = run_async(
            sandbox->run("return await models.classify({ input: 'x' });", {}, limits(), {}, std::move(handler)));

    REQUIRE(result.has_value());
    CHECK_FALSE(result->error.has_value());
    CHECK(called.load());
    REQUIRE(result->value_json.has_value());
    CHECK(*result->value_json == R"("classified")");
}

TEST_CASE("a public codemode caller rejects invalid globals at construction with the frozen errors",
        "[codemode][globals][issue1091][spec]") {
    // #1091 frozen observations (audit-1053 script-execution.json): identifier
    // shape, one reserved name, duplicates, and namespace conflicts are
    // construction errors with these exact messages.
    const std::pair<std::vector<codemode::CodemodeGlobal>, std::string> cases[] = {
            {{codemode::CodemodeGlobal{.name = "tools"}}, "Invalid global name \"tools\""},
            {{codemode::CodemodeGlobal{.name = "a-b"}}, "Invalid global name \"a-b\""},
            {{codemode::CodemodeGlobal{.name = "x"}, codemode::CodemodeGlobal{.name = "x"}},
                    "Global \"x\" is already registered"},
            {{codemode::CodemodeGlobal{.name = "ns"}, codemode::CodemodeGlobal{.name = "ns.m"}},
                    "Global \"ns\" conflicts with the namespace \"ns\""},
    };
    for (const auto& [globals, expected] : cases) {
        auto created = codemode::CodemodeSandbox::create(codemode::default_codemode_guest_wasm_path(), globals);
        CHECK_FALSE(created.has_value());
        if (!created) {
            CHECK(created.error().message == expected);
        }
    }
}

TEST_CASE("an aborted public codemode caller does not start the script", "[codemode][sandbox][issue1064][spec]") {
    auto sandbox = make_sandbox();
    std::stop_source stop;
    stop.request_stop();

    auto result = run_async(sandbox->run("text('should not run'); return 1;", {}, limits(), stop.get_token()));

    REQUIRE(result.has_value());
    REQUIRE(result->error.has_value());
    CHECK(result->error->kind == codemode::CodemodeError::Kind::Aborted);
    CHECK(result->output.empty());
}
