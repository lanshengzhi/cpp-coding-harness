#include "../../third_party/catch2/catch_test_macros.hpp"

#include "../../include/cch/coding_agent/PromptProcessing.hpp"

#include <string>
#include <vector>

using namespace cch;

TEST_CASE("expand_prompt_template non-slash input passes through", "[coding_agent][prompt][expand]") {
    auto result = coding_agent::expand_prompt_template("hello world", {});
    CHECK(result == "hello world");
}

TEST_CASE("expand_prompt_template no matching template passes through", "[coding_agent][prompt][expand]") {
    std::vector<coding_agent::PromptTemplate> templates = {
        {"greet", std::nullopt, "Hello!"},
    };
    auto result = coding_agent::expand_prompt_template("/unknown", templates);
    CHECK(result == "/unknown");
}

TEST_CASE("expand_prompt_template substitutes $1", "[coding_agent][prompt][expand]") {
    std::vector<coding_agent::PromptTemplate> templates = {
        {"greet", std::nullopt, "Hello $1!"},
    };
    auto result = coding_agent::expand_prompt_template("/greet world", templates);
    CHECK(result == "Hello world!");
}

TEST_CASE("expand_prompt_template substitutes $@", "[coding_agent][prompt][expand]") {
    std::vector<coding_agent::PromptTemplate> templates = {
        {"echo", std::nullopt, "You said: $@"},
    };
    auto result = coding_agent::expand_prompt_template("/echo hello world", templates);
    CHECK(result == "You said: hello world");
}

TEST_CASE("expand_prompt_template substitutes $ARGUMENTS", "[coding_agent][prompt][expand]") {
    std::vector<coding_agent::PromptTemplate> templates = {
        {"args", std::nullopt, "Args: $ARGUMENTS"},
    };
    auto result = coding_agent::expand_prompt_template("/args a b c", templates);
    CHECK(result == "Args: a b c");
}

TEST_CASE("expand_prompt_template ${N:-default} with missing arg uses default", "[coding_agent][prompt][expand]") {
    std::vector<coding_agent::PromptTemplate> templates = {
        {"greet", std::nullopt, "Hello ${1:-there}!"},
    };
    auto result = coding_agent::expand_prompt_template("/greet", templates);
    CHECK(result == "Hello there!");
}

TEST_CASE("expand_prompt_template ${N:-default} with provided arg uses arg", "[coding_agent][prompt][expand]") {
    std::vector<coding_agent::PromptTemplate> templates = {
        {"greet", std::nullopt, "Hello ${1:-there}!"},
    };
    auto result = coding_agent::expand_prompt_template("/greet world", templates);
    CHECK(result == "Hello world!");
}

TEST_CASE("expand_prompt_template ${@:N} skips arguments", "[coding_agent][prompt][expand]") {
    std::vector<coding_agent::PromptTemplate> templates = {
        {"drop", std::nullopt, "Remaining: ${@:2}"},
    };
    auto result = coding_agent::expand_prompt_template("/drop first second third", templates);
    CHECK(result == "Remaining: second third");
}

TEST_CASE("expand_prompt_template ${@:N:L} slices arguments", "[coding_agent][prompt][expand]") {
    std::vector<coding_agent::PromptTemplate> templates = {
        {"slice", std::nullopt, "Slice: ${@:2:2}"},
    };
    auto result = coding_agent::expand_prompt_template("/slice a b c d", templates);
    CHECK(result == "Slice: b c");
}

TEST_CASE("expand_prompt_template quoted arguments preserve spaces", "[coding_agent][prompt][expand]") {
    std::vector<coding_agent::PromptTemplate> templates = {
        {"greet", std::nullopt, "Hello $1!"},
    };
    auto result = coding_agent::expand_prompt_template("/greet \"hello world\"", templates);
    CHECK(result == "Hello hello world!");
}

TEST_CASE("expand_prompt_template single-quoted arguments preserve $", "[coding_agent][prompt][expand]") {
    std::vector<coding_agent::PromptTemplate> templates = {
        {"cmd", std::nullopt, "Running: $1"},
    };
    auto result = coding_agent::expand_prompt_template("/cmd '$HOME'", templates);
    CHECK(result == "Running: $HOME");
}

TEST_CASE("expand_prompt_template process_prompt integration expands template", "[coding_agent][prompt][expand]") {
    std::vector<coding_agent::PromptTemplate> templates = {
        {"greet", std::nullopt, "Hello $1!"},
    };
    coding_agent::CommandRegistry registry;
    auto result = coding_agent::process_prompt("/greet world", templates, registry);
    CHECK(result.command_handled == false); // not a command — expanded text goes to agent loop
    CHECK(result.expanded_prompt == "Hello world!");
}

TEST_CASE("expand_prompt_template out of range positional ref is empty", "[coding_agent][prompt][expand]") {
    std::vector<coding_agent::PromptTemplate> templates = {
        {"cmd", std::nullopt, "Arg: [$1][$2]"},
    };
    auto result = coding_agent::expand_prompt_template("/cmd one", templates);
    CHECK(result == "Arg: [one][]");
}
