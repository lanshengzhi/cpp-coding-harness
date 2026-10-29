#pragma once

#include <cch/ai/Message.hpp>
#include <cch/durable/Records.hpp>
#include <cch/durable/Session.hpp>
#include <cch/support/Error.hpp>

#include <optional>

namespace cch::durable {

/// Recover the unfinished call's committed nested history for the generation's
/// abort transaction, before task-scoped documents retire.
[[nodiscard]] support::Expected<std::optional<ai::ToolResultMessage>> aborted_codemode_result(
        Transaction& tx, const TaskRecord& task);

} // namespace cch::durable
