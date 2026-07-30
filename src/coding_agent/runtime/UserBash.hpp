#pragma once

#include <cch/ai/Message.hpp>
#include <cch/util/Error.hpp>

#include <functional>
#include <optional>
#include <string>

namespace cch::coding_agent::runtime {

struct UserBashProgress {
    std::string command;
    std::string output;
    bool exclude_from_context{false};
    /// The execution completed during an active Agent run; commitment to
    /// Live Session State and the Session Store is deferred until the whole
    /// run settles.
    bool awaiting_commitment{false};
};

struct UserBashCompletion {
    ai::BashExecutionMessage message;
    std::optional<util::Error> diagnostic;
};

using UserBashProgressSink =
    std::move_only_function<util::ExpectedVoid(const UserBashProgress&)>;

} // namespace cch::coding_agent::runtime
