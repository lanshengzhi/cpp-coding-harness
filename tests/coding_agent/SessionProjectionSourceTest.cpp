#include "ai/ModelStreamBridge.hpp"
#include "support/ModelsFixture.hpp"
#include "support/RuntimeFixture.hpp"
#include "support/TempWorkspace.hpp"

#include "coding_agent/AgentSession.hpp"
#include "coding_agent/runtime/SessionFactory.hpp"
#include <cch/ai/Content.hpp>
#include <cch/ai/Message.hpp>
#include <cch/coding_agent/SessionProjectionSource.hpp>

#include <catch2/catch_test_macros.hpp>
#include <boost/asio/awaitable.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace cch;
using tests::run_awaitable;

namespace {

/// Scripted provider whose stream emits AssistantStart + TextStart followed by
/// a caller-chosen number of TextDelta chunks, then completes. The agent
/// reducer emits one MessageUpdateEvent per chunk, so `chunk_count` drives the
/// exact number of projection updates a prompt produces.
class ChunkedProjectionProvider final : public tests::ScriptedProvider {
public:
    ChunkedProjectionProvider() : ScriptedProvider("fake") {}

    void set_chunk_count(int count) { chunk_count_ = count; }

    [[nodiscard]] ai::ModelStream stream(
            ai::Model model, ai::AiContext, coding_agent::ModelRuntimeTestStreamOptions) override {
        const int chunk_count = chunk_count_;
        return ai::detail::make_model_stream(
                [model = std::move(model), chunk_count](ai::AssistantEventSink sink) mutable
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
                        for (int index = 0; index < chunk_count; ++index) {
                            std::get<ai::TextContent>(partial.content[0]).text += "chunk ";
                            if (auto emitted = sink(ai::TextDeltaEvent{
                                        .content_index = 0,
                                        .delta = "chunk ",
                                        .partial = partial,
                                });
                                    !emitted) {
                                co_return std::unexpected(emitted.error());
                            }
                        }
                    }
                    partial.stop_reason = ai::AssistantStopReason::Stop;
                    co_return partial;
                });
    }

private:
    int chunk_count_{0};
};

[[nodiscard]] support::Expected<coding_agent::CreateAgentSessionResult> create_projection_session(
        tests::RuntimeFixture& runtime,
        const tests::TempWorkspace& workspace,
        std::shared_ptr<tests::ScriptedProvider> provider) {
    tests::ModelsSessionOptions options;
    options.session_target = coding_agent::InMemorySessionTarget{};
    options.workspace = workspace.path();
    options.request_model = tests::scripted_request_model("fake", "fake-model");
    auto models = tests::models_from_provider(std::move(provider));
    coding_agent::runtime::AgentSessionCreationRequest request = std::move(options);
    request.execution_runtime_target = runtime.make_target();
    return runtime.run(coding_agent::create_agent_session_async(std::move(request),
            std::nullopt,
            coding_agent::runtime::AssemblyOverrides{
                    .model_runtime = nullptr, .models = std::move(models), .user_shell = nullptr}));
}

} // namespace

TEST_CASE("Core projection increments the version and notifies the dirty listener once per agent event",
        "[coding_agent][projection][issue600]") {
    tests::TempWorkspace workspace;
    tests::RuntimeFixture runtime;
    auto provider = std::make_shared<ChunkedProjectionProvider>();
    provider->set_chunk_count(4);
    auto created = create_projection_session(runtime, workspace, provider);
    REQUIRE(created.has_value());
    auto& source = created->session->projection_source();
    std::vector<std::uint64_t> notified_versions;

    // The listener is detached with the session; the prompt and all callbacks
    // complete before this test's local version vector is destroyed.
    source.set_dirty_listener([source_ptr = &source, notified = &notified_versions] {
        // Reading the version from inside the notification proves the callback
        // runs with no Core lock held (zero-mutex, non-blocking reads).
        notified->push_back(source_ptr->state_version());
    });

    const auto version_before = source.state_version();
    REQUIRE(run_awaitable(runtime, created->session->prompt("projection versioning")).has_value());

    // At least one update per streamed chunk; every notification carries the
    // next consecutive version and no version is published without one.
    CHECK(notified_versions.size() >= 4);
    CHECK(source.state_version() - version_before == notified_versions.size());
    for (std::size_t index = 0; index < notified_versions.size(); ++index) {
        CHECK(notified_versions[index] == version_before + 1 + index);
    }
    created->session->close();
}

TEST_CASE("Core projection serves one immutable snapshot instance per version and freezes earlier versions",
        "[coding_agent][projection][issue600]") {
    tests::TempWorkspace workspace;
    tests::RuntimeFixture runtime;
    auto provider = std::make_shared<ChunkedProjectionProvider>();
    provider->set_chunk_count(1);
    auto created = create_projection_session(runtime, workspace, provider);
    REQUIRE(created.has_value());
    auto& source = created->session->projection_source();

    REQUIRE(run_awaitable(runtime, created->session->prompt("first turn")).has_value());
    const auto first = source.snapshot();
    REQUIRE(first);
    CHECK(first->agent_state.messages.size() == 2);

    // Repeat sampling at one version is the zero-mutex fast path: the same
    // immutable instance, no re-materialization.
    const auto repeat = source.snapshot();
    CHECK(repeat.get() == first.get());

    REQUIRE(run_awaitable(runtime, created->session->prompt("second turn")).has_value());
    const auto second = source.snapshot();
    REQUIRE(second);
    CHECK(second.get() != first.get());
    CHECK(second->agent_state.messages.size() == 4);

    // The earlier publication is frozen: later Core work cannot reach into it.
    CHECK(first->agent_state.messages.size() == 2);
    created->session->close();
}

TEST_CASE("Core projection ingests 100 message-update chunks inside the issue cost bound over a long history",
        "[coding_agent][projection][issue600]") {
    tests::TempWorkspace workspace;
    tests::RuntimeFixture runtime;
    auto provider = std::make_shared<ChunkedProjectionProvider>();
    auto created = create_projection_session(runtime, workspace, provider);
    REQUIRE(created.has_value());
    auto& source = created->session->projection_source();

    // Seed a long history so that a per-event deep copy of the complete
    // session state is measurably expensive.
    for (int turn = 0; turn < 50; ++turn) {
        REQUIRE(run_awaitable(runtime, created->session->prompt("seed turn")).has_value());
    }
    REQUIRE(created->session->message_count() == 100);

    std::size_t notifications{0};
    source.set_dirty_listener([recorded = &notifications] { ++*recorded; });

    provider->set_chunk_count(100);
    REQUIRE(run_awaitable(runtime, created->session->prompt("stream one hundred chunks")).has_value());
    // Issue #600 cost contract: ingestion publishes a version and dirty edge
    // per event without materializing the complete history. The dirty listener
    // is the only synchronous callback and publication is O(1) per update.
    CHECK(notifications >= 100);
    created->session->close();
}

TEST_CASE("Core projection keeps answering the final snapshot after session close",
        "[coding_agent][projection][issue600]") {
    tests::TempWorkspace workspace;
    tests::RuntimeFixture runtime;
    auto provider = std::make_shared<ChunkedProjectionProvider>();
    provider->set_chunk_count(1);
    auto created = create_projection_session(runtime, workspace, provider);
    REQUIRE(created.has_value());

    REQUIRE(run_awaitable(runtime, created->session->prompt("closing turn")).has_value());
    created->session->close();

    // Post-close introspection keeps answering even though Close released the
    // live Agent: the final publication is the immutable terminal state.
    const auto snapshot = created->session->projection_snapshot();
    REQUIRE(snapshot);
    REQUIRE(snapshot->agent_state.messages.size() == 2);
    REQUIRE(std::holds_alternative<ai::AssistantMessage>(snapshot->agent_state.messages[1]));
    const auto& content = std::get<ai::AssistantMessage>(snapshot->agent_state.messages[1]).content;
    REQUIRE(content.size() == 1);
    REQUIRE(std::holds_alternative<ai::TextContent>(content[0]));
    CHECK(std::get<ai::TextContent>(content[0]).text == "chunk ");
}
