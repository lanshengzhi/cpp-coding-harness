#include "../../third_party/catch2/catch_test_macros.hpp"

#include "coding_agent/prompt/PromptProcessor.hpp"

#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace cch;

namespace {

[[nodiscard]] std::string process_template(
    std::string input,
    std::vector<coding_agent::PromptTemplate> templates) {
    coding_agent::prompt::PromptResources resources;
    resources.templates = std::move(templates);
    coding_agent::prompt::PromptProcessor processor{std::move(resources)};

    auto outcome = processor.process(std::move(input), {});
    REQUIRE(std::holds_alternative<coding_agent::prompt::AgentPrompt>(outcome));
    return std::move(std::get<coding_agent::prompt::AgentPrompt>(outcome).text);
}

} // namespace

TEST_CASE("prompt processor passes through non-template input", "[coding_agent][prompt][expand]") {
    CHECK(process_template("hello world", {}) == "hello world");
    CHECK(process_template("/unknown", {{"greet", std::nullopt, "Hello!"}}) == "/unknown");
}

TEST_CASE("prompt processor substitutes template positional arguments", "[coding_agent][prompt][expand]") {
    CHECK(process_template(
              "/greet world",
              {{"greet", std::nullopt, "Hello $1!"}}) == "Hello world!");
}

TEST_CASE("prompt processor substitutes template argument collections", "[coding_agent][prompt][expand]") {
    CHECK(process_template(
              "/echo hello world",
              {{"echo", std::nullopt, "You said: $@"}}) == "You said: hello world");
    CHECK(process_template(
              "/args a b c",
              {{"args", std::nullopt, "Args: $ARGUMENTS"}}) == "Args: a b c");
}

TEST_CASE("prompt processor applies template argument defaults", "[coding_agent][prompt][expand]") {
    const std::vector<coding_agent::PromptTemplate> templates = {
        {"greet", std::nullopt, "Hello ${1:-there}!"},
    };
    CHECK(process_template("/greet", templates) == "Hello there!");
    CHECK(process_template("/greet world", templates) == "Hello world!");
}

TEST_CASE("prompt processor slices template arguments", "[coding_agent][prompt][expand]") {
    CHECK(process_template(
              "/drop first second third",
              {{"drop", std::nullopt, "Remaining: ${@:2}"}}) == "Remaining: second third");
    CHECK(process_template(
              "/slice a b c d",
              {{"slice", std::nullopt, "Slice: ${@:2:2}"}}) == "Slice: b c");
}

TEST_CASE("prompt processor preserves spaces and dollars inside quoted template arguments", "[coding_agent][prompt][expand]") {
    CHECK(process_template(
              "/greet \"hello world\"",
              {{"greet", std::nullopt, "Hello $1!"}}) == "Hello hello world!");
    CHECK(process_template(
              "/cmd '$HOME'",
              {{"cmd", std::nullopt, "Running: $1"}}) == "Running: $HOME");
}

TEST_CASE("prompt processor expands matching templates into agent prompts", "[coding_agent][prompt][expand]") {
    CHECK(process_template(
              "/greet world",
              {{"greet", std::nullopt, "Hello $1!"}}) == "Hello world!");
}

TEST_CASE("prompt processor replaces out-of-range template positions with empty text", "[coding_agent][prompt][expand]") {
    CHECK(process_template(
              "/cmd one",
              {{"cmd", std::nullopt, "Arg: [$1][$2]"}}) == "Arg: [one][]");
}

TEST_CASE("prompt processor matches pi positional and slice edge cases", "[coding_agent][prompt][expand]") {
    CHECK(process_template(
              "/args a b '' d e f g h i j ten",
              {{"args", std::nullopt, "[$0][$10][${@:0}][${@:2:0}][${11:-fallback}]"}}) ==
          "[][ten][a b d e f g h i j ten][][fallback]");
}

TEST_CASE("prompt processor treats mixed whitespace as template argument separators", "[coding_agent][prompt][expand]") {
    CHECK(process_template(
              "/args\nfirst\n\n\tsecond  third fourth",
              {{"args", std::nullopt, "$1|$2|${@:3}"}}) == "first|second|third fourth");
}

TEST_CASE("prompt processor treats Unicode whitespace as template argument separators", "[coding_agent][prompt][expand]") {
    const std::string input = std::string{"/args"} + "\xC2\xA0" + "first" + "\xE3\x80\x80" + "second";
    CHECK(process_template(
              input,
              {{"args", std::nullopt, "$1|$2"}}) == "first|second");
}

TEST_CASE("prompt processor preserves pi quote and escaped-quote parsing", "[coding_agent][prompt][expand]") {
    CHECK(process_template(
              "/args \"first value\" \"quoted \\\"text\\\"\"",
              {{"args", std::nullopt, "$1|$2"}}) == "first value|quoted \\text\\");
}

TEST_CASE("prompt processor expands templates once and preserves malformed placeholders", "[coding_agent][prompt][expand]") {
    CHECK(process_template(
              "/args '$ARGUMENTS'",
              {{"args", std::nullopt, "$1|${2:-$ARGUMENTS}|${bad}|${1-default}|${@:}"}}) ==
          "$ARGUMENTS|$ARGUMENTS|${bad}|${1-default}|${@:}");
}

TEST_CASE("prompt processor recognizes template invocations only at column zero", "[coding_agent][prompt][expand]") {
    const std::vector<coding_agent::PromptTemplate> templates = {
        {"args", std::nullopt, "expanded"},
    };
    CHECK(process_template(" /args", templates) == " /args");
    CHECK(process_template("\t/args", templates) == "\t/args");
}
