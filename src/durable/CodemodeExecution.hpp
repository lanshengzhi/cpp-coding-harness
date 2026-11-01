#pragma once

#include <cch/ai/Message.hpp>
#include <cch/durable/Records.hpp>
#include <cch/durable/Session.hpp>
#include <cch/support/Error.hpp>

#include <optional>

namespace cch::durable {

/// The model-visible result of a codemode call the generation's abort transaction
/// ends: a committed result stands, else the frozen harness error. The nested
/// calls' own terminal records carry their history.
[[nodiscard]] support::Expected<std::optional<ai::ToolResultMessage>> aborted_codemode_result(
        Transaction& tx, const TaskRecord& task);

} // namespace cch::durable
