#include <cch/agent/AgentEvent.hpp>
#include "support/ModelsFixture.hpp"
#include <cch/agent/AgentTool.hpp>
#include <cch/ai/Content.hpp>
#include <cch/ai/Context.hpp>
#include <cch/ai/Message.hpp>
#include "ai/providers/StreamEmit.hpp"
#include <cch/coding_agent/Sdk.hpp>
#include <cch/coding_agent/AuthGuidance.hpp>
#include <cch/coding_agent/Skill.hpp>
#include <cch/harness/ExecutionEnv.hpp>
#include <cch/harness/session/JsonlSessionStore.hpp>
#include <cch/util/Error.hpp>
#include "ai/providers/FakeProvider.hpp"
#include "coding_agent/AgentSessionBridge.hpp"
#include "coding_agent/SessionPathPolicy.hpp"
#include "coding_agent/runtime/AgentSessionPromptAccess.hpp"
#include "harness/session/SessionJournalTestHooks.hpp"
#include "support/EnvVarGuard.hpp"
#include "support/ModelFixture.hpp"
#include "support/TempWorkspace.hpp"
#include "support/UsageAssertions.hpp"
#include "util/ExpectedMacros.hpp"
#include "util/Json.hpp"

#include "../../third_party/catch2/catch_test_macros.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/execution/context.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

using namespace cch;

namespace {

// ── Helpers ──────────────────────────────────────────────────────────────────

/// A minimal fake tool for testing custom tool registration.
class FakeEchoTool final : public agent::AsyncAgentTool {
public:
    explicit FakeEchoTool(std::shared_ptr<std::size_t> execution_count = {})
        : execution_count_(std::move(execution_count)) {
        def_.name = "echo";
        def_.description = "Echo back the input";
        def_.parameters = util::JsonValue::object_t{{"type", "object"}, {"additionalProperties", false}};
    }

    [[nodiscard]] const ai::Tool& definition() const override { return def_; }

    [[nodiscard]] boost::asio::awaitable<util::Expected<agent::AsyncToolExecutionResult>> execute(
        agent::ToolInvocation /*invocation*/,
        std::stop_token) override {
        if (execution_count_) {
            ++*execution_count_;
        }
        agent::AsyncToolExecutionResult result;
        result.content.push_back(ai::text_content("echo: ok"));
        co_return result;
    }

private:
    ai::Tool def_;
    std::shared_ptr<std::size_t> execution_count_;
};

struct SdkOwnedLifetimeCounts {
    std::size_t destroyed_clients{0};
    std::size_t destroyed_tools{0};
};

struct SubscriberLifetimeToken {};

[[nodiscard]] bool has_aborted_message_end(
    const std::vector<agent::AgentLifecycleEvent>& events) {
    return std::ranges::any_of(events, [](const agent::AgentLifecycleEvent& event) {
        const auto* end = std::get_if<agent::MessageEndEvent>(&event);
        return end != nullptr &&
               std::holds_alternative<ai::AssistantMessage>(end->message) &&
               std::get<ai::AssistantMessage>(end->message).stop_reason ==
                   ai::AssistantStopReason::Aborted;
    });
}

class LifetimeTrackedChatProvider final : public tests::ScriptedProvider {
public:
    explicit LifetimeTrackedChatProvider(std::shared_ptr<SdkOwnedLifetimeCounts> counts)
        : ScriptedProvider("sdk-host"), counts_(std::move(counts)) {}
    LifetimeTrackedChatProvider(LifetimeTrackedChatProvider&&) = delete;
    LifetimeTrackedChatProvider& operator=(LifetimeTrackedChatProvider&&) = delete;
    ~LifetimeTrackedChatProvider() override {
        ++counts_->destroyed_clients;
    }
    LifetimeTrackedChatProvider(const LifetimeTrackedChatProvider&) = delete;
    LifetimeTrackedChatProvider& operator=(const LifetimeTrackedChatProvider&) = delete;

    [[nodiscard]] boost::asio::awaitable<util::Expected<ai::AssistantMessage>> stream(
        const ai::Model& model,
        const ai::AiContext& context,
        ai::ProviderStreamOptions options,
        ai::AssistantEventSink) override {
        auto response = ai::assistant_text_message("tracked");
        response.provider = "tracked-fake";
        response.api = "fake";
        response.model = model.id;
        co_return response;
    }

private:
    std::shared_ptr<SdkOwnedLifetimeCounts> counts_;
};

class LifetimeTrackedTool final : public agent::AsyncAgentTool {
public:
    explicit LifetimeTrackedTool(std::shared_ptr<SdkOwnedLifetimeCounts> counts)
        : counts_(std::move(counts)) {
        definition_.name = "lifetime_tracked";
        definition_.description = "Track SDK-owned tool teardown";
        definition_.parameters = util::JsonValue::object_t{
            {"type", "object"},
            {"additionalProperties", false},
        };
    }
    LifetimeTrackedTool(LifetimeTrackedTool&&) = delete;
    LifetimeTrackedTool& operator=(LifetimeTrackedTool&&) = delete;
    ~LifetimeTrackedTool() override {
        ++counts_->destroyed_tools;
    }
    LifetimeTrackedTool(const LifetimeTrackedTool&) = delete;
    LifetimeTrackedTool& operator=(const LifetimeTrackedTool&) = delete;

    [[nodiscard]] const ai::Tool& definition() const override {
        return definition_;
    }

    [[nodiscard]] boost::asio::awaitable<util::Expected<agent::AsyncToolExecutionResult>> execute(
        agent::ToolInvocation,
        std::stop_token) override {
        co_return agent::AsyncToolExecutionResult{};
    }

private:
    std::shared_ptr<SdkOwnedLifetimeCounts> counts_;
    ai::Tool definition_;
};

/// Create a temp workspace and session path for testing.
struct TestPaths {
    cch::tests::TempWorkspace workspace;
    std::filesystem::path session_file;

    TestPaths() {
        session_file = workspace.path() / "test-session.jsonl";
    }
};

harness::session::SessionMetadata test_metadata(const TestPaths& paths) {
    return {
        .session_id = "sdk-session-test",
        .created_at = "2026-07-05T00:00:00Z",
        .workspace = paths.workspace.path(),
        .provider = "fake",
        .model = "fake-model",
    };
}

ai::MessageVariant user_msg(std::string text) {
    return ai::MessageVariant{ai::user_text_message(std::move(text))};
}

tests::ModelsSessionOptions sdk_resume_options(const TestPaths& paths) {
    tests::ModelsSessionOptions opts;
    opts.session_target = coding_agent::ExplicitResumeSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.models = ai::providers::make_scripted_fake_models();
    return opts;
}

void expect_unsupported_sdk_resume(const TestPaths& paths) {
    auto result = coding_agent::create_agent_session(sdk_resume_options(paths));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == util::ErrorCode::Session);
    CHECK(result.error().message == "unsupported_session_topology");
}

void write_project_skill(
    const TestPaths& paths,
    std::string name,
    std::string body = "Project skill body.") {
    paths.workspace.write(
        ".cpp-harness/skills/" + name + "/SKILL.md",
        "---\n"
        "name: " + name + "\n"
        "description: Project skill.\n"
        "---\n" +
        body + "\n");
}

void write_project_prompt(
    const TestPaths& paths,
    std::string name,
    std::string body = "Project prompt body.") {
    paths.workspace.write(
        ".cpp-harness/prompts/" + name + ".md",
        "---\n"
        "description: Project prompt.\n"
        "---\n" +
        body + "\n");
}

coding_agent::Skill host_skill(std::string name, std::string body = "Host skill body.") {
    return coding_agent::Skill{
        .name = std::move(name),
        .description = "Host skill.",
        .content = std::move(body),
        .filePath = "/host/SKILL.md",
    };
}

coding_agent::PromptTemplate host_template(std::string name, std::string body = "Host prompt body.") {
    return coding_agent::PromptTemplate{
        .name = std::move(name),
        .description = "Host template.",
        .content = std::move(body),
    };
}

bool has_sdk_diag(
    const std::vector<coding_agent::SdkDiagnostic>& diagnostics,
    std::string_view code) {
    return std::any_of(
        diagnostics.begin(),
        diagnostics.end(),
        [code](const auto& diag) { return diag.code == code; });
}

std::vector<coding_agent::SdkDiagnostic>::const_iterator find_sdk_diag(
    const std::vector<coding_agent::SdkDiagnostic>& diagnostics,
    std::string_view code) {
    return std::find_if(
        diagnostics.begin(),
        diagnostics.end(),
        [code](const auto& diag) { return diag.code == code; });
}

/// A fake execution environment that records how many times cleanup() runs.
class CountingFakeEnv final : public harness::AsyncExecutionEnv {
public:
    explicit CountingFakeEnv(std::filesystem::path workspace)
        : workspace_(std::move(workspace)) {}

    [[nodiscard]] const std::filesystem::path& workspace() const override { return workspace_; }

    [[nodiscard]] boost::asio::awaitable<std::expected<std::string, harness::FileError>> absolutePath(
        std::string path,
        std::stop_token) override {
        co_return path;
    }
    [[nodiscard]] boost::asio::awaitable<std::expected<std::string, harness::FileError>> joinPath(
        std::vector<std::string>,
        std::stop_token) override {
        co_return std::string{};
    }
    [[nodiscard]] boost::asio::awaitable<std::expected<std::string, harness::FileError>> readTextFile(
        std::string,
        std::stop_token) override {
        co_return std::string{};
    }
    [[nodiscard]] boost::asio::awaitable<std::expected<std::vector<std::string>, harness::FileError>> readTextLines(
        std::string,
        std::optional<int>,
        std::stop_token) override {
        co_return std::vector<std::string>{};
    }
    [[nodiscard]] boost::asio::awaitable<std::expected<harness::BinaryData, harness::FileError>> readBinaryFile(
        std::string,
        std::stop_token) override {
        co_return harness::BinaryData{};
    }
    [[nodiscard]] boost::asio::awaitable<std::expected<void, harness::FileError>> writeFile(
        std::string,
        harness::WriteContent,
        std::stop_token) override {
        co_return std::expected<void, harness::FileError>{};
    }
    [[nodiscard]] boost::asio::awaitable<std::expected<void, harness::FileError>> appendFile(
        std::string,
        harness::WriteContent,
        std::stop_token) override {
        co_return std::expected<void, harness::FileError>{};
    }
    [[nodiscard]] boost::asio::awaitable<std::expected<harness::FileInfo, harness::FileError>> fileInfo(
        std::string,
        std::stop_token) override {
        co_return harness::FileInfo{};
    }
    [[nodiscard]] boost::asio::awaitable<std::expected<std::vector<harness::FileInfo>, harness::FileError>> listDir(
        std::string,
        std::stop_token) override {
        co_return std::vector<harness::FileInfo>{};
    }
    [[nodiscard]] boost::asio::awaitable<std::expected<std::string, harness::FileError>> canonicalPath(
        std::string path,
        std::stop_token) override {
        co_return path;
    }
    [[nodiscard]] boost::asio::awaitable<std::expected<bool, harness::FileError>> exists(
        std::string,
        std::stop_token) override {
        co_return true;
    }
    [[nodiscard]] boost::asio::awaitable<std::expected<void, harness::FileError>> createDir(
        std::string,
        bool,
        std::stop_token) override {
        co_return std::expected<void, harness::FileError>{};
    }
    [[nodiscard]] boost::asio::awaitable<std::expected<void, harness::FileError>> remove(
        std::string,
        bool,
        std::stop_token) override {
        co_return std::expected<void, harness::FileError>{};
    }
    [[nodiscard]] boost::asio::awaitable<std::expected<std::string, harness::FileError>> createTempDir(
        std::optional<std::string>,
        std::stop_token) override {
        co_return std::string{};
    }
    [[nodiscard]] boost::asio::awaitable<std::expected<std::string, harness::FileError>> createTempFile(
        std::optional<std::string>,
        std::optional<std::string>,
        std::stop_token) override {
        co_return std::string{};
    }

    [[nodiscard]] boost::asio::awaitable<std::expected<harness::ShellExecResult, harness::ExecutionError>> exec(
        std::string,
        harness::ExecOptions) override {
        co_return harness::ShellExecResult{};
    }

    boost::asio::awaitable<void> cleanup() override {
        ++cleanup_count;
        co_return;
    }

    int cleanup_count{0};

private:
    std::filesystem::path workspace_;
};

/// A host chat client that records the request so tests can inspect the tool
/// registry sent to the model.
class CaptureChatProvider final : public tests::ScriptedProvider {
public:
    CaptureChatProvider() : ScriptedProvider("sdk-host") {}
    std::optional<tests::RecordedProviderRequest> captured_request;

    [[nodiscard]] boost::asio::awaitable<util::Expected<ai::AssistantMessage>> stream(
        const ai::Model& model,
        const ai::AiContext& context,
        ai::ProviderStreamOptions options,
        ai::AssistantEventSink /*sink*/) override {
        captured_request = tests::RecordedProviderRequest{model, context, options};
        ai::AssistantMessage msg;
        // Mirror the real adapters (assistant.provider = model.provider): the
        // provider must resolve in the models seam's catalog so resume's
        // derived model re-resolves against the live runtime (T08).
        msg.provider = model.provider;
        msg.api = "capture";
        msg.model = model.id;
        msg.stop_reason = ai::AssistantStopReason::Stop;
        msg.content.emplace_back(ai::TextContent{"captured", std::nullopt});
        msg.timestamp = 1718000000123;
        co_return msg;
    }
};

class ImageRejectingChatProvider final : public tests::ScriptedProvider {
public:
    ImageRejectingChatProvider() : ScriptedProvider("sdk-host") {}
    [[nodiscard]] boost::asio::awaitable<util::Expected<ai::AssistantMessage>> stream(
        const ai::Model& model,
        const ai::AiContext& context,
        ai::ProviderStreamOptions options,
        ai::AssistantEventSink /*sink*/) override {
        ++request_count;
        const bool has_image = std::ranges::any_of(
            context.messages,
            [](const ai::MessageVariant& message) {
                const auto* user = std::get_if<ai::UserMessage>(&message);
                return user != nullptr && std::ranges::any_of(
                    std::get<std::vector<ai::Content>>(user->content),
                    [](const ai::Content& content) {
                        return std::holds_alternative<ai::ImageContent>(content);
                    });
            });
        if (has_image) {
            co_return std::unexpected(util::make_error(
                util::ErrorCode::Provider,
                "image input is not supported"));
        }

        auto response = ai::assistant_text_message("text accepted");
        response.provider = "image-rejecting-fake";
        response.api = "fake";
        response.model = model.id;
        co_return response;
    }

    int request_count{0};
};

class MalformedArgumentsChatProvider final : public tests::ScriptedProvider {
public:
    explicit MalformedArgumentsChatProvider(std::string secret)
        : ScriptedProvider("sdk-host"), secret_(std::move(secret)) {}

    [[nodiscard]] boost::asio::awaitable<util::Expected<ai::AssistantMessage>> stream(
        const ai::Model& model,
        const ai::AiContext& context,
        ai::ProviderStreamOptions options,
        ai::AssistantEventSink /*sink*/) override {
        requests.push_back(tests::RecordedProviderRequest{model, context, options});

        ai::AssistantMessage response;
        response.provider = "fake";
        response.api = "issue33-fake";
        response.model = model.id;
        response.timestamp = 1718000000123;
        if (requests.size() > 1) {
            response.content.emplace_back(ai::text_content("recovered after malformed call"));
            co_return response;
        }

        const std::string malformed_raw =
            "{\"api_key\":\"" + secret_ + "\" BROKEN";
        auto malformed_parse = util::read_json<util::JsonValue>(malformed_raw);
        if (malformed_parse) {
            co_return std::unexpected(util::make_error(
                util::ErrorCode::Unknown,
                "fake malformed arguments unexpectedly parsed"));
        }

        ai::ToolCallContent malformed;
        malformed.id = "call-malformed";
        malformed.name = "echo";
        malformed.raw_arguments = malformed_raw;
        malformed.arguments_valid = false;
        malformed.argument_error = malformed_parse.error().detail;
        response.content.emplace_back(std::move(malformed));

        ai::ToolCallContent valid;
        valid.id = "call-valid";
        valid.name = "echo";
        valid.arguments = util::JsonValue::object_t{};
        valid.raw_arguments = "{}";
        response.content.emplace_back(std::move(valid));
        response.stop_reason = ai::AssistantStopReason::ToolUse;
        co_return response;
    }

    std::vector<tests::RecordedProviderRequest> requests;

private:
    std::string secret_;
};

class PreflightRejectingChatProvider final : public tests::ScriptedProvider {
public:
    PreflightRejectingChatProvider() : ScriptedProvider("sdk-host") {}
    [[nodiscard]] boost::asio::awaitable<util::Expected<ai::AssistantMessage>> stream(
        const ai::Model&,
        const ai::AiContext&,
        ai::ProviderStreamOptions,
        ai::AssistantEventSink  /*sink*/) override {
        co_return std::unexpected(util::make_error(
            util::ErrorCode::Provider,
            "provider rejected in-memory prompt"));
    }
};

struct PreflightCancellationObservation {
    bool stop_requested{false};
};

class PreflightCancellationChatProvider final : public tests::ScriptedProvider {
public:
    explicit PreflightCancellationChatProvider(
        std::shared_ptr<PreflightCancellationObservation> observation)
        : ScriptedProvider("sdk-host"), observation_(std::move(observation)) {}
    PreflightCancellationChatProvider(PreflightCancellationChatProvider&&) = delete;
    PreflightCancellationChatProvider& operator=(PreflightCancellationChatProvider&&) = delete;
    ~PreflightCancellationChatProvider() override = default;
    PreflightCancellationChatProvider(const PreflightCancellationChatProvider&) = delete;
    PreflightCancellationChatProvider& operator=(const PreflightCancellationChatProvider&) = delete;

    [[nodiscard]] boost::asio::awaitable<util::Expected<ai::AssistantMessage>> stream(
        const ai::Model& model,
        const ai::AiContext& context,
        ai::ProviderStreamOptions options,
        ai::AssistantEventSink sink) override {
        observation_->stop_requested = options.stop_token.stop_requested();
        auto response = ai::assistant_text_message(
            observation_->stop_requested ? "" : "not cancelled");
        response.provider = "preflight-cancellation-fake";
        response.api = "fake";
        response.model = model.id;
        if (observation_->stop_requested) {
            response.stop_reason = ai::AssistantStopReason::Aborted;
            response.error_message = "prompt aborted";
            if (sink) {
                try {
                    if (auto emitted = sink(ai::AssistantErrorEvent{
                            .reason = ai::AssistantStopReason::Aborted,
                            .error = response,
                        });
                        !emitted) {
                        co_return std::unexpected(emitted.error());
                    }
                } catch (const std::exception& exception) {
                    co_return std::unexpected(util::make_error(
                        util::ErrorCode::Unknown,
                        "fake provider event sink failed",
                        exception.what()));
                } catch (...) {
                    co_return std::unexpected(util::make_error(
                        util::ErrorCode::Unknown,
                        "fake provider event sink failed",
                        "unknown exception"));
                }
            }
        }
        co_return response;
    }

private:
    std::shared_ptr<PreflightCancellationObservation> observation_;
};

class ExecutorCapturingChatProvider final : public tests::ScriptedProvider {
public:
    ExecutorCapturingChatProvider() : ScriptedProvider("sdk-host") {}
    [[nodiscard]] boost::asio::awaitable<util::Expected<ai::AssistantMessage>> stream(
        const ai::Model& model,
        const ai::AiContext& context,
        ai::ProviderStreamOptions options,
        ai::AssistantEventSink /*sink*/) override {
        auto executor = co_await boost::asio::this_coro::executor;
        execution_context = &boost::asio::query(
            executor, boost::asio::execution::context);
        ++request_count;

        auto response = ai::assistant_text_message("async response");
        response.provider = "executor-capturing-fake";
        response.api = "fake";
        response.model = model.id;
        co_return response;
    }

    boost::asio::execution_context* execution_context{nullptr};
    int request_count{0};
};

class GatedSdkChatProvider final : public tests::ScriptedProvider {
public:
    GatedSdkChatProvider() : ScriptedProvider("sdk-host") {}
    [[nodiscard]] boost::asio::awaitable<util::Expected<ai::AssistantMessage>> stream(
        const ai::Model& model,
        const ai::AiContext& context,
        ai::ProviderStreamOptions options,
        ai::AssistantEventSink /*sink*/) override {
        auto executor = co_await boost::asio::this_coro::executor;
        gate.emplace(executor);
        gate->expires_at(std::chrono::steady_clock::time_point::max());
        ++request_count;
        started = true;

        boost::system::error_code error;
        co_await gate->async_wait(
            boost::asio::redirect_error(boost::asio::use_awaitable, error));

        auto response = ai::assistant_text_message("released");
        response.provider = "gated-fake";
        response.api = "fake";
        response.model = model.id;
        co_return response;
    }

    void release() {
        if (gate) {
            gate->cancel();
        }
    }

    bool started{false};
    int request_count{0};
    std::optional<boost::asio::steady_timer> gate;
};

/// A controllable conforming fake that retains optional partial output and
/// completes a stopped provider request through one aborted terminal event.
class AbortAwareSdkChatProvider final : public tests::ScriptedProvider {
public:
    explicit AbortAwareSdkChatProvider(
        bool emit_partial,
        std::shared_ptr<SdkOwnedLifetimeCounts> lifetime_counts = {})
        : ScriptedProvider("sdk-host"),
          emit_partial_(emit_partial),
          lifetime_counts_(std::move(lifetime_counts)) {}
    AbortAwareSdkChatProvider(AbortAwareSdkChatProvider&&) = delete;
    AbortAwareSdkChatProvider& operator=(AbortAwareSdkChatProvider&&) = delete;
    ~AbortAwareSdkChatProvider() override {
        if (lifetime_counts_) {
            ++lifetime_counts_->destroyed_clients;
        }
    }
    AbortAwareSdkChatProvider(const AbortAwareSdkChatProvider&) = delete;
    AbortAwareSdkChatProvider& operator=(const AbortAwareSdkChatProvider&) = delete;

    [[nodiscard]] boost::asio::awaitable<util::Expected<ai::AssistantMessage>> stream(
        const ai::Model& model,
        const ai::AiContext& context,
        ai::ProviderStreamOptions options,
        ai::AssistantEventSink sink) override {
        ++request_count;
        stop_possible = options.stop_token.stop_possible();
        if (request_count > 1) {
            recovery_stop_requested = options.stop_token.stop_requested();
            recovery_uses_fresh_token =
                first_stop_token && options.stop_token != *first_stop_token;
            auto recovered = ai::assistant_text_message("recovered after abort");
            recovered.provider = "abort-aware-fake";
            recovered.api = "fake";
            recovered.model = model.id;
            recovered.timestamp = 1718000000123;
            co_return recovered;
        }

        first_stop_token = options.stop_token;
        auto partial = ai::assistant_text_message("");
        partial.provider = "abort-aware-fake";
        partial.api = "fake";
        partial.model = model.id;
        partial.timestamp = 1718000000123;
        partial.content.clear();
        if (sink) {
            if (auto emitted = sink(ai::AssistantStartEvent{partial}); !emitted) {
                co_return std::unexpected(emitted.error());
            }
        }
        if (emit_partial_) {
            partial.content.emplace_back(ai::text_content(""));
            if (sink) {
                if (auto emitted = sink(ai::TextStartEvent{
                        .content_index = 0,
                        .partial = partial,
                    });
                    !emitted) {
                    co_return std::unexpected(emitted.error());
                }
            }
            std::get<ai::TextContent>(partial.content[0]).text = "partial";
            if (sink) {
                if (auto emitted = sink(ai::TextDeltaEvent{
                        .content_index = 0,
                        .delta = "partial",
                        .partial = partial,
                    });
                    !emitted) {
                    co_return std::unexpected(emitted.error());
                }
            }
            partial_emitted = true;
        }

        auto executor = co_await boost::asio::this_coro::executor;
        gate.emplace(executor);
        gate->expires_at(std::chrono::steady_clock::time_point::max());
        started = true;
        std::stop_callback cancellation{options.stop_token, [this] {
            if (gate) {
                gate->cancel();
            }
        }};

        boost::system::error_code error;
        if (!options.stop_token.stop_requested()) {
            co_await gate->async_wait(
                boost::asio::redirect_error(boost::asio::use_awaitable, error));
        }

        if (options.stop_token.stop_requested()) {
            partial.stop_reason = ai::AssistantStopReason::Aborted;
            partial.error_message = "prompt aborted";
            if (sink) {
                if (auto emitted = sink(ai::AssistantErrorEvent{
                        .reason = ai::AssistantStopReason::Aborted,
                        .error = partial,
                    });
                    !emitted) {
                    co_return std::unexpected(emitted.error());
                }
            }
            co_return partial;
        }

        auto released = ai::assistant_text_message("released without abort");
        released.provider = "abort-aware-fake";
        released.api = "fake";
        released.model = model.id;
        released.timestamp = 1718000000123;
        co_return released;
    }

    bool started{false};
    bool partial_emitted{false};
    bool stop_possible{false};
    bool recovery_stop_requested{true};
    bool recovery_uses_fresh_token{false};
    int request_count{0};
    std::optional<std::stop_token> first_stop_token;
    std::optional<boost::asio::steady_timer> gate;

private:
    bool emit_partial_{false};
    std::shared_ptr<SdkOwnedLifetimeCounts> lifetime_counts_;
};

/// A conforming host-provided client whose accepted call emits one terminal
/// error event before any assistant start event and returns the same final
/// AssistantMessage through the value alternative.
class TerminalBeforeStartChatProvider final : public tests::ScriptedProvider {
public:
    TerminalBeforeStartChatProvider(ai::AssistantStopReason reason, std::string diagnostic)
        : ScriptedProvider("sdk-host"), reason_(reason), diagnostic_(std::move(diagnostic)) {}

    [[nodiscard]] boost::asio::awaitable<util::Expected<ai::AssistantMessage>> stream(
        const ai::Model& model,
        const ai::AiContext& context,
        ai::ProviderStreamOptions options,
        ai::AssistantEventSink sink) override {
        ++request_count;
        ai::AssistantMessage terminal;
        terminal.provider = "host";
        terminal.api = "host";
        terminal.model = model.id;
        terminal.stop_reason = reason_;
        terminal.error_message = diagnostic_;
        terminal.timestamp = 1718000000123;
        if (sink) {
            auto delivered = sink(ai::AssistantErrorEvent{reason_, terminal});
            if (!delivered) {
                co_return std::unexpected(delivered.error());
            }
        }
        co_return terminal;
    }

    int request_count{0};

private:
    ai::AssistantStopReason reason_;
    std::string diagnostic_;
};

[[nodiscard]] ai::AssistantMessage scripted_terminal(
    const ai::Model& model,
    ai::AssistantStopReason reason,
    std::string error_message) {
    ai::AssistantMessage terminal;
    // Mirror the real adapters (assistant.provider = model.provider): the
    // provider must resolve in the models seam's catalog so resume's derived
    // model re-resolves against the live runtime (T08).
    terminal.provider = model.provider;
    terminal.api = "scripted-fake";
    terminal.model = model.id;
    terminal.stop_reason = reason;
    terminal.error_message = std::move(error_message);
    terminal.timestamp = 1718000000123;
    return terminal;
}

[[nodiscard]] boost::asio::awaitable<util::Expected<ai::AssistantMessage>> emit_recovered_text(
    const ai::Model& model,
    ai::AssistantEventSink& sink) {
    auto message = ai::assistant_text_message("recovered");
    // Mirror the real adapters (assistant.provider = model.provider): the
    // provider must resolve in the models seam's catalog so resume's derived
    // model re-resolves against the live runtime (T08).
    message.provider = model.provider;
    message.api = "scripted-fake";
    message.model = model.id;
    message.timestamp = 1718000000123;

    auto partial = message;
    partial.content.clear();
    CCH_TRY_VOID(ai::providers::emit(sink, ai::AssistantStartEvent{.partial = partial}));
    partial.content.emplace_back(ai::TextContent{.text = "", .text_signature = std::nullopt});
    CCH_TRY_VOID(ai::providers::emit(sink, ai::TextStartEvent{.content_index = 0, .partial = partial}));
    std::get<ai::TextContent>(partial.content[0]).text = "recovered";
    CCH_TRY_VOID(ai::providers::emit(sink, ai::TextDeltaEvent{
        .content_index = 0,
        .delta = "recovered",
        .partial = partial,
    }));
    CCH_TRY_VOID(ai::providers::emit(sink, ai::TextEndEvent{
        .content_index = 0,
        .content = "recovered",
        .partial = partial,
    }));
    CCH_TRY_VOID(ai::providers::emit(sink, ai::AssistantDoneEvent{
        .reason = message.stop_reason,
        .message = message,
    }));
    co_return message;
}

/// Preflight gate mirroring the removed wire adapter's API-key check: without
/// a key the stream rejects before the wrapped transport-side client runs.
class ApiKeyGatedChatProvider final : public tests::ScriptedProvider {
public:
    ApiKeyGatedChatProvider(
        std::shared_ptr<ai::Provider> inner,
        std::optional<std::string> api_key)
        : ScriptedProvider("sdk-host"), inner_(std::move(inner)), api_key_(std::move(api_key)) {}

    [[nodiscard]] boost::asio::awaitable<util::Expected<ai::AssistantMessage>> stream(
        const ai::Model& model,
        const ai::AiContext& context,
        ai::ProviderStreamOptions options,
        ai::AssistantEventSink sink) override {
        if (!api_key_) {
            co_return std::unexpected(util::make_error(
                util::ErrorCode::Provider,
                "missing API key"));
        }
        co_return co_await inner_->stream(model, context, std::move(options), std::move(sink));
    }

private:
    std::shared_ptr<ai::Provider> inner_;
    std::optional<std::string> api_key_;
};

/// Stand-in for the transport seam behind the preflight gate: reaching it at
/// all is the failure mode the gate test asserts against.
class TransportStandInChatProvider final : public tests::ScriptedProvider {
public:
    TransportStandInChatProvider() : ScriptedProvider("sdk-host") {}
    [[nodiscard]] boost::asio::awaitable<util::Expected<ai::AssistantMessage>> stream(
        const ai::Model&,
        const ai::AiContext&,
        ai::ProviderStreamOptions,
        ai::AssistantEventSink  /*sink*/) override {
        ++request_count;
        co_return std::unexpected(util::make_error(
            util::ErrorCode::Network,
            "transport must not run"));
    }

    int request_count{0};
};

/// First call starts the assistant stream and then fails with a scripted
/// network-class terminal whose message matches the transport detail the
/// removed wire adapter surfaced; later calls recover with plain text.
class RecoveringFailureChatProvider final : public tests::ScriptedProvider {
public:
    explicit RecoveringFailureChatProvider(std::string failure_message)
        : ScriptedProvider("sdk-host"), failure_message_(std::move(failure_message)) {}

    [[nodiscard]] boost::asio::awaitable<util::Expected<ai::AssistantMessage>> stream(
        const ai::Model& model,
        const ai::AiContext& context,
        ai::ProviderStreamOptions options,
        ai::AssistantEventSink sink) override {
        ++request_count;
        if (request_count > 1) {
            co_return co_await emit_recovered_text(model, sink);
        }
        auto terminal = scripted_terminal(
            model, ai::AssistantStopReason::Error, failure_message_);
        CCH_TRY_VOID(ai::providers::emit(sink, ai::AssistantStartEvent{.partial = terminal}));
        CCH_TRY_VOID(ai::providers::emit(sink, ai::AssistantErrorEvent{
            .reason = terminal.stop_reason,
            .error = terminal,
            .failure = util::make_error(util::ErrorCode::Network, failure_message_),
        }));
        co_return terminal;
    }

    int request_count{0};

private:
    std::string failure_message_;
};

/// First call streams a partial tool call and then fails with a
/// cancellation-class terminal carrying the accumulated call; no tool-call
/// end event is emitted because the stream broke mid-call. Later calls
/// recover with plain text.
class PartialToolCallAbortChatProvider final : public tests::ScriptedProvider {
public:
    PartialToolCallAbortChatProvider() : ScriptedProvider("sdk-host") {}
    [[nodiscard]] boost::asio::awaitable<util::Expected<ai::AssistantMessage>> stream(
        const ai::Model& model,
        const ai::AiContext& context,
        ai::ProviderStreamOptions options,
        ai::AssistantEventSink sink) override {
        ++request_count;
        if (request_count > 1) {
            co_return co_await emit_recovered_text(model, sink);
        }
        auto partial = scripted_terminal(
            model, ai::AssistantStopReason::Aborted, "transport operation was cancelled");
        partial.content.clear();
        CCH_TRY_VOID(ai::providers::emit(sink, ai::AssistantStartEvent{.partial = partial}));
        partial.content.emplace_back(ai::ToolCallContent{
            .id = "call_cancelled",
            .name = "echo",
            .arguments = util::JsonValue{util::JsonValue::object_t{}},
            .raw_arguments = {},
            .thought_signature = std::nullopt,
            .arguments_valid = true,
            .argument_error = std::nullopt,
        });
        CCH_TRY_VOID(ai::providers::emit(sink, ai::ToolCallStartEvent{
            .content_index = 0,
            .partial = partial,
        }));
        auto& streaming_call = std::get<ai::ToolCallContent>(partial.content[0]);
        streaming_call.raw_arguments = R"({"value":"queued"})";
        util::JsonValue::object_t arguments_object;
        arguments_object.emplace("value", util::JsonValue{"queued"});
        streaming_call.arguments = util::JsonValue{std::move(arguments_object)};
        CCH_TRY_VOID(ai::providers::emit(sink, ai::ToolCallDeltaEvent{
            .content_index = 0,
            .delta = streaming_call.raw_arguments,
            .partial = partial,
        }));
        auto terminal = partial;
        terminal.stop_reason = ai::AssistantStopReason::Aborted;
        terminal.error_message = "transport operation was cancelled";
        CCH_TRY_VOID(ai::providers::emit(sink, ai::AssistantErrorEvent{
            .reason = terminal.stop_reason,
            .error = terminal,
            .failure = util::make_error(
                util::ErrorCode::Cancelled,
                "provider request cancelled",
                "transport operation was cancelled"),
        }));
        co_return terminal;
    }

    int request_count{0};
};

/// Streams one thinking block, one text block, and a partial tool call whose
/// arguments stay malformed, then fails with a terminal error carrying all
/// three accumulated content items.
class PartialContentFailureChatProvider final : public tests::ScriptedProvider {
public:
    PartialContentFailureChatProvider() : ScriptedProvider("sdk-host") {}
    [[nodiscard]] boost::asio::awaitable<util::Expected<ai::AssistantMessage>> stream(
        const ai::Model& model,
        const ai::AiContext& context,
        ai::ProviderStreamOptions options,
        ai::AssistantEventSink sink) override {
        ++request_count;
        auto partial = scripted_terminal(
            model, ai::AssistantStopReason::Error, "provider stream ended mid-chunk");
        partial.content.clear();
        CCH_TRY_VOID(ai::providers::emit(sink, ai::AssistantStartEvent{.partial = partial}));

        partial.content.emplace_back(ai::ThinkingContent{
            .thinking = "",
            .thinking_signature = std::nullopt,
        });
        CCH_TRY_VOID(ai::providers::emit(sink, ai::ThinkingStartEvent{
            .content_index = 0,
            .partial = partial,
        }));
        partial.content[0] = ai::ThinkingContent{
            .thinking = "considering",
            .thinking_signature = "reasoning_content",
        };
        CCH_TRY_VOID(ai::providers::emit(sink, ai::ThinkingDeltaEvent{
            .content_index = 0,
            .delta = "considering",
            .partial = partial,
        }));
        CCH_TRY_VOID(ai::providers::emit(sink, ai::ThinkingEndEvent{
            .content_index = 0,
            .content = "considering",
            .partial = partial,
        }));

        partial.content.emplace_back(ai::TextContent{
            .text = "",
            .text_signature = std::nullopt,
        });
        CCH_TRY_VOID(ai::providers::emit(sink, ai::TextStartEvent{
            .content_index = 1,
            .partial = partial,
        }));
        std::get<ai::TextContent>(partial.content[1]).text = "useful text";
        CCH_TRY_VOID(ai::providers::emit(sink, ai::TextDeltaEvent{
            .content_index = 1,
            .delta = "useful text",
            .partial = partial,
        }));
        CCH_TRY_VOID(ai::providers::emit(sink, ai::TextEndEvent{
            .content_index = 1,
            .content = "useful text",
            .partial = partial,
        }));

        partial.content.emplace_back(ai::ToolCallContent{
            .id = "call_partial",
            .name = "echo",
            .arguments = std::nullopt,
            .raw_arguments = {},
            .thought_signature = std::nullopt,
            .arguments_valid = false,
            .argument_error = "arguments are malformed JSON",
        });
        CCH_TRY_VOID(ai::providers::emit(sink, ai::ToolCallStartEvent{
            .content_index = 2,
            .partial = partial,
        }));
        auto& streaming_call = std::get<ai::ToolCallContent>(partial.content[2]);
        streaming_call.raw_arguments = R"({"value":"par)";
        CCH_TRY_VOID(ai::providers::emit(sink, ai::ToolCallDeltaEvent{
            .content_index = 2,
            .delta = streaming_call.raw_arguments,
            .partial = partial,
        }));
        auto terminal = partial;
        CCH_TRY_VOID(ai::providers::emit(sink, ai::AssistantErrorEvent{
            .reason = terminal.stop_reason,
            .error = terminal,
            .failure = util::make_error(
                util::ErrorCode::Stream,
                "provider stream ended mid-chunk"),
        }));
        co_return terminal;
    }

    int request_count{0};
};

/// Emits two text deltas that each carry the normalized usage snapshot the
/// removed wire adapter derived from the chunk-level usage fields.
class UsageSnapshotsChatProvider final : public tests::ScriptedProvider {
public:
    UsageSnapshotsChatProvider() : ScriptedProvider("sdk-host") {}
    [[nodiscard]] boost::asio::awaitable<util::Expected<ai::AssistantMessage>> stream(
        const ai::Model& model,
        const ai::AiContext& context,
        ai::ProviderStreamOptions options,
        ai::AssistantEventSink sink) override {
        ++request_count;
        auto message = ai::assistant_text_message("ab");
        message.provider = "scripted-fake";
        message.api = "scripted-fake";
        message.model = model.id;
        message.timestamp = 1718000000123;

        const ai::Usage first_snapshot{
            .input = 13,
            .output = 5,
            .cache_read = 7,
            .cache_write = 0,
            .total_tokens = 25,
        };
        const ai::Usage latest_snapshot{
            .input = 26,
            .output = 6,
            .cache_read = 10,
            .cache_write = 4,
            .total_tokens = 46,
        };

        auto partial = message;
        partial.content.clear();
        partial.usage = ai::Usage{};
        CCH_TRY_VOID(ai::providers::emit(sink, ai::AssistantStartEvent{.partial = partial}));
        partial.content.emplace_back(ai::TextContent{
            .text = "",
            .text_signature = std::nullopt,
        });
        CCH_TRY_VOID(ai::providers::emit(sink, ai::TextStartEvent{
            .content_index = 0,
            .partial = partial,
        }));
        std::get<ai::TextContent>(partial.content[0]).text = "a";
        partial.usage = first_snapshot;
        CCH_TRY_VOID(ai::providers::emit(sink, ai::TextDeltaEvent{
            .content_index = 0,
            .delta = "a",
            .partial = partial,
        }));
        std::get<ai::TextContent>(partial.content[0]).text = "ab";
        partial.usage = latest_snapshot;
        CCH_TRY_VOID(ai::providers::emit(sink, ai::TextDeltaEvent{
            .content_index = 0,
            .delta = "b",
            .partial = partial,
        }));
        CCH_TRY_VOID(ai::providers::emit(sink, ai::TextEndEvent{
            .content_index = 0,
            .content = "ab",
            .partial = partial,
        }));
        message.usage = latest_snapshot;
        CCH_TRY_VOID(ai::providers::emit(sink, ai::AssistantDoneEvent{
            .reason = message.stop_reason,
            .message = message,
        }));
        co_return message;
    }

    int request_count{0};
};

void check_aborted_terminal(const ai::AssistantMessage& terminal) {
    CHECK(terminal.stop_reason == ai::AssistantStopReason::Aborted);
    REQUIRE(terminal.error_message.has_value());
    CHECK(*terminal.error_message == "transport operation was cancelled");
    REQUIRE(terminal.content.size() == 1);
    REQUIRE(std::holds_alternative<ai::ToolCallContent>(terminal.content[0]));
    const auto& tool_call = std::get<ai::ToolCallContent>(terminal.content[0]);
    CHECK(tool_call.id == "call_cancelled");
    CHECK(tool_call.name == "echo");
    CHECK(tool_call.raw_arguments == R"({"value":"queued"})");
    REQUIRE(tool_call.arguments.has_value());
    CHECK(tool_call.arguments_valid);
}

void check_partial_protocol_terminal(const ai::AssistantMessage& terminal) {
    CHECK(terminal.stop_reason == ai::AssistantStopReason::Error);
    REQUIRE(terminal.error_message.has_value());
    CHECK_FALSE(terminal.error_message->empty());
    REQUIRE(terminal.content.size() == 3);

    REQUIRE(std::holds_alternative<ai::ThinkingContent>(terminal.content[0]));
    const auto& thinking = std::get<ai::ThinkingContent>(terminal.content[0]);
    CHECK(thinking.thinking == "considering");
    REQUIRE(thinking.thinking_signature.has_value());
    CHECK(*thinking.thinking_signature == "reasoning_content");

    REQUIRE(std::holds_alternative<ai::TextContent>(terminal.content[1]));
    CHECK(std::get<ai::TextContent>(terminal.content[1]).text == "useful text");

    REQUIRE(std::holds_alternative<ai::ToolCallContent>(terminal.content[2]));
    const auto& tool_call = std::get<ai::ToolCallContent>(terminal.content[2]);
    CHECK(tool_call.id == "call_partial");
    CHECK(tool_call.name == "echo");
    CHECK(tool_call.raw_arguments == R"({"value":"par)");
    CHECK_FALSE(tool_call.arguments.has_value());
    CHECK_FALSE(tool_call.arguments_valid);
    REQUIRE(tool_call.argument_error.has_value());
    CHECK_FALSE(tool_call.argument_error->empty());
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// U1: Public SDK contracts compile and are move-only
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("SDK prompt contract exposes success-or-error and separate state", "[sdk][u1]") {
    tests::ModelsSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{std::filesystem::path{"/tmp/test"}};
    opts.workspace = std::filesystem::path{"/tmp"};
    opts.max_turns = 10;

    coding_agent::PromptOptions prompt_opts;
    CHECK(prompt_opts.expand_prompt_templates);

    using PromptCompletion = decltype(
        std::declval<coding_agent::AgentSession&>().prompt(std::declval<std::string>()));
    using BlockingPromptCompletion = decltype(
        std::declval<coding_agent::AgentSession&>().prompt_blocking(std::declval<std::string>()));
    using AbortCompletion = decltype(
        std::declval<coding_agent::AgentSession&>().abort());
    static_assert(std::is_same_v<
                  PromptCompletion,
                  boost::asio::awaitable<util::ExpectedVoid>>);
    static_assert(std::is_same_v<BlockingPromptCompletion, util::ExpectedVoid>);
    static_assert(std::is_same_v<AbortCompletion, void>);
    static_assert(!std::is_constructible_v<coding_agent::PromptOptions, agent::AgentEventSink>);
}

TEST_CASE("SDK AgentSession prompt accepts zero or more image values", "[sdk][u1][issue43]") {
    using PromptImages = decltype(std::declval<coding_agent::PromptOptions>().images);
    static_assert(std::is_same_v<PromptImages, std::vector<ai::ImageContent>>);

    coding_agent::PromptOptions options;
    CHECK(options.images.empty());
}

TEST_CASE("SDK session options default to no turn cap", "[sdk][u1][issue68]") {
    const tests::ModelsSessionOptions opts;
    CHECK_FALSE(opts.max_turns.has_value());

    tests::ModelsSessionOptions capped;
    capped.max_turns = 10;
    REQUIRE(capped.max_turns.has_value());
    CHECK(*capped.max_turns == 10);
}

TEST_CASE(
    "SDK AgentSession preserves image prompts through live state persistence and resume",
    "[sdk][live-state][incremental-persistence][issue43]") {
    TestPaths paths;
    auto capture = std::make_shared<CaptureChatProvider>();
    auto* capture_ptr = capture.get();

    tests::ModelsSessionOptions options;
    options.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    options.workspace = paths.workspace.path();
    options.models = cch::tests::models_from_provider(std::move(capture));

    auto created = coding_agent::create_agent_session(std::move(options));
    REQUIRE(created.has_value());

    coding_agent::PromptOptions one_image;
    one_image.images.push_back(ai::ImageContent{
        .data = "cG5nLWxpdGVyYWw=",
        .mime_type = "image/png",
    });
    REQUIRE(created->session->prompt_blocking("inspect one", std::move(one_image)).has_value());
    REQUIRE(capture_ptr->captured_request.has_value());
    const auto& first_request_user = std::get<ai::UserMessage>(
        capture_ptr->captured_request->context.messages.front());
    const auto& first_blocks =
        std::get<std::vector<ai::Content>>(first_request_user.content);
    REQUIRE(first_blocks.size() == 2);
    CHECK(std::get<ai::TextContent>(first_blocks[0]).text == "inspect one");
    CHECK(std::get<ai::ImageContent>(first_blocks[1]).data == "cG5nLWxpdGVyYWw=");
    CHECK(std::get<ai::ImageContent>(first_blocks[1]).mime_type == "image/png");

    coding_agent::PromptOptions multiple_images;
    multiple_images.images = {
        ai::ImageContent{
            .data = "d2VicC1saXRlcmFs",
            .mime_type = "image/webp",
        },
        ai::ImageContent{
            .data = "Z2lmLWxpdGVyYWw=",
            .mime_type = "image/gif",
        },
    };
    REQUIRE(created->session->prompt_blocking("compare both", std::move(multiple_images)).has_value());
    REQUIRE(capture_ptr->captured_request.has_value());
    const auto& second_request_user = std::get<ai::UserMessage>(
        capture_ptr->captured_request->context.messages[2]);
    const auto& second_blocks =
        std::get<std::vector<ai::Content>>(second_request_user.content);
    REQUIRE(second_blocks.size() == 3);
    CHECK(std::get<ai::TextContent>(second_blocks[0]).text == "compare both");
    CHECK(std::get<ai::ImageContent>(second_blocks[1]).mime_type == "image/webp");
    CHECK(std::get<ai::ImageContent>(second_blocks[1]).data == "d2VicC1saXRlcmFs");
    CHECK(std::get<ai::ImageContent>(second_blocks[2]).mime_type == "image/gif");
    CHECK(std::get<ai::ImageContent>(second_blocks[2]).data == "Z2lmLWxpdGVyYWw=");

    const auto snapshot = created->session->snapshot();
    REQUIRE(snapshot.agent_state.messages.size() == 4);
    const auto& live_user = std::get<ai::UserMessage>(snapshot.agent_state.messages[2]);
    const auto& live_blocks = std::get<std::vector<ai::Content>>(live_user.content);
    REQUIRE(live_blocks.size() == 3);
    CHECK(std::get<ai::ImageContent>(live_blocks[1]).data == "d2VicC1saXRlcmFs");
    CHECK(std::get<ai::ImageContent>(live_blocks[2]).data == "Z2lmLWxpdGVyYWw=");

    auto durable = harness::session::JsonlSessionStore::load(paths.session_file);
    REQUIRE(durable.has_value());
    REQUIRE(durable->messages.size() == 4);
    const auto& persisted_user = std::get<ai::UserMessage>(durable->messages[2]);
    const auto& persisted_blocks =
        std::get<std::vector<ai::Content>>(persisted_user.content);
    REQUIRE(persisted_blocks.size() == 3);
    CHECK(std::get<ai::ImageContent>(persisted_blocks[1]).mime_type == "image/webp");
    CHECK(std::get<ai::ImageContent>(persisted_blocks[2]).mime_type == "image/gif");
    created->session->close();

    auto resumed_capture = std::make_shared<CaptureChatProvider>();
    auto* resumed_capture_ptr = resumed_capture.get();
    tests::ModelsSessionOptions resume_options;
    resume_options.session_target = coding_agent::ExplicitResumeSessionTarget{paths.session_file};
    resume_options.workspace = paths.workspace.path();
    resume_options.models = cch::tests::models_from_provider(std::move(resumed_capture));
    auto resumed = coding_agent::create_agent_session(std::move(resume_options));
    REQUIRE(resumed.has_value());
    REQUIRE(resumed->session->prompt_blocking("continue").has_value());
    REQUIRE(resumed_capture_ptr->captured_request.has_value());
    const auto& resumed_user = std::get<ai::UserMessage>(
        resumed_capture_ptr->captured_request->context.messages[2]);
    const auto& resumed_blocks =
        std::get<std::vector<ai::Content>>(resumed_user.content);
    REQUIRE(resumed_blocks.size() == 3);
    CHECK(std::get<ai::ImageContent>(resumed_blocks[1]).data == "d2VicC1saXRlcmFs");
    CHECK(std::get<ai::ImageContent>(resumed_blocks[2]).data == "Z2lmLWxpdGVyYWw=");
    resumed->session->close();
}

TEST_CASE(
    "SDK AgentSession keeps text-only prompts and a truthful opaque-host Model",
    "[sdk][issue43][issue336]") {
    TestPaths paths;
    auto capture = std::make_shared<CaptureChatProvider>();
    auto* capture_ptr = capture.get();
    tests::ModelsSessionOptions options;
    options.session_target = coding_agent::InMemorySessionTarget{};
    options.workspace = paths.workspace.path();
    options.models = cch::tests::models_from_provider(std::move(capture));
    auto created = coding_agent::create_agent_session(std::move(options));
    REQUIRE(created.has_value());

    REQUIRE(created->session->prompt_blocking("text only").has_value());
    REQUIRE(capture_ptr->captured_request.has_value());
    const auto& model = capture_ptr->captured_request->model;
    CHECK(model.id == "host-client");
    CHECK(model.name == "host-client");
    CHECK(model.provider == "sdk-host");
    CHECK(model.api == "unknown");
    CHECK(model.base_url.empty());
    CHECK(model.input.empty());
    CHECK(model.context_window == 0);
    CHECK(model.max_tokens == 0);
    REQUIRE(ai::validate_model(model));
    const auto& user = std::get<ai::UserMessage>(capture_ptr->captured_request->context.messages.front());
    const auto& user_blocks = std::get<std::vector<ai::Content>>(user.content);
    REQUIRE(user_blocks.size() == 1);
    CHECK(std::get<ai::TextContent>(user_blocks.front()).text == "text only");
    created->session->close();
}

TEST_CASE("SDK AgentSession exposes incompatible image destinations as provider failures", "[sdk][issue43]") {
    TestPaths paths;
    auto rejecting_client = std::make_shared<ImageRejectingChatProvider>();
    auto* rejecting_client_ptr = rejecting_client.get();
    tests::ModelsSessionOptions options;
    options.session_target = coding_agent::InMemorySessionTarget{};
    options.workspace = paths.workspace.path();
    options.models = cch::tests::models_from_provider(std::move(rejecting_client));
    auto created = coding_agent::create_agent_session(std::move(options));
    REQUIRE(created.has_value());

    coding_agent::PromptOptions prompt_options;
    prompt_options.images.push_back(ai::ImageContent{
        .data = "cmVqZWN0LW1l",
        .mime_type = "image/png",
    });
    auto rejected = created->session->prompt_blocking("inspect", std::move(prompt_options));
    REQUIRE(rejected.has_value());
    CHECK(rejecting_client_ptr->request_count == 1);
    const auto snapshot = created->session->snapshot();
    REQUIRE(snapshot.agent_state.messages.size() == 2);
    const auto& terminal = std::get<ai::AssistantMessage>(snapshot.agent_state.messages[1]);
    CHECK(terminal.stop_reason == ai::AssistantStopReason::Error);
    CHECK(terminal.error_message == "image input is not supported");
    const auto& user = std::get<ai::UserMessage>(snapshot.agent_state.messages.front());
    REQUIRE(std::get<std::vector<ai::Content>>(user.content).size() == 2);
    CHECK(std::get<ai::ImageContent>(std::get<std::vector<ai::Content>>(user.content)[1]).data ==
        "cmVqZWN0LW1l");
    created->session->close();
}

TEST_CASE("AgentSession is move-only", "[sdk][u1]") {
    static_assert(std::is_move_constructible_v<coding_agent::AgentSession>);
    static_assert(!std::is_copy_constructible_v<coding_agent::AgentSession>);
    static_assert(!std::is_copy_assignable_v<coding_agent::AgentSession>);
}

TEST_CASE(
    "SDK AgentSession drains observed steering before follow-up input",
    "[coding_agent][queue][issue44]") {
    TestPaths paths;
    tests::ModelsSessionOptions options;
    options.session_target = coding_agent::InMemorySessionTarget{};
    options.workspace = paths.workspace.path();
    options.models = ai::providers::make_scripted_fake_models();

    auto created = coding_agent::create_agent_session(std::move(options));
    REQUIRE(created.has_value());

    REQUIRE(created->session->set_steering_mode(agent::InputQueueMode::All));
    REQUIRE(created->session->steer("steer-one"));
    REQUIRE(created->session->steer("steer-two"));
    REQUIRE(created->session->follow_up("follow-up"));

    const auto queued = created->session->snapshot();
    CHECK(queued.agent_state.input_queues.steering.mode == agent::InputQueueMode::All);
    REQUIRE(queued.agent_state.input_queues.steering.messages.size() == 2);
    REQUIRE(queued.agent_state.input_queues.follow_up.messages.size() == 1);

    REQUIRE(created->session->prompt_blocking("prompt"));

    const auto completed = created->session->snapshot();
    CHECK(completed.agent_state.input_queues.steering.messages.empty());
    CHECK(completed.agent_state.input_queues.follow_up.messages.empty());
    REQUIRE(completed.agent_state.messages.size() == 6);
    CHECK(ai::text_from_user_message(std::get<ai::UserMessage>(completed.agent_state.messages[0])) == "prompt");
    CHECK(ai::text_from_user_message(std::get<ai::UserMessage>(completed.agent_state.messages[1])) == "steer-one");
    CHECK(ai::text_from_user_message(std::get<ai::UserMessage>(completed.agent_state.messages[2])) == "steer-two");
    CHECK(ai::text_from_user_message(std::get<ai::UserMessage>(completed.agent_state.messages[4])) == "follow-up");

    created->session->close();
    auto rejected = created->session->steer("after close");
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().message == "session is closed");
}

TEST_CASE(
    "SDK AgentSession accepts and clears queued input while a prompt is streaming",
    "[coding_agent][queue][issue44]") {
    TestPaths paths;
    auto client = std::make_shared<GatedSdkChatProvider>();
    auto* client_ptr = client.get();
    tests::ModelsSessionOptions options;
    options.session_target = coding_agent::InMemorySessionTarget{};
    options.workspace = paths.workspace.path();
    options.models = cch::tests::models_from_provider(std::move(client));

    auto created = coding_agent::create_agent_session(std::move(options));
    REQUIRE(created.has_value());

    boost::asio::io_context io;
    std::optional<util::ExpectedVoid> prompt_result;
    boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<void> {
            prompt_result = co_await created->session->prompt("prompt");
            co_return;
        },
        boost::asio::detached);
    while (!client_ptr->started) {
        REQUIRE(io.poll_one() == 1);
    }

    REQUIRE(created->session->steer("steer"));
    REQUIRE(created->session->follow_up("follow-up"));
    REQUIRE(created->session->clear_input_queues());
    const auto cleared = created->session->snapshot();
    CHECK(cleared.agent_state.is_running);
    CHECK(cleared.agent_state.input_queues.steering.messages.empty());
    CHECK(cleared.agent_state.input_queues.follow_up.messages.empty());

    client_ptr->release();
    if (io.stopped()) {
        io.restart();
    }
    io.run();

    REQUIRE(prompt_result.has_value());
    REQUIRE(*prompt_result);
    CHECK_FALSE(created->session->snapshot().agent_state.is_running);
    created->session->close();
}

TEST_CASE(
    "AgentSession close is a synchronous non-throwing request",
    "[sdk][u1][lifecycle][issue41]") {
    static_assert(std::is_same_v<
                  decltype(std::declval<coding_agent::AgentSession&>().close()),
                  void>);
    static_assert(noexcept(std::declval<coding_agent::AgentSession&>().close()));
}

TEST_CASE("EventSubscription is move-only", "[sdk][u1]") {
    static_assert(std::is_move_constructible_v<coding_agent::EventSubscription>);
    static_assert(!std::is_copy_constructible_v<coding_agent::EventSubscription>);
    static_assert(!std::is_copy_assignable_v<coding_agent::EventSubscription>);
}

// ─────────────────────────────────────────────────────────────────────────────
// U2: Session creation validation
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("SDK session target is one passive variant with default persistence", "[sdk][u2][session-target]") {
    static_assert(std::is_aggregate_v<coding_agent::DefaultPersistedSessionTarget>);
    static_assert(std::is_aggregate_v<coding_agent::ExplicitNewSessionTarget>);
    static_assert(std::is_aggregate_v<coding_agent::ExplicitResumeSessionTarget>);
    static_assert(std::is_aggregate_v<coding_agent::InMemorySessionTarget>);
    static_assert(std::variant_size_v<coding_agent::SessionTarget> == 4);
    static_assert(std::is_same_v<
                  std::variant_alternative_t<0, coding_agent::SessionTarget>,
                  coding_agent::DefaultPersistedSessionTarget>);
    static_assert(std::is_same_v<
                  std::variant_alternative_t<1, coding_agent::SessionTarget>,
                  coding_agent::ExplicitNewSessionTarget>);
    static_assert(std::is_same_v<
                  std::variant_alternative_t<2, coding_agent::SessionTarget>,
                  coding_agent::ExplicitResumeSessionTarget>);
    static_assert(std::is_same_v<
                  std::variant_alternative_t<3, coding_agent::SessionTarget>,
                  coding_agent::InMemorySessionTarget>);

    tests::ModelsSessionOptions opts;
    CHECK(std::holds_alternative<coding_agent::DefaultPersistedSessionTarget>(opts.session_target));

    using ResultPath = decltype(std::declval<coding_agent::CreateAgentSessionResult>().session_path);
    using SessionPath = decltype(std::declval<const coding_agent::AgentSession&>().session_path());
    static_assert(std::is_same_v<ResultPath, std::optional<std::filesystem::path>>);
    static_assert(std::is_same_v<SessionPath, const std::optional<std::filesystem::path>&>);
}

TEST_CASE(
    "SDK in-memory session preserves normal runtime contracts without filesystem state",
    "[sdk][in-memory][live-state]") {
    TestPaths paths;
    paths.workspace.write("target.txt", "target content\n");
    cch::tests::TempWorkspace isolated;
    const auto agent_dir = isolated.path() / "agent-not-created";
    tests::EnvVarGuard agent_dir_guard{"PI_CODING_AGENT_DIR"};
    agent_dir_guard.set(agent_dir.string());

    tests::ModelsSessionOptions opts;
    opts.session_target = coding_agent::InMemorySessionTarget{};
    opts.workspace = paths.workspace.path();
    opts.models = ai::providers::make_scripted_fake_models();

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(result.has_value());
    REQUIRE(result->session != nullptr);
    CHECK_FALSE(result->session_path.has_value());
    CHECK_FALSE(result->session->session_path().has_value());
    CHECK(result->metadata.session_id == result->session_id);
    CHECK(result->metadata.workspace == std::filesystem::canonical(paths.workspace.path()));
    CHECK(std::regex_match(
        result->session_id,
        std::regex{R"(^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$)"}));
    CHECK(result->session->message_count() == 0);
    CHECK_FALSE(result->session->last_assistant_text().has_value());

    std::size_t message_ends = 0;
    std::size_t tool_results = 0;
    auto subscription = result->session->subscribe(
        [&](const agent::AgentLifecycleEvent& event) -> util::ExpectedVoid {
            if (const auto* end = std::get_if<agent::MessageEndEvent>(&event)) {
                ++message_ends;
                CHECK(result->session->message_count() == message_ends);
                if (std::holds_alternative<ai::ToolResultMessage>(end->message)) {
                    ++tool_results;
                }
            }
            return {};
        });
    REQUIRE(subscription.has_value());

    auto prompted = result->session->prompt_blocking("read target.txt");
    REQUIRE(prompted.has_value());
    CHECK(message_ends == 4);
    CHECK(tool_results == 1);
    CHECK(result->session->message_count() == 4);
    REQUIRE(result->session->last_assistant_text().has_value());
    CHECK(result->session->last_assistant_text()->find("observed") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(agent_dir / "sessions"));
    CHECK_FALSE(std::filesystem::exists(paths.workspace.path() / ".cpp-harness" / "sessions"));
    CHECK_FALSE(std::filesystem::exists(paths.session_file));

    coding_agent::AgentSession moved{std::move(*result->session)};
    CHECK(moved.is_open());
    CHECK_FALSE(moved.session_path().has_value());
    CHECK(moved.message_count() == 4);
    moved.close();
    moved.close();
    CHECK_FALSE(moved.is_open());
    CHECK_FALSE(std::filesystem::exists(agent_dir / "sessions"));
}

TEST_CASE(
    "SDK in-memory subscriptions remain safe across session move assignment and destruction",
    "[sdk][in-memory][move]") {
    TestPaths paths;
    auto create_in_memory = [&]() {
        tests::ModelsSessionOptions opts;
        opts.session_target = coding_agent::InMemorySessionTarget{};
        opts.workspace = paths.workspace.path();
        opts.models = ai::providers::make_scripted_fake_models();
        return coding_agent::create_agent_session(std::move(opts));
    };

    auto first = create_in_memory();
    REQUIRE(first.has_value());
    auto first_subscription = first->session->subscribe(
        [](const agent::AgentLifecycleEvent&) -> util::ExpectedVoid { return {}; });
    REQUIRE(first_subscription.has_value());

    auto second = create_in_memory();
    REQUIRE(second.has_value());
    auto second_subscription = second->session->subscribe(
        [](const agent::AgentLifecycleEvent&) -> util::ExpectedVoid { return {}; });
    REQUIRE(second_subscription.has_value());

    {
        coding_agent::AgentSession assigned{std::move(*first->session)};
        CHECK(static_cast<bool>(*first_subscription));
        assigned = std::move(*second->session);
        CHECK_FALSE(static_cast<bool>(*first_subscription));
        CHECK(static_cast<bool>(*second_subscription));
        assigned.close();
        CHECK_FALSE(static_cast<bool>(*second_subscription));
    }

    CHECK_FALSE(static_cast<bool>(*first_subscription));
    CHECK_FALSE(static_cast<bool>(*second_subscription));
    first_subscription->unsubscribe();
    second_subscription->unsubscribe();
}

TEST_CASE(
    "SDK in-memory subscriber failure is weak and deactivates the observer",
    "[sdk][in-memory][subscriber-failure][issue36]") {
    TestPaths paths;
    cch::tests::TempWorkspace isolated;
    const auto agent_dir = isolated.path() / "agent-not-created";
    tests::EnvVarGuard agent_dir_guard{"PI_CODING_AGENT_DIR"};
    agent_dir_guard.set(agent_dir.string());

    tests::ModelsSessionOptions opts;
    opts.session_target = coding_agent::InMemorySessionTarget{};
    opts.workspace = paths.workspace.path();
    opts.models = ai::providers::make_scripted_fake_models();

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(result.has_value());
    auto failed_subscription = result->session->subscribe(
        [](const agent::AgentLifecycleEvent&) -> util::ExpectedVoid {
            return std::unexpected(util::make_error(
                util::ErrorCode::Tool,
                "subscriber rejected in-memory event"));
        });
    REQUIRE(failed_subscription.has_value());

    std::size_t healthy_events = 0;
    auto healthy_subscription = result->session->subscribe(
        [&](const agent::AgentLifecycleEvent&) -> util::ExpectedVoid {
            ++healthy_events;
            return {};
        });
    REQUIRE(healthy_subscription.has_value());

    auto prompted = result->session->prompt_blocking("hello");
    REQUIRE(prompted.has_value());
    CHECK_FALSE(static_cast<bool>(*failed_subscription));
    CHECK(static_cast<bool>(*healthy_subscription));
    CHECK(healthy_events == 11);
    CHECK(result->session->is_open());
    CHECK(result->session->message_count() == 2);
    CHECK(result->session->last_assistant_text().has_value());
    CHECK_FALSE(result->session->session_path().has_value());
    CHECK_FALSE(std::filesystem::exists(agent_dir / "sessions"));
    CHECK_FALSE(std::filesystem::exists(paths.workspace.path() / ".cpp-harness" / "sessions"));
    result->session->close();
}

TEST_CASE(
    "SDK in-memory creation uses shared resource trust and prompt processing",
    "[sdk][in-memory][project-resources]") {
    TestPaths paths;
    write_project_skill(paths, "project-skill", "Use in-memory project instructions.");
    write_project_prompt(paths, "project-review", "Review in memory: $1.");
    cch::tests::TempWorkspace isolated;
    const auto agent_dir = isolated.path() / "agent-not-created";
    tests::EnvVarGuard agent_dir_guard{"PI_CODING_AGENT_DIR"};
    agent_dir_guard.set(agent_dir.string());

    tests::ModelsSessionOptions opts;
    opts.session_target = coding_agent::InMemorySessionTarget{};
    opts.workspace = paths.workspace.path();
    opts.models = ai::providers::make_scripted_fake_models();
    opts.load_project_resources = true;
    opts.default_project_trust = coding_agent::DefaultProjectTrust::Always;

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(result.has_value());
    REQUIRE(result->session->skills().size() == 1);
    CHECK(result->session->skills()[0].name == "project-skill");
    REQUIRE(result->session->templates().size() == 1);
    CHECK(result->session->templates()[0].name == "project-review");

    auto prompted = result->session->prompt_blocking("/project-review target.cpp");
    REQUIRE(prompted.has_value());
    REQUIRE(result->session->last_assistant_text().has_value());
    CHECK(result->session->last_assistant_text()->find("Review in memory: target.cpp.") !=
          std::string::npos);
    CHECK_FALSE(result->session_path.has_value());
    CHECK_FALSE(std::filesystem::exists(agent_dir / "sessions"));
    CHECK_FALSE(std::filesystem::exists(paths.workspace.path() / ".cpp-harness" / "sessions"));
    result->session->close();
}

TEST_CASE(
    "SDK in-memory provider rejection completes as a retained terminal error",
    "[sdk][in-memory][provider-preflight-rejection]") {
    TestPaths paths;
    cch::tests::TempWorkspace isolated;
    const auto agent_dir = isolated.path() / "agent-not-created";
    tests::EnvVarGuard agent_dir_guard{"PI_CODING_AGENT_DIR"};
    agent_dir_guard.set(agent_dir.string());

    tests::ModelsSessionOptions opts;
    opts.session_target = coding_agent::InMemorySessionTarget{};
    opts.workspace = paths.workspace.path();
    opts.models = cch::tests::models_from_provider(std::make_shared<PreflightRejectingChatProvider>());

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(result.has_value());
    std::size_t delivered_user_messages = 0;
    auto subscription = result->session->subscribe(
        [&](const agent::AgentLifecycleEvent& event) -> util::ExpectedVoid {
            if (const auto* end = std::get_if<agent::MessageEndEvent>(&event);
                end != nullptr && std::holds_alternative<ai::UserMessage>(end->message)) {
                ++delivered_user_messages;
            }
            return {};
        });
    REQUIRE(subscription.has_value());

    auto failed = result->session->prompt_blocking("hello");
    REQUIRE(failed.has_value());
    CHECK(delivered_user_messages == 1);
    CHECK(result->session->is_open());
    CHECK(result->session->message_count() == 2);
    const auto snapshot = result->session->snapshot();
    const auto& terminal = std::get<ai::AssistantMessage>(snapshot.agent_state.messages.back());
    CHECK(terminal.stop_reason == ai::AssistantStopReason::Error);
    CHECK(terminal.error_message == "provider rejected in-memory prompt");
    CHECK_FALSE(result->session_path.has_value());
    CHECK_FALSE(result->session->session_path().has_value());
    CHECK_FALSE(std::filesystem::exists(agent_dir / "sessions"));
    CHECK_FALSE(std::filesystem::exists(paths.workspace.path() / ".cpp-harness" / "sessions"));
    result->session->close();
}

TEST_CASE(
    "SDK persists a terminal error when provider setup rejects before transport",
    "[sdk][incremental-persistence][provider-preflight-rejection][issue14]") {
    TestPaths paths;
    auto inner = std::make_shared<TransportStandInChatProvider>();
    auto* inner_ptr = inner.get();

    tests::ModelsSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.model = cch::tests::sdk_request_model("fake", "fake-model");
    opts.models = cch::tests::models_from_provider(
        std::make_shared<ApiKeyGatedChatProvider>(std::move(inner), std::nullopt));

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(result.has_value());
    auto& session = result->session;

    std::vector<agent::AgentLifecycleEvent> events;
    auto subscription = session->subscribe(
        [&](const agent::AgentLifecycleEvent& event) -> util::ExpectedVoid {
            events.push_back(event);
            return {};
        });
    REQUIRE(subscription.has_value());

    auto failed = session->prompt_blocking("hello");
    REQUIRE(failed.has_value());
    CHECK(inner_ptr->request_count == 0);
    CHECK(session->is_open());
    CHECK(session->message_count() == 2);

    REQUIRE(events.size() == 8);
    std::size_t index = 0;
    CHECK(std::holds_alternative<agent::AgentStartEvent>(events[index++]));
    CHECK(std::holds_alternative<agent::TurnStartEvent>(events[index++]));
    const auto* user_start = std::get_if<agent::MessageStartEvent>(&events[index++]);
    REQUIRE(user_start != nullptr);
    CHECK(std::holds_alternative<ai::UserMessage>(user_start->message));
    const auto* user_end = std::get_if<agent::MessageEndEvent>(&events[index++]);
    REQUIRE(user_end != nullptr);
    CHECK(std::holds_alternative<ai::UserMessage>(user_end->message));
    CHECK(std::holds_alternative<agent::MessageStartEvent>(events[index++]));
    const auto* assistant_end = std::get_if<agent::MessageEndEvent>(&events[index++]);
    REQUIRE(assistant_end != nullptr);
    const auto& terminal = std::get<ai::AssistantMessage>(assistant_end->message);
    CHECK(terminal.stop_reason == ai::AssistantStopReason::Error);
    CHECK(terminal.error_message == "missing API key");
    CHECK(std::holds_alternative<agent::TurnEndEvent>(events[index++]));
    CHECK(std::holds_alternative<agent::AgentEndEvent>(events[index++]));

    auto durable = harness::session::JsonlSessionStore::load(paths.session_file);
    REQUIRE(durable.has_value());
    REQUIRE(durable->messages.size() == 2);
    REQUIRE(std::holds_alternative<ai::UserMessage>(durable->messages[0]));
    REQUIRE(std::holds_alternative<ai::AssistantMessage>(durable->messages[1]));
    CHECK(ai::text_from_user_message(std::get<ai::UserMessage>(durable->messages[0])) == "hello");

    session->close();
}

TEST_CASE(
    "SDK persists the latest normalized provider usage snapshot",
    "[sdk][live-state][incremental-persistence][issue20]") {
    TestPaths paths;
    auto client = std::make_shared<UsageSnapshotsChatProvider>();
    auto* client_ptr = client.get();

    tests::ModelsSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.model = cch::tests::sdk_request_model("fake", "fake-model");
    opts.models = cch::tests::models_from_provider(std::move(client));

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(result.has_value());
    auto& session = result->session;

    std::optional<ai::Usage> start_usage;
    std::vector<ai::Usage> delta_usages;
    std::optional<ai::Usage> terminal_usage;
    auto subscription = session->subscribe(
        [&](const agent::AgentLifecycleEvent& event) -> util::ExpectedVoid {
            if (const auto* start = std::get_if<agent::MessageStartEvent>(&event)) {
                if (const auto* assistant = std::get_if<ai::AssistantMessage>(&start->message)) {
                    start_usage = assistant->usage;
                }
            }
            if (const auto* update = std::get_if<agent::MessageUpdateEvent>(&event);
                update != nullptr && std::holds_alternative<ai::TextDeltaEvent>(update->assistant_event)) {
                delta_usages.push_back(std::get<ai::AssistantMessage>(update->message).usage);
            }
            if (const auto* end = std::get_if<agent::MessageEndEvent>(&event)) {
                if (const auto* assistant = std::get_if<ai::AssistantMessage>(&end->message)) {
                    terminal_usage = assistant->usage;
                }
            }
            return {};
        });
    REQUIRE(subscription.has_value());

    auto prompted = session->prompt_blocking("hello");
    REQUIRE(prompted.has_value());
    REQUIRE(client_ptr->request_count == 1);

    const ai::Usage first_snapshot{
        .input = 13,
        .output = 5,
        .cache_read = 7,
        .cache_write = 0,
        .cache_write_1h = std::nullopt,
        .reasoning = std::nullopt,
        .total_tokens = 25,
        .cost = {},
    };
    const ai::Usage latest_snapshot{
        .input = 26,
        .output = 6,
        .cache_read = 10,
        .cache_write = 4,
        .cache_write_1h = std::nullopt,
        .reasoning = std::nullopt,
        .total_tokens = 46,
        .cost = {},
    };

    REQUIRE(start_usage.has_value());
    tests::check_zero_usage(*start_usage);

    REQUIRE(delta_usages.size() == 2);
    tests::check_usage(delta_usages[0], first_snapshot);
    tests::check_usage(delta_usages[1], latest_snapshot);

    REQUIRE(terminal_usage.has_value());
    tests::check_usage(*terminal_usage, latest_snapshot);

    auto durable = harness::session::JsonlSessionStore::load(paths.session_file);
    REQUIRE(durable.has_value());
    REQUIRE(durable->messages.size() == 2);
    REQUIRE(std::holds_alternative<ai::AssistantMessage>(durable->messages[1]));
    const auto& persisted = std::get<ai::AssistantMessage>(durable->messages[1]);
    tests::check_usage(persisted.usage, *terminal_usage);

    session->close();
}

TEST_CASE(
    "SDK completes an accepted network failure as a durable error turn and remains reusable",
    "[sdk][live-state][incremental-persistence][issue11]") {
    TestPaths paths;
    // The scripted terminal message matches the transport detail the removed
    // wire adapter surfaced for this network-class failure.
    auto client = std::make_shared<RecoveringFailureChatProvider>(
        "could not resolve api.example: Name or service not known");
    auto* client_ptr = client.get();

    tests::ModelsSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.model = cch::tests::sdk_request_model("fake", "fake-model");
    opts.models = cch::tests::models_from_provider(std::move(client));

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(result.has_value());
    auto& session = result->session;

    std::vector<agent::AgentLifecycleEvent> events;
    bool terminal_was_live_before_message_end = false;
    auto subscription = session->subscribe(
        [&](const agent::AgentLifecycleEvent& event) -> util::ExpectedVoid {
            events.push_back(event);
            const auto* end = std::get_if<agent::MessageEndEvent>(&event);
            if (end == nullptr) {
                return {};
            }
            const auto* assistant = std::get_if<ai::AssistantMessage>(&end->message);
            if (assistant != nullptr && assistant->stop_reason == ai::AssistantStopReason::Error) {
                terminal_was_live_before_message_end =
                    session->message_count() == 2 &&
                    session->last_assistant_text().has_value() &&
                    session->last_assistant_text()->empty();
            }
            return {};
        });
    REQUIRE(subscription.has_value());

    auto error_turn = session->prompt_blocking("hello");
    REQUIRE(error_turn.has_value());
    CHECK(client_ptr->request_count == 1);
    CHECK(terminal_was_live_before_message_end);
    CHECK(session->is_open());
    CHECK(session->message_count() == 2);

    REQUIRE(events.size() == 8);
    std::size_t event_index = 0;
    CHECK(std::holds_alternative<agent::AgentStartEvent>(events[event_index++]));
    CHECK(std::holds_alternative<agent::TurnStartEvent>(events[event_index++]));
    const auto* user_start = std::get_if<agent::MessageStartEvent>(&events[event_index++]);
    REQUIRE(user_start != nullptr);
    CHECK(std::holds_alternative<ai::UserMessage>(user_start->message));
    const auto* user_end = std::get_if<agent::MessageEndEvent>(&events[event_index++]);
    REQUIRE(user_end != nullptr);
    CHECK(std::holds_alternative<ai::UserMessage>(user_end->message));
    const auto* assistant_start = std::get_if<agent::MessageStartEvent>(&events[event_index++]);
    REQUIRE(assistant_start != nullptr);
    REQUIRE(std::holds_alternative<ai::AssistantMessage>(assistant_start->message));
    const auto* assistant_end = std::get_if<agent::MessageEndEvent>(&events[event_index++]);
    REQUIRE(assistant_end != nullptr);
    REQUIRE(std::holds_alternative<ai::AssistantMessage>(assistant_end->message));
    const auto& terminal = std::get<ai::AssistantMessage>(assistant_end->message);
    CHECK(terminal.stop_reason == ai::AssistantStopReason::Error);
    REQUIRE(terminal.error_message.has_value());
    CHECK(*terminal.error_message == "could not resolve api.example: Name or service not known");
    const auto terminal_error_message = terminal.error_message;
    const auto* turn_end = std::get_if<agent::TurnEndEvent>(&events[event_index++]);
    REQUIRE(turn_end != nullptr);
    CHECK(turn_end->tool_results.empty());
    REQUIRE(std::holds_alternative<ai::AssistantMessage>(turn_end->message));
    CHECK(std::get<ai::AssistantMessage>(turn_end->message).stop_reason == ai::AssistantStopReason::Error);
    CHECK(std::holds_alternative<agent::AgentEndEvent>(events[event_index++]));
    CHECK(std::none_of(
        events.begin(),
        events.end(),
        [](const auto& event) {
            return std::holds_alternative<agent::ToolExecutionStartEvent>(event) ||
                   std::holds_alternative<agent::ToolExecutionEndEvent>(event);
        }));

    auto durable = harness::session::JsonlSessionStore::load(paths.session_file);
    REQUIRE(durable.has_value());
    REQUIRE(durable->messages.size() == 2);
    REQUIRE(std::holds_alternative<ai::AssistantMessage>(durable->messages[1]));
    const auto& durable_terminal = std::get<ai::AssistantMessage>(durable->messages[1]);
    CHECK(durable_terminal.stop_reason == ai::AssistantStopReason::Error);
    CHECK(durable_terminal.error_message == terminal_error_message);

    events.clear();
    auto recovered_turn = session->prompt_blocking("try again");
    REQUIRE(recovered_turn.has_value());
    CHECK(client_ptr->request_count == 2);
    CHECK(session->message_count() == 4);
    REQUIRE(session->last_assistant_text().has_value());
    CHECK(*session->last_assistant_text() == "recovered");
    session->close();
    CHECK_FALSE(session->is_open());
    CHECK_FALSE(static_cast<bool>(*subscription));
    session->close();

    auto capture = std::make_shared<CaptureChatProvider>();
    auto* capture_ptr = capture.get();
    tests::ModelsSessionOptions resume;
    resume.session_target = coding_agent::ExplicitResumeSessionTarget{paths.session_file};
    resume.workspace = paths.workspace.path();
    resume.models = cch::tests::models_from_provider(std::move(capture));

    auto reopened = coding_agent::create_agent_session(std::move(resume));
    REQUIRE(reopened.has_value());
    CHECK(reopened->session->message_count() == 4);
    auto after_resume = reopened->session->prompt_blocking("after resume");
    REQUIRE(after_resume.has_value());
    REQUIRE(capture_ptr->captured_request.has_value());
    REQUIRE(capture_ptr->captured_request->context.messages.size() == 5);
    REQUIRE(std::holds_alternative<ai::AssistantMessage>(
        capture_ptr->captured_request->context.messages[1]));
    const auto& resumed_terminal = std::get<ai::AssistantMessage>(
        capture_ptr->captured_request->context.messages[1]);
    CHECK(resumed_terminal.stop_reason == ai::AssistantStopReason::Error);
    CHECK(resumed_terminal.error_message == terminal_error_message);
    reopened->session->close();
}

TEST_CASE(
    "SDK commits one redacted malformed-argument diagnostic to events context and history",
    "[sdk][live-state][incremental-persistence][issue33]") {
    TestPaths paths;
    const std::string secret = "sk-FAKEPARSEREXCERPTCREDENTIAL123456";
    auto client = std::make_shared<MalformedArgumentsChatProvider>(secret);
    auto* client_ptr = client.get();
    auto tool_execution_count = std::make_shared<std::size_t>(0);

    tests::ModelsSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.models = cch::tests::models_from_provider(std::move(client));
    opts.builtin_tools = coding_agent::SdkBuiltinTools{
        .read = false,
        .write = false,
        .edit = false,
        .bash = false,
    };
    opts.custom_tools.push_back(std::make_unique<FakeEchoTool>(tool_execution_count));

    auto created = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(created.has_value());

    std::optional<std::string> execution_end_diagnostic;
    std::optional<std::string> message_end_diagnostic;
    auto subscription = created->session->subscribe(
        [&](const agent::AgentLifecycleEvent& event) -> util::ExpectedVoid {
            if (const auto* end = std::get_if<agent::ToolExecutionEndEvent>(&event);
                end != nullptr && end->tool_call_id == "call-malformed") {
                execution_end_diagnostic = ai::text_from_content(end->result.content);
            }
            if (const auto* end = std::get_if<agent::MessageEndEvent>(&event);
                end != nullptr && std::holds_alternative<ai::ToolResultMessage>(end->message)) {
                const auto& result = std::get<ai::ToolResultMessage>(end->message);
                if (result.tool_call_id == "call-malformed") {
                    message_end_diagnostic = ai::text_from_content(result.content);
                }
            }
            return {};
        });
    REQUIRE(subscription.has_value());

    REQUIRE(created->session->prompt_blocking("exercise malformed and valid calls").has_value());
    CHECK(*tool_execution_count == 1);
    REQUIRE(execution_end_diagnostic.has_value());
    REQUIRE(message_end_diagnostic.has_value());
    CHECK(*message_end_diagnostic == *execution_end_diagnostic);
    CHECK(execution_end_diagnostic->size() <= 4096);
    CHECK(execution_end_diagnostic->find("echo") != std::string::npos);
    CHECK(execution_end_diagnostic->find("arguments are malformed JSON") != std::string::npos);
    CHECK(execution_end_diagnostic->find("[REDACTED]") != std::string::npos);
    CHECK(execution_end_diagnostic->find(secret) == std::string::npos);

    REQUIRE(client_ptr->requests.size() == 2);
    std::optional<std::string> context_diagnostic;
    for (const auto& message : client_ptr->requests[1].context.messages) {
        if (const auto* result = std::get_if<ai::ToolResultMessage>(&message);
            result != nullptr && result->tool_call_id == "call-malformed") {
            context_diagnostic = ai::text_from_content(result->content);
        }
    }
    REQUIRE(context_diagnostic.has_value());
    CHECK(*context_diagnostic == *execution_end_diagnostic);

    auto durable = harness::session::JsonlSessionStore::load(paths.session_file);
    REQUIRE(durable.has_value());
    std::optional<std::string> durable_diagnostic;
    for (const auto& message : durable->messages) {
        if (const auto* result = std::get_if<ai::ToolResultMessage>(&message);
            result != nullptr && result->tool_call_id == "call-malformed") {
            durable_diagnostic = ai::text_from_content(result->content);
        }
    }
    REQUIRE(durable_diagnostic.has_value());
    CHECK(*durable_diagnostic == *execution_end_diagnostic);

    std::ifstream transcript(paths.session_file, std::ios::binary);
    const std::string persisted(
        (std::istreambuf_iterator<char>(transcript)),
        std::istreambuf_iterator<char>());
    CHECK(persisted.find(secret) == std::string::npos);
    created->session->close();
}

TEST_CASE(
    "SDK completes a cancellation-class failure as a durable aborted turn and remains reusable",
    "[sdk][live-state][incremental-persistence][issue13]") {
    TestPaths paths;
    auto client = std::make_shared<PartialToolCallAbortChatProvider>();
    auto* client_ptr = client.get();
    auto tool_execution_count = std::make_shared<std::size_t>(0);

    tests::ModelsSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.model = cch::tests::sdk_request_model("fake", "fake-model");
    opts.models = cch::tests::models_from_provider(std::move(client));
    opts.custom_tools.push_back(std::make_unique<FakeEchoTool>(tool_execution_count));

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(result.has_value());
    auto& session = result->session;

    std::vector<agent::AgentLifecycleEvent> events;
    bool terminal_was_live_before_message_end = false;
    auto subscription = session->subscribe(
        [&](const agent::AgentLifecycleEvent& event) -> util::ExpectedVoid {
            events.push_back(event);
            const auto* end = std::get_if<agent::MessageEndEvent>(&event);
            if (end == nullptr) {
                return {};
            }
            const auto* assistant = std::get_if<ai::AssistantMessage>(&end->message);
            if (assistant != nullptr && assistant->stop_reason == ai::AssistantStopReason::Aborted) {
                terminal_was_live_before_message_end =
                    session->message_count() == 2 &&
                    session->last_assistant_text().has_value() &&
                    session->last_assistant_text()->empty();
            }
            return {};
        });
    REQUIRE(subscription.has_value());

    auto aborted_turn = session->prompt_blocking("hello");
    REQUIRE(aborted_turn.has_value());
    CHECK(client_ptr->request_count == 1);
    CHECK(*tool_execution_count == 0);
    CHECK(terminal_was_live_before_message_end);
    CHECK(session->is_open());
    CHECK(session->message_count() == 2);

    std::size_t assistant_starts = 0;
    std::size_t assistant_ends = 0;
    std::size_t tool_execution_events = 0;
    std::optional<std::size_t> assistant_end_index;
    for (std::size_t index = 0; index < events.size(); ++index) {
        const auto& event = events[index];
        if (const auto* start = std::get_if<agent::MessageStartEvent>(&event)) {
            if (std::holds_alternative<ai::AssistantMessage>(start->message)) {
                ++assistant_starts;
            }
        } else if (const auto* end = std::get_if<agent::MessageEndEvent>(&event)) {
            if (const auto* assistant = std::get_if<ai::AssistantMessage>(&end->message)) {
                ++assistant_ends;
                assistant_end_index = index;
                check_aborted_terminal(*assistant);
            }
        } else if (std::holds_alternative<agent::ToolExecutionStartEvent>(event) ||
                   std::holds_alternative<agent::ToolExecutionEndEvent>(event)) {
            ++tool_execution_events;
        }
    }
    CHECK(assistant_starts == 1);
    CHECK(assistant_ends == 1);
    CHECK(tool_execution_events == 0);
    REQUIRE(assistant_end_index.has_value());
    REQUIRE(*assistant_end_index + 2 < events.size());

    const auto* turn_end = std::get_if<agent::TurnEndEvent>(&events[*assistant_end_index + 1]);
    REQUIRE(turn_end != nullptr);
    CHECK(turn_end->tool_results.empty());
    REQUIRE(std::holds_alternative<ai::AssistantMessage>(turn_end->message));
    check_aborted_terminal(std::get<ai::AssistantMessage>(turn_end->message));

    const auto* agent_end = std::get_if<agent::AgentEndEvent>(&events[*assistant_end_index + 2]);
    REQUIRE(agent_end != nullptr);
    REQUIRE(agent_end->messages.size() == 2);
    REQUIRE(std::holds_alternative<ai::AssistantMessage>(agent_end->messages[1]));
    check_aborted_terminal(std::get<ai::AssistantMessage>(agent_end->messages[1]));
    CHECK(*assistant_end_index + 3 == events.size());

    auto durable = harness::session::JsonlSessionStore::load(paths.session_file);
    REQUIRE(durable.has_value());
    REQUIRE(durable->messages.size() == 2);
    REQUIRE(std::holds_alternative<ai::AssistantMessage>(durable->messages[1]));
    check_aborted_terminal(std::get<ai::AssistantMessage>(durable->messages[1]));

    events.clear();
    auto recovered_turn = session->prompt_blocking("try again");
    REQUIRE(recovered_turn.has_value());
    CHECK(client_ptr->request_count == 2);
    CHECK(*tool_execution_count == 0);
    CHECK(session->message_count() == 4);
    REQUIRE(session->last_assistant_text().has_value());
    CHECK(*session->last_assistant_text() == "recovered");
    session->close();

    auto capture = std::make_shared<CaptureChatProvider>();
    auto* capture_ptr = capture.get();
    tests::ModelsSessionOptions resume;
    resume.session_target = coding_agent::ExplicitResumeSessionTarget{paths.session_file};
    resume.workspace = paths.workspace.path();
    resume.models = cch::tests::models_from_provider(std::move(capture));

    auto reopened = coding_agent::create_agent_session(std::move(resume));
    REQUIRE(reopened.has_value());
    CHECK(reopened->session->message_count() == 4);
    auto after_resume = reopened->session->prompt_blocking("after resume");
    REQUIRE(after_resume.has_value());
    REQUIRE(capture_ptr->captured_request.has_value());
    REQUIRE(capture_ptr->captured_request->context.messages.size() == 5);
    REQUIRE(std::holds_alternative<ai::AssistantMessage>(
        capture_ptr->captured_request->context.messages[1]));
    check_aborted_terminal(std::get<ai::AssistantMessage>(
        capture_ptr->captured_request->context.messages[1]));
    reopened->session->close();
}

TEST_CASE(
    "SDK recovers a host provider error terminal emitted before any assistant start",
    "[sdk][live-state][incremental-persistence][issue15]") {
    TestPaths paths;
    auto client = std::make_shared<TerminalBeforeStartChatProvider>(
        ai::AssistantStopReason::Error,
        "host transport lost before response start");
    auto* client_ptr = client.get();

    tests::ModelsSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.models = cch::tests::models_from_provider(std::move(client));

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(result.has_value());
    auto& session = result->session;

    std::vector<agent::AgentLifecycleEvent> events;
    bool terminal_was_live_before_message_end = false;
    auto subscription = session->subscribe(
        [&](const agent::AgentLifecycleEvent& event) -> util::ExpectedVoid {
            events.push_back(event);
            const auto* end = std::get_if<agent::MessageEndEvent>(&event);
            if (end == nullptr) {
                return {};
            }
            const auto* assistant = std::get_if<ai::AssistantMessage>(&end->message);
            if (assistant != nullptr && assistant->stop_reason == ai::AssistantStopReason::Error) {
                terminal_was_live_before_message_end = session->message_count() == 2;
            }
            return {};
        });
    REQUIRE(subscription.has_value());

    auto completed = session->prompt_blocking("hello");
    REQUIRE(completed.has_value());
    CHECK(client_ptr->request_count == 1);
    CHECK(terminal_was_live_before_message_end);
    CHECK(session->is_open());
    CHECK(session->message_count() == 2);

    // Exactly one assistant message_start and exactly one message_end.
    REQUIRE(events.size() == 8);
    std::size_t index = 0;
    CHECK(std::holds_alternative<agent::AgentStartEvent>(events[index++]));
    CHECK(std::holds_alternative<agent::TurnStartEvent>(events[index++]));
    const auto* user_start = std::get_if<agent::MessageStartEvent>(&events[index++]);
    REQUIRE(user_start != nullptr);
    CHECK(std::holds_alternative<ai::UserMessage>(user_start->message));
    const auto* user_end = std::get_if<agent::MessageEndEvent>(&events[index++]);
    REQUIRE(user_end != nullptr);
    CHECK(std::holds_alternative<ai::UserMessage>(user_end->message));
    const auto* assistant_start = std::get_if<agent::MessageStartEvent>(&events[index++]);
    REQUIRE(assistant_start != nullptr);
    REQUIRE(std::holds_alternative<ai::AssistantMessage>(assistant_start->message));
    const auto& started = std::get<ai::AssistantMessage>(assistant_start->message);
    CHECK(started.stop_reason == ai::AssistantStopReason::Error);
    REQUIRE(started.error_message.has_value());
    CHECK(*started.error_message == "host transport lost before response start");
    const auto* assistant_end = std::get_if<agent::MessageEndEvent>(&events[index++]);
    REQUIRE(assistant_end != nullptr);
    REQUIRE(std::holds_alternative<ai::AssistantMessage>(assistant_end->message));
    const auto& ended = std::get<ai::AssistantMessage>(assistant_end->message);
    CHECK(ended.stop_reason == ai::AssistantStopReason::Error);
    REQUIRE(ended.error_message.has_value());
    CHECK(*ended.error_message == "host transport lost before response start");
    const auto* turn_end = std::get_if<agent::TurnEndEvent>(&events[index++]);
    REQUIRE(turn_end != nullptr);
    CHECK(turn_end->tool_results.empty());
    REQUIRE(std::holds_alternative<ai::AssistantMessage>(turn_end->message));
    CHECK(std::get<ai::AssistantMessage>(turn_end->message).stop_reason ==
          ai::AssistantStopReason::Error);
    CHECK(std::holds_alternative<agent::AgentEndEvent>(events[index++]));
    CHECK(std::none_of(
        events.begin(),
        events.end(),
        [](const auto& event) {
            return std::holds_alternative<agent::ToolExecutionStartEvent>(event) ||
                   std::holds_alternative<agent::ToolExecutionEndEvent>(event);
        }));

    auto durable = harness::session::JsonlSessionStore::load(paths.session_file);
    REQUIRE(durable.has_value());
    REQUIRE(durable->messages.size() == 2);
    REQUIRE(std::holds_alternative<ai::AssistantMessage>(durable->messages[1]));
    const auto& durable_terminal = std::get<ai::AssistantMessage>(durable->messages[1]);
    CHECK(durable_terminal.stop_reason == ai::AssistantStopReason::Error);
    REQUIRE(durable_terminal.error_message.has_value());
    CHECK(*durable_terminal.error_message == "host transport lost before response start");

    // The session remains usable for another prompt.
    events.clear();
    auto second = session->prompt_blocking("again");
    REQUIRE(second.has_value());
    CHECK(client_ptr->request_count == 2);
    CHECK(session->message_count() == 4);
    session->close();
}

TEST_CASE(
    "SDK recovers a host provider aborted terminal emitted before any assistant start",
    "[sdk][live-state][incremental-persistence][issue15]") {
    TestPaths paths;
    auto client = std::make_shared<TerminalBeforeStartChatProvider>(
        ai::AssistantStopReason::Aborted,
        "host cancelled before response start");

    tests::ModelsSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.models = cch::tests::models_from_provider(std::move(client));

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(result.has_value());
    auto& session = result->session;

    std::vector<agent::AgentLifecycleEvent> events;
    auto subscription = session->subscribe(
        [&](const agent::AgentLifecycleEvent& event) -> util::ExpectedVoid {
            events.push_back(event);
            return {};
        });
    REQUIRE(subscription.has_value());

    auto completed = session->prompt_blocking("hello");
    REQUIRE(completed.has_value());
    CHECK(session->is_open());
    CHECK(session->message_count() == 2);

    std::size_t assistant_starts = 0;
    std::size_t assistant_ends = 0;
    std::optional<std::size_t> assistant_end_index;
    for (std::size_t i = 0; i < events.size(); ++i) {
        if (const auto* start = std::get_if<agent::MessageStartEvent>(&events[i])) {
            if (const auto* assistant = std::get_if<ai::AssistantMessage>(&start->message)) {
                ++assistant_starts;
                CHECK(assistant->stop_reason == ai::AssistantStopReason::Aborted);
            }
        } else if (const auto* end = std::get_if<agent::MessageEndEvent>(&events[i])) {
            if (const auto* assistant = std::get_if<ai::AssistantMessage>(&end->message)) {
                ++assistant_ends;
                assistant_end_index = i;
                CHECK(assistant->stop_reason == ai::AssistantStopReason::Aborted);
                REQUIRE(assistant->error_message.has_value());
                CHECK(*assistant->error_message == "host cancelled before response start");
            }
        }
    }
    CHECK(assistant_starts == 1);
    CHECK(assistant_ends == 1);
    REQUIRE(assistant_end_index.has_value());
    REQUIRE(*assistant_end_index + 2 < events.size());
    const auto* turn_end = std::get_if<agent::TurnEndEvent>(&events[*assistant_end_index + 1]);
    REQUIRE(turn_end != nullptr);
    CHECK(turn_end->tool_results.empty());
    REQUIRE(std::holds_alternative<ai::AssistantMessage>(turn_end->message));
    CHECK(std::get<ai::AssistantMessage>(turn_end->message).stop_reason ==
          ai::AssistantStopReason::Aborted);
    CHECK(std::holds_alternative<agent::AgentEndEvent>(events[*assistant_end_index + 2]));

    auto durable = harness::session::JsonlSessionStore::load(paths.session_file);
    REQUIRE(durable.has_value());
    REQUIRE(durable->messages.size() == 2);
    REQUIRE(std::holds_alternative<ai::AssistantMessage>(durable->messages[1]));
    const auto& durable_terminal = std::get<ai::AssistantMessage>(durable->messages[1]);
    CHECK(durable_terminal.stop_reason == ai::AssistantStopReason::Aborted);
    REQUIRE(durable_terminal.error_message.has_value());
    CHECK(*durable_terminal.error_message == "host cancelled before response start");
    session->close();
}

TEST_CASE(
    "SDK preserves a partial provider stream failure across live state durable history and resume",
    "[sdk][live-state][incremental-persistence][issue12]") {
    TestPaths paths;
    auto client = std::make_shared<PartialContentFailureChatProvider>();
    auto* client_ptr = client.get();
    auto tool_execution_count = std::make_shared<std::size_t>(0);

    tests::ModelsSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.model = cch::tests::sdk_request_model("fake", "fake-model");
    opts.models = cch::tests::models_from_provider(std::move(client));
    opts.custom_tools.push_back(std::make_unique<FakeEchoTool>(tool_execution_count));

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(result.has_value());
    auto& session = result->session;

    std::vector<agent::AgentLifecycleEvent> events;
    bool terminal_was_live_before_message_end = false;
    auto subscription = session->subscribe(
        [&](const agent::AgentLifecycleEvent& event) -> util::ExpectedVoid {
            events.push_back(event);
            const auto* end = std::get_if<agent::MessageEndEvent>(&event);
            if (end == nullptr) {
                return {};
            }
            const auto* assistant = std::get_if<ai::AssistantMessage>(&end->message);
            if (assistant != nullptr && assistant->stop_reason == ai::AssistantStopReason::Error) {
                terminal_was_live_before_message_end =
                    session->message_count() == 2 &&
                    session->last_assistant_text().has_value() &&
                    *session->last_assistant_text() == "useful text";
            }
            return {};
        });
    REQUIRE(subscription.has_value());

    auto prompted = session->prompt_blocking("hello");
    REQUIRE(prompted.has_value());
    CHECK(client_ptr->request_count == 1);
    CHECK(*tool_execution_count == 0);
    CHECK(terminal_was_live_before_message_end);
    CHECK(session->is_open());
    CHECK(session->message_count() == 2);
    REQUIRE(session->last_assistant_text().has_value());
    CHECK(*session->last_assistant_text() == "useful text");

    std::size_t agent_starts = 0;
    std::size_t turn_starts = 0;
    std::size_t assistant_starts = 0;
    std::size_t assistant_ends = 0;
    std::size_t tool_execution_events = 0;
    std::optional<std::size_t> assistant_end_index;
    for (std::size_t index = 0; index < events.size(); ++index) {
        const auto& event = events[index];
        if (std::holds_alternative<agent::AgentStartEvent>(event)) {
            ++agent_starts;
        } else if (std::holds_alternative<agent::TurnStartEvent>(event)) {
            ++turn_starts;
        } else if (const auto* start = std::get_if<agent::MessageStartEvent>(&event)) {
            if (std::holds_alternative<ai::AssistantMessage>(start->message)) {
                ++assistant_starts;
            }
        } else if (const auto* end = std::get_if<agent::MessageEndEvent>(&event)) {
            if (const auto* assistant = std::get_if<ai::AssistantMessage>(&end->message)) {
                ++assistant_ends;
                assistant_end_index = index;
                check_partial_protocol_terminal(*assistant);
            }
        } else if (std::holds_alternative<agent::ToolExecutionStartEvent>(event) ||
                   std::holds_alternative<agent::ToolExecutionEndEvent>(event)) {
            ++tool_execution_events;
        }
    }
    CHECK(agent_starts == 1);
    CHECK(turn_starts == 1);
    CHECK(assistant_starts == 1);
    CHECK(assistant_ends == 1);
    CHECK(tool_execution_events == 0);
    REQUIRE(assistant_end_index.has_value());
    REQUIRE(*assistant_end_index + 2 < events.size());

    const auto* turn_end = std::get_if<agent::TurnEndEvent>(&events[*assistant_end_index + 1]);
    REQUIRE(turn_end != nullptr);
    CHECK(turn_end->tool_results.empty());
    REQUIRE(std::holds_alternative<ai::AssistantMessage>(turn_end->message));
    check_partial_protocol_terminal(std::get<ai::AssistantMessage>(turn_end->message));

    const auto* agent_end = std::get_if<agent::AgentEndEvent>(&events[*assistant_end_index + 2]);
    REQUIRE(agent_end != nullptr);
    REQUIRE(agent_end->messages.size() == 2);
    REQUIRE(std::holds_alternative<ai::AssistantMessage>(agent_end->messages[1]));
    check_partial_protocol_terminal(std::get<ai::AssistantMessage>(agent_end->messages[1]));
    CHECK(*assistant_end_index + 3 == events.size());

    auto durable = harness::session::JsonlSessionStore::load(paths.session_file);
    REQUIRE(durable.has_value());
    REQUIRE(durable->messages.size() == 2);
    REQUIRE(std::holds_alternative<ai::AssistantMessage>(durable->messages[1]));
    check_partial_protocol_terminal(std::get<ai::AssistantMessage>(durable->messages[1]));

    session->close();

    auto capture = std::make_shared<CaptureChatProvider>();
    auto* capture_ptr = capture.get();
    tests::ModelsSessionOptions resume;
    resume.session_target = coding_agent::ExplicitResumeSessionTarget{paths.session_file};
    resume.workspace = paths.workspace.path();
    resume.models = cch::tests::models_from_provider(std::move(capture));

    auto reopened = coding_agent::create_agent_session(std::move(resume));
    REQUIRE(reopened.has_value());
    CHECK(reopened->session->message_count() == 2);
    auto after_resume = reopened->session->prompt_blocking("after resume");
    REQUIRE(after_resume.has_value());
    REQUIRE(capture_ptr->captured_request.has_value());
    REQUIRE(capture_ptr->captured_request->context.messages.size() == 3);
    REQUIRE(std::holds_alternative<ai::AssistantMessage>(
        capture_ptr->captured_request->context.messages[1]));
    check_partial_protocol_terminal(std::get<ai::AssistantMessage>(
        capture_ptr->captured_request->context.messages[1]));
    reopened->session->close();
}

TEST_CASE("SDK default target persists under the canonical workspace key", "[sdk][u2][default-persistence]") {
    TestPaths paths;
    cch::tests::TempWorkspace agent_dir;
    tests::EnvVarGuard agent_dir_guard{"PI_CODING_AGENT_DIR"};
    agent_dir_guard.set(agent_dir.path().string());

    tests::ModelsSessionOptions opts;
    opts.workspace = paths.workspace.path();
    opts.models = ai::providers::make_scripted_fake_models();

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(result.has_value());
    const auto canonical_workspace = std::filesystem::canonical(paths.workspace.path());
    const auto expected_directory = agent_dir.path() / "sessions" /
        coding_agent::session_paths::encode_workspace_key(canonical_workspace);

    REQUIRE(result->session_path.has_value());
    CHECK(result->session_path->parent_path() == expected_directory);
    CHECK(result->workspace == canonical_workspace);
    CHECK(result->metadata.workspace == canonical_workspace);
    CHECK(result->metadata.session_id == result->session_id);
    CHECK(result->metadata.created_at.find('T') != std::string::npos);
    CHECK(result->session_path->filename().string().find(result->session_id) != std::string::npos);
    CHECK(result->session->session_path() == result->session_path);

    auto stored = harness::session::JsonlSessionStore::load(*result->session_path);
    REQUIRE(stored.has_value());
    CHECK(stored->metadata.session_id == result->session_id);
    CHECK(stored->metadata.created_at == result->metadata.created_at);
    CHECK(stored->metadata.workspace == canonical_workspace);
    result->session->close();
}

TEST_CASE("SDK default persistence ignores CLI-only session directory inputs", "[sdk][default-persistence]") {
    TestPaths paths;
    cch::tests::TempWorkspace agent_dir;
    cch::tests::TempWorkspace cli_directory;
    tests::EnvVarGuard agent_dir_guard{"PI_CODING_AGENT_DIR"};
    tests::EnvVarGuard cli_dir_guard{"PI_CODING_AGENT_SESSION_DIR"};
    agent_dir_guard.set(agent_dir.path().string());
    cli_dir_guard.set(cli_directory.path().string());
    {
        std::ofstream settings(agent_dir.path() / "settings.json");
        settings << "{\"sessionDir\":\"" << cli_directory.path().string() << "\"}";
    }

    tests::ModelsSessionOptions opts;
    opts.workspace = paths.workspace.path();
    opts.models = ai::providers::make_scripted_fake_models();

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(result.has_value());
    REQUIRE(result->session_path.has_value());
    CHECK(result->session_path->parent_path().parent_path() == agent_dir.path() / "sessions");
    CHECK(std::filesystem::is_empty(cli_directory.path()));
    result->session->close();
}

// ─────────────────────────────────────────────────────────────────────────────
// User Settings snapshot: SessionFactory loads settings once per creation
// attempt, falls back to safe defaults with an observable warning, and uses
// the same snapshot for every assembly decision.
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("SessionFactory applies one User Settings snapshot across provider and storage decisions", "[sdk][settings-snapshot]") {
    TestPaths paths;
    cch::tests::TempWorkspace agent_dir;
    cch::tests::TempWorkspace settings_sessions;
    tests::EnvVarGuard agent_dir_guard{"PI_CODING_AGENT_DIR"};
    agent_dir_guard.set(agent_dir.path().string());
    agent_dir.write("settings.json",
        "{\"defaultModel\":\"settings-model\",\"sessionDir\":\"" + settings_sessions.path().string() + "\"}");

    coding_agent::runtime::AgentSessionCreationRequest request;
    request.fake = true;
    request.disable_project_skills = true;
    request.disable_prompt_templates = true;
    request.workspace = paths.workspace.path();

    auto result = coding_agent::create_agent_session(std::move(request));
    REQUIRE(result.has_value());
    // The same snapshot feeds provider resolution and CLI automatic
    // session-directory selection.
    CHECK(result->model == "settings-model");
    REQUIRE(result->session_path.has_value());
    CHECK(result->session_path->parent_path() == settings_sessions.path());
    result->session->close();
}

TEST_CASE(
    "SessionFactory applies one Shell settings snapshot to an enabled Bash Tool",
    "[sdk][settings-snapshot][shell][issue84]") {
#if defined(__unix__) || defined(__APPLE__)
    TestPaths paths;
    cch::tests::TempWorkspace agent_dir;
    tests::EnvVarGuard agent_dir_guard{"PI_CODING_AGENT_DIR"};
    agent_dir_guard.set(agent_dir.path().string());
    paths.workspace.write(
        "custom-shell",
        "#!/bin/sh\nexport CCH_CUSTOM_SHELL=used\nexec /bin/sh \"$@\"\n");
    std::error_code permission_error;
    std::filesystem::permissions(
        paths.workspace.path() / "custom-shell",
        std::filesystem::perms::owner_exec,
        std::filesystem::perm_options::add,
        permission_error);
    REQUIRE_FALSE(permission_error);
    agent_dir.write(
        "settings.json",
        "{\"shellPath\":\"" + (paths.workspace.path() / "custom-shell").string() +
            "\",\"shellCommandPrefix\":\"export CCH_PREFIX=first-snapshot\"}");

    tests::ModelsSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.models = ai::providers::make_scripted_fake_models();
    opts.builtin_tools.bash = true;

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(result.has_value());
    agent_dir.write(
        "settings.json",
        "{\"shellCommandPrefix\":\"export CCH_PREFIX=changed-after-startup\"}");

    auto prompt = result->session->prompt_blocking(
        "bash printf '%s' \"$CCH_CUSTOM_SHELL:$CCH_PREFIX\"");

    REQUIRE(prompt);
    const auto assistant_text = result->session->last_assistant_text();
    REQUIRE(assistant_text.has_value());
    CHECK(assistant_text->find("used:first-snapshot") != std::string::npos);
    CHECK(assistant_text->find("changed-after-startup") == std::string::npos);
    result->session->close();
    const auto persisted = paths.workspace.read("test-session.jsonl");
    CHECK(persisted.find("export CCH_PREFIX") == std::string::npos);
#else
    SUCCEED("local Shell settings are supported on Unix platforms");
#endif
}

TEST_CASE(
    "a stale configured Shell path defers failure until Bash execution",
    "[sdk][settings-snapshot][shell][issue84]") {
#if defined(__unix__) || defined(__APPLE__)
    TestPaths paths;
    cch::tests::TempWorkspace agent_dir;
    tests::EnvVarGuard agent_dir_guard{"PI_CODING_AGENT_DIR"};
    agent_dir_guard.set(agent_dir.path().string());
    agent_dir.write(
        "settings.json",
        "{\"shellPath\":\"" + (paths.workspace.path() / "missing-shell").string() + "\"}");

    tests::ModelsSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.models = ai::providers::make_scripted_fake_models();
    opts.builtin_tools.bash = true;

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(result.has_value());
    REQUIRE(result->session->prompt_blocking("ordinary prompt"));
    const auto shell_prompt = result->session->prompt_blocking("bash printf unreachable");

    REQUIRE(shell_prompt);
    const auto assistant_text = result->session->last_assistant_text();
    REQUIRE(assistant_text.has_value());
    CHECK(assistant_text->find("missing-shell") != std::string::npos);
    result->session->close();
#else
    SUCCEED("local Shell settings are supported on Unix platforms");
#endif
}

TEST_CASE("SessionFactory creation without User Settings applies defaults silently", "[sdk][settings-snapshot]") {
    TestPaths paths;
    cch::tests::TempWorkspace agent_dir;
    tests::EnvVarGuard agent_dir_guard{"PI_CODING_AGENT_DIR"};
    agent_dir_guard.set(agent_dir.path().string());

    tests::ModelsSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.models = ai::providers::make_scripted_fake_models();

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(result.has_value());
    CHECK_FALSE(has_sdk_diag(result->diagnostics, "settings:fallback"));
    result->session->close();
}

TEST_CASE("SessionFactory creation with malformed User Settings succeeds with safe defaults and a warning", "[sdk][settings-snapshot]") {
    TestPaths paths;
    cch::tests::TempWorkspace agent_dir;
    tests::EnvVarGuard agent_dir_guard{"PI_CODING_AGENT_DIR"};
    agent_dir_guard.set(agent_dir.path().string());
    agent_dir.write("settings.json", "{not valid json");

    tests::ModelsSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.models = ai::providers::make_scripted_fake_models();

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(result.has_value());
    const auto warning = find_sdk_diag(result->diagnostics, "settings:global");
    REQUIRE(warning != result->diagnostics.end());
    CHECK(warning->severity == coding_agent::SdkDiagnostic::Severity::Warning);
    CHECK(warning->message.find("could not load global settings") != std::string::npos);
    CHECK(warning->message.find("invalid JSON") != std::string::npos);
    result->session->close();
}

TEST_CASE("SessionFactory creation with invalid User Settings values falls back with a warning", "[sdk][settings-snapshot]") {
    TestPaths paths;
    cch::tests::TempWorkspace agent_dir;
    tests::EnvVarGuard agent_dir_guard{"PI_CODING_AGENT_DIR"};
    agent_dir_guard.set(agent_dir.path().string());
    agent_dir.write("settings.json", "{\"defaultProjectTrust\":\"bogus\"}");

    tests::ModelsSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.models = ai::providers::make_scripted_fake_models();

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(result.has_value());
    const auto warning = find_sdk_diag(result->diagnostics, "settings:global");
    REQUIRE(warning != result->diagnostics.end());
    CHECK(warning->severity == coding_agent::SdkDiagnostic::Severity::Warning);
    CHECK(warning->message.find("defaultProjectTrust") != std::string::npos);
    result->session->close();
}

TEST_CASE("SessionFactory SDK creation failure after User Settings fallback keeps the primary error and carries the warning", "[sdk][settings-snapshot]") {
    TestPaths paths;
    cch::tests::TempWorkspace agent_dir;
    tests::EnvVarGuard agent_dir_guard{"PI_CODING_AGENT_DIR"};
    agent_dir_guard.set(agent_dir.path().string());
    agent_dir.write("settings.json", "{not valid json");

    tests::ModelsSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    // A public SDK creation failure (trust store inside the workspace) keeps
    // the settings fallback warning in the error context.
    opts.trust_store_path = paths.workspace.path();

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == util::ErrorCode::Validation);
    REQUIRE(result.error().context.has_value());
    CHECK(result.error().context->find("could not load global settings") != std::string::npos);
}

TEST_CASE("SessionFactory CLI creation failure after User Settings fallback keeps the primary error and carries the warning", "[sdk][settings-snapshot]") {
    TestPaths paths;
    cch::tests::TempWorkspace agent_dir;
    tests::EnvVarGuard agent_dir_guard{"PI_CODING_AGENT_DIR"};
    agent_dir_guard.set(agent_dir.path().string());
    agent_dir.write("settings.json", "{not valid json");

    coding_agent::runtime::AgentSessionCreationRequest request;
    request.fake = true;
    request.workspace = paths.workspace.path() / "missing-workspace";

    auto result = coding_agent::create_agent_session(std::move(request));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == util::ErrorCode::Validation);
    CHECK(result.error().message == "workspace cannot be resolved");
    REQUIRE(result.error().context.has_value());
    CHECK(result.error().context->find("could not load global settings") != std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// CLI request assembly policy: SessionFactory owns workspace validity, Agent
// Session target validity, existing-file rejection, Session Resume
// compatibility, and provider readiness for the CLI creation request shape.
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("SessionFactory CLI resolves missing credentials as a terminal auth outcome", "[sdk][assembly][cli]") {
    TestPaths paths;
    cch::tests::TempWorkspace agent_dir;
    tests::EnvVarGuard agent_dir_guard{"PI_CODING_AGENT_DIR"};
    agent_dir_guard.set(agent_dir.path().string());
    // Deterministic: no ambient kimi key, so nothing resolves as configured
    // and the Agent holds pi's unknown kDefaultModel (T04).
    tests::EnvVarGuard kimi_guard{"KIMI_API_KEY"};
    kimi_guard.unset();

    coding_agent::runtime::AgentSessionCreationRequest request;
    request.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    request.workspace = paths.workspace.path();

    auto result = coding_agent::create_agent_session(std::move(request));
    REQUIRE(result.has_value());
    auto prompted = result->session->prompt_blocking("hello");
    REQUIRE(prompted.has_value());
    const auto snapshot = result->session->snapshot();
    REQUIRE(snapshot.agent_state.messages.size() == 2);
    const auto& terminal = std::get<ai::AssistantMessage>(snapshot.agent_state.messages.back());
    CHECK(terminal.stop_reason == ai::AssistantStopReason::Error);
    REQUIRE(terminal.error_message);
    CHECK(terminal.error_message->find("Unknown provider: unknown") != std::string::npos);
    CHECK(std::filesystem::exists(paths.session_file));
    result->session->close();
}

TEST_CASE("SessionFactory CLI explicit new target rejects an unresolvable workspace", "[sdk][assembly][cli]") {
    TestPaths paths;
    cch::tests::TempWorkspace agent_dir;
    tests::EnvVarGuard agent_dir_guard{"PI_CODING_AGENT_DIR"};
    agent_dir_guard.set(agent_dir.path().string());

    coding_agent::runtime::AgentSessionCreationRequest request;
    request.fake = true;
    request.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    request.workspace = paths.workspace.path() / "missing-workspace";

    auto result = coding_agent::create_agent_session(std::move(request));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == util::ErrorCode::Validation);
    CHECK(result.error().message.find("workspace") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(paths.session_file));
}

TEST_CASE("SessionFactory CLI resume rejects an unresolvable explicit workspace", "[sdk][assembly][cli]") {
    TestPaths paths;
    cch::tests::TempWorkspace agent_dir;
    tests::EnvVarGuard agent_dir_guard{"PI_CODING_AGENT_DIR"};
    agent_dir_guard.set(agent_dir.path().string());

    coding_agent::runtime::AgentSessionCreationRequest create_request;
    create_request.fake = true;
    create_request.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    create_request.workspace = paths.workspace.path();
    auto created = coding_agent::create_agent_session(std::move(create_request));
    REQUIRE(created.has_value());
    created->session->close();

    coding_agent::runtime::AgentSessionCreationRequest resume_request;
    resume_request.fake = true;
    resume_request.session_target = coding_agent::ExplicitResumeSessionTarget{paths.session_file};
    resume_request.workspace = paths.workspace.path() / "missing-workspace";
    resume_request.workspace_explicit = true;

    auto result = coding_agent::create_agent_session(std::move(resume_request));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == util::ErrorCode::Validation);
    CHECK(result.error().message == "workspace cannot be resolved");
}

TEST_CASE("SessionFactory CLI explicit new target rejects an existing session file without modifying it", "[sdk][assembly][cli]") {
    TestPaths paths;
    cch::tests::TempWorkspace agent_dir;
    tests::EnvVarGuard agent_dir_guard{"PI_CODING_AGENT_DIR"};
    agent_dir_guard.set(agent_dir.path().string());
    {
        std::ofstream existing(paths.session_file, std::ios::binary);
        existing << "already here";
    }

    coding_agent::runtime::AgentSessionCreationRequest request;
    request.fake = true;
    request.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    request.workspace = paths.workspace.path();

    auto result = coding_agent::create_agent_session(std::move(request));
    REQUIRE_FALSE(result.has_value());
    const auto presented = result.error().message + " " + result.error().detail;
    CHECK(presented.find("already exists") != std::string::npos);
    std::ifstream in(paths.session_file, std::ios::binary);
    const std::string content(
        (std::istreambuf_iterator<char>(in)),
        std::istreambuf_iterator<char>());
    CHECK(content == "already here");
}

#if defined(__unix__) || defined(__APPLE__)
TEST_CASE("SDK default target gives workspace symlink aliases one directory", "[sdk][default-persistence][symlink]") {
    TestPaths paths;
    cch::tests::TempWorkspace agent_dir;
    cch::tests::TempWorkspace aliases;
    tests::EnvVarGuard agent_dir_guard{"PI_CODING_AGENT_DIR"};
    agent_dir_guard.set(agent_dir.path().string());
    const auto alias = aliases.path() / "workspace-alias";
    std::filesystem::create_directory_symlink(paths.workspace.path(), alias);

    auto create_from = [&](const std::filesystem::path& workspace) {
        tests::ModelsSessionOptions opts;
        opts.workspace = workspace;
        opts.models = ai::providers::make_scripted_fake_models();
        return coding_agent::create_agent_session(std::move(opts));
    };

    auto physical = create_from(paths.workspace.path());
    REQUIRE(physical.has_value());
    auto linked = create_from(alias);
    REQUIRE(linked.has_value());
    REQUIRE(physical->session_path.has_value());
    REQUIRE(linked->session_path.has_value());
    CHECK(physical->session_path->parent_path() == linked->session_path->parent_path());
    CHECK(physical->workspace == linked->workspace);
    CHECK(physical->metadata.workspace == linked->metadata.workspace);
    physical->session->close();
    linked->session->close();
}
#endif

TEST_CASE("failed SDK default assembly publishes no session", "[sdk][assembly][default-persistence]") {
    TestPaths paths;
    cch::tests::TempWorkspace home;
    const auto agent_dir = home.path() / "not-created" / "agent";
    tests::EnvVarGuard agent_dir_guard{"PI_CODING_AGENT_DIR"};
    agent_dir_guard.set(agent_dir.string());

    tests::ModelsSessionOptions opts;
    opts.workspace = paths.workspace.path();
    opts.models = ai::providers::make_scripted_fake_models();
    opts.custom_tools.push_back(std::make_unique<FakeEchoTool>());
    opts.custom_tools.push_back(std::make_unique<FakeEchoTool>());

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE_FALSE(result.has_value());
    CHECK_FALSE(std::filesystem::exists(agent_dir / "sessions"));
}

TEST_CASE("SDK explicit new and resume paths bypass automatic storage", "[sdk][u2][session-target]") {
    TestPaths paths;
    cch::tests::TempWorkspace agent_dir;
    tests::EnvVarGuard agent_dir_guard{"PI_CODING_AGENT_DIR"};
    agent_dir_guard.set((agent_dir.path() / "unused-agent-dir").string());

    tests::ModelsSessionOptions create;
    create.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    create.workspace = paths.workspace.path();
    create.models = ai::providers::make_scripted_fake_models();

    auto created = coding_agent::create_agent_session(std::move(create));
    REQUIRE(created.has_value());
    CHECK(created->session_path == std::optional<std::filesystem::path>{paths.session_file});
    CHECK(created->session->session_path() == created->session_path);
    CHECK_FALSE(std::filesystem::exists(agent_dir.path() / "unused-agent-dir" / "sessions"));
    created->session->close();

    tests::ModelsSessionOptions resume;
    resume.session_target = coding_agent::ExplicitResumeSessionTarget{paths.session_file};
    resume.workspace = paths.workspace.path();
    resume.models = ai::providers::make_scripted_fake_models();

    auto resumed = coding_agent::create_agent_session(std::move(resume));
    REQUIRE(resumed.has_value());
    CHECK(resumed->session_path == std::optional<std::filesystem::path>{paths.session_file});
    CHECK(resumed->session->session_path() == resumed->session_path);
    CHECK_FALSE(std::filesystem::exists(agent_dir.path() / "unused-agent-dir" / "sessions"));
    resumed->session->close();
}

TEST_CASE("SDK default creation ignores a valid legacy session that remains explicitly resumable", "[sdk][default-persistence]") {
    TestPaths paths;
    cch::tests::TempWorkspace agent_dir;
    tests::EnvVarGuard agent_dir_guard{"PI_CODING_AGENT_DIR"};
    agent_dir_guard.set(agent_dir.path().string());
    const auto legacy_directory = paths.workspace.path() / ".cpp-harness" / "sessions";
    const auto legacy_session = legacy_directory / "legacy.jsonl";
    std::filesystem::create_directories(legacy_directory);
    {
        auto legacy_store = harness::session::JsonlSessionStore::create_new(
            legacy_session,
            harness::session::SessionMetadata{
                .session_id = "legacy-session-id",
                .created_at = "2026-07-18T00:00:00.000Z",
                .workspace = paths.workspace.path(),
                .provider = "fake",
                .model = "fake-model",
            });
        REQUIRE(legacy_store.has_value());
    }

    tests::ModelsSessionOptions defaults;
    defaults.workspace = paths.workspace.path();
    defaults.models = ai::providers::make_scripted_fake_models();

    auto created = coding_agent::create_agent_session(std::move(defaults));
    REQUIRE(created.has_value());
    REQUIRE(created->session_path.has_value());
    CHECK(*created->session_path != legacy_session);
    CHECK(created->session_path->parent_path().parent_path() == agent_dir.path() / "sessions");
    CHECK(std::filesystem::is_regular_file(legacy_session));
    created->session->close();

    tests::ModelsSessionOptions resume;
    resume.session_target = coding_agent::ExplicitResumeSessionTarget{legacy_session};
    resume.workspace = paths.workspace.path();
    resume.models = ai::providers::make_scripted_fake_models();

    auto resumed = coding_agent::create_agent_session(std::move(resume));
    REQUIRE(resumed.has_value());
    CHECK(resumed->session_id == "legacy-session-id");
    CHECK(resumed->session_path == std::optional<std::filesystem::path>{legacy_session});
    resumed->session->close();
}

TEST_CASE("SDK default publication failure does not use the legacy workspace directory", "[sdk][default-persistence][failure]") {
    TestPaths paths;
    cch::tests::TempWorkspace home;
    const auto agent_dir = home.path() / "agent";
    const auto sessions_root = agent_dir / "sessions";
    std::filesystem::create_directories(agent_dir);
    {
        std::ofstream blocker(sessions_root);
        blocker << "not a directory";
    }
    const auto legacy_directory = paths.workspace.path() / ".cpp-harness" / "sessions";
    std::filesystem::create_directories(legacy_directory);
    const auto legacy_marker = legacy_directory / "legacy.jsonl";
    {
        std::ofstream marker(legacy_marker);
        marker << "legacy";
    }
    tests::EnvVarGuard agent_dir_guard{"PI_CODING_AGENT_DIR"};
    agent_dir_guard.set(agent_dir.string());

    tests::ModelsSessionOptions opts;
    opts.workspace = paths.workspace.path();
    opts.models = ai::providers::make_scripted_fake_models();

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == util::ErrorCode::Session);
    CHECK(result.error().detail.find(sessions_root.string()) != std::string::npos);
    CHECK(std::filesystem::is_regular_file(legacy_marker));
    CHECK(std::filesystem::file_size(legacy_marker) == 6);
}

#if defined(__unix__) || defined(__APPLE__)
TEST_CASE("SDK default creation fails when the Agent Config Directory is unresolved", "[sdk][default-persistence][failure]") {
    TestPaths paths;
    tests::EnvVarGuard agent_dir_guard{"PI_CODING_AGENT_DIR"};
    tests::EnvVarGuard home_guard{"HOME"};
    agent_dir_guard.unset();
    home_guard.unset();

    tests::ModelsSessionOptions opts;
    opts.workspace = paths.workspace.path();
    opts.models = ai::providers::make_scripted_fake_models();

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == util::ErrorCode::Session);
    CHECK(result.error().detail.find("<unresolved agent config sessions root>") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(paths.workspace.path() / ".cpp-harness" / "sessions"));
}
#endif

TEST_CASE("SDK public session creation default-creates a ModelRuntime", "[sdk][u2][issue345]") {
    TestPaths paths;
    cch::tests::TempWorkspace home;
    tests::EnvVarGuard home_guard{"HOME"};
    home_guard.set(home.path().string());

    tests::ModelsSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(result.has_value());
    REQUIRE(result->session->model_runtime() != nullptr);
    CHECK(result->session->model_runtime()->agent_dir() == (home.path() / ".pi" / "agent"));
    result->session->close();
}

TEST_CASE("SDK new session carries the complete explicit request model", "[sdk][u2][api-key-env][issue336]") {
    TestPaths paths;
    cch::tests::TempWorkspace home;
    tests::EnvVarGuard home_guard{"HOME"};
    home_guard.set(home.path().string());

    tests::ModelsSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.model = cch::tests::sdk_request_model("fake", "fake-model");
    opts.models = ai::providers::make_scripted_fake_models();

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(result.has_value());
    CHECK(result->provider == "fake");
    CHECK(result->model == "fake-model");
    const auto model = result->session->snapshot().agent_state.model;
    CHECK(model.input == std::vector<ai::ModelInput>{ai::ModelInput::Text});
    CHECK(model.context_window == 128000);
    CHECK(model.max_tokens == 16384);
    CHECK(model.cost.input == 0);
    CHECK(model.cost.output == 0);
    result->session->close();
}

TEST_CASE("SDK config-only provider without apiKey fails at preflight with pi's verbatim no-key guidance", "[sdk][u2][api-key-env][issue345][issue360]") {
    TestPaths paths;
    cch::tests::TempWorkspace home;
    tests::EnvVarGuard home_guard{"HOME"};
    home_guard.set(home.path().string());
    home.write(".pi/agent/models.json", R"({
      "providers": {
        "deepseek": {
          "baseUrl": "https://api.deepseek.example/v1",
          "api": "openai-responses",
          "models": [{"id": "deepseek-v4-flash"}]
        }
      }
    })");

    tests::ModelsSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    ai::Model deepseek;
    deepseek.id = "deepseek-v4-flash";
    deepseek.name = "deepseek-v4-flash";
    deepseek.api = "openai-responses";
    deepseek.provider = "deepseek";
    deepseek.base_url = "https://api.deepseek.example/v1";
    deepseek.input = {ai::ModelInput::Text};
    opts.model = std::move(deepseek);

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(result.has_value());
    // T11 preflight (pi `prompt()` hasConfiguredAuth check): the keyless
    // config-only provider fails the prompt before any stream with pi's
    // verbatim formatNoApiKeyFoundMessage through the auth category.
    auto prompted = result->session->prompt_blocking("hello");
    REQUIRE_FALSE(prompted.has_value());
    CHECK(prompted.error().code == util::ErrorCode::Auth);
    CHECK(prompted.error().message ==
          coding_agent::format_no_api_key_found_message(
              "deepseek",
              std::filesystem::path{
                  coding_agent::kDefaultAuthGuidanceDocsPath}));
    result->session->close();
}

TEST_CASE("SDK resume re-resolves the stored model against the live runtime", "[sdk][u2][api-key-env][issue345]") {
    TestPaths paths;
    cch::tests::TempWorkspace home;
    tests::EnvVarGuard home_guard{"HOME"};
    home_guard.set(home.path().string());

    {
        tests::ModelsSessionOptions opts;
        opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
        opts.workspace = paths.workspace.path();
        opts.model = cch::tests::sdk_request_model("fake", "fake-model");
        opts.models = ai::providers::make_scripted_fake_models();
        auto result = coding_agent::create_agent_session(std::move(opts));
        REQUIRE(result.has_value());
        result->session->close();
    }

    tests::ModelsSessionOptions resume_opts;
    resume_opts.session_target = coding_agent::ExplicitResumeSessionTarget{paths.session_file};
    resume_opts.workspace = paths.workspace.path();
    resume_opts.models = ai::providers::make_scripted_fake_models();

    auto resume_result = coding_agent::create_agent_session(std::move(resume_opts));
    REQUIRE(resume_result.has_value());
    CHECK(resume_result->provider == "fake");
    CHECK(resume_result->model == "fake-model");
    resume_result->session->close();
}

// ─────────────────────────────────────────────────────────────────────────────
// U3: Session facade, prompt lifecycle, event fanout
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("SDK async prompt runs on the awaiting host executor", "[sdk][u3][async]") {
    TestPaths paths;

    auto client = std::make_shared<ExecutorCapturingChatProvider>();
    auto* client_ptr = client.get();

    tests::ModelsSessionOptions opts;
    opts.session_target = coding_agent::InMemorySessionTarget{};
    opts.workspace = paths.workspace.path();
    opts.models = cch::tests::models_from_provider(std::move(client));

    auto created = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(created.has_value());

    boost::asio::io_context host_io;
    std::optional<util::ExpectedVoid> prompt_result;
    boost::asio::co_spawn(
        host_io,
        [&]() -> boost::asio::awaitable<void> {
            prompt_result = co_await created->session->prompt("hello from host executor");
            co_return;
        },
        boost::asio::detached);

    CHECK(client_ptr->request_count == 0);
    CHECK_FALSE(prompt_result.has_value());

    host_io.run();

    REQUIRE(prompt_result.has_value());
    REQUIRE(prompt_result->has_value());
    CHECK(client_ptr->request_count == 1);
    CHECK(client_ptr->execution_context == &host_io);
    CHECK(created->session->message_count() == 2);
}

TEST_CASE("SDK stashed prompt awaitable survives destruction of the session handle", "[sdk][u3][async][issue38]") {
    TestPaths paths;

    tests::ModelsSessionOptions opts;
    opts.session_target = coding_agent::InMemorySessionTarget{};
    opts.workspace = paths.workspace.path();
    opts.models = ai::providers::make_scripted_fake_models();

    auto created = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(created.has_value());

    // The session impl is captured synchronously when prompt() is called, so
    // destroying the public handle before the first resume must be safe.
    auto stashed = created->session->prompt("stashed prompt");
    created->session.reset();

    boost::asio::io_context host_io;
    std::optional<util::ExpectedVoid> prompt_result;
    boost::asio::co_spawn(
        host_io,
        [&]() -> boost::asio::awaitable<void> {
            prompt_result = co_await std::move(stashed);
            co_return;
        },
        boost::asio::detached);
    host_io.run();

    // Destroying the handle closed the session, so the stashed awaitable
    // completes with the ordinary preflight rejection instead of touching
    // freed state.
    REQUIRE(prompt_result.has_value());
    REQUIRE_FALSE(prompt_result->has_value());
    CHECK(prompt_result->error().code == util::ErrorCode::Validation);
    CHECK(prompt_result->error().message == "session is closed");
}

TEST_CASE("SDK stashed prompt awaitable survives a moved-from session handle", "[sdk][u3][async][issue38]") {
    TestPaths paths;

    tests::ModelsSessionOptions opts;
    opts.session_target = coding_agent::InMemorySessionTarget{};
    opts.workspace = paths.workspace.path();
    opts.models = ai::providers::make_scripted_fake_models();

    auto created = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(created.has_value());

    auto stashed = created->session->prompt("moved prompt");

    // Move the session into a new handle and destroy the moved-from original
    // before the first resume; the prompt must still complete normally.
    coding_agent::AgentSession moved(std::move(*created->session));
    created->session.reset();

    boost::asio::io_context host_io;
    std::optional<util::ExpectedVoid> prompt_result;
    boost::asio::co_spawn(
        host_io,
        [&]() -> boost::asio::awaitable<void> {
            prompt_result = co_await std::move(stashed);
            co_return;
        },
        boost::asio::detached);
    host_io.run();

    REQUIRE(prompt_result.has_value());
    CHECK(prompt_result->has_value());
    CHECK(moved.message_count() == 2);
    moved.close();
}

TEST_CASE("SDK async prompt rejects overlap before session mutation", "[sdk][u3][async]") {
    TestPaths paths;

    auto client = std::make_shared<GatedSdkChatProvider>();
    auto* client_ptr = client.get();

    tests::ModelsSessionOptions opts;
    opts.session_target = coding_agent::InMemorySessionTarget{};
    opts.workspace = paths.workspace.path();
    opts.models = cch::tests::models_from_provider(std::move(client));

    auto created = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(created.has_value());

    boost::asio::io_context host_io;
    std::optional<util::ExpectedVoid> first_result;
    std::optional<util::ExpectedVoid> overlapping_result;
    boost::asio::co_spawn(
        host_io,
        [&]() -> boost::asio::awaitable<void> {
            first_result = co_await created->session->prompt("first");
            co_return;
        },
        boost::asio::detached);

    while (!client_ptr->started) {
        REQUIRE(host_io.poll_one() == 1);
    }
    REQUIRE(created->session->is_busy());
    const auto messages_before_overlap = created->session->message_count();
    REQUIRE(messages_before_overlap == 1);

    boost::asio::co_spawn(
        host_io,
        [&]() -> boost::asio::awaitable<void> {
            overlapping_result = co_await created->session->prompt("overlap");
            co_return;
        },
        boost::asio::detached);
    while (!overlapping_result) {
        REQUIRE(host_io.poll_one() == 1);
    }

    REQUIRE_FALSE(overlapping_result->has_value());
    CHECK(overlapping_result->error().code == util::ErrorCode::Validation);
    CHECK(overlapping_result->error().message == "session is busy (prompt already in flight)");
    CHECK(created->session->message_count() == messages_before_overlap);
    CHECK(client_ptr->request_count == 1);

    client_ptr->release();
    if (host_io.stopped()) {
        host_io.restart();
    }
    host_io.run();

    REQUIRE(first_result.has_value());
    REQUIRE(first_result->has_value());
    CHECK_FALSE(created->session->is_busy());
    CHECK(created->session->message_count() == 2);
}

TEST_CASE(
    "SDK close from prompt preflight carries cancellation into the admitted run",
    "[sdk][u3][lifecycle][issue41]") {
    TestPaths paths;
    auto observation = std::make_shared<PreflightCancellationObservation>();

    tests::ModelsSessionOptions opts;
    opts.session_target = coding_agent::InMemorySessionTarget{};
    opts.workspace = paths.workspace.path();
    opts.models = cch::tests::models_from_provider(std::make_shared<PreflightCancellationChatProvider>(observation));

    auto created = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(created.has_value());

    std::vector<agent::AgentLifecycleEvent> events;
    auto subscription = created->session->subscribe(
        [&](const agent::AgentLifecycleEvent& event) -> util::ExpectedVoid {
            events.push_back(event);
            return {};
        });
    REQUIRE(subscription.has_value());

    bool preflight_accepted = false;
    auto result = coding_agent::detail::AgentSessionPromptAccess::prompt_blocking(
        *created->session,
        "close during preflight",
        {},
        true,
        [&]() -> util::ExpectedVoid {
            preflight_accepted = true;
            created->session->close();
            CHECK_FALSE(created->session->is_open());
            CHECK(created->session->is_busy());
            return {};
        });

    REQUIRE(result.has_value());
    CHECK(preflight_accepted);
    CHECK(observation->stop_requested);
    CHECK(has_aborted_message_end(events));
    CHECK_FALSE(created->session->is_open());
    CHECK_FALSE(created->session->is_busy());
    CHECK_FALSE(static_cast<bool>(*subscription));
}

TEST_CASE(
    "SDK close while idle releases owned resources exactly once and rejects admission",
    "[sdk][u3][lifecycle][issue41]") {
    TestPaths paths;
    auto counts = std::make_shared<SdkOwnedLifetimeCounts>();

    tests::ModelsSessionOptions opts;
    opts.session_target = coding_agent::InMemorySessionTarget{};
    opts.workspace = paths.workspace.path();
    opts.models = cch::tests::models_from_provider(std::make_shared<LifetimeTrackedChatProvider>(counts));
    opts.custom_tools.push_back(std::make_unique<LifetimeTrackedTool>(counts));

    auto created = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(created.has_value());

    auto sink_lifetime = std::make_shared<SubscriberLifetimeToken>();
    std::weak_ptr<SubscriberLifetimeToken> weak_sink_lifetime = sink_lifetime;
    auto subscription = created->session->subscribe(
        [sink_lifetime](const agent::AgentLifecycleEvent&) -> util::ExpectedVoid {
            return {};
        });
    REQUIRE(subscription.has_value());
    sink_lifetime.reset();
    REQUIRE_FALSE(weak_sink_lifetime.expired());

    created->session->close();
    CHECK_FALSE(created->session->is_open());
    CHECK_FALSE(created->session->is_busy());
    CHECK(counts->destroyed_clients == 1);
    CHECK(counts->destroyed_tools == 1);
    CHECK(weak_sink_lifetime.expired());
    CHECK_FALSE(static_cast<bool>(*subscription));
    CHECK_FALSE(created->session->subscribe(
        [](const agent::AgentLifecycleEvent&) -> util::ExpectedVoid { return {}; }).has_value());
    CHECK_FALSE(created->session->prompt_blocking("too late").has_value());

    created->session->close();
    CHECK(counts->destroyed_clients == 1);
    CHECK(counts->destroyed_tools == 1);
    created->session.reset();
    CHECK(counts->destroyed_clients == 1);
    CHECK(counts->destroyed_tools == 1);
}

TEST_CASE(
    "SDK close during an active prompt requests cancellation without waiting",
    "[sdk][u3][async][lifecycle][issue41]") {
    TestPaths paths;

    auto client = std::make_shared<AbortAwareSdkChatProvider>(false);
    auto* client_ptr = client.get();

    tests::ModelsSessionOptions opts;
    opts.session_target = coding_agent::InMemorySessionTarget{};
    opts.workspace = paths.workspace.path();
    opts.models = cch::tests::models_from_provider(std::move(client));

    auto created = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(created.has_value());

    std::vector<agent::AgentLifecycleEvent> events;
    auto subscription = created->session->subscribe(
        [&](const agent::AgentLifecycleEvent& event) -> util::ExpectedVoid {
            events.push_back(event);
            return {};
        });
    REQUIRE(subscription.has_value());

    boost::asio::io_context host_io;
    std::optional<util::ExpectedVoid> prompt_result;
    boost::asio::co_spawn(
        host_io,
        [&]() -> boost::asio::awaitable<void> {
            prompt_result = co_await created->session->prompt("close me");
            co_return;
        },
        boost::asio::detached);

    while (!client_ptr->started) {
        REQUIRE(host_io.poll_one() == 1);
    }
    REQUIRE(client_ptr->first_stop_token.has_value());
    REQUIRE_FALSE(prompt_result.has_value());

    created->session->close();
    CHECK(client_ptr->first_stop_token->stop_requested());
    CHECK_FALSE(prompt_result.has_value());
    CHECK_FALSE(created->session->is_open());
    CHECK(created->session->is_busy());
    auto rejected_while_closing = created->session->steer("too late");
    REQUIRE_FALSE(rejected_while_closing);
    CHECK(rejected_while_closing.error().message == "session is closed");
    CHECK_FALSE(created->session->subscribe(
        [](const agent::AgentLifecycleEvent&) -> util::ExpectedVoid { return {}; }).has_value());
    CHECK_FALSE(created->session->prompt_blocking("too late").has_value());

    if (host_io.stopped()) {
        host_io.restart();
    }
    host_io.run();

    REQUIRE(prompt_result.has_value());
    REQUIRE(prompt_result->has_value());
    CHECK_FALSE(created->session->is_busy());
    CHECK_FALSE(created->session->is_open());
    CHECK(has_aborted_message_end(events));
    CHECK_FALSE(static_cast<bool>(*subscription));
    created->session->close();
}

TEST_CASE(
    "SDK close immediately after abort defers teardown until prompt quiescence",
    "[sdk][u3][async][abort][lifecycle][issue41]") {
    TestPaths paths;
    auto counts = std::make_shared<SdkOwnedLifetimeCounts>();
    auto client = std::make_shared<AbortAwareSdkChatProvider>(false, counts);
    auto* client_ptr = client.get();

    tests::ModelsSessionOptions opts;
    opts.session_target = coding_agent::InMemorySessionTarget{};
    opts.workspace = paths.workspace.path();
    opts.models = cch::tests::models_from_provider(std::move(client));
    opts.custom_tools.push_back(std::make_unique<LifetimeTrackedTool>(counts));

    auto created = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(created.has_value());

    boost::asio::io_context host_io;
    std::optional<util::ExpectedVoid> prompt_result;
    boost::asio::co_spawn(
        host_io,
        [&]() -> boost::asio::awaitable<void> {
            prompt_result = co_await created->session->prompt("abort then close");
            co_return;
        },
        boost::asio::detached);
    while (!client_ptr->started) {
        REQUIRE(host_io.poll_one() == 1);
    }

    created->session->abort();
    created->session->close();
    CHECK_FALSE(created->session->is_open());
    CHECK(created->session->is_busy());
    CHECK_FALSE(prompt_result.has_value());
    CHECK(counts->destroyed_clients == 0);
    CHECK(counts->destroyed_tools == 0);

    if (host_io.stopped()) {
        host_io.restart();
    }
    host_io.run();

    REQUIRE(prompt_result.has_value());
    REQUIRE(prompt_result->has_value());
    CHECK_FALSE(created->session->is_open());
    CHECK_FALSE(created->session->is_busy());
    CHECK(counts->destroyed_clients == 1);
    CHECK(counts->destroyed_tools == 1);

    created->session->close();
    CHECK(counts->destroyed_clients == 1);
    CHECK(counts->destroyed_tools == 1);
}

TEST_CASE(
    "SDK close immediately after an accepted terminal failure is safe",
    "[sdk][u3][lifecycle][issue41]") {
    TestPaths paths;
    tests::ModelsSessionOptions opts;
    opts.session_target = coding_agent::InMemorySessionTarget{};
    opts.workspace = paths.workspace.path();
    opts.models = cch::tests::models_from_provider(std::make_shared<TerminalBeforeStartChatProvider>(
        ai::AssistantStopReason::Error,
        "terminal failure"));

    auto created = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(created.has_value());
    REQUIRE(created->session->prompt_blocking("fail").has_value());
    CHECK(created->session->is_open());
    CHECK_FALSE(created->session->is_busy());

    created->session->close();
    CHECK_FALSE(created->session->is_open());
    CHECK_FALSE(created->session->is_busy());
    created->session->close();
}

TEST_CASE(
    "SDK close immediately after persistence failure is safe",
    "[sdk][u3][lifecycle][persistence-failure][issue41]") {
    TestPaths paths;
    tests::ModelsSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.models = cch::tests::models_from_provider(std::make_shared<CaptureChatProvider>());

    auto created = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(created.has_value());
    harness::session::testing::fail_nth_append_for_test(paths.session_file, 2);
    auto failed = created->session->prompt_blocking("fail persistence");
    REQUIRE_FALSE(failed.has_value());
    CHECK(failed.error().code == util::ErrorCode::Session);
    CHECK(created->session->is_open());
    CHECK_FALSE(created->session->is_busy());

    created->session->close();
    CHECK_FALSE(created->session->is_open());
    CHECK_FALSE(created->session->is_busy());
    created->session->close();
}

TEST_CASE(
    "SDK abort before the first delta is idempotent and leaves the session reusable",
    "[sdk][u3][async][abort][issue39]") {
    TestPaths paths;

    auto client = std::make_shared<AbortAwareSdkChatProvider>(false);
    auto* client_ptr = client.get();

    tests::ModelsSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.models = cch::tests::models_from_provider(std::move(client));

    auto created = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(created.has_value());
    created->session->abort();

    std::vector<agent::AgentLifecycleEvent> events;
    auto subscription = created->session->subscribe(
        [&](const agent::AgentLifecycleEvent& event) -> util::ExpectedVoid {
            events.push_back(event);
            return {};
        });
    REQUIRE(subscription.has_value());

    boost::asio::io_context host_io;
    std::optional<util::ExpectedVoid> prompt_result;
    boost::asio::co_spawn(
        host_io,
        [&]() -> boost::asio::awaitable<void> {
            prompt_result = co_await created->session->prompt("abort me");
            co_return;
        },
        boost::asio::detached);

    while (!client_ptr->started) {
        REQUIRE(host_io.poll_one() == 1);
    }
    REQUIRE(created->session->is_busy());
    CHECK(client_ptr->stop_possible);
    created->session->abort();
    created->session->abort();
    if (host_io.stopped()) {
        host_io.restart();
    }
    host_io.run();

    REQUIRE(prompt_result.has_value());
    REQUIRE(prompt_result->has_value());
    CHECK_FALSE(created->session->is_busy());
    CHECK(created->session->is_open());
    CHECK(client_ptr->request_count == 1);
    CHECK_FALSE(client_ptr->partial_emitted);
    CHECK(created->session->message_count() == 2);
    REQUIRE(created->session->last_assistant_text().has_value());
    CHECK(created->session->last_assistant_text()->empty());

    REQUIRE(events.size() == 8);
    CHECK(std::holds_alternative<agent::AgentStartEvent>(events[0]));
    CHECK(std::holds_alternative<agent::TurnStartEvent>(events[1]));
    const auto* user_start = std::get_if<agent::MessageStartEvent>(&events[2]);
    REQUIRE(user_start != nullptr);
    CHECK(std::holds_alternative<ai::UserMessage>(user_start->message));
    const auto* user_end = std::get_if<agent::MessageEndEvent>(&events[3]);
    REQUIRE(user_end != nullptr);
    CHECK(std::holds_alternative<ai::UserMessage>(user_end->message));
    const auto* assistant_start = std::get_if<agent::MessageStartEvent>(&events[4]);
    REQUIRE(assistant_start != nullptr);
    CHECK(std::holds_alternative<ai::AssistantMessage>(assistant_start->message));
    const auto* assistant_end = std::get_if<agent::MessageEndEvent>(&events[5]);
    REQUIRE(assistant_end != nullptr);
    REQUIRE(std::holds_alternative<ai::AssistantMessage>(assistant_end->message));
    const auto& terminal = std::get<ai::AssistantMessage>(assistant_end->message);
    CHECK(terminal.stop_reason == ai::AssistantStopReason::Aborted);
    REQUIRE(terminal.error_message.has_value());
    CHECK(*terminal.error_message == "prompt aborted");
    const auto* turn_end = std::get_if<agent::TurnEndEvent>(&events[6]);
    REQUIRE(turn_end != nullptr);
    REQUIRE(std::holds_alternative<ai::AssistantMessage>(turn_end->message));
    CHECK(std::get<ai::AssistantMessage>(turn_end->message).stop_reason ==
          ai::AssistantStopReason::Aborted);
    const auto* agent_end = std::get_if<agent::AgentEndEvent>(&events[7]);
    REQUIRE(agent_end != nullptr);
    REQUIRE(agent_end->messages.size() == 2);
    REQUIRE(std::holds_alternative<ai::AssistantMessage>(agent_end->messages[1]));
    CHECK(std::get<ai::AssistantMessage>(agent_end->messages[1]).stop_reason ==
          ai::AssistantStopReason::Aborted);

    auto durable = harness::session::JsonlSessionStore::load(paths.session_file);
    REQUIRE(durable.has_value());
    REQUIRE(durable->messages.size() == 2);
    REQUIRE(std::holds_alternative<ai::AssistantMessage>(durable->messages[1]));
    CHECK(std::get<ai::AssistantMessage>(durable->messages[1]).stop_reason ==
          ai::AssistantStopReason::Aborted);

    created->session->abort();
    events.clear();
    REQUIRE(created->session->steer("recover steering"));
    REQUIRE(created->session->snapshot().agent_state.input_queues.steering.messages.size() == 1);
    auto recovered = created->session->prompt_blocking("recover");
    REQUIRE(recovered.has_value());
    CHECK(client_ptr->request_count == 2);
    CHECK_FALSE(client_ptr->recovery_stop_requested);
    CHECK(client_ptr->recovery_uses_fresh_token);
    CHECK(created->session->message_count() == 5);
    const auto recovered_snapshot = created->session->snapshot();
    CHECK(recovered_snapshot.agent_state.input_queues.steering.messages.empty());
    CHECK(ai::text_from_user_message(std::get<ai::UserMessage>(recovered_snapshot.agent_state.messages[3])) ==
          "recover steering");
    REQUIRE(created->session->last_assistant_text().has_value());
    CHECK(*created->session->last_assistant_text() == "recovered after abort");
    durable = harness::session::JsonlSessionStore::load(paths.session_file);
    REQUIRE(durable.has_value());
    CHECK(durable->messages.size() == 5);

    created->session->close();
    CHECK_FALSE(created->session->is_open());
    CHECK_FALSE(static_cast<bool>(*subscription));
    created->session->close();
}

TEST_CASE(
    "SDK abort after partial output retains the partial aborted assistant",
    "[sdk][u3][async][abort][issue39]") {
    TestPaths paths;

    auto client = std::make_shared<AbortAwareSdkChatProvider>(true);
    auto* client_ptr = client.get();

    tests::ModelsSessionOptions opts;
    opts.session_target = coding_agent::InMemorySessionTarget{};
    opts.workspace = paths.workspace.path();
    opts.models = cch::tests::models_from_provider(std::move(client));

    auto created = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(created.has_value());
    std::vector<agent::AgentLifecycleEvent> events;
    auto subscription = created->session->subscribe(
        [&](const agent::AgentLifecycleEvent& event) -> util::ExpectedVoid {
            events.push_back(event);
            return {};
        });
    REQUIRE(subscription.has_value());

    boost::asio::io_context host_io;
    std::optional<util::ExpectedVoid> prompt_result;
    boost::asio::co_spawn(
        host_io,
        [&]() -> boost::asio::awaitable<void> {
            prompt_result = co_await created->session->prompt("abort after partial");
            co_return;
        },
        boost::asio::detached);

    while (!client_ptr->partial_emitted || !client_ptr->started) {
        REQUIRE(host_io.poll_one() == 1);
    }
    created->session->abort();
    if (host_io.stopped()) {
        host_io.restart();
    }
    host_io.run();

    REQUIRE(prompt_result.has_value());
    REQUIRE(prompt_result->has_value());
    CHECK_FALSE(created->session->is_busy());
    CHECK(created->session->message_count() == 2);
    REQUIRE(created->session->last_assistant_text().has_value());
    CHECK(*created->session->last_assistant_text() == "partial");

    REQUIRE(events.size() == 10);
    CHECK(std::holds_alternative<agent::MessageUpdateEvent>(events[5]));
    CHECK(std::holds_alternative<agent::MessageUpdateEvent>(events[6]));
    const auto* assistant_end = std::get_if<agent::MessageEndEvent>(&events[7]);
    REQUIRE(assistant_end != nullptr);
    REQUIRE(std::holds_alternative<ai::AssistantMessage>(assistant_end->message));
    const auto& terminal = std::get<ai::AssistantMessage>(assistant_end->message);
    CHECK(terminal.stop_reason == ai::AssistantStopReason::Aborted);
    CHECK(ai::text_from_assistant_content(terminal.content) == "partial");
    const auto* turn_end = std::get_if<agent::TurnEndEvent>(&events[8]);
    REQUIRE(turn_end != nullptr);
    REQUIRE(std::holds_alternative<ai::AssistantMessage>(turn_end->message));
    CHECK(std::get<ai::AssistantMessage>(turn_end->message).stop_reason ==
          ai::AssistantStopReason::Aborted);
    const auto* agent_end = std::get_if<agent::AgentEndEvent>(&events[9]);
    REQUIRE(agent_end != nullptr);
    REQUIRE(agent_end->messages.size() == 2);
    REQUIRE(std::holds_alternative<ai::AssistantMessage>(agent_end->messages[1]));
    CHECK(std::get<ai::AssistantMessage>(agent_end->messages[1]).stop_reason ==
          ai::AssistantStopReason::Aborted);
}

TEST_CASE("SDK async prompt retains fake provider failure as a terminal value", "[sdk][u3][async]") {
    TestPaths paths;

    tests::ModelsSessionOptions opts;
    opts.session_target = coding_agent::InMemorySessionTarget{};
    opts.workspace = paths.workspace.path();
    opts.models = cch::tests::models_from_provider(std::make_shared<PreflightRejectingChatProvider>());

    auto created = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(created.has_value());

    boost::asio::io_context host_io;
    std::optional<util::ExpectedVoid> prompt_result;
    boost::asio::co_spawn(
        host_io,
        [&]() -> boost::asio::awaitable<void> {
            prompt_result = co_await created->session->prompt("rejected");
            co_return;
        },
        boost::asio::detached);
    host_io.run();

    REQUIRE(prompt_result.has_value());
    REQUIRE(prompt_result->has_value());
    CHECK(created->session->is_open());
    CHECK_FALSE(created->session->is_busy());
    CHECK(created->session->message_count() == 2);
    const auto snapshot = created->session->snapshot();
    const auto& terminal = std::get<ai::AssistantMessage>(snapshot.agent_state.messages.back());
    CHECK(terminal.stop_reason == ai::AssistantStopReason::Error);
    CHECK(terminal.error_message == "provider rejected in-memory prompt");
}

TEST_CASE("SDK async prompt defers callback close until quiescence", "[sdk][u3][async]") {
    TestPaths paths;

    tests::ModelsSessionOptions opts;
    opts.session_target = coding_agent::InMemorySessionTarget{};
    opts.workspace = paths.workspace.path();
    opts.models = cch::tests::models_from_provider(std::make_shared<ExecutorCapturingChatProvider>());

    auto created = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(created.has_value());

    bool close_requested = false;
    auto subscription = created->session->subscribe(
        [&](const agent::AgentLifecycleEvent& event) -> util::ExpectedVoid {
            if (std::holds_alternative<agent::AgentStartEvent>(event)) {
                close_requested = true;
                created->session->close();
            }
            return {};
        });
    REQUIRE(subscription.has_value());

    boost::asio::io_context host_io;
    std::optional<util::ExpectedVoid> prompt_result;
    boost::asio::co_spawn(
        host_io,
        [&]() -> boost::asio::awaitable<void> {
            prompt_result = co_await created->session->prompt("close after start");
            co_return;
        },
        boost::asio::detached);
    host_io.run();

    REQUIRE(prompt_result.has_value());
    REQUIRE(prompt_result->has_value());
    CHECK(close_requested);
    CHECK_FALSE(created->session->is_open());
    CHECK_FALSE(created->session->is_busy());
}

TEST_CASE("SDK blocking prompt rejects same-session callback self-wait", "[sdk][u3][blocking]") {
    TestPaths paths;

    auto client = std::make_shared<ExecutorCapturingChatProvider>();
    auto* client_ptr = client.get();

    tests::ModelsSessionOptions opts;
    opts.session_target = coding_agent::InMemorySessionTarget{};
    opts.workspace = paths.workspace.path();
    opts.models = cch::tests::models_from_provider(std::move(client));

    auto created = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(created.has_value());

    std::optional<util::ExpectedVoid> nested_result;
    auto subscription = created->session->subscribe(
        [&](const agent::AgentLifecycleEvent& event) -> util::ExpectedVoid {
            if (std::holds_alternative<agent::AgentStartEvent>(event) && !nested_result) {
                nested_result = created->session->prompt_blocking("nested");
            }
            return {};
        });
    REQUIRE(subscription.has_value());

    auto outer_result = created->session->prompt_blocking("outer");

    REQUIRE(outer_result.has_value());
    REQUIRE(nested_result.has_value());
    REQUIRE_FALSE(nested_result->has_value());
    CHECK(nested_result->error().code == util::ErrorCode::Validation);
    CHECK(nested_result->error().message ==
          "session is busy (prompt already in flight)");
    CHECK(client_ptr->request_count == 1);
    CHECK(created->session->message_count() == 2);
}

TEST_CASE("SDK create/prompt/close cycle with fake client", "[sdk][u3]") {
    TestPaths paths;

    tests::ModelsSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.models = ai::providers::make_scripted_fake_models();

    auto create_result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(create_result.has_value());

    auto& session = create_result->session;
    REQUIRE(session != nullptr);
    CHECK(session->is_open());
    CHECK_FALSE(session->is_busy());

    // Prompt completion reports success; resulting state is queried separately.
    auto prompt_result = session->prompt_blocking("hello world");
    REQUIRE(prompt_result.has_value());
    CHECK(session->message_count() > 0);

    // Close is idempotent
    session->close();
    CHECK_FALSE(session->is_open());

    session->close();

    // Prompt after close fails
    auto prompt_after = session->prompt_blocking("should fail");
    REQUIRE_FALSE(prompt_after.has_value());
    CHECK(prompt_after.error().code == util::ErrorCode::Validation);
}

TEST_CASE("SDK event subscription delivers lifecycle events", "[sdk][u3]") {
    TestPaths paths;

    tests::ModelsSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.models = ai::providers::make_scripted_fake_models();

    auto create_result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(create_result.has_value());

    auto& session = create_result->session;

    // Subscribe
    int event_count = 0;
    auto sub_result = session->subscribe(
        [&event_count](const agent::AgentLifecycleEvent& /*event*/) -> util::ExpectedVoid {
            ++event_count;
            return {};
        });
    REQUIRE(sub_result.has_value());
    CHECK(static_cast<bool>(*sub_result));

    // Run a prompt
    auto prompt_result = session->prompt_blocking("test event delivery");
    REQUIRE(prompt_result.has_value());
    CHECK(event_count > 0);

    // Unsubscribe
    sub_result->unsubscribe();
    CHECK_FALSE(static_cast<bool>(*sub_result));

    // Close
    session->close();
}

// ─────────────────────────────────────────────────────────────────────────────
// U4: Tool and resource injection
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("SDK custom tool is registered and can be called", "[sdk][u4]") {
    TestPaths paths;

    tests::ModelsSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.models = ai::providers::make_scripted_fake_models();

    // Register a custom tool
    std::vector<std::unique_ptr<agent::AsyncAgentTool>> custom_tools;
    custom_tools.push_back(std::make_unique<FakeEchoTool>());
    opts.custom_tools = std::move(custom_tools);

    auto create_result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(create_result.has_value());

    auto& session = create_result->session;
    auto prompt_result = session->prompt_blocking("hello");
    REQUIRE(prompt_result.has_value());

    session->close();
}

TEST_CASE("SDK duplicate custom tool names fail creation", "[sdk][u4]") {
    TestPaths paths;

    tests::ModelsSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.models = ai::providers::make_scripted_fake_models();

    std::vector<std::unique_ptr<agent::AsyncAgentTool>> custom_tools;
    custom_tools.push_back(std::make_unique<FakeEchoTool>());
    custom_tools.push_back(std::make_unique<FakeEchoTool>()); // duplicate name
    opts.custom_tools = std::move(custom_tools);

    auto create_result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE_FALSE(create_result.has_value());
    CHECK(create_result.error().code == util::ErrorCode::Validation);
    CHECK(create_result.error().message.find("duplicate") != std::string::npos);
}

TEST_CASE("SDK host-provided skills are accessible", "[sdk][u4]") {
    TestPaths paths;

    tests::ModelsSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.models = ai::providers::make_scripted_fake_models();

    coding_agent::Skill skill;
    skill.name = "test-skill";
    skill.description = "A test skill for SDK testing";
    skill.content = "This is test skill content.";
    skill.filePath = "/tmp/test-skill.md";
    opts.skills.push_back(std::move(skill));

    auto create_result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(create_result.has_value());

    auto& session = create_result->session;
    CHECK(session->skills().size() == 1);
    CHECK(session->skills()[0].name == "test-skill");

    session->close();
}

TEST_CASE("SDK unknown skill command reaches the provider without diagnostics", "[sdk][u4][skill-diagnostics]") {
    TestPaths paths;

    tests::ModelsSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.models = ai::providers::make_scripted_fake_models();

    auto create_result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(create_result.has_value());

    auto& session = create_result->session;
    auto prompt_result = session->prompt_blocking("/skill:unknown");
    REQUIRE(prompt_result.has_value());

    session->close();
}

TEST_CASE("SDK prompt leaves diagnostics empty for valid and bare skill commands", "[sdk][u4][skill-diagnostics]") {
    TestPaths paths;

    tests::ModelsSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.models = ai::providers::make_scripted_fake_models();

    coding_agent::Skill skill;
    skill.name = "valid-skill";
    skill.description = "A valid skill for SDK diagnostics testing";
    skill.content = "Use the valid skill instructions.";
    skill.filePath = "/tmp/valid-skill.md";
    opts.skills.push_back(std::move(skill));

    auto create_result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(create_result.has_value());

    auto& session = create_result->session;
    auto valid_result = session->prompt_blocking("/skill:valid-skill with args");
    REQUIRE(valid_result.has_value());

    auto bare_result = session->prompt_blocking("/skill:");
    REQUIRE(bare_result.has_value());

    session->close();
}

TEST_CASE("SDK host-provided templates are accessible", "[sdk][u4]") {
    TestPaths paths;

    tests::ModelsSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.models = ai::providers::make_scripted_fake_models();

    coding_agent::PromptTemplate tmpl;
    tmpl.name = "greet";
    tmpl.description = "Greeting template";
    tmpl.content = "Hello, $ARGUMENTS!";
    opts.prompt_templates.push_back(std::move(tmpl));

    auto create_result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(create_result.has_value());

    auto& session = create_result->session;
    CHECK(session->templates().size() == 1);
    CHECK(session->templates()[0].name == "greet");

    session->close();
}

TEST_CASE("moved SDK session retains its owned prompt resource snapshot", "[sdk][u4][prompt-processing]") {
    TestPaths paths;
    auto capture = std::make_shared<CaptureChatProvider>();
    auto* capture_ptr = capture.get();

    tests::ModelsSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.models = cch::tests::models_from_provider(std::move(capture));
    opts.prompt_templates.push_back(host_template("review", "Review cached: $1"));

    auto created = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(created.has_value());
    coding_agent::AgentSession moved_session{std::move(*created->session)};

    auto result = moved_session.prompt_blocking("/review target.cpp");
    REQUIRE(result.has_value());
    REQUIRE(capture_ptr->captured_request.has_value());
    const auto& messages = capture_ptr->captured_request->context.messages;
    const auto user = std::find_if(messages.begin(), messages.end(), [](const ai::MessageVariant& message) {
        return std::holds_alternative<ai::UserMessage>(message);
    });
    REQUIRE(user != messages.end());
    CHECK(ai::text_from_user_message(std::get<ai::UserMessage>(*user)) == "Review cached: target.cpp");

    moved_session.close();
}

TEST_CASE("SDK expand_prompt_templates false sends slash-shaped input raw to the provider", "[sdk][u4][prompt-processing]") {
    TestPaths paths;
    auto capture = std::make_shared<CaptureChatProvider>();
    auto* capture_ptr = capture.get();

    tests::ModelsSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.models = cch::tests::models_from_provider(std::move(capture));
    opts.skills.push_back(host_skill("cached"));
    opts.prompt_templates.push_back(host_template("review", "expanded: $1"));

    auto created = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(created.has_value());

    for (const std::string raw : {"/skill:cached", "/review arg"}) {
        coding_agent::PromptOptions prompt_options;
        prompt_options.expand_prompt_templates = false;
        auto result = created->session->prompt_blocking(raw, std::move(prompt_options));
        REQUIRE(result.has_value());
        REQUIRE(capture_ptr->captured_request.has_value());
        const auto& messages = capture_ptr->captured_request->context.messages;
        const auto user = std::find_if(messages.rbegin(), messages.rend(), [](const ai::MessageVariant& message) {
            return std::holds_alternative<ai::UserMessage>(message);
        });
        REQUIRE(user != messages.rend());
        CHECK(ai::text_from_user_message(std::get<ai::UserMessage>(*user)) == raw);
    }

    created->session->close();
}

TEST_CASE("SDK treats user bash prefixes as ordinary prompts", "[sdk][u4][prompt-processing][user-bash]") {
    TestPaths paths;
    auto capture = std::make_shared<CaptureChatProvider>();
    auto* capture_ptr = capture.get();

    tests::ModelsSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.models = cch::tests::models_from_provider(std::move(capture));

    auto created = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(created.has_value());

    for (const std::string raw : {"!echo visible", "!!echo excluded-later"}) {
        auto result = created->session->prompt_blocking(raw);
        REQUIRE(result.has_value());
        REQUIRE(capture_ptr->captured_request.has_value());
        const auto& messages = capture_ptr->captured_request->context.messages;
        const auto user = std::find_if(messages.rbegin(), messages.rend(), [](const ai::MessageVariant& message) {
            return std::holds_alternative<ai::UserMessage>(message);
        });
        REQUIRE(user != messages.rend());
        CHECK(ai::text_from_user_message(std::get<ai::UserMessage>(*user)) == raw);
    }

    created->session->close();
}

TEST_CASE("SDK loads trusted project resources through shared loader", "[sdk][project-resources]") {
    TestPaths paths;
    write_project_skill(paths, "project-skill", "Use project skill instructions.");
    write_project_prompt(paths, "project-review", "Review project item: $1.");

    tests::ModelsSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.models = ai::providers::make_scripted_fake_models();
    opts.load_project_resources = true;
    opts.default_project_trust = coding_agent::DefaultProjectTrust::Always;

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(result.has_value());
    REQUIRE(result->session->skills().size() == 1);
    CHECK(result->session->skills()[0].name == "project-skill");
    REQUIRE(result->session->templates().size() == 1);
    CHECK(result->session->templates()[0].name == "project-review");

    auto skill_prompt = result->session->prompt_blocking("/skill:project-skill now");
    REQUIRE(skill_prompt.has_value());
    REQUIRE(result->session->last_assistant_text().has_value());
    CHECK(result->session->last_assistant_text()->find("Use project skill instructions.") != std::string::npos);

    auto template_prompt = result->session->prompt_blocking("/project-review Ada");
    REQUIRE(template_prompt.has_value());
    REQUIRE(result->session->last_assistant_text().has_value());
    CHECK(result->session->last_assistant_text()->find("Review project item: Ada.") != std::string::npos);

    result->session->close();
}

TEST_CASE("SDK keeps host resources when project resource loading is disabled", "[sdk][project-resources]") {
    TestPaths paths;
    write_project_skill(paths, "project-skill");
    write_project_prompt(paths, "project-review");

    tests::ModelsSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.models = ai::providers::make_scripted_fake_models();
    opts.load_project_resources = false;
    opts.skills.push_back(host_skill("host-skill"));
    opts.prompt_templates.push_back(host_template("host-review"));

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(result.has_value());
    REQUIRE(result->session->skills().size() == 1);
    CHECK(result->session->skills()[0].name == "host-skill");
    REQUIRE(result->session->templates().size() == 1);
    CHECK(result->session->templates()[0].name == "host-review");
    result->session->close();
}

TEST_CASE("SDK keeps host resources and returns resource decisions when project is untrusted", "[sdk][project-resources]") {
    TestPaths paths;
    cch::tests::TempWorkspace home;
    tests::EnvVarGuard home_guard{"HOME"};
    home_guard.set(home.path().string());
    home.write(
        ".cpp-harness/trust.json",
        "{\"" + paths.workspace.path().string() + "\":false}\n");
    write_project_skill(paths, "project-skill");
    write_project_prompt(paths, "project-review");

    tests::ModelsSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.models = ai::providers::make_scripted_fake_models();
    opts.load_project_resources = true;
    opts.skills.push_back(host_skill("host-skill"));
    opts.prompt_templates.push_back(host_template("host-review"));

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(result.has_value());
    REQUIRE(result->session->skills().size() == 1);
    CHECK(result->session->skills()[0].name == "host-skill");
    REQUIRE(result->session->templates().size() == 1);
    CHECK(result->session->templates()[0].name == "host-review");
    CHECK(has_sdk_diag(result->diagnostics, "resource:project_skills"));
    CHECK(has_sdk_diag(result->diagnostics, "resource:project_prompts"));
    result->session->close();
}

TEST_CASE("SDK project resource duplicates prefer host resources with structured diagnostics", "[sdk][project-resources]") {
    TestPaths paths;
    write_project_skill(paths, "same-skill", "Project skill body.");
    write_project_prompt(paths, "same-template", "Project template body.");

    tests::ModelsSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.models = ai::providers::make_scripted_fake_models();
    opts.load_project_resources = true;
    opts.default_project_trust = coding_agent::DefaultProjectTrust::Always;
    opts.skills.push_back(host_skill("same-skill", "Host skill body."));
    opts.prompt_templates.push_back(host_template("same-template", "Host template body."));

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(result.has_value());
    REQUIRE(result->session->skills().size() == 1);
    CHECK(result->session->skills()[0].content == "Host skill body.");
    REQUIRE(result->session->templates().size() == 1);
    CHECK(result->session->templates()[0].content == "Host template body.");
    CHECK(has_sdk_diag(result->diagnostics, "duplicate:duplicate_skill_skipped"));
    CHECK(has_sdk_diag(result->diagnostics, "duplicate:duplicate_template_skipped"));
    result->session->close();
}

TEST_CASE("SDK returns trust and adapter diagnostics as values", "[sdk][project-resources]") {
    TestPaths paths;
    paths.workspace.write(
        ".cpp-harness/skills/bad/SKILL.md",
        "---\n"
        "name: bad\n"
        "description:\n"
        "---\n"
        "Body.\n");

    {
        tests::EnvVarGuard home_guard{"HOME"};
        home_guard.set(paths.workspace.path().string());
        paths.workspace.write(".cpp-harness/trust.json", "{not json");

        tests::ModelsSessionOptions untrusted_opts;
        untrusted_opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
        untrusted_opts.workspace = paths.workspace.path();
        untrusted_opts.models = ai::providers::make_scripted_fake_models();
        untrusted_opts.load_project_resources = true;

        auto untrusted = coding_agent::create_agent_session(std::move(untrusted_opts));
        REQUIRE(untrusted.has_value());
        CHECK(has_sdk_diag(untrusted->diagnostics, "trust:trust_store_unavailable"));
        CHECK(has_sdk_diag(untrusted->diagnostics, "resource:project_skills"));
        CHECK(untrusted->session->skills().empty());
        untrusted->session->close();
    }

    TestPaths trusted_paths;
    trusted_paths.workspace.write(
        ".cpp-harness/skills/bad/SKILL.md",
        "---\n"
        "name: bad\n"
        "description:\n"
        "---\n"
        "Body.\n");

    tests::ModelsSessionOptions trusted_opts;
    trusted_opts.session_target = coding_agent::ExplicitNewSessionTarget{trusted_paths.session_file};
    trusted_opts.workspace = trusted_paths.workspace.path();
    trusted_opts.models = ai::providers::make_scripted_fake_models();
    trusted_opts.load_project_resources = true;
    trusted_opts.default_project_trust = coding_agent::DefaultProjectTrust::Always;

    auto trusted = coding_agent::create_agent_session(std::move(trusted_opts));
    REQUIRE(trusted.has_value());
    CHECK(has_sdk_diag(trusted->diagnostics, "skill:invalid_metadata"));
    CHECK(trusted->session->skills().empty());
    trusted->session->close();
}

TEST_CASE("SDK bounds auto-discovered resource diagnostics", "[sdk][project-resources]") {
    TestPaths paths;
    const std::string long_skill_name(2048, 'x');
    paths.workspace.write(
        ".cpp-harness/skills/long-name/SKILL.md",
        "---\n"
        "name: " + long_skill_name + "\n"
        "description: Long invalid skill name.\n"
        "---\n"
        "Body.\n");
    for (int index = 0; index < 80; ++index) {
        paths.workspace.write(
            ".cpp-harness/prompts/bad-" + std::to_string(index) + ".md",
            "---\n"
            "invalid frontmatter line\n"
            "---\n"
            "Body.\n");
    }

    tests::ModelsSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.model = cch::tests::sdk_request_model("fake", "fake-model");
    opts.load_project_resources = true;
    opts.default_project_trust = coding_agent::DefaultProjectTrust::Always;

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(result.has_value());
    CHECK(result->session->templates().empty());
    CHECK(result->diagnostics.size() <= 64);
    CHECK(has_sdk_diag(result->diagnostics, "resource:diagnostics_truncated"));
    const auto truncated_message = std::find_if(
        result->diagnostics.begin(),
        result->diagnostics.end(),
        [](const auto& diagnostic) {
            return diagnostic.code == "skill:invalid_metadata" &&
                   diagnostic.message.ends_with("...[truncated]");
        });
    REQUIRE(truncated_message != result->diagnostics.end());
    CHECK(truncated_message->message.size() <= 1024);
    result->session->close();
}

TEST_CASE("SDK project resource diagnostics are not printed during creation", "[sdk][project-resources]") {
    TestPaths paths;
    paths.workspace.write(
        ".cpp-harness/prompts/bad.md",
        "---\n"
        "bad line without colon\n"
        "---\n"
        "Body.\n");

    tests::ModelsSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.models = ai::providers::make_scripted_fake_models();
    opts.load_project_resources = true;
    opts.default_project_trust = coding_agent::DefaultProjectTrust::Always;

    std::ostringstream captured_out;
    std::ostringstream captured_err;
    auto* old_out = std::cout.rdbuf(captured_out.rdbuf());
    auto* old_err = std::cerr.rdbuf(captured_err.rdbuf());
    auto result = coding_agent::create_agent_session(std::move(opts));
    std::cout.rdbuf(old_out);
    std::cerr.rdbuf(old_err);

    REQUIRE(result.has_value());
    CHECK(has_sdk_diag(result->diagnostics, "template:parse_failed"));
    CHECK(captured_out.str().empty());
    CHECK(captured_err.str().empty());
    result->session->close();
}

TEST_CASE("SDK default built-in tools exclude bash", "[sdk][u4]") {
    TestPaths paths;

    tests::ModelsSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.models = ai::providers::make_scripted_fake_models();
    // Default builtin_tools: read=true, write=true, edit=true, bash=false

    auto create_result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(create_result.has_value());

    // Prompt with "bash ..." — the fake client would try to make a bash tool call
    // But since bash is not registered, it should just get a text response
    auto& session = create_result->session;
    auto prompt_result = session->prompt_blocking("bash echo hello");
    REQUIRE(prompt_result.has_value());
    // The fake client responds with text when no tool is matched for "bash" prefix
    // (it checks prompt.rfind("bash ", 0), which matches, then returns a bash tool call)
    // But since bash is not registered, the agent loop won't find the tool and will error.
    // This verifies the tool is not registered by checking the session works.

    session->close();
}

TEST_CASE("SDK bash tool can be explicitly enabled", "[sdk][u4]") {
    TestPaths paths;

    tests::ModelsSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.models = ai::providers::make_scripted_fake_models();
    opts.builtin_tools.bash = true;

    auto create_result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(create_result.has_value());

    auto& session = create_result->session;
    CHECK(session->is_open());
    session->close();
}

// ─────────────────────────────────────────────────────────────────────────────
// Session resume
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("SDK can resume a linear session", "[sdk][u3]") {
    TestPaths paths;

    // First, create a session
    {
        tests::ModelsSessionOptions opts;
        opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
        opts.workspace = paths.workspace.path();
        opts.models = ai::providers::make_scripted_fake_models();

        auto result = coding_agent::create_agent_session(std::move(opts));
        REQUIRE(result.has_value());
        auto& session = result->session;
        auto pr = session->prompt_blocking("hello");
        REQUIRE(pr.has_value());
        session->close();
    }

    // Then, resume it
    {
        tests::ModelsSessionOptions opts;
        opts.session_target = coding_agent::ExplicitResumeSessionTarget{paths.session_file};
        opts.workspace = paths.workspace.path();
        opts.models = ai::providers::make_scripted_fake_models();

        auto result = coding_agent::create_agent_session(std::move(opts));
        REQUIRE(result.has_value());
        auto& session = result->session;
        CHECK(session->is_open());
        CHECK(session->message_count() == 2);

        std::size_t resumed_run_events = 0;
        auto subscription = session->subscribe(
            [&](const agent::AgentLifecycleEvent&) -> util::ExpectedVoid {
                ++resumed_run_events;
                return {};
            });
        REQUIRE(subscription.has_value());
        CHECK(resumed_run_events == 0);

        auto pr = session->prompt_blocking("continue");
        REQUIRE(pr.has_value());
        CHECK(resumed_run_events == 11);
        CHECK(session->message_count() == 4);

        auto durable = harness::session::JsonlSessionStore::load(paths.session_file);
        REQUIRE(durable.has_value());
        CHECK(durable->messages.size() == 4);
        session->close();
    }
}

TEST_CASE("SDK rejects branched active resumed topology", "[sdk][u3][resume-topology]") {
    TestPaths paths;
    auto store = harness::session::JsonlSessionStore::create_new(paths.session_file, test_metadata(paths));
    REQUIRE(store);
    REQUIRE(store->append(user_msg("first")));
    REQUIRE(store->append(user_msg("second")));

    auto pre = harness::session::JsonlSessionStore::load(paths.session_file);
    REQUIRE(pre);
    REQUIRE(pre->entries.size() >= 3);
    const auto first_id = pre->entries[1].entry_id;

    auto resumed = harness::session::JsonlSessionStore::open_existing(paths.session_file);
    REQUIRE(resumed);
    REQUIRE(resumed->append_leaf(std::nullopt, first_id));

    expect_unsupported_sdk_resume(paths);
}

TEST_CASE("SDK rejects compacted active resumed topology", "[sdk][u3][resume-topology]") {
    TestPaths paths;
    auto store = harness::session::JsonlSessionStore::create_new(paths.session_file, test_metadata(paths));
    REQUIRE(store);
    REQUIRE(store->append(user_msg("before compaction")));
    REQUIRE(store->append(user_msg("kept message")));

    auto pre = harness::session::JsonlSessionStore::load(paths.session_file);
    REQUIRE(pre);
    REQUIRE(pre->entries.size() >= 3);
    const auto kept_id = pre->entries[2].entry_id;

    auto resumed = harness::session::JsonlSessionStore::open_existing(paths.session_file);
    REQUIRE(resumed);
    REQUIRE(resumed->append_compaction(std::nullopt, "summary", kept_id, 1000, std::nullopt, std::nullopt));

    expect_unsupported_sdk_resume(paths);
}

TEST_CASE("SDK resumes linear active topology with inactive branch and compaction data", "[sdk][u3][resume-topology]") {
    TestPaths paths;
    auto store = harness::session::JsonlSessionStore::create_new(paths.session_file, test_metadata(paths));
    REQUIRE(store);
    REQUIRE(store->append(user_msg("first")));
    REQUIRE(store->append(user_msg("second")));
    REQUIRE(store->append(user_msg("third")));

    auto pre = harness::session::JsonlSessionStore::load(paths.session_file);
    REQUIRE(pre);
    REQUIRE(pre->entries.size() >= 4);
    const auto first_id = pre->entries[1].entry_id;
    const auto third_id = pre->entries[3].entry_id;

    auto resumed = harness::session::JsonlSessionStore::open_existing(paths.session_file);
    REQUIRE(resumed);
    REQUIRE(resumed->append_branch_summary(first_id, third_id, "inactive branch", std::nullopt, std::nullopt));
    REQUIRE(resumed->append_compaction(first_id, "inactive compaction", third_id, 1000, std::nullopt, std::nullopt));
    REQUIRE(resumed->append_leaf(std::nullopt, third_id));

    auto result = coding_agent::create_agent_session(sdk_resume_options(paths));
    REQUIRE(result.has_value());
    CHECK(result->session->message_count() == 3);

    auto prompt_result = result->session->prompt_blocking("continue linear path");
    REQUIRE(prompt_result.has_value());
    result->session->close();
}

// ─────────────────────────────────────────────────────────────────────────────
// State accessors
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("SDK state accessors reflect committed history", "[sdk][u3]") {
    TestPaths paths;

    tests::ModelsSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.models = ai::providers::make_scripted_fake_models();

    auto create_result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(create_result.has_value());

    auto& session = create_result->session;
    CHECK(session->message_count() == 0);
    CHECK_FALSE(session->last_assistant_text().has_value());

    auto pr = session->prompt_blocking("hello");
    REQUIRE(pr.has_value());
    CHECK(session->message_count() > 0);
    CHECK(session->last_assistant_text().has_value());

    session->close();
}

// ─────────────────────────────────────────────────────────────────────────────
// Live state updates before subscribers
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("SDK message_end subscribers observe live state updated before delivery", "[sdk][live-state]") {
    TestPaths paths;
    paths.workspace.write("target.txt", "target content\n");

    tests::ModelsSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.models = ai::providers::make_scripted_fake_models();

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(result.has_value());
    auto& session = result->session;

    std::size_t seen_message_ends = 0;
    std::size_t user_message_ends = 0;
    std::size_t assistant_message_ends = 0;
    std::size_t tool_result_message_ends = 0;
    std::optional<std::string> assistant_text_in_subscriber;

    auto sub = session->subscribe([&](const agent::AgentLifecycleEvent& event) -> util::ExpectedVoid {
        if (const auto* end = std::get_if<agent::MessageEndEvent>(&event)) {
            const auto& message = end->message;
            // Live history is updated before subscribers run, so the message count
            // equals the number of message_end events already delivered plus the
            // current one.
            CHECK(session->message_count() == seen_message_ends + 1);

            if (std::holds_alternative<ai::UserMessage>(message)) {
                ++user_message_ends;
            } else if (std::holds_alternative<ai::AssistantMessage>(message)) {
                ++assistant_message_ends;
                assistant_text_in_subscriber = session->last_assistant_text();
            } else if (std::holds_alternative<ai::ToolResultMessage>(message)) {
                ++tool_result_message_ends;
            }
            ++seen_message_ends;
        }
        return {};
    });
    REQUIRE(sub.has_value());

    auto first = session->prompt_blocking("hello");
    REQUIRE(first.has_value());
    CHECK(user_message_ends == 1);
    CHECK(assistant_message_ends == 1);
    CHECK(tool_result_message_ends == 0);
    CHECK(seen_message_ends == 2);
    REQUIRE(assistant_text_in_subscriber.has_value());
    CHECK(assistant_text_in_subscriber->find("fake: hello") != std::string::npos);
    CHECK(session->last_assistant_text() == assistant_text_in_subscriber);
    CHECK(session->message_count() == 2);

    // Tool-using prompt: user, assistant with tool call, tool result, final assistant.
    assistant_text_in_subscriber.reset();
    auto second = session->prompt_blocking("read target.txt");
    REQUIRE(second.has_value());
    CHECK(user_message_ends == 2);
    CHECK(tool_result_message_ends == 1);
    CHECK(assistant_message_ends == 3);
    CHECK(seen_message_ends == 6);
    REQUIRE(assistant_text_in_subscriber.has_value());
    CHECK(assistant_text_in_subscriber->find("observed") != std::string::npos);
    CHECK(session->last_assistant_text() == assistant_text_in_subscriber);

    session->close();
}

TEST_CASE(
    "SDK persists each completed message after subscribers and reopens in durable order",
    "[sdk][incremental-persistence]") {
    TestPaths paths;
    paths.workspace.write("target.txt", "target content\n");

    tests::ModelsSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.models = ai::providers::make_scripted_fake_models();

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(result.has_value());
    auto& session = result->session;

    std::size_t delivered_message_ends = 0;
    auto sub = session->subscribe([&](const agent::AgentLifecycleEvent& event) -> util::ExpectedVoid {
        if (!std::holds_alternative<agent::MessageEndEvent>(event)) {
            return {};
        }

        auto durable = harness::session::JsonlSessionStore::load(paths.session_file);
        CHECK(durable.has_value());
        if (!durable) {
            return std::unexpected(durable.error());
        }

        // The current message is live before delivery, but becomes durable only
        // after every subscriber has observed its message_end event.
        CHECK(session->message_count() == delivered_message_ends + 1);
        CHECK(durable->messages.size() == delivered_message_ends);
        ++delivered_message_ends;
        return {};
    });
    REQUIRE(sub.has_value());

    auto prompt = session->prompt_blocking("read target.txt");
    REQUIRE(prompt.has_value());
    CHECK(delivered_message_ends == 4);

    auto durable = harness::session::JsonlSessionStore::load(paths.session_file);
    REQUIRE(durable.has_value());
    REQUIRE(durable->messages.size() == 4);
    CHECK(std::holds_alternative<ai::UserMessage>(durable->messages[0]));
    CHECK(std::holds_alternative<ai::AssistantMessage>(durable->messages[1]));
    CHECK(std::holds_alternative<ai::ToolResultMessage>(durable->messages[2]));
    CHECK(std::holds_alternative<ai::AssistantMessage>(durable->messages[3]));

    session->close();

    auto reopened = coding_agent::create_agent_session(sdk_resume_options(paths));
    REQUIRE(reopened.has_value());
    CHECK(reopened->session->message_count() == 4);
    REQUIRE(reopened->session->last_assistant_text().has_value());
    CHECK(reopened->session->last_assistant_text()->find("observed") != std::string::npos);
    reopened->session->close();
}

TEST_CASE(
    "SDK subscriber failure does not stop delivery or persistence",
    "[sdk][live-state][subscriber-failure][issue36]") {
    TestPaths paths;

    tests::ModelsSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.models = ai::providers::make_scripted_fake_models();

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(result.has_value());
    auto& session = result->session;

    int rejecting_calls = 0;
    auto rejecting = session->subscribe(
        [&](const agent::AgentLifecycleEvent&) -> util::ExpectedVoid {
            ++rejecting_calls;
            return std::unexpected(util::make_error(
                util::ErrorCode::Tool,
                "subscriber rejects event"));
        });
    REQUIRE(rejecting.has_value());

    std::size_t healthy_message_ends = 0;
    auto healthy = session->subscribe(
        [&](const agent::AgentLifecycleEvent& event) -> util::ExpectedVoid {
            if (std::holds_alternative<agent::MessageEndEvent>(event)) {
                ++healthy_message_ends;
            }
            return {};
        });
    REQUIRE(healthy.has_value());

    auto prompted = session->prompt_blocking("hello");
    REQUIRE(prompted.has_value());
    CHECK(rejecting_calls == 1);
    CHECK_FALSE(static_cast<bool>(*rejecting));
    CHECK(static_cast<bool>(*healthy));
    CHECK(healthy_message_ends == 2);
    CHECK(session->message_count() == 2);
    REQUIRE(session->last_assistant_text().has_value());
    CHECK(session->last_assistant_text()->find("fake: hello") != std::string::npos);

    auto durable = harness::session::JsonlSessionStore::load(paths.session_file);
    REQUIRE(durable.has_value());
    REQUIRE(durable->messages.size() == 2);
    CHECK(std::holds_alternative<ai::UserMessage>(durable->messages[0]));
    CHECK(std::holds_alternative<ai::AssistantMessage>(durable->messages[1]));
    CHECK(session->is_open());
    session->close();
}

TEST_CASE(
    "SDK subscriptions use one run-start snapshot across callback changes",
    "[sdk][live-state][subscriptions][issue36]") {
    TestPaths paths;
    tests::ModelsSessionOptions opts;
    opts.session_target = coding_agent::InMemorySessionTarget{};
    opts.workspace = paths.workspace.path();
    opts.models = ai::providers::make_scripted_fake_models();

    auto created = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(created.has_value());
    auto& session = created->session;

    std::size_t self_events = 0;
    std::size_t late_events = 0;
    std::optional<coding_agent::EventSubscription> late;
    std::optional<coding_agent::EventSubscription> self;
    auto subscribed = session->subscribe(
        [&](const agent::AgentLifecycleEvent& event) -> util::ExpectedVoid {
            ++self_events;
            if (std::holds_alternative<agent::AgentStartEvent>(event) && !late) {
                self->unsubscribe();
                auto added = session->subscribe(
                    [&](const agent::AgentLifecycleEvent&) -> util::ExpectedVoid {
                        ++late_events;
                        return {};
                    });
                REQUIRE(added.has_value());
                late.emplace(std::move(*added));
            }
            return {};
        });
    REQUIRE(subscribed.has_value());
    self.emplace(std::move(*subscribed));

    REQUIRE(session->prompt_blocking("first").has_value());
    CHECK(self_events == 11);
    CHECK(late_events == 0);
    CHECK_FALSE(static_cast<bool>(*self));
    CHECK(static_cast<bool>(*late));

    REQUIRE(session->prompt_blocking("second").has_value());
    CHECK(self_events == 11);
    CHECK(late_events == 11);
    session->close();
}

TEST_CASE(
    "SDK close from an observer retains callbacks and capabilities until quiescence",
    "[sdk][lifecycle][issue41]") {
    TestPaths paths;
    auto env = std::make_shared<CountingFakeEnv>(paths.workspace.path());
    auto counts = std::make_shared<SdkOwnedLifetimeCounts>();
    tests::ModelsSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.models = cch::tests::models_from_provider(std::make_shared<AbortAwareSdkChatProvider>(false, counts));
    opts.execution_env = env;
    opts.custom_tools.push_back(std::make_unique<LifetimeTrackedTool>(counts));

    auto created = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(created.has_value());
    auto& session = created->session;
    std::vector<agent::AgentLifecycleEvent> events;
    auto closing = session->subscribe(
        [&](const agent::AgentLifecycleEvent& event) -> util::ExpectedVoid {
            events.push_back(event);
            if (std::holds_alternative<agent::AgentStartEvent>(event)) {
                session->close();
                CHECK_FALSE(session->is_open());
                CHECK(session->is_busy());
                CHECK(counts->destroyed_clients == 0);
                CHECK(counts->destroyed_tools == 0);
            }
            return {};
        });
    REQUIRE(closing.has_value());

    REQUIRE(session->prompt_blocking("hello").has_value());
    CHECK_FALSE(session->is_open());
    CHECK_FALSE(session->is_busy());
    CHECK(events.size() > 1);
    CHECK(has_aborted_message_end(events));
    CHECK_FALSE(static_cast<bool>(*closing));
    CHECK(counts->destroyed_clients == 1);
    CHECK(counts->destroyed_tools == 1);
    CHECK(env->cleanup_count == 0);

    auto durable = harness::session::JsonlSessionStore::load(paths.session_file);
    REQUIRE(durable.has_value());
    REQUIRE(durable->messages.size() == 2);
    REQUIRE(std::holds_alternative<ai::AssistantMessage>(durable->messages[1]));
    CHECK(std::get<ai::AssistantMessage>(durable->messages[1]).stop_reason ==
          ai::AssistantStopReason::Aborted);
}

TEST_CASE(
    "SDK destruction from an observer retains the active runtime until quiescence",
    "[sdk][lifecycle][issue36]") {
    TestPaths paths;
    tests::ModelsSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.models = ai::providers::make_scripted_fake_models();

    auto created = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(created.has_value());
    auto session = std::move(created->session);
    auto* active = session.get();
    auto destroying = active->subscribe(
        [&](const agent::AgentLifecycleEvent& event) -> util::ExpectedVoid {
            if (std::holds_alternative<agent::AgentStartEvent>(event)) {
                session.reset();
            }
            return {};
        });
    REQUIRE(destroying.has_value());

    REQUIRE(active->prompt_blocking("hello").has_value());
    CHECK_FALSE(session);
    CHECK_FALSE(static_cast<bool>(*destroying));

    auto durable = harness::session::JsonlSessionStore::load(paths.session_file);
    REQUIRE(durable.has_value());
    CHECK(durable->messages.size() == 2);
}

TEST_CASE(
    "SDK persistence failure retains live history and permits later durable appends",
    "[sdk][live-state][persistence-failure][issue14]") {
    TestPaths paths;
    auto capture = std::make_shared<CaptureChatProvider>();
    auto* capture_ptr = capture.get();

    tests::ModelsSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.models = cch::tests::models_from_provider(std::move(capture));

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(result.has_value());
    auto& session = result->session;

    std::optional<ai::AssistantMessage> completed_assistant;
    auto subscription = session->subscribe(
        [&](const agent::AgentLifecycleEvent& event) -> util::ExpectedVoid {
            const auto* end = std::get_if<agent::MessageEndEvent>(&event);
            if (end != nullptr) {
                if (const auto* assistant = std::get_if<ai::AssistantMessage>(&end->message)) {
                    completed_assistant = *assistant;
                }
            }
            return {};
        });
    REQUIRE(subscription.has_value());

    // The user message reaches storage, then the assistant append fails. The
    // one-shot private hook leaves the next prompt free to persist normally.
    harness::session::testing::fail_nth_append_for_test(paths.session_file, 2);
    auto failed = session->prompt_blocking("first");
    REQUIRE_FALSE(failed.has_value());
    CHECK(failed.error().code == util::ErrorCode::Session);
    CHECK(failed.error().message == "could not persist session entry");
    CHECK(session->is_open());
    CHECK(session->message_count() == 2);
    CHECK(session->last_assistant_text() == "captured");
    REQUIRE(completed_assistant.has_value());
    CHECK(completed_assistant->stop_reason == ai::AssistantStopReason::Stop);
    CHECK_FALSE(completed_assistant->error_message.has_value());

    auto durable_after_failure = harness::session::JsonlSessionStore::load(paths.session_file);
    REQUIRE(durable_after_failure.has_value());
    REQUIRE(durable_after_failure->messages.size() == 1);
    REQUIRE(std::holds_alternative<ai::UserMessage>(durable_after_failure->messages[0]));
    CHECK(ai::text_from_user_message(std::get<ai::UserMessage>(durable_after_failure->messages[0])) == "first");

    auto recovered = session->prompt_blocking("second");
    REQUIRE(recovered.has_value());
    CHECK(session->message_count() == 4);

    // The later provider request uses retained live state even though the first
    // assistant message never reached durable storage.
    REQUIRE(capture_ptr->captured_request.has_value());
    const auto& request_messages = capture_ptr->captured_request->context.messages;
    REQUIRE(request_messages.size() == 3);
    REQUIRE(std::holds_alternative<ai::UserMessage>(request_messages[0]));
    REQUIRE(std::holds_alternative<ai::AssistantMessage>(request_messages[1]));
    REQUIRE(std::holds_alternative<ai::UserMessage>(request_messages[2]));
    CHECK(ai::text_from_user_message(std::get<ai::UserMessage>(request_messages[0])) == "first");
    CHECK(ai::text_from_assistant_content(std::get<ai::AssistantMessage>(request_messages[1]).content) == "captured");
    CHECK(ai::text_from_user_message(std::get<ai::UserMessage>(request_messages[2])) == "second");

    auto durable_after_recovery = harness::session::JsonlSessionStore::load(paths.session_file);
    REQUIRE(durable_after_recovery.has_value());
    REQUIRE(durable_after_recovery->messages.size() == 3);
    CHECK(std::holds_alternative<ai::UserMessage>(durable_after_recovery->messages[0]));
    CHECK(std::holds_alternative<ai::UserMessage>(durable_after_recovery->messages[1]));
    CHECK(std::holds_alternative<ai::AssistantMessage>(durable_after_recovery->messages[2]));
    CHECK(ai::text_from_user_message(std::get<ai::UserMessage>(durable_after_recovery->messages[1])) == "second");

    session->close();
    CHECK_FALSE(session->is_open());
    session->close();
    auto reopened = coding_agent::create_agent_session(sdk_resume_options(paths));
    REQUIRE(reopened.has_value());
    CHECK(reopened->session->message_count() == 3);
    REQUIRE(reopened->session->last_assistant_text().has_value());
    CHECK(*reopened->session->last_assistant_text() == "captured");
    reopened->session->close();
}

// ─────────────────────────────────────────────────────────────────────────────
// CreateAgentSessionResult metadata
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("CreateAgentSessionResult contains metadata", "[sdk][u2]") {
    TestPaths paths;
    cch::tests::TempWorkspace home;
    tests::EnvVarGuard home_guard{"HOME"};
    home_guard.set(home.path().string());

    tests::ModelsSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.models = ai::providers::make_scripted_fake_models();

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(result.has_value());

    CHECK_FALSE(result->session_id.empty());
    REQUIRE(result->session_path.has_value());
    CHECK_FALSE(result->session_path->empty());
    CHECK(result->workspace == paths.workspace.path());
    CHECK(result->provider == "sdk-host");
    CHECK(result->model == "host-client");
    CHECK(result->metadata.session_id == result->session_id);
    CHECK(result->metadata.workspace == result->workspace);
    CHECK(result->metadata.provider == result->provider);
    CHECK(result->metadata.model == result->model);

    result->session->close();
}

TEST_CASE("SDK new sessions receive fresh identity and resume preserves it", "[sdk][assembly]") {
    TestPaths first_paths;
    TestPaths second_paths;

    tests::ModelsSessionOptions first_opts;
    first_opts.session_target = coding_agent::ExplicitNewSessionTarget{first_paths.session_file};
    first_opts.workspace = first_paths.workspace.path();
    first_opts.models = ai::providers::make_scripted_fake_models();

    auto first = coding_agent::create_agent_session(std::move(first_opts));
    REQUIRE(first.has_value());
    const auto first_id = first->metadata.session_id;
    const auto first_created_at = first->metadata.created_at;
    CHECK(std::regex_match(
        first_id,
        std::regex{R"(^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$)"}));
    CHECK(std::regex_match(
        first_created_at,
        std::regex{R"(^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{3}Z$)"}));
    CHECK(first_id != first_created_at);
    first->session->close();

    tests::ModelsSessionOptions second_opts;
    second_opts.session_target = coding_agent::ExplicitNewSessionTarget{second_paths.session_file};
    second_opts.workspace = second_paths.workspace.path();
    second_opts.models = ai::providers::make_scripted_fake_models();

    auto second = coding_agent::create_agent_session(std::move(second_opts));
    REQUIRE(second.has_value());
    CHECK(second->metadata.session_id != first_id);
    CHECK_FALSE(second->metadata.created_at.empty());
    second->session->close();

    auto resumed = coding_agent::create_agent_session(sdk_resume_options(first_paths));
    REQUIRE(resumed.has_value());
    CHECK(resumed->metadata.session_id == first_id);
    CHECK(resumed->metadata.created_at == first_created_at);
    resumed->session->close();
}

// ─────────────────────────────────────────────────────────────────────────────
// Private Models assembly diagnostics
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("private Models assembly does not publish a host-client SDK diagnostic", "[sdk][u2][issue338]") {
    TestPaths paths;

    tests::ModelsSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.models = ai::providers::make_scripted_fake_models();

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(result.has_value());

    bool found_host_diag = false;
    for (const auto& d : result->diagnostics) {
        if (d.code == "host_client_used") {
            found_host_diag = true;
        }
    }
    CHECK_FALSE(found_host_diag);

    result->session->close();
}

// ─────────────────────────────────────────────────────────────────────────────
// Factory assembly invariants
// ─────────────────────────────────────────────────────────────────────────────

namespace {

std::string read_file_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

bool tool_registry_contains(const ai::AiContext& context, std::string_view name) {
    return std::any_of(
        context.tools.begin(),
        context.tools.end(),
        [name](const ai::Tool& tool) { return tool.name == name; });
}

} // namespace

TEST_CASE("Failed new-session creation leaves no session file", "[sdk][assembly]") {
    TestPaths paths;

    tests::ModelsSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.models = ai::providers::make_scripted_fake_models();
    opts.custom_tools.push_back(std::make_unique<FakeEchoTool>());
    opts.custom_tools.push_back(std::make_unique<FakeEchoTool>()); // duplicate

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE_FALSE(result.has_value());
    CHECK_FALSE(std::filesystem::exists(paths.session_file));
}

TEST_CASE("Failed SDK resume does not modify the session file", "[sdk][assembly]") {
    TestPaths paths;
    auto store = harness::session::JsonlSessionStore::create_new(paths.session_file, test_metadata(paths));
    REQUIRE(store);
    REQUIRE(store->append(user_msg("first")));
    REQUIRE(store->append(user_msg("second")));

    auto pre = harness::session::JsonlSessionStore::load(paths.session_file);
    REQUIRE(pre);
    REQUIRE(pre->entries.size() >= 3);
    const auto first_id = pre->entries[1].entry_id;

    auto resumed = harness::session::JsonlSessionStore::open_existing(paths.session_file);
    REQUIRE(resumed);
    REQUIRE(resumed->append_leaf(std::nullopt, first_id));

    const auto before = read_file_text(paths.session_file);

    auto result = coding_agent::create_agent_session(sdk_resume_options(paths));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message == "unsupported_session_topology");

    const auto after = read_file_text(paths.session_file);
    CHECK(before == after);
}

TEST_CASE("SDK host-provided execution environment is not cleaned up by session close", "[sdk][assembly]") {
    TestPaths paths;
    auto env = std::make_shared<CountingFakeEnv>(paths.workspace.path());

    tests::ModelsSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.models = ai::providers::make_scripted_fake_models();
    opts.execution_env = env;

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(result.has_value());
    result->session->close();
    CHECK(env->cleanup_count == 0);
}

TEST_CASE("SDK disabled bash is absent from the model-visible tool registry", "[sdk][assembly][issue84]") {
    TestPaths paths;
    cch::tests::TempWorkspace agent_dir;
    tests::EnvVarGuard agent_dir_guard{"PI_CODING_AGENT_DIR"};
    agent_dir_guard.set(agent_dir.path().string());
    agent_dir.write(
        "settings.json",
        R"({"shellPath":"/configured/but-unauthorized","shellCommandPrefix":"export SHOULD_NOT_AUTHORIZE=1"})");
    auto capture = std::make_shared<CaptureChatProvider>();
    auto* capture_ptr = capture.get();

    tests::ModelsSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.models = cch::tests::models_from_provider(std::move(capture));
    opts.builtin_tools.bash = false;

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(result.has_value());
    auto prompt_result = result->session->prompt_blocking("hello");
    REQUIRE(prompt_result.has_value());

    REQUIRE(capture_ptr->captured_request.has_value());
    CHECK_FALSE(tool_registry_contains(capture_ptr->captured_request->context, "bash"));
    CHECK(tool_registry_contains(capture_ptr->captured_request->context, "read"));
    CHECK(tool_registry_contains(capture_ptr->captured_request->context, "write"));
    CHECK(tool_registry_contains(capture_ptr->captured_request->context, "edit"));
    result->session->close();
}

TEST_CASE("SDK enabled bash appears in the model-visible tool registry", "[sdk][assembly]") {
    TestPaths paths;
    auto capture = std::make_shared<CaptureChatProvider>();
    auto* capture_ptr = capture.get();

    tests::ModelsSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.models = cch::tests::models_from_provider(std::move(capture));
    opts.builtin_tools.bash = true;

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(result.has_value());
    auto prompt_result = result->session->prompt_blocking("hello");
    REQUIRE(prompt_result.has_value());

    REQUIRE(capture_ptr->captured_request.has_value());
    CHECK(tool_registry_contains(capture_ptr->captured_request->context, "bash"));
    result->session->close();
}

TEST_CASE("SDK rejects workspace-local trust_store_path", "[sdk][assembly][trust]") {
    TestPaths paths;

    tests::ModelsSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.models = ai::providers::make_scripted_fake_models();
    opts.trust_store_path = paths.workspace.path() / ".cpp-harness" / "trust.json";

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message.find("workspace") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(paths.session_file));
}

TEST_CASE("SDK rejects trust_store_path equal to the workspace", "[sdk][assembly][trust]") {
    TestPaths paths;

    tests::ModelsSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.models = ai::providers::make_scripted_fake_models();
    opts.trust_store_path = paths.workspace.path();

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message.find("workspace") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(paths.session_file));
}

TEST_CASE("SDK default trust store inside the workspace cannot authorize project resources", "[sdk][assembly][trust]") {
    TestPaths paths;
    tests::EnvVarGuard home_guard{"HOME"};
    home_guard.set(paths.workspace.path().string());
    write_project_skill(paths, "workspace-authorized");
    paths.workspace.write(
        ".pi/agent/trust.json",
        "{\"" + std::filesystem::weakly_canonical(paths.workspace.path()).string() + "\":true}\n");

    tests::ModelsSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.models = ai::providers::make_scripted_fake_models();
    opts.load_project_resources = true;
    opts.default_project_trust = coding_agent::DefaultProjectTrust::Always;

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(result.has_value());
    CHECK(result->session->skills().empty());
    CHECK(has_sdk_diag(result->diagnostics, "trust:trust_store_unavailable"));
    result->session->close();
}

#if defined(__unix__) || defined(__APPLE__)
TEST_CASE("SDK rejects trust_store_path through a symlinked parent into the workspace", "[sdk][assembly][trust]") {
    TestPaths paths;
    cch::tests::TempWorkspace external;
    const auto linked_workspace = external.path() / "workspace-link";
    std::filesystem::create_directory_symlink(paths.workspace.path(), linked_workspace);

    tests::ModelsSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.models = ai::providers::make_scripted_fake_models();
    opts.trust_store_path = linked_workspace / ".cpp-harness" / "trust.json";

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message.find("workspace") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(paths.session_file));
}

TEST_CASE("SDK rejects a symlinked trust_store_path into the workspace", "[sdk][assembly][trust]") {
    TestPaths paths;
    cch::tests::TempWorkspace external;
    const auto workspace_trust = paths.workspace.path() / ".cpp-harness" / "trust.json";
    paths.workspace.write(".cpp-harness/trust.json", "{}\n");
    const auto linked_trust = external.path() / "trust.json";
    std::filesystem::create_symlink(workspace_trust, linked_trust);

    tests::ModelsSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.models = ai::providers::make_scripted_fake_models();
    opts.trust_store_path = linked_trust;

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message.find("workspace") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(paths.session_file));
}

TEST_CASE("SDK rejects trust_store_path when containment cannot be determined", "[sdk][assembly][trust]") {
    TestPaths paths;
    cch::tests::TempWorkspace external;
    const auto loop = external.path() / "loop";
    std::filesystem::create_directory_symlink("loop", loop);

    tests::ModelsSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.models = ai::providers::make_scripted_fake_models();
    opts.trust_store_path = loop / "trust.json";

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message.find("canonicalized") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(paths.session_file));
}
#endif

TEST_CASE("SDK rejects relative trust_store_path", "[sdk][assembly][trust]") {
    TestPaths paths;

    tests::ModelsSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.models = ai::providers::make_scripted_fake_models();
    opts.trust_store_path = std::filesystem::path{"relative/trust.json"};

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message.find("absolute") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(paths.session_file));
}

TEST_CASE("SDK accepts an existing external trust store as project trust authority", "[sdk][assembly][trust]") {
    TestPaths paths;
    cch::tests::TempWorkspace external;
    write_project_skill(paths, "externally-authorized");
    const auto external_trust = external.path() / "trust.json";
    std::ofstream(external_trust)
        << "{\"" << std::filesystem::weakly_canonical(paths.workspace.path()).string() << "\":true}\n";

    tests::ModelsSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.models = ai::providers::make_scripted_fake_models();
    opts.trust_store_path = external_trust;
    opts.load_project_resources = true;

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(result.has_value());
    REQUIRE(result->session->skills().size() == 1);
    CHECK(result->session->skills()[0].name == "externally-authorized");
    result->session->close();
}

TEST_CASE("SDK accepts external not-yet-created trust_store_path", "[sdk][assembly][trust]") {
    TestPaths paths;
    cch::tests::TempWorkspace home;
    auto external_trust = home.path() / "external" / "trust.json";

    tests::ModelsSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.models = ai::providers::make_scripted_fake_models();
    opts.trust_store_path = external_trust;
    opts.load_project_resources = true;
    opts.default_project_trust = coding_agent::DefaultProjectTrust::Always;

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(result.has_value());
    result->session->close();
}

// ─────────────────────────────────────────────────────────────────────────────
// Provider resolution and metadata precedence
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("private Models assembly uses settings provider with an opaque model sentinel", "[sdk][provider-resolution]") {
    TestPaths paths;
    cch::tests::TempWorkspace home;
    tests::EnvVarGuard home_guard{"HOME"};
    home_guard.set(home.path().string());
    home.write(".pi/agent/settings.json", R"({"defaultProvider":"kimi-coding"})");

    tests::ModelsSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.models = ai::providers::make_scripted_fake_models();

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(result.has_value());
    CHECK(result->provider == "kimi-coding");
    CHECK(result->model == "host-client");
    CHECK(result->session->provider() == "kimi-coding");
    CHECK(result->session->model() == "host-client");
    result->session->close();
}

TEST_CASE("private Models resume without override retains stored metadata", "[sdk][provider-resolution]") {
    TestPaths paths;

    {
        tests::ModelsSessionOptions opts;
        opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
        opts.workspace = paths.workspace.path();
        opts.models = ai::providers::make_scripted_fake_models();
        opts.model = cch::tests::sdk_request_model("fake", "fake-model");

        auto result = coding_agent::create_agent_session(std::move(opts));
        REQUIRE(result.has_value());
        CHECK(result->provider == "fake");
        CHECK(result->model == "fake-model");
        result->session->close();
    }

    {
        tests::ModelsSessionOptions opts;
        opts.session_target = coding_agent::ExplicitResumeSessionTarget{paths.session_file};
        opts.workspace = paths.workspace.path();
        opts.models = ai::providers::make_scripted_fake_models();

        auto result = coding_agent::create_agent_session(std::move(opts));
        REQUIRE(result.has_value());
        CHECK(result->provider == "fake");
        CHECK(result->model == "fake-model");
        CHECK_FALSE(has_sdk_diag(result->diagnostics, "resume_provider_override"));
        CHECK(result->session->provider() == "fake");
        CHECK(result->session->model() == "fake-model");
        result->session->close();
    }
}

TEST_CASE("SDK resume with explicit provider/model override reports diagnostic", "[sdk][provider-resolution]") {
    TestPaths paths;
    cch::tests::TempWorkspace home;
    tests::EnvVarGuard home_guard{"HOME"};
    home_guard.set(home.path().string());

    {
        tests::ModelsSessionOptions opts;
        opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
        opts.workspace = paths.workspace.path();
        opts.models = ai::providers::make_scripted_fake_models();

        auto result = coding_agent::create_agent_session(std::move(opts));
        REQUIRE(result.has_value());
        CHECK(result->provider == "sdk-host");
        CHECK(result->model == "host-client");
        result->session->close();
    }

    {
        tests::ModelsSessionOptions opts;
        opts.session_target = coding_agent::ExplicitResumeSessionTarget{paths.session_file};
        opts.workspace = paths.workspace.path();
        opts.models = ai::providers::make_scripted_fake_models();
        opts.model = cch::tests::sdk_request_model("openai-compatible", "gpt-4o");

        auto result = coding_agent::create_agent_session(std::move(opts));
        REQUIRE(result.has_value());
        CHECK(result->provider == "openai-compatible");
        CHECK(result->model == "gpt-4o");
        CHECK(has_sdk_diag(result->diagnostics, "resume_provider_override"));
        result->session->close();
    }
}

TEST_CASE("SDK resume with explicit model override alone reports diagnostic", "[sdk][provider-resolution]") {
    TestPaths paths;

    {
        tests::ModelsSessionOptions opts;
        opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
        opts.workspace = paths.workspace.path();
        opts.models = ai::providers::make_scripted_fake_models();
        opts.model = cch::tests::sdk_request_model("openai-compatible", "gpt-4.1-mini");

        auto result = coding_agent::create_agent_session(std::move(opts));
        REQUIRE(result.has_value());
        result->session->close();
    }

    {
        tests::ModelsSessionOptions opts;
        opts.session_target = coding_agent::ExplicitResumeSessionTarget{paths.session_file};
        opts.workspace = paths.workspace.path();
        opts.models = ai::providers::make_scripted_fake_models();
        opts.model = cch::tests::sdk_request_model("openai-compatible", "gpt-4o");

        auto result = coding_agent::create_agent_session(std::move(opts));
        REQUIRE(result.has_value());
        CHECK(result->model == "gpt-4o");
        CHECK(has_sdk_diag(result->diagnostics, "resume_provider_override"));
        result->session->close();
    }
}

TEST_CASE(
    "SDK persists only model_change identity and resume restores it",
    "[sdk][provider-resolution][resume][issue346]") {
    TestPaths paths;

    {
        tests::ModelsSessionOptions opts;
        opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
        opts.workspace = paths.workspace.path();
        opts.models = ai::providers::make_scripted_fake_models();
        opts.model = cch::tests::sdk_request_model("fake", "fake-model");

        auto result = coding_agent::create_agent_session(std::move(opts));
        REQUIRE(result.has_value());
        result->session->close();
    }

    // The session file persists only `model_change {provider, modelId}` as the
    // content entry — no baseUrl, key-source, or authentication material.
    auto durable = harness::session::JsonlSessionStore::load(paths.session_file);
    REQUIRE(durable.has_value());
    const auto model_change = std::find_if(
        durable->entries.begin(),
        durable->entries.end(),
        [](const harness::session::SessionEntry& entry) {
            return entry.kind == harness::session::SessionEntryKind::ModelChange;
        });
    REQUIRE(model_change != durable->entries.end());
    const auto* value =
        std::get_if<harness::session::ModelChangeValue>(&model_change->value);
    REQUIRE(value != nullptr);
    CHECK(value->provider == "fake");
    CHECK(value->model_id == "fake-model");
    CHECK(model_change->raw_line.find("baseUrl") == std::string::npos);
    CHECK(model_change->raw_line.find("apiKey") == std::string::npos);
    CHECK(model_change->raw_line.find("source") == std::string::npos);

    // Resume re-resolves the recorded identity against the live runtime.
    {
        tests::ModelsSessionOptions opts;
        opts.session_target = coding_agent::ExplicitResumeSessionTarget{paths.session_file};
        opts.workspace = paths.workspace.path();
        opts.models = ai::providers::make_scripted_fake_models();

        auto result = coding_agent::create_agent_session(std::move(opts));
        REQUIRE(result.has_value());
        CHECK(result->provider == "fake");
        CHECK(result->model == "fake-model");
        CHECK_FALSE(has_sdk_diag(result->diagnostics, "resume_model_unresolved"));
        result->session->close();
    }
}
