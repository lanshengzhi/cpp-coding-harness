#include "ai/ModelStreamBridge.hpp"
#include "coding_agent/AgentSession.hpp"
#include "coding_agent/tui/InteractiveEngine.hpp"
#include "coding_agent/tui/InteractiveSessionRun.hpp"
#include "support/ModelsFixture.hpp"
#include "support/RuntimeFixture.hpp"
#include "support/TempWorkspace.hpp"

#include <cch/ai/Content.hpp>
#include <cch/tui/VirtualTerminal.hpp>
#include <catch2/catch_test_macros.hpp>

#include <boost/asio/awaitable.hpp>

#include <memory>
#include <string>
#include <utility>

using namespace cch;

namespace {

class ChunkedProvider final : public tests::ScriptedProvider {
public:
    ChunkedProvider() : ScriptedProvider("fake") {}

    [[nodiscard]] ai::ModelStream stream(
            ai::Model model, ai::AiContext, coding_agent::ModelRuntimeTestStreamOptions) override {
        return ai::detail::make_model_stream(
                [model = std::move(model)](ai::AssistantEventSink sink)
                        -> boost::asio::awaitable<support::Expected<ai::AssistantMessage>> {
                    auto partial = ai::assistant_text_message("");
                    partial.provider = "fake";
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
                        for (int index = 0; index < 50; ++index) {
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
                    if (auto emitted = sink(ai::AssistantDoneEvent{
                                .reason = partial.stop_reason,
                                .message = partial,
                        });
                            !emitted) {
                        co_return std::unexpected(emitted.error());
                    }
                    co_return partial;
                });
    }
};

[[nodiscard]] support::Expected<std::unique_ptr<coding_agent::AgentSession>> create_session(
        tests::RuntimeFixture& runtime, const tests::TempWorkspace& workspace) {
    auto provider = std::make_shared<ChunkedProvider>();
    tests::ModelsSessionOptions options;
    options.session_target = coding_agent::InMemorySessionTarget{};
    options.workspace = workspace.path();
    options.request_model = tests::scripted_request_model("fake", "fake-model");
    auto models = tests::models_from_provider(std::move(provider));
    coding_agent::runtime::AgentSessionCreationRequest request = std::move(options);
    request.execution_runtime_target = runtime.make_target();
    auto created = runtime.run(coding_agent::create_agent_session_async(std::move(request),
            std::nullopt,
            coding_agent::runtime::AssemblyOverrides{
                    .model_runtime = nullptr,
                    .models = std::move(models),
                    .user_shell = nullptr,
            }));
    if (!created) return std::unexpected(created.error());
    return std::move(created->session);
}

[[nodiscard]] std::string visible_screen(const tui::VirtualTerminal& terminal) {
    std::string text;
    for (const auto& line : terminal.screen()) {
        text += line;
        text.push_back('\n');
    }
    return text;
}

} // namespace

TEST_CASE("Interactive frame ticker coalesces a burst into one complete snapshot render",
        "[coding_agent][tui][frame-ticker][issue601][spec]") {
    tests::TempWorkspace workspace;
    tests::RuntimeFixture runtime;
    auto created = create_session(runtime, workspace);
    REQUIRE(created);

    tui::VirtualTerminal terminal{{.columns = 120, .rows = 40}};
    auto engine = std::make_shared<coding_agent::tui::InteractiveEngine>(terminal, runtime.make_target()->executor());
    auto started = engine->start(coding_agent::tui::InteractiveSessionRunBuilder{}
                    .with_session(**created)
                    .with_agent_config_directory(workspace.path())
                    .build());
    REQUIRE(started);

    // Cancelling the arm keeps this deterministic while the test invokes the
    // public tick directly.
    (void)engine->frame_ticker().cancel();
    engine->on_frame_tick();
    CHECK(engine->render_count() == 0);
    (void)engine->frame_ticker().cancel();

    auto prompted = tests::run_awaitable(runtime, (*created)->prompt("coalesced prompt"));
    const auto prompt_error = prompted ? std::string{} : prompted.error().message;
    INFO(prompt_error);
    REQUIRE(prompted.has_value());
    (void)engine->frame_ticker().cancel();

    engine->on_frame_tick();
    CHECK(engine->render_count() == 1);
    CHECK(visible_screen(terminal).find("chunk chunk") != std::string::npos);
}
