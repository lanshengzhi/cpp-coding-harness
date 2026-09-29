#include "mcp/transport/RetryPolicy.hpp"

#include <cch/support/Error.hpp>

#include <string_view>

namespace cch::mcp::transport {

bool is_retryable(McpFailureClass failure_class) noexcept {
    switch (failure_class) {
    case McpFailureClass::RequestNotDelivered:
        return true;
    case McpFailureClass::RequestDelivered:
    case McpFailureClass::ResponseFlooded:
    case McpFailureClass::Cancelled:
    case McpFailureClass::Rejected:
        return false;
    }
    return false;
}

McpFailureClass classify_failure(cch::support::ErrorCode code, bool request_delivered) noexcept {
    switch (code) {
    case cch::support::ErrorCode::Cancelled:
        return McpFailureClass::Cancelled;
    case cch::support::ErrorCode::ResourceLimit:
        return McpFailureClass::ResponseFlooded;
    case cch::support::ErrorCode::Validation:
        return McpFailureClass::Rejected;
    case cch::support::ErrorCode::Network:
    case cch::support::ErrorCode::Timeout:
    case cch::support::ErrorCode::Stream:
    case cch::support::ErrorCode::Unknown:
        break;
    default:
        break;
    }
    return request_delivered ? McpFailureClass::RequestDelivered : McpFailureClass::RequestNotDelivered;
}

std::string_view describe(McpFailureClass failure_class) noexcept {
    switch (failure_class) {
    case McpFailureClass::RequestNotDelivered:
        return "the request was never delivered";
    case McpFailureClass::RequestDelivered:
        return "the request was delivered and no complete response arrived";
    case McpFailureClass::ResponseFlooded:
        return "the response passed the retention bound";
    case McpFailureClass::Cancelled:
        return "the call was cancelled";
    case McpFailureClass::Rejected:
        return "the transport refused to send the request";
    }
    return "the exchange failed";
}

} // namespace cch::mcp::transport
