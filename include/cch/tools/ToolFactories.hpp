#pragma once

#include <cch/agent/AgentTool.hpp>
#include <cch/harness/ExecutionEnv.hpp>

#include <memory>

namespace cch::tools {

[[nodiscard]] std::unique_ptr<agent::AsyncAgentTool> make_async_read_file_tool(std::shared_ptr<harness::AsyncExecutionEnv> env);
[[nodiscard]] std::unique_ptr<agent::AsyncAgentTool> make_async_write_file_tool(std::shared_ptr<harness::AsyncExecutionEnv> env);
[[nodiscard]] std::unique_ptr<agent::AsyncAgentTool> make_async_edit_file_tool(std::shared_ptr<harness::AsyncExecutionEnv> env);
[[nodiscard]] std::unique_ptr<agent::AsyncAgentTool> make_async_bash_tool(std::shared_ptr<harness::AsyncExecutionEnv> env);

} // namespace cch::tools
