#include "coding_agent/extensions/codemode/CodemodeTool.hpp"

#include "coding_agent/extensions/codemode/CodemodeSource.hpp"

#include <string>
#include <utility>

namespace cch::coding_agent::extensions {

namespace {

/// pi `DESCRIPTION_INTRO` (`packages/coding-agent/src/extensions/codemode/tool.ts`), verbatim.
constexpr std::string_view kDescriptionIntro =
        "Run JavaScript that calls other tools. The input is raw JavaScript (not JSON, no code fence), run as an "
        "async function body in a QuickJS sandbox: top-level `await` and `return` work. No Node, file system, "
        "network, or timers.\n"
        "- `await tools.<name>({ ...args })` resolves to a string, or an object if the tool's declaration says so, "
        "and rejects with an Error on failure. Calls still running when the script ends are cancelled.\n"
        "- Optional first line: `// @options: {\"max_output_tokens\": 10000, \"timeout_ms\": 60000}`";

/// pi `describeGlobals(false)` (`tool.ts`), verbatim.
constexpr std::string_view kGlobals =
        "Globals:\n"
        "- `text(value)`, `image(dataUrlOrImageBlock)`, `console.log(...)`, and top-level `return` add output; "
        "`exit()` ends the script. `image()` also saves the image to a temp file and the result names its path.\n"
        "- `store(key, value)` and `load(key)` keep JSON values across codemode calls.\n"
        "- `ALL_TOOLS`, `searchTools(query, { limit?, namespace? })`, `describeTool(name)`, "
        "`describeNamespace(name)`: find unlisted tools, such as MCP tools.";

} // namespace

std::vector<std::string> codemode_prompt_guidelines() {
    return {
            "Use codemode to batch independent tool calls (Promise.allSettled), chain them, or filter large output, "
            "instead of many separate calls.",
    };
}

std::string_view codemode_description_intro() { return kDescriptionIntro; }

std::string codemode_globals_text() { return std::string{kGlobals}; }

std::string codemode_description() { return std::string{kDescriptionIntro} + "\n\n" + std::string{kGlobals}; }

support::JsonValue codemode_parameters() {
    // pi `codemodeSchema` (TypeBox `Type.Object({ code: Type.String({...}) })`).
    return support::JsonValue::object_t{
            {"type", "object"},
            {"required", support::JsonValue::array_t{"code"}},
            {"properties",
                    support::JsonValue::object_t{
                            {"code",
                                    support::JsonValue::object_t{
                                            {"type", "string"},
                                            {"description", "Raw JavaScript source."},
                                    }},
                    }},
    };
}

ai::Tool codemode_tool_definition() {
    ai::Tool tool;
    tool.name = std::string{kCodemodeToolName};
    tool.description = codemode_description();
    tool.parameters = codemode_parameters();
    tool.constrained_sampling = ai::ConstrainedSampling{
            .variants = {{"openai_lark", std::string{kCodemodeSourceGrammar}}},
    };
    return tool;
}

} // namespace cch::coding_agent::extensions
