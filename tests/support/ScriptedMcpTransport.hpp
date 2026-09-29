#pragma once

#include <cch/mcp/McpTransport.hpp>
#include <cch/support/Error.hpp>
#include <cch/support/JsonValue.hpp>
#include "mcp/JsonRpc.hpp"
#include "support/Json.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
namespace cch::tests {

/// What a scripted Upstream MCP Server answers one request with.
struct ScriptedMcpAnswer {
    /// The JSON-RPC `result` of a conforming answer.
    support::JsonValue result{support::JsonValue::object_t{}};
    /// Set to answer with a JSON-RPC error instead of a result.
    int error_code{0};
    std::string error_message{};
    /// Framed JSON-RPC messages placed ahead of the answer, as an Upstream is
    /// free to send a notification before the response it belongs to.
    std::vector<support::JsonValue> leading_messages{};
    int status_code{200};
    /// Set to fail the exchange at the transport instead of answering, which
    /// is how a cancellation and a network loss reach the client stack.
    std::optional<support::Error> transport_error{};
    /// A raw body that replaces the framed answer entirely, for a body the
    /// framing rules must reject.
    std::optional<std::string> raw_body{};
};

/// The one MCP Host test seam, filled from memory (issue #836). Production
/// supplies the Streamable HTTP transport; this records every request it was
/// asked to send and answers from a per-method script, so a test asserts the
/// exact bytes the client stack framed rather than its internal values.
///
/// A scripted answer echoes the request's own id, exactly as a conforming
/// Upstream does, so nothing in the test needs to predict the client's ids.
class ScriptedMcpTransport final : public mcp::McpTransport {
public:
    /// Answer every request for `method` with a fixed answer, replacing any
    /// answer a test already scripted for that method.
    void answer(std::string_view method, ScriptedMcpAnswer value) {
        answer_with(method, [value = std::move(value)](const support::JsonValue&) { return value; });
    }

    /// Answer every request for `method` from the request's own `params`, for
    /// a script that has to react to a pagination cursor. Replaces any answer
    /// a test already scripted for that method.
    void answer_with(
            std::string_view method, std::function<ScriptedMcpAnswer(const support::JsonValue& params)> handler) {
        const std::string name(method);
        for (auto& [scripted, existing] : script_) {
            if (scripted == name) {
                existing = std::move(handler);
                return;
            }
        }
        script_.push_back({name, std::move(handler)});
    }

    [[nodiscard]] const std::vector<mcp::McpRequest>& requests() const noexcept { return requests_; }

    [[nodiscard]] std::size_t request_count() const noexcept { return requests_.size(); }

    /// The JSON-RPC method the request at `index` carries.
    [[nodiscard]] support::Expected<std::string> recorded_method(std::size_t index) const {
        auto recorded = record(index);
        if (!recorded) {
            return std::unexpected(recorded.error());
        }
        return recorded->method;
    }

    /// The decoded `params` object the request at `index` carries.
    [[nodiscard]] support::Expected<support::JsonValue> recorded_params(std::size_t index) const {
        auto recorded = record(index);
        if (!recorded) {
            return std::unexpected(recorded.error());
        }
        return recorded->params;
    }

    [[nodiscard]] cch::support::AsyncResult<mcp::McpResponse> send(mcp::McpRequest request) override {
        const auto index = requests_.size();
        requests_.push_back(request);
        const auto answer = build_answer(requests_.at(index));
        if (!answer) {
            return cch::support::AsyncResult<mcp::McpResponse>(
                    std::expected<mcp::McpResponse, support::Error>{std::unexpected(answer.error())});
        }
        return cch::support::AsyncResult<mcp::McpResponse>(
                std::expected<mcp::McpResponse, support::Error>{mcp::McpResponse{
                        .status_code = answer->status_code,
                        .headers = {},
                        .body = answer->raw_body.value_or(answer->body),
                }});
    }

private:
    struct ScriptedResponse {
        int status_code{200};
        std::string body{};
        std::optional<std::string> raw_body{};
    };

    [[nodiscard]] support::Expected<mcp::jsonrpc::WireMessage> record(std::size_t index) const {
        auto parsed = support::read_json(requests_.at(index).body);
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        return mcp::jsonrpc::decode_message(*parsed);
    }

    [[nodiscard]] support::Expected<ScriptedResponse> build_answer(const mcp::McpRequest& request) const {
        auto decoded = mcp::jsonrpc::decode_message(*support::read_json(request.body));
        if (!decoded) {
            return std::unexpected(decoded.error());
        }
        const auto* handler = find(decoded->method);
        if (handler == nullptr) {
            return std::unexpected(support::make_error(support::ErrorCode::Validation,
                    "the scripted MCP transport has no answer for the request method",
                    "method \"" + decoded->method + "\""));
        }
        const auto answer = (*handler)(decoded->params);
        if (answer.transport_error.has_value()) {
            return std::unexpected(*answer.transport_error);
        }
        ScriptedResponse response;
        response.status_code = answer.status_code;
        response.raw_body = answer.raw_body;
        response.body = answer.leading_messages.empty() ? "" : frame(answer.leading_messages);
        const auto id = decoded->id.value_or(0.0);
        const auto tail =
                answer.error_code == 0
                        ? frame(std::vector<support::JsonValue>{mcp::jsonrpc::encode_result(id, answer.result)})
                        : frame(std::vector<support::JsonValue>{mcp::jsonrpc::encode_error(
                                  id, answer.error_code, answer.error_message, support::JsonValue{})});
        response.body += tail;
        return response;
    }

    [[nodiscard]] static std::string frame(const std::vector<support::JsonValue>& messages) {
        std::string body;
        for (const auto& message : messages) {
            const auto text = support::write_json(message);
            if (text) {
                body += *text;
            }
        }
        return body;
    }

    using Handler = std::function<ScriptedMcpAnswer(const support::JsonValue& params)>;

    [[nodiscard]] const Handler* find(std::string_view method) const {
        for (const auto& [name, handler] : script_) {
            if (name == method) {
                return &handler;
            }
        }
        return nullptr;
    }

    std::vector<std::pair<std::string, Handler>> script_{};
    std::vector<mcp::McpRequest> requests_{};
};

/// A well-formed Modern Era `server/discover` result. A test overrides only the
/// members whose case it is exercising.
[[nodiscard]] inline support::JsonValue discover_result(std::string name = "executor",
        std::string version = "1.4.0",
        std::string instructions = "Use the codemode tools.") {
    using JsonValue = support::JsonValue;
    return JsonValue::object_t{
            {"protocolVersion", JsonValue("2026-07-28")},
            {"serverInfo",
                    JsonValue::object_t{
                            {"name", JsonValue(std::move(name))}, {"version", JsonValue(std::move(version))}}},
            {"capabilities", JsonValue::object_t{{"tools", JsonValue::object_t{{"listChanged", JsonValue(true)}}}}},
            {"instructions", JsonValue(std::move(instructions))},
    };
}

/// A `tools/list` result carrying `tools` and an optional `nextCursor`.
[[nodiscard]] inline support::JsonValue tool_list_result(
        std::vector<support::JsonValue> tools, std::optional<std::string> next_cursor = std::nullopt) {
    using JsonValue = support::JsonValue;
    JsonValue::object_t result{{"tools", JsonValue(std::move(tools))}};
    if (next_cursor.has_value()) {
        result.emplace("nextCursor", JsonValue(*next_cursor));
    }
    return JsonValue(std::move(result));
}

/// One `tools/list` tool entry.
[[nodiscard]] inline support::JsonValue tool_entry(
        std::string name, std::vector<std::string> properties = {}, support::JsonValue annotations = nullptr) {
    using JsonValue = support::JsonValue;
    JsonValue::object_t input_schema{{"type", JsonValue("object")}, {"properties", JsonValue::object_t{}}};
    if (!properties.empty()) {
        JsonValue::object_t declared;
        for (auto& property : properties) {
            declared.emplace(std::move(property), JsonValue::object_t{{"type", JsonValue("string")}});
        }
        input_schema["properties"] = JsonValue(std::move(declared));
    }
    JsonValue::object_t entry{
            {"name", JsonValue(std::move(name))},
            {"description", JsonValue("an upstream tool")},
            {"inputSchema", JsonValue(std::move(input_schema))},
    };
    if (!annotations.holds<JsonValue::null_t>()) {
        entry.emplace("annotations", std::move(annotations));
    }
    return JsonValue(std::move(entry));
}

/// A well-formed `tools/call` result.
[[nodiscard]] inline support::JsonValue tool_call_result(support::JsonValue content, bool is_error = false) {
    using JsonValue = support::JsonValue;
    return JsonValue::object_t{
            {"resultType", JsonValue("call_result")},
            {"content", std::move(content)},
            {"isError", JsonValue(is_error)},
    };
}

/// A `notifications/tools/list_changed` message, which v1 ignores wherever it
/// arrives.
[[nodiscard]] inline support::JsonValue tools_list_changed_notification() {
    using JsonValue = support::JsonValue;
    return JsonValue::object_t{
            {"jsonrpc", JsonValue("2.0")},
            {"method", JsonValue("notifications/tools/list_changed")},
    };
}

/// Drive one `AsyncResult` to its terminal outcome. The scripted transport
/// completes inline, so a driven operation is finished before `drive` returns
/// and the borrowed `outcome` below never outlives the call; the initial error
/// is a sentinel that an operation which failed to complete would leave
/// behind (CODING_STANDARDS.md §7.5).
template <typename T> [[nodiscard]] support::Expected<T> drive(cch::support::AsyncResult<T> operation) {
    support::Expected<T> outcome = std::unexpected(support::make_error(
            support::ErrorCode::Busy, "the operation never completed", "the scripted transport completes inline"));
    operation.start(
            [&outcome](std::expected<T, support::Error> result) mutable noexcept { outcome = std::move(result); });
    return outcome;
}

} // namespace cch::tests
