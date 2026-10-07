#pragma once

#include <cch/ai/Message.hpp>
#include <cch/ai/Model.hpp>
#include <cch/ai/StreamEvent.hpp>
#include <cch/ai/Tool.hpp>
#include <cch/support/Error.hpp>
#include <cch/support/JsonValue.hpp>

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cch::ai::api {

using JsonObject = support::JsonValue::object_t;

/// pi `GrammarConstrainedSampling`: the provider constraint resolved from a
/// tool's `constrainedSampling` config.
struct GrammarConstrainedSampling {
    std::string format;
    std::string definition;
    std::string input_property;
};

/// pi `resolveGrammarConstrainedSampling`: the grammar variant a grammar-capable
/// provider emits, or nullopt when the tool carries none or the provider cannot
/// use one. pi throws on a malformed declaration; the no-exception core falls
/// back to an ordinary function tool instead.
[[nodiscard]] std::optional<GrammarConstrainedSampling> resolve_grammar_constrained_sampling(
        const Tool& tool, bool supports_openai_grammar_tools);

/// Whether the model's compat declares `supportsOpenAIGrammarTools`.
[[nodiscard]] bool model_supports_openai_grammar_tools(const Model& model);

/// pi `createGrammarToolInputProperties`: tool name -> its grammar input
/// property, for the tools that resolve to a grammar constraint.
[[nodiscard]] std::map<std::string, std::string, std::less<>> grammar_tool_input_properties(
        const std::vector<Tool>& tools, const Model& model);

/// pi `GrammarToolInputJsonBuffer`.
struct GrammarToolInputJsonBuffer {
    std::string input;
    bool started{false};
    bool closed{false};
};

/// pi `appendGrammarToolInputJsonDelta`: the JSON fragment to append to the
/// streamed tool-call arguments. Empty when nothing changed; the caller owns
/// the buffer's monotonicity invariant.
[[nodiscard]] std::optional<std::string> append_grammar_tool_input_json_delta(
        GrammarToolInputJsonBuffer& buffer, std::string_view input_property, std::string_view next_input, bool close);

/// One open Responses output slot, keyed by the provider's output_index.
struct Slot {
    enum class Kind {
        Thinking,
        Text,
        ToolCall,
    };

    Kind kind{Kind::Text};
    std::size_t content_index{};
    std::string partial_arguments{};
    /// Set for a grammar-constrained `custom_tool_call` slot: the input
    /// property and the incremental JSON buffer pi streams through.
    std::string custom_property{};
    GrammarToolInputJsonBuffer custom_buffer{};
};

/// Responses slot lifecycle shared with the event processor: index
/// extraction, block creation, delta append, and block finalization.

[[nodiscard]] std::optional<std::size_t> output_index(const JsonObject& event);

[[nodiscard]] support::ExpectedVoid create_slot(std::size_t index,
        const JsonObject& item,
        std::map<std::size_t, Slot>& slots,
        AssistantMessage& assistant,
        AssistantEventSink& sink,
        const std::map<std::string, std::string, std::less<>>* grammar_properties = nullptr);

[[nodiscard]] support::ExpectedVoid append_delta(const JsonObject& event,
        std::string_view type,
        std::map<std::size_t, Slot>& slots,
        AssistantMessage& assistant,
        AssistantEventSink& sink);

[[nodiscard]] support::ExpectedVoid append_reasoning_separator(const JsonObject& event,
        std::map<std::size_t, Slot>& slots,
        AssistantMessage& assistant,
        AssistantEventSink& sink);

[[nodiscard]] support::ExpectedVoid finish_argument_stream(const JsonObject& event,
        std::map<std::size_t, Slot>& slots,
        AssistantMessage& assistant,
        AssistantEventSink& sink);

[[nodiscard]] support::ExpectedVoid finish_output_item(const JsonObject& event,
        std::map<std::size_t, Slot>& slots,
        AssistantMessage& assistant,
        AssistantEventSink& sink);

} // namespace cch::ai::api
