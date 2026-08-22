#pragma once

#include <cch/ai/Content.hpp>
#include <cch/ai/Provider.hpp>
#include <cch/coding_agent/ModelRuntime.hpp>
#include <cch/support/Error.hpp>
#include "ai/ModelStreamBridge.hpp"
#include "support/ModelsFixture.hpp"
#include "support/ExpectedMacros.hpp"
#include "support/ReleaseGate.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/error_code.hpp>

#include <chrono>
#include <cstddef>
#include <deque>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace cch::tests {

/// One recorded model-stream request at the provider seam (the post-Models
/// `ProviderStreamOptions` shape; `stop_token` is preserved for the gating
/// and cancellation contracts).
struct RecordedRuntimeCall {
    ai::Model model;
    ai::AiContext context;
    ai::ProviderStreamOptions options;
};

class FakeModelRuntime;

/// Scripted `ai::Provider` that records each request at the provider seam and
/// serves FIFO responses, with an optional gate on one call index. It is the
/// streaming half of the session-seam `FakeModelRuntime` (ADR 0040 / #453: the
/// Agent streams through `ai_models()`, never through a virtual runtime
/// surface).
class ScriptedRuntimeProvider final : public ai::Provider {
public:
    explicit ScriptedRuntimeProvider(FakeModelRuntime* owner);

    [[nodiscard]] std::string_view id() const noexcept override { return "fake"; }
    [[nodiscard]] std::string_view name() const noexcept override { return "Fake"; }
    [[nodiscard]] ai::ProviderAuth& auth() noexcept override { return auth_; }
    [[nodiscard]] std::vector<ai::Model> models() const override { return {}; }

    [[nodiscard]] ai::ModelStream stream(
        ai::Model model,
        ai::AiContext context,
        ai::ProviderStreamOptions options) override;

private:
    ai::ProviderAuth auth_;
    FakeModelRuntime* owner_; // must outlive the provider; the owner holds the catalog
};

/// Session-seam recording fake ModelRuntime (ADR 0034 / #349 primary session
/// seam): serves model resolution, auth preflight, and an AI-owned Models
/// catalog so a session can run a scripted turn end to end. Streaming flows
/// through `ai_models()` (the `ModelStream` surface), not a virtual
/// `stream_simple` override. Optional gating (`gate_at` + `release()`) holds
/// one call until released or the run's stop token fires.
class FakeModelRuntime final : public coding_agent::ModelRuntime {
public:
    FakeModelRuntime();
    ~FakeModelRuntime() = default;
    FakeModelRuntime(const FakeModelRuntime&) = delete;
    FakeModelRuntime& operator=(const FakeModelRuntime&) = delete;

    // ── Session-seam overrides (model resolution / auth) ──────────────────

    [[nodiscard]] std::optional<ai::Model> model(
        std::string_view provider_id,
        std::string_view model_id) const override;

    [[nodiscard]] support::AsyncResult<std::vector<ai::Model>> get_available(
            std::optional<std::string> provider_id = std::nullopt) override;

    [[nodiscard]] support::AsyncResult<std::optional<ai::AuthCheck>> check_auth(std::string provider_id) override;

    [[nodiscard]] bool has_configured_auth(std::string_view provider_id) const override;

    [[nodiscard]] std::vector<std::string> configured_api_key_env_names() const override;

    // ── AI-owned streaming catalog (ADR 0040 / #453) ──────────────────────

    [[nodiscard]] std::shared_ptr<ai::Models> ai_models() const override {
        return models_;
    }

    // ── Gating / scripting surface ────────────────────────────────────────

    /// Release the gated call, if any. Idempotent.
    void release();

    /// Call index that gates (blocks until release or stop). Defaults to a
    /// value larger than any real call, disabling gating.
    std::size_t gate_at{std::numeric_limits<std::size_t>::max()};
    /// Emit a "streaming in flight" partial before the gated call blocks
    /// (the interactive interrupt gating behavior).
    bool emit_partial_before_gate{false};
    /// Response served after the gate releases when `responses` is empty.
    std::string gated_response{"gated done"};
    /// Recorded per-turn calls in issue order.
    std::vector<RecordedRuntimeCall> calls;
    /// Scripted assistant responses consumed in FIFO order.
    std::deque<ai::AssistantMessage> responses;

private:
    std::shared_ptr<ai::Models> models_;
    ReleaseGate gate_;
    friend class ScriptedRuntimeProvider;
};

// ── Inline implementations (header-only test support) ─────────────────────

inline ScriptedRuntimeProvider::ScriptedRuntimeProvider(FakeModelRuntime* owner)
    : owner_(owner) {
    ai::ApiKeyAuth api_key;
    api_key.name = "scripted fake runtime";
    api_key.check = [](const ai::AuthContext&, std::optional<ai::ApiKeyCredential>)
        -> cch::support::AsyncResult<std::optional<ai::AuthCheck>> {
        return cch::support::AsyncResult<std::optional<ai::AuthCheck>>(
            std::expected<std::optional<ai::AuthCheck>, cch::support::Error>{
                ai::AuthCheck{
                    .source = "scripted fake runtime",
                    .type = ai::AuthType::ApiKey,
                }});
    };
    api_key.resolve = [](const ai::AuthContext&, std::optional<ai::ApiKeyCredential>)
        -> cch::support::AsyncResult<std::optional<ai::AuthResult>> {
        return cch::support::AsyncResult<std::optional<ai::AuthResult>>(
            std::expected<std::optional<ai::AuthResult>, cch::support::Error>{
                ai::AuthResult{.source = "scripted fake runtime"}});
    };
    auth_ = ai::ProviderAuth{.api_key = std::move(api_key)};
}

inline ai::ModelStream
ScriptedRuntimeProvider::stream(
    ai::Model model,
    ai::AiContext context,
    ai::ProviderStreamOptions options) {
    return ai::detail::make_model_stream(
        [this,
         model = std::move(model),
         context = std::move(context),
         options = std::move(options)](
            ai::AssistantEventSink sink)
            -> boost::asio::awaitable<support::Expected<ai::AssistantMessage>> {
    const std::size_t call_index = owner_->calls.size();
    owner_->calls.push_back(RecordedRuntimeCall{model, context, options});
    const auto& stop_token = owner_->calls.back().options.stop_token;

    if (stop_token.stop_requested()) {
        auto terminal = ai::assistant_text_message("");
        terminal.stop_reason = ai::AssistantStopReason::Aborted;
        terminal.error_message = "Request was aborted";
        if (sink) {
            CCH_TRY_VOID(sink(ai::AssistantErrorEvent{
                .reason = terminal.stop_reason,
                .error = terminal,
                .failure = support::make_error(
                    support::ErrorCode::Cancelled, "Request was aborted"),
            }));
        }
        co_return terminal;
    }

    if (owner_->emit_partial_before_gate && call_index == owner_->gate_at) {
        auto partial = ai::assistant_text_message("streaming in flight");
        if (sink) {
            CCH_TRY_VOID(sink(ai::AssistantStartEvent{partial}));
            CCH_TRY_VOID(sink(ai::TextDeltaEvent{0, "streaming in flight", partial}));
        }
    }

    if (call_index == owner_->gate_at) {
        // Hold the gated call; interrupt also wakes on the run's stop token.
        // The stop check must precede wait(): an interrupt delivered before
        // the gate is armed does not linger (ReleaseGate contract).
        std::stop_callback release_on_stop{stop_token, [this] { owner_->gate_.interrupt(); }};
        if (!stop_token.stop_requested()) {
            co_await owner_->gate_.wait();
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
                .failure = support::make_error(
                    support::ErrorCode::Cancelled, "Request was aborted"),
            }));
        }
        co_return terminal;
    }

    if (owner_->responses.empty()) {
        co_return ai::assistant_text_message(owner_->gated_response);
    }
    auto response = std::move(owner_->responses.front());
    owner_->responses.pop_front();
    if (sink) {
        if (response.stop_reason == ai::AssistantStopReason::Error ||
            response.stop_reason == ai::AssistantStopReason::Aborted) {
            std::optional<support::Error> terminal_failure = std::nullopt;
            if (response.error_message) {
                terminal_failure = support::make_error(
                    support::ErrorCode::Stream, *response.error_message);
            }
            CCH_TRY_VOID(sink(ai::AssistantErrorEvent{
                .reason = response.stop_reason,
                .error = response,
                .failure = std::move(terminal_failure),
            }));
            co_return response;
        }
        CCH_TRY_VOID(sink(ai::AssistantStartEvent{response}));
        for (std::size_t index = 0; index < response.content.size(); ++index) {
            const auto& block = response.content[index];
            if (const auto* text = std::get_if<ai::TextContent>(&block)) {
                CCH_TRY_VOID(sink(ai::TextDeltaEvent{index, text->text, response}));
            } else if (const auto* thinking = std::get_if<ai::ThinkingContent>(&block)) {
                CCH_TRY_VOID(sink(ai::ThinkingDeltaEvent{index, thinking->thinking, response}));
            } else if (const auto* call = std::get_if<ai::ToolCallContent>(&block)) {
                CCH_TRY_VOID(sink(ai::ToolCallStartEvent{index, response}));
                CCH_TRY_VOID(sink(ai::ToolCallDeltaEvent{index, call->raw_arguments, response}));
                CCH_TRY_VOID(sink(ai::ToolCallEndEvent{index, *call, response}));
            }
        }
    }
    co_return response;
        });
}

inline FakeModelRuntime::FakeModelRuntime() {
    models_ = models_from_provider(std::make_shared<ScriptedRuntimeProvider>(this));
}

inline std::optional<ai::Model> FakeModelRuntime::model(
    std::string_view provider_id,
    std::string_view model_id) const {
    ai::Model model;
    model.id = std::string{model_id};
    model.name = model.id;
    model.api = "scripted-fake";
    model.provider = std::string{provider_id};
    model.reasoning = false;
    model.input = {ai::ModelInput::Text};
    model.context_window = 128000;
    model.max_tokens = 16384;
    return model;
}

inline support::AsyncResult<std::vector<ai::Model>> FakeModelRuntime::get_available(
        std::optional<std::string> provider_id) {
    std::vector<ai::Model> models;
    if (auto resolved = model(provider_id.value_or("fake"), "fake-model")) {
        models.push_back(std::move(*resolved));
    }
    return support::AsyncResult<std::vector<ai::Model>>(support::Expected<std::vector<ai::Model>>(std::move(models)));
}

inline support::AsyncResult<std::optional<ai::AuthCheck>> FakeModelRuntime::check_auth(std::string provider_id) {
    (void)provider_id;
    return support::AsyncResult<std::optional<ai::AuthCheck>>(support::Expected<std::optional<ai::AuthCheck>>(
            ai::AuthCheck{.source = "fake", .type = ai::AuthType::ApiKey}));
}

inline bool FakeModelRuntime::has_configured_auth(std::string_view provider_id) const {
    (void)provider_id;
    return true;
}

inline std::vector<std::string> FakeModelRuntime::configured_api_key_env_names() const {
    return {};
}

inline void FakeModelRuntime::release() {
    gate_.release();
}

} // namespace cch::tests
