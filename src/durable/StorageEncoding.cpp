#include "StorageEncoding.hpp"

#include <cmath>
#include <limits>
#include <type_traits>
#include <utility>
#include <variant>

namespace cch::durable {
namespace {

[[nodiscard]] support::Expected<chord::Json> decode_json_value(const support::JsonValue& value) {
    return chord::Json::from_support(value);
}

[[nodiscard]] support::Error corruption(std::string message) {
    return support::make_error(support::ErrorCode::Validation, std::move(message));
}

} // namespace

std::optional<std::int64_t> nonnegative_integer(const support::JsonValue& value) {
    const auto* number = value.get_if<double>();
    if (number == nullptr || !std::isfinite(*number) || *number < 0 ||
            *number > static_cast<double>(std::numeric_limits<std::int64_t>::max()) || std::trunc(*number) != *number) {
        return std::nullopt;
    }
    return static_cast<std::int64_t>(*number);
}

support::JsonValue number(std::int64_t value) { return support::JsonValue{static_cast<double>(value)}; }

support::JsonValue number(std::size_t value) { return support::JsonValue{static_cast<double>(value)}; }

support::JsonValue path_value(const chord::Path& path) {
    support::JsonValue::array_t result;
    result.reserve(path.size());
    for (const chord::PathSegment& segment : path) {
        if (const auto* key = std::get_if<chord::PathKey>(&segment)) {
            result.emplace_back(key->key);
        } else {
            result.emplace_back(number(std::get<chord::PathIndex>(segment).index));
        }
    }
    return support::JsonValue{std::move(result)};
}

support::JsonValue op_value(const chord::Op& op) {
    return std::visit(
            [](const auto& operation) -> support::JsonValue {
                using T = std::decay_t<decltype(operation)>;
                if constexpr (std::is_same_v<T, chord::ReplaceOp>) {
                    return support::JsonValue{
                            support::JsonValue::array_t{support::JsonValue{"r"}, operation.value.to_support()}};
                } else if constexpr (std::is_same_v<T, chord::SetOp>) {
                    return support::JsonValue{support::JsonValue::array_t{
                            support::JsonValue{"s"}, path_value(operation.path), operation.value.to_support()}};
                } else if constexpr (std::is_same_v<T, chord::DeleteOp>) {
                    return support::JsonValue{
                            support::JsonValue::array_t{support::JsonValue{"d"}, path_value(operation.path)}};
                } else if constexpr (std::is_same_v<T, chord::AppendOp>) {
                    return support::JsonValue{support::JsonValue::array_t{
                            support::JsonValue{"a"}, path_value(operation.path), support::JsonValue{operation.text}}};
                } else if constexpr (std::is_same_v<T, chord::TruncateOp>) {
                    return support::JsonValue{support::JsonValue::array_t{
                            support::JsonValue{"t"}, path_value(operation.path), number(operation.count)}};
                } else if constexpr (std::is_same_v<T, chord::SpliceOp>) {
                    support::JsonValue::array_t items;
                    items.reserve(operation.items.size());
                    for (const chord::Json& item : operation.items) {
                        items.push_back(item.to_support());
                    }
                    return support::JsonValue{support::JsonValue::array_t{support::JsonValue{"p"},
                            path_value(operation.path),
                            number(operation.index),
                            number(operation.remove),
                            support::JsonValue{std::move(items)}}};
                } else {
                    support::JsonValue::array_t permutation;
                    permutation.reserve(operation.permutation.size());
                    for (const std::size_t index : operation.permutation) {
                        permutation.push_back(number(index));
                    }
                    return support::JsonValue{support::JsonValue::array_t{support::JsonValue{"m"},
                            path_value(operation.path),
                            support::JsonValue{std::move(permutation)}}};
                }
            },
            op);
}

support::JsonValue ops_value(const chord::OpBatch& ops) {
    support::JsonValue::array_t encoded;
    encoded.reserve(ops.size());
    for (const chord::Op& op : ops) {
        encoded.push_back(op_value(op));
    }
    return support::JsonValue{std::move(encoded)};
}

support::Expected<chord::Path> decode_path(const support::JsonValue& value) {
    const auto* array = value.get_if<support::JsonValue::array_t>();
    if (array == nullptr) {
        return std::unexpected(corruption("Invalid path"));
    }
    chord::Path path;
    path.reserve(array->size());
    for (const support::JsonValue& segment : *array) {
        if (const auto* key = segment.get_if<std::string>()) {
            path.push_back(chord::path_key(*key));
            continue;
        }
        const auto index = nonnegative_integer(segment);
        if (!index.has_value() || static_cast<std::uint64_t>(*index) >
                                          static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
            return std::unexpected(corruption("Invalid path"));
        }
        path.push_back(chord::path_index(static_cast<std::size_t>(*index)));
    }
    return path;
}

support::Expected<chord::OpBatch> decode_ops(const support::JsonValue& value) {
    const auto* array = value.get_if<support::JsonValue::array_t>();
    if (array == nullptr) {
        return std::unexpected(corruption("Invalid document operations"));
    }
    chord::OpBatch ops;
    ops.reserve(array->size());
    for (const support::JsonValue& encoded : *array) {
        const auto* tuple = encoded.get_if<support::JsonValue::array_t>();
        if (tuple == nullptr || tuple->empty()) {
            return std::unexpected(corruption("Invalid document operation"));
        }
        const auto* verb = tuple->front().get_if<std::string>();
        if (verb == nullptr) {
            return std::unexpected(corruption("Invalid document operation"));
        }
        if (*verb == "r" && tuple->size() == 2) {
            auto replacement = decode_json_value((*tuple)[1]);
            if (!replacement) {
                return std::unexpected(std::move(replacement.error()));
            }
            ops.push_back(chord::ReplaceOp{.value = std::move(*replacement)});
        } else if (*verb == "s" && tuple->size() == 3) {
            auto path = decode_path((*tuple)[1]);
            auto item = decode_json_value((*tuple)[2]);
            if (!path || !item) {
                return std::unexpected(corruption("Invalid document operation"));
            }
            ops.push_back(chord::SetOp{.path = std::move(*path), .value = std::move(*item)});
        } else if (*verb == "d" && tuple->size() == 2) {
            auto path = decode_path((*tuple)[1]);
            if (!path) {
                return std::unexpected(std::move(path.error()));
            }
            ops.push_back(chord::DeleteOp{.path = std::move(*path)});
        } else if (*verb == "a" && tuple->size() == 3) {
            auto path = decode_path((*tuple)[1]);
            const auto* text = (*tuple)[2].get_if<std::string>();
            if (!path || text == nullptr) {
                return std::unexpected(corruption("Invalid document operation"));
            }
            ops.push_back(chord::AppendOp{.path = std::move(*path), .text = *text});
        } else if (*verb == "t" && tuple->size() == 3) {
            auto path = decode_path((*tuple)[1]);
            const auto count = nonnegative_integer((*tuple)[2]);
            if (!path || !count ||
                    static_cast<std::uint64_t>(*count) >
                            static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
                return std::unexpected(corruption("Invalid document operation"));
            }
            ops.push_back(chord::TruncateOp{.path = std::move(*path), .count = static_cast<std::size_t>(*count)});
        } else if (*verb == "p" && tuple->size() == 5) {
            auto path = decode_path((*tuple)[1]);
            const auto index = nonnegative_integer((*tuple)[2]);
            const auto remove = nonnegative_integer((*tuple)[3]);
            const auto* items = (*tuple)[4].get_if<support::JsonValue::array_t>();
            if (!path || !index || !remove || items == nullptr) {
                return std::unexpected(corruption("Invalid document operation"));
            }
            std::vector<chord::Json> decoded_items;
            decoded_items.reserve(items->size());
            for (const support::JsonValue& item : *items) {
                auto decoded = decode_json_value(item);
                if (!decoded) {
                    return std::unexpected(std::move(decoded.error()));
                }
                decoded_items.push_back(std::move(*decoded));
            }
            ops.push_back(chord::SpliceOp{
                    .path = std::move(*path),
                    .index = static_cast<std::size_t>(*index),
                    .remove = static_cast<std::size_t>(*remove),
                    .items = std::move(decoded_items),
            });
        } else if (*verb == "m" && tuple->size() == 3) {
            auto path = decode_path((*tuple)[1]);
            const auto* items = (*tuple)[2].get_if<support::JsonValue::array_t>();
            if (!path || items == nullptr) {
                return std::unexpected(corruption("Invalid document operation"));
            }
            std::vector<std::size_t> permutation;
            permutation.reserve(items->size());
            for (const support::JsonValue& item : *items) {
                const auto index = nonnegative_integer(item);
                if (!index.has_value() || static_cast<std::uint64_t>(*index) >
                                                  static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
                    return std::unexpected(corruption("Invalid document operation"));
                }
                permutation.push_back(static_cast<std::size_t>(*index));
            }
            ops.push_back(chord::MoveOp{
                    .path = std::move(*path),
                    .permutation = std::move(permutation),
            });
        } else {
            return std::unexpected(corruption("Unknown document operation verb"));
        }
    }
    if (auto valid = chord::validate_batch(ops); !valid) {
        return std::unexpected(std::move(valid.error()));
    }
    return ops;
}

chord::Json encode_task_record(const TaskRecord& task) {
    chord::Json::Object obj;
    obj.emplace("id", chord::Json(static_cast<double>(task.id.value)));
    obj.emplace("conversationId", chord::Json(static_cast<double>(task.conversation_id.value)));
    obj.emplace("kind", chord::Json(task.kind));
    obj.emplace("version", chord::Json(static_cast<double>(task.version)));
    obj.emplace("input", task.input);
    if (task.owner.has_value()) {
        obj.emplace("owner", chord::Json(static_cast<double>(task.owner->value)));
    }
    obj.emplace("background", chord::Json(task.background));
    obj.emplace("abortRequested", chord::Json(task.abort_requested));
    if (task.abort_reason.has_value()) {
        obj.emplace("abortReason", chord::Json(*task.abort_reason));
    }
    if (task.started_at.has_value()) {
        obj.emplace("startedAt", chord::Json(static_cast<double>(*task.started_at)));
    }
    if (task.ended_at.has_value()) {
        obj.emplace("endedAt", chord::Json(static_cast<double>(*task.ended_at)));
    }
    chord::Json::Object state_obj;
    state_obj.emplace("status", chord::Json(task.state.status));
    if (task.state.checkpoint.has_value()) {
        state_obj.emplace("checkpoint", *task.state.checkpoint);
    }
    if (task.state.status == "waiting") {
        chord::Json::Array on;
        on.reserve(task.state.on.size());
        for (const TaskId id : task.state.on) {
            on.push_back(chord::Json(static_cast<double>(id.value)));
        }
        state_obj.emplace("on", chord::Json(std::move(on)));
        if (task.state.policy.has_value()) {
            state_obj.emplace("policy", chord::Json(std::string{join_policy_name(*task.state.policy)}));
        }
    }
    if (task.state.outcome.has_value()) {
        chord::Json::Object outcome_obj;
        outcome_obj.emplace("status", chord::Json(task.state.outcome->status));
        if (task.state.outcome->result.has_value()) {
            outcome_obj.emplace("result", *task.state.outcome->result);
        }
        if (task.state.outcome->reason.has_value()) {
            outcome_obj.emplace("reason", chord::Json(*task.state.outcome->reason));
        }
        if (task.state.outcome->error.has_value()) {
            chord::Json::Object error_obj;
            error_obj.emplace("message", chord::Json(task.state.outcome->error->message));
            if (task.state.outcome->error->detail.has_value()) {
                error_obj.emplace("detail", *task.state.outcome->error->detail);
            }
            outcome_obj.emplace("error", chord::Json(std::move(error_obj)));
        }
        state_obj.emplace("outcome", chord::Json(std::move(outcome_obj)));
    }
    obj.emplace("state", chord::Json(std::move(state_obj)));
    if (!task.memos.empty()) {
        chord::Json::Object memos_obj;
        for (const auto& [k, v] : task.memos) {
            memos_obj.emplace(k, v);
        }
        obj.emplace("memos", chord::Json(std::move(memos_obj)));
    }
    return chord::Json(std::move(obj));
}

support::Expected<TaskRecord> decode_task_record(const chord::Json& json) {
    const auto* obj = json.as_object();
    if (obj == nullptr) {
        return std::unexpected(corruption("Task record must be a JSON object"));
    }
    const auto id_it = obj->find("id");
    const auto conv_it = obj->find("conversationId");
    const auto kind_it = obj->find("kind");
    const auto ver_it = obj->find("version");
    const auto input_it = obj->find("input");
    const auto state_it = obj->find("state");
    if (id_it == obj->end() || conv_it == obj->end() || kind_it == obj->end() || ver_it == obj->end() ||
            input_it == obj->end() || state_it == obj->end()) {
        return std::unexpected(corruption("Task record missing required fields"));
    }
    const auto* id_num = id_it->second.as_number();
    const auto* conv_num = conv_it->second.as_number();
    const auto* kind_str = kind_it->second.as_string();
    const auto* ver_num = ver_it->second.as_number();
    const auto* state_obj = state_it->second.as_object();
    if (id_num == nullptr || conv_num == nullptr || kind_str == nullptr || ver_num == nullptr || state_obj == nullptr) {
        return std::unexpected(corruption("Invalid task record field types"));
    }
    const auto status_it = state_obj->find("status");
    if (status_it == state_obj->end() || status_it->second.as_string() == nullptr) {
        return std::unexpected(corruption("Task state missing status"));
    }
    TaskState state;
    state.status = *status_it->second.as_string();
    const auto cp_it = state_obj->find("checkpoint");
    if (cp_it != state_obj->end()) {
        state.checkpoint = cp_it->second;
    }
    const auto on_it = state_obj->find("on");
    if (on_it != state_obj->end()) {
        const auto* on = on_it->second.as_array();
        if (on == nullptr) {
            return std::unexpected(corruption("Task wait list must be an array"));
        }
        for (const chord::Json& entry : *on) {
            const double* number = entry.as_number();
            if (number == nullptr) {
                return std::unexpected(corruption("Task wait list entry must be a number"));
            }
            state.on.push_back(TaskId{static_cast<std::int64_t>(*number)});
        }
    }
    const auto policy_it = state_obj->find("policy");
    if (policy_it != state_obj->end()) {
        const std::string* name = policy_it->second.as_string();
        const auto policy = name == nullptr ? std::nullopt : join_policy_from_name(*name);
        if (!policy.has_value()) {
            return std::unexpected(corruption("Task join policy is invalid"));
        }
        state.policy = *policy;
    }
    const auto outcome_it = state_obj->find("outcome");
    if (outcome_it != state_obj->end()) {
        const auto* outcome_obj = outcome_it->second.as_object();
        if (outcome_obj != nullptr) {
            TaskOutcome outcome;
            const auto out_status_it = outcome_obj->find("status");
            if (out_status_it != outcome_obj->end() && out_status_it->second.as_string() != nullptr) {
                outcome.status = *out_status_it->second.as_string();
            }
            const auto res_it = outcome_obj->find("result");
            if (res_it != outcome_obj->end()) {
                outcome.result = res_it->second;
            }
            const auto reason_it = outcome_obj->find("reason");
            if (reason_it != outcome_obj->end() && reason_it->second.as_string() != nullptr) {
                outcome.reason = *reason_it->second.as_string();
            }
            const auto err_it = outcome_obj->find("error");
            if (err_it != outcome_obj->end()) {
                const auto* err_obj = err_it->second.as_object();
                if (err_obj != nullptr) {
                    TaskOutcomeError err;
                    const auto msg_it = err_obj->find("message");
                    if (msg_it != err_obj->end() && msg_it->second.as_string() != nullptr) {
                        err.message = *msg_it->second.as_string();
                    }
                    const auto detail_it = err_obj->find("detail");
                    if (detail_it != err_obj->end()) {
                        err.detail = detail_it->second;
                    }
                    outcome.error = std::move(err);
                }
            }
            state.outcome = std::move(outcome);
        }
    }

    TaskRecord record{
            .id = TaskId{static_cast<std::int64_t>(*id_num)},
            .conversation_id = ConversationId{static_cast<std::int64_t>(*conv_num)},
            .kind = *kind_str,
            .version = static_cast<std::int64_t>(*ver_num),
            .input = input_it->second,
            .owner = std::nullopt,
            .background = false,
            .abort_requested = false,
            .abort_reason = std::nullopt,
            .started_at = std::nullopt,
            .ended_at = std::nullopt,
            .state = std::move(state),
            .memos = {},
    };

    const auto owner_it = obj->find("owner");
    if (owner_it != obj->end() && owner_it->second.as_number() != nullptr) {
        record.owner = TaskId{static_cast<std::int64_t>(*owner_it->second.as_number())};
    }
    const auto bg_it = obj->find("background");
    if (bg_it != obj->end() && bg_it->second.as_boolean() != nullptr) {
        record.background = *bg_it->second.as_boolean();
    }
    const auto abort_it = obj->find("abortRequested");
    if (abort_it != obj->end() && abort_it->second.as_boolean() != nullptr) {
        record.abort_requested = *abort_it->second.as_boolean();
    }
    const auto abort_reason_it = obj->find("abortReason");
    if (abort_reason_it != obj->end() && abort_reason_it->second.as_string() != nullptr) {
        record.abort_reason = *abort_reason_it->second.as_string();
    }
    const auto started_it = obj->find("startedAt");
    if (started_it != obj->end() && started_it->second.as_number() != nullptr) {
        record.started_at = static_cast<std::int64_t>(*started_it->second.as_number());
    }
    const auto ended_it = obj->find("endedAt");
    if (ended_it != obj->end() && ended_it->second.as_number() != nullptr) {
        record.ended_at = static_cast<std::int64_t>(*ended_it->second.as_number());
    }
    const auto memos_it = obj->find("memos");
    if (memos_it != obj->end() && memos_it->second.as_object() != nullptr) {
        for (const auto& [k, v] : *memos_it->second.as_object()) {
            record.memos.emplace(k, v);
        }
    }

    return record;
}

chord::Json encode_conversation_record(const ConversationRecord& conversation) {
    chord::Json::Object obj;
    obj.emplace("id", chord::Json(static_cast<double>(conversation.id.value)));
    if (conversation.owner.has_value()) {
        chord::Json::Object owner;
        owner.emplace("conversationId", chord::Json(static_cast<double>(conversation.owner->conversation_id.value)));
        owner.emplace("taskId", chord::Json(static_cast<double>(conversation.owner->task_id.value)));
        obj.emplace("owner", chord::Json(std::move(owner)));
    }
    if (conversation.parent.has_value()) {
        chord::Json::Object parent;
        parent.emplace("conversationId", chord::Json(static_cast<double>(conversation.parent->conversation_id.value)));
        parent.emplace("at", chord::Json(static_cast<double>(conversation.parent->at.value)));
        obj.emplace("parent", chord::Json(std::move(parent)));
    }
    return chord::Json(std::move(obj));
}

support::Expected<ConversationRecord> decode_conversation_record(const chord::Json& json) {
    const auto* obj = json.as_object();
    if (obj == nullptr) {
        return std::unexpected(corruption("Conversation record must be a JSON object"));
    }
    const auto id_it = obj->find("id");
    const auto* id_num = id_it == obj->end() ? nullptr : id_it->second.as_number();
    if (id_num == nullptr) {
        return std::unexpected(corruption("Conversation record missing id"));
    }
    ConversationRecord record{.id = ConversationId{static_cast<std::int64_t>(*id_num)}};
    const auto parent_it = obj->find("parent");
    if (parent_it != obj->end()) {
        const auto* parent = parent_it->second.as_object();
        if (parent == nullptr) return std::unexpected(corruption("Conversation parent must be a JSON object"));
        const auto conversation_it = parent->find("conversationId");
        const auto at_it = parent->find("at");
        const auto* conversation_num = conversation_it == parent->end() ? nullptr : conversation_it->second.as_number();
        const auto* at_num = at_it == parent->end() ? nullptr : at_it->second.as_number();
        if (conversation_num == nullptr || at_num == nullptr) {
            return std::unexpected(corruption("Conversation parent missing an edge field"));
        }
        record.parent = ConversationRecord::Parent{
                .conversation_id = ConversationId{static_cast<std::int64_t>(*conversation_num)},
                .at = EntryId{static_cast<std::int64_t>(*at_num)}};
    }
    const auto owner_it = obj->find("owner");
    if (owner_it != obj->end()) {
        const auto* owner = owner_it->second.as_object();
        if (owner == nullptr) {
            return std::unexpected(corruption("Conversation owner must be a JSON object"));
        }
        const auto conversation_it = owner->find("conversationId");
        const auto task_it = owner->find("taskId");
        const auto* conversation_num = conversation_it == owner->end() ? nullptr : conversation_it->second.as_number();
        const auto* task_num = task_it == owner->end() ? nullptr : task_it->second.as_number();
        if (conversation_num == nullptr || task_num == nullptr) {
            return std::unexpected(corruption("Conversation owner missing an edge field"));
        }
        record.owner =
                ConversationOwner{.conversation_id = ConversationId{static_cast<std::int64_t>(*conversation_num)},
                        .task_id = TaskId{static_cast<std::int64_t>(*task_num)}};
    }
    return record;
}

chord::Json encode_submission_record(const SubmissionRecord& submission) {
    chord::Json::Object obj;
    obj.emplace("id", chord::Json(static_cast<double>(submission.id.value)));
    obj.emplace("conversationId", chord::Json(static_cast<double>(submission.conversation_id.value)));
    if (submission.request_id.has_value()) {
        obj.emplace("requestId", chord::Json(*submission.request_id));
    }
    obj.emplace("type", chord::Json(submission.type));
    obj.emplace("status", chord::Json(submission.status));
    if (submission.entry.has_value()) {
        obj.emplace("entry", chord::Json(static_cast<double>(submission.entry->value)));
    }
    if (submission.answer.has_value()) {
        obj.emplace("answer", chord::Json(static_cast<double>(submission.answer->value)));
    }
    if (submission.reason.has_value()) {
        obj.emplace("reason", chord::Json(*submission.reason));
    }
    if (submission.detail.has_value()) {
        obj.emplace("detail", *submission.detail);
    }
    return chord::Json(std::move(obj));
}

support::Expected<SubmissionRecord> decode_submission_record(const chord::Json& json) {
    const auto* obj = json.as_object();
    if (obj == nullptr) {
        return std::unexpected(corruption("Submission record must be a JSON object"));
    }
    const auto id_it = obj->find("id");
    const auto conv_it = obj->find("conversationId");
    const auto type_it = obj->find("type");
    const auto status_it = obj->find("status");
    if (id_it == obj->end() || conv_it == obj->end() || type_it == obj->end() || status_it == obj->end()) {
        return std::unexpected(corruption("Submission record missing required fields"));
    }
    const auto* id_num = id_it->second.as_number();
    const auto* conv_num = conv_it->second.as_number();
    const auto* type_str = type_it->second.as_string();
    const auto* status_str = status_it->second.as_string();
    if (id_num == nullptr || conv_num == nullptr || type_str == nullptr || status_str == nullptr) {
        return std::unexpected(corruption("Invalid submission record field types"));
    }
    SubmissionRecord record{
            .id = SubmissionId{static_cast<std::int64_t>(*id_num)},
            .conversation_id = ConversationId{static_cast<std::int64_t>(*conv_num)},
            .request_id = std::nullopt,
            .type = *type_str,
            .status = *status_str,
            .entry = std::nullopt,
            .answer = std::nullopt,
            .reason = std::nullopt,
            .detail = std::nullopt,
    };
    const auto request_it = obj->find("requestId");
    if (request_it != obj->end() && request_it->second.as_string() != nullptr) {
        record.request_id = *request_it->second.as_string();
    }
    const auto entry_it = obj->find("entry");
    if (entry_it != obj->end() && entry_it->second.as_number() != nullptr) {
        record.entry = EntryId{static_cast<std::int64_t>(*entry_it->second.as_number())};
    }
    const auto answer_it = obj->find("answer");
    if (answer_it != obj->end() && answer_it->second.as_number() != nullptr) {
        record.answer = EntryId{static_cast<std::int64_t>(*answer_it->second.as_number())};
    }
    const auto reason_it = obj->find("reason");
    if (reason_it != obj->end() && reason_it->second.as_string() != nullptr) {
        record.reason = *reason_it->second.as_string();
    }
    const auto detail_it = obj->find("detail");
    if (detail_it != obj->end()) {
        record.detail = detail_it->second;
    }
    return record;
}

support::JsonValue encode_json(const chord::Json& json) {
    return json.to_support();
}

support::Expected<chord::Json> decode_json(const support::JsonValue& json) {
    return chord::Json::from_support(json);
}

} // namespace cch::durable
