#include "SimpleOptions.hpp"

#include "util/Json.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <type_traits>
#include <variant>

namespace cch::ai {
namespace {

constexpr std::array<ModelThinkingLevel, 7> kThinkingLevels{
    ModelThinkingLevel::Off,
    ModelThinkingLevel::Minimal,
    ModelThinkingLevel::Low,
    ModelThinkingLevel::Medium,
    ModelThinkingLevel::High,
    ModelThinkingLevel::XHigh,
    ModelThinkingLevel::Max,
};
constexpr std::uint64_t kCharactersPerToken = 4;
constexpr std::uint64_t kEstimatedImageCharacters = 4800;
constexpr std::uint64_t kContextSafetyTokens = 4096;

[[nodiscard]] std::size_t utf8_character_bytes(
    std::string_view text,
    std::size_t index) {
    const auto lead = static_cast<unsigned char>(text[index]);
    std::size_t length = 1;
    if (lead >= 0xc2 && lead <= 0xdf) {
        length = 2;
    } else if (lead >= 0xe0 && lead <= 0xef) {
        length = 3;
    } else if (lead >= 0xf0 && lead <= 0xf4) {
        length = 4;
    }
    if (index + length > text.size()) {
        return 1;
    }
    for (std::size_t offset = 1; offset < length; ++offset) {
        const auto continuation = static_cast<unsigned char>(text[index + offset]);
        if (continuation < 0x80 || continuation > 0xbf) {
            return 1;
        }
    }
    return length;
}

[[nodiscard]] std::uint64_t estimated_text_tokens(std::string_view text) {
    return (static_cast<std::uint64_t>(text.size()) + kCharactersPerToken - 1) /
           kCharactersPerToken;
}

[[nodiscard]] std::uint64_t estimated_content_characters(const std::vector<Content>& content) {
    std::uint64_t characters = 0;
    for (const auto& block : content) {
        std::visit(
            [&characters](const auto& value) {
                using Value = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<Value, TextContent>) {
                    characters += value.text.size();
                } else if constexpr (std::is_same_v<Value, ImageContent>) {
                    characters += kEstimatedImageCharacters;
                }
            },
            block);
    }
    return characters;
}

[[nodiscard]] std::uint64_t usage_tokens(const Usage& usage) {
    const auto total = usage.total_tokens != 0
        ? usage.total_tokens
        : usage.input + usage.output + usage.cache_read + usage.cache_write;
    return total > 0 ? static_cast<std::uint64_t>(total) : 0;
}

[[nodiscard]] std::uint64_t estimate_message_tokens(const MessageVariant& message) {
    std::uint64_t characters = 0;
    std::visit(
        [&characters](const auto& value) {
            using Value = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Value, SystemMessage>) {
                characters = value.content.size();
            } else if constexpr (std::is_same_v<Value, UserMessage>) {
                if (const auto* text = std::get_if<std::string>(&value.content)) {
                    characters = text->size();
                } else {
                    characters = estimated_content_characters(
                        std::get<std::vector<Content>>(value.content));
                }
            } else if constexpr (std::is_same_v<Value, ToolResultMessage> ||
                                 std::is_same_v<Value, CustomMessage>) {
                characters = estimated_content_characters(value.content);
            } else if constexpr (std::is_same_v<Value, AssistantMessage>) {
                for (const auto& block : value.content) {
                    std::visit(
                        [&characters](const auto& content) {
                            using ContentValue = std::decay_t<decltype(content)>;
                            if constexpr (std::is_same_v<ContentValue, TextContent>) {
                                characters += content.text.size();
                            } else if constexpr (std::is_same_v<ContentValue, ThinkingContent>) {
                                characters += content.thinking.size();
                            } else {
                                characters += content.name.size();
                                if (content.arguments) {
                                    const auto serialized = util::write_json(*content.arguments);
                                    characters += serialized ? serialized->size() : 16;
                                } else {
                                    characters += content.raw_arguments.size();
                                }
                            }
                        },
                        block);
                }
            } else if constexpr (std::is_same_v<Value, BashExecutionMessage>) {
                const auto converted = bash_execution_to_user_message(value);
                characters = std::get<TextContent>(
                    std::get<std::vector<Content>>(converted.content).front())
                    .text.size();
            } else if constexpr (std::is_same_v<Value, BranchSummaryMessage>) {
                characters = value.summary.size() + BRANCH_SUMMARY_PREFIX.size() +
                             BRANCH_SUMMARY_SUFFIX.size();
            } else if constexpr (std::is_same_v<Value, CompactionSummaryMessage>) {
                characters = value.summary.size() + COMPACTION_SUMMARY_PREFIX.size() +
                             COMPACTION_SUMMARY_SUFFIX.size();
            }
        },
        message);
    return (characters + kCharactersPerToken - 1) / kCharactersPerToken;
}

[[nodiscard]] std::uint64_t estimate_tools_tokens(const std::vector<Tool>& tools) {
    if (tools.empty()) {
        return 0;
    }
    util::JsonValue::array_t values;
    values.reserve(tools.size());
    for (const auto& tool : tools) {
        values.emplace_back(util::JsonValue::object_t{
            {"description", tool.description},
            {"name", tool.name},
            {"parameters", tool.parameters},
        });
    }
    const auto serialized = util::write_json(util::JsonValue{std::move(values)});
    return serialized ? estimated_text_tokens(*serialized) : 0;
}

} // namespace

std::vector<ModelThinkingLevel> get_supported_thinking_levels(const Model& model) {
    if (!model.reasoning) {
        return {ModelThinkingLevel::Off};
    }

    std::vector<ModelThinkingLevel> result;
    for (const auto level : kThinkingLevels) {
        const auto found = model.thinking_level_map
            ? model.thinking_level_map->find(level)
            : ThinkingLevelMap::const_iterator{};
        const bool has_entry = model.thinking_level_map &&
                               found != model.thinking_level_map->end();
        const bool explicitly_unsupported = has_entry && !found->second;
        const bool extended_without_mapping =
            (level == ModelThinkingLevel::XHigh || level == ModelThinkingLevel::Max) && !has_entry;
        if (!explicitly_unsupported && !extended_without_mapping) {
            result.push_back(level);
        }
    }
    return result;
}

ModelThinkingLevel clamp_thinking_level(
    const Model& model,
    ModelThinkingLevel requested) {
    const auto available = get_supported_thinking_levels(model);
    if (std::ranges::find(available, requested) != available.end()) {
        return requested;
    }
    const auto requested_position = std::ranges::find(kThinkingLevels, requested);
    if (requested_position == kThinkingLevels.end()) {
        return available.empty() ? ModelThinkingLevel::Off : available.front();
    }
    for (auto current = requested_position; current != kThinkingLevels.end(); ++current) {
        if (std::ranges::find(available, *current) != available.end()) {
            return *current;
        }
    }
    for (auto current = requested_position; current != kThinkingLevels.begin();) {
        --current;
        if (std::ranges::find(available, *current) != available.end()) {
            return *current;
        }
    }
    return available.empty() ? ModelThinkingLevel::Off : available.front();
}

namespace detail {

ContextTokenEstimate estimate_context_tokens(const AiContext& context) {
    std::optional<std::size_t> last_usage_index;
    std::uint64_t last_usage_tokens = 0;
    TimestampMs latest_prefix_timestamp = std::numeric_limits<TimestampMs>::min();

    for (std::size_t index = 0; index < context.messages.size(); ++index) {
        const auto& message = context.messages[index];
        if (const auto* assistant = std::get_if<AssistantMessage>(&message)) {
            const auto tokens = usage_tokens(assistant->usage);
            if (assistant->timestamp >= latest_prefix_timestamp &&
                assistant->stop_reason != AssistantStopReason::Aborted &&
                assistant->stop_reason != AssistantStopReason::Error && tokens > 0) {
                last_usage_index = index;
                last_usage_tokens = tokens;
            }
        }
        std::visit(
            [&latest_prefix_timestamp](const auto& value) {
                latest_prefix_timestamp = std::max(latest_prefix_timestamp, value.timestamp);
            },
            message);
    }

    std::uint64_t trailing_tokens = 0;
    const std::size_t start = last_usage_index ? *last_usage_index + 1 : 0;
    for (std::size_t index = start; index < context.messages.size(); ++index) {
        trailing_tokens += estimate_message_tokens(context.messages[index]);
    }

    if (!last_usage_index) {
        if (context.system_prompt) {
            trailing_tokens += estimated_text_tokens(*context.system_prompt);
        }
        trailing_tokens += estimate_tools_tokens(context.tools);
    }
    return ContextTokenEstimate{
        .tokens = last_usage_tokens + trailing_tokens,
        .usage_tokens = last_usage_tokens,
        .trailing_tokens = trailing_tokens,
        .last_usage_index = last_usage_index,
    };
}

std::uint64_t clamp_max_tokens_to_context(
    const Model& model,
    const AiContext& context,
    std::uint64_t max_tokens) {
    const auto requested = std::max<std::uint64_t>(1, max_tokens);
    if (model.context_window == 0) {
        return requested;
    }
    const auto estimated = estimate_context_tokens(context).tokens;
    const auto reserved = estimated > std::numeric_limits<std::uint64_t>::max() - kContextSafetyTokens
        ? std::numeric_limits<std::uint64_t>::max()
        : estimated + kContextSafetyTokens;
    const auto available = reserved >= model.context_window ? 1 : model.context_window - reserved;
    return std::min(requested, std::max<std::uint64_t>(1, available));
}

std::string clamp_openai_prompt_cache_key(std::string_view key) {
    std::size_t index = 0;
    std::size_t characters = 0;
    while (index < key.size() && characters < 64) {
        index += utf8_character_bytes(key, index);
        ++characters;
    }
    return std::string{key.substr(0, index)};
}

CacheRetention resolve_cache_retention(
    std::optional<CacheRetention> requested,
    const ProviderEnv& env) {
    if (requested) {
        return *requested;
    }
    if (const auto found = env.find("PI_CACHE_RETENTION");
        found != env.end() && found->second == "long") {
        return CacheRetention::Long;
    }
    return CacheRetention::Short;
}

} // namespace detail
} // namespace cch::ai
