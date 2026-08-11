// P26 (#422): the System Prompt message-level differential golden. The
// committed snapshots under `fixtures/pi-coding-agent/prompts/` pin the
// FROZEN pi `buildSystemPrompt` output (baseline 83114817, message-level:
// a `system` message with one text content block) for the three scripted
// scenarios this file drives through the C++ `SystemPromptBuilder` with
// identical inputs. Each snapshot carries the `identityDelta`: the identity
// line and the documentation block in both forms (pi vs the C++ binary's own
// "cch"). This test swaps the delta regions into pi's message and byte-
// compares against the C++-built prompt, so the golden pins "structure byte-
// identical, identity lines swapped" (ADR 0036 G4 / #392) as a differential
// check — the only transformations between pi's message and the C++ prompt
// are exactly the two identity regions. The custom branch carries no identity
// regions and is pinned byte-identical with an empty delta.
#include "../../third_party/catch2/catch_test_macros.hpp"

#include "coding_agent/prompt/SystemPromptBuilder.hpp"

#include "util/Json.hpp"

#include <cch/util/JsonValue.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace cch;

namespace {

/// The scenario inputs must match the capture sidecar
/// (`fixtures/pi-coding-agent/capture/system-prompt-snapshots.mts`) and the
/// `SystemPromptBuilderTest` session shape byte-for-byte. The docs paths are
/// the scrubbed `/cch/*` identity forms; pi's side resolves the scrubbed
/// `/pi/*` paths through `PI_PACKAGE_DIR=/pi`.
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
    options.readmePath = "/cch/README.md";
    options.docsPath = "/cch/docs";
    options.examplesPath = "/cch/examples";
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

// ── Snapshot I/O ────────────────────────────────────────────────────────────

[[nodiscard]] std::filesystem::path snapshot_path(std::string_view name) {
    return std::filesystem::path{CCH_SOURCE_DIR} /
           "fixtures/pi-coding-agent/prompts" /
           ("system-prompt-" + std::string{name} + "-message.json");
}

[[nodiscard]] util::Expected<util::JsonValue> read_snapshot(std::string_view name) {
    const auto path = snapshot_path(name);
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Unknown,
            "Failed to open snapshot: " + path.string()));
    }
    const std::string json{std::istreambuf_iterator<char>{input},
                           std::istreambuf_iterator<char>{}};
    return util::read_json(json);
}

/// Navigates `object[key]` and requires a string value.
[[nodiscard]] std::string required_string(
    const util::JsonValue::object_t& object,
    std::string_view key) {
    const auto it = object.find(std::string{key});
    REQUIRE(it != object.end());
    return it->second.get<std::string>();
}

/// Navigates `object[key]` and requires an object value.
[[nodiscard]] const util::JsonValue::object_t& required_object(
    const util::JsonValue::object_t& object,
    std::string_view key) {
    const auto it = object.find(std::string{key});
    REQUIRE(it != object.end());
    const auto* nested = it->second.get_if<util::JsonValue::object_t>();
    REQUIRE(nested != nullptr);
    return *nested;
}

/// Replaces every occurrence of `needle` in `haystack`.
[[nodiscard]] std::string replace_all(
    std::string haystack,
    std::string_view needle,
    std::string_view replacement) {
    REQUIRE(!needle.empty());
    std::size_t position = 0;
    while ((position = haystack.find(needle, position)) != std::string::npos) {
        haystack.replace(position, needle.size(), replacement);
        position += replacement.size();
    }
    return haystack;
}

// ── Scenario runners ────────────────────────────────────────────────────────

/// Pins one scenario: loads pi's message from the committed snapshot, swaps
/// the identity-delta regions (pi -> cch) into pi's text, and byte-compares
/// the result against the C++-built prompt.
void check_message_golden(
    std::string_view scenario,
    const coding_agent::prompt::BuildSystemPromptOptions& options) {
    const auto snapshot = read_snapshot(scenario);
    REQUIRE(snapshot.has_value());
    const auto* root = snapshot->get_if<util::JsonValue::object_t>();
    REQUIRE(root != nullptr);

    // Pinned baseline citation (same meta contract as the session suites).
    const auto& meta = required_object(*root, "meta");
    CHECK(required_string(meta, "baseline") ==
          "83114817c68f5413e4d7ba6d7003ddc511cd31d2");
    CHECK(required_string(meta, "artifact") ==
          "@earendil-works/pi-coding-agent@0.83.0");
    CHECK(required_string(meta, "family") ==
          "system-prompt-message-" + std::string{scenario});

    // Message-level shape: a `system` message with one text content block.
    const auto& message = required_object(*root, "message");
    CHECK(required_string(message, "role") == "system");
    const auto& content = message.find("content");
    REQUIRE(content != message.end());
    const auto* blocks = content->second.get_if<util::JsonValue::array_t>();
    REQUIRE(blocks != nullptr);
    REQUIRE(blocks->size() == 1);
    const auto* block = (*blocks)[0].get_if<util::JsonValue::object_t>();
    REQUIRE(block != nullptr);
    CHECK(required_string(*block, "type") == "text");
    const auto pi_text = required_string(*block, "text");

    const auto cch_prompt = coding_agent::prompt::buildSystemPrompt(options);

    const auto& identity_delta = required_object(*root, "identityDelta");
    if (identity_delta.empty()) {
        // Custom branch: no identity regions; the two sides are byte-identical.
        CHECK(pi_text == cch_prompt);
        return;
    }

    // Default branch: swap the identity regions (pi -> cch) into pi's text.
    const auto& identity_line = required_object(identity_delta, "identityLine");
    const auto& docs_block = required_object(identity_delta, "docsBlock");
    const auto pi_identity = required_string(identity_line, "pi");
    const auto cch_identity = required_string(identity_line, "cch");
    const auto pi_docs = required_string(docs_block, "pi");
    const auto cch_docs = required_string(docs_block, "cch");

    // The delta regions are present in pi's message (sanity).
    REQUIRE(pi_text.find(pi_identity) != std::string::npos);
    REQUIRE(pi_text.find(pi_docs) != std::string::npos);
    // The cch forms carry the C++ binary's own identity, not pi's.
    CHECK(cch_identity.find("inside cch,") != std::string::npos);
    CHECK(cch_docs.find("cch documentation") != std::string::npos);
    CHECK(cch_docs.find("/cch/") != std::string::npos);

    auto swapped = replace_all(pi_text, pi_identity, cch_identity);
    swapped = replace_all(swapped, pi_docs, cch_docs);
    CHECK(swapped == cch_prompt);
}

} // namespace

TEST_CASE("system prompt default message golden: pi structure, cch identity "
          "line and docs block swapped",
          "[coding_agent][prompt][golden][issue422]") {
    auto options = session_shape_options();
    options.skills = {dummy_skill()};
    check_message_golden("default", options);
}

TEST_CASE("system prompt custom message golden: byte-identical to pi with no "
          "identity delta",
          "[coding_agent][prompt][golden][issue422]") {
    auto options = session_shape_options();
    // pi truthy customPrompt: the custom branch, no selectedTools (mirrors
    // the TS side, which passes none).
    options.selectedTools = std::nullopt;
    options.toolSnippets.clear();
    options.promptGuidelines.clear();
    options.customPrompt = "You are a custom assistant.";
    options.appendSystemPrompt = "Custom append section.";
    options.contextFiles = {
        {"AGENTS.md",
         "Project-specific instructions and guidelines:\n\nBe careful."},
    };
    options.skills = {dummy_skill()};
    check_message_golden("custom", options);
}

TEST_CASE("system prompt empty-tools message golden: default branch with the "
          "empty tool set and the identity delta",
          "[coding_agent][prompt][golden][issue422]") {
    auto options = session_shape_options();
    options.selectedTools = std::vector<std::string>{};
    options.toolSnippets.clear();
    options.skills.clear();
    check_message_golden("empty-tools", options);
}
