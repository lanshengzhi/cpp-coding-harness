#include <cch/mcp/Client.hpp>

#include "ProtocolInternal.hpp"

#include "support/AsyncResultBridge.hpp"

#include <boost/asio/awaitable.hpp>

#include <expected>
#include <map>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

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

namespace detail {
namespace {

/// The `roots/list` result (pi `{roots: [...configured]}`): each configured
/// root verbatim, with `name` omitted when the connection did not set one.
[[nodiscard]] support::JsonValue roots_list_result(const std::vector<Root>& roots) {
    support::JsonValue::array_t items;
    items.reserve(roots.size());
    for (const auto& root : roots) {
        support::JsonValue::object_t item{{"uri", root.uri}};
        if (root.name.has_value()) {
            item.emplace("name", *root.name);
        }
        items.emplace_back(std::move(item));
    }
    return support::JsonValue{support::JsonValue::object_t{
            {"roots", support::JsonValue{std::move(items)}},
    }};
}

} // namespace

void install_default_request_handlers(
        std::map<std::string, Client::ServerRequestHandler>& handlers, const std::optional<std::vector<Root>>& roots) {
    handlers.insert_or_assign(
            "ping", [](const support::JsonValue&, std::stop_token) -> support::AsyncResult<support::JsonValue> {
                return support::AsyncResult<support::JsonValue>{
                        support::Expected<support::JsonValue>{support::JsonValue{support::JsonValue::object_t{}}}};
            });
    if (roots.has_value()) {
        handlers.insert_or_assign("roots/list",
                [configured = *roots](
                        const support::JsonValue&, std::stop_token) -> support::AsyncResult<support::JsonValue> {
                    return support::AsyncResult<support::JsonValue>{
                            support::Expected<support::JsonValue>{roots_list_result(configured)}};
                });
    }
}

} // namespace detail

} // namespace cch::mcp
