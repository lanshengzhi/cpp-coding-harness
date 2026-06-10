#pragma once

#include "../../../include/cch/ai/glaze/ToolSchemaDtos.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace cch::ai::glaze {

struct ProviderToolDto {
    std::string type{"function"};
    FunctionToolDto function;
};

struct ProviderToolCallFunctionDto {
    std::string name;
    std::string arguments;
};

struct ProviderToolCallDto {
    std::string id;
    std::string type{"function"};
    ProviderToolCallFunctionDto function;
};

[[nodiscard]] inline ProviderToolDto to_provider_tool_dto(const Tool& tool) {
    return ProviderToolDto{"function", to_function_tool_dto(tool)};
}

struct OpenAIChatMessageDto {
    std::string role;
    std::string content;
    std::optional<std::string> tool_call_id;
    std::optional<std::vector<ProviderToolCallDto>> tool_calls;
};

struct OpenAIChatRequestDto {
    std::string model;
    std::vector<OpenAIChatMessageDto> messages;
    std::optional<std::vector<ProviderToolDto>> tools;
    bool stream{true};
};

struct OpenAIStreamFunctionDeltaDto {
    std::optional<std::string> name;
    std::optional<std::string> arguments;
};

struct OpenAIStreamToolCallDeltaDto {
    std::int64_t index{};
    std::optional<std::string> id;
    std::optional<std::string> type;
    std::optional<OpenAIStreamFunctionDeltaDto> function;
};

struct OpenAIStreamDeltaDto {
    std::optional<std::string> role;
    std::optional<std::string> content;
    std::optional<std::vector<OpenAIStreamToolCallDeltaDto>> tool_calls;
};

struct OpenAIStreamChoiceDto {
    std::int64_t index{};
    std::optional<OpenAIStreamDeltaDto> delta;
    std::optional<std::string> finish_reason;
};

struct OpenAIUsageDto {
    std::int64_t prompt_tokens{};
    std::int64_t completion_tokens{};
    std::int64_t total_tokens{};
};

struct OpenAIStreamChunkDto {
    std::optional<std::string> id;
    std::optional<std::string> model;
    std::vector<OpenAIStreamChoiceDto> choices;
    std::optional<OpenAIUsageDto> usage;
};

} // namespace cch::ai::glaze
