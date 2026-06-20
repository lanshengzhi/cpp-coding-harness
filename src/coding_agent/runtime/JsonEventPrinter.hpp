#pragma once

#include "../../../include/cch/agent/AgentEvent.hpp"
#include "../../../include/cch/harness/session/SessionEntry.hpp"
#include "../../../include/cch/util/Error.hpp"

#include <iosfwd>
#include <string>

namespace cch::coding_agent::runtime {

class JsonEventPrinter {
public:
    explicit JsonEventPrinter(std::ostream& out);

    [[nodiscard]] util::ExpectedVoid print_session_header(const harness::session::SessionMetadata& metadata);
    [[nodiscard]] util::ExpectedVoid print_agent_event(const agent::AgentLifecycleEvent& event);
    [[nodiscard]] util::ExpectedVoid print_terminal(bool success, std::string code, std::string message = {});

private:
    [[nodiscard]] int next_seq();
    [[nodiscard]] util::ExpectedVoid write_record(util::JsonValue::object_t record);

    std::ostream* out_;
    int next_seq_{1};
};

} // namespace cch::coding_agent::runtime
