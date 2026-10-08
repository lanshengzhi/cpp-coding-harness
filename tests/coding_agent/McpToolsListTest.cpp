// Spec #882: `tools/list` pagination follows pi `client.ts` `listAll` — at
// most MAX_LIST_PAGES, a duplicate cursor is an explicit error, and a null or
// empty nextCursor ends pagination (some servers end with `null` or `""`
// instead of omitting the cursor). The scripted connection stands in for a
// paginating server; the client under test is the real
// `list_mcp_server_tools`.

#include "coding_agent/mcp/McpExtensionToolSource.hpp"
#include "coding_agent/mcp/McpServerConnection.hpp"

#include <cch/support/Error.hpp>
#include <cch/support/JsonValue.hpp>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/use_future.hpp>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <charconv>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace cch;

namespace {

namespace mcp = cch::coding_agent::mcp;

/// One scripted `McpServerConnection`: every `tools/list` is answered from the
/// scripted page function, recording the cursors the client followed.
class ScriptedListConnection final : public mcp::McpServerConnection {
public:
    std::move_only_function<support::Expected<support::JsonValue>(const std::optional<std::string>&)> page_for;
    std::vector<std::optional<std::string>> cursors;

    [[nodiscard]] support::AsyncResult<support::JsonValue> request(std::string method,
            std::optional<support::JsonValue> params = std::nullopt,
            RequestOptions options = {}) override {
        (void)options;
        if (method != "tools/list" || page_for == nullptr) {
            return support::AsyncResult<support::JsonValue>{support::Expected<support::JsonValue>{
                    std::unexpected(support::make_error(support::ErrorCode::Process, "unexpected request"))}};
        }
        std::optional<std::string> cursor;
        if (params) {
            if (const auto* object = params->get_if<support::JsonValue::object_t>()) {
                if (const auto found = object->find("cursor");
                        found != object->end() && found->second.holds<std::string>()) {
                    cursor = found->second.get_string();
                }
            }
        }
        cursors.push_back(cursor);
        return support::AsyncResult<support::JsonValue>{page_for(cursor)};
    }

    void notify(std::string, std::optional<support::JsonValue> = std::nullopt) override {}

    [[nodiscard]] const std::string& server_name() const noexcept override { return name_; }

    std::string name_{"scripted"};
};

[[nodiscard]] support::JsonValue tool(std::string name) {
    return support::JsonValue{support::JsonValue::object_t{
            {"name", std::move(name)},
            {"inputSchema", support::JsonValue::object_t{{"type", "object"}}},
    }};
}

[[nodiscard]] support::JsonValue page(std::vector<support::JsonValue> tools) {
    return support::JsonValue{support::JsonValue::object_t{{"tools", support::JsonValue{std::move(tools)}}}};
}

[[nodiscard]] support::JsonValue page_with_next(std::vector<support::JsonValue> tools, support::JsonValue cursor) {
    return support::JsonValue{support::JsonValue::object_t{
            {"tools", support::JsonValue{std::move(tools)}},
            {"nextCursor", std::move(cursor)},
    }};
}

} // namespace

TEST_CASE("tools/list pagination follows a nextCursor across pages", "[coding_agent][mcp][issue884][spec]") {
    ScriptedListConnection connection;
    int calls = 0;
    connection.page_for = [&calls](const std::optional<std::string>& cursor) -> support::Expected<support::JsonValue> {
        ++calls;
        if (!cursor.has_value()) {
            return page_with_next({tool("first")}, "1");
        }
        return page({tool("second")});
    };
    boost::asio::thread_pool pool{1};
    auto outcome = boost::asio::co_spawn(
            pool,
            [&connection]() -> boost::asio::awaitable<support::Expected<std::vector<mcp::McpToolDescriptor>>> {
                co_return co_await mcp::list_mcp_server_tools(connection);
            },
            boost::asio::use_future);
    const auto listed = outcome.get();
    pool.join();

    REQUIRE(listed.has_value());
    REQUIRE(listed->size() == 2);
    CHECK((*listed)[0].server_tool_name == "first");
    CHECK((*listed)[1].server_tool_name == "second");
    REQUIRE(connection.cursors.size() == 2);
    CHECK(!connection.cursors[0].has_value());
    CHECK(connection.cursors[1] == std::optional<std::string>{"1"});
}

TEST_CASE("tools/list treats a null or empty nextCursor as the end of pagination",
        "[coding_agent][mcp][issue884][spec]") {
    // pi `validateListPage`: some servers end pagination with `null` or ""
    // instead of omitting the cursor; both end the walk without another
    // request (an echoed empty cursor would otherwise loop forever).
    for (const char* label : {"null", "empty"}) {
        ScriptedListConnection connection;
        int calls = 0;
        connection.page_for = [&calls, label](
                                      const std::optional<std::string>&) -> support::Expected<support::JsonValue> {
            ++calls;
            if (calls > 1) {
                return std::unexpected(
                        support::make_error(support::ErrorCode::Process, "unexpected extra page request"));
            }
            if (std::string{label} == "null") {
                return page_with_next({tool("only")}, support::JsonValue{});
            }
            return page_with_next({tool("only")}, "");
        };
        boost::asio::thread_pool pool{1};
        auto outcome = boost::asio::co_spawn(
                pool,
                [&connection]() -> boost::asio::awaitable<support::Expected<std::vector<mcp::McpToolDescriptor>>> {
                    co_return co_await mcp::list_mcp_server_tools(connection);
                },
                boost::asio::use_future);
        const auto listed = outcome.get();
        pool.join();

        INFO("cursor form: " << label);
        REQUIRE(listed.has_value());
        REQUIRE(listed->size() == 1);
        CHECK(connection.cursors.size() == 1);
    }
}

TEST_CASE("tools/list rejects a duplicate cursor with pi's message", "[coding_agent][mcp][issue884][spec]") {
    ScriptedListConnection connection;
    connection.page_for = [](const std::optional<std::string>&) -> support::Expected<support::JsonValue> {
        return page_with_next({}, "loop");
    };
    boost::asio::thread_pool pool{1};
    auto outcome = boost::asio::co_spawn(
            pool,
            [&connection]() -> boost::asio::awaitable<support::Expected<std::vector<mcp::McpToolDescriptor>>> {
                co_return co_await mcp::list_mcp_server_tools(connection);
            },
            boost::asio::use_future);
    const auto listed = outcome.get();
    pool.join();

    REQUIRE(!listed.has_value());
    CHECK(listed.error().message == "MCP tools/list returned duplicate cursor: loop");
}

TEST_CASE("tools/list stops after pi's 1000-page bound", "[coding_agent][mcp][issue884][spec]") {
    ScriptedListConnection connection;
    int pages = 0;
    connection.page_for = [&pages](const std::optional<std::string>& cursor) -> support::Expected<support::JsonValue> {
        ++pages;
        int start = 0;
        if (cursor) {
            const auto parsed = std::from_chars(cursor->data(), cursor->data() + cursor->size(), start);
            REQUIRE(parsed.ec == std::errc{});
        }
        return page_with_next({}, std::to_string(start + 1));
    };
    boost::asio::thread_pool pool{1};
    auto outcome = boost::asio::co_spawn(
            pool,
            [&connection]() -> boost::asio::awaitable<support::Expected<std::vector<mcp::McpToolDescriptor>>> {
                co_return co_await mcp::list_mcp_server_tools(connection);
            },
            boost::asio::use_future);
    const auto listed = outcome.get();
    pool.join();

    REQUIRE(!listed.has_value());
    CHECK(listed.error().message == "MCP tools/list exceeded 1000 pages");
    CHECK(pages == 1000);
}

TEST_CASE("tools/list rejects a non-string cursor with pi's invalid-cursor error",
        "[coding_agent][mcp][issue884][spec]") {
    ScriptedListConnection connection;
    connection.page_for = [](const std::optional<std::string>&) -> support::Expected<support::JsonValue> {
        return page_with_next({}, support::JsonValue{1.0});
    };
    boost::asio::thread_pool pool{1};
    auto outcome = boost::asio::co_spawn(
            pool,
            [&connection]() -> boost::asio::awaitable<support::Expected<std::vector<mcp::McpToolDescriptor>>> {
                co_return co_await mcp::list_mcp_server_tools(connection);
            },
            boost::asio::use_future);
    const auto listed = outcome.get();
    pool.join();

    REQUIRE(!listed.has_value());
    CHECK(listed.error().message == "Invalid MCP tools/list cursor");
}
