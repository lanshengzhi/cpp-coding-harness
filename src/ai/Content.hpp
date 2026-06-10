#pragma once

#include <boost/json.hpp>

#include <string>

namespace cch::ai {

enum class ContentKind {
    Text,
    ToolCall,
    Thinking,
    Image,
};

struct ToolCall {
    std::string id;
    std::string name;
    boost::json::object arguments;
    std::string raw_arguments;
    bool arguments_valid{true};
    std::string argument_error;
};

struct ContentBlock {
    ContentKind kind{ContentKind::Text};
    std::string text;
    ToolCall tool_call;

    static ContentBlock from_text(std::string value) {
        ContentBlock block;
        block.kind = ContentKind::Text;
        block.text = std::move(value);
        return block;
    }

    static ContentBlock from_tool_call(ToolCall call) {
        ContentBlock block;
        block.kind = ContentKind::ToolCall;
        block.tool_call = std::move(call);
        return block;
    }
};

} // namespace cch::ai
