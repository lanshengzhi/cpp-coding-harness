#pragma once

#include <cch/ai/Content.hpp>
#include <cch/coding_agent/ModelRuntime.hpp>
#include <cch/support/AsyncResult.hpp>
#include "ai/ModelStreamBridge.hpp"
#include "coding_agent/ModelRuntimeTestSupport.hpp"
#include "support/EnvVarGuard.hpp"
#include "support/ExpectedMacros.hpp"
#include "support/ModelsFixture.hpp"
#include "support/ReleaseGate.hpp"
#include "support/StreamAdapterFixture.hpp"

#include <boost/asio/awaitable.hpp>

#include <cstddef>
#include <deque>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::tests {

[[nodiscard]] inline ai::ProviderAuth unconfigured_fixture_auth() {
    ai::ApiKeyAuth api_key;
    api_key.name = "unconfigured scripted provider";
    api_key.check = [](const ai::AuthContext&,
                            std::optional<ai::ApiKeyCredential>) -> support::AsyncResult<std::optional<ai::AuthCheck>> {
        return support::AsyncResult<std::optional<ai::AuthCheck>>(
                std::expected<std::optional<ai::AuthCheck>, support::Error>{std::optional<ai::AuthCheck>{}});
    };
    api_key.resolve =
            [](const ai::AuthContext&,
                    std::optional<ai::ApiKeyCredential>) -> support::AsyncResult<std::optional<ai::AuthResult>> {
        return support::AsyncResult<std::optional<ai::AuthResult>>(
                std::expected<std::optional<ai::AuthResult>, support::Error>{std::optional<ai::AuthResult>{}});
    };
    return ai::ProviderAuth{.api_key = std::move(api_key)};
}

/// One recorded request at the concrete Provider seam.
struct RecordedRuntimeCall {
    ai::Model model;
    ai::AiContext context;
    coding_agent::ModelRuntimeTestStreamOptions options;
};

/// Mutable controls shared by the scripted Provider Definitions installed in
/// one concrete ModelRuntime. The state is separate from the runtime so tests
/// can vary provider responses without manufacturing a ModelRuntime subtype.
struct ScriptedProviderControl final {
    /// Release the gated call, if any. Idempotent.
    void release() { gate_.release(); }

    /// Call index that gates (blocks until release or stop). Absent disables
    /// gating.
    std::optional<std::size_t> gate_at{std::nullopt};
    /// Emit a "streaming in flight" partial before the gated call blocks.
    bool emit_partial_before_gate{false};
    /// Response served after the gate releases when `responses` is empty.
    std::string gated_response{"gated done"};
    /// Recorded per-turn calls in issue order.
    std::vector<RecordedRuntimeCall> calls;
    /// Scripted assistant responses consumed in FIFO order.
    std::deque<ai::AssistantMessage> responses;

    /// Wake a gated stream for stop-token cancellation without recording a
    /// release permit.
    void interrupt() { gate_.interrupt(); }

    /// Wait for one release permit or an interrupt.
    [[nodiscard]] boost::asio::awaitable<void> wait() { co_await gate_.wait(); }

private:
    tests::ReleaseGate gate_;
};

/// A concrete scripted Provider used by the runtime fixture. Its catalog
/// contains the real `fake/fake-model` or `sdk-host/fake-model` identity, and
/// its stream is controlled by the shared FIFO/gating state.
class ScriptedRuntimeProvider final : public ScriptedProvider {
public:
    ScriptedRuntimeProvider(std::string provider_id, std::shared_ptr<ScriptedProviderControl> control, bool configured)
        : ScriptedProvider(std::move(provider_id), configured ? detail::fixture_auth() : unconfigured_fixture_auth()),
          control_(std::move(control)) {}

    [[nodiscard]] std::string_view name() const noexcept override { return provider_id(); }
    [[nodiscard]] std::vector<ai::Model> models() const override {
        ai::Model model;
        model.id = "fake-model";
        model.name = "fake-model";
        model.api = "scripted-fake";
        model.provider = provider_id();
        model.base_url = "";
        model.reasoning = false;
        model.thinking_level_map = std::nullopt;
        model.input = {ai::ModelInput::Text};
        model.cost = {};
        model.context_window = 128000;
        model.max_tokens = 16384;
        model.headers = std::nullopt;
        model.compat = std::nullopt;
        return {std::move(model)};
    }

    [[nodiscard]] ai::ModelStream stream(
            ai::Model model, ai::AiContext context, coding_agent::ModelRuntimeTestStreamOptions options) override {
        return ai::detail::make_model_stream(
                [control = control_,
                        model = std::move(model),
                        context = std::move(context),
                        options = std::move(options)](ai::AssistantEventSink sink)
                        -> boost::asio::awaitable<support::Expected<ai::AssistantMessage>> {
                    const std::size_t call_index = control->calls.size();
                    control->calls.push_back(RecordedRuntimeCall{
                            .model = std::move(model),
                            .context = std::move(context),
                            .options = std::move(options),
                    });
                    const auto& stop_token = control->calls.back().options.stop_token;

                    if (stop_token.stop_requested()) {
                        auto terminal = ai::assistant_text_message("");
                        terminal.stop_reason = ai::AssistantStopReason::Aborted;
                        terminal.error_message = "Request was aborted";
                        if (sink) {
                            CCH_TRY_VOID(sink(ai::AssistantErrorEvent{
                                    .reason = terminal.stop_reason,
                                    .error = terminal,
                                    .failure =
                                            support::make_error(support::ErrorCode::Cancelled, "Request was aborted"),
                            }));
                        }
                        co_return terminal;
                    }

                    if (control->emit_partial_before_gate && control->gate_at && call_index == *control->gate_at) {
                        auto partial = ai::assistant_text_message("streaming in flight");
                        if (sink) {
                            CCH_TRY_VOID(sink(ai::AssistantStartEvent{.partial = partial}));
                            CCH_TRY_VOID(sink(ai::TextDeltaEvent{
                                    .content_index = 0,
                                    .delta = "streaming in flight",
                                    .partial = partial,
                            }));
                        }
                    }

                    if (control->gate_at && call_index == *control->gate_at) {
                        // Hold the gated call; interrupt also wakes on the run's
                        // stop token. The stop check must precede wait(): an
                        // interrupt delivered before the gate is armed does not
                        // linger (ReleaseGate contract).
                        std::stop_callback release_on_stop{stop_token, [control] { control->interrupt(); }};
                        if (!stop_token.stop_requested()) {
                            co_await control->wait();
                        }
                    }

                    if (stop_token.stop_requested()) {
                        auto terminal = ai::assistant_text_message("");
                        terminal.stop_reason = ai::AssistantStopReason::Aborted;
                        terminal.error_message = "Request was aborted";
                        if (sink) {
                            CCH_TRY_VOID(sink(ai::AssistantErrorEvent{
                                    .reason = terminal.stop_reason,
                                    .error = terminal,
                                    .failure =
                                            support::make_error(support::ErrorCode::Cancelled, "Request was aborted"),
                            }));
                        }
                        co_return terminal;
                    }

                    if (control->responses.empty()) {
                        co_return ai::assistant_text_message(control->gated_response);
                    }
                    auto response = std::move(control->responses.front());
                    control->responses.pop_front();
                    if (sink) {
                        if (response.stop_reason == ai::AssistantStopReason::Error ||
                                response.stop_reason == ai::AssistantStopReason::Aborted) {
                            std::optional<support::Error> terminal_failure = std::nullopt;
                            if (response.error_message) {
                                terminal_failure =
                                        support::make_error(support::ErrorCode::Stream, *response.error_message);
                            }
                            CCH_TRY_VOID(sink(ai::AssistantErrorEvent{
                                    .reason = response.stop_reason,
                                    .error = response,
                                    .failure = std::move(terminal_failure),
                            }));
                            co_return response;
                        }
                        CCH_TRY_VOID(sink(ai::AssistantStartEvent{.partial = response}));
                        for (std::size_t index = 0; index < response.content.size(); ++index) {
                            const auto& block = response.content[index];
                            if (const auto* text = std::get_if<ai::TextContent>(&block)) {
                                CCH_TRY_VOID(sink(ai::TextDeltaEvent{
                                        .content_index = index,
                                        .delta = text->text,
                                        .partial = response,
                                }));
                            } else if (const auto* thinking = std::get_if<ai::ThinkingContent>(&block)) {
                                CCH_TRY_VOID(sink(ai::ThinkingDeltaEvent{
                                        .content_index = index,
                                        .delta = thinking->thinking,
                                        .partial = response,
                                }));
                            } else if (const auto* call = std::get_if<ai::ToolCallContent>(&block)) {
                                CCH_TRY_VOID(sink(ai::ToolCallStartEvent{
                                        .content_index = index,
                                        .partial = response,
                                }));
                                CCH_TRY_VOID(sink(ai::ToolCallDeltaEvent{
                                        .content_index = index,
                                        .delta = call->raw_arguments,
                                        .partial = response,
                                }));
                                CCH_TRY_VOID(sink(ai::ToolCallEndEvent{
                                        .content_index = index,
                                        .tool_call = *call,
                                        .partial = response,
                                }));
                            }
                        }
                    }
                    co_return response;
                });
    }

private:
    std::shared_ptr<ScriptedProviderControl> control_;
};

[[nodiscard]] inline coding_agent::ModelRuntimeTestProvider scripted_runtime_definition(
        std::shared_ptr<ScriptedRuntimeProvider> provider) {
    const std::string provider_id{provider->id()};
    auto models = provider->models();
    auto auth = std::move(provider->auth());
    return make_test_provider_definition(std::move(provider), provider_id, std::move(models), std::move(auth));
}

/// Test fixture that owns a concrete ModelRuntime and separate scripted
/// Provider/control state. Both the `fake` and `sdk-host` definitions are
/// installed through the ModelRuntime test seam.
struct ScriptedRuntimeFixture final {
    std::shared_ptr<EnvVarGuard> kimi_api_key;
    std::shared_ptr<coding_agent::ModelRuntime> runtime;
    std::shared_ptr<ScriptedProviderControl> control;

    ScriptedRuntimeFixture()
        : kimi_api_key(std::make_shared<EnvVarGuard>("KIMI_API_KEY", std::nullopt)),
          control(std::make_shared<ScriptedProviderControl>()) {
        coding_agent::ModelRuntimeOptions options;
        options.models_path = std::filesystem::path{};
        options.credentials = std::make_shared<detail::FixtureCredentialStore>();
        std::vector<coding_agent::ModelRuntimeTestProvider> definitions;
        definitions.push_back(
                scripted_runtime_definition(std::make_shared<ScriptedRuntimeProvider>("fake", control, true)));
        definitions.push_back(
                scripted_runtime_definition(std::make_shared<ScriptedRuntimeProvider>("sdk-host", control, false)));
        auto created = coding_agent::create_model_runtime_for_testing(std::move(options),
                coding_agent::ModelRuntimeTestOptions{
                        .providers = std::move(definitions),
                });
        if (!created) {
            std::terminate();
        }
        runtime = std::move(*created);

        if (!runtime->model("fake", "fake-model") || !runtime->model("sdk-host", "fake-model")) {
            std::terminate();
        }
        const auto available = run_async_result(runtime->get_available());
        if (!available || available->size() != 1 || available->front().provider != "fake") {
            std::terminate();
        }
    }
};

} // namespace cch::tests
