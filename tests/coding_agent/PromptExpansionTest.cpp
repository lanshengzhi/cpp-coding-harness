#include "coding_agent/prompt/PromptExpansion.hpp"
#include "../support/TempWorkspace.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <string_view>

using namespace cch;

namespace {

/// Write a skill file and return the Skill discovery record pointing at it.
/// The file body is NOT preloaded: `/skill:` expansion reads the file at
/// invocation time (pi `_expandSkillCommand`).
coding_agent::Skill make_skill_on_disk(
    tests::TempWorkspace& workspace,
    std::string name,
    std::string body) {
    const auto relative = "skills/" + name + "/SKILL.md";
    workspace.write(
        relative,
        "---\n"
        "name: " + name + "\n"
        "description: test skill\n"
        "---\n" + body);
    const auto file_path = (workspace.path() / relative).string();
    return coding_agent::Skill{
        .name = std::move(name),
        .description = "test skill",
        .filePath = file_path,
        .baseDir = std::filesystem::path{file_path}.parent_path().string(),
        .sourceInfo = coding_agent::SourceInfo{
            .path = file_path,
            .source = "auto",
            .scope = coding_agent::SourceScope::Temporary,
            .origin = coding_agent::SourceOrigin::TopLevel,
            .base_dir = std::nullopt,
        },
    };
}

[[nodiscard]] std::string expand(
    std::string input,
    std::vector<coding_agent::Skill> skills = {},
    std::vector<coding_agent::PromptTemplate> templates = {},
    bool expand_templates = true) {
    return coding_agent::prompt::expand_prompt_input(
        std::move(input), skills, templates, expand_templates);
}

coding_agent::PromptTemplate make_template(std::string name, std::string content) {
    return coding_agent::PromptTemplate{
        .name = std::move(name),
        .description = std::nullopt,
        .content = std::move(content),
        .filePath = "/snapshot/" + name + ".md",
    };
}

} // namespace

TEST_CASE("prompt expansion returns ordinary and empty input unchanged", "[coding_agent][prompt][expansion]") {
    CHECK(expand("hello") == "hello");
    CHECK(expand("").empty());
}

TEST_CASE("prompt expansion bypasses skill and template expansion when requested", "[coding_agent][prompt][expansion]") {
    tests::TempWorkspace workspace;
    CHECK(expand(
              "/skill:cached",
              {make_skill_on_disk(workspace, "cached", "cached skill body")},
              {make_template("review", "Review: $1")},
              /*expand_templates=*/false) == "/skill:cached");
    CHECK(expand(
              "/review target",
              {},
              {make_template("review", "Review: $1")},
              /*expand_templates=*/false) == "/review target");
}

TEST_CASE("prompt expansion expands skills before templates", "[coding_agent][prompt][expansion]") {
    tests::TempWorkspace workspace;
    const auto skill = expand(
        "/skill:same raw instructions",
        {make_skill_on_disk(workspace, "same", "cached skill body")},
        {
            make_template("skill:same", "template won"),
            make_template("template", "template: $1"),
        });

    CHECK(skill.find("<skill name=\"same\"") != std::string::npos);
    CHECK(skill.find("cached skill body") != std::string::npos);
    CHECK(skill.find("</skill>\n\nraw instructions") != std::string::npos);
    CHECK(skill.find("template won") == std::string::npos);
}

TEST_CASE("prompt expansion reads the skill file at invocation time", "[coding_agent][prompt][expansion]") {
    tests::TempWorkspace workspace;
    auto skill = make_skill_on_disk(workspace, "late", "initial body");

    // The discovery record is unchanged, but the on-disk body is what
    // expands: write a new body after the snapshot was built.
    workspace.write(
        "skills/late/SKILL.md",
        "---\n"
        "name: late\n"
        "description: test skill\n"
        "---\n"
        "updated body\n");

    const auto expanded = expand("/skill:late", {std::move(skill)});
    CHECK(expanded.find("updated body") != std::string::npos);
    CHECK(expanded.find("initial body") == std::string::npos);
}

TEST_CASE("prompt expansion expands templates once and preserves unmatched slash input", "[coding_agent][prompt][expansion]") {
    CHECK(expand("/handoff\nfirst second third", {}, {make_template("handoff", "/quit $1 ${@:2}")}) ==
          "/quit first second third");

    for (const std::string input : {"/missing", "/", "!echo hi", "!!echo hi"}) {
        CHECK(expand(input) == input);
    }
}

TEST_CASE("prompt expansion ignores skill and template content that is not a slash invocation", "[coding_agent][prompt][expansion]") {
    tests::TempWorkspace workspace;
    const auto help = expand(
        "/help",
        {make_skill_on_disk(workspace, "skill-only", "skill")},
        {make_template("template-only", "template")});
    CHECK(help == "/help");
    CHECK(help.find("skill-only") == std::string::npos);
    CHECK(help.find("template-only") == std::string::npos);
}

TEST_CASE("prompt expansion treats slash input at column zero as potential skill or template", "[coding_agent][prompt][expansion]") {
    CHECK(expand(" /args", {}, {make_template("args", "expanded")}) == " /args");
    CHECK(expand("\t/args", {}, {make_template("args", "expanded")}) == "\t/args");
    CHECK(expand("/args", {}, {make_template("args", "expanded")}) == "expanded");
}

TEST_CASE("prompt expansion passes through unknown or unreadable skill invocation", "[coding_agent][prompt][expansion]") {
    tests::TempWorkspace workspace;
    auto skill = make_skill_on_disk(workspace, "missing-file", "body");

    // Unknown skill name passes through unchanged.
    CHECK(expand("/skill:unknown-skill") == "/skill:unknown-skill");
    // A discovery record whose file vanished passes through unchanged (pi
    // returns the original text on read failure).
    std::filesystem::remove(workspace.path() / "skills/missing-file/SKILL.md");
    CHECK(expand("/skill:missing-file", {std::move(skill)}) == "/skill:missing-file");
}
