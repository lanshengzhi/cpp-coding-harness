#include "coding_agent/runtime/McpToolBinding.hpp"

#include "support/AsyncResultBridge.hpp"
#include "support/Json.hpp"

#include <cch/support/OutputLimiter.hpp>

#include <boost/asio/awaitable.hpp>

#include <string>
#include <utility>

namespace cch::coding_agent::runtime {
namespace {

/// The model-visible and user-visible bound on one upstream tool result. It is
/// the product's ordinary tool-output limit rather than a second one: an MCP
/// result is a tool result, and a flooding Upstream is contained by exactly the
/// bound `read` and `bash` already use. Redaction happens before the bound is
/// applied (CODING_STANDARDS §10.2), never after.
constexpr std::size_t kMaxResultBytes{50 * 1024};
constexpr std::size_t kMaxResultLines{2000};
/// The bound on any single Upstream-supplied explanation, matching the
/// connection machinery's own diagnostic bound (`kMaxDiagnosticBytes`).
constexpr std::size_t kMaxDiagnosticBytes{1024};

[[nodiscard]] support::OutputLimit result_output_limit() noexcept {
    return support::OutputLimit{.max_bytes = kMaxResultBytes, .max_lines = kMaxResultLines};
}

[[nodiscard]] agent::AsyncToolExecutionResult failure_result(std::string message) {
    return agent::AsyncToolExecutionResult{
            .content = std::vector<ai::Content>{ai::text_content(std::move(message))},
            .details = std::nullopt,
            .is_error = true,
    };
}

/// The details every completed upstream call carries: the reverse mapping the
/// generic tool renderer has no other way to recover, plus whether the
/// model-visible text is the whole result (ADR 0061 moved truncation metadata
/// out of `content` and into `details`).
[[nodiscard]] support::JsonValue call_details(
        const McpPublishedTool& published, bool truncated, std::size_t total_bytes) {
    return support::JsonValue{support::JsonValue::object_t{
            {"serverId", support::JsonValue(published.server_id)},
            {"toolName", support::JsonValue(published.tool_name)},
            {"qualifiedName", support::JsonValue(published.qualified_name)},
            {"truncated", support::JsonValue(truncated)},
            {"totalBytes", support::JsonValue(static_cast<double>(total_bytes))},
    }};
}

[[nodiscard]] boost::asio::awaitable<support::Expected<agent::AsyncToolExecutionResult>> execute_upstream_call(
        std::shared_ptr<mcp::UpstreamConnection> connection,
        McpPublishedTool published,
        mcp::UpstreamToolDescriptor descriptor,
        support::JsonValue arguments,
        std::stop_token stop_token) {
    auto called = co_await support::detail::await_async_result(connection->call_tool(
            mcp::UpstreamToolCall{.tool = std::move(descriptor), .arguments = std::move(arguments)}, stop_token));
    if (!called) {
        // A transport failure, a cancelled run, and a call against a
        // connection that is no longer up are operation errors here. They
        // become one failed tool call, never a session failure (ADR 0008).
        std::string detail = called.error().message;
        if (!called.error().detail.empty()) {
            detail += ": ";
            detail += called.error().detail;
        }
        co_return failure_result(support::bounded_redacted_text(std::move(detail), kMaxDiagnosticBytes, "..."));
    }
    if (called->is_error) {
        // An Upstream-reported failure and every protocol violation by the
        // Upstream land here: one failed call with a bounded, redacted
        // explanation (ADR 0008, spec #833 stories 23 and 32).
        co_return failure_result(support::bounded_redacted_text(
                called->diagnostic.empty() ? std::string{"the Upstream MCP Server reported a failed tool call"}
                                           : called->diagnostic,
                kMaxDiagnosticBytes,
                "..."));
    }

    auto serialized = support::write_json(called->content);
    if (!serialized) {
        co_return failure_result("the Upstream MCP Server's tool result is not serializable JSON");
    }
    // Redact the complete result before the bound is applied, exactly as the
    // shell tool does, so a secret is erased rather than truncated in half.
    const auto limit = result_output_limit();
    auto limited = support::truncate_output_tail(support::redact_text(std::move(*serialized)), limit);
    auto text = limited.text.empty() ? std::string{"(no output)"} : std::move(limited.text);
    co_return agent::AsyncToolExecutionResult{
            .content = std::vector<ai::Content>{ai::text_content(std::move(text))},
            .details = call_details(published, limited.truncated, limited.total_bytes),
            .is_error = false,
    };
}

/// The one `agent::Tool` value for one Upstream tool. The descriptor travels by
/// value into the closure because `tools/call` needs the whole descriptor
/// (the `x-mcp-header` annotations) and a closure that re-read the catalog would
/// race a catalog refresh.
[[nodiscard]] agent::Tool make_tool(std::shared_ptr<mcp::UpstreamConnection> connection,
        McpPublishedTool published,
        mcp::UpstreamToolDescriptor descriptor) {
    agent::Tool tool;
    tool.definition = ai::Tool{
            .name = published.qualified_name,
            .description = published.description,
            // The Upstream's own JSON Schema is the executable argument
            // contract. The Agent validates every call against it before the
            // call-approval policy hook and before execution (ADR 0007), so an
            // invalid call never reaches the Upstream.
            .parameters = descriptor.parameters,
    };
    // No prompt snippet: an upstream tool does not join the System Prompt's
    // `Available tools` list, and it must not widen the built-in tool surface
    // the prompt describes.
    tool.prompt_snippet = std::nullopt;
    tool.concurrency = agent::ToolConcurrency::ParallelSafe;
    tool.execute = [connection, published = std::move(published), descriptor = std::move(descriptor)](
                           agent::ToolInvocation invocation,
                           std::stop_token stop_token,
                           agent::ToolUpdateSink) -> agent::ToolExecuteResult {
        return support::detail::make_async_result([connection,
                                                          published = std::move(published),
                                                          descriptor = std::move(descriptor),
                                                          invocation = std::move(invocation),
                                                          stop_token]() mutable {
            return execute_upstream_call(std::move(connection),
                    std::move(published),
                    std::move(descriptor),
                    std::move(invocation.arguments),
                    stop_token);
        });
    };
    return tool;
}

} // namespace

support::ExpectedVoid McpToolBinding::publish(
        const std::shared_ptr<mcp::UpstreamConnection>& connection, mcp::UpstreamToolDescriptor descriptor) {
    if (connection == nullptr) {
        return std::unexpected(support::make_error(
                support::ErrorCode::Validation, "cannot publish an Upstream tool without a connection"));
    }
    if (descriptor.name.empty()) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation,
                "the Upstream advertised a tool without a name",
                connection->server_id()));
    }
    const std::string qualified = mcp_qualified_tool_name(connection->server_id(), descriptor.name);
    return stage(connection, std::move(descriptor), qualified);
}

support::ExpectedVoid McpToolBinding::stage(std::shared_ptr<mcp::UpstreamConnection> connection,
        mcp::UpstreamToolDescriptor descriptor,
        std::string qualified) {
    const std::string server_id = connection->server_id();
    const McpPublishedTool row{
            .qualified_name = qualified,
            .server_id = server_id,
            .tool_name = descriptor.name,
            .description = std::move(descriptor.description),
    };

    const std::scoped_lock lock(mutex_);
    if (closed_) {
        return std::unexpected(
                support::make_error(support::ErrorCode::Cancelled, "this session's MCP tool binding is closed"));
    }
    const auto known = published_.find(qualified);
    if (known != published_.end() && known->second.tool_name != row.tool_name) {
        // Two different Upstream tools that sanitize or truncate onto one
        // Qualified Tool Name. Refused rather than merged: the registry would
        // silently overwrite, and a model calling that name could reach the
        // wrong Upstream.
        return std::unexpected(support::make_error(support::ErrorCode::Validation,
                "the Qualified Tool Name is already held by another Upstream tool",
                qualified + " (server '" + known->second.server_id + "' tool '" + known->second.tool_name + "')"));
    }

    published_[qualified] = row;
    staged_[qualified] = row;
    // Republishing the same tool replaces its staged value rather than
    // stacking a second one: a reconnect must not produce a second
    // `toolsAdded` entry for a tool the transcript already declares.
    auto pending_position = std::ranges::find_if(
            pending_, [&qualified](const McpToolPublication& entry) { return entry.tool.qualified_name == qualified; });
    if (pending_position == pending_.end()) {
        pending_.push_back(
                McpToolPublication{.tool = row, .binding = make_tool(connection, row, std::move(descriptor))});
    } else {
        pending_position->tool = row;
        pending_position->binding = make_tool(std::move(connection), row, std::move(descriptor));
    }
    return {};
}

void McpToolBinding::set_servers(const std::vector<UserMcpServerSettings>& servers) {
    const std::scoped_lock lock(mutex_);
    approvals_.clear();
    for (const auto& server : servers) {
        approvals_.insert_or_assign(server.server_id, server.approval_policy());
    }
}

std::vector<McpToolPublication> McpToolBinding::take_pending() {
    const std::scoped_lock lock(mutex_);
    if (closed_) {
        return {};
    }
    std::vector<McpToolPublication> drained;
    drained.swap(pending_);
    return drained;
}

std::vector<McpPublishedTool> McpToolBinding::published() const {
    const std::scoped_lock lock(mutex_);
    std::vector<McpPublishedTool> rows;
    rows.reserve(published_.size());
    for (const auto& [name, row] : published_) {
        (void)name;
        rows.push_back(row);
    }
    return rows;
}

std::optional<std::string> McpToolBinding::server_id_for(std::string_view qualified_name) const {
    const std::scoped_lock lock(mutex_);
    const auto found = published_.find(qualified_name);
    if (found == published_.end()) {
        return std::nullopt;
    }
    return found->second.server_id;
}

McpServerApproval McpToolBinding::approval_for(std::string_view server_id) const {
    const std::scoped_lock lock(mutex_);
    const auto found = approvals_.find(server_id);
    if (found == approvals_.end()) {
        return McpServerApproval::Allow;
    }
    return found->second;
}

void McpToolBinding::close() noexcept {
    const std::scoped_lock lock(mutex_);
    closed_ = true;
    pending_.clear();
}

bool McpToolBinding::closed() const {
    const std::scoped_lock lock(mutex_);
    return closed_;
}

} // namespace cch::coding_agent::runtime
