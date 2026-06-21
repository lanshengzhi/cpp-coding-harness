#include "../../third_party/catch2/catch_test_macros.hpp"

#include "coding_agent/PromptTemplateLoader.hpp"
#include "harness/WorkspaceFileSystem.hpp"
#include "../support/TempWorkspace.hpp"

#include <string>

using namespace cch;

namespace {

struct LoaderTestFixture {
    tests::TempWorkspace workspace;
    harness::WorkspaceFileSystem fs;

    LoaderTestFixture() {
        auto result = harness::WorkspaceFileSystem::create(workspace.path());
        REQUIRE(result);
        fs = std::move(*result);
    }

    void writeFile(const std::string& relativePath, const std::string& content) {
        workspace.write(relativePath, content);
    }
};

} // namespace

// ── loadPromptTemplateFromFile ──

TEST_CASE("loadPromptTemplateFromFile basic frontmatter", "[coding_agent][prompt][loader]") {
    LoaderTestFixture fix;
    fix.writeFile("greet.md",
        "---\n"
        "description: Send a greeting\n"
        "---\n"
        "Hello $1, welcome!\n");

    auto result = coding_agent::loadPromptTemplateFromFile(fix.fs, "greet.md");
    REQUIRE(result.templates.size() == 1);
    CHECK(result.diagnostics.empty());

    const auto& tmpl = result.templates[0];
    CHECK(tmpl.name == "greet");
    CHECK(tmpl.description == "Send a greeting");
    CHECK(tmpl.content == "Hello $1, welcome!");
    CHECK_FALSE(tmpl.argument_hint.has_value());
}

TEST_CASE("loadPromptTemplateFromFile with argument-hint", "[coding_agent][prompt][loader]") {
    LoaderTestFixture fix;
    fix.writeFile("review.md",
        "---\n"
        "description: Review staged changes\n"
        "argument-hint: \"<PR-URL>\"\n"
        "---\n"
        "Review the staged changes.\n");

    auto result = coding_agent::loadPromptTemplateFromFile(fix.fs, "review.md");
    REQUIRE(result.templates.size() == 1);
    CHECK(result.diagnostics.empty());

    const auto& tmpl = result.templates[0];
    CHECK(tmpl.name == "review");
    CHECK(tmpl.description == "Review staged changes");
    CHECK(tmpl.argument_hint == "<PR-URL>");
    CHECK(tmpl.content == "Review the staged changes.");
}

TEST_CASE("loadPromptTemplateFromFile no frontmatter", "[coding_agent][prompt][loader]") {
    LoaderTestFixture fix;
    fix.writeFile("plain.md", "Just a plain template with $1 and $2.\n");

    auto result = coding_agent::loadPromptTemplateFromFile(fix.fs, "plain.md");
    REQUIRE(result.templates.size() == 1);
    CHECK(result.diagnostics.empty());

    const auto& tmpl = result.templates[0];
    CHECK(tmpl.name == "plain");
    CHECK_FALSE(tmpl.description.has_value());
    CHECK(tmpl.content == "Just a plain template with $1 and $2.");
}

TEST_CASE("loadPromptTemplateFromFile empty body", "[coding_agent][prompt][loader]") {
    LoaderTestFixture fix;
    fix.writeFile("empty.md",
        "---\n"
        "description: Empty body\n"
        "---\n");

    auto result = coding_agent::loadPromptTemplateFromFile(fix.fs, "empty.md");
    REQUIRE(result.templates.size() == 1);
    CHECK(result.diagnostics.empty());

    const auto& tmpl = result.templates[0];
    CHECK(tmpl.name == "empty");
    CHECK(tmpl.content.empty());
}

TEST_CASE("loadPromptTemplateFromFile frontmatter only no body", "[coding_agent][prompt][loader]") {
    LoaderTestFixture fix;
    fix.writeFile("meta.md",
        "---\n"
        "description: Metadata only\n"
        "---");

    auto result = coding_agent::loadPromptTemplateFromFile(fix.fs, "meta.md");
    REQUIRE(result.templates.size() == 1);

    const auto& tmpl = result.templates[0];
    CHECK(tmpl.name == "meta");
    CHECK(tmpl.description == "Metadata only");
    // body should be empty or whitespace-only
    CHECK(tmpl.content.find_first_not_of(" \t\n\r") == std::string::npos);
}

TEST_CASE("loadPromptTemplateFromFile file not found", "[coding_agent][prompt][loader]") {
    LoaderTestFixture fix;
    auto result = coding_agent::loadPromptTemplateFromFile(fix.fs, "nonexistent.md");
    CHECK(result.templates.empty());
    REQUIRE(result.diagnostics.size() == 1);
    CHECK(result.diagnostics[0].code == coding_agent::PromptTemplateDiagnosticCode::read_failed);
    CHECK(result.diagnostics[0].path == "nonexistent.md");
}

TEST_CASE("loadPromptTemplateFromFile non-md extension skipped", "[coding_agent][prompt][loader]") {
    LoaderTestFixture fix;
    fix.writeFile("notes.txt", "Some text content\n");
    auto result = coding_agent::loadPromptTemplateFromFile(fix.fs, "notes.txt");
    // Non-.md files from directory scans should be skipped by the directory loader,
    // but explicit file loading should still attempt to load them.
    // The current implementation loads any file path passed explicitly.
    CHECK(result.templates.empty());
}

// ── loadPromptTemplates (directory) ──

TEST_CASE("loadPromptTemplates directory with multiple files", "[coding_agent][prompt][loader]") {
    LoaderTestFixture fix;
    fix.writeFile("prompts/greet.md",
        "---\n"
        "description: Greeting\n"
        "---\n"
        "Hello $1!\n");
    fix.writeFile("prompts/review.md",
        "---\n"
        "description: Review\n"
        "---\n"
        "Review: $@\n");
    fix.writeFile("prompts/notes.txt", "not a template\n");

    std::vector<coding_agent::PromptTemplateDirSpec> dirs = {{"prompts"}};
    auto result = coding_agent::loadPromptTemplates(fix.fs, dirs);
    REQUIRE(result.templates.size() == 2);
    CHECK(result.diagnostics.empty());

    // Templates should be sorted by name
    CHECK(result.templates[0].name == "greet");
    CHECK(result.templates[1].name == "review");
}

TEST_CASE("loadPromptTemplates missing directory is silent", "[coding_agent][prompt][loader]") {
    LoaderTestFixture fix;
    std::vector<coding_agent::PromptTemplateDirSpec> dirs = {{"nonexistent_dir"}};
    auto result = coding_agent::loadPromptTemplates(fix.fs, dirs);
    CHECK(result.templates.empty());
    CHECK(result.diagnostics.empty());
}

TEST_CASE("loadPromptTemplates duplicate name dedup", "[coding_agent][prompt][loader]") {
    LoaderTestFixture fix;
    fix.writeFile("prompts/greet.md",
        "---\n"
        "description: First\n"
        "---\n"
        "First body\n");
    // Create a subdirectory with a duplicate name
    fix.writeFile("more/greet.md",
        "---\n"
        "description: Second\n"
        "---\n"
        "Second body\n");

    std::vector<coding_agent::PromptTemplateDirSpec> dirs = {{"prompts"}, {"more"}};
    auto result = coding_agent::loadPromptTemplates(fix.fs, dirs);
    REQUIRE(result.templates.size() == 1);
    CHECK(result.templates[0].name == "greet");
    CHECK(result.templates[0].description == "First"); // first wins

    REQUIRE(result.diagnostics.size() == 1);
    CHECK(result.diagnostics[0].code == coding_agent::PromptTemplateDiagnosticCode::duplicate_name);
}

TEST_CASE("loadPromptTemplates explicit file path", "[coding_agent][prompt][loader]") {
    LoaderTestFixture fix;
    fix.writeFile("custom.md",
        "---\n"
        "description: Custom template\n"
        "---\n"
        "Custom: $@\n");

    std::vector<coding_agent::PromptTemplateDirSpec> dirs = {{"custom.md", true}};
    auto result = coding_agent::loadPromptTemplates(fix.fs, dirs);
    REQUIRE(result.templates.size() == 1);
    CHECK(result.templates[0].name == "custom");
    CHECK(result.templates[0].description == "Custom template");
}

TEST_CASE("loadPromptTemplates parse failure produces diagnostic", "[coding_agent][prompt][loader]") {
    LoaderTestFixture fix;
    fix.writeFile("prompts/bad.md",
        "---\n"
        "bad line without colon\n"
        "---\n"
        "Body\n");

    std::vector<coding_agent::PromptTemplateDirSpec> dirs = {{"prompts"}};
    auto result = coding_agent::loadPromptTemplates(fix.fs, dirs);
    CHECK(result.templates.empty());
    REQUIRE(result.diagnostics.size() == 1);
    CHECK(result.diagnostics[0].code == coding_agent::PromptTemplateDiagnosticCode::parse_failed);
}

TEST_CASE("loadPromptTemplates empty directory", "[coding_agent][prompt][loader]") {
    LoaderTestFixture fix;
    // Create the directory but no .md files
    fix.writeFile("prompts/.gitkeep", "");

    std::vector<coding_agent::PromptTemplateDirSpec> dirs = {{"prompts"}};
    auto result = coding_agent::loadPromptTemplates(fix.fs, dirs);
    CHECK(result.templates.empty());
    CHECK(result.diagnostics.empty());
}

TEST_CASE("loadPromptTemplates dotfile skipped", "[coding_agent][prompt][loader]") {
    LoaderTestFixture fix;
    fix.writeFile("prompts/.hidden.md",
        "---\n"
        "description: Hidden\n"
        "---\n"
        "Should be skipped\n");

    std::vector<coding_agent::PromptTemplateDirSpec> dirs = {{"prompts"}};
    auto result = coding_agent::loadPromptTemplates(fix.fs, dirs);
    CHECK(result.templates.empty());
}
