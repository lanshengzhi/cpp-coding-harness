// ADR 0052 Projection Stream evidence (#617): the subscription seam replaces
// the three-method sampling contract — attach delivers a Base then ordered
// PatchMsg batches to concurrent subscribers, per-subscriber bounded mailboxes
// resynchronize with a fresh Base on overflow without ever erroring or
// blocking the Core's serialized domain, and patch application converges with
// the Core snapshot for a streaming tool partial and a message-chunk burst.
// The scripted fake `Models` seam serves every request; no live keys or
// network.

#include "ai/ModelStreamBridge.hpp"
#include "support/FakeTool.hpp"
#include "support/ModelsFixture.hpp"
#include "support/RuntimeFixture.hpp"
#include "support/TempWorkspace.hpp"

#include <cch/ai/Content.hpp>
#include <cch/ai/Message.hpp>
#include "coding_agent/AgentSession.hpp"
#include <cch/coding_agent/ProjectionStream.hpp>
#include "coding_agent/runtime/SessionFactory.hpp"

#include <catch2/catch_test_macros.hpp>
#include <boost/asio/awaitable.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace cch;
using coding_agent::ProjectionStreamBase;
using coding_agent::ProjectionStreamMessageVariant;
using coding_agent::ProjectionStreamPatchMsg;

namespace {

/// Scripted provider whose stream emits AssistantStart + TextStart followed by
/// a caller-chosen number of TextDelta chunks, then completes. The agent
/// reducer emits one MessageUpdateEvent per chunk, so `chunk_count` drives the
/// exact number of projection publications a prompt produces.
class ChunkedProjectionProvider : public tests::ScriptedProvider {
public:
    ChunkedProjectionProvider() : ScriptedProvider("fake") {}

    void set_chunk_text(std::string text) { chunk_text_ = std::move(text); }

    void set_chunk_count(std::size_t count) { chunk_count_ = count; }

    [[nodiscard]] std::chrono::nanoseconds delta_loop_elapsed() const noexcept { return *delta_loop_elapsed_; }

    [[nodiscard]] ai::ModelStream stream(
            ai::Model model, ai::AiContext, coding_agent::ModelRuntimeTestStreamOptions) override {
        const std::size_t chunk_count = chunk_count_;
        const std::string chunk_text = chunk_text_;
        const auto elapsed = delta_loop_elapsed_;
        return ai::detail::make_model_stream(
                [model = std::move(model), chunk_count, chunk_text, elapsed](ai::AssistantEventSink sink) mutable
                        -> boost::asio::awaitable<support::Expected<ai::AssistantMessage>> {
                    auto partial = ai::assistant_text_message("");
                    partial.provider = "projection-fake";
                    partial.api = "fake";
                    partial.model = model.id;
                    partial.content.clear();
                    partial.content.emplace_back(ai::text_content(""));
                    if (sink) {
                        if (auto emitted = sink(ai::AssistantStartEvent{.partial = partial}); !emitted) {
                            co_return std::unexpected(emitted.error());
                        }
                        if (auto emitted = sink(ai::TextStartEvent{.content_index = 0, .partial = partial}); !emitted) {
                            co_return std::unexpected(emitted.error());
                        }
                        const auto loop_start = std::chrono::steady_clock::now();
                        for (std::size_t index = 0; index < chunk_count; ++index) {
                            std::get<ai::TextContent>(partial.content[0]).text += chunk_text;
                            if (auto emitted = sink(ai::TextDeltaEvent{
                                        .content_index = 0,
                                        .delta = chunk_text,
                                        .partial = partial,
                                });
                                    !emitted) {
                                co_return std::unexpected(emitted.error());
                            }
                        }
                        *elapsed = std::chrono::steady_clock::now() - loop_start;
                    }
                    partial.stop_reason = ai::AssistantStopReason::Stop;
                    co_return partial;
                });
    }

private:
    std::size_t chunk_count_{0};
    std::string chunk_text_{"chunk "};
    std::shared_ptr<std::chrono::nanoseconds> delta_loop_elapsed_{std::make_shared<std::chrono::nanoseconds>(0)};
};

/// Scripted provider whose first request returns a caller-provided assistant
/// message (the tool-use round) and later requests stream chunked text, so a
/// session-level test can drive a full tool round with streaming partials.
class ToolRoundProvider final : public ChunkedProjectionProvider {
public:
    using ChunkedProjectionProvider::ChunkedProjectionProvider;

    void set_tool_round(ai::AssistantMessage round) { tool_round_ = std::move(round); }

    [[nodiscard]] ai::ModelStream stream(ai::Model model,
            ai::AiContext context,
            coding_agent::ModelRuntimeTestStreamOptions stream_options) override {
        if (request_count_++ == 0) {
            auto round = tool_round_;
            return ai::detail::make_model_stream(
                    [model = std::move(model), round = std::move(round)](ai::AssistantEventSink sink) mutable
                            -> boost::asio::awaitable<support::Expected<ai::AssistantMessage>> {
                        round.provider = "projection-fake";
                        round.api = "fake";
                        round.model = model.id;
                        if (sink) {
                            if (auto emitted = sink(ai::AssistantStartEvent{.partial = round}); !emitted) {
                                co_return std::unexpected(emitted.error());
                            }
                        }
                        co_return round;
                    });
        }
        return ChunkedProjectionProvider::stream(std::move(model), std::move(context), std::move(stream_options));
    }

private:
    int request_count_{0};
    ai::AssistantMessage tool_round_{};
};

[[nodiscard]] support::Expected<coding_agent::CreateAgentSessionResult> create_projection_session(
        tests::RuntimeFixture& runtime,
        const tests::TempWorkspace& workspace,
        std::shared_ptr<tests::ScriptedProvider> provider,
        std::vector<agent::Tool> custom_tools = {}) {
    tests::ModelsSessionOptions options;
    options.session_target = coding_agent::InMemorySessionTarget{};
    options.workspace = workspace.path();
    options.request_model = tests::scripted_request_model("fake", "fake-model");
    options.custom_tools = std::move(custom_tools);
    auto models = tests::models_from_provider(std::move(provider));
    coding_agent::runtime::AgentSessionCreationRequest request = std::move(options);
    request.execution_runtime_target = runtime.make_target();
    return runtime.run(coding_agent::create_agent_session_async(std::move(request),
            std::nullopt,
            coding_agent::runtime::AssemblyOverrides{
                    .model_runtime = nullptr, .models = std::move(models), .user_shell = nullptr}));
}

/// A custom tool that streams cumulative partial results through the update
/// sink before completing — the session-level shape of a streaming tool
/// partial (the built-in tools never stream partials).
[[nodiscard]] agent::Tool make_streaming_tool(std::shared_ptr<std::size_t> partial_count, std::size_t partials = 3) {
    ai::Tool definition;
    definition.name = "streamy";
    definition.description = "Stream a few partial results";
    definition.parameters = support::JsonValue::object_t{{"type", "object"}, {"additionalProperties", false}};
    return tests::make_fake_tool(std::move(definition),
            agent::ToolConcurrency::Exclusive,
            [partial_count = std::move(partial_count), partials](
                    agent::ToolInvocation, std::stop_token, agent::ToolUpdateSink update_sink)
                    -> boost::asio::awaitable<support::Expected<agent::AsyncToolExecutionResult>> {
                std::string accumulated;
                for (std::size_t index = 1; index <= partials; ++index) {
                    accumulated += "partial " + std::to_string(index) + " " + std::string(128, 'x');
                    agent::AsyncToolExecutionResult partial;
                    partial.content.push_back(ai::text_content(accumulated));
                    if (partial_count) {
                        ++*partial_count;
                    }
                    if (update_sink) {
                        if (auto emitted = update_sink(partial); !emitted) {
                            co_return std::unexpected(emitted.error());
                        }
                    }
                }
                agent::AsyncToolExecutionResult result;
                result.content.push_back(ai::text_content("streamy: done"));
                co_return result;
            });
}

/// Compact, comparable rendering of one message value for the convergence
/// comparison (the value types carry no equality operators).
[[nodiscard]] std::string content_text(const std::vector<ai::Content>& blocks) {
    std::string text;
    for (const auto& block : blocks) {
        if (const auto* content = std::get_if<ai::TextContent>(&block)) {
            text += content->text;
        }
    }
    return text;
}

[[nodiscard]] std::string message_digest(const ai::MessageVariant& message) {
    if (const auto* user = std::get_if<ai::UserMessage>(&message)) {
        if (const auto* text_value = std::get_if<std::string>(&user->content)) {
            return "user:" + *text_value;
        }
        return "user:" + content_text(std::get<std::vector<ai::Content>>(user->content));
    }
    if (const auto* assistant = std::get_if<ai::AssistantMessage>(&message)) {
        std::string text;
        std::string calls;
        for (const auto& block : assistant->content) {
            if (const auto* content = std::get_if<ai::TextContent>(&block)) {
                text += content->text;
            } else if (const auto* call = std::get_if<ai::ToolCallContent>(&block)) {
                calls += std::string{"["} + call->name + "#" + call->id + "]";
            }
        }
        return "assistant:" + text + calls;
    }
    if (const auto* result = std::get_if<ai::ToolResultMessage>(&message)) {
        return "toolresult(" + result->tool_name + " err=" + (result->is_error ? "1" : "0") +
               "):" + content_text(result->content);
    }
    return "other:" + std::to_string(message.index());
}

/// One recorded stream message with its kind and version.
struct RecordedMessage {
    bool is_base{false};
    std::uint64_t version{0};
    /// Base snapshots and patch batches compose; the convergence test applies
    /// them in order. The full value is retained per message.
    ProjectionStreamMessageVariant value{};
};

/// Record every drained stream message for one subscriber.
[[nodiscard]] std::shared_ptr<std::vector<RecordedMessage>> make_recorder() {
    return std::make_shared<std::vector<RecordedMessage>>();
}

[[nodiscard]] bool is_base(const RecordedMessage& message) {
    return std::holds_alternative<ProjectionStreamBase>(message.value);
}

[[nodiscard]] std::uint64_t version_of(const RecordedMessage& message) {
    return std::visit([](const auto& value) { return value.version; }, message.value);
}

/// Compose the recorded stream for one subscriber: Base + patches in order.
/// `exclude_degenerate_patches` drops every SessionSnapshotPatch, so the
/// composition verifies the fine-grained slice patches alone (the final
/// degenerate whole-snapshot patch would otherwise mask intermediate
/// fine-grained mistakes).
[[nodiscard]] coding_agent::AgentSessionSnapshot compose(
        const std::vector<RecordedMessage>& records, bool exclude_degenerate_patches = false) {
    coding_agent::AgentSessionSnapshot composed{};
    for (const auto& record : records) {
        std::visit(
                [&](const auto& value) {
                    using T = std::decay_t<decltype(value)>;
                    if constexpr (std::is_same_v<T, ProjectionStreamBase>) {
                        composed = value.snapshot;
                    } else {
                        if (exclude_degenerate_patches) {
                            for (const auto& patch : value.patches) {
                                if (std::holds_alternative<coding_agent::SessionSnapshotPatch>(patch)) {
                                    continue;
                                }
                                coding_agent::apply_projection_patches(composed, {patch});
                            }
                        } else {
                            coding_agent::apply_projection_patches(composed, value.patches);
                        }
                    }
                },
                record.value);
    }
    return composed;
}

/// Convergence comparison (ADR 0052 invariant): composed == Core snapshot.
/// `compare_running` is false when the composed stream excluded the
/// degenerate whole-snapshot patches: the settled `is_running=false` reaches
/// the stream only through that degenerate patch, so a fine-grained-only
/// composition keeps the run's running flag.
void check_converged(const coding_agent::AgentSessionSnapshot& composed,
        const coding_agent::AgentSessionSnapshot& core,
        bool compare_running = true) {
    REQUIRE(composed.agent_state.messages.size() == core.agent_state.messages.size());
    for (std::size_t index = 0; index < core.agent_state.messages.size(); ++index) {
        INFO(std::string{"message "} + std::to_string(index));
        CHECK(message_digest(composed.agent_state.messages[index]) == message_digest(core.agent_state.messages[index]));
    }
    if (compare_running) {
        CHECK(composed.agent_state.is_running == core.agent_state.is_running);
    }
    CHECK(composed.agent_state.pending_tool_call_ids == core.agent_state.pending_tool_call_ids);
    CHECK(composed.agent_state.thinking_level == core.agent_state.thinking_level);
    CHECK(composed.agent_state.model.id == core.agent_state.model.id);
    CHECK(composed.agent_state.model.provider == core.agent_state.model.provider);
    CHECK(composed.agent_state.input_queues.steering.messages.size() ==
            core.agent_state.input_queues.steering.messages.size());
    CHECK(composed.agent_state.input_queues.follow_up.messages.size() ==
            core.agent_state.input_queues.follow_up.messages.size());
    CHECK(composed.agent_state.diagnostics.size() == core.agent_state.diagnostics.size());
    CHECK(composed.metadata.session_id == core.metadata.session_id);
    CHECK(composed.topology == core.topology);
    CHECK(composed.session_path == core.session_path);
    CHECK(composed.session_event_diagnostics.size() == core.session_event_diagnostics.size());
    REQUIRE(composed.tool_executions.size() == core.tool_executions.size());
    for (std::size_t index = 0; index < core.tool_executions.size(); ++index) {
        const auto& actual = composed.tool_executions[index];
        const auto& expected = core.tool_executions[index];
        CHECK(actual.tool_call_id == expected.tool_call_id);
        CHECK(actual.tool_name == expected.tool_name);
        CHECK(actual.arguments_json == expected.arguments_json);
        CHECK(actual.status == expected.status);
        CHECK(actual.output_tail == expected.output_tail);
        CHECK(actual.output_truncated == expected.output_truncated);
        CHECK(actual.artifact_reference == expected.artifact_reference);
        CHECK(actual.error == expected.error);
    }
    if (compare_running) {
        CHECK(composed.run_state.phase == core.run_state.phase);
        CHECK(composed.run_state.terminal == core.run_state.terminal);
        CHECK(composed.run_state.error == core.run_state.error);
        CHECK(composed.recovery_state.retry_count == core.recovery_state.retry_count);
        CHECK(composed.recovery_state.max_retry_count == core.recovery_state.max_retry_count);
        CHECK(composed.recovery_state.next_retry_at_ms == core.recovery_state.next_retry_at_ms);
        CHECK(composed.recovery_state.retry_error == core.recovery_state.retry_error);
        CHECK(composed.recovery_state.compaction == core.recovery_state.compaction);
        CHECK(composed.recovery_state.compaction_reason == core.recovery_state.compaction_reason);
        CHECK(composed.recovery_state.compaction_error == core.recovery_state.compaction_error);
    }
    // The streaming partial: presence and rendered text must agree.
    REQUIRE(composed.agent_state.streaming_message.has_value() == core.agent_state.streaming_message.has_value());
    if (core.agent_state.streaming_message) {
        CHECK(message_digest(ai::MessageVariant{*composed.agent_state.streaming_message}) ==
                message_digest(ai::MessageVariant{*core.agent_state.streaming_message}));
    }
}

} // namespace

TEST_CASE("Projection attach delivers a Base then ordered patches to concurrent subscribers",
        "[coding_agent][projection][issue617][spec]") {
    tests::TempWorkspace workspace;
    tests::RuntimeFixture runtime;
    auto provider = std::make_shared<ChunkedProjectionProvider>();
    provider->set_chunk_count(4);
    auto created = create_projection_session(runtime, workspace, provider);
    REQUIRE(created.has_value());
    auto& session = *created->session;

    // Two independent projections attach by calling attach alone; each
    // receives its own Base and its own mailbox.
    auto first_records = make_recorder();
    auto second_records = make_recorder();
    auto first = session.attach_projection([first_records](const ProjectionStreamMessageVariant& message) {
        first_records->push_back(RecordedMessage{
                .is_base = std::holds_alternative<ProjectionStreamBase>(message),
                .version = std::visit([](const auto& value) { return value.version; }, message),
                .value = message,
        });
    });
    auto second = session.attach_projection([second_records](const ProjectionStreamMessageVariant& message) {
        second_records->push_back(RecordedMessage{
                .is_base = std::holds_alternative<ProjectionStreamBase>(message),
                .version = std::visit([](const auto& value) { return value.version; }, message),
                .value = message,
        });
    });
    REQUIRE(first.drain() == 1);
    REQUIRE(second.drain() == 1);
    REQUIRE(first_records->size() == 1);
    REQUIRE(is_base(first_records->front()));
    REQUIRE(second_records->size() == 1);
    REQUIRE(is_base(second_records->front()));

    REQUIRE(tests::run_awaitable(runtime, session.prompt("projection attach")).has_value());

    // Both subscribers drain the same ordered patch batches: identical
    // sequences of strictly increasing versions after the Base (versions with
    // no published value — e.g. a user message's MessageStart — carry no
    // stream message).
    CHECK(first.drain() >= 4);
    CHECK(second.drain() >= 4);
    REQUIRE(first_records->size() == second_records->size());
    for (std::size_t index = 0; index < first_records->size(); ++index) {
        CHECK(first_records->at(index).is_base == second_records->at(index).is_base);
        CHECK(version_of(first_records->at(index)) == version_of(second_records->at(index)));
    }
    const std::uint64_t base_version = version_of(first_records->front());
    for (std::size_t index = 1; index < first_records->size(); ++index) {
        CHECK_FALSE(first_records->at(index).is_base);
        CHECK(version_of(first_records->at(index)) > version_of(first_records->at(index - 1)));
    }
    CHECK(version_of(first_records->back()) > base_version);

    // No subscriber's unsubscribe affects the other's delivery.
    first.unsubscribe();
    REQUIRE(tests::run_awaitable(runtime, session.prompt("second turn")).has_value());
    const auto second_before = second_records->size();
    CHECK(second.drain() >= 4);
    CHECK(first.drain() == 0);
    CHECK(second_records->size() > second_before);
    for (std::size_t index = second_before; index < second_records->size(); ++index) {
        CHECK_FALSE(second_records->at(index).is_base);
    }
    session.close();
}

TEST_CASE("Projection mailbox overflow resynchronizes with a fresh Base and never blocks the Core",
        "[coding_agent][projection][issue617][spec]") {
    tests::TempWorkspace workspace;
    tests::RuntimeFixture runtime;
    auto provider = std::make_shared<ChunkedProjectionProvider>();
    provider->set_chunk_count(100);
    provider->set_chunk_text("x");
    auto created = create_projection_session(runtime, workspace, provider);
    REQUIRE(created.has_value());
    auto& session = *created->session;

    auto records = make_recorder();
    auto slow = session.attach_projection([records](const ProjectionStreamMessageVariant& message) {
        records->push_back(RecordedMessage{
                .is_base = std::holds_alternative<ProjectionStreamBase>(message),
                .version = std::visit([](const auto& value) { return value.version; }, message),
                .value = message,
        });
    });
    REQUIRE(slow.drain() == 1);
    REQUIRE(is_base(records->front()));
    const std::uint64_t attach_version = version_of(records->front());

    // The subscriber never drains while the Core publishes more than a full
    // mailbox of value-bearing batches (100 streaming chunks). Overflow must
    // discard the backlog, resynchronize with a fresh Base, and never error
    // or block the Core's serialized domain.
    REQUIRE(tests::run_awaitable(runtime, session.prompt("stream one hundred chunks")).has_value());

    CHECK(slow.drain() >= 1);
    CHECK(slow.drain() == 0);
    // The mailbox is the only retention: the drained stream is the attach-
    // time Base, then (the backlog having been discarded) at least one fresh
    // resync Base — a version newer than every publication that was dropped —
    // followed by strictly newer ordered patches. Bounded throughout.
    REQUIRE(records->size() <= coding_agent::kProjectionMailboxCapacity + 2);
    REQUIRE(is_base(records->front()));
    CHECK(version_of(records->front()) == attach_version);
    std::size_t resyncs = 0;
    for (std::size_t index = 1; index < records->size(); ++index) {
        if (is_base(records->at(index))) {
            ++resyncs;
        } else {
            CHECK(version_of(records->at(index)) > version_of(records->at(index - 1)));
        }
        CHECK(version_of(records->at(index)) > version_of(records->at(index - 1)));
    }
    CHECK(resyncs >= 1);

    // Resynchronization is indistinguishable from attaching: the composed
    // stream converges with the Core snapshot (ADR 0052 invariant).
    check_converged(compose(*records), session.snapshot());
    session.close();
}

TEST_CASE("Projection patches converge with the Core snapshot for a message-chunk burst",
        "[coding_agent][projection][issue617][spec]") {
    tests::TempWorkspace workspace;
    tests::RuntimeFixture runtime;
    auto provider = std::make_shared<ChunkedProjectionProvider>();
    provider->set_chunk_count(10);
    auto created = create_projection_session(runtime, workspace, provider);
    REQUIRE(created.has_value());
    auto& session = *created->session;

    auto records = make_recorder();
    auto subscription = session.attach_projection([records](const ProjectionStreamMessageVariant& message) {
        records->push_back(RecordedMessage{
                .is_base = std::holds_alternative<ProjectionStreamBase>(message),
                .version = std::visit([](const auto& value) { return value.version; }, message),
                .value = message,
        });
    });
    REQUIRE(subscription.drain() == 1);

    REQUIRE(tests::run_awaitable(runtime, session.prompt("streaming burst")).has_value());
    CHECK(subscription.drain() >= 10);

    // Full convergence (including the degenerate whole-snapshot patches).
    check_converged(compose(*records), session.snapshot());
    // Fine-grained convergence: the same records with every degenerate
    // SessionSnapshotPatch excluded must still compose to the Core snapshot
    // for every slice the slice patches own — intermediate fine-grained
    // mistakes cannot hide behind the final degenerate patch. The settled
    // running flag reaches the stream only through the degenerate patch, so
    // it is excluded from that comparison.
    check_converged(compose(*records, /*exclude_degenerate_patches=*/true),
            session.snapshot(),
            /*compare_running=*/false);
    session.close();
}

TEST_CASE("Projection patches converge with the Core snapshot for a streaming tool partial",
        "[coding_agent][projection][issue617][diverge][issue622]") {
    tests::TempWorkspace workspace;
    tests::RuntimeFixture runtime;
    auto partial_count = std::make_shared<std::size_t>(0);

    // The provider's first response carries a tool call; the streaming tool
    // emits cumulative partials through its update sink before completing.
    auto tool_use = ai::assistant_text_message("Streaming a tool now.");
    tool_use.stop_reason = ai::AssistantStopReason::ToolUse;
    tool_use.content.emplace_back(ai::ToolCallContent{
            .id = "call_1",
            .name = "streamy",
            .arguments = support::JsonValue{support::JsonValue::object_t{}},
            .raw_arguments = {},
            .thought_signature = std::nullopt,
            .arguments_valid = true,
            .argument_error = std::nullopt,
    });

    auto round_provider = std::make_shared<ToolRoundProvider>();
    round_provider->set_tool_round(std::move(tool_use));
    round_provider->set_chunk_count(2);

    tests::ModelsSessionOptions options;
    options.session_target = coding_agent::InMemorySessionTarget{};
    options.workspace = workspace.path();
    options.request_model = tests::scripted_request_model("fake", "fake-model");
    std::vector<agent::Tool> tools;
    tools.push_back(make_streaming_tool(partial_count));
    options.custom_tools = std::move(tools);
    auto models = tests::models_from_provider(std::move(round_provider));
    coding_agent::runtime::AgentSessionCreationRequest request = std::move(options);
    request.execution_runtime_target = runtime.make_target();
    auto created = runtime.run(coding_agent::create_agent_session_async(std::move(request),
            std::nullopt,
            coding_agent::runtime::AssemblyOverrides{
                    .model_runtime = nullptr, .models = std::move(models), .user_shell = nullptr}));
    REQUIRE(created.has_value());
    auto& session = *created->session;

    auto records = make_recorder();
    auto subscription = session.attach_projection([records](const ProjectionStreamMessageVariant& message) {
        records->push_back(RecordedMessage{
                .is_base = std::holds_alternative<ProjectionStreamBase>(message),
                .version = std::visit([](const auto& value) { return value.version; }, message),
                .value = message,
        });
    });
    REQUIRE(subscription.drain() == 1);

    REQUIRE(tests::run_awaitable(runtime, session.prompt("run the streaming tool")).has_value());
    CHECK(*partial_count == 3);
    CHECK(subscription.drain() >= 1);

    // The push-only tool facts rode the one stream: start, three partials,
    // and the finish are all present as presentation patches.
    std::size_t started = 0;
    std::size_t partials = 0;
    std::size_t finished = 0;
    for (const auto& record : *records) {
        if (const auto* batch = std::get_if<ProjectionStreamPatchMsg>(&record.value)) {
            for (const auto& patch : batch->patches) {
                if (std::holds_alternative<coding_agent::ToolStartedPatch>(patch)) {
                    ++started;
                } else if (std::holds_alternative<coding_agent::ToolPartialPatch>(patch)) {
                    ++partials;
                } else if (std::holds_alternative<coding_agent::ToolFinishedPatch>(patch)) {
                    ++finished;
                }
            }
        }
    }
    CHECK(started == 1);
    CHECK(partials == 3);
    CHECK(finished == 1);

    // Full convergence (including the degenerate whole-snapshot patches).
    check_converged(compose(*records), session.snapshot());
    // Fine-grained convergence: the same records with every degenerate
    // SessionSnapshotPatch excluded must still compose to the Core snapshot
    // for every slice the slice patches own — intermediate fine-grained
    // mistakes cannot hide behind the final degenerate patch. The settled
    // running flag reaches the stream only through the degenerate patch, so
    // it is excluded from that comparison.
    check_converged(compose(*records, /*exclude_degenerate_patches=*/true),
            session.snapshot(),
            /*compare_running=*/false);
    session.close();
}

TEST_CASE("Projection observers converge on tool and run state after a stalled mailbox resync",
        "[coding_agent][projection][issue622][spec]") {
    tests::TempWorkspace workspace;
    tests::RuntimeFixture runtime;
    auto partial_count = std::make_shared<std::size_t>(0);

    auto tool_use = ai::assistant_text_message("Streaming a long tool run.");
    tool_use.stop_reason = ai::AssistantStopReason::ToolUse;
    tool_use.content.emplace_back(ai::ToolCallContent{
            .id = "call_622",
            .name = "streamy",
            .arguments = support::JsonValue{support::JsonValue::object_t{}},
            .raw_arguments = "{}",
            .thought_signature = std::nullopt,
            .arguments_valid = true,
            .argument_error = std::nullopt,
    });

    auto provider = std::make_shared<ToolRoundProvider>();
    provider->set_tool_round(std::move(tool_use));
    provider->set_chunk_count(1);

    tests::ModelsSessionOptions options;
    options.session_target = coding_agent::InMemorySessionTarget{};
    options.workspace = workspace.path();
    options.request_model = tests::scripted_request_model("fake", "fake-model");
    std::vector<agent::Tool> tools;
    tools.push_back(make_streaming_tool(partial_count, 100));
    options.custom_tools = std::move(tools);
    auto models = tests::models_from_provider(std::move(provider));
    coding_agent::runtime::AgentSessionCreationRequest request = std::move(options);
    request.execution_runtime_target = runtime.make_target();
    auto created = runtime.run(coding_agent::create_agent_session_async(std::move(request),
            std::nullopt,
            coding_agent::runtime::AssemblyOverrides{
                    .model_runtime = nullptr, .models = std::move(models), .user_shell = nullptr}));
    REQUIRE(created.has_value());
    auto& session = *created->session;

    auto stalled_records = make_recorder();
    auto stalled = session.attach_projection([stalled_records](const ProjectionStreamMessageVariant& message) {
        stalled_records->push_back(RecordedMessage{
                .is_base = std::holds_alternative<ProjectionStreamBase>(message),
                .version = std::visit([](const auto& value) { return value.version; }, message),
                .value = message,
        });
    });
    REQUIRE(stalled.drain() == 1);

    // Do not drain the first observer while more than one mailbox capacity of
    // tool updates is published. It must recover through a fresh Base rather
    // than relying on the old tool events being replayed.
    REQUIRE(tests::run_awaitable(runtime, session.prompt("run a long tool")).has_value());
    REQUIRE(*partial_count == 100);

    auto late_records = make_recorder();
    auto late = session.attach_projection([late_records](const ProjectionStreamMessageVariant& message) {
        late_records->push_back(RecordedMessage{
                .is_base = std::holds_alternative<ProjectionStreamBase>(message),
                .version = std::visit([](const auto& value) { return value.version; }, message),
                .value = message,
        });
    });
    REQUIRE(late.drain() == 1);
    REQUIRE(stalled.drain() >= 1);
    REQUIRE(stalled.drain() == 0);

    const auto core = session.snapshot();
    const auto stalled_composed = compose(*stalled_records);
    const auto late_composed = compose(*late_records);
    check_converged(stalled_composed, core);
    check_converged(late_composed, core);
    REQUIRE(stalled_composed.tool_executions.size() == 1);
    REQUIRE(late_composed.tool_executions.size() == 1);
    const auto& tool = stalled_composed.tool_executions.front();
    CHECK(tool.status == coding_agent::ToolExecutionStatus::Succeeded);
    CHECK(tool.output_truncated);
    CHECK(tool.output_tail.size() <= coding_agent::kProjectionToolOutputTailBytes);
    CHECK(tool.output_tail == late_composed.tool_executions.front().output_tail);
    CHECK(stalled_composed.run_state.phase == coding_agent::RunPhase::Idle);
    CHECK(stalled_composed.run_state.terminal == coding_agent::RunTerminalState::Succeeded);
    CHECK(stalled_composed.recovery_state.retry_count == late_composed.recovery_state.retry_count);
    CHECK(stalled_composed.recovery_state.compaction == late_composed.recovery_state.compaction);
    session.close();
}

TEST_CASE("Projection attach after Session Close delivers the terminal snapshot as its Base",
        "[coding_agent][projection][issue617][spec]") {
    tests::TempWorkspace workspace;
    tests::RuntimeFixture runtime;
    auto provider = std::make_shared<ChunkedProjectionProvider>();
    provider->set_chunk_count(1);
    auto created = create_projection_session(runtime, workspace, provider);
    REQUIRE(created.has_value());
    auto& session = *created->session;

    REQUIRE(tests::run_awaitable(runtime, session.prompt("closing turn")).has_value());
    session.close();

    auto records = make_recorder();
    auto subscription = session.attach_projection([records](const ProjectionStreamMessageVariant& message) {
        records->push_back(RecordedMessage{
                .is_base = std::holds_alternative<ProjectionStreamBase>(message),
                .version = std::visit([](const auto& value) { return value.version; }, message),
                .value = message,
        });
    });
    REQUIRE(subscription.drain() == 1);
    REQUIRE(records->size() == 1);
    const auto& base = std::get<ProjectionStreamBase>(records->front().value);
    REQUIRE(base.snapshot.agent_state.messages.size() == 2);
    REQUIRE(std::holds_alternative<ai::AssistantMessage>(base.snapshot.agent_state.messages[1]));
    const auto& content = std::get<ai::AssistantMessage>(base.snapshot.agent_state.messages[1]).content;
    REQUIRE(content.size() == 1);
    REQUIRE(std::holds_alternative<ai::TextContent>(content[0]));
    CHECK(std::get<ai::TextContent>(content[0]).text == "chunk ");
}

// Quarantine (issue #634 mechanism, owner: #632): this case enforces the
// #617/ADR 0052 100us cost contract in Release (NDEBUG), but shared CI
// runners measure over it, so the GCC 16 Release lane excludes the
// `quarantine` label (see .github/workflows/linux-toolchain.yml) instead of
// re-tuning the bound. The Debug 2ms bound still runs on every other lane.
// Re-enable on the Release lane when dedicated-runner measurements clear
// 100us with headroom.
TEST_CASE("Projection publishes 100 message-update chunks inside the issue cost bound over a long history",
        "[coding_agent][projection][issue617][quarantine][spec]") {
    tests::TempWorkspace workspace;
    tests::RuntimeFixture runtime;
    auto provider = std::make_shared<ChunkedProjectionProvider>();
    auto created = create_projection_session(runtime, workspace, provider);
    REQUIRE(created.has_value());
    auto& session = *created->session;

    // Seed a long history so that a per-event deep copy of the complete
    // session state would be measurably expensive.
    for (int turn = 0; turn < 50; ++turn) {
        REQUIRE(tests::run_awaitable(runtime, session.prompt("seed turn")).has_value());
    }
    REQUIRE(session.message_count() == 100);

    auto records = make_recorder();
    auto subscription = session.attach_projection([records](const ProjectionStreamMessageVariant& message) {
        records->push_back(RecordedMessage{
                .is_base = std::holds_alternative<ProjectionStreamBase>(message),
                .version = std::visit([](const auto& value) { return value.version; }, message),
                .value = message,
        });
    });
    (void)subscription.drain();

    provider->set_chunk_count(100);
    provider->set_chunk_text("x");
    // Wall-clock single samples jitter on shared CI runners (the Release lane measured
    // 229us against the 100us contract below); repeat the streaming pass and assert on the
    // minimum. An algorithmic regression (per-event full-history materialization) slows
    // every repetition, so the minimum still enforces the contract.
    auto elapsed = std::chrono::nanoseconds::max();
    for (int pass = 0; pass < 5; ++pass) {
        REQUIRE(tests::run_awaitable(runtime, session.prompt("stream one hundred chunks")).has_value());
        elapsed = std::min(elapsed, provider->delta_loop_elapsed());
    }

    // Issue #600 cost contract carried into ADR 0052: 100 MessageUpdateEvent
    // chunks ingest without materializing the complete history per event.
    // Publication builds value-bearing slices only (the streaming partial and
    // bounded bookkeeping); full snapshot copies stay confined to attach,
    // resync, and the rare degenerate coarse patch, so the deltas-loop
    // contract still holds.
    INFO(std::string{"100-chunk projection publication loop: "} + std::to_string(elapsed.count()) + "ns");
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
    // Sanitizers slow this loop systematically on CI (~55x under ASan: 5.6ms against
    // the 2ms Debug bound), so wall-clock bounds are meaningless under instrumentation.
    // The functional assertions above still execute on sanitizer lanes; only the timing
    // contract is scoped out (CODING_STANDARDS.md section 11.9).
#elif defined(NDEBUG)
    CHECK(elapsed < std::chrono::microseconds{100});
#else
    // The supported Debug preset intentionally keeps assertions and disables
    // optimization; retain a generous sanity bound there and enforce the
    // issue's 0.1ms contract in Release.
    CHECK(elapsed < std::chrono::milliseconds{2});
#endif
    session.close();
}
