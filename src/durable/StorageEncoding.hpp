#pragma once

#include <cch/chord/Delta.hpp>
#include <cch/chord/Json.hpp>
#include <cch/durable/Records.hpp>
#include <cch/support/Error.hpp>
#include <cch/support/JsonValue.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>

namespace cch::durable {

[[nodiscard]] std::optional<std::int64_t> nonnegative_integer(const support::JsonValue& value);
[[nodiscard]] support::JsonValue number(std::int64_t value);
[[nodiscard]] support::JsonValue number(std::size_t value);

[[nodiscard]] support::JsonValue path_value(const chord::Path& path);
[[nodiscard]] support::JsonValue op_value(const chord::Op& op);
[[nodiscard]] support::JsonValue ops_value(const chord::OpBatch& ops);

[[nodiscard]] support::Expected<chord::Path> decode_path(const support::JsonValue& value);
[[nodiscard]] support::Expected<chord::OpBatch> decode_ops(const support::JsonValue& value);

[[nodiscard]] chord::Json encode_task_record(const TaskRecord& task);
[[nodiscard]] support::Expected<TaskRecord> decode_task_record(const chord::Json& json);

[[nodiscard]] chord::Json encode_conversation_record(const ConversationRecord& conversation);
[[nodiscard]] support::Expected<ConversationRecord> decode_conversation_record(const chord::Json& json);

[[nodiscard]] chord::Json encode_submission_record(const SubmissionRecord& submission);
[[nodiscard]] support::Expected<SubmissionRecord> decode_submission_record(const chord::Json& json);

[[nodiscard]] support::JsonValue encode_json(const chord::Json& json);
[[nodiscard]] support::Expected<chord::Json> decode_json(const support::JsonValue& json);

} // namespace cch::durable
