// Spec #865 codemode slice (#874): the sandboxed execution wiring. The
// declared script runs inside pi's actual codemode guest (QuickJS compiled to
// wasm) under WasmEdge, returns its value and output, and fails closed at the
// sandbox boundary: an escape attempt is refused, a runaway script is
// interrupted, and no host filesystem or network capability exists.
//
// The separation case the cheap check lets through: a script that *successfully
// returns* while having attempted a filesystem write. The tests therefore assert
// both the returned value and the absence of the side effect on disk, and pair a
// caught escape (returns "refused") with an uncaught one (an explicit error).

#include "coding_agent/extensions/codemode/CodemodePrelude.hpp"
#include "coding_agent/extensions/codemode/CodemodeSandbox.hpp"
#include "coding_agent/extensions/codemode/CodemodeToolSource.hpp"
#include "support/StreamAdapterFixture.hpp"
#include "support/TempWorkspace.hpp"

#include <cch/ai/Content.hpp>
#include <cch/support/AsyncResult.hpp>
#include <cch/support/Error.hpp>
#include <cch/support/JsonValue.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

using namespace cch;

namespace {

using coding_agent::extensions::CodemodeError;
using coding_agent::extensions::CodemodeLimits;
using coding_agent::extensions::CodemodeOutputItem;
using coding_agent::extensions::CodemodeRunResult;
using coding_agent::extensions::CodemodeSandbox;
using coding_agent::extensions::CodemodeToolCallHandler;
using coding_agent::extensions::CodemodeToolDescriptor;
using coding_agent::extensions::CodemodeToolSource;

[[nodiscard]] std::filesystem::path guest_path() {
    return std::filesystem::path{CCH_SOURCE_DIR} / "fixtures" / "codemode" / "quickjs" / "quickjs.wasm";
}

[[nodiscard]] std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

[[nodiscard]] std::unique_ptr<CodemodeSandbox> make_sandbox() {
    auto sandbox = CodemodeSandbox::create(guest_path());
    REQUIRE(sandbox.has_value());
    return std::move(*sandbox);
}

[[nodiscard]] CodemodeRunResult run_script(
        CodemodeSandbox& sandbox, std::string script, std::chrono::milliseconds timeout = std::chrono::seconds{30}) {
    CodemodeLimits limits;
    limits.timeout = timeout;
    auto outcome = tests::run_async_result(sandbox.run(std::move(script), {}, limits));
    REQUIRE(outcome.has_value());
    return std::move(*outcome);
}

} // namespace

TEST_CASE("the codemode guest fixture is the pinned quickjs-wasi module", "[coding_agent][codemode][issue874][spec]") {
    auto bytes = read_file(guest_path());
    // Provenance is pinned by digest in fixtures/codemode/quickjs/README.md and
    // asserted here by the WebAssembly magic and the exact size of the pinned
    // quickjs-wasi 3.6.2 artifact.
    REQUIRE(bytes.size() == 637405);
    REQUIRE(bytes.starts_with(std::string_view{"\0asm\1\0\0\0", 8}));
}

TEST_CASE("the embedded codemode prelude matches the committed fixture", "[coding_agent][codemode][issue874][spec]") {
    const auto fixture =
            read_file(std::filesystem::path{CCH_SOURCE_DIR} / "fixtures" / "codemode" / "quickjs" / "prelude.js");
    CHECK(std::string{coding_agent::extensions::kCodemodePreludeSource} == fixture);
}

TEST_CASE("a script runs in the sandbox and returns its value and text output",
        "[coding_agent][codemode][issue874][spec]") {
    auto sandbox = make_sandbox();

    auto result = run_script(*sandbox, "text('hello'); console.log('world'); return { ok: 42, name: 'pike' };");

    REQUIRE_FALSE(result.error.has_value());
    REQUIRE(result.value_json.has_value());
    CHECK(*result.value_json == R"({"ok":42,"name":"pike"})");
    REQUIRE(result.output.size() == 2);
    CHECK(result.output[0].kind == CodemodeOutputItem::Kind::Text);
    CHECK(result.output[0].data == "hello");
    CHECK(result.output[1].data == "world");
}

TEST_CASE("image output is detected and typed like pi's runtime", "[coding_agent][codemode][issue874][spec]") {
    auto sandbox = make_sandbox();

    auto result = run_script(*sandbox,
            "image('data:image/png;base64,"
            "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAAC0lEQVR42mNk+M9QDwADhgGAWjR9awAAAABJRU5ErkJggg==');"
            "return 'rendered';");

    REQUIRE_FALSE(result.error.has_value());
    REQUIRE(result.output.size() == 1);
    CHECK(result.output[0].kind == CodemodeOutputItem::Kind::Image);
    CHECK(result.output[0].mime_type == "image/png");
    CHECK(result.output[0].data.starts_with("iVBORw0KGgo"));
}

TEST_CASE("a script error is reported with its message and kind", "[coding_agent][codemode][issue874][spec]") {
    auto sandbox = make_sandbox();

    auto result = run_script(*sandbox, "throw new Error('boom from the script');");

    REQUIRE(result.error.has_value());
    CHECK(result.error->kind == CodemodeError::Kind::Script);
    CHECK(result.error->message.find("boom from the script") != std::string::npos);
    CHECK_FALSE(result.value_json.has_value());
}

TEST_CASE("an over-limit script is interrupted with a timeout error", "[coding_agent][codemode][issue874][spec]") {
    auto sandbox = make_sandbox();

    auto result = run_script(*sandbox, "for (let i = 0; i < 1e18; i += 1) {}", std::chrono::milliseconds{150});

    REQUIRE(result.error.has_value());
    CHECK(result.error->kind == CodemodeError::Kind::Timeout);
    CHECK(result.error->message.find("timed out") != std::string::npos);
}

TEST_CASE("a pre-cancelled run never starts the script", "[coding_agent][codemode][issue874][spec]") {
    auto sandbox = make_sandbox();
    std::stop_source stop;
    stop.request_stop();
    CodemodeLimits limits;
    limits.timeout = std::chrono::seconds{30};

    auto outcome =
            tests::run_async_result(sandbox->run("text('should not run'); return 1;", {}, limits, stop.get_token()));
    REQUIRE(outcome.has_value());
    const auto& result = *outcome;

    REQUIRE(result.error.has_value());
    CHECK(result.error->kind == CodemodeError::Kind::Aborted);
    CHECK(result.output.empty());
}

TEST_CASE("an escape attempt fails closed and writes nothing to the host filesystem",
        "[coding_agent][codemode][issue874][spec]") {
    auto sandbox = make_sandbox();
    tests::TempWorkspace workspace;
    const auto target = workspace.path() / "escaped.txt";

    // The script RETURNS successfully while attempting a filesystem write. The
    // cheap check would only see a string return; the property is the absent
    // side effect, which is asserted directly.
    auto caught = run_script(*sandbox,
            "try { require('fs').writeFileSync('" + target.string() +
                    "', 'owned'); return 'wrote'; } "
                    "catch (e) { return 'refused:' + e.name; }");
    REQUIRE_FALSE(caught.error.has_value());
    REQUIRE(caught.value_json.has_value());
    CHECK(*caught.value_json == R"("refused:ReferenceError")");
    CHECK_FALSE(std::filesystem::exists(target));

    // The same attempt uncaught is an explicit, user-visible sandbox error.
    auto uncaught =
            run_script(*sandbox, "require('fs').writeFileSync('/tmp/pike-874-should-not-exist', 'x'); return 1;");
    REQUIRE(uncaught.error.has_value());
    CHECK(uncaught.error->kind == CodemodeError::Kind::Script);
    CHECK(uncaught.error->message.find("require is not defined") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists("/tmp/pike-874-should-not-exist"));

    // Network and host-object escapes are equally unreachable.
    auto probe = run_script(
            *sandbox, "return typeof fetch + '|' + typeof process + '|' + typeof WebAssembly + '|' + typeof require;");
    REQUIRE(probe.value_json.has_value());
    CHECK(*probe.value_json == R"("undefined|undefined|undefined|undefined")");
}

TEST_CASE("tools.* calls route back to the host tool handler", "[coding_agent][codemode][issue874][spec]") {
    auto sandbox = make_sandbox();
    const std::vector<CodemodeToolDescriptor> tools{
            {"echo", "echo", "Echo the argument."},
    };
    std::string seen_name;
    std::string seen_args;
    CodemodeToolCallHandler handler = [&](std::string_view name, std::string_view args, std::stop_token) {
        seen_name = std::string{name};
        seen_args = std::string{args};
        return support::AsyncResult<std::string>{support::Expected<std::string>{std::string{R"("pong")"}}};
    };
    CodemodeLimits limits;
    limits.timeout = std::chrono::seconds{30};

    auto outcome = tests::run_async_result(
            sandbox->run("const r = await tools.echo({ x: 1 }); return r;", tools, limits, {}, std::move(handler)));
    REQUIRE(outcome.has_value());
    const auto& result = *outcome;

    REQUIRE_FALSE(result.error.has_value());
    REQUIRE(result.value_json.has_value());
    CHECK(*result.value_json == R"("pong")");
    CHECK(seen_name == "echo");
    CHECK(seen_args == R"({"x":1})");
}

TEST_CASE("a tool call with no handler is rejected explicitly, never silently",
        "[coding_agent][codemode][issue874][spec]") {
    auto sandbox = make_sandbox();
    const std::vector<CodemodeToolDescriptor> tools{
            {"echo", "echo", "Echo the argument."},
    };
    CodemodeLimits limits;
    limits.timeout = std::chrono::seconds{30};

    auto outcome = tests::run_async_result(sandbox->run("return await tools.echo(1);", tools, limits));
    REQUIRE(outcome.has_value());
    const auto& result = *outcome;

    REQUIRE(result.error.has_value());
    CHECK(result.error->kind == CodemodeError::Kind::Script);
    CHECK(result.error->message.find("echo") != std::string::npos);
    CHECK(result.error->message.find("not available") != std::string::npos);
}

TEST_CASE("a script's tools.* call is an explicit does-not-exist error, never silent",
        "[coding_agent][codemode][issue874][spec]") {
    auto sandbox = make_sandbox();
    CodemodeLimits limits;
    limits.timeout = std::chrono::seconds{30};

    // The sandbox exposes no session tool surface in this slice; a script that
    // calls one is refused explicitly (the prelude's guard) rather than getting
    // an undefined function or an unimplemented-capability stub.
    auto outcome = tests::run_async_result(sandbox->run("return await tools.read({ path: 'x' });", {}, limits));
    REQUIRE(outcome.has_value());
    const auto& result = *outcome;

    REQUIRE(result.error.has_value());
    CHECK(result.error->kind == CodemodeError::Kind::Script);
    CHECK(result.error->message.find("tools.read does not exist") != std::string::npos);
}

TEST_CASE("a missing guest module is an explicit create error, not a deferred failure",
        "[coding_agent][codemode][issue874][spec]") {
    auto sandbox = CodemodeSandbox::create(
            std::filesystem::path{CCH_SOURCE_DIR} / "fixtures" / "codemode" / "quickjs" / "does-not-exist.wasm");

    REQUIRE_FALSE(sandbox.has_value());
    CHECK(sandbox.error().code == support::ErrorCode::Validation);
}

TEST_CASE("the model-facing codemode tool runs an inline script in the wasm sandbox",
        "[coding_agent][codemode][issue885][spec]") {
    // The inline tool carries no on-disk declaration; the source is the model's
    // `code` argument, exactly as pi's `codemode` tool.
    CodemodeToolSource source{};
    auto tools = source.load_tools();
    REQUIRE(tools.has_value());
    REQUIRE(tools->size() == 1);
    CHECK(tools->front().definition.name == "codemode");

    auto executed = tools->front().context_execute(
            support::JsonValue{support::JsonValue::object_t{{"code", "text('computing'); return 1 + 1;"}}},
            coding_agent::extensions::ExtensionToolContext{},
            std::stop_token{});
    auto outcome = tests::run_async_result(std::move(executed));

    REQUIRE(outcome.has_value());
    CHECK_FALSE(outcome->is_error);
    const auto text = ai::text_from_content(outcome->content);
    CHECK(text.find("computing") != std::string::npos);
    CHECK(text.find("2") != std::string::npos);
}
