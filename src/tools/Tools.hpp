#pragma once

#include "../agent/Tool.hpp"
#include "../agent/ToolRegistry.hpp"
#include "../util/Process.hpp"

#include <memory>

namespace cch::tools {

agent::ToolPtr make_read_file_tool();
agent::ToolPtr make_write_file_tool();
agent::ToolPtr make_edit_file_tool();
agent::ToolPtr make_bash_tool(std::shared_ptr<util::ProcessRunner> runner = std::make_shared<util::DefaultProcessRunner>());

void add_file_tools(agent::ToolRegistry& registry);
void add_all_tools(agent::ToolRegistry& registry, std::shared_ptr<util::ProcessRunner> runner = std::make_shared<util::DefaultProcessRunner>());

} // namespace cch::tools
