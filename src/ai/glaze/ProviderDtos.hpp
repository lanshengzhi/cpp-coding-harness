#pragma once

#include "ToolSchemaDtos.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
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

struct OpenAIImageUrlDto {
    std::string url;
};

struct OpenAIContentPartDto {
    std::string type;
    std::optional<std::string> text;
    std::optional<OpenAIImageUrlDto> image_url;
};

using OpenAIMessageContentDto = std::variant<std::string, std::vector<OpenAIContentPartDto>>;

struct OpenAIChatMessageDto {
    std::string role;
    OpenAIMessageContentDto content;
    std::optional<std::string> name;
    std::optional<std::string> tool_call_id;
    std::optional<std::vector<ProviderToolCallDto>> tool_calls;
};

struct OpenAIStreamOptionsDto {
    bool include_usage{true};
};

struct OpenAIChatRequestDto {
    std::string model;
    std::vector<OpenAIChatMessageDto> messages;
    std::optional<std::vector<ProviderToolDto>> tools;
    bool stream{true};
    std::optional<bool> store;
    std::optional<std::string> reasoning_effort;
    std::optional<std::string> reasoning;
    std::optional<std::string> thinking;
    std::optional<bool> enable_thinking;
    std::optional<std::string> max_completion_tokens;
    std::optional<std::string> max_tokens;
    std::optional<OpenAIStreamOptionsDto> stream_options;
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
    std::optional<std::string> reasoning_content;
    std::optional<std::string> reasoning;
    std::optional<std::string> reasoning_text;
    std::optional<std::vector<OpenAIStreamToolCallDeltaDto>> tool_calls;
};

struct OpenAIPromptTokensDetailsDto {
    std::optional<std::int64_t> cached_tokens;
    std::optional<std::int64_t> cache_write_tokens;
};

struct OpenAICompletionTokensDetailsDto {
    std::optional<std::int64_t> reasoning_tokens;
};

struct OpenAIUsageDto {
    std::int64_t prompt_tokens{};
    std::int64_t completion_tokens{};
    std::int64_t total_tokens{};
    std::optional<std::int64_t> prompt_cache_hit_tokens;
    std::optional<OpenAIPromptTokensDetailsDto> prompt_tokens_details;
    std::optional<OpenAICompletionTokensDetailsDto> completion_tokens_details;
};

struct OpenAIStreamChoiceDto {
    std::int64_t index{};
    std::optional<OpenAIStreamDeltaDto> delta;
    std::optional<std::string> finish_reason;
    std::optional<OpenAIUsageDto> usage;
};

struct OpenAIStreamChunkDto {
    std::optional<std::string> id;
    std::optional<std::string> model;
    std::vector<OpenAIStreamChoiceDto> choices;
    std::optional<OpenAIUsageDto> usage;
};

} // namespace cch::ai::glaze
