#include <cch/mcp/Client.hpp>

#include "support/AsyncResultBridge.hpp"

#include <boost/asio/awaitable.hpp>

#include <expected>
#include <utility>

namespace cch::mcp {

support::AsyncResult<ToolCallResult> Client::call_tool(
        std::string name, support::JsonValue arguments, RequestOptions options) {
    return support::detail::make_async_result(
            [this, name = std::move(name), arguments = std::move(arguments), options = std::move(options)]() mutable
                    -> boost::asio::awaitable<support::Expected<ToolCallResult>> {
                support::JsonValue params{support::JsonValue::object_t{
                        {"name", std::move(name)},
                        {"arguments", std::move(arguments)},
                }};
                auto result = co_await support::detail::await_async_result(
                        request("tools/call", std::move(params), std::move(options)));
                if (!result) {
                    co_return std::unexpected(std::move(result.error()));
                }
                co_return ToolCallResult{std::move(*result), std::nullopt};
            });
}

} // namespace cch::mcp
