#include <cch/agent/AgentTool.hpp>
#include "coding_agent/mcp/McpExtensionToolSource.hpp"
#include "coding_agent/mcp/McpProtocol.hpp"
#include "coding_agent/mcp/McpServerConnection.hpp"
#include <cch/support/JsonValue.hpp>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <vector>

namespace cch {
namespace {

class ScriptedProgressConnection final : public coding_agent::mcp::McpServerConnection {
public:
    std::string recorded_method;
    std::optional<support::JsonValue> recorded_params;
    coding_agent::mcp::McpServerConnection::ProgressCallback progress_callback;

    [[nodiscard]] support::AsyncResult<support::JsonValue> request(std::string method,
            std::optional<support::JsonValue> params = std::nullopt,
            RequestOptions options = {}) override {
        recorded_method = std::move(method);
        recorded_params = std::move(params);
        progress_callback = std::move(options.on_progress);
        support::JsonValue::object_t result;
        result.emplace("content",
                support::JsonValue{support::JsonValue::array_t{
                        support::JsonValue{support::JsonValue::object_t{
                                {"type", "text"},
                                {"text", "done"},
                        }},
                }});
        return support::AsyncResult<support::JsonValue>{support::JsonValue{std::move(result)}};
    }

    void notify(std::string, std::optional<support::JsonValue> = std::nullopt) override {}

    [[nodiscard]] const std::string& server_name() const noexcept override {
        static const std::string kName = "testserver";
        return kName;
    }
};

} // namespace

TEST_CASE("progress_update_text formats pi progress messages verbatim", "[coding_agent][mcp][issue884][spec]") {
    // 1. With message: message wins regardless of progress/total.
    {
        support::JsonValue params{support::JsonValue::object_t{
                {"message", "custom message"},
                {"progress", 1.0},
                {"total", 5.0},
        }};
        CHECK(coding_agent::mcp::progress_update_text(params) == "custom message");
    }

    // 2. With progress and total: "Progress <progress>/<total>"
    {
        support::JsonValue params{support::JsonValue::object_t{
                {"progress", 2.0},
                {"total", 10.0},
        }};
        CHECK(coding_agent::mcp::progress_update_text(params) == "Progress 2/10");
    }

    // 3. With progress only: "Progress <progress>"
    {
        support::JsonValue params{support::JsonValue::object_t{
                {"progress", 3.0},
        }};
        CHECK(coding_agent::mcp::progress_update_text(params) == "Progress 3");
    }

    // 4. Empty or invalid: empty string
    {
        support::JsonValue params{support::JsonValue::object_t{}};
        CHECK(coding_agent::mcp::progress_update_text(params).empty());
    }
}

TEST_CASE("request with on_progress injects _meta.progressToken and progress_token_of extracts it",
        "[coding_agent][mcp][issue884][spec]") {
    // Use with_progress_token helper to verify contract
    auto params = support::JsonValue::object_t{{"arg", "val"}};
    auto with_meta = coding_agent::mcp::detail::with_progress_token(support::JsonValue{params}, 42);
    const auto* with_meta_obj = with_meta.get_if<support::JsonValue::object_t>();
    REQUIRE(with_meta_obj != nullptr);
    const auto meta_it = with_meta_obj->find("_meta");
    REQUIRE(meta_it != with_meta_obj->end());
    const auto* meta_obj = meta_it->second.get_if<support::JsonValue::object_t>();
    REQUIRE(meta_obj != nullptr);
    CHECK(meta_obj->at("progressToken").get_number() == 42.0);

    // progress_token_of extracts token from notification
    support::JsonValue notification{support::JsonValue::object_t{
            {"method", "notifications/progress"},
            {"params",
                    support::JsonValue{support::JsonValue::object_t{
                            {"progressToken", 42.0},
                            {"progress", 1.0},
                            {"total", 2.0},
                    }}},
    }};
    auto extracted = coding_agent::mcp::detail::progress_token_of(notification);
    REQUIRE(extracted.has_value());
    CHECK(*extracted == 42.0);
}

} // namespace cch
