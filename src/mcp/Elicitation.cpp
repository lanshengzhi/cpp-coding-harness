#include <cch/mcp/UpstreamElicitation.hpp>

#include "mcp/Protocol.hpp"

#include <string_view>

namespace cch::mcp {

std::string_view to_string(ElicitationAction action) noexcept {
    switch (action) {
    case ElicitationAction::Accept:
        return protocol::kInputResponseAccept;
    case ElicitationAction::Decline:
        return protocol::kInputResponseDecline;
    case ElicitationAction::Cancel:
        return protocol::kInputResponseCancel;
    }
    return protocol::kInputResponseCancel;
}

} // namespace cch::mcp
