#include "CodemodeCatalog.hpp"

#include "coding_agent/extensions/codemode/CodemodeTool.hpp"

#include <cch/codemode/Declarations.hpp>
#include <cch/support/JsonValue.hpp>

#include "support/Json.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::coding_agent::extensions {

namespace {

using codemode::render_tool_output_type;
using codemode::render_tool_sample;
using codemode::to_codemode_identifier;
using support::JsonValue;

/// pi `DESCRIPTION_INTRO` (`extensions/codemode/tool.ts`), verbatim.
constexpr std::string_view kDescriptionIntro =
        "Run JavaScript that calls other tools. The input is raw JavaScript (not JSON, no code fence), run as an "
        "async function body in a QuickJS sandbox: top-level `await` and `return` work. No Node, file system, "
        "network, or timers.\n"
        "- `await tools.<name>({ ...args })` resolves to a string, or an object if the tool's declaration says so, "
        "and rejects with an Error on failure. A tool whose declaration returns an object resolves on failure too "
        "when it returned that object; check its fields. Calls still running when the script ends are cancelled.\n"
        "- Optional first line: `// @options: {\"max_output_tokens\": 10000, \"timeout_ms\": 60000}`";

/// pi `describeGlobals`: one line per global the sandbox binds. The `models`
/// line appears only when the session binds the `models` namespace; Pike
/// retains no codemode `models.*` provider execution, so its catalog never
/// advertises one.
[[nodiscard]] std::string describe_globals(bool models) {
    std::string globals =
            "Globals:\n"
            "- `text(value)`, `image(dataUrlOrImageBlock)`, `console.log(...)`, and top-level `return` add output; "
            "`exit()` ends the script. With several text items, each starts with a `==> text N/M <==` line, and "
            "`console` lines follow the other output in one `<console_output>` block. `image()` also saves the image "
            "to a temp file and the result names its path.\n"
            "- `store(key, value)` and `load(key)` keep JSON values across codemode calls.\n"
            "- `ALL_TOOLS`, `await searchTools(query, { limit?, namespace? })`, `await describeTool(name)`, "
            "`await describeNamespace(name)`: find unlisted tools, such as MCP tools.";
    if (models) {
        globals += "\n- `models`: classifiers and image generation. Read codemode.md first.";
    }
    return globals;
}

/// pi `CHARS_PER_TOKEN`: characters per token when estimating a section's cost.
constexpr std::size_t kCharsPerToken = 4;

[[nodiscard]] std::string join_sections(const std::vector<std::string>& sections) {
    std::string joined;
    for (const auto& section : sections) {
        if (!joined.empty()) joined += "\n\n";
        joined += section;
    }
    return joined;
}

[[nodiscard]] bool is_listable(ToolExposure exposure) {
    return exposure != ToolExposure::Deferred && exposure != ToolExposure::ModelOnly &&
           exposure != ToolExposure::Hidden;
}

[[nodiscard]] std::string trimmed(std::string_view text) {
    const auto not_space = [](unsigned char c) { return std::isspace(c) == 0; };
    const auto begin = std::find_if(text.begin(), text.end(), not_space);
    const auto end = std::find_if(text.rbegin(), text.rend(), not_space).base();
    return begin < end ? std::string{begin, end} : std::string{};
}

/// pi `renderToolSection`: `### \`id\` (\`raw name\`)` followed by the tool's
/// description and declaration.
[[nodiscard]] std::string render_tool_section(const CodemodeDeclaration& declaration) {
    const std::string id = to_codemode_identifier(declaration.name);
    const std::string heading =
            id == declaration.name ? "### `" + id + "`" : "### `" + id + "` (`" + declaration.name + "`)";
    ai::Tool sample_tool;
    sample_tool.name = declaration.name;
    sample_tool.description = declaration.description;
    sample_tool.parameters = declaration.input_schema;
    sample_tool.output_schema = declaration.output_schema;
    return heading + "\n" + trimmed(render_tool_sample(sample_tool));
}

/// pi `createCodemodeDescription`'s catalog entry: the rendered section and its
/// estimated token cost.
struct CatalogEntry {
    std::string name;
    std::string section;
    std::size_t cost{0};
};

struct CatalogGroup {
    std::optional<ToolNamespace> name_space{};
    std::vector<CatalogEntry> entries{};
};

/// pi `selectCatalog`: pick the sections that fit the budget like OpenCode's
/// catalog. In each round every group (tools without a namespace first, then
/// namespaces by name) places its cheapest remaining tool; a group whose next
/// tool does not fit drops out while the others continue.
[[nodiscard]] std::set<std::string, std::less<>> select_catalog(
        const std::vector<CatalogGroup>& groups, std::optional<std::int64_t> budget) {
    if (!budget.has_value()) {
        std::set<std::string, std::less<>> all;
        for (const auto& group : groups) {
            for (const auto& entry : group.entries)
                all.insert(entry.name);
        }
        return all;
    }
    std::vector<std::vector<CatalogEntry>> queues;
    queues.reserve(groups.size());
    for (const auto& group : groups) {
        std::vector<CatalogEntry> queue = group.entries;
        std::stable_sort(queue.begin(), queue.end(), [](const CatalogEntry& left, const CatalogEntry& right) {
            return left.cost < right.cost;
        });
        queues.push_back(std::move(queue));
    }
    std::set<std::string, std::less<>> shown;
    std::int64_t remaining = *budget;
    std::vector<std::size_t> active;
    for (std::size_t index = 0; index < queues.size(); ++index) {
        if (!queues[index].empty()) active.push_back(index);
    }
    while (!active.empty()) {
        std::vector<std::size_t> next_active;
        for (const std::size_t index : active) {
            auto& queue = queues[index];
            const CatalogEntry& next = queue.front();
            if (static_cast<std::int64_t>(next.cost) > remaining) continue;
            remaining -= static_cast<std::int64_t>(next.cost);
            shown.insert(next.name);
            queue.erase(queue.begin());
            if (!queue.empty()) next_active.push_back(index);
        }
        active = std::move(next_active);
    }
    return shown;
}

} // namespace

CodemodeDeclaration to_codemode_declaration(const ai::Tool& tool, const std::vector<std::string>& guidelines) {
    CodemodeDeclaration declaration;
    declaration.name = tool.name;
    std::string bullets;
    for (const auto& guideline : guidelines) {
        const std::string bullet = trimmed(guideline);
        if (bullet.empty()) continue;
        if (!bullets.empty()) bullets += "\n";
        bullets += "- " + bullet;
    }
    const std::string description = trimmed(tool.description);
    declaration.description = bullets.empty() ? description : description + "\n\n" + bullets;
    declaration.input_schema = tool.parameters;
    declaration.output_schema =
            tool.output_schema.has_value() ? *tool.output_schema : JsonValue{JsonValue::object_t{{"type", "string"}}};
    return declaration;
}

std::vector<LoadoutTool> callable_tools(const std::vector<LoadoutTool>& tools) {
    std::vector<LoadoutTool> callable;
    callable.reserve(tools.size());
    for (const auto& tool : tools) {
        if (tool.definition.name == kCodemodeToolName) continue;
        callable.push_back(tool);
    }
    return callable;
}

std::optional<std::string> describe_fields(const JsonValue& schema, std::size_t depth) {
    if (!support::is_json_object(schema)) return std::nullopt;
    const JsonValue* type = support::json_member(schema, "type");
    const JsonValue* properties = support::json_member(schema, "properties");
    if (type == nullptr || !type->holds<std::string>() || type->get_string() != "object" || properties == nullptr ||
            !support::is_json_object(*properties)) {
        return std::nullopt;
    }
    std::set<std::string, std::less<>> required;
    if (const JsonValue* names = support::json_member(schema, "required");
            names != nullptr && names->holds<JsonValue::array_t>()) {
        for (const auto& name : names->get_array()) {
            if (name.holds<std::string>()) required.insert(name.get_string());
        }
    }
    std::string fields;
    for (const auto& [name, field] : properties->get_object()) {
        if (!fields.empty()) fields += ", ";
        fields += name;
        if (!required.contains(name)) fields += "?";
        if (depth > 0) {
            if (auto nested = describe_fields(field, depth - 1); nested.has_value()) {
                fields += ": " + *nested;
            }
        }
    }
    return "{ " + fields + " }";
}

std::string describe_output(const JsonValue& output_schema) {
    const std::string type = render_tool_output_type(output_schema);
    if (type == "string") return "a string";
    if (auto fields = describe_fields(output_schema, 1); fields.has_value()) {
        return "`" + *fields + "`";
    }
    std::string collapsed;
    bool pending_space = false;
    for (const char character : type) {
        if (std::isspace(static_cast<unsigned char>(character)) != 0) {
            pending_space = true;
            continue;
        }
        if (pending_space && !collapsed.empty()) collapsed += ' ';
        pending_space = false;
        collapsed += character;
    }
    return "`" + collapsed + "`";
}

std::string describe_script_call(const ai::Tool& tool) {
    const CodemodeDeclaration declaration = to_codemode_declaration(tool);
    return trimmed(tool.description) + "\n\nCodemode: `tools." + to_codemode_identifier(tool.name) +
           "(args)` resolves to " + describe_output(declaration.output_schema) + ".";
}

bool is_namespace_name(const ToolNamespace& name_space, std::string_view query) {
    const std::string query_id = to_codemode_identifier(query);
    const auto matches = [&](std::string_view name) {
        return name == query || to_codemode_identifier(name) == query_id;
    };
    if (matches(name_space.name)) return true;
    return std::ranges::any_of(name_space.aliases, matches);
}

std::string create_codemode_description(
        const std::vector<LoadoutTool>& tools, const CodemodeCatalogSettings& settings) {
    // pi builds the declaration list first (callable minus deferred), then
    // groups it. The empty group always exists, so ungrouped tools keep their
    // own section even when every namespaced tool is deferred or over budget.
    std::vector<CodemodeDeclaration> declarations;
    std::vector<const LoadoutTool*> declaration_tools;
    const std::vector<LoadoutTool> callable = callable_tools(tools);
    for (const auto& tool : callable) {
        if (!is_listable(tool.exposure)) continue;
        declarations.push_back(to_codemode_declaration(tool.definition, tool.guidelines));
        declaration_tools.push_back(&tool);
    }

    std::vector<CatalogGroup> groups;
    groups.push_back(CatalogGroup{});
    std::map<std::string, std::size_t, std::less<>> group_by_namespace;
    for (const auto* tool : declaration_tools) {
        if (!tool->name_space.has_value()) continue;
        if (!group_by_namespace.contains(tool->name_space->name)) {
            group_by_namespace.emplace(tool->name_space->name, groups.size());
            groups.push_back(CatalogGroup{.name_space = tool->name_space});
        }
    }
    for (std::size_t index = 0; index < declarations.size(); ++index) {
        const auto& declaration = declarations[index];
        const auto* tool = declaration_tools[index];
        const std::size_t group_index =
                tool->name_space.has_value() ? group_by_namespace.at(tool->name_space->name) : 0;
        std::string section = render_tool_section(declaration);
        const std::size_t cost = (section.size() + kCharsPerToken - 1) / kCharsPerToken;
        groups[group_index].entries.push_back(
                CatalogEntry{.name = declaration.name, .section = std::move(section), .cost = cost});
    }
    std::sort(groups.begin() + 1, groups.end(), [](const CatalogGroup& left, const CatalogGroup& right) {
        return left.name_space->name < right.name_space->name;
    });
    const auto shown = select_catalog(groups, settings.inline_budget);

    std::vector<std::string> sections{std::string{kDescriptionIntro}, describe_globals(settings.models())};
    if (declarations.empty()) return join_sections(sections);

    std::vector<std::string> tool_sections{"Nested tools:"};
    for (const auto& group : groups) {
        std::vector<const CatalogEntry*> visible;
        for (const auto& entry : group.entries) {
            if (shown.contains(entry.name)) visible.push_back(&entry);
        }
        if (group.name_space.has_value()) {
            std::string listing;
            if (visible.size() != group.entries.size()) {
                listing = visible.empty() ? " (tools not listed)" : " (some tools not listed)";
            }
            const std::string description = trimmed(group.name_space->description);
            tool_sections.push_back(
                    "## " + group.name_space->name + listing + (description.empty() ? "" : "\n" + description));
        }
        for (const auto* entry : visible)
            tool_sections.push_back(entry->section);
    }
    sections.push_back(join_sections(tool_sections));
    return join_sections(sections);
}

CodemodeLoadoutChanges prepare_codemode_loadout(
        const CodemodeLoadout& loadout, const CodemodeCatalogSettings& settings) {
    const std::string mode = settings.mode == "only" ? "only" : "on";
    const auto is_direct = [](const LoadoutTool& tool) { return tool.exposure == ToolExposure::Direct; };
    const std::vector<LoadoutTool> callable = callable_tools(loadout.callable);
    std::set<std::string, std::less<>> callable_names;
    for (const auto& tool : callable)
        callable_names.insert(tool.definition.name);

    CodemodeLoadoutChanges changes;
    if (mode == "on") {
        for (const auto& tool : loadout.declared) {
            if (!callable_names.contains(tool.name)) continue;
            changes.descriptions.emplace(tool.name, describe_script_call(tool));
        }
    }
    std::vector<LoadoutTool> listed;
    for (const auto& tool : callable) {
        if (mode == "only" || !is_direct(tool)) listed.push_back(tool);
    }
    // pi `prepareCodemodeLoadout` always passes the resolved budget:
    // `getInlineBudget() ?? DEFAULT_CODEMODE_INLINE_BUDGET`.
    CodemodeCatalogSettings listed_settings = settings;
    if (!listed_settings.inline_budget.has_value()) {
        listed_settings.inline_budget = kCodemodeDefaultInlineBudget;
    }
    changes.descriptions.emplace(kCodemodeToolName, create_codemode_description(listed, listed_settings));
    if (mode == "only") {
        std::set<std::string, std::less<>> declared_names;
        for (const auto& tool : loadout.declared)
            declared_names.insert(tool.name);
        for (const auto& tool : callable) {
            if (is_direct(tool) && declared_names.contains(tool.definition.name)) {
                changes.hidden_declarations.push_back(tool.definition.name);
            }
        }
    }
    return changes;
}

} // namespace cch::coding_agent::extensions
