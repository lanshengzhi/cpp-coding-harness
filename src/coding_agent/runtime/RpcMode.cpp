#include "RpcMode.hpp"

#include "AgentSessionPromptAccess.hpp"
#include "JsonEventPrinter.hpp"
#include "RpcJsonl.hpp"

#include "../../../include/cch/ai/Content.hpp"
#include "util/Json.hpp"

#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace cch::coding_agent::runtime {
namespace {

using util::JsonValue;

[[nodiscard]] std::optional<std::string> request_id(const JsonValue::object_t& object) {
    return rpc_jsonl::string_field(object, "id");
}

[[nodiscard]] std::optional<std::string> command_type(const JsonValue::object_t& object) {
    return rpc_jsonl::string_field(object, "type");
}


[[nodiscard]] JsonValue::object_t state_data(const RpcModeConfig& config) {
    JsonValue::object_t data;
    data.emplace("provider", JsonValue{config.provider});
    data.emplace("model", JsonValue{config.model});
    data.emplace("sessionId", JsonValue{config.session.session_id()});
    data.emplace("workspace", JsonValue{config.workspace.string()});
    data.emplace("messageCount", JsonValue{static_cast<int>(config.session.message_count())});
    // pi's RpcSessionState.sessionFile: present for persisted sessions, absent
    // for in-memory operation rather than an empty-path sentinel.
    if (const auto& session_path = config.session.session_path(); session_path.has_value()) {
        data.emplace("sessionFile", JsonValue{session_path->string()});
    }
    return data;
}

[[nodiscard]] JsonValue::object_t last_text_data(const AgentSession& session) {
    JsonValue::object_t data;
    auto text = session.last_assistant_text();
    if (text) {
        data.emplace("text", JsonValue{std::move(*text)});
    } else {
        data.emplace("text", JsonValue{nullptr});
    }
    return data;
}

[[nodiscard]] util::ExpectedVoid flush_output(std::ostream& out) {
    out.flush();
    if (!out) {
        return std::unexpected(util::make_error(
            util::ErrorCode::JsonSerialize,
            "failed to flush RPC JSON record",
            "output stream failed"));
    }
    return {};
}

[[nodiscard]] util::ExpectedVoid write_response(std::ostream& out, JsonValue::object_t record) {
    auto written = rpc_jsonl::write_record(out, std::move(record));
    if (!written) {
        return written;
    }
    return flush_output(out);
}

[[nodiscard]] std::string invalid_command_name(const std::optional<std::string>& type) {
    return type ? *type : std::string{"invalid"};
}

[[nodiscard]] bool is_invalid_envelope(const JsonValue::object_t& object) {
    return !object.contains("type") || rpc_jsonl::has_non_string_field(object, "type") ||
           rpc_jsonl::has_non_string_field(object, "id");
}

[[nodiscard]] std::optional<std::string> prompt_message(const JsonValue::object_t& object) {
    return rpc_jsonl::string_field(object, "message");
}

} // namespace

int run_rpc_mode(RpcModeConfig config) {
    struct PendingPromptResponse {
        std::optional<std::string> id;
        bool accepted{false};
    };

    JsonEventPrinter printer(config.output);
    std::optional<PendingPromptResponse> pending_prompt;
    std::optional<util::Error> output_error;
    std::optional<EventSubscription> event_subscription;
    auto subscribed = config.session.subscribe(
        [&](const agent::AgentLifecycleEvent& event) -> util::ExpectedVoid {
            auto printed = printer.print_agent_event(event);
            if (!printed) {
                output_error = printed.error();
                return printed;
            }
            auto flushed = flush_output(config.output);
            if (!flushed) {
                output_error = flushed.error();
            }
            return flushed;
        });
    if (subscribed) {
        event_subscription.emplace(std::move(*subscribed));
    }

    std::string line;
    while (std::getline(config.input, line)) {
        line = rpc_jsonl::strip_trailing_cr(std::move(line));
        if (line.empty()) {
            if (auto written = write_response(
                    config.output,
                    rpc_jsonl::error_response(std::nullopt, "invalid", "empty JSONL record"));
                !written) {
                return 1;
            }
            continue;
        }

        auto parsed = util::read_json<JsonValue>(line);
        if (!parsed) {
            if (auto written = write_response(
                    config.output,
                    rpc_jsonl::error_response(std::nullopt, "parse", "failed to parse JSON"));
                !written) {
                return 1;
            }
            continue;
        }

        const auto* object = parsed->get_if<JsonValue::object_t>();
        if (object == nullptr) {
            if (auto written = write_response(
                    config.output,
                    rpc_jsonl::error_response(std::nullopt, "invalid", "RPC command must be a JSON object"));
                !written) {
                return 1;
            }
            continue;
        }

        const auto id = request_id(*object);
        const auto type = command_type(*object);
        if (is_invalid_envelope(*object)) {
            if (auto written = write_response(
                    config.output,
                    rpc_jsonl::error_response(id, invalid_command_name(type), "invalid RPC command envelope"));
                !written) {
                return 1;
            }
            continue;
        }

        if (*type == "get_state") {
            if (auto written = write_response(
                    config.output,
                    rpc_jsonl::success_response(id, *type, state_data(config)));
                !written) {
                return 1;
            }
            continue;
        }

        if (*type == "get_last_assistant_text") {
            if (auto written = write_response(
                    config.output,
                    rpc_jsonl::success_response(id, *type, last_text_data(config.session)));
                !written) {
                return 1;
            }
            continue;
        }

        if (*type == "shutdown") {
            if (auto written = write_response(config.output, rpc_jsonl::success_response(id, *type)); !written) {
                return 1;
            }
            return 0;
        }

        if (*type == "prompt") {
            auto message = prompt_message(*object);
            if (!message || message->empty()) {
                if (auto written = write_response(
                        config.output,
                        rpc_jsonl::error_response(id, *type, "prompt.message must be a non-empty string"));
                    !written) {
                    return 1;
                }
                continue;
            }

            pending_prompt = PendingPromptResponse{.id = id};
            auto result = detail::AgentSessionPromptAccess::prompt_blocking(
                config.session,
                std::move(*message),
                true,
                [&]() -> util::ExpectedVoid {
                    auto written = write_response(
                        config.output,
                        rpc_jsonl::success_response(pending_prompt->id, "prompt"));
                    if (!written) {
                        output_error = written.error();
                        return written;
                    }
                    pending_prompt->accepted = true;
                    return {};
                });
            if (output_error) {
                return 1;
            }
            if (!result) {
                if (!pending_prompt->accepted) {
                    if (auto written = write_response(
                            config.output,
                            rpc_jsonl::error_response(id, *type, result.error().message));
                        !written) {
                        return 1;
                    }
                }
                pending_prompt.reset();
                continue;
            }
            if (!pending_prompt->accepted) {
                return 1;
            }
            pending_prompt.reset();
            continue;
        }

        if (auto written = write_response(
                config.output,
                rpc_jsonl::error_response(id, *type, "unsupported RPC command in C++ v1"));
            !written) {
            return 1;
        }
    }

    return 0;
}

} // namespace cch::coding_agent::runtime
