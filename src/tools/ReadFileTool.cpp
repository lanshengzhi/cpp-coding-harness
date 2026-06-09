#include "Tools.hpp"

#include "OutputLimiter.hpp"
#include "PathGuard.hpp"
#include "../util/JsonSchema.hpp"

#include <fstream>
#include <sstream>

namespace cch::tools {
namespace {

std::string required_string(const boost::json::object& args, const char* key) {
    auto* value = args.if_contains(key);
    if (value == nullptr || !value->is_string()) {
        return {};
    }
    return std::string(value->as_string());
}

int optional_int(const boost::json::object& args, const char* key, int fallback) {
    auto* value = args.if_contains(key);
    if (value == nullptr) {
        return fallback;
    }
    if (value->is_int64()) {
        return static_cast<int>(value->as_int64());
    }
    return fallback;
}

class ReadFileTool final : public agent::Tool {
public:
    [[nodiscard]] agent::ToolDefinition definition() const override {
        boost::json::object props;
        props["path"] = util::string_property("Path to a file inside the workspace");
        props["offset"] = util::integer_property("1-based line number to start reading from");
        props["limit"] = util::integer_property("Maximum number of lines to return");
        return {"read_file", "Read a text file inside the workspace", util::object_schema(std::move(props), {"path"})};
    }

    [[nodiscard]] agent::ToolExecutionResult execute(const boost::json::object& arguments, const agent::ToolContext& context) override {
        const auto path = required_string(arguments, "path");
        if (path.empty()) {
            return {"read_file.path is required", true};
        }
        auto guard = PathGuard::create(context.workspace);
        if (!guard) {
            return {guard.error(), true};
        }
        auto resolved = guard.value().resolve_existing_file(path);
        if (!resolved) {
            return {resolved.error(), true};
        }
        std::ifstream input(resolved.value(), std::ios::binary);
        if (!input) {
            return {"could not open file for reading: " + path, true};
        }
        const int offset = std::max(1, optional_int(arguments, "offset", 1));
        const int limit = optional_int(arguments, "limit", 0);
        OutputLimit output_limit;
        std::string output;
        std::string line;
        std::size_t bytes = 0;
        std::size_t lines = 0;
        bool truncated = false;
        int line_number = 1;
        int emitted = 0;
        while (std::getline(input, line)) {
            if (line_number++ < offset) {
                continue;
            }
            if (limit > 0 && emitted >= limit) {
                break;
            }
            const auto next_bytes = bytes + line.size() + 1;
            if (lines >= output_limit.max_lines || next_bytes > output_limit.max_bytes) {
                truncated = true;
                break;
            }
            output += line;
            output += '\n';
            bytes = next_bytes;
            ++lines;
            ++emitted;
        }
        if (!output.empty()) {
            output.pop_back();
        }
        if (truncated) {
            output += "\n[output truncated]";
        }
        std::ostringstream result;
        result << "path: " << path << "\n" << output;
        return {result.str(), false};
    }
};

} // namespace

agent::ToolPtr make_read_file_tool() { return std::make_shared<ReadFileTool>(); }

} // namespace cch::tools
