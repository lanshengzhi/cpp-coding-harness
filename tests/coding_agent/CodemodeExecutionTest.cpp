// Ticket #1119 (spec #1044, ADR 0075): the ordinary application codemode
// execution wrapper — output presentation, budget/spill, images, nested-call
// records and the persistent store — compared against the frozen pi-v1.3.0
// application capture (`fixtures/pi-v1.3.0/audit-1119/execution.json`).
//
// Acceptance discipline: the cheap stand-ins are "the result has the right
// number of blocks" (passes while the numbering, console grouping or error
// summary is wrong) and "the store value changed" (passes while the entry was
// never persisted, or a failed run's writes leaked through). Each case below
// compares the whole normalized observation, and the store/image cases also
// inspect the durable side effect the content alone would not prove.

#include "coding_agent/extensions/codemode/CodemodeExecution.hpp"
#include "coding_agent/extensions/codemode/CodemodeToolSource.hpp"

#include "coding_agent/Base64.hpp"
#include <cch/coding_agent/environment/LocalFileSystem.hpp>
#include "support/AsyncResultBridge.hpp"
#include "support/Json.hpp"
#include "support/JsonCompare.hpp"
#include "support/StreamAdapterFixture.hpp"
#include "support/RuntimeFixture.hpp"
#include "support/TempWorkspace.hpp"

#include <cch/agent/AgentTool.hpp>
#include <cch/agent/NestedToolCalls.hpp>
#include <cch/ai/Content.hpp>
#include <cch/coding_agent/environment/FileSystem.hpp>
#include <cch/coding_agent/session/SessionEntry.hpp>
#include <cch/coding_agent/session/SessionStore.hpp>
#include <cch/codemode/Sandbox.hpp>
#include <cch/support/JsonValue.hpp>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <regex>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using namespace cch;

namespace {

namespace extensions = cch::coding_agent::extensions;

[[nodiscard]] std::filesystem::path artifact_path() {
    return std::filesystem::path{CCH_SOURCE_DIR} / "fixtures" / "pi-v1.3.0" / "audit-1119" / "execution.json";
}

[[nodiscard]] std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

[[nodiscard]] support::JsonValue read_artifact() {
    auto parsed = support::read_json(read_text(artifact_path()));
    REQUIRE(parsed.has_value());
    return std::move(*parsed);
}

[[nodiscard]] const support::JsonValue& scenario(const support::JsonValue& artifact, std::string_view name) {
    for (const auto& candidate : artifact.get_object().at("scenarios").get_array()) {
        if (candidate.get_object().at("name").get_string() == name) return candidate;
    }
    REQUIRE(false);
    return artifact;
}

/// One nested tool the fake dispatcher serves. `run` receives the call
/// argument and the sandbox's per-call stop token.
struct FakeTool {
    std::string name;
    std::function<support::AsyncResult<agent::AsyncToolExecutionResult>(support::JsonValue, std::stop_token)> run;
};

[[nodiscard]] support::AsyncResult<agent::AsyncToolExecutionResult> ready_result(
        agent::AsyncToolExecutionResult result) {
    return support::AsyncResult<agent::AsyncToolExecutionResult>{
            support::Expected<agent::AsyncToolExecutionResult>{std::move(result)}};
}

[[nodiscard]] FakeTool text_tool(std::string name, std::string text) {
    return FakeTool{
            .name = std::move(name),
            .run =
                    [text = std::move(text)](support::JsonValue, std::stop_token) {
                        agent::AsyncToolExecutionResult result;
                        result.content.push_back(ai::text_content(text));
                        return ready_result(std::move(result));
                    },
    };
}

/// A tool that never settles until the sandbox cancels it (pi's `sleep 30`
/// under a 100 ms script deadline).
[[nodiscard]] FakeTool waiting_tool(std::string name) {
    return FakeTool{
            .name = std::move(name),
            .run =
                    [](support::JsonValue, std::stop_token signal) {
                        return support::detail::make_async_result(
                                [signal]()
                                        -> boost::asio::awaitable<support::Expected<agent::AsyncToolExecutionResult>> {
                                    auto executor = co_await boost::asio::this_coro::executor;
                                    boost::asio::steady_timer timer{executor};
                                    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
                                    while (!signal.stop_requested() && std::chrono::steady_clock::now() < deadline) {
                                        timer.expires_after(std::chrono::milliseconds{2});
                                        const auto [error] = co_await timer.async_wait(
                                                boost::asio::as_tuple(boost::asio::use_awaitable));
                                        (void)error;
                                    }
                                    agent::AsyncToolExecutionResult result;
                                    result.is_error = true;
                                    result.content.push_back(ai::text_content("Execution aborted"));
                                    co_return support::Expected<agent::AsyncToolExecutionResult>{std::move(result)};
                                });
                    },
    };
}

/// A dispatcher over the fake tools, shaped like the Agent's nested-call seam.
[[nodiscard]] agent::NestedToolCallRunner make_runner(std::vector<FakeTool> tools) {
    auto shared = std::make_shared<std::vector<FakeTool>>(std::move(tools));
    agent::NestedToolCallHost host;
    host.get_tools = [shared]() {
        std::vector<ai::Tool> definitions;
        for (const auto& tool : *shared) {
            definitions.push_back(ai::Tool{
                    .name = tool.name,
                    .description = "fake nested tool",
                    .parameters = support::JsonValue{support::JsonValue::object_t{}},
            });
        }
        return definitions;
    };
    host.is_sequential = []() { return false; };
    host.run_tool_call = [shared](agent::ToolInvocation invocation,
                                 std::string,
                                 std::stop_token signal) -> support::AsyncResult<agent::NestedToolCallOutcome> {
        for (const auto& tool : *shared) {
            if (tool.name != invocation.name) continue;
            return support::detail::make_async_result(
                    [run = tool.run, arguments = std::move(invocation.arguments), signal]() mutable
                            -> boost::asio::awaitable<support::Expected<agent::NestedToolCallOutcome>> {
                        agent::NestedToolCallOutcome outcome;
                        auto executed = co_await support::detail::await_async_result(run(std::move(arguments), signal));
                        if (executed) {
                            outcome.result = std::move(*executed);
                        } else {
                            outcome.result.is_error = true;
                            outcome.result.content.push_back(ai::text_content(executed.error().message));
                        }
                        co_return support::Expected<agent::NestedToolCallOutcome>{std::move(outcome)};
                    });
        }
        return support::AsyncResult<agent::NestedToolCallOutcome>{support::Expected<agent::NestedToolCallOutcome>{
                std::unexpected(support::make_error(support::ErrorCode::Validation, "unknown fake tool"))}};
    };
    host.emit = [](const agent::AgentLifecycleEvent&) -> support::ExpectedVoid { return {}; };
    return agent::NestedToolCallRunner{std::move(host)};
}

/// Run one script through the real codemode tool source and its wasm sandbox.
[[nodiscard]] extensions::ExtensionToolResult run_script(std::string script,
        agent::NestedToolCallRunner* runner = nullptr,
        std::shared_ptr<extensions::CodemodeSessionState> session = nullptr,
        tests::RuntimeFixture* runtime = nullptr) {
    extensions::CodemodeToolSource source{codemode::default_codemode_guest_wasm_path(), nullptr, std::move(session)};
    auto tools = source.load_tools();
    REQUIRE(tools.has_value());
    REQUIRE(tools->size() == 1);
    extensions::ExtensionToolContext context;
    context.call_id = "call_1";
    context.nested_calls = runner;
    auto executed = tools->front().context_execute(
            support::JsonValue{support::JsonValue::object_t{{"code", std::move(script)}}},
            std::move(context),
            std::stop_token{});
    // A session-backed run writes through the Session filesystem, whose work
    // is admitted to the RuntimeFixture's loop; the plain seam runs on its own.
    if (runtime != nullptr) {
        auto outcome = tests::run_awaitable(*runtime, std::move(executed));
        REQUIRE(outcome.has_value());
        return std::move(*outcome);
    }
    auto outcome = tests::run_async_result(std::move(executed));
    REQUIRE(outcome.has_value());
    return std::move(*outcome);
}

/// The tool result as the captured envelope stores it: content blocks, details,
/// and the error flag.
[[nodiscard]] support::JsonValue result_json(const extensions::ExtensionToolResult& result) {
    support::JsonValue::array_t content;
    for (const auto& block : result.content) {
        if (const auto* text = std::get_if<ai::TextContent>(&block); text != nullptr) {
            content.emplace_back(support::JsonValue::object_t{{"type", "text"}, {"text", text->text}});
        } else if (const auto* image = std::get_if<ai::ImageContent>(&block); image != nullptr) {
            content.emplace_back(support::JsonValue::object_t{
                    {"type", "image"},
                    {"data", image->data},
                    {"mimeType", image->mime_type},
            });
        }
    }
    support::JsonValue::object_t root{{"content", support::JsonValue{std::move(content)}}};
    if (result.details.has_value()) root.emplace("details", *result.details);
    if (result.is_error) root.emplace("isError", true);
    return support::JsonValue{std::move(root)};
}

/// Apply the capture's declared normalizations to an observed value: wall time
/// and every present `durationMs` become `<elapsed>` (presence is retained),
/// temporary output paths become `<output-file>`, and the unique workspace
/// becomes `<workspace>`.
[[nodiscard]] support::JsonValue normalize(support::JsonValue value, const std::string& workspace) {
    static const std::regex wall_time{R"(Wall time [0-9]+\.[0-9] seconds)"};
    static const std::regex output_path{R"(/[^\s\]]*pi-codemode-[0-9a-f]+\.[a-z]+)"};
    if (const auto* text = value.get_if<std::string>(); text != nullptr) {
        std::string normalized = std::regex_replace(*text, wall_time, "Wall time <elapsed> seconds");
        normalized = std::regex_replace(normalized, output_path, "<output-file>");
        if (!workspace.empty()) {
            std::string::size_type position = 0;
            while ((position = normalized.find(workspace, position)) != std::string::npos) {
                normalized.replace(position, workspace.size(), "<workspace>");
                position += std::string_view{"<workspace>"}.size();
            }
        }
        return support::JsonValue{std::move(normalized)};
    }
    if (auto* object = value.get_if<support::JsonValue::object_t>(); object != nullptr) {
        for (auto& [key, member] : *object) {
            if (key == "durationMs") {
                member = support::JsonValue{"<elapsed>"};
                continue;
            }
            member = normalize(std::move(member), workspace);
        }
        return value;
    }
    if (auto* array = value.get_if<support::JsonValue::array_t>(); array != nullptr) {
        for (auto& member : *array)
            member = normalize(std::move(member), workspace);
        return value;
    }
    return value;
}

void check_scenario(const support::JsonValue& artifact,
        std::string_view name,
        const extensions::ExtensionToolResult& result,
        const std::string& workspace = {}) {
    // `fullText` is the capture's evidence copy of the spilled bytes; the
    // budget case reads the real file instead, so it is not part of the result.
    auto expected = scenario(artifact, name).get_object().at("expected").get_object().at("result");
    if (auto* fields = expected.get_if<support::JsonValue::object_t>(); fields != nullptr) fields->erase("fullText");
    const auto actual = normalize(result_json(result), workspace);
    INFO("scenario: " << name);
    const auto mismatch = tests::json_mismatch(expected, actual);
    INFO(mismatch.value_or(""));
    CHECK_FALSE(mismatch.has_value());
}

} // namespace

TEST_CASE("the frozen pi-v1.3.0 codemode output layout numbers text and groups console output",
        "[coding_agent][codemode][issue1119][spec]") {
    const auto artifact = read_artifact();
    check_scenario(
            artifact, "presentation", run_script("console.log('log'); text('first'); text('second'); return 'last';"));
}

TEST_CASE("the frozen pi-v1.3.0 codemode output budget truncates and spills the full text",
        "[coding_agent][codemode][issue1119][spec]") {
    const auto artifact = read_artifact();
    tests::TempWorkspace workspace;
    tests::RuntimeFixture runtime;
    auto session = std::make_shared<extensions::CodemodeSessionState>();
    session->filesystem = std::make_shared<harness::AsyncLocalFileSystem>(runtime.make_target(), workspace.path());

    const auto budget = run_script(
            "// @options: {\"max_output_tokens\": 2}\ntext('abcdefghijklmnopqrstuvwxyz');", nullptr, session, &runtime);
    check_scenario(artifact, "budget", budget, workspace.path().string());
    REQUIRE(budget.details.has_value());
    const auto* spilled = budget.details->get_object().at("fullOutputPath").get_if<std::string>();
    REQUIRE(spilled != nullptr);
    // The content says the full text was saved; the file must actually hold it.
    CHECK(read_text(*spilled) == "abcdefghijklmnopqrstuvwxyz");
    std::filesystem::remove(*spilled);

    const auto unicode =
            run_script("// @options: {\"max_output_tokens\": 1}\ntext('猫'.repeat(12));", nullptr, session, &runtime);
    check_scenario(artifact, "unicode-budget", unicode, workspace.path().string());
    REQUIRE(unicode.details.has_value());
    if (const auto* path = unicode.details->get_object().at("fullOutputPath").get_if<std::string>(); path != nullptr) {
        CHECK(read_text(*path) == "猫猫猫猫猫猫猫猫猫猫猫猫");
        std::filesystem::remove(*path);
    }

    const auto error_budget =
            run_script("// @options: {\"max_output_tokens\": 0}\nthrow 'boom';", nullptr, session, &runtime);
    check_scenario(artifact, "zero-error-budget", error_budget, workspace.path().string());
    REQUIRE(error_budget.details.has_value());
    if (const auto* path = error_budget.details->get_object().at("fullOutputPath").get_if<std::string>();
            path != nullptr) {
        CHECK(read_text(*path).starts_with("Script error:\nError: boom"));
        std::filesystem::remove(*path);
    }
}

TEST_CASE("the frozen pi-v1.3.0 codemode failure keeps prior calls and console output",
        "[coding_agent][codemode][issue1119][spec]") {
    const auto artifact = read_artifact();
    auto runner = make_runner({text_tool("write", "kept")});
    check_scenario(artifact,
            "prior-calls",
            run_script("await tools.write({path:'before.txt',content:'kept'}); console.log('log'); text('before'); "
                       "throw 'boom';",
                    &runner));
}

TEST_CASE("the frozen pi-v1.3.0 codemode timeout reports the interrupted call as cancelled",
        "[coding_agent][codemode][issue1119][spec]") {
    const auto artifact = read_artifact();
    auto runner = make_runner({text_tool("write", "kept"), waiting_tool("bash")});
    check_scenario(artifact,
            "timeout",
            run_script("// @options: {\"timeout_ms\": 100}\nawait tools.write({path:'before.txt',content:'kept'}); "
                       "text('before'); await tools.bash({command:'sleep 30'});",
                    &runner));
}

TEST_CASE("a codemode image is saved to disk next to its path label", "[coding_agent][codemode][issue1119][spec]") {
    const auto artifact = read_artifact();
    tests::TempWorkspace workspace;
    tests::RuntimeFixture runtime;
    auto session = std::make_shared<extensions::CodemodeSessionState>();
    session->filesystem = std::make_shared<harness::AsyncLocalFileSystem>(runtime.make_target(), workspace.path());

    // The frozen scenario's own script, so the image bytes and its saved-size
    // label compare against the capture rather than a different fixture image.
    const auto image_script =
            scenario(artifact, "image").get_object().at("inputs").get_object().at("code").get_string();
    const auto result = run_script(image_script, nullptr, session, &runtime);
    check_scenario(artifact, "image", result, workspace.path().string());

    // The content check alone would pass while no file exists; the label names
    // a real path and its bytes are the decoded image.
    REQUIRE(result.content.size() == 3);
    const auto* label = std::get_if<ai::TextContent>(&result.content[1]);
    REQUIRE(label != nullptr);
    static const std::regex saved{R"(\[Image saved to (/[^\]]*pi-codemode-[0-9a-f]+\.png) \(image/png, 68B\)\])"};
    std::smatch match;
    REQUIRE(std::regex_search(label->text, match, saved));
    const auto bytes = read_text(match[1].str());
    static const std::regex base64_data{R"(base64,([^')]+))"};
    std::smatch encoded;
    REQUIRE(std::regex_search(image_script, encoded, base64_data));
    std::string expected_bytes;
    for (const auto byte : coding_agent::decode_base64(encoded[1].str())) {
        expected_bytes.push_back(static_cast<char>(byte));
    }
    CHECK(bytes == expected_bytes);
    std::filesystem::remove(match[1].str());
}

TEST_CASE("a codemode store entry replays from the root and a later delete wins",
        "[coding_agent][codemode][issue1119][spec]") {
    auto store = std::make_shared<harness::session::SessionStore>(
            harness::session::SessionStore::in_memory(harness::session::SessionMetadata{}));
    const auto append = [&store](std::string_view set_json, std::string_view delete_json) {
        auto parsed = support::read_json(
                std::string{"{\"set\":"} + std::string{set_json} + ",\"delete\":" + std::string{delete_json} + "}");
        REQUIRE(parsed.has_value());
        REQUIRE(store->append_custom_entry(std::nullopt, "codemode-store", std::move(*parsed)).has_value());
    };
    append("{\"answer\":42,\"kept\":true}", "[]");
    append("{\"answer\":7}", "[\"kept\"]");
    const auto values = extensions::codemode_store_from_branch(store->get_branch());
    REQUIRE(values.size() == 1);
    CHECK(values.at("answer").get_number() == 7);

    auto session = std::make_shared<extensions::CodemodeSessionState>();
    session->branch = [store]() {
        return support::Expected<std::vector<harness::session::SessionEntry>>{store->get_branch()};
    };
    session->append_store = [store](std::string custom_type, support::JsonValue data) {
        return store->append_custom_entry(std::nullopt, std::move(custom_type), std::move(data));
    };
    // `load()` sees the persisted value; the successful delete run persists its
    // own write set, and the next replay observes the deletion.
    const auto loaded = run_script("return load('answer');", nullptr, session);
    REQUIRE(loaded.content.size() == 2);
    CHECK(std::get<ai::TextContent>(loaded.content.at(1)).text == "7");
    const auto deleted = run_script("store('answer', undefined); return load('answer');", nullptr, session);
    CHECK_FALSE(deleted.is_error);
    CHECK(extensions::codemode_store_from_branch(store->get_branch()).empty());
}
