#include "coding_agent/runtime/McpToolBinding.hpp"

#include "support/AsyncResultBridge.hpp"
#include "support/Json.hpp"

#include <cch/support/OutputLimiter.hpp>

#include <boost/asio/awaitable.hpp>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
/// How many catalog lines one `mcp_search` call returns. The catalog is a
/// discovery aid, not a context dump: the model narrows with `query` and
/// `server` and activates what it needs, so a page of 100 is generous for one
/// call and keeps a server advertising thousands of tools from flooding the
/// context. Recorded in `docs/runtime-capacities.md`.
constexpr std::size_t kMaxSearchRows{100};
/// The bound on one catalog line's description. An Upstream's own description
/// is a third party's text and is bounded like every other one.
constexpr std::size_t kMaxSearchLineBytes{240};
/// The bound on one server's own `instructions` before it is rendered into the
/// System Prompt (spec #833 story 18). The text is guidance, not an interface;
/// a server that writes a novel gets the first 2 KiB and a truncation mark.
constexpr std::size_t kMaxInstructionsBytes{2 * 1024};

/// What the tool-execution display keeps of a long upstream operation's
/// progress (spec #833 story 31). The sink publishes a *cumulative* partial
/// result, so what it publishes is the retained lines rather than only the
/// newest one, and the retention is bounded twice: a flooding Upstream cannot
/// make a tool's display grow without limit, and the oldest line is dropped
/// rather than the newest, because the newest is the one the user is watching.
constexpr std::size_t kMaxProgressLines{16};
/// The bound on one Upstream-supplied progress message. `cch_mcp` already
/// bounded and redacted it (`mcp/Diagnostics.hpp`); this is the display's own
/// second bound, so a line is short before it is ever laid out.
constexpr std::size_t kMaxProgressMessageBytes{200};

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

/// One progress line as the display shows it: the Upstream's own counter, the
/// total when it declared one, and its message when it sent one. An Upstream
/// that reports neither a total nor a message still shows movement, because
/// the counter alone is the movement.
[[nodiscard]] std::string progress_line(const mcp::UpstreamToolProgress& progress) {
    std::string line = support::bounded_redacted_text(progress.message, kMaxProgressMessageBytes, "…");
    std::string counter = std::to_string(static_cast<long long>(progress.progress));
    if (progress.total.has_value()) {
        counter += "/" + std::to_string(static_cast<long long>(*progress.total));
    }
    if (line.empty()) {
        return counter;
    }
    return counter + " — " + line;
}

/// The progress one in-flight upstream call has published, and the sink that
/// re-issues it. It is shared rather than captured by value because both the
/// move-only `ToolUpdateSink` and the retained lines have to outlive a single
/// publication: `cch_mcp`'s progress sink is move-only, callable more than
/// once, and held by an operation that may complete after this coroutine has
/// resumed, so nothing in it may be a reference into the coroutine frame.
struct UpstreamProgressDisplay {
    /// The reverse mapping each partial result carries, copied so the display
    /// survives this coroutine's own settlement.
    McpPublishedTool published{};
    /// The agent's partial-result sink, or null when the caller wants none.
    agent::ToolUpdateSink sink{nullptr};
    /// The retained lines, oldest first, bounded by `kMaxProgressLines`.
    std::deque<std::string> lines{};
    /// The most recent values, carried into `details` so a Tool Renderer
    /// (ADR 0061) can draw a counter instead of parsing the lines back out of
    /// the display text.
    double progress{0.0};
    std::optional<double> total{std::nullopt};
    std::string message{};

    [[nodiscard]] std::string text() const {
        std::string joined;
        for (const auto& line : lines) {
            if (!joined.empty()) {
                joined += '\n';
            }
            joined += line;
        }
        return joined;
    }
};

/// A tool result the meta-tools produce without suspending: the catalog is
/// already discovered and the activation is a map edit, so the whole operation
/// is inline and no Upstream request is involved.
[[nodiscard]] agent::ToolExecuteResult ready(agent::AsyncToolExecutionResult result) {
    return agent::ToolExecuteResult(std::expected<agent::AsyncToolExecutionResult, support::Error>{std::move(result)});
}

[[nodiscard]] agent::AsyncToolExecutionResult text_result(std::string text) {
    return agent::AsyncToolExecutionResult{
            .content = std::vector<ai::Content>{ai::text_content(std::move(text))},
            .details = std::nullopt,
            .is_error = false,
    };
}

/// One JSON Schema object property: a named type with its own description, the
/// shape every tool definition in the product uses.
[[nodiscard]] support::JsonValue property_schema(std::string type, std::string description) {
    return support::JsonValue{support::JsonValue::object_t{
            {"type", support::JsonValue(std::move(type))},
            {"description", support::JsonValue(std::move(description))},
    }};
}

/// One JSON Schema object, with `additionalProperties` refused exactly as the
/// built-in tools declare it.
[[nodiscard]] support::JsonValue object_schema(
        support::JsonValue::object_t properties, std::vector<std::string> required) {
    support::JsonValue::array_t required_values;
    required_values.reserve(required.size());
    for (auto& name : required) {
        required_values.emplace_back(std::move(name));
    }
    return support::JsonValue{support::JsonValue::object_t{
            {"type", support::JsonValue("object")},
            {"properties", support::JsonValue(std::move(properties))},
            {"required", support::JsonValue(std::move(required_values))},
            {"additionalProperties", support::JsonValue(false)},
    }};
}

/// An optional string argument the model passed, or an empty string when it
/// passed none. A non-string value cannot reach here — the Agent validates
/// every call against this tool's own schema first (ADR 0007) — and a
/// non-string that somehow did is reported rather than coerced.
[[nodiscard]] std::expected<std::string, std::string> string_argument(
        const support::JsonValue& arguments, std::string_view name) {
    const auto* object = arguments.get_if<support::JsonValue::object_t>();
    if (object == nullptr) {
        return std::unexpected("the arguments are not a JSON object");
    }
    const auto found = object->find(std::string{name});
    if (found == object->end()) {
        return std::string{};
    }
    const auto* value = found->second.get_if<std::string>();
    if (value == nullptr) {
        return std::unexpected(std::string{name} + " must be a string");
    }
    return *value;
}

/// The one-line form of an Upstream's own description: whitespace runs
/// collapse to a single space and the line is bounded, because the catalog
/// line is a *catalog* entry, not the tool's documentation.
[[nodiscard]] std::string one_line_bounded(std::string_view text) {
    std::string one_line;
    one_line.reserve(std::min(text.size(), kMaxSearchLineBytes));
    bool pending_space = false;
    for (const char character : text) {
        if (std::isspace(static_cast<unsigned char>(character))) {
            pending_space = true;
            continue;
        }
        if (pending_space && !one_line.empty()) {
            one_line += ' ';
        }
        pending_space = false;
        one_line += character;
    }
    return support::bounded_redacted_text(std::move(one_line), kMaxSearchLineBytes, "...");
}

/// Whether one catalogued tool matches a `mcp_search` query. An empty query or
/// Server Id matches everything; a query is a case-insensitive substring over
/// the four fields a model would search by.
[[nodiscard]] bool matches_query(const McpPublishedTool& row, std::string_view query, std::string_view server_id) {
    if (!server_id.empty() && row.server_id != server_id) {
        return false;
    }
    if (query.empty()) {
        return true;
    }
    const auto contains_ignoring_case = [query](std::string_view haystack) {
        if (haystack.size() < query.size()) {
            return false;
        }
        return std::search(haystack.begin(), haystack.end(), query.begin(), query.end(), [](char left, char right) {
            return std::tolower(static_cast<unsigned char>(left)) == std::tolower(static_cast<unsigned char>(right));
        }) != haystack.end();
    };
    return contains_ignoring_case(row.qualified_name) || contains_ignoring_case(row.tool_name) ||
           contains_ignoring_case(row.server_id) || contains_ignoring_case(row.description);
}

/// One compact catalog line: Server Id, Qualified Tool Name, one-line
/// description. The columns are tab-separated so a description containing a
/// space cannot be mistaken for another column. No JSON Schema ever appears
/// here — that is the entire context economy of Lazy Tool Activation (spec
/// #833 stories 12 and 13).
[[nodiscard]] std::string catalog_line(const McpPublishedTool& row) {
    std::string line = row.server_id;
    line += '\t';
    line += row.qualified_name;
    line += '\t';
    line += one_line_bounded(row.description);
    return line;
}

/// The `mcp_search` result text for one call: the matching compact lines, and
/// an explicit statement when the page was cut or nothing matched.
[[nodiscard]] std::string search_text(
        const std::vector<McpPublishedTool>& rows, std::string_view query, std::string_view server_id) {
    if (rows.empty()) {
        return "No discovered Upstream MCP tool matches this search. "
               "Every connected upstream server is listed by calling mcp_search with no arguments.";
    }
    std::string text;
    std::size_t shown = 0;
    for (const auto& row : rows) {
        if (shown == kMaxSearchRows) {
            break;
        }
        if (shown != 0) {
            text += '\n';
        }
        text += catalog_line(row);
        shown += 1;
    }
    if (rows.size() > shown) {
        text += "\n... ";
        text += std::to_string(rows.size() - shown);
        text += " more tool(s) match; narrow the search with a query or a server id.";
    }
    (void)query;
    (void)server_id;
    return text;
}

/// The `mcp_activate` result text for one call.
[[nodiscard]] std::string activation_text(McpActivationOutcome outcome, const McpPublishedTool& row) {
    switch (outcome) {
    case McpActivationOutcome::Activated:
        return "Activated " + row.qualified_name +
               ". Its schema is in your tool list from the next turn onward; call it by that name.";
    case McpActivationOutcome::AlreadyActive:
        break;
    }
    return row.qualified_name + " is already active; nothing changed.";
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

/// The partial result a progress publication re-issues: the same reverse
/// mapping every settled result carries, plus the progress facts. It is
/// display-only — a `ToolUpdateSink` result never reaches the model's context
/// or the transcript (ADR 0052), so progress costs the model nothing and
/// disappears when the call settles.
[[nodiscard]] agent::AsyncToolExecutionResult progress_result(
        const McpPublishedTool& published, const UpstreamProgressDisplay& display) {
    auto details = call_details(published, false, 0).get_object();
    details["progress"] = support::JsonValue{support::JsonValue::object_t{
            {"progress", support::JsonValue(display.progress)},
            {"total", display.total.has_value() ? support::JsonValue(*display.total) : support::JsonValue{nullptr}},
            {"message", support::JsonValue(display.message)},
    }};
    return agent::AsyncToolExecutionResult{
            .content = std::vector<ai::Content>{ai::text_content(display.text())},
            .details = support::JsonValue(std::move(details)),
            .is_error = false,
    };
}

[[nodiscard]] boost::asio::awaitable<support::Expected<agent::AsyncToolExecutionResult>> execute_upstream_call(
        std::shared_ptr<mcp::UpstreamConnection> connection,
        McpPublishedTool published,
        mcp::UpstreamToolDescriptor descriptor,
        support::JsonValue arguments,
        agent::ToolUpdateSink update_sink,
        std::stop_token stop_token) {
    // The run's stop token reaches `call_tool` unchanged, so a cancelled prompt
    // closes the upstream response stream and sends `notifications/cancelled`
    // through the one stop vocabulary the rest of the product already uses
    // (ADR 0020). Nothing here bridges cancellation itself.
    auto progress = std::make_shared<UpstreamProgressDisplay>(
            UpstreamProgressDisplay{.published = published, .sink = std::move(update_sink)});
    auto called = co_await support::detail::await_async_result(connection->call_tool(
            mcp::UpstreamToolCall{.tool = std::move(descriptor), .arguments = std::move(arguments)},
            stop_token,
            mcp::UpstreamProgressSink([progress](const mcp::UpstreamToolProgress& reported) {
                if (!progress->sink) {
                    return;
                }
                progress->lines.push_back(progress_line(reported));
                while (progress->lines.size() > kMaxProgressLines) {
                    progress->lines.pop_front();
                }
                progress->progress = reported.progress;
                progress->total = reported.total;
                progress->message = reported.message;
                // A refused publication means the display is gone; the call is
                // not failed for it, because the work is the Upstream's
                // (ADR 0008).
                (void)progress->sink(progress_result(progress->published, *progress));
            })));
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
                           agent::ToolUpdateSink update_sink) -> agent::ToolExecuteResult {
        return support::detail::make_async_result([connection,
                                                          published = std::move(published),
                                                          descriptor = std::move(descriptor),
                                                          invocation = std::move(invocation),
                                                          stop_token,
                                                          update_sink = std::move(update_sink)]() mutable {
            return execute_upstream_call(std::move(connection),
                    std::move(published),
                    std::move(descriptor),
                    std::move(invocation.arguments),
                    std::move(update_sink),
                    stop_token);
        });
    };
    return tool;
}

/// The `mcp_search` result for one call: the catalog rows the binding holds,
/// and nothing else. The search never consults the Upstream — the catalogs are
/// already discovered — so a search is as cheap as reading a map and never
/// waits.
[[nodiscard]] agent::AsyncToolExecutionResult run_search(
        const std::shared_ptr<McpToolBinding>& binding, const agent::ToolInvocation& invocation) {
    const auto query = string_argument(invocation.arguments, "query");
    if (!query) {
        return failure_result(query.error());
    }
    const auto server = string_argument(invocation.arguments, "server");
    if (!server) {
        return failure_result(server.error());
    }
    return text_result(search_text(binding->search(*query, *server), *query, *server));
}

/// The `mcp_activate` result for one call. A name no catalog holds is an
/// error result, not a thrown failure: the model asked for something that does
/// not exist, and one failed tool call is the whole of the consequence
/// (ADR 0008).
[[nodiscard]] agent::AsyncToolExecutionResult run_activation(
        const std::shared_ptr<McpToolBinding>& binding, const agent::ToolInvocation& invocation) {
    const auto name = string_argument(invocation.arguments, "name");
    if (!name) {
        return failure_result(name.error());
    }
    if (name->empty()) {
        return failure_result("name is empty; call mcp_search to list the discovered Upstream tools");
    }
    const auto activated = binding->activate(*name);
    if (!activated) {
        return failure_result("no discovered Upstream tool is named " + *name +
                              "; call mcp_search to list the tools these servers offer");
    }
    const auto row = binding->row_for(*name);
    return text_result(activation_text(*activated, row ? *row : McpPublishedTool{.qualified_name = *name}));
}

/// The two built-in meta-tools, staged on the first connected `lazy` Upstream
/// (spec #833 stories 13, 14, 15; issue #847). They hold the binding weakly:
/// the Agent owns the tool values and the session owns the binding, so neither
/// can keep the other alive.
[[nodiscard]] std::vector<agent::Tool> make_meta_tools(std::weak_ptr<McpToolBinding> binding) {
    auto search = [binding](agent::ToolInvocation invocation,
                          std::stop_token,
                          agent::ToolUpdateSink) -> agent::ToolExecuteResult {
        const auto owner = binding.lock();
        if (owner == nullptr) {
            return ready(failure_result("this session's MCP tool catalog is gone"));
        }
        return ready(run_search(owner, invocation));
    };
    auto activate = [binding](agent::ToolInvocation invocation,
                            std::stop_token,
                            agent::ToolUpdateSink) -> agent::ToolExecuteResult {
        const auto owner = binding.lock();
        if (owner == nullptr) {
            return ready(failure_result("this session's MCP tool catalog is gone"));
        }
        return ready(run_activation(owner, invocation));
    };

    agent::Tool search_tool;
    search_tool.definition = ai::Tool{
            .name = std::string{kMcpSearchToolName},
            .description = "List the tools the connected Upstream MCP Servers offer, one compact line each: "
                           "server id, qualified tool name, one-line description. Use the qualified name to "
                           "activate a tool with mcp_activate; the tool's argument schema appears in your tool "
                           "list only after it is activated.",
            .parameters = object_schema(
                    {{"query",
                             property_schema("string",
                                     "Substring to match against a tool name, qualified name, "
                                     "or description. Omit to list everything.")},
                            {"server", property_schema("string", "One server id to list. Omit to list every server.")}},
                    {}),
    };
    search_tool.prompt_snippet = std::nullopt;
    search_tool.concurrency = agent::ToolConcurrency::ParallelSafe;
    search_tool.execute = search;

    agent::Tool activate_tool;
    activate_tool.definition = ai::Tool{
            .name = std::string{kMcpActivateToolName},
            .description = "Add one discovered Upstream MCP tool to your tool list for the rest of this session, "
                           "by the qualified name mcp_search reported. Activation needs no approval and does not "
                           "call the server; the tool's schema reaches your next request.",
            .parameters = object_schema({{"name",
                                                property_schema("string",
                                                        "The qualified tool name, as mcp_search reported it "
                                                        "(mcp__<server>__<tool>).")}},
                    {"name"}),
    };
    activate_tool.prompt_snippet = std::nullopt;
    activate_tool.concurrency = agent::ToolConcurrency::ParallelSafe;
    activate_tool.execute = activate;
    std::vector<agent::Tool> tools;
    tools.reserve(2);
    tools.push_back(std::move(search_tool));
    tools.push_back(std::move(activate_tool));
    return tools;
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
    // A drained publication is registered by the session at the turn boundary
    // it drained on, so the catalogued tool is active from there on and a
    // later catalog refresh will not stage it a second time.
    for (const auto& publication : drained) {
        const auto found = catalog_.find(publication.tool.qualified_name);
        if (found != catalog_.end() && found->second.activation == Activation::Staged) {
            found->second.activation = Activation::Active;
        }
    }
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

std::optional<McpPublishedTool> McpToolBinding::publication_for(std::string_view qualified_name) const {
    const std::scoped_lock lock(mutex_);
    const auto found = published_.find(qualified_name);
    if (found == published_.end()) {
        return std::nullopt;
    }
    return found->second;
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
    pending_meta_tools_.clear();
}

bool McpToolBinding::closed() const {
    const std::scoped_lock lock(mutex_);
    return closed_;
}

support::ExpectedVoid McpToolBinding::refuse_name_collision_locked(
        const std::string& qualified_name, std::string_view tool_name) const {
    // Two different Upstream tools that sanitize or truncate onto one
    // Qualified Tool Name. Refused rather than merged, on both the eager and
    // the lazy path: the registry would silently overwrite, and a model
    // calling that name could reach the wrong Upstream.
    const auto known = published_.find(qualified_name);
    if (known != published_.end() && known->second.tool_name != tool_name) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation,
                "the Qualified Tool Name is already held by another Upstream tool",
                qualified_name + " (server '" + known->second.server_id + "' tool '" + known->second.tool_name + "')"));
    }
    const auto catalogued = catalog_.find(qualified_name);
    if (catalogued != catalog_.end() && catalogued->second.tool.tool_name != tool_name) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation,
                "the Qualified Tool Name is already held by another Upstream tool",
                qualified_name + " (server '" + catalogued->second.tool.server_id + "' tool '" +
                        catalogued->second.tool.tool_name + "')"));
    }
    return {};
}

void McpToolBinding::stage_activation_locked(std::map<std::string, CatalogEntry, std::less<>>::iterator& entry) {
    const std::string qualified = entry->first;
    const McpPublishedTool row = entry->second.tool;
    // The catalogued entry keeps its own connection and descriptor: a staged
    // value the session never drains must still be stageable again, and a
    // refresh must not leave the catalog holding a moved-from descriptor.
    auto binding = make_tool(entry->second.connection, row, entry->second.descriptor);
    // Re-staging a name the session has not drained yet replaces the staged
    // value rather than stacking a second one, exactly as the eager
    // publication path does.
    auto position = std::ranges::find_if(pending_,
            [&qualified](const McpToolPublication& staged) { return staged.tool.qualified_name == qualified; });
    if (position == pending_.end()) {
        pending_.push_back(McpToolPublication{.tool = row, .binding = std::move(binding)});
    } else {
        position->tool = row;
        position->binding = std::move(binding);
    }
    entry->second.activation = Activation::Staged;
}

support::ExpectedVoid McpToolBinding::record_lazy_catalog(
        const std::shared_ptr<mcp::UpstreamConnection>& connection, mcp::UpstreamCatalog catalog) {
    if (connection == nullptr) {
        return std::unexpected(support::make_error(
                support::ErrorCode::Validation, "cannot record an Upstream catalog without a connection"));
    }
    // The meta-tools reach the model through the Agent, which outlives the
    // call that discovers them, so their closures hold this value weakly. A
    // binding no `shared_ptr` owns cannot produce that weak handle, and
    // recording into it would stage tools nothing can ever drain.
    const std::weak_ptr<McpToolBinding> self = weak_from_this();
    if (self.expired()) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation,
                "a lazy Upstream catalog needs a tool binding owned by a session",
                "the meta-tools are staged with a weak handle to this value"));
    }

    const std::string server_id = connection->server_id();
    const std::scoped_lock lock(mutex_);
    if (closed_) {
        return std::unexpected(
                support::make_error(support::ErrorCode::Cancelled, "this session's MCP tool binding is closed"));
    }
    // The server's own usage guidance reaches the model through the System
    // Prompt (spec #833 story 18), bounded like every other Upstream-supplied
    // text.
    if (!catalog.instructions.empty()) {
        instructions_[server_id] =
                support::bounded_redacted_text(std::move(catalog.instructions), kMaxInstructionsBytes, "...");
    }
    for (auto& descriptor : catalog.tools) {
        if (descriptor.name.empty()) {
            // An entry with no tool name has no Qualified Tool Name and cannot
            // be called; `cch_mcp` refuses such a `tools/list` result outright,
            // so reaching this is a third party's malformed catalog and costs
            // one row rather than the whole catalog.
            continue;
        }
        const std::string qualified = mcp_qualified_tool_name(server_id, descriptor.name);
        if (auto refused = refuse_name_collision_locked(qualified, descriptor.name); !refused) {
            // Dropped rather than merged onto the tool that already holds the
            // name, exactly as the eager publication path does: one refused
            // row costs one row, not the catalog (ADR 0008).
            continue;
        }
        const McpPublishedTool row{
                .qualified_name = qualified,
                .server_id = server_id,
                .tool_name = descriptor.name,
                .description = descriptor.description,
        };
        // `try_emplace` adds the row and leaves a row a refresh already
        // stored alone; the three assignments below then replace the
        // descriptor without touching the activation state, so a live tool is
        // never un-activated and never re-bound. Either would invalidate the
        // provider's prompt cache (ADR 0064, spec #833 story 16).
        auto entry = catalog_.try_emplace(qualified).first;
        entry->second.tool = row;
        entry->second.connection = connection;
        entry->second.descriptor = std::move(descriptor);
        if (entry->second.activation != Activation::Catalogued) {
            continue;
        }
        // A name the resumed transcript records as active is staged the moment
        // its catalog lands, which is how a resumed session restores its
        // active loadout without blocking its start on discovery (ADR 0066).
        if (std::ranges::find(restore_names_, qualified) != restore_names_.end()) {
            stage_activation_locked(entry);
        }
    }
    if (!meta_tools_staged_) {
        // The registration condition of the spec: the meta-tools exist once at
        // least one `lazy` Upstream is connected, and never otherwise.
        meta_tools_staged_ = true;
        pending_meta_tools_ = make_meta_tools(self);
    }
    return {};
}

std::vector<McpPublishedTool> McpToolBinding::search(std::string_view query, std::string_view server_id) const {
    const std::scoped_lock lock(mutex_);
    std::vector<McpPublishedTool> rows;
    rows.reserve(catalog_.size());
    for (const auto& [qualified, entry] : catalog_) {
        (void)qualified;
        if (matches_query(entry.tool, query, server_id)) {
            rows.push_back(entry.tool);
        }
    }
    return rows;
}

std::optional<McpPublishedTool> McpToolBinding::row_for(std::string_view qualified_name) const {
    const std::scoped_lock lock(mutex_);
    const auto found = catalog_.find(qualified_name);
    if (found == catalog_.end()) {
        return std::nullopt;
    }
    return found->second.tool;
}

support::Expected<McpActivationOutcome> McpToolBinding::activate(std::string_view qualified_name) {
    const std::scoped_lock lock(mutex_);
    if (closed_) {
        return std::unexpected(
                support::make_error(support::ErrorCode::Cancelled, "this session's MCP tool binding is closed"));
    }
    const auto found = catalog_.find(qualified_name);
    if (found == catalog_.end()) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation,
                "no discovered Upstream tool carries that qualified name",
                std::string{qualified_name}));
    }
    if (found->second.activation != Activation::Catalogued) {
        // Sticky: activation is once per session, and a second request is a
        // success that changed nothing rather than a duplicate publication.
        return McpActivationOutcome::AlreadyActive;
    }
    auto entry = found;
    stage_activation_locked(entry);
    return McpActivationOutcome::Activated;
}

void McpToolBinding::set_restore_names(std::vector<std::string> qualified_names) {
    const std::scoped_lock lock(mutex_);
    if (closed_) {
        return;
    }
    for (auto& qualified : qualified_names) {
        if (qualified.empty()) {
            continue;
        }
        auto found = catalog_.find(qualified);
        if (found == catalog_.end()) {
            // The catalog has not been discovered for this name yet. It is
            // remembered, because discovery never blocks session start and the
            // name is restored the moment its catalog lands instead.
            if (std::ranges::find(restore_names_, qualified) == restore_names_.end()) {
                restore_names_.push_back(std::move(qualified));
            }
            continue;
        }
        if (found->second.activation != Activation::Catalogued) {
            continue;
        }
        auto entry = found;
        stage_activation_locked(entry);
    }
}

std::vector<agent::Tool> McpToolBinding::take_pending_meta_tools() {
    const std::scoped_lock lock(mutex_);
    if (closed_) {
        return {};
    }
    std::vector<agent::Tool> drained;
    drained.swap(pending_meta_tools_);
    return drained;
}

std::vector<McpServerInstructions> McpToolBinding::instructions() const {
    const std::scoped_lock lock(mutex_);
    std::vector<McpServerInstructions> rows;
    rows.reserve(instructions_.size());
    for (const auto& [server_id, text] : instructions_) {
        rows.push_back(McpServerInstructions{.server_id = server_id, .text = text});
    }
    return rows;
}

} // namespace cch::coding_agent::runtime
