#include "Compaction.hpp"

#include <cch/ai/Content.hpp>
#include <cch/util/JsonValue.hpp>
#include "util/Json.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <random>
#include <regex>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace cch::harness::session {
namespace {

constexpr std::size_t kEstimatedImageChars = 4800;
constexpr std::size_t kToolResultMaxChars = 2000;

[[nodiscard]] util::Error compaction_error(
    util::ErrorCode code,
    std::string message) {
    return util::make_error(code, std::move(message));
}

[[nodiscard]] bool is_assistant_usage_valid(const ai::AssistantMessage& message) {
    return message.stop_reason != ai::AssistantStopReason::Aborted &&
        message.stop_reason != ai::AssistantStopReason::Error &&
        calculate_context_tokens(message.usage) > 0;
}

/// pi `estimateTextAndImageContentChars`: string content counts raw chars;
/// block content counts text chars plus the fixed image estimate.
[[nodiscard]] std::size_t estimate_text_and_image_chars(
    const ai::UserMessage& message) {
    if (const auto* text = std::get_if<std::string>(&message.content)) {
        return text->size();
    }
    std::size_t chars = 0;
    for (const auto& block : std::get<std::vector<ai::Content>>(message.content)) {
        if (const auto* text = std::get_if<ai::TextContent>(&block)) {
            chars += text->text.size();
        } else if (std::holds_alternative<ai::ImageContent>(block)) {
            chars += kEstimatedImageChars;
        }
    }
    return chars;
}

[[nodiscard]] std::size_t estimate_content_chars(const std::vector<ai::Content>& content) {
    std::size_t chars = 0;
    for (const auto& block : content) {
        if (const auto* text = std::get_if<ai::TextContent>(&block)) {
            chars += text->text.size();
        } else if (std::holds_alternative<ai::ImageContent>(block)) {
            chars += kEstimatedImageChars;
        }
    }
    return chars;
}

/// pi `safeJsonStringify`: serialized arguments length for the token estimate
/// and the conversation text. The wire `raw_arguments` string is used when
/// present; in-process values serialize canonically.
[[nodiscard]] std::string json_stringify(const ai::ToolCallContent& call) {
    if (!call.raw_arguments.empty()) {
        return call.raw_arguments;
    }
    if (call.arguments) {
        if (auto serialized = util::write_json(*call.arguments)) {
            return std::move(*serialized);
        }
    }
    return "undefined";
}

/// pi `getMessageFromEntryForCompaction`: the AgentMessage an entry
/// contributes to a summarization/retention range; compaction entries
/// contribute nothing. The C++ branch_summary projection (T07) guards on a
/// non-empty summary, matching `sessionEntryToContextMessages` semantics.
[[nodiscard]] std::optional<ai::MessageVariant> message_from_entry_for_compaction(
    const SessionEntry* entry) {
    if (entry->kind == SessionEntryKind::Compaction) {
        return std::nullopt;
    }
    if (entry->kind == SessionEntryKind::Message) {
        return entry->message;
    }
    if (entry->kind == SessionEntryKind::CustomMessage) {
        const auto* value = std::get_if<CustomMessageEntryValue>(&entry->value);
        if (value == nullptr) {
            return std::nullopt;
        }
        ai::CustomMessage message;
        message.custom_type = value->custom_type;
        if (const auto* text = std::get_if<std::string>(&value->content)) {
            message.content = {ai::TextContent{.text = *text, .text_signature = std::nullopt}};
        } else {
            const auto& blocks =
                std::get<std::vector<CustomMessageEntryContentBlock>>(value->content);
            message.content.reserve(blocks.size());
            for (const auto& block : blocks) {
                message.content.push_back(std::visit(
                    [](const auto& concrete) -> ai::Content { return concrete; },
                    block));
            }
        }
        message.display = value->display;
        message.details = value->details;
        message.timestamp = entry->timestamp;
        return ai::MessageVariant{std::move(message)};
    }
    if (entry->kind == SessionEntryKind::BranchSummary) {
        const auto* value = std::get_if<BranchSummaryEntryValue>(&entry->value);
        if (value == nullptr || value->summary.empty()) {
            return std::nullopt;
        }
        ai::BranchSummaryMessage message;
        message.summary = value->summary;
        message.from_id = value->from_id;
        message.timestamp = entry->timestamp;
        return ai::MessageVariant{std::move(message)};
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::string> role_of_message(
    const std::optional<ai::MessageVariant>& message) {
    if (!message) {
        return std::nullopt;
    }
    if (std::holds_alternative<ai::UserMessage>(*message)) {
        return "user";
    }
    if (std::holds_alternative<ai::AssistantMessage>(*message)) {
        return "assistant";
    }
    if (std::holds_alternative<ai::ToolResultMessage>(*message)) {
        return "toolResult";
    }
    if (std::holds_alternative<ai::BashExecutionMessage>(*message)) {
        return "bashExecution";
    }
    if (std::holds_alternative<ai::CustomMessage>(*message)) {
        return "custom";
    }
    if (std::holds_alternative<ai::BranchSummaryMessage>(*message)) {
        return "branchSummary";
    }
    if (std::holds_alternative<ai::CompactionSummaryMessage>(*message)) {
        return "compactionSummary";
    }
    return std::nullopt;
}

[[nodiscard]] bool is_cut_point_role(std::string_view role) {
    return role == "user" || role == "assistant" || role == "bashExecution" ||
        role == "custom" || role == "branchSummary" || role == "compactionSummary";
}

[[nodiscard]] bool is_turn_start_role(std::string_view role) {
    return role == "user" || role == "bashExecution";
}

/// pi `convertToLlm` applied to compaction messages: extended roles become
/// user messages (bashExecution excluded from context dropped), system
/// messages and unknown roles contribute nothing.
[[nodiscard]] std::vector<ai::MessageVariant> convert_to_llm(
    const std::vector<ai::MessageVariant>& messages) {
    std::vector<ai::MessageVariant> converted;
    converted.reserve(messages.size());
    for (const auto& message : messages) {
        if (const auto* bash = std::get_if<ai::BashExecutionMessage>(&message)) {
            if (!bash->exclude_from_context) {
                converted.push_back(ai::bash_execution_to_user_message(*bash));
            }
        } else if (const auto* custom = std::get_if<ai::CustomMessage>(&message)) {
            converted.push_back(ai::custom_message_to_user_message(*custom));
        } else if (const auto* branch = std::get_if<ai::BranchSummaryMessage>(&message)) {
            converted.push_back(ai::branch_summary_to_user_message(*branch));
        } else if (const auto* compaction =
                       std::get_if<ai::CompactionSummaryMessage>(&message)) {
            converted.push_back(ai::compaction_summary_to_user_message(*compaction));
        } else if (std::holds_alternative<ai::UserMessage>(message) ||
                   std::holds_alternative<ai::AssistantMessage>(message) ||
                   std::holds_alternative<ai::ToolResultMessage>(message)) {
            converted.push_back(message);
        }
    }
    return converted;
}

[[nodiscard]] std::string truncate_for_summary(std::string_view text) {
    if (text.size() <= kToolResultMaxChars) {
        return std::string{text};
    }
    const std::size_t truncated = text.size() - kToolResultMaxChars;
    std::string result{text.substr(0, kToolResultMaxChars)};
    result += "\n\n[... ";
    result += std::to_string(truncated);
    result += " more characters truncated]";
    return result;
}

[[nodiscard]] std::string tool_call_arguments_text(const ai::ToolCallContent& call) {
    std::string text;
    if (!call.arguments) {
        return text;
    }
    const auto* object = call.arguments->get_if<util::JsonValue::object_t>();
    if (object == nullptr) {
        return text;
    }
    bool first = true;
    for (const auto& [key, value] : *object) {
        if (!first) {
            text += ", ";
        }
        first = false;
        text += key;
        text += "=";
        if (auto serialized = util::write_json(value)) {
            text += *serialized;
        }
    }
    return text;
}

/// pi `completeSimpleWithRetries` request construction: summarization requests
/// are standalone, so routing is isolated and cache writes that cannot be
/// reused are avoided — `cacheRetention: "none"` plus a fresh session id.
struct SummarizationCall {
    ai::AiContext context;
    ai::SimpleStreamOptions options;
};

[[nodiscard]] SummarizationCall make_summarization_call(
    const std::vector<ai::MessageVariant>& current_messages,
    const ai::Model& model,
    std::string_view base_prompt,
    std::string_view system_prompt,
    const std::optional<std::string>& custom_instructions,
    const std::optional<std::string>& previous_summary,
    std::string_view thinking_level,
    std::size_t max_tokens,
    std::stop_token stop_token) {
    std::string prompt{base_prompt};
    if (custom_instructions) {
        prompt += "\n\nAdditional focus: ";
        prompt += *custom_instructions;
    }

    const auto llm_messages = convert_to_llm(current_messages);
    const std::string conversation_text = serialize_conversation(llm_messages);
    std::string prompt_text = "<conversation>\n";
    prompt_text += conversation_text;
    prompt_text += "\n</conversation>\n\n";
    if (previous_summary) {
        prompt_text += "<previous-summary>\n";
        prompt_text += *previous_summary;
        prompt_text += "\n</previous-summary>\n\n";
    }
    prompt_text += prompt;

    SummarizationCall call;
    call.context.system_prompt = std::string{system_prompt};
    call.context.messages.push_back(
        ai::user_text_message(std::move(prompt_text), 0));
    call.options.max_tokens = max_tokens;
    call.options.stop_token = std::move(stop_token);
    if (model.reasoning && !thinking_level.empty() && thinking_level != "off") {
        if (thinking_level == "minimal") {
            call.options.reasoning = ai::ThinkingLevel::Minimal;
        } else if (thinking_level == "low") {
            call.options.reasoning = ai::ThinkingLevel::Low;
        } else if (thinking_level == "medium") {
            call.options.reasoning = ai::ThinkingLevel::Medium;
        } else if (thinking_level == "high") {
            call.options.reasoning = ai::ThinkingLevel::High;
        } else if (thinking_level == "xhigh") {
            call.options.reasoning = ai::ThinkingLevel::XHigh;
        } else if (thinking_level == "max") {
            call.options.reasoning = ai::ThinkingLevel::Max;
        }
    }
    return call;
}

/// One summarization response with its provider usage (pi
/// `generateSummaryWithUsage` / `generateTurnPrefixSummary` result).
struct SummarizationOutcome {
    std::string text;
    ai::Usage usage;
};

[[nodiscard]] util::Error summarization_failure(
    ai::AssistantStopReason reason,
    const std::optional<std::string>& error_message,
    std::string_view aborted_fallback,
    std::string_view failed_prefix) {
    if (reason == ai::AssistantStopReason::Aborted) {
        return compaction_error(
            util::ErrorCode::Cancelled,
            error_message.value_or(std::string{aborted_fallback}));
    }
    return compaction_error(
        util::ErrorCode::Stream,
        std::string{failed_prefix} +
            (error_message.value_or(std::string{"Unknown error"})));
}

[[nodiscard]] ai::Usage combine_usage(
    const ai::Usage& first,
    const ai::Usage& second) {
    ai::Usage combined;
    combined.input = first.input + second.input;
    combined.output = first.output + second.output;
    combined.cache_read = first.cache_read + second.cache_read;
    combined.cache_write = first.cache_write + second.cache_write;
    if (first.cache_write_1h || second.cache_write_1h) {
        combined.cache_write_1h =
            first.cache_write_1h.value_or(0) + second.cache_write_1h.value_or(0);
    }
    if (first.reasoning || second.reasoning) {
        combined.reasoning = first.reasoning.value_or(0) + second.reasoning.value_or(0);
    }
    combined.total_tokens = first.total_tokens + second.total_tokens;
    combined.cost.input = first.cost.input + second.cost.input;
    combined.cost.output = first.cost.output + second.cost.output;
    combined.cost.cache_read = first.cost.cache_read + second.cost.cache_read;
    combined.cost.cache_write = first.cost.cache_write + second.cost.cache_write;
    combined.cost.total = first.cost.total + second.cost.total;
    return combined;
}

/// Fresh session id for one summarization request: isolation only, so
/// compaction never pollutes the session's cache affinity (pi `uuidv7()`).
[[nodiscard]] std::string fresh_summarization_session_id() {
    thread_local std::random_device rd;
    thread_local std::mt19937_64 gen(rd());
    thread_local std::uniform_int_distribution<unsigned> dist(0, 15);
    const char hex_chars[] = "0123456789abcdef";
    std::string id(32, '0');
    for (auto& c : id) {
        c = hex_chars[dist(gen)];
    }
    return id;
}

} // namespace

std::size_t calculate_context_tokens(const ai::Usage& usage) {
    if (usage.total_tokens != 0) {
        return static_cast<std::size_t>(usage.total_tokens);
    }
    return static_cast<std::size_t>(
        usage.input + usage.output + usage.cache_read + usage.cache_write);
}

namespace {

/// Provider error-message patterns that indicate a context overflow (pi
/// `OVERFLOW_PATTERNS` in `packages/ai/src/utils/overflow.ts`, translated to
/// `std::regex` ECMAScript syntax).
[[nodiscard]] const std::vector<std::regex>& overflow_patterns() {
    static const std::vector<std::regex> patterns{
        std::regex{R"(prompt is too long)", std::regex_constants::icase},
        std::regex{R"(request_too_large)", std::regex_constants::icase},
        std::regex{R"(input is too long for requested model)", std::regex_constants::icase},
        std::regex{R"(exceeds the context window)", std::regex_constants::icase},
        std::regex{R"(exceeds (?:the )?(?:model'?s )?maximum context length(?: of [\d,]+ tokens?|\s*\([\d,]+\)))", std::regex_constants::icase},
        std::regex{R"(input token count.*exceeds the maximum)", std::regex_constants::icase},
        std::regex{R"(maximum prompt length is \d+)", std::regex_constants::icase},
        std::regex{R"(reduce the length of the messages)", std::regex_constants::icase},
        std::regex{R"(maximum context length is \d+ tokens)", std::regex_constants::icase},
        std::regex{R"(exceeds (?:the )?maximum allowed input length of [\d,]+ tokens?)", std::regex_constants::icase},
        std::regex{R"(input \(\d+ tokens\) is longer than the model'?s context length \(\d+ tokens\))", std::regex_constants::icase},
        std::regex{R"(exceeds the limit of \d+)", std::regex_constants::icase},
        std::regex{R"(exceeds the available context size)", std::regex_constants::icase},
        std::regex{R"(greater than the context length)", std::regex_constants::icase},
        std::regex{R"(context window exceeds limit)", std::regex_constants::icase},
        std::regex{R"(exceeded model token limit)", std::regex_constants::icase},
        std::regex{R"(too large for model with \d+ maximum context length)", std::regex_constants::icase},
        std::regex{R"(prompt has [\d,]+ tokens?, but the configured context size is [\d,]+ tokens?)", std::regex_constants::icase},
        std::regex{R"(model_context_window_exceeded)", std::regex_constants::icase},
        std::regex{R"(prompt too long; exceeded (?:max )?context length)", std::regex_constants::icase},
        std::regex{R"(range of input length should be)", std::regex_constants::icase},
        std::regex{R"(context[_ ]length[_ ]exceeded)", std::regex_constants::icase},
        std::regex{R"(too many tokens)", std::regex_constants::icase},
        std::regex{R"(token limit exceeded)", std::regex_constants::icase},
        std::regex{R"(^4(?:00|13)\s*(?:status code)?\s*\(no body\))", std::regex_constants::icase},
    };
    return patterns;
}

/// Patterns that indicate a non-overflow error even when an overflow pattern
/// also matches (pi `NON_OVERFLOW_PATTERNS`; e.g. Bedrock throttling errors
/// containing "too many tokens").
[[nodiscard]] const std::vector<std::regex>& non_overflow_patterns() {
    static const std::vector<std::regex> patterns{
        std::regex{R"(^(Throttling error|Service unavailable):)", std::regex_constants::icase},
        std::regex{R"(rate limit)", std::regex_constants::icase},
        std::regex{R"(too many requests)", std::regex_constants::icase},
    };
    return patterns;
}

[[nodiscard]] bool matches_any(
    const std::vector<std::regex>& patterns,
    const std::string& text) {
    for (const auto& pattern : patterns) {
        if (std::regex_search(text, pattern)) {
            return true;
        }
    }
    return false;
}

} // namespace

bool should_compact(
    std::size_t context_tokens,
    std::size_t context_window,
    const CompactionSettings& settings) {
    if (!settings.enabled) {
        return false;
    }
    // Signed comparison mirrors pi's JS arithmetic exactly: an unknown (zero)
    // context window behaves like pi's `contextWindow ?? 0` (any non-empty
    // context exceeds `0 - reserveTokens`), and a small window never underflows.
    return static_cast<std::int64_t>(context_tokens) >
        static_cast<std::int64_t>(context_window) -
        static_cast<std::int64_t>(settings.reserve_tokens);
}

bool is_context_overflow(
    const ai::AssistantMessage& message,
    std::size_t context_window) {
    // Case 1: error-based overflow — a provider error message matching an
    // overflow pattern that is not also a known non-overflow pattern.
    if (message.stop_reason == ai::AssistantStopReason::Error &&
        message.error_message) {
        const std::string text = *message.error_message;
        if (!matches_any(non_overflow_patterns(), text) &&
            matches_any(overflow_patterns(), text)) {
            return true;
        }
    }

    // Case 2: silent overflow (z.ai style) — a successful response whose
    // input usage already exceeds the context window. A zero window disables
    // the usage-based cases (pi's truthiness gate on `contextWindow`).
    if (context_window != 0 &&
        message.stop_reason == ai::AssistantStopReason::Stop) {
        const std::int64_t input_tokens =
            message.usage.input + message.usage.cache_read;
        if (input_tokens > static_cast<std::int64_t>(context_window)) {
            return true;
        }
    }

    // Case 3: length-stop overflow (Xiaomi MiMo style) — the server truncated
    // the oversized input to fill the window, leaving no room for output.
    if (context_window != 0 &&
        message.stop_reason == ai::AssistantStopReason::Length &&
        message.usage.output == 0) {
        const std::int64_t input_tokens =
            message.usage.input + message.usage.cache_read;
        if (input_tokens * 100 >= static_cast<std::int64_t>(context_window) * 99) {
            return true;
        }
    }

    return false;
}

std::size_t estimate_tokens(const ai::MessageVariant& message) {
    std::size_t chars = 0;
    if (const auto* user = std::get_if<ai::UserMessage>(&message)) {
        chars = estimate_text_and_image_chars(*user);
    } else if (const auto* assistant = std::get_if<ai::AssistantMessage>(&message)) {
        for (const auto& block : assistant->content) {
            if (const auto* text = std::get_if<ai::TextContent>(&block)) {
                chars += text->text.size();
            } else if (const auto* thinking = std::get_if<ai::ThinkingContent>(&block)) {
                chars += thinking->thinking.size();
            } else if (const auto* call = std::get_if<ai::ToolCallContent>(&block)) {
                chars += call->name.size() + json_stringify(*call).size();
            }
        }
    } else if (const auto* tool_result = std::get_if<ai::ToolResultMessage>(&message)) {
        chars = estimate_content_chars(tool_result->content);
    } else if (const auto* bash = std::get_if<ai::BashExecutionMessage>(&message)) {
        chars = bash->command.size() + bash->output.size();
    } else if (const auto* custom = std::get_if<ai::CustomMessage>(&message)) {
        chars = estimate_content_chars(custom->content);
    } else if (const auto* branch = std::get_if<ai::BranchSummaryMessage>(&message)) {
        chars = branch->summary.size();
    } else if (const auto* compaction =
                   std::get_if<ai::CompactionSummaryMessage>(&message)) {
        chars = compaction->summary.size();
    } else {
        return 0;
    }
    return (chars + 3) / 4;
}

std::optional<ai::Usage> get_last_assistant_usage(
    const std::vector<ai::MessageVariant>& messages) {
    for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
        if (const auto* assistant = std::get_if<ai::AssistantMessage>(&*it)) {
            if (is_assistant_usage_valid(*assistant)) {
                return assistant->usage;
            }
        }
    }
    return std::nullopt;
}

ContextUsageEstimate estimate_context_tokens(
    const std::vector<ai::MessageVariant>& messages) {
    std::optional<std::pair<ai::Usage, std::size_t>> usage_info;
    for (std::size_t i = messages.size(); i > 0; --i) {
        const auto& message = messages[i - 1];
        if (const auto* assistant = std::get_if<ai::AssistantMessage>(&message)) {
            if (is_assistant_usage_valid(*assistant)) {
                usage_info = std::make_pair(assistant->usage, i - 1);
                break;
            }
        }
    }

    if (!usage_info) {
        std::size_t estimated = 0;
        for (const auto& message : messages) {
            estimated += estimate_tokens(message);
        }
        return ContextUsageEstimate{
            .tokens = estimated,
            .usage_tokens = 0,
            .trailing_tokens = estimated,
            .last_usage_index = std::nullopt,
        };
    }

    const std::size_t usage_tokens = calculate_context_tokens(usage_info->first);
    std::size_t trailing_tokens = 0;
    for (std::size_t i = usage_info->second + 1; i < messages.size(); ++i) {
        trailing_tokens += estimate_tokens(messages[i]);
    }
    return ContextUsageEstimate{
        .tokens = usage_tokens + trailing_tokens,
        .usage_tokens = usage_tokens,
        .trailing_tokens = trailing_tokens,
        .last_usage_index = usage_info->second,
    };
}

namespace {

[[nodiscard]] std::vector<std::size_t> find_valid_cut_points(
    const std::vector<const SessionEntry*>& path,
    std::size_t start_index,
    std::size_t end_index) {
    std::vector<std::size_t> cut_points;
    for (std::size_t i = start_index; i < end_index; ++i) {
        const auto* entry = path[i];
        if (entry->kind == SessionEntryKind::Message) {
            const auto role = role_of_message(entry->message);
            if (role && is_cut_point_role(*role)) {
                cut_points.push_back(i);
            }
        } else if (entry->kind == SessionEntryKind::BranchSummary ||
                   entry->kind == SessionEntryKind::CustomMessage) {
            cut_points.push_back(i);
        }
    }
    return cut_points;
}

} // namespace

std::ptrdiff_t find_turn_start_index(
    const std::vector<const SessionEntry*>& path,
    std::size_t entry_index,
    std::size_t start_index) {
    for (std::size_t i = entry_index + 1; i > start_index; --i) {
        const auto* entry = path[i - 1];
        if (entry->kind == SessionEntryKind::BranchSummary ||
            entry->kind == SessionEntryKind::CustomMessage) {
            return static_cast<std::ptrdiff_t>(i - 1);
        }
        if (entry->kind == SessionEntryKind::Message) {
            const auto role = role_of_message(entry->message);
            if (role && is_turn_start_role(*role)) {
                return static_cast<std::ptrdiff_t>(i - 1);
            }
        }
    }
    return -1;
}

CutPointResult find_cut_point(
    const std::vector<const SessionEntry*>& path,
    std::size_t start_index,
    std::size_t end_index,
    std::size_t keep_recent_tokens) {
    const auto cut_points = find_valid_cut_points(path, start_index, end_index);
    if (cut_points.empty()) {
        return CutPointResult{
            .first_kept_entry_index = start_index,
            .turn_start_index = -1,
            .is_split_turn = false,
        };
    }

    std::size_t accumulated_tokens = 0;
    std::size_t cut_index = cut_points.front();
    for (std::size_t i = end_index; i > start_index; --i) {
        const auto* entry = path[i - 1];
        if (entry->kind != SessionEntryKind::Message || !entry->message) {
            continue;
        }
        accumulated_tokens += estimate_tokens(*entry->message);
        if (accumulated_tokens >= keep_recent_tokens) {
            for (std::size_t c = 0; c < cut_points.size(); ++c) {
                if (cut_points[c] >= i - 1) {
                    cut_index = cut_points[c];
                    break;
                }
            }
            break;
        }
    }

    // Scan backwards from the cut to include adjacent metadata entries that
    // do not affect context (pi's trailing while loop), stopping at compaction
    // boundaries and context-visible (message) entries.
    while (cut_index > start_index) {
        const auto* prev_entry = path[cut_index - 1];
        if (prev_entry->kind == SessionEntryKind::Compaction) {
            break;
        }
        if (prev_entry->kind == SessionEntryKind::Message) {
            break;
        }
        --cut_index;
    }

    const auto* cut_entry = path[cut_index];
    const bool is_user_message = cut_entry->kind == SessionEntryKind::Message &&
        role_of_message(cut_entry->message) == "user";
    const std::ptrdiff_t turn_start_index =
        is_user_message ? -1 : find_turn_start_index(path, cut_index, start_index);

    return CutPointResult{
        .first_kept_entry_index = cut_index,
        .turn_start_index = turn_start_index,
        .is_split_turn = !is_user_message && turn_start_index != -1,
    };
}

void extract_file_ops_from_message(
    const ai::MessageVariant& message,
    FileOperations& file_ops) {
    const auto* assistant = std::get_if<ai::AssistantMessage>(&message);
    if (assistant == nullptr) {
        return;
    }
    for (const auto& block : assistant->content) {
        const auto* call = std::get_if<ai::ToolCallContent>(&block);
        if (call == nullptr || !call->arguments) {
            continue;
        }
        const auto* object = call->arguments->get_if<util::JsonValue::object_t>();
        if (object == nullptr) {
            continue;
        }
        const auto path_it = object->find("path");
        if (path_it == object->end()) {
            continue;
        }
        const auto* path = path_it->second.get_if<std::string>();
        if (path == nullptr) {
            continue;
        }
        if (call->name == "read") {
            file_ops.read.insert(*path);
        } else if (call->name == "write") {
            file_ops.written.insert(*path);
        } else if (call->name == "edit") {
            file_ops.edited.insert(*path);
        }
    }
}

FileLists compute_file_lists(const FileOperations& file_ops) {
    std::set<std::string> modified{file_ops.edited};
    modified.insert(file_ops.written.begin(), file_ops.written.end());

    std::vector<std::string> read_only;
    read_only.reserve(file_ops.read.size());
    for (const auto& file : file_ops.read) {
        if (modified.count(file) == 0) {
            read_only.push_back(file);
        }
    }
    std::sort(read_only.begin(), read_only.end());

    std::vector<std::string> modified_files{modified.begin(), modified.end()};
    return FileLists{
        .read_files = std::move(read_only),
        .modified_files = std::move(modified_files),
    };
}

std::string format_file_operations(
    const std::vector<std::string>& read_files,
    const std::vector<std::string>& modified_files) {
    std::vector<std::string> sections;
    if (!read_files.empty()) {
        std::string section = "<read-files>\n";
        for (std::size_t i = 0; i < read_files.size(); ++i) {
            if (i > 0) {
                section += "\n";
            }
            section += read_files[i];
        }
        section += "\n</read-files>";
        sections.push_back(std::move(section));
    }
    if (!modified_files.empty()) {
        std::string section = "<modified-files>\n";
        for (std::size_t i = 0; i < modified_files.size(); ++i) {
            if (i > 0) {
                section += "\n";
            }
            section += modified_files[i];
        }
        section += "\n</modified-files>";
        sections.push_back(std::move(section));
    }
    if (sections.empty()) {
        return "";
    }
    std::string result = "\n\n";
    for (std::size_t i = 0; i < sections.size(); ++i) {
        if (i > 0) {
            result += "\n\n";
        }
        result += sections[i];
    }
    return result;
}

std::string serialize_conversation(
    const std::vector<ai::MessageVariant>& messages) {
    std::vector<std::string> parts;
    for (const auto& message : messages) {
        if (const auto* user = std::get_if<ai::UserMessage>(&message)) {
            const std::string content = ai::text_from_user_message(*user);
            if (!content.empty()) {
                parts.push_back("[User]: " + content);
            }
        } else if (const auto* assistant = std::get_if<ai::AssistantMessage>(&message)) {
            std::vector<std::string> thinking_parts;
            std::vector<std::string> tool_calls;
            bool has_text = false;
            for (const auto& block : assistant->content) {
                if (const auto* thinking = std::get_if<ai::ThinkingContent>(&block)) {
                    thinking_parts.push_back(thinking->thinking);
                } else if (const auto* call = std::get_if<ai::ToolCallContent>(&block)) {
                    tool_calls.push_back(
                        call->name + "(" + tool_call_arguments_text(*call) + ")");
                } else if (std::holds_alternative<ai::TextContent>(block)) {
                    has_text = true;
                }
            }
            if (!thinking_parts.empty()) {
                std::string thinking = "[Assistant thinking]: ";
                for (std::size_t i = 0; i < thinking_parts.size(); ++i) {
                    if (i > 0) {
                        thinking += "\n";
                    }
                    thinking += thinking_parts[i];
                }
                parts.push_back(std::move(thinking));
            }
            if (has_text) {
                parts.push_back(
                    "[Assistant]: " + ai::text_from_assistant_content(assistant->content));
            }
            if (!tool_calls.empty()) {
                std::string calls;
                for (std::size_t i = 0; i < tool_calls.size(); ++i) {
                    if (i > 0) {
                        calls += "; ";
                    }
                    calls += tool_calls[i];
                }
                parts.push_back("[Assistant tool calls]: " + std::move(calls));
            }
        } else if (const auto* tool_result =
                       std::get_if<ai::ToolResultMessage>(&message)) {
            const std::string content = ai::text_from_content(tool_result->content);
            if (!content.empty()) {
                parts.push_back("[Tool result]: " + truncate_for_summary(content));
            }
        }
    }

    std::string result;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) {
            result += "\n\n";
        }
        result += parts[i];
    }
    return result;
}

util::Expected<std::optional<CompactionPreparation>> prepare_compaction(
    const std::vector<const SessionEntry*>& path,
    CompactionSettings settings) {
    if (path.empty() || path.back()->kind == SessionEntryKind::Compaction) {
        return std::nullopt;
    }

    std::ptrdiff_t prev_compaction_index = -1;
    for (std::size_t i = path.size(); i > 0; --i) {
        if (path[i - 1]->kind == SessionEntryKind::Compaction) {
            prev_compaction_index = static_cast<std::ptrdiff_t>(i - 1);
            break;
        }
    }

    std::optional<std::string> previous_summary;
    std::size_t boundary_start = 0;
    if (prev_compaction_index >= 0) {
        const auto* prev_compaction = path[static_cast<std::size_t>(prev_compaction_index)];
        const auto* value = std::get_if<CompactionEntryValue>(&prev_compaction->value);
        if (value != nullptr) {
            previous_summary = value->summary;
        }
        std::ptrdiff_t first_kept_index = -1;
        if (value != nullptr && value->first_kept_entry_id) {
            for (std::size_t j = 0; j < path.size(); ++j) {
                if (path[j]->entry_id == *value->first_kept_entry_id) {
                    first_kept_index = static_cast<std::ptrdiff_t>(j);
                    break;
                }
            }
        }
        boundary_start = first_kept_index >= 0
            ? static_cast<std::size_t>(first_kept_index)
            : static_cast<std::size_t>(prev_compaction_index) + 1;
    }
    const std::size_t boundary_end = path.size();

    const auto context = buildSessionContext(path);
    const std::size_t tokens_before =
        estimate_context_tokens(context.messages).tokens;

    const auto cut_point = find_cut_point(
        path, boundary_start, boundary_end, settings.keep_recent_tokens);
    const auto* first_kept_entry = path[cut_point.first_kept_entry_index];
    if (first_kept_entry == nullptr || first_kept_entry->entry_id.empty()) {
        return std::unexpected(compaction_error(
            util::ErrorCode::Session,
            "First kept entry has no UUID - session may need migration"));
    }
    const std::string first_kept_entry_id = first_kept_entry->entry_id;

    const std::size_t history_end = cut_point.is_split_turn
        ? static_cast<std::size_t>(cut_point.turn_start_index)
        : cut_point.first_kept_entry_index;

    CompactionPreparation preparation;
    preparation.first_kept_entry_id = first_kept_entry_id;
    preparation.is_split_turn = cut_point.is_split_turn;
    preparation.tokens_before = tokens_before;
    preparation.previous_summary = std::move(previous_summary);
    preparation.settings = settings;

    for (std::size_t i = boundary_start; i < history_end; ++i) {
        if (auto message = message_from_entry_for_compaction(path[i])) {
            preparation.messages_to_summarize.push_back(std::move(*message));
        }
    }
    if (cut_point.is_split_turn) {
        for (std::size_t i = static_cast<std::size_t>(cut_point.turn_start_index);
             i < cut_point.first_kept_entry_index; ++i) {
            if (auto message = message_from_entry_for_compaction(path[i])) {
                preparation.turn_prefix_messages.push_back(std::move(*message));
            }
        }
    }
    for (std::size_t i = cut_point.first_kept_entry_index; i < boundary_end; ++i) {
        if (auto message = message_from_entry_for_compaction(path[i])) {
            preparation.retained_tail.push_back(std::move(*message));
        }
    }

    // File operations extracted from the summarized history and any previous
    // pi-generated compaction's details (pi `extractFileOperations`).
    auto& file_ops = preparation.file_ops;
    if (prev_compaction_index >= 0) {
        const auto* prev_compaction = path[static_cast<std::size_t>(prev_compaction_index)];
        const auto* value = std::get_if<CompactionEntryValue>(&prev_compaction->value);
        if (value != nullptr && !value->from_hook.value_or(false) &&
            value->details.has_value()) {
            const auto* details =
                value->details->get_if<util::JsonValue::object_t>();
            if (details != nullptr) {
                if (const auto read_it = details->find("readFiles");
                    read_it != details->end()) {
                    if (const auto* files = read_it->second.get_if<util::JsonValue::array_t>()) {
                        for (const auto& file : *files) {
                            if (const auto* name = file.get_if<std::string>()) {
                                file_ops.read.insert(*name);
                            }
                        }
                    }
                }
                if (const auto modified_it = details->find("modifiedFiles");
                    modified_it != details->end()) {
                    if (const auto* files =
                            modified_it->second.get_if<util::JsonValue::array_t>()) {
                        for (const auto& file : *files) {
                            if (const auto* name = file.get_if<std::string>()) {
                                file_ops.edited.insert(*name);
                            }
                        }
                    }
                }
            }
        }
    }
    for (const auto& message : preparation.messages_to_summarize) {
        extract_file_ops_from_message(message, file_ops);
    }
    if (cut_point.is_split_turn) {
        for (const auto& message : preparation.turn_prefix_messages) {
            extract_file_ops_from_message(message, file_ops);
        }
    }

    return preparation;
}

namespace {

/// pi `generateSummaryWithUsage`: summarize `current_messages` into text plus
/// usage through one `cacheRetention:"none"` + fresh-session-id request.
[[nodiscard]] boost::asio::awaitable<util::Expected<SummarizationOutcome>>
generate_summary_with_usage(
    const std::vector<ai::MessageVariant>& current_messages,
    const ai::Model& model,
    std::optional<std::string> custom_instructions,
    std::optional<std::string> previous_summary,
    std::string_view thinking_level,
    std::size_t max_tokens,
    std::stop_token stop_token,
    SummarizationStreamFn& stream_fn,
    SummarizationSessionIdFactory& session_id_factory) {
    auto call = make_summarization_call(
        current_messages,
        model,
        previous_summary ? kUpdateSummarizationPrompt : kSummarizationPrompt,
        kSummarizationSystemPrompt,
        custom_instructions,
        previous_summary,
        thinking_level,
        max_tokens,
        stop_token);
    call.options.cache_retention = ai::CacheRetention::None;
    call.options.session_id = session_id_factory();

    auto response = co_await stream_fn(
        std::move(call.context), std::move(call.options));
    if (!response) {
        co_return std::unexpected(response.error());
    }
    if (response->stop_reason == ai::AssistantStopReason::Aborted ||
        response->stop_reason == ai::AssistantStopReason::Error) {
        co_return std::unexpected(summarization_failure(
            response->stop_reason,
            response->error_message,
            "Summarization aborted",
            "Summarization failed: "));
    }
    co_return SummarizationOutcome{
        .text = ai::text_from_assistant_content(response->content),
        .usage = response->usage,
    };
}

/// pi `generateTurnPrefixSummary`: a separate smaller-budget summarization
/// for the prefix of a split turn.
[[nodiscard]] boost::asio::awaitable<util::Expected<SummarizationOutcome>>
generate_turn_prefix_summary(
    const std::vector<ai::MessageVariant>& turn_prefix_messages,
    const ai::Model& model,
    std::string_view thinking_level,
    std::size_t max_tokens,
    std::stop_token stop_token,
    SummarizationStreamFn& stream_fn,
    SummarizationSessionIdFactory& session_id_factory) {
    auto call = make_summarization_call(
        turn_prefix_messages,
        model,
        kTurnPrefixSummarizationPrompt,
        kSummarizationSystemPrompt,
        std::nullopt,
        std::nullopt,
        thinking_level,
        max_tokens,
        stop_token);
    call.options.cache_retention = ai::CacheRetention::None;
    call.options.session_id = session_id_factory();

    auto response = co_await stream_fn(
        std::move(call.context), std::move(call.options));
    if (!response) {
        co_return std::unexpected(response.error());
    }
    if (response->stop_reason == ai::AssistantStopReason::Aborted ||
        response->stop_reason == ai::AssistantStopReason::Error) {
        co_return std::unexpected(summarization_failure(
            response->stop_reason,
            response->error_message,
            "Turn prefix summarization aborted",
            "Turn prefix summarization failed: "));
    }
    co_return SummarizationOutcome{
        .text = ai::text_from_assistant_content(response->content),
        .usage = response->usage,
    };
}

[[nodiscard]] std::size_t summary_max_tokens(
    const ai::Model& model,
    std::size_t reserve_tokens,
    double fraction) {
    const auto capped = static_cast<std::size_t>(
        std::floor(static_cast<double>(reserve_tokens) * fraction));
    if (model.max_tokens > 0) {
        return std::min(capped, static_cast<std::size_t>(model.max_tokens));
    }
    return capped;
}

} // namespace

boost::asio::awaitable<util::Expected<CompactionResult>> compact(
    const CompactionPreparation& preparation,
    const ai::Model& model,
    CompactionRunOptions run_options) {
    if (preparation.first_kept_entry_id.empty()) {
        co_return std::unexpected(compaction_error(
            util::ErrorCode::Session,
            "First kept entry has no UUID - session may need migration"));
    }
    if (!run_options.summarization_stream) {
        co_return std::unexpected(compaction_error(
            util::ErrorCode::Validation,
            "summarization stream is unavailable"));
    }
    SummarizationSessionIdFactory session_id_factory =
        run_options.session_id_factory
            ? std::move(run_options.session_id_factory)
            : SummarizationSessionIdFactory{fresh_summarization_session_id};

    CompactionResult result;
    result.first_kept_entry_id = preparation.first_kept_entry_id;
    result.tokens_before = preparation.tokens_before;
    result.retained_tail = preparation.retained_tail;

    if (preparation.is_split_turn && !preparation.turn_prefix_messages.empty()) {
        std::string history_text = "No prior history.";
        std::optional<ai::Usage> history_usage;
        if (!preparation.messages_to_summarize.empty()) {
            const std::size_t max_tokens = summary_max_tokens(
                model, preparation.settings.reserve_tokens, 0.8);
            auto history = co_await generate_summary_with_usage(
                preparation.messages_to_summarize,
                model,
                run_options.custom_instructions,
                preparation.previous_summary,
                run_options.thinking_level,
                max_tokens,
                run_options.stop_token,
                run_options.summarization_stream,
                session_id_factory);
            if (!history) {
                co_return std::unexpected(history.error());
            }
            history_text = std::move(history->text);
            history_usage = history->usage;
        }
        const std::size_t prefix_max_tokens = summary_max_tokens(
            model, preparation.settings.reserve_tokens, 0.5);
        auto prefix = co_await generate_turn_prefix_summary(
            preparation.turn_prefix_messages,
            model,
            run_options.thinking_level,
            prefix_max_tokens,
            run_options.stop_token,
            run_options.summarization_stream,
            session_id_factory);
        if (!prefix) {
            co_return std::unexpected(prefix.error());
        }
        result.summary = history_text + "\n\n---\n\n**Turn Context (split turn):**\n\n" +
            prefix->text;
        result.usage = history_usage
            ? combine_usage(*history_usage, prefix->usage)
            : prefix->usage;
    } else {
        const std::size_t max_tokens = summary_max_tokens(
            model, preparation.settings.reserve_tokens, 0.8);
        auto summary = co_await generate_summary_with_usage(
            preparation.messages_to_summarize,
            model,
            run_options.custom_instructions,
            preparation.previous_summary,
            run_options.thinking_level,
            max_tokens,
            run_options.stop_token,
            run_options.summarization_stream,
            session_id_factory);
        if (!summary) {
            co_return std::unexpected(summary.error());
        }
        result.summary = std::move(summary->text);
        result.usage = summary->usage;
    }

    const auto lists = compute_file_lists(preparation.file_ops);
    result.summary += format_file_operations(lists.read_files, lists.modified_files);
    util::JsonValue::object_t details_object;
    util::JsonValue::array_t read_array;
    for (const auto& file : lists.read_files) {
        read_array.emplace_back(file);
    }
    util::JsonValue::array_t modified_array;
    for (const auto& file : lists.modified_files) {
        modified_array.emplace_back(file);
    }
    details_object.emplace("readFiles", std::move(read_array));
    details_object.emplace("modifiedFiles", std::move(modified_array));
    result.details = util::JsonValue{std::move(details_object)};
    co_return result;
}

} // namespace cch::harness::session
