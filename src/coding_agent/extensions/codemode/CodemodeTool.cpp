#include "coding_agent/extensions/codemode/CodemodeTool.hpp"

#include "coding_agent/extensions/codemode/CodemodeCatalog.hpp"

#include <cch/codemode/Source.hpp>

#include <string>
#include <utility>
#include <vector>

namespace cch::coding_agent::extensions {

std::vector<std::string> codemode_prompt_guidelines() {
    return {
            "Use codemode to batch independent tool calls (Promise.allSettled), chain them, or filter large output, "
            "instead of many separate calls.",
    };
}

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
    // pi `createCodemodeToolDefinition` starts from
    // `createCodemodeDescription([], { models })`: an empty catalog still
    // declares the globals and the options grammar. The session's catalog
    // refresh replaces this description with the live loadout.
    tool.description = create_codemode_description({}, CodemodeCatalogSettings{});
    tool.parameters = codemode_parameters();
    tool.constrained_sampling = ai::ConstrainedSampling{
            .variants = {{"openai_lark", std::string{codemode::kCodemodeSourceGrammar}}},
    };
    return tool;
}

} // namespace cch::coding_agent::extensions
