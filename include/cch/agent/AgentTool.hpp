#pragma once

#include "../ai/Content.hpp"
#include "../ai/Context.hpp"
#include "../ai/Message.hpp"
#include "../ai/Tool.hpp"
#include "../util/Error.hpp"
#include "../util/JsonValue.hpp"

#include <boost/asio/awaitable.hpp>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace cch::agent {

struct ToolInvocation {
    std::string call_id;
    std::string name;
    util::JsonValue arguments;
    std::string raw_arguments;
};

struct AsyncToolExecutionResult {
    std::vector<ai::Content> content;
    std::optional<util::JsonValue> details;
    bool is_error{false};
    bool terminate{false};
};

struct BeforeToolCallContext {
    ai::AssistantMessage assistant_message;
    ai::ToolCallContent tool_call;
    util::JsonValue args;
    ai::AiContext context;
};

struct BeforeToolCallResult {
    bool block{false};
    std::optional<std::string> reason;
};

struct AfterToolCallContext {
    ai::AssistantMessage assistant_message;
    ai::ToolCallContent tool_call;
    util::JsonValue args;
    AsyncToolExecutionResult result;
    bool is_error{false};
    ai::AiContext context;
};

struct AfterToolCallResult {
    std::optional<std::vector<ai::Content>> content;
    std::optional<util::JsonValue> details;
    std::optional<bool> is_error;
    std::optional<bool> terminate;
};

using BeforeToolCallHook = std::move_only_function<util::Expected<BeforeToolCallResult>(const BeforeToolCallContext&)>;
using AfterToolCallHook = std::move_only_function<util::Expected<AfterToolCallResult>(const AfterToolCallContext&)>;

class AsyncAgentTool {
public:
    virtual ~AsyncAgentTool() = default;

    [[nodiscard]] virtual const ai::Tool& definition() const = 0;
    [[nodiscard]] virtual boost::asio::awaitable<util::Expected<AsyncToolExecutionResult>> execute(
        ToolInvocation invocation) = 0;
};

} // namespace cch::agent
