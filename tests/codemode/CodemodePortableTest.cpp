// Ticket #1194 (spec #1044, ADR 0075): the portable no-Node codemode entry.
// This shard links ONLY cch_codemode_portable — never the worker-mode
// cch_codemode — so a portable subset that secretly imports or links a
// native-only runtime (WasmEdge, Boost.Asio, threads) fails to build, and the
// configure-time graph check in cmake/targets/Codemode.cmake fails before
// that. The constants below are compared against the frozen
// audit-1053/source-options.json observation through this shard, so the row's
// receipt binds the portable caller, not the full engine's headers.

#include <cch/codemode/Declarations.hpp>
#include <cch/codemode/Portable.hpp>
#include <cch/codemode/Prelude.hpp>
#include <cch/codemode/Source.hpp>
#include <cch/codemode/Types.hpp>

#include "support/Json.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <variant>

using namespace cch;

// Compile-level half of the no-Node/no-native check: if any portable header
// dragged a native-only header onto this translation unit, these guards fail
// the build even though the source below only touches constants.
#if defined(BOOST_ASIO_HPP)
#error "the portable codemode subset must not import Boost.Asio"
#endif
#if defined(WASMEDGE_C_API_H)
#error "the portable codemode subset must not import the WasmEdge C API"
#endif

namespace {

[[nodiscard]] support::JsonValue read_source_options_artifact() {
    const auto path =
            std::filesystem::path{CCH_SOURCE_DIR} / "fixtures" / "pi-v1.3.0" / "audit-1053" / "source-options.json";
    std::ifstream input(path, std::ios::binary);
    const std::string text{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    auto parsed = support::read_json(text);
    REQUIRE(parsed.has_value());
    return std::move(*parsed);
}

[[nodiscard]] const support::JsonValue& frozen_constants_scenario(const support::JsonValue& artifact) {
    for (const auto& candidate : artifact.get_object().at("scenarios").get_array()) {
        if (candidate.get_object().at("name").get_string() ==
                "the public options prefix and grammar constants are fixed")
            return candidate;
    }
    REQUIRE(false);
    return artifact;
}

} // namespace

TEST_CASE("the portable codemode entry exposes the frozen source constants and protocol types",
        "[codemode][portable][issue1194][spec]") {
    // A header-existence check passes while the entry still links the
    // worker-mode engine; this case links only cch_codemode_portable and
    // compares the constants' bytes against the frozen observation.
    const auto artifact = read_source_options_artifact();
    const auto& expected = frozen_constants_scenario(artifact).get_object().at("expected").get_object();

    CHECK(std::string{codemode::kCodemodeOptionsPrefix} == expected.at("prefix").get_string());
    CHECK(std::string{codemode::kCodemodeSourceGrammar} == expected.at("grammar").get_string());

    // The prelude and declaration constants are the same bytes the full
    // engine embeds, so a portable host renders and declares identically.
    CHECK(std::string_view{codemode::kCodemodePreludeSource}.starts_with("(function (bridge, toolsJson"));
    CHECK(codemode::kMaxOutputChars > 0);
    CHECK(codemode::kMaxOutputItems > 0);
    CHECK(codemode::kMaxStoreTotalChars > 0);
    CHECK(codemode::kMaxStoreValueChars > 0);
    CHECK(codemode::kDefaultInputSchemaMaxChars > 0);
    CHECK(codemode::kDefaultInterruptBudget == 100000);
    CHECK(codemode::kRemoteFlushItems == 1000);
    CHECK(codemode::kRemoteFlushChars == (std::size_t{1} << 20));

    // The Inline/Remote protocol types are complete passive values: a no-Node
    // host can build the request and read the exchange without the engine.
    codemode::CodemodeRemoteRequest request;
    request.data.code = "return 1;";
    request.data.tools.push_back(codemode::CodemodeToolDescriptor{.name = "a-b", .js_name = "a_b", .description = "t"});
    request.data.globals.push_back(codemode::CodemodeGlobal{.name = "searchTools", .spread = true});
    request.data.store.emplace("k", "1");
    CHECK(request.data.code == "return 1;");
    CHECK(request.data.tools.front().js_name == "a_b");
    CHECK(request.data.globals.front().spread);

    const codemode::CodemodeRemoteMessageVariant message = codemode::CodemodeRemoteCallMessage{
            .id = 1, .target = "tool", .name = "a-b", .args = std::string{"{\"x\":1}"}};
    const auto* call = std::get_if<codemode::CodemodeRemoteCallMessage>(&message);
    REQUIRE(call != nullptr);
    CHECK(call->name == "a-b");

    const codemode::CodemodeRemoteReplyVariant reply = codemode::CodemodeRemoteResultReply{
            .id = 1, .ok = true, .payload = std::string{"1"}, .seq = 1, .now = 2.0, .properties = std::nullopt};
    CHECK(std::holds_alternative<codemode::CodemodeRemoteResultReply>(reply));
}
