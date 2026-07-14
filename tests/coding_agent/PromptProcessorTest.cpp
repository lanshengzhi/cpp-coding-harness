#include "../../third_party/catch2/catch_test_macros.hpp"

#include "coding_agent/prompt/PromptProcessor.hpp"

#include <string>
#include <string_view>

using namespace cch;

namespace {

coding_agent::prompt::PromptProcessor make_processor(
    std::vector<coding_agent::Skill> skills = {},
    std::vector<coding_agent::PromptTemplate> templates = {}) {
    return coding_agent::prompt::PromptProcessor{
        std::move(skills), std::move(templates)};
}

coding_agent::Skill make_skill(std::string name, std::string content) {
    return coding_agent::Skill{
        .name = std::move(name),
        .description = "test skill",
        .content = std::move(content),
        .filePath = "/snapshot/test/SKILL.md",
    };
}

coding_agent::PromptTemplate make_template(std::string name, std::string content) {
    return coding_agent::PromptTemplate{
        .name = std::move(name),
        .description = std::nullopt,
        .content = std::move(content),
    };
}

} // namespace

TEST_CASE("prompt processor returns ordinary and empty input as agent prompts", "[coding_agent][prompt][processor]") {
    auto processor = make_processor();

    const auto ordinary = processor.process("hello", true);
    const auto empty = processor.process("", true);

    CHECK(ordinary.text == "hello");
    CHECK(empty.text.empty());
}

TEST_CASE("prompt processor bypasses skill and template expansion when requested", "[coding_agent][prompt][processor]") {
    auto processor = make_processor(
        {make_skill("cached", "cached skill body")},
        {make_template("review", "Review: $1")});

    CHECK(processor.process("/skill:cached", false).text == "/skill:cached");
    CHECK(processor.process("/review target", false).text == "/review target");
}

TEST_CASE("prompt processor expands skills before templates from its owned snapshot", "[coding_agent][prompt][processor]") {
    auto processor = make_processor(
        {make_skill("same", "cached skill body")},
        {
            make_template("skill:same", "template won"),
            make_template("template", "template: $1"),
        });

    const auto skill = processor.process("/skill:same raw instructions", true);
    CHECK(skill.text.find("<skill name=\"same\"") != std::string::npos);
    CHECK(skill.text.find("cached skill body") != std::string::npos);
    CHECK(skill.text.find("</skill>\n\nraw instructions") != std::string::npos);
    CHECK(skill.text.find("template won") == std::string::npos);
}

TEST_CASE("prompt processor expands templates once and preserves unmatched slash input", "[coding_agent][prompt][processor]") {
    auto processor = make_processor(
        {},
        {make_template("handoff", "/quit $1 ${@:2}")});

    const auto expanded = processor.process("/handoff\nfirst second third", true);
    CHECK(expanded.text == "/quit first second third");

    for (const std::string input : {"/missing", "/", "!echo hi", "!!echo hi"}) {
        const auto passthrough = processor.process(input, true);
        CHECK(passthrough.text == input);
    }
}

TEST_CASE("prompt processor ignores skill and template content that is not a slash invocation", "[coding_agent][prompt][processor]") {
    auto processor = make_processor(
        {make_skill("skill-only", "skill")},
        {make_template("template-only", "template")});

    const auto help = processor.process("/help", true);
    CHECK(help.text == "/help");
    CHECK(help.text.find("skill-only") == std::string::npos);
    CHECK(help.text.find("template-only") == std::string::npos);
}

TEST_CASE("prompt processor treats slash input at column zero as potential skill or template", "[coding_agent][prompt][processor]") {
    auto processor = make_processor(
        {},
        {make_template("args", "expanded")});

    CHECK(processor.process(" /args", true).text == " /args");
    CHECK(processor.process("\t/args", true).text == "\t/args");
    CHECK(processor.process("/args", true).text == "expanded");
}

TEST_CASE("prompt processor retains its immutable skill and template snapshots", "[coding_agent][prompt][processor]") {
    auto processor = make_processor(
        {make_skill("cached", "cached body")},
        {make_template("review", "Review: $1")});

    REQUIRE(processor.skills().size() == 1);
    CHECK(processor.skills()[0].name == "cached");
    REQUIRE(processor.templates().size() == 1);
    CHECK(processor.templates()[0].name == "review");
}
