#include "coding_agent/prompt/SystemPromptBuilder.hpp"
#include "support/TempWorkspace.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

using namespace cch;

namespace {

/// Committed golden fixtures pin the identity delta byte-for-byte (ADR 0036
/// G4): the System Prompt structure is pi's `core/system-prompt.ts`
/// `buildSystemPrompt` at baseline 83114817 with only the identity line and
/// the documentation block swapped for the C++ binary's own ("pike") identity
/// and docs paths. Paths inside the goldens are scrubbed dummy values.
[[nodiscard]] std::filesystem::path fixture_path(std::string_view name) {
    return std::filesystem::path{CCH_SOURCE_DIR} /
           "tests/fixtures/prompts/goldens" / name;
}

[[nodiscard]] std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string{
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{}};
}

/// Writes the observed prompt to the committed golden when
/// CCH_CAPTURE_GOLDENS=1.
void capture_golden(std::string_view name, const std::string& prompt) {
    if (std::getenv("CCH_CAPTURE_GOLDENS") == nullptr) return;
    std::ofstream output(fixture_path(name), std::ios::binary);
    output << prompt;
}

/// Byte-compares the built prompt against the committed golden.
void check_golden(std::string_view golden_name, const std::string& prompt) {
    capture_golden(golden_name, prompt);
    CHECK(prompt == read_text_file(fixture_path(golden_name)));
}

/// pi `core/tools/*.ts` prompt metadata for the four fixed tools, verbatim.
[[nodiscard]] coding_agent::prompt::BuildSystemPromptOptions session_shape_options(
    std::string cwd = "/tmp/workspace") {
    coding_agent::prompt::BuildSystemPromptOptions options;
    options.selectedTools =
        std::vector<std::string>{"read", "bash", "edit", "write"};
    options.toolSnippets = {
        {"read", "Read file contents"},
        {"bash", "Execute bash commands (ls, grep, find, etc.)"},
        {"edit",
         "Make precise file edits with exact text replacement, including "
         "multiple disjoint edits in one call"},
        {"write", "Create or overwrite files"},
    };
    options.promptGuidelines = {
        "Use read to examine files instead of cat or sed.",
        "Inspect PI_* environment variables for current model and session "
        "details.",
        "Use edit for precise changes (edits[].oldText must match exactly)",
        "When changing multiple separate locations in one file, use one edit "
        "call with multiple entries in edits[] instead of multiple edit calls",
        "Each edits[].oldText is matched against the original file, not after "
        "earlier edits are applied. Do not emit overlapping or nested edits. "
        "Merge nearby changes into one edit.",
        "Keep edits[].oldText as small as possible while still being unique "
        "in the file. Do not pad with large unchanged regions.",
        "Use write only for new files or complete rewrites.",
    };
    options.cwd = std::move(cwd);
    // Identity delta: the C++ binary's own docs paths (scrubbed in goldens).
    options.readmePath = "/pike/README.md";
    options.docsPath = "/pike/docs";
    options.examplesPath = "/pike/examples";
    return options;
}

[[nodiscard]] coding_agent::Skill dummy_skill() {
    return coding_agent::Skill{
        .name = "my-skill",
        .description = "Do things.",
        .filePath = "/home/user/.agents/skills/my-skill/SKILL.md",
        .baseDir = "/home/user/.agents/skills/my-skill",
        .sourceInfo = coding_agent::SourceInfo{
            .path = "/home/user/.agents/skills/my-skill/SKILL.md",
            .source = "auto",
            .scope = coding_agent::SourceScope::User,
            .origin = coding_agent::SourceOrigin::TopLevel,
            .base_dir = std::nullopt,
        },
    };
}

} // namespace

TEST_CASE("system prompt default branch matches the Pike identity golden", "[coding_agent][prompt][system-prompt]") {
    auto options = session_shape_options();
    options.skills = {dummy_skill()};
    check_golden("system-prompt-default.txt", coding_agent::prompt::buildSystemPrompt(options));
}

TEST_CASE("system prompt custom branch matches the Pike identity golden", "[coding_agent][prompt][system-prompt]") {
    auto options = session_shape_options();
    options.customPrompt = "You are a custom assistant.";
    options.appendSystemPrompt = "Custom append section.";
    options.contextFiles = {
        {"AGENTS.md", "Project-specific instructions and guidelines:\n\nBe careful."},
    };
    options.skills = {dummy_skill()};
    check_golden("system-prompt-custom.txt", coding_agent::prompt::buildSystemPrompt(options));
}

TEST_CASE(
        "system prompt pi-truthy edge cases match the Pike identity goldens", "[coding_agent][prompt][system-prompt]") {
    // pi `if (customPrompt)`: an empty custom prompt is falsy and takes the
    // default branch — byte-identical to the default golden.
    auto empty_custom = session_shape_options();
    empty_custom.customPrompt = "";
    empty_custom.skills = {dummy_skill()};
    check_golden("system-prompt-default.txt", coding_agent::prompt::buildSystemPrompt(empty_custom));

    // pi `selectedTools || defaults`: an explicitly empty list keeps no tools
    // (no bash exploration rule, no read gate, no skills); the tool
    // guidelines still render.
    auto empty_tools = session_shape_options();
    empty_tools.selectedTools = std::vector<std::string>{};
    empty_tools.toolSnippets.clear();
    empty_tools.skills.clear();
    check_golden("system-prompt-empty-tools.txt", coding_agent::prompt::buildSystemPrompt(empty_tools));
}

TEST_CASE("system prompt renders (none) when no tool snippets are known", "[coding_agent][prompt][system-prompt]") {
    auto options = session_shape_options();
    options.toolSnippets.clear();
    const auto prompt = coding_agent::prompt::buildSystemPrompt(options);

    CHECK(prompt.find("Available tools:\n(none)\n") != std::string::npos);
    // The default tool names still apply, so the bash exploration rule holds.
    CHECK(prompt.find("- Use bash for file operations like ls, rg, find\n") != std::string::npos);
}

TEST_CASE("system prompt renders the tools list only for tools with snippets", "[coding_agent][prompt][system-prompt]") {
    auto options = session_shape_options();
    options.toolSnippets = {{"read", "Read file contents"}};
    const auto prompt = coding_agent::prompt::buildSystemPrompt(options);

    CHECK(prompt.find("Available tools:\n- read: Read file contents\n\n") != std::string::npos);
    CHECK(prompt.find("- bash:") == std::string::npos);
    // bash is still selected, so the exploration rule is present.
    CHECK(prompt.find("- Use bash for file operations like ls, rg, find\n") != std::string::npos);
}

TEST_CASE("system prompt dedupes and trims guideline bullets", "[coding_agent][prompt][system-prompt]") {
    auto options = session_shape_options();
    options.toolSnippets.clear();
    options.promptGuidelines = {
        "  duplicate bullet  ",
        "duplicate bullet",
        "   ",
        "Be concise in your responses",
    };
    const auto prompt = coding_agent::prompt::buildSystemPrompt(options);

    const auto guidelines_at = prompt.find("Guidelines:\n");
    REQUIRE(guidelines_at != std::string::npos);
    const auto section = prompt.substr(guidelines_at);
    // One bullet each, deduped; the always-lines land exactly once.
    CHECK(section.find("- duplicate bullet\n") != std::string::npos);
    CHECK(section.find("- Be concise in your responses\n") != std::string::npos);
    CHECK(section.find("  duplicate bullet  ") == std::string::npos);
    const auto first_concise = section.find("- Be concise in your responses\n");
    const auto second_concise = section.find("- Be concise in your responses\n", first_concise + 1);
    CHECK(second_concise == std::string::npos);
}

TEST_CASE("system prompt suppresses the bash exploration rule when grep/find/ls are selected", "[coding_agent][prompt][system-prompt]") {
    for (const std::string extra : {"grep", "find", "ls"}) {
        auto options = session_shape_options();
        options.selectedTools = std::vector<std::string>{"read", "bash", extra};
        options.toolSnippets = {{"read", "Read file contents"}};
        const auto prompt = coding_agent::prompt::buildSystemPrompt(options);
        CHECK(prompt.find("Use bash for file operations like ls, rg, find") == std::string::npos);
    }
}

TEST_CASE("system prompt appends the append section after the documentation block", "[coding_agent][prompt][system-prompt]") {
    auto options = session_shape_options();
    options.appendSystemPrompt = "APPENDED";
    const auto prompt = coding_agent::prompt::buildSystemPrompt(options);

    const auto docs_at = prompt.find("TUI API details)");
    REQUIRE(docs_at != std::string::npos);
    CHECK(prompt.find("\n\nAPPENDED", docs_at) == docs_at + 16);
}

TEST_CASE("system prompt renders project context files in both branches", "[coding_agent][prompt][system-prompt]") {
    const coding_agent::prompt::ProjectContextFile file{
        "AGENTS.md", "Be careful.\n\nNo trailing newline"};
    auto options = session_shape_options();
    options.contextFiles = {file};

    const auto default_prompt = coding_agent::prompt::buildSystemPrompt(options);
    CHECK(default_prompt.find(
              "<project_context>\n\n"
              "Project-specific instructions and guidelines:\n\n"
              "<project_instructions path=\"AGENTS.md\">\n"
              "Be careful.\n\nNo trailing newline\n"
              "</project_instructions>\n\n"
              "</project_context>\n") != std::string::npos);

    options.customPrompt = "custom";
    const auto custom_prompt = coding_agent::prompt::buildSystemPrompt(options);
    CHECK(custom_prompt.find(
              "<project_instructions path=\"AGENTS.md\">\n"
              "Be careful.\n\nNo trailing newline\n"
              "</project_instructions>") != std::string::npos);
    CHECK(custom_prompt.find("custom\n\n<project_context>") != std::string::npos);
}

TEST_CASE("system prompt gates the skills section on the read tool", "[coding_agent][prompt][system-prompt]") {
    const auto skill = dummy_skill();

    // Default branch: skills render only when read is selected.
    auto default_with_read = session_shape_options();
    default_with_read.skills = {skill};
    CHECK(coding_agent::prompt::buildSystemPrompt(default_with_read)
              .find("<available_skills>") != std::string::npos);

    auto default_without_read = session_shape_options();
    default_without_read.selectedTools = std::vector<std::string>{"bash"};
    default_without_read.toolSnippets = {{"bash", "Execute bash commands"}};
    default_without_read.skills = {skill};
    CHECK(coding_agent::prompt::buildSystemPrompt(default_without_read)
              .find("<available_skills>") == std::string::npos);

    // Custom branch: pi `!selectedTools || selectedTools.includes("read")`.
    auto custom_without_read = session_shape_options();
    custom_without_read.selectedTools = std::vector<std::string>{"bash"};
    custom_without_read.toolSnippets = {{"bash", "Execute bash commands"}};
    custom_without_read.customPrompt = "custom";
    custom_without_read.skills = {skill};
    CHECK(coding_agent::prompt::buildSystemPrompt(custom_without_read)
              .find("<available_skills>") == std::string::npos);

    auto custom_no_tools = session_shape_options();
    // pi `!selectedTools`: absent means read is assumed available.
    custom_no_tools.selectedTools = std::nullopt;
    custom_no_tools.customPrompt = "custom";
    custom_no_tools.skills = {skill};
    CHECK(coding_agent::prompt::buildSystemPrompt(custom_no_tools)
              .find("<available_skills>") != std::string::npos);
}

TEST_CASE("system prompt distinguishes absent from explicitly empty tool selections", "[coding_agent][prompt][system-prompt]") {
    const auto skill = dummy_skill();

    // pi `selectedTools || defaults`: an explicitly empty list keeps no tools
    // (no bash exploration rule, no skills through the read gate), while an
    // absent selection applies the pi default names.
    auto empty_selection = session_shape_options();
    empty_selection.selectedTools = std::vector<std::string>{};
    empty_selection.skills = {skill};
    const auto empty_prompt = coding_agent::prompt::buildSystemPrompt(empty_selection);
    CHECK(empty_prompt.find("Available tools:\n(none)\n") != std::string::npos);
    CHECK(empty_prompt.find("Use bash for file operations like ls, rg, find") == std::string::npos);
    CHECK(empty_prompt.find("<available_skills>") == std::string::npos);

    auto absent_selection = session_shape_options();
    absent_selection.selectedTools = std::nullopt;
    absent_selection.toolSnippets.clear();
    absent_selection.skills = {skill};
    const auto absent_prompt = coding_agent::prompt::buildSystemPrompt(absent_selection);
    // Default names apply: the bash exploration rule and read-gated skills.
    CHECK(absent_prompt.find("- Use bash for file operations like ls, rg, find\n") != std::string::npos);
    CHECK(absent_prompt.find("<available_skills>") != std::string::npos);
}

TEST_CASE("system prompt treats an empty custom prompt as absent like pi's truthy check", "[coding_agent][prompt][system-prompt]") {
    auto options = session_shape_options();
    options.customPrompt = "";
    const auto prompt = coding_agent::prompt::buildSystemPrompt(options);

    // pi `if (customPrompt)`: the empty string is falsy → default branch.
    CHECK(prompt.find("Available tools:") != std::string::npos);
    CHECK(prompt.find("pike documentation") != std::string::npos);
}

TEST_CASE("system prompt renders the skills section in pi's shape", "[coding_agent][prompt][system-prompt]") {
    auto options = session_shape_options();
    options.skills = {dummy_skill()};
    const auto prompt = coding_agent::prompt::buildSystemPrompt(options);

    CHECK(prompt.find(
              "\n\nThe following skills provide specialized instructions for "
              "specific tasks.\n"
              "Use the read tool to load a skill's file when the task matches "
              "its description.\n") != std::string::npos);
    CHECK(prompt.find(
              "  <skill>\n"
              "    <name>my-skill</name>\n"
              "    <description>Do things.</description>\n"
              "    <location>/home/user/.agents/skills/my-skill/SKILL.md"
              "</location>\n"
              "  </skill>\n"
              "</available_skills>") != std::string::npos);
}

TEST_CASE("system prompt posix-normalizes the cwd and keeps the trailing line", "[coding_agent][prompt][system-prompt]") {
    auto options = session_shape_options("C:\\workspace\\path");
    const auto prompt = coding_agent::prompt::buildSystemPrompt(options);

    CHECK(prompt.find("\nCurrent working directory: C:/workspace/path") != std::string::npos);
    CHECK(prompt.find("C:\\workspace") == std::string::npos);
}

TEST_CASE("system prompt custom branch omits the default sections", "[coding_agent][prompt][system-prompt]") {
    auto options = session_shape_options();
    options.customPrompt = "Only this.";
    const auto prompt = coding_agent::prompt::buildSystemPrompt(options);

    CHECK(prompt == "Only this.\nCurrent working directory: /tmp/workspace");
    CHECK(prompt.find("Available tools:") == std::string::npos);
    CHECK(prompt.find("Guidelines:") == std::string::npos);
    CHECK(prompt.find("pike documentation") == std::string::npos);
}
