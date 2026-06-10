#pragma once

#include <cch/ai/Tool.hpp>
#include <cch/util/Error.hpp>

#include <boost/asio/awaitable.hpp>
#include <glaze/glaze.hpp>

#include <optional>
#include <string>

namespace cch::agent {

struct ToolInvocation {
    std::string call_id;
    std::string name;
    glz::generic arguments;
    std::string raw_arguments;
};

struct AsyncToolExecutionResult {
    std::string content;
    std::optional<glz::generic> details;
    bool is_error{false};
};

class AsyncAgentTool {
public:
    virtual ~AsyncAgentTool() = default;

    [[nodiscard]] virtual const ai::Tool& definition() const = 0;
    [[nodiscard]] virtual boost::asio::awaitable<util::Expected<AsyncToolExecutionResult>> execute(
        ToolInvocation invocation) = 0;
};

} // namespace cch::agent
