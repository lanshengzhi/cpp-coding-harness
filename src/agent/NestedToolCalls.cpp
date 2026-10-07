#include <cch/agent/NestedToolCalls.hpp>

#include "support/AsyncResultBridge.hpp"
#include "support/Json.hpp"

#include <cch/ai/Content.hpp>

#include <boost/asio/awaitable.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace cch::agent {

namespace {

/// pi `textOf(result)`: the text content blocks joined with a newline.
[[nodiscard]] std::string text_of(const std::vector<ai::Content>& content) {
    return ai::text_from_content(content);
}

/// pi `NestedCallRecorder`: collects the nested calls of one model-issued tool
/// call, including calls made by nested tools. The snapshot becomes
/// `nested_calls` on the tool result message.
class NestedCallRecorder {
public:
    /// Record a call as it starts. Returns the record's index, or nullopt when
    /// the call is dropped for the count limit.
    [[nodiscard]] std::optional<std::size_t> start(const ToolInvocation& call) {
        if (calls_.size() >= kNestedCallLimits.max_calls) {
            complete_ = false;
            return std::nullopt;
        }
        NestedToolCallRecord record;
        record.id = call.call_id;
        record.name = call.name;
        record.status = "unfinished";
        std::string json = "{}";
        if (auto written = support::write_json(call.arguments); written) {
            json = std::move(*written);
        }
        const std::uint64_t bytes = json.size();
        if (bytes > kNestedCallLimits.max_argument_bytes_per_call ||
                argument_bytes_ + bytes > kNestedCallLimits.max_argument_bytes_total) {
            record.arguments_bytes = bytes;
            complete_ = false;
        } else {
            record.arguments = call.arguments;
            argument_bytes_ += bytes;
        }
        calls_.push_back(std::move(record));
        return calls_.size() - 1;
    }

    void finish(std::optional<std::size_t> index, bool is_error, std::string_view error_text,
            std::uint64_t duration_ms) {
        if (!index || *index >= calls_.size()) return;
        NestedToolCallRecord& record = calls_[*index];
        record.status = is_error ? "error" : "ok";
        record.duration_ms = duration_ms;
        if (is_error && !error_text.empty()) {
            record.error = std::string{error_text.substr(0, kNestedCallLimits.max_error_chars)};
        }
    }

    /// Copy of the record so far, or nullopt when no nested call was made.
    [[nodiscard]] std::optional<NestedToolCalls> snapshot() const {
        if (calls_.empty() && complete_) return std::nullopt;
        NestedToolCalls snapshot;
        snapshot.calls = calls_;
        snapshot.complete = complete_ &&
                std::all_of(snapshot.calls.begin(), snapshot.calls.end(), [](const NestedToolCallRecord& call) {
                    return call.status != "unfinished";
                });
        return snapshot;
    }

private:
    std::vector<NestedToolCallRecord> calls_;
    bool complete_{true};
    std::uint64_t argument_bytes_{0};
};

} // namespace

/// Calls below one model-issued call share its recorder (pi `CallScope`).
struct NestedToolCallRunner::Impl {
    struct CallScope {
        std::shared_ptr<NestedCallRecorder> recorder;
        std::uint64_t next_id{1};
    };

    explicit Impl(NestedToolCallHost host_in) : host(std::move(host_in)) {}

    NestedToolCallHost host;
    /// The owning runner, so a nested call's invocation carries the same
    /// dispatcher and further nesting is recorded on the same result.
    NestedToolCallRunner* owner{nullptr};
    std::map<std::string, CallScope, std::less<>> scopes;

    [[nodiscard]] boost::asio::awaitable<AsyncToolExecutionResult> execute(
            std::string caller_id, std::string name, support::JsonValue arguments, std::stop_token signal) {
        auto scope_it = scopes.find(caller_id);
        if (scope_it == scopes.end()) {
            scope_it = scopes.emplace(caller_id, CallScope{std::make_shared<NestedCallRecorder>(), 1}).first;
        }
        auto& scope = scope_it->second;
        const std::string nested_id = caller_id + "/" + std::to_string(scope.next_id++);

        ToolInvocation invocation;
        invocation.call_id = nested_id;
        invocation.name = name;
        invocation.arguments = arguments;
        if (auto written = support::write_json(arguments); written) {
            invocation.raw_arguments = std::move(*written);
        }
        invocation.nested_calls = owner;

        std::optional<std::size_t> record = scope.recorder->start(invocation);
        const auto started_at = std::chrono::steady_clock::now();
        (void)host.emit(ToolExecutionStartEvent{
                .tool_call_id = nested_id,
                .tool_name = name,
                .args = invocation.arguments,
                .parent_tool_call_id = caller_id,
        });
        scopes.emplace(nested_id, CallScope{scope.recorder, 1});

        auto awaited = co_await support::detail::await_async_result(
                host.run_tool_call(std::move(invocation), caller_id, signal));
        AsyncToolExecutionResult outcome;
        if (awaited) {
            outcome = std::move(*awaited);
        } else {
            outcome.content.emplace_back(ai::text_content(awaited.error().detail.empty()
                            ? awaited.error().message
                            : awaited.error().detail));
            outcome.is_error = true;
        }

        scopes.erase(nested_id);
        const auto duration_ms = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started_at)
                        .count());
        const std::string error_text = outcome.is_error ? text_of(outcome.content) : std::string{};
        scope.recorder->finish(record, outcome.is_error, error_text, duration_ms);
        (void)host.emit(ToolExecutionEndEvent{
                .tool_call_id = nested_id,
                .tool_name = name,
                .result = outcome,
                .is_error = outcome.is_error,
                .parent_tool_call_id = caller_id,
        });
        co_return outcome;
    }
};

NestedToolCallRunner::NestedToolCallRunner(NestedToolCallHost host)
    : impl_(std::make_unique<Impl>(std::move(host))) {
    impl_->owner = this;
}

NestedToolCallRunner::NestedToolCallRunner(NestedToolCallRunner&&) noexcept = default;
NestedToolCallRunner& NestedToolCallRunner::operator=(NestedToolCallRunner&&) noexcept = default;
NestedToolCallRunner::~NestedToolCallRunner() = default;

support::AsyncResult<AsyncToolExecutionResult> NestedToolCallRunner::execute(
        std::string caller_id, std::string name, support::JsonValue arguments, std::stop_token signal) {
    return support::detail::make_async_result(
            [this,
                    caller_id = std::move(caller_id),
                    name = std::move(name),
                    arguments = std::move(arguments),
                    signal]() -> boost::asio::awaitable<support::Expected<AsyncToolExecutionResult>> {
                co_return co_await impl_->execute(std::move(caller_id), std::move(name), std::move(arguments), signal);
            });
}

std::optional<NestedCallSummary> NestedToolCallRunner::take_record(std::string_view caller_id) {
    auto it = impl_->scopes.find(caller_id);
    if (it == impl_->scopes.end()) {
        return std::nullopt;
    }
    auto recorder = std::move(it->second.recorder);
    impl_->scopes.erase(it);
    NestedCallSummary summary;
    summary.calls = recorder->snapshot();
    return summary;
}

void NestedToolCallRunner::clear() { impl_->scopes.clear(); }

std::vector<ai::Tool> NestedToolCallRunner::tools() const {
    return impl_->host.get_tools ? impl_->host.get_tools() : std::vector<ai::Tool>{};
}

bool NestedToolCallRunner::is_sequential() const {
    return impl_->host.is_sequential && impl_->host.is_sequential();
}

support::JsonValue nested_calls_to_json(const NestedToolCalls& calls) {
    support::JsonValue::array_t records;
    records.reserve(calls.calls.size());
    for (const auto& call : calls.calls) {
        support::JsonValue::object_t record{
                {"id", call.id},
                {"name", call.name},
                {"status", call.status},
        };
        if (call.duration_ms.has_value()) {
            record.emplace("durationMs", static_cast<double>(*call.duration_ms));
        }
        if (call.arguments.has_value()) {
            record.emplace("arguments", *call.arguments);
        }
        if (call.arguments_bytes.has_value()) {
            record.emplace("argumentsBytes", static_cast<double>(*call.arguments_bytes));
        }
        if (call.error.has_value()) {
            record.emplace("error", *call.error);
        }
        records.emplace_back(std::move(record));
    }
    return support::JsonValue{support::JsonValue::object_t{
            {"calls", std::move(records)},
            {"complete", calls.complete},
    }};
}

} // namespace cch::agent
