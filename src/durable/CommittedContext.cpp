#include "durable/CommittedContext.hpp"

#include "support/Json.hpp"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <cstddef>
#include <format>
#include <limits>
#include <map>
#include <string>
#include <string_view>

namespace cch::durable {
namespace {

using Json = chord::Json;
using Object = Json::Object;
using Array = Json::Array;

std::string_view message_role(const Json& message) {
    const auto* object = message.as_object();
    if (object == nullptr) return {};
    const auto role = object->find("role");
    if (role == object->end()) return {};
    const std::string* value = role->second.as_string();
    return value == nullptr ? std::string_view{} : std::string_view{*value};
}

/// Text blocks joined, or the plain string content (frozen pi `contentText`).
std::string content_text(const Json& content) {
    if (const std::string* text = content.as_string()) return *text;
    const auto* blocks = content.as_array();
    if (blocks == nullptr) return {};
    std::string text;
    for (const Json& block : *blocks) {
        const auto* object = block.as_object();
        if (object == nullptr) continue;
        const auto type = object->find("type");
        const auto value = object->find("text");
        if (type == object->end() || value == object->end()) continue;
        const std::string* kind = type->second.as_string();
        const std::string* body = value->second.as_string();
        if (kind == nullptr || body == nullptr || *kind != "text") continue;
        if (!text.empty()) text += "\n";
        text += *body;
    }
    return text;
}

std::int64_t estimate_chars(std::size_t chars) {
    return static_cast<std::int64_t>(std::ceil(static_cast<double>(chars) / 3.5));
}

std::int64_t estimate_json_chars(const Json& value) {
    const auto text = support::write_json(value.to_support());
    return estimate_chars(text ? text->size() : 2);
}

/// The estimated tokens of one content block array (frozen pi
/// `estimateMessageTokens` over block content).
std::int64_t estimate_blocks_tokens(const Json& content) {
    const auto* blocks = content.as_array();
    if (blocks == nullptr) return estimate_chars(content_text(content).size());
    std::int64_t tokens = 0;
    for (const Json& block : *blocks) {
        const auto* object = block.as_object();
        if (object == nullptr) continue;
        const auto type = object->find("type");
        if (type == object->end()) continue;
        const std::string* kind = type->second.as_string();
        if (kind == nullptr) continue;
        if (*kind == "text" || *kind == "thinking") {
            const auto text = object->find("text");
            if (text == object->end()) continue;
            if (const std::string* body = text->second.as_string()) tokens += estimate_chars(body->size());
        } else if (*kind == "image") {
            tokens += 4800;
        } else if (*kind == "toolCall") {
            const auto name = object->find("name");
            if (name != object->end()) {
                if (const std::string* value = name->second.as_string()) tokens += estimate_chars(value->size());
            }
            const auto arguments = object->find("arguments");
            if (arguments != object->end()) tokens += estimate_json_chars(arguments->second);
        }
    }
    return tokens;
}

/// Context tokens of one usage object: `totalTokens` or the sum of the token
/// columns (frozen pi `calculateContextTokens`).
std::int64_t usage_context_tokens(const Json& usage) {
    const auto* object = usage.as_object();
    if (object == nullptr) return 0;
    const auto total = object->find("totalTokens");
    if (total != object->end()) {
        if (const double* value = total->second.as_number(); value != nullptr && *value > 0) {
            return static_cast<std::int64_t>(*value);
        }
    }
    std::int64_t sum = 0;
    for (std::string_view key : {"input", "output", "cacheRead", "cacheWrite"}) {
        const auto found = object->find(std::string{key});
        if (found == object->end()) continue;
        if (const double* value = found->second.as_number()) sum += static_cast<std::int64_t>(*value);
    }
    return sum;
}

/// A synthesized result for a tool call whose result is not in the kept
/// range (frozen pi `missingResult`).
Json missing_result(const Json& call, const Json& assistant) {
    const Object& call_object = *call.as_object();
    Object message{{"role", "toolResult"},
            {"toolCallId", call_object.at("id")},
            {"toolName", call_object.at("name")},
            {"content",
                    Json{Array{Json{Object{{"type", "text"},
                            {"text", "Tool result unavailable: history ends before this call completed."}}}}}},
            {"isError", true},
            {"details", Json{Object{{"reason", "missing_result"}}}}};
    const auto* assistant_object = assistant.as_object();
    if (assistant_object != nullptr) {
        const auto timestamp = assistant_object->find("timestamp");
        if (timestamp != assistant_object->end()) message.emplace("timestamp", timestamp->second);
    }
    return Json{std::move(message)};
}

std::vector<Json> order_tool_results(const std::vector<Json>& messages) {
    std::vector<Json> ordered;
    for (std::size_t index = 0; index < messages.size(); ++index) {
        const Json& message = messages[index];
        if (message_role(message) == "toolResult") continue;
        ordered.push_back(message);
        if (message_role(message) != "assistant") continue;
        std::vector<const Json*> calls;
        if (const auto* content = message.as_object()) {
            const auto blocks = content->find("content");
            if (blocks != content->end()) {
                if (const auto* array = blocks->second.as_array()) {
                    for (const Json& block : *array) {
                        const auto* object = block.as_object();
                        if (object == nullptr) continue;
                        const auto type = object->find("type");
                        if (type == object->end() || type->second.as_string() == nullptr) continue;
                        if (*type->second.as_string() == "toolCall" && object->contains("id") &&
                                object->contains("name"))
                            calls.push_back(&block);
                    }
                }
            }
        }
        if (calls.empty()) continue;
        std::map<std::string, std::size_t> results;
        for (std::size_t next = index + 1; next < messages.size() && message_role(messages[next]) != "assistant";
                ++next) {
            const Json& candidate = messages[next];
            if (message_role(candidate) != "toolResult") continue;
            const auto* object = candidate.as_object();
            if (object == nullptr) continue;
            const auto id = object->find("toolCallId");
            if (id == object->end()) continue;
            const std::string* call_id = id->second.as_string();
            if (call_id == nullptr || results.contains(*call_id)) continue;
            results.emplace(*call_id, next);
        }
        for (const Json* call : calls) {
            const auto* object = call->as_object();
            const auto id = object->find("id");
            const std::string* call_id = id == object->end() ? nullptr : id->second.as_string();
            const auto found = call_id == nullptr ? results.end() : results.find(*call_id);
            if (found == results.end()) {
                ordered.push_back(missing_result(*call, message));
            } else {
                ordered.push_back(messages[found->second]);
            }
        }
    }
    return ordered;
}

/// The tool call ids one assistant message started (frozen pi `selectCut`
/// candidate scan).
std::vector<std::string> assistant_call_ids(const std::vector<Json>& contribution) {
    for (auto it = contribution.rbegin(); it != contribution.rend(); ++it) {
        if (message_role(*it) != "assistant") continue;
        std::vector<std::string> ids;
        const auto* object = it->as_object();
        if (object == nullptr) return ids;
        const auto blocks = object->find("content");
        if (blocks == object->end()) return ids;
        const auto* array = blocks->second.as_array();
        if (array == nullptr) return ids;
        for (const Json& block : *array) {
            const auto* block_object = block.as_object();
            if (block_object == nullptr) continue;
            const auto type = block_object->find("type");
            if (type == block_object->end() || type->second.as_string() == nullptr) continue;
            if (*type->second.as_string() != "toolCall") continue;
            const auto id = block_object->find("id");
            if (id == block_object->end()) continue;
            if (const std::string* call_id = id->second.as_string()) ids.push_back(*call_id);
        }
        return ids;
    }
    return {};
}

/// Whether the entry at `index` may start the kept range: an entry whose
/// contribution starts with a user or assistant message, never a tool result,
/// and never a user entry that a result of the preceding assistant's calls
/// still follows (frozen pi `selectCut.isCandidate`).
bool is_candidate(const std::vector<std::vector<Json>>& contributions, std::size_t index) {
    if (index >= contributions.size() || contributions[index].empty()) return false;
    const std::string_view first = message_role(contributions[index].front());
    if (first == "assistant") return true;
    if (first != "user") return false;
    std::vector<std::string> calls;
    for (std::size_t before = index; before-- > 0;) {
        const auto& contribution = contributions[before];
        if (std::ranges::any_of(
                    contribution, [](const Json& message) { return message_role(message) == "assistant"; })) {
            calls = assistant_call_ids(contribution);
            break;
        }
    }
    if (calls.empty()) return true;
    for (std::size_t after = index; after < contributions.size(); ++after) {
        for (std::size_t position = 0; position < contributions[after].size(); ++position) {
            const std::string_view role = message_role(contributions[after][position]);
            if (role == "assistant" && (after > index || position > 0)) return true;
            if (role == "toolResult") {
                const auto* object = contributions[after][position].as_object();
                if (object == nullptr) continue;
                const auto id = object->find("toolCallId");
                if (id == object->end()) continue;
                const std::string* call_id = id->second.as_string();
                if (call_id != nullptr && std::ranges::find(calls, *call_id) != calls.end()) return false;
            }
        }
    }
    return true;
}

std::string truncate_chars(std::string_view text, std::size_t max_chars) {
    if (text.size() <= max_chars) return std::string{text};
    return std::format("{}\n\n[... {} more characters truncated]", text.substr(0, max_chars), text.size() - max_chars);
}

} // namespace

support::Expected<CommittedContext> derive_committed_context(
        Session& session, ConversationId conversation_id, std::optional<EntryId> at) {
    std::optional<CommittedContext> view;
    std::mutex mutex;
    std::condition_variable ready;
    std::optional<support::ExpectedVoid> outcome;
    auto operation = session.commit([&](Transaction& tx) -> support::ExpectedVoid {
        auto loaded = tx.context_view(conversation_id, at);
        if (!loaded) return std::unexpected(loaded.error());
        view = std::move(*loaded);
        return {};
    });
    std::move(operation).start([&](support::ExpectedVoid result) noexcept {
        std::lock_guard lock(mutex);
        outcome.emplace(std::move(result));
        ready.notify_one();
    });
    std::unique_lock lock(mutex);
    ready.wait(lock, [&] { return outcome.has_value(); });
    if (!*outcome) return std::unexpected(outcome->error());
    return std::move(*view);
}

std::int64_t estimate_message_tokens(const Json& message) {
    const auto* object = message.as_object();
    if (object == nullptr) return 0;
    std::int64_t tokens = 0;
    const std::string_view role = message_role(message);
    if (role == "system") {
        const auto content = object->find("content");
        if (content != object->end()) {
            if (const std::string* text = content->second.as_string()) tokens += estimate_chars(text->size());
        }
        const auto tools = object->find("toolsAdded");
        tokens += estimate_json_chars(tools == object->end() ? Json{Array{}} : tools->second);
        return tokens;
    }
    const auto content = object->find("content");
    tokens += estimate_blocks_tokens(content == object->end() ? Json{} : content->second);
    if (role == "toolResult") {
        const auto id = object->find("toolCallId");
        if (id != object->end()) {
            if (const std::string* call_id = id->second.as_string()) tokens += estimate_chars(call_id->size());
        }
    }
    return tokens;
}

std::int64_t estimate_context_tokens(const CommittedContext& context, const std::vector<Json>& extra) {
    // The usage of the newest assistant appended after the head marker, whose
    // request included the marker (frozen pi `estimateContext`).
    const Json* measured = nullptr;
    const std::int64_t after =
            context.head.has_value() ? context.head->id.value : std::numeric_limits<std::int64_t>::min();
    for (std::size_t index = context.entries.size(); index-- > 0 && measured == nullptr;) {
        if (context.entries[index].id.value <= after) continue;
        for (auto it = context.contributions[index].rbegin(); it != context.contributions[index].rend(); ++it) {
            if (message_role(*it) != "assistant") continue;
            const auto* object = it->as_object();
            if (object == nullptr) continue;
            const auto usage = object->find("usage");
            if (usage == object->end() || usage_context_tokens(usage->second) <= 0) continue;
            measured = &*it;
            break;
        }
    }
    std::int64_t tokens = 0;
    std::size_t from = 0;
    if (measured != nullptr) {
        const auto* object = measured->as_object();
        const auto usage = object->find("usage");
        tokens += usage_context_tokens(usage->second);
        for (std::size_t index = context.messages.size(); index-- > 0;) {
            if (context.messages[index] != *measured) continue;
            from = index + 1;
            break;
        }
    }
    for (std::size_t index = from; index < context.messages.size(); ++index) {
        tokens += estimate_message_tokens(context.messages[index]);
    }
    for (const Json& message : extra)
        tokens += estimate_message_tokens(message);
    return tokens;
}

std::optional<std::size_t> select_cut(const CommittedContext& context, const std::int64_t keep_recent_tokens) {
    const std::vector<std::vector<Json>>& contributions = context.contributions;
    const std::size_t start = context.head.has_value() ? 1 : 0;
    std::vector<std::size_t> candidates;
    for (std::size_t index = start; index < contributions.size(); ++index) {
        if (is_candidate(contributions, index)) candidates.push_back(index);
    }
    if (candidates.empty()) return std::nullopt;
    std::int64_t kept = 0;
    std::optional<std::size_t> cut;
    for (std::size_t index = contributions.size(); index-- > start;) {
        for (const Json& message : contributions[index])
            kept += estimate_message_tokens(message);
        if (kept < keep_recent_tokens) continue;
        const auto first = std::ranges::lower_bound(candidates, index);
        cut = first != candidates.end() ? std::optional<std::size_t>{*first}
                                        : std::optional<std::size_t>{candidates.back()};
        break;
    }
    if (!cut.has_value()) return std::nullopt;
    for (std::size_t index = start; index < *cut; ++index) {
        if (!contributions[index].empty()) return cut;
    }
    return std::nullopt;
}

std::vector<Json> summarized_messages(const CommittedContext& context, const std::size_t cut) {
    std::vector<Json> flat;
    const std::size_t end = std::min(cut, context.contributions.size());
    for (std::size_t index = 0; index < end; ++index) {
        for (const Json& message : context.contributions[index])
            flat.push_back(message);
    }
    return order_tool_results(flat);
}

std::string serialize_conversation(const std::vector<Json>& messages) {
    constexpr std::size_t kToolResultMaxChars = 2000;
    std::vector<std::string> parts;
    for (const Json& message : messages) {
        const auto* object = message.as_object();
        if (object == nullptr) continue;
        const auto content = object->find("content");
        const Json body = content == object->end() ? Json{} : content->second;
        const std::string_view role = message_role(message);
        if (role == "user") {
            const std::string text = content_text(body);
            if (!text.empty()) parts.push_back(std::format("[User]: {}", text));
        } else if (role == "assistant") {
            std::vector<std::string> texts;
            std::vector<std::string> calls;
            const auto* blocks = body.as_array();
            if (blocks != nullptr) {
                for (const Json& block : *blocks) {
                    const auto* block_object = block.as_object();
                    if (block_object == nullptr) continue;
                    const auto type = block_object->find("type");
                    if (type == block_object->end() || type->second.as_string() == nullptr) continue;
                    if (*type->second.as_string() == "text") {
                        const auto text = block_object->find("text");
                        if (text != block_object->end()) {
                            if (const std::string* value = text->second.as_string()) texts.push_back(*value);
                        }
                    } else if (*type->second.as_string() == "toolCall") {
                        const auto name = block_object->find("name");
                        const auto arguments = block_object->find("arguments");
                        if (name == block_object->end() || arguments == block_object->end()) continue;
                        const std::string* call_name = name->second.as_string();
                        if (call_name == nullptr) continue;
                        std::string rendered_arguments;
                        if (const auto* arguments_object = arguments->second.as_object()) {
                            std::vector<std::string> pairs;
                            for (const auto& [key, value] : *arguments_object) {
                                const auto encoded = support::write_json(value.to_support());
                                pairs.push_back(std::format("{}={}", key, encoded.value_or("null")));
                            }
                            for (std::size_t pair = 0; pair < pairs.size(); ++pair) {
                                if (pair > 0) rendered_arguments += ", ";
                                rendered_arguments += pairs[pair];
                            }
                        } else {
                            rendered_arguments = support::write_json(arguments->second.to_support()).value_or("null");
                        }
                        calls.push_back(std::format("{}({})", *call_name, rendered_arguments));
                    }
                }
            }
            if (!texts.empty()) {
                std::string joined;
                for (const std::string& text : texts) {
                    if (!joined.empty()) joined += "\n";
                    joined += text;
                }
                parts.push_back(std::format("[Assistant]: {}", joined));
            }
            if (!calls.empty()) {
                std::string joined;
                for (const std::string& call : calls) {
                    if (!joined.empty()) joined += "; ";
                    joined += call;
                }
                parts.push_back(std::format("[Assistant tool calls]: {}", joined));
            }
        } else if (role == "toolResult") {
            const std::string text = content_text(body);
            if (!text.empty())
                parts.push_back(std::format("[Tool result]: {}", truncate_chars(text, kToolResultMaxChars)));
        }
    }
    std::string serialized;
    for (const std::string& part : parts) {
        if (!serialized.empty()) serialized += "\n\n";
        serialized += part;
    }
    return serialized;
}

} // namespace cch::durable
