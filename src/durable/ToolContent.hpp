#pragma once

#include <cch/ai/Message.hpp>
#include <cch/chord/Json.hpp>
#include <cch/support/Error.hpp>

#include <span>
#include <string_view>
#include <vector>

namespace cch::durable {

/// Encode one model content block as strict JSON (frozen pi `Content`).
[[nodiscard]] chord::Json content_block_json(const ai::Content& block);
[[nodiscard]] chord::Json content_block_json(const ai::AssistantContent& block);

/// Encode a content list as strict JSON.
[[nodiscard]] chord::Json content_json(std::span<const ai::Content> blocks);
[[nodiscard]] chord::Json content_json(std::span<const ai::AssistantContent> blocks);

/// The call as tool hooks and the transcript see it (frozen pi `ToolCall`): the
/// arguments the call carries, which a `beforeTool` rewrite does not change.
[[nodiscard]] chord::Json tool_call_json(const ai::ToolCallContent& call, const chord::Json& arguments);

/// Decode a tool result or hook content list (frozen pi `Content`): text,
/// image and thinking blocks; anything else is rejected.
[[nodiscard]] support::Expected<std::vector<ai::Content>> content_from_json(const chord::Json& value);

/// The content of a harness-produced tool result: its diagnostic rendered in
/// the frozen `<harness>` block (frozen pi `appendToolResult` of
/// `harnessError`).
[[nodiscard]] std::vector<ai::Content> harness_error_content(std::string_view message);

} // namespace cch::durable
