#include "coding_agent/prompt/PromptExpansion.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>
#include <vector>

using namespace cch;

namespace {

[[nodiscard]] std::string process_template(
    std::string input,
    std::vector<coding_agent::PromptTemplate> templates) {
    return coding_agent::prompt::expand_prompt_input(
        std::move(input),
        std::vector<coding_agent::Skill>{},
        std::move(templates),
        true);
}

} // namespace

TEST_CASE("prompt expansion passes through non-template input", "[coding_agent][prompt][expand]") {
    CHECK(process_template("hello world", {}) == "hello world");
    CHECK(process_template("/unknown", {{"greet", std::nullopt, "Hello!", std::nullopt, "", {}}}) == "/unknown");
}

TEST_CASE("prompt expansion substitutes template positional arguments", "[coding_agent][prompt][expand]") {
    CHECK(process_template(
              "/greet world",
              {{"greet", std::nullopt, "Hello $1!", std::nullopt, "", {}}}) == "Hello world!");
}

TEST_CASE("prompt expansion substitutes template argument collections", "[coding_agent][prompt][expand]") {
    CHECK(process_template(
              "/echo hello world",
              {{"echo", std::nullopt, "You said: $@", std::nullopt, "", {}}}) == "You said: hello world");
    CHECK(process_template(
              "/args a b c",
              {{"args", std::nullopt, "Args: $ARGUMENTS", std::nullopt, "", {}}}) == "Args: a b c");
}

TEST_CASE("prompt expansion applies template argument defaults", "[coding_agent][prompt][expand]") {
    const std::vector<coding_agent::PromptTemplate> templates = {
        {"greet", std::nullopt, "Hello ${1:-there}!", std::nullopt, "", {}},
    };
    CHECK(process_template("/greet", templates) == "Hello there!");
    CHECK(process_template("/greet world", templates) == "Hello world!");
}

TEST_CASE("prompt expansion slices template arguments", "[coding_agent][prompt][expand]") {
    CHECK(process_template(
              "/drop first second third",
              {{"drop", std::nullopt, "Remaining: ${@:2}", std::nullopt, "", {}}}) == "Remaining: second third");
    CHECK(process_template(
              "/slice a b c d",
              {{"slice", std::nullopt, "Slice: ${@:2:2}", std::nullopt, "", {}}}) == "Slice: b c");
}

TEST_CASE("prompt expansion preserves spaces and dollars inside quoted template arguments", "[coding_agent][prompt][expand]") {
    CHECK(process_template(
              "/greet \"hello world\"",
              {{"greet", std::nullopt, "Hello $1!", std::nullopt, "", {}}}) == "Hello hello world!");
    CHECK(process_template(
              "/cmd '$HOME'",
              {{"cmd", std::nullopt, "Running: $1", std::nullopt, "", {}}}) == "Running: $HOME");
}

TEST_CASE("prompt expansion expands matching templates", "[coding_agent][prompt][expand]") {
    CHECK(process_template(
              "/greet world",
              {{"greet", std::nullopt, "Hello $1!", std::nullopt, "", {}}}) == "Hello world!");
}

TEST_CASE("prompt expansion replaces out-of-range template positions with empty text", "[coding_agent][prompt][expand]") {
    CHECK(process_template(
              "/cmd one",
              {{"cmd", std::nullopt, "Arg: [$1][$2]", std::nullopt, "", {}}}) == "Arg: [one][]");
}

TEST_CASE("prompt expansion matches pi positional and slice edge cases", "[coding_agent][prompt][expand]") {
    CHECK(process_template(
              "/args a b '' d e f g h i j ten",
              {{"args", std::nullopt, "[$0][$10][${@:0}][${@:2:0}][${11:-fallback}]", std::nullopt, "", {}}}) ==
          "[][ten][a b d e f g h i j ten][][fallback]");
}

TEST_CASE("prompt expansion treats mixed whitespace as template argument separators", "[coding_agent][prompt][expand]") {
    CHECK(process_template(
              "/args\nfirst\n\n\tsecond  third fourth",
              {{"args", std::nullopt, "$1|$2|${@:3}", std::nullopt, "", {}}}) == "first|second|third fourth");
}

TEST_CASE("prompt expansion treats Unicode whitespace as template argument separators", "[coding_agent][prompt][expand]") {
    const std::string input = std::string{"/args"} + "\xC2\xA0" + "first" + "\xE3\x80\x80" + "second";
    CHECK(process_template(
              input,
              {{"args", std::nullopt, "$1|$2", std::nullopt, "", {}}}) == "first|second");
}

TEST_CASE("prompt expansion preserves pi quote and escaped-quote parsing", "[coding_agent][prompt][expand]") {
    CHECK(process_template(
              "/args \"first value\" \"quoted \\\"text\\\"\"",
              {{"args", std::nullopt, "$1|$2", std::nullopt, "", {}}}) == "first value|quoted \\text\\");
}

TEST_CASE("prompt expansion expands templates once and preserves malformed placeholders", "[coding_agent][prompt][expand]") {
    CHECK(process_template(
              "/args '$ARGUMENTS'",
              {{"args", std::nullopt, "$1|${2:-$ARGUMENTS}|${bad}|${1-default}|${@:}", std::nullopt, "", {}}}) ==
          "$ARGUMENTS|$ARGUMENTS|${bad}|${1-default}|${@:}");
}

TEST_CASE("prompt expansion recognizes template invocations only at column zero", "[coding_agent][prompt][expand]") {
    const std::vector<coding_agent::PromptTemplate> templates = {
        {"args", std::nullopt, "expanded", std::nullopt, "", {}},
    };
    CHECK(process_template(" /args", templates) == " /args");
    CHECK(process_template("\t/args", templates) == "\t/args");
}
