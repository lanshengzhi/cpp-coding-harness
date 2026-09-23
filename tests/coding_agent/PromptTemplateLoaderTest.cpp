#include "coding_agent/PromptTemplateLoader.hpp"
#include "support/LegacyAsyncLoader.hpp"
#include "agent/harness/WorkspaceFileSystem.hpp"
#include "support/TempWorkspace.hpp"

#include <catch2/catch_test_macros.hpp>

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

TEST_CASE("loadPromptTemplateFromFile basic frontmatter", "[coding_agent][prompt][loader][spec]") {
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

TEST_CASE("loadPromptTemplateFromFile with argument-hint", "[coding_agent][prompt][loader][spec]") {
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

TEST_CASE("loadPromptTemplateFromFile no frontmatter", "[coding_agent][prompt][loader][spec]") {
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

TEST_CASE("loadPromptTemplateFromFile empty body", "[coding_agent][prompt][loader][spec]") {
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

TEST_CASE("loadPromptTemplateFromFile frontmatter only no body", "[coding_agent][prompt][loader][spec]") {
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

TEST_CASE("loadPromptTemplateFromFile file not found", "[coding_agent][prompt][loader][spec]") {
    LoaderTestFixture fix;
    auto result = coding_agent::loadPromptTemplateFromFile(fix.fs, "nonexistent.md");
    CHECK(result.templates.empty());
    REQUIRE(result.diagnostics.size() == 1);
    CHECK(result.diagnostics[0].code == coding_agent::PromptTemplateDiagnosticCode::read_failed);
    CHECK(result.diagnostics[0].path == "nonexistent.md");
}

TEST_CASE("loadPromptTemplateFromFile diagnoses non-md extension", "[coding_agent][prompt][loader][spec]") {
    LoaderTestFixture fix;
    fix.writeFile("notes.txt", "Some text content\n");
    auto result = coding_agent::loadPromptTemplateFromFile(fix.fs, "notes.txt");
    CHECK(result.templates.empty());
    REQUIRE(result.diagnostics.size() == 1);
    CHECK(result.diagnostics[0].code ==
          coding_agent::PromptTemplateDiagnosticCode::unsupported_type);
}

// ── loadPromptTemplates (directory) ──

TEST_CASE("loadPromptTemplates directory with multiple files", "[coding_agent][prompt][loader][spec]") {
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

TEST_CASE("loadPromptTemplates missing directory is silent", "[coding_agent][prompt][loader][spec]") {
    LoaderTestFixture fix;
    std::vector<coding_agent::PromptTemplateDirSpec> dirs = {{"nonexistent_dir"}};
    auto result = coding_agent::loadPromptTemplates(fix.fs, dirs);
    CHECK(result.templates.empty());
    CHECK(result.diagnostics.empty());
}

TEST_CASE("loadPromptTemplates keeps duplicate names for the loader dedupe",
        "[coding_agent][prompt][loader][issue405][spec]") {
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
    // pi `loadPromptTemplates` returns the raw list; the resource loader
    // deduplicates with pi-shaped collision diagnostics (winner/loser paths).
    REQUIRE(result.templates.size() == 2);
    CHECK(result.diagnostics.empty());
    for (const auto& tmpl : result.templates) {
        CHECK(tmpl.name == "greet");
        CHECK_FALSE(tmpl.filePath.empty());
    }
}

TEST_CASE("loadPromptTemplates explicit file path", "[coding_agent][prompt][loader][spec]") {
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

TEST_CASE("loadPromptTemplates parse failure produces diagnostic", "[coding_agent][prompt][loader][spec]") {
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

TEST_CASE("loadPromptTemplates empty directory", "[coding_agent][prompt][loader][spec]") {
    LoaderTestFixture fix;
    // Create the directory but no .md files
    fix.writeFile("prompts/.gitkeep", "");

    std::vector<coding_agent::PromptTemplateDirSpec> dirs = {{"prompts"}};
    auto result = coding_agent::loadPromptTemplates(fix.fs, dirs);
    CHECK(result.templates.empty());
    CHECK(result.diagnostics.empty());
}

TEST_CASE("loadPromptTemplates dotfile skipped", "[coding_agent][prompt][loader][spec]") {
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

TEST_CASE("loadPromptTemplates loads templates from absolute paths outside the workspace root",
        "[coding_agent][prompt][loader][issue700][spec]") {
    LoaderTestFixture fix;
    tests::TempWorkspace external;
    external.write("outside-file.md",
            "---\n"
            "description: Single-file template outside the workspace.\n"
            "---\n"
            "Outside file body.\n");
    external.write("outside-dir/nested.md",
            "---\n"
            "description: Directory template outside the workspace.\n"
            "---\n"
            "Outside dir body.\n");

    // ADR 0057: an explicit path resolved by the capability is honored
    // anywhere on the host; the resolved absolute path becomes the template's
    // filePath.
    std::vector<coding_agent::PromptTemplateDirSpec> dirs = {
            {.path = (external.path() / "outside-file.md").string(), .is_file = true},
            {.path = (external.path() / "outside-dir").string(), .is_file = false},
    };
    auto result = coding_agent::loadPromptTemplates(fix.fs, dirs);

    REQUIRE(result.templates.size() == 2);
    // Templates are name-sorted by the loader.
    CHECK(result.templates[0].name == "nested");
    CHECK(result.templates[0].filePath == (external.path() / "outside-dir" / "nested.md").string());
    CHECK(result.templates[1].name == "outside-file");
    CHECK(result.templates[1].filePath == (external.path() / "outside-file.md").string());
    CHECK(result.diagnostics.empty());
}

TEST_CASE("loadPromptTemplateFromFile rejects a non-string argument-hint", "[coding_agent][prompt][loader][spec]") {
    LoaderTestFixture fix;
    // pi `typeof frontmatter["argument-hint"] === "string"`: a number, a
    // boolean, and a flow sequence are all rejected (the template still
    // loads, without a hint and without a diagnostic).
    fix.writeFile("numeric.md",
            "---\n"
            "description: Numeric hint\n"
            "argument-hint: 42\n"
            "---\n"
            "Body\n");
    fix.writeFile("boolean.md",
            "---\n"
            "description: Boolean hint\n"
            "argument-hint: true\n"
            "---\n"
            "Body\n");
    fix.writeFile("sequence.md",
            "---\n"
            "description: Sequence hint\n"
            "argument-hint:\n"
            "  - a\n"
            "  - b\n"
            "---\n"
            "Body\n");

    for (const auto* name : {"numeric", "boolean", "sequence"}) {
        auto result = coding_agent::loadPromptTemplateFromFile(fix.fs, std::string{name} + ".md");
        REQUIRE(result.templates.size() == 1);
        CHECK_FALSE(result.templates[0].argument_hint.has_value());
        CHECK(result.templates[0].description.has_value());
        CHECK(result.diagnostics.empty());
    }
}

TEST_CASE("loadPromptTemplateFromFile accepts a quoted numeric argument-hint", "[coding_agent][prompt][loader][spec]") {
    LoaderTestFixture fix;
    fix.writeFile("quoted.md",
            "---\n"
            "description: Quoted hint\n"
            "argument-hint: \"42\"\n"
            "---\n"
            "Body\n");

    auto result = coding_agent::loadPromptTemplateFromFile(fix.fs, "quoted.md");

    // A quoted value is a YAML string and stays accepted.
    REQUIRE(result.templates.size() == 1);
    CHECK(result.templates[0].argument_hint == "42");
    CHECK(result.diagnostics.empty());
}

TEST_CASE("loadPromptTemplateFromFile surfaces pi-shaped read and parse diagnostics",
        "[coding_agent][prompt][loader][spec]") {
    LoaderTestFixture fix;
    // Read failure (pi: warning with the fs error message and the path).
    auto unreadable = coding_agent::loadPromptTemplateFromFile(fix.fs, "missing.md");
    CHECK(unreadable.templates.empty());
    REQUIRE(unreadable.diagnostics.size() == 1);
    CHECK(unreadable.diagnostics[0].type == "warning");
    CHECK(unreadable.diagnostics[0].code == coding_agent::PromptTemplateDiagnosticCode::read_failed);
    CHECK_FALSE(unreadable.diagnostics[0].message.empty());
    CHECK(unreadable.diagnostics[0].path == "missing.md");

    // Parse failure (pi: warning with the parse error message and the path).
    fix.writeFile("broken.md",
            "---\n"
            "this line has no colon\n"
            "---\n"
            "Body\n");
    auto unparseable = coding_agent::loadPromptTemplateFromFile(fix.fs, "broken.md");
    CHECK(unparseable.templates.empty());
    REQUIRE(unparseable.diagnostics.size() == 1);
    CHECK(unparseable.diagnostics[0].type == "warning");
    CHECK(unparseable.diagnostics[0].code == coding_agent::PromptTemplateDiagnosticCode::parse_failed);
    CHECK_FALSE(unparseable.diagnostics[0].message.empty());
    CHECK(unparseable.diagnostics[0].path == "broken.md");
}

TEST_CASE("loadPromptTemplateFromFile strips a UTF-8 BOM", "[coding_agent][prompt][loader][spec]") {
    LoaderTestFixture fix;
    fix.writeFile("bom.md",
            "\xEF\xBB\xBF"
            "---\n"
            "description: BOM-prefixed template\n"
            "argument-hint: \"<PR-URL>\"\n"
            "---\n"
            "Body after BOM\n");

    auto result = coding_agent::loadPromptTemplateFromFile(fix.fs, "bom.md");

    // Without the strip the BOM bytes would defeat the `---` prefix and the
    // template would load with no frontmatter at all.
    REQUIRE(result.templates.size() == 1);
    CHECK(result.templates[0].name == "bom");
    CHECK(result.templates[0].description == "BOM-prefixed template");
    CHECK(result.templates[0].argument_hint == "<PR-URL>");
    CHECK(result.templates[0].content == "Body after BOM");
    CHECK(result.diagnostics.empty());
}
