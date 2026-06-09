#include "Tools.hpp"

#include "AtomicWrite.hpp"
#include "PathGuard.hpp"
#include "../util/JsonSchema.hpp"

#include <fstream>
#include <sstream>

namespace cch::tools {
namespace {

util::Result<std::string> required_string_arg_any(
    const boost::json::object& args,
    const char* first,
    const char* second,
    const char* label,
    bool allow_empty) {
    auto read_key = [&](const char* key) -> util::Result<std::string> {
        if (key == nullptr) {
            return util::Result<std::string>::failure(std::string(label) + " is required");
        }
        auto* value = args.if_contains(key);
        if (value == nullptr) {
            return util::Result<std::string>::failure(std::string(label) + " is required");
        }
        if (!value->is_string()) {
            return util::Result<std::string>::failure(std::string(label) + " must be a string");
        }
        std::string text(value->as_string());
        if (!allow_empty && text.empty()) {
            return util::Result<std::string>::failure(std::string(label) + " must be non-empty");
        }
        return util::Result<std::string>::success(std::move(text));
    };

    if (args.if_contains(first) != nullptr) {
        return read_key(first);
    }
    if (second != nullptr && args.if_contains(second) != nullptr) {
        return read_key(second);
    }
    return util::Result<std::string>::failure(std::string(label) + " is required");
}

std::size_t count_occurrences(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) {
        return 0;
    }
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

class EditFileTool final : public agent::Tool {
public:
    [[nodiscard]] agent::ToolDefinition definition() const override {
        boost::json::object props;
        props["path"] = util::string_property("Path to edit inside the workspace");
        props["old_text"] = util::string_property("Exact text to replace; must occur exactly once");
        props["new_text"] = util::string_property("Replacement text");
        return {"edit_file", "Replace one unique exact text block inside a workspace file", util::object_schema(std::move(props), {"path", "old_text", "new_text"})};
    }

    [[nodiscard]] agent::ToolExecutionResult execute(const boost::json::object& arguments, const agent::ToolContext& context) override {
        auto path = required_string_arg_any(arguments, "path", nullptr, "edit_file.path", false);
        if (!path) {
            return {path.error(), true};
        }
        auto old_text = required_string_arg_any(arguments, "old_text", "oldText", "edit_file.old_text", false);
        if (!old_text) {
            return {old_text.error(), true};
        }
        auto new_text = required_string_arg_any(arguments, "new_text", "newText", "edit_file.new_text", true);
        if (!new_text) {
            return {new_text.error(), true};
        }
        auto guard = PathGuard::create(context.workspace);
        if (!guard) {
            return {guard.error(), true};
        }
        auto resolved = guard.value().resolve_existing_file(path.value());
        if (!resolved) {
            return {resolved.error(), true};
        }
        std::ifstream input(resolved.value(), std::ios::binary);
        if (!input) {
            return {"could not open file for editing: " + path.value(), true};
        }
        std::ostringstream buffer;
        buffer << input.rdbuf();
        auto content = buffer.str();
        const auto matches = count_occurrences(content, old_text.value());
        if (matches == 0) {
            return {"old_text did not match any text in " + path.value(), true};
        }
        if (matches > 1) {
            return {"old_text matched multiple regions in " + path.value() + "; edit is ambiguous", true};
        }
        const auto pos = content.find(old_text.value());
        content.replace(pos, old_text.value().size(), new_text.value());
        auto written = write_atomic_file(resolved.value(), content);
        if (!written) {
            return {written.error() + ": " + path.value(), true};
        }
        std::ostringstream result;
        result << "Successfully replaced 1 block(s) in " << path.value() << ".\n";
        result << "-" << old_text.value().substr(0, 80) << "\n+" << new_text.value().substr(0, 80);
        return {result.str(), false};
    }
};

} // namespace

agent::ToolPtr make_edit_file_tool() { return std::make_shared<EditFileTool>(); }

} // namespace cch::tools
