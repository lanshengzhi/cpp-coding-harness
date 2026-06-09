#include "Tools.hpp"

namespace cch::tools {

void add_file_tools(agent::ToolRegistry& registry) {
    registry.add(make_read_file_tool());
    registry.add(make_write_file_tool());
    registry.add(make_edit_file_tool());
}

void add_all_tools(agent::ToolRegistry& registry, std::shared_ptr<util::ProcessRunner> runner) {
    add_file_tools(registry);
    registry.add(make_bash_tool(std::move(runner)));
}

} // namespace cch::tools
