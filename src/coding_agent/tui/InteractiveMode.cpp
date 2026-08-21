#include "InteractiveMode.hpp"

#include <cch/agent/AgentContext.hpp>
#include <cch/agent/AgentEvent.hpp>
#include <cch/ai/Content.hpp>
#include "coding_agent/AgentSession.hpp"
#include "ai/AsyncResultBridge.hpp"
#include "ai/ModelThinkingLevel.hpp"
#include <cch/ai/Model.hpp>
#include <cch/coding_agent/Settings.hpp>
#include <cch/coding_agent/ProjectResources.hpp>
#include <cch/coding_agent/ProjectTrust.hpp>
#include <cch/coding_agent/AgentConfigDir.hpp>
#include "agent/harness/WorkspaceFileSystem.hpp"
#include <cch/tui/Autocomplete.hpp>
#include <cch/tui/Editor.hpp>
#include <cch/tui/Fuzzy.hpp>
#include <cch/tui/Loader.hpp>
#include <cch/tui/Overlay.hpp>
#include <cch/tui/Terminal.hpp>
#include <cch/tui/TruncatedText.hpp>
#include <cch/tui/Tui.hpp>

#include "coding_agent/BoundedText.hpp"
#include "coding_agent/ImageInput.hpp"
#include "coding_agent/prompt/BuiltinSlashCommands.hpp"
#include "coding_agent/runtime/AgentSessionInteractiveAccess.hpp"
#include "coding_agent/runtime/AgentSessionRuntime.hpp"
#include "coding_agent/tui/BashExecutionComponent.hpp"
#include "coding_agent/tui/ChatContainer.hpp"
#include "coding_agent/tui/ClipboardWrite.hpp"
#include "coding_agent/tui/ErrorPresentation.hpp"
#include "coding_agent/tui/ExternalEditor.hpp"
#include "coding_agent/tui/Footer.hpp"
#include "coding_agent/tui/KeybindingsManager.hpp"
#include "coding_agent/tui/KeybindingHints.hpp"
#include "coding_agent/tui/InteractiveView.hpp"
#include "coding_agent/tui/InteractiveViewActions.hpp"
#include "coding_agent/tui/AuthFlowController.hpp"
#include "coding_agent/tui/LoadedResources.hpp"
#include "coding_agent/tui/ModalPresenter.hpp"
#include "coding_agent/tui/ModelFlowController.hpp"
#include "coding_agent/tui/SessionFlowController.hpp"
#include "coding_agent/tui/SessionUiBinding.hpp"
#include "coding_agent/tui/SharedKeybindings.hpp"
#include "coding_agent/tui/ModelSearch.hpp"
#include "coding_agent/tui/OpenBrowser.hpp"
#include "coding_agent/tui/SlashCommandRouter.hpp"
#include "coding_agent/tui/SettingsSelector.hpp"
#include "coding_agent/tui/ThemeController.hpp"
#include "support/UniqueFd.hpp"
#include "agent/tools/TerminalText.hpp"

#include <cch/coding_agent/AgentConfigDir.hpp>
#include <cch/agent/harness/session/SessionStore.hpp>

#include <cch/support/Error.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <csignal>
#include <exception>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

namespace cch::coding_agent::tui {
namespace {

using ActionSink = std::move_only_function<void()>;

// ── Focused-editor User Bash syntax (ADR 0026) ────────────────────────────
// Folded from the deleted UserBashSyntax module: only a direct focused
// Native TUI editor submission interprets the `!`/`!!` prefixes.

struct UserBashInvocation {
    std::string command;
    bool exclude_from_context{false};
};

// The main-screen action types and payload structs live in
// InteractiveViewActions.hpp; the shared pure helpers used by the state
// live in InteractiveView.hpp's detail namespace.
using interactive_view_detail::trim_editor_submission;
using interactive_view_detail::user_bash_editor_mode;
using interactive_view_detail::editor_text_after_interrupt;
using interactive_view_detail::thinking_border_token_for;
using interactive_view_detail::queued_editor_text;

/// `JSON.stringify`-shaped quoting for the `/name` normalization warning
/// (pi `JSON.stringify(name)`): a double-quoted literal with the JSON
/// escapes pi would emit.
[[nodiscard]] std::string json_quote_string(std::string_view value) {
    std::string result = "\"";
    for (const char character : value) {
        switch (character) {
        case '\\': result += "\\\\"; break;
        case '"': result += "\\\""; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default: result.push_back(character); break;
        }
    }
    result.push_back('"');
    return result;
}

/// Parses one trimmed submission as User Bash. `!` runs with later model
/// context; `!!` runs excluded from model conversion; `!!!foo` is excluded
/// User Bash running `!foo`. A bare `!` or `!!` yields no invocation and
/// falls through to an ordinary Agent Prompt.
[[nodiscard]] std::optional<UserBashInvocation> parse_user_bash_invocation(
    std::string text) {
    text = trim_editor_submission(std::move(text));
    if (!text.starts_with('!')) return std::nullopt;
    const bool excluded = text.starts_with("!!");
    auto command = trim_editor_submission(text.substr(excluded ? 2 : 1));
    if (command.empty()) return std::nullopt;
    return UserBashInvocation{
        .command = std::move(command),
        .exclude_from_context = excluded,
    };
}

/// Bash mode is the unsubmitted editor state whose trimmed text begins with
/// `!`; it exists only where User Bash dispatch is available.

// ── Submission kinds (folded from the deleted InteractionPolicy) ──────────

enum class SubmissionOrigin { FocusedEditor, InitialPrompt };

enum class InterruptRoute {
    AbortAgentRun,
    CancelUserBash,
    ClearPendingBash,
    None,
};

struct InteractiveStartupDiagnostics {
    std::vector<KeybindingDiagnostic> keybindings;
    /// Theme parse/collision diagnostics from the boot session's theme
    /// discovery (pi `resource-loader.ts` `getThemes` diagnostics).
    std::vector<ResourceDiagnostic> themes;
};

[[nodiscard]] support::Error presentation_error(
    const support::Error& error,
    std::string message) {
    return support::make_error(
        error.code,
        std::move(message),
        bounded_redacted_presentation(combined_error_text(error)));
}

/// pi `theme.ts` `getThinkingBorderColor`: the editor border token for a
/// thinking-level wire name ("off".."max"); unknown levels fall back to
/// `thinkingOff` like pi's default branch.

[[nodiscard]] std::string clipboard_uuid() {
    std::random_device random;
    std::array<std::uint8_t, 16> bytes{};
    for (auto& byte : bytes) byte = static_cast<std::uint8_t>(random());
    bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0fU) | 0x40U);
    bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3fU) | 0x80U);

    std::string value;
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) value.push_back('-');
        value += std::format("{:02x}", bytes[index]);
    }
    return value;
}

[[nodiscard]] support::Expected<std::filesystem::path> write_clipboard_image(
    std::span<const std::uint8_t> bytes,
    std::string_view extension) {
    std::error_code temp_error;
    const auto temp_directory = std::filesystem::temp_directory_path(temp_error);
    if (temp_error || temp_directory.empty()) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Process,
            "clipboard temporary directory is unavailable",
            temp_error.message()));
    }

    for (std::size_t attempt = 0; attempt < 16; ++attempt) {
        std::filesystem::path path;
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
        try {
#endif
            path = temp_directory /
                std::format("pi-clipboard-{}{}", clipboard_uuid(), extension);
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
        } catch (const std::exception& error) {
            return std::unexpected(support::make_error(
                support::ErrorCode::Process,
                "could not generate a clipboard image path",
                error.what()));
        } catch (...) {
            return std::unexpected(support::make_error(
                support::ErrorCode::Process,
                "could not generate a clipboard image path"));
        }
#endif
        support::UniqueFd fd(::open(
            path.c_str(),
            O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
            0600));
        if (!fd) {
            if (errno == EEXIST) continue;
            return std::unexpected(support::make_error(
                support::ErrorCode::Process,
                "could not create clipboard image file",
                std::error_code(errno, std::generic_category()).message()));
        }

        std::size_t written = 0;
        while (written < bytes.size()) {
            const auto count = ::write(
                fd.get(),
                bytes.data() + written,
                bytes.size() - written);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) {
                const auto write_error = errno;
                (void)fd.close();
                std::error_code remove_error;
                std::filesystem::remove(path, remove_error);
                return std::unexpected(support::make_error(
                    support::ErrorCode::Process,
                    "could not write clipboard image file",
                    std::error_code(write_error, std::generic_category()).message()));
            }
            written += static_cast<std::size_t>(count);
        }
        if (fd.close() != 0) {
            const auto close_error = errno;
            std::error_code remove_error;
            std::filesystem::remove(path, remove_error);
            return std::unexpected(support::make_error(
                support::ErrorCode::Process,
                "could not finish clipboard image file",
                std::error_code(close_error, std::generic_category()).message()));
        }
        return path;
    }
    return std::unexpected(support::make_error(
        support::ErrorCode::Process,
        "could not allocate a unique clipboard image path"));
}

[[nodiscard]] support::Expected<std::vector<std::string>> queued_editor_texts(
    const agent::AgentInputQueues& queues) {
    std::vector<std::string> restored;
    restored.reserve(
        queues.steering.messages.size() + queues.follow_up.messages.size());
    const auto append = [&restored](const auto& messages) {
        for (const auto& message : messages) {
            auto text = queued_editor_text(message);
            if (!text) return false;
            restored.push_back(std::move(*text));
        }
        return true;
    };
    if (!append(queues.steering.messages) || !append(queues.follow_up.messages)) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "queued input contains content that the editor cannot restore"));
    }
    return restored;
}

[[nodiscard]] support::Error startup_error(const support::Error& error) {
    return presentation_error(error, "Native TUI startup failed");
}

/// pi `renderProjectTrustWarningIfNeeded` chat warning text, with the C++
/// binary's own identity and the absent packages clause dropped.
[[nodiscard]] std::string project_trust_warning_text() {
    return "This project is not trusted. Project .pi resources are ignored. "
           "Use /trust to save a trust decision, then restart cch.";
}

[[nodiscard]] support::Error aggregate_presentation_errors(
    const support::Error& primary,
    const support::Error& restoration,
    std::string message) {
    return support::make_error(
        primary.code,
        std::move(message),
        bounded_redacted_presentation(std::format(
            "primary: {}; restoration: {}",
            combined_error_text(primary),
            combined_error_text(restoration))));
}

/// pi `prefixAutocompleteDescription` subset (`core/source-info.ts` + the
/// interactive-mode `getAutocompleteSourceTag`): the scope-prefixed
/// description for discovered prompt templates and skills. The loader subset
/// produces only `source` "auto"/"cli" (no npm/git sources), so the tag is
/// the scope letter `[u]`/`[p]`/`[t]` alone.
[[nodiscard]] std::string prefix_autocomplete_description(
    const std::string& description,
    SourceScope scope) {
    const char tag = scope == SourceScope::User ? 'u' : scope == SourceScope::Project ? 'p' : 't';
    return std::format("[{}]{}", tag, description.empty() ? "" : " " + description);
}

/// Build the editor autocomplete command list: the 17 Supported built-in
/// slash commands (pi `BUILTIN_SLASH_COMMANDS` subset) as plain items, the
/// loaded prompt templates (scope-prefixed descriptions), and `/skill:`
/// commands while the `enableSkillCommands` setting is enabled — plus the
/// `model` command as a `SlashCommand` whose argument completion resolves
/// pi's `model-search` text over the current candidate snapshot (scoped
/// models when the session carries a scope, else the availability snapshot).
/// The Deferred slashes (`/export` `/import` `/share` `/changelog`
/// `/clone`), `/debug`, and the easter eggs are absent.
[[nodiscard]] std::vector<std::variant<cch::tui::SlashCommand, cch::tui::AutocompleteItem>>
command_autocomplete_commands(
    std::span<const PromptTemplate> prompt_templates,
    std::span<const Skill> skills,
    std::shared_ptr<const ModelCompletionSnapshot> model_completion,
    bool include_skill_commands) {
    std::vector<std::variant<cch::tui::SlashCommand, cch::tui::AutocompleteItem>> items;
    std::set<std::string, std::less<>> names;
    for (const auto& command : prompt::builtin_slash_commands()) {
        std::string description = std::string{command.description};
        if (!command.argument_hint.empty()) {
            description = description.empty()
                ? std::string{command.argument_hint}
                : std::format("{} — {}", command.argument_hint, description);
        }
        if (command.name == "model") {
            // pi `createBaseAutocompleteProvider`: `/model` argument
            // completion over `getModelSearchText`, value `provider/id`,
            // label `id`, description `provider`. The description stays
            // plain: the combined provider prepends the argument hint.
            cch::tui::SlashCommand slash;
            slash.name = std::string{command.name};
            slash.description = std::string{command.description};
            slash.argument_hint = std::string{command.argument_hint};
            slash.get_argument_completions =
                [model_completion](std::string_view prefix)
                -> std::optional<std::vector<cch::tui::AutocompleteItem>> {
                    if (!model_completion || model_completion->empty()) return std::nullopt;
                    std::vector<ModelSearchItem> items;
                    items.reserve(model_completion->size());
                    for (const auto& candidate : *model_completion) {
                        items.push_back(ModelSearchItem{
                            .id = candidate.id,
                            .provider = candidate.provider,
                            .name = candidate.name.empty()
                                ? std::nullopt
                                : std::optional<std::string>{candidate.name},
                        });
                    }
                    const auto filtered = cch::tui::fuzzy_filter(
                        std::move(items), prefix, get_model_search_text);
                    if (filtered.empty()) return std::nullopt;
                    std::vector<cch::tui::AutocompleteItem> result;
                    result.reserve(filtered.size());
                    for (const auto& item : filtered) {
                        result.push_back(cch::tui::AutocompleteItem{
                            .value = item.provider + "/" + item.id,
                            .label = item.id,
                            .description = item.provider,
                        });
                    }
                    return result;
                };
            items.push_back(std::move(slash));
        } else {
            items.push_back(cch::tui::AutocompleteItem{
                .value = std::string{command.name},
                .label = std::string{command.name},
                .description = std::move(description),
            });
        }
        names.insert(std::string{command.name});
    }
    for (const auto& prompt_template : prompt_templates) {
        if (!names.insert(prompt_template.name).second) continue;
        std::string description = prompt_template.description.value_or("");
        if (prompt_template.argument_hint && !prompt_template.argument_hint->empty()) {
            description = description.empty()
                ? *prompt_template.argument_hint
                : std::format("{} — {}", *prompt_template.argument_hint, description);
        }
        items.push_back(cch::tui::AutocompleteItem{
            .value = prompt_template.name,
            .label = prompt_template.name,
            .description =
                prefix_autocomplete_description(description, prompt_template.sourceInfo.scope),
        });
    }
    // pi `createBaseAutocompleteProvider`: skill commands register only
    // while the `enableSkillCommands` setting is enabled.
    if (include_skill_commands) {
        for (const auto& skill : skills) {
            auto name = "skill:" + skill.name;
            if (!names.insert(name).second) continue;
            items.push_back(cch::tui::AutocompleteItem{
                .value = name,
                .label = name,
                .description =
                    prefix_autocomplete_description(skill.description, skill.sourceInfo.scope),
            });
        }
    }
    std::sort(items.begin(), items.end(), [](const auto& left, const auto& right) {
        const auto label = [](const auto& item) -> std::string_view {
            if (const auto* slash = std::get_if<cch::tui::SlashCommand>(&item)) {
                return slash->name;
            }
            return std::get<cch::tui::AutocompleteItem>(item).label;
        };
        return label(left) < label(right);
    });
    return items;
}

/// Resolve an executable on PATH (pi's `ensureTool`); nullopt when absent so
/// `@`/`#` completion degrades gracefully to empty file suggestions.
[[nodiscard]] std::optional<std::filesystem::path> find_executable_on_path(std::string_view name) {
    const char* path_env = std::getenv("PATH");
    if (path_env == nullptr) return std::nullopt;
    std::string_view path_view{path_env};
    std::size_t begin = 0;
    for (std::size_t index = 0; index <= path_view.size(); ++index) {
        if (index != path_view.size() && path_view[index] != ':') continue;
        const auto dir = path_view.substr(begin, index - begin);
        begin = index + 1;
        if (dir.empty()) continue;
        const auto candidate = std::filesystem::path{dir} / name;
        std::error_code error;
        const auto status = std::filesystem::status(candidate, error);
        if (error || !std::filesystem::is_regular_file(status)) continue;
        if (::access(candidate.c_str(), X_OK) == 0) return candidate;
    }
    return std::nullopt;
}

/// Executor-bound one-shot debounce timer for the editor's autocomplete
/// requests. All timer state is confined to the executor thread via posts;
/// the shared state keeps the wait handler safe after the editor is gone.
class AsioAutocompleteDebounceTimer final : public cch::tui::AutocompleteDebounceTimer {
public:
    explicit AsioAutocompleteDebounceTimer(boost::asio::any_io_executor executor)
        : state_(std::make_shared<State>(std::move(executor))) {}

    void start(std::chrono::milliseconds delay,
               std::move_only_function<support::ExpectedVoid()> on_fire) override {
        const auto state = state_;
        boost::asio::post(state->executor, [state, delay, on_fire = std::move(on_fire)]() mutable {
            const auto generation = ++state->generation;
            state->active_callback = std::move(on_fire);
            state->timer.expires_after(delay);
            state->timer.async_wait([state, generation](boost::system::error_code error) {
                if (generation != state->generation) return;
                auto callback = std::move(state->active_callback);
                state->active_callback = nullptr;
                if (!error && callback) (void)callback();
            });
        });
    }

    void cancel() override {
        const auto state = state_;
        boost::asio::post(state->executor, [state] {
            ++state->generation;
            state->active_callback = nullptr;
            state->timer.cancel();
        });
    }

private:
    struct State {
        explicit State(boost::asio::any_io_executor executor)
            : executor(executor), timer(executor) {}
        boost::asio::any_io_executor executor;
        boost::asio::steady_timer timer;
        std::size_t generation{0};
        std::move_only_function<support::ExpectedVoid()> active_callback;
    };
    std::shared_ptr<State> state_;
};

class DismissibleView final
    : public cch::tui::Component,
      public cch::tui::InputHandler,
      public cch::tui::Focusable {
public:
    DismissibleView(
        std::unique_ptr<cch::tui::Component> content,
        std::shared_ptr<const cch::tui::KeybindingRegistry> keybindings,
        ActionSink on_cancel)
        : content_(std::move(content)),
          keybindings_(std::move(keybindings)),
          on_cancel_(std::move(on_cancel)) {}
    DismissibleView(DismissibleView&&) = delete;
    DismissibleView& operator=(DismissibleView&&) = delete;
    ~DismissibleView() override = default;

    DismissibleView(const DismissibleView&) = delete;
    DismissibleView& operator=(const DismissibleView&) = delete;

    [[nodiscard]] support::Expected<cch::tui::RenderResult> render(std::size_t width) override {
        if (callback_error_) return std::unexpected(*callback_error_);
        return content_->render(width);
    }
    void invalidate() override { content_->invalidate(); }
    void handle_input(const cch::tui::InputEventVariant& input) override {
        const auto* key = std::get_if<cch::tui::KeyEvent>(&input);
        if (key == nullptr || key->type == cch::tui::KeyEventType::Release ||
            !keybindings_->matches(*key, "tui.select.cancel") || !on_cancel_) {
            return;
        }
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
        try {
#endif
            on_cancel_();
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
        } catch (const std::exception& error) {
            callback_error_ = support::make_error(
                support::ErrorCode::Unknown,
                "Hotkey help cancellation failed",
                error.what());
        } catch (...) {
            callback_error_ = support::make_error(
                support::ErrorCode::Unknown,
                "Hotkey help cancellation failed");
        }
#endif
    }
    [[nodiscard]] bool accepts_key_releases() const override { return false; }
    void set_focused(bool focused) override { focused_ = focused; }
    [[nodiscard]] bool focused() const override { return focused_; }
    [[nodiscard]] std::optional<cch::tui::CursorPosition> cursor_location() const override {
        return std::nullopt;
    }

private:
    std::unique_ptr<cch::tui::Component> content_;
    std::shared_ptr<const cch::tui::KeybindingRegistry> keybindings_;
    ActionSink on_cancel_;
    std::optional<support::Error> callback_error_;
    bool focused_{false};
};

/// The pi main-screen composition: header (keybinding hints only, no logo),
class InteractiveState final
    : public std::enable_shared_from_this<InteractiveState>,
      public ModalPresenter {
public:
    InteractiveState(
        AgentSession* session,
        cch::tui::Terminal& terminal,
        boost::asio::any_io_executor executor)
        : session_(session),
          terminal_(terminal),
          tui_(terminal),
          executor_(std::move(executor)),
          exit_wait_(executor_),
          flows_settled_(executor_) {
        exit_wait_.expires_at(std::chrono::steady_clock::time_point::max());
        flows_settled_.expires_at(std::chrono::steady_clock::time_point::max());
    }
    InteractiveState(InteractiveState&&) = delete;
    InteractiveState& operator=(InteractiveState&&) = delete;
    ~InteractiveState() override = default;
    InteractiveState(const InteractiveState&) = delete;
    InteractiveState& operator=(const InteractiveState&) = delete;

    [[nodiscard]] support::ExpectedVoid start(InteractiveModeConfig config) {
        clipboard_reader_ = std::move(config.clipboard_reader);
        model_fallback_message_ = std::move(config.model_fallback_message);
        action_sink_ = std::move(config.action_sink);
        session_facts_ = std::move(config.session_facts);
        boot_request_ = std::move(config.boot_request);
        const bool booting = boot_request_.has_value();

        InteractiveStartupDiagnostics diagnostics;
        if (auto loaded = load_startup_resources(config); !loaded) {
            return fail_start(loaded.error());
        } else {
            diagnostics = std::move(*loaded);
        }

        // The model flows need the startup resources (live theme, settings,
        // keybindings), so the controller is created right after they load;
        // the initial `/model` completion snapshot follows (nothing reads it
        // before view composition).
        model_flows_ = make_model_flow_controller();
        auth_flows_ = make_auth_flow_controller();
        session_flows_ = make_session_flow_controller();
        session_ui_ = make_session_ui_binding();
        if (!booting) {
            model_flows_->update_model_completion();
        }

        const auto weak = weak_from_this();
        auto view = make_interactive_view(weak);
        view_ = view.get();
        if (auto attached = tui_.add_child(std::move(view)); !attached) {
            return fail_start(attached.error());
        }
        if (!booting) {
            if (auto subscribed = session_ui_->bind(*session_); !subscribed) {
                return fail_start(subscribed.error());
            }
        }

        tui_.set_render_request_sink([weak]() -> support::ExpectedVoid {
            if (const auto self = weak.lock()) self->post_render();
            return {};
        });
        if (auto started = tui_.start(); !started) return fail_start(started.error());
        tui_started_ = true;
        running_ = true;

        if (!booting) {
            initialize_view(diagnostics);
            if (auto rendered = tui_.render(); !rendered) return fail_start(rendered.error());
            if (auto focused = tui_.set_focus(view_); !focused) return fail_start(focused.error());
            if (auto rendered = tui_.render(); !rendered) return fail_start(rendered.error());
            if (config.initial_prompt) {
                submit(
                    std::move(*config.initial_prompt),
                    InputSubmission::Ordinary,
                    std::move(config.initial_prompt_options),
                    SubmissionOrigin::InitialPrompt);
            }
        } else {
            // pi main.ts: the boot trust prompt resolves as an overlay on the
            // main TUI before session bind (G2 record); `boot_session`
            // creates the boot session and then binds the view. The startup
            // diagnostics render after bind, alongside the created session's
            // snapshot.
            startup_diagnostics_ = std::move(diagnostics);
            if (auto rendered = tui_.render(); !rendered) return fail_start(rendered.error());
            if (auto focused = tui_.set_focus(view_); !focused) return fail_start(focused.error());
            if (auto rendered = tui_.render(); !rendered) return fail_start(rendered.error());
            initial_prompt_ = std::move(config.initial_prompt);
            initial_prompt_options_ = config.initial_prompt_options;
        }
        return {};
    }

    /// pi main.ts `createAgentSessionRuntime` + `resolveProjectTrusted`:
    /// the deferred boot of the interactive host. Resolves boot trust
    /// (prompt overlay when a trust-requiring resource exists and no
    /// override is set), creates the boot session through the config's
    /// `boot_request`/`action_sink` with the decided trust, then binds
    /// it (subscribe, initialize view, render, initial prompt).
    [[nodiscard]] boost::asio::awaitable<support::ExpectedVoid> boot_session() {
        // 1. Resolve boot trust (pi resolveProjectTrusted): override → no
        //    trust-requiring resources → saved decision → default
        //    always/never → ask prompt (the generic string-list selector
        //    overlay; G2 record). The controller also arms pi's implicit
        //    trust-on-reload behavior for a resource-free boot.
        session_flows_->arm_auto_trust_on_reload(
            boot_request_->workspace,
            boot_request_->project_trust_override);
        auto decision = co_await session_flows_->resolve_boot_trust(
            boot_request_->workspace,
            boot_request_->project_trust_override);
        // pi `projectTrustByCwd`: remember the boot decision for the boot
        // workspace so in-session session creations in the same workspace
        // reuse it instead of re-resolving (ask-without-UI would silently
        // drop a session-only trust).
        resolved_boot_trust_.emplace(boot_request_->workspace, decision);
        // 2. Create the boot session with the decided trust so SessionFactory
        //    resolves deterministically (pi `projectTrustByCwd` cache).
        auto request = std::move(*boot_request_);
        request.project_trust_override = decision;
        auto created = request_session_replacement(action_generation_, std::move(request));
        if (!created) {
            const support::Error failure = created.error();
            // pi `print_creation_failure`: the host reports the failure
            // through the closed action seam before the boot exits.
            (void)deliver_action(
                action_generation_,
                TuiActionVariant{ReportBootCreationFailureAction{failure}});
            // Stop the TUI so the terminal is restored before the host
            // reports the error; the session never bound.
            running_ = false;
            if (tui_started_) {
                (void)tui_.stop();
                tui_started_ = false;
            }
            co_return std::unexpected(failure);
        }
        // pi `reportDiagnostics`: the host prints the creation diagnostics.
        if (!created->diagnostics.empty()) {
            (void)deliver_action(
                action_generation_,
                TuiActionVariant{ReportBootDiagnosticsAction{
                    std::move(created->diagnostics)}});
        }
        model_fallback_message_ = std::move(created->model_fallback_message);
        // pi interactive-mode ctor `setRegisteredThemes(...)` + init
        // `applyFromSettings()`: register the boot session's discovered
        // themes (project `.pi/themes` trust-gated, user directory,
        // explicit `--theme`) and re-apply the active theme from the
        // settings with dark fallback (pi `/reload` re-runs the same two
        // steps). Parse/collision diagnostics render with the startup
        // diagnostics.
        if (theme_controller_) {
            auto discovery = coding_agent::tui::discover_themes(
                std::move(created->theme_resources));
            loaded_theme_diagnostics_ = discovery.diagnostics;
            startup_diagnostics_.themes = std::move(discovery.diagnostics);
            theme_controller_->set_registered_themes(std::move(discovery.themes));
            theme_controller_->apply_from_settings();
        }
        // 3. Bind: the boot-created session replaces the borrowed null
        //    session and the presentation renders its snapshot like pi's
        //    `renderInitialMessages`.
        owned_session_ = std::move(created->session);
        session_ = owned_session_.get();
        model_flows_->update_model_completion();
        rebuild_autocomplete_provider();
        if (auto subscribed = session_ui_->bind(*session_); !subscribed) {
            co_return std::unexpected(subscribed.error());
        }
        initialize_view(startup_diagnostics_);
        if (auto rendered = tui_.render(); !rendered) {
            co_return std::unexpected(rendered.error());
        }
        if (auto focused = tui_.set_focus(view_); !focused) {
            co_return std::unexpected(focused.error());
        }
        if (auto rendered = tui_.render(); !rendered) {
            co_return std::unexpected(rendered.error());
        }
        if (initial_prompt_) {
            submit(
                std::move(*initial_prompt_),
                InputSubmission::Ordinary,
                std::move(initial_prompt_options_),
                SubmissionOrigin::InitialPrompt);
        }
        co_return support::ExpectedVoid{};
    }

    [[nodiscard]] boost::asio::steady_timer& exit_wait() {
        return exit_wait_;
    }

    /// Final application Close (ADR 0040): cancel the extracted modal/session
    /// flows, stop admission, retire the action generation, and await every
    /// admitted detached flow to reach a terminal outcome before terminal
    /// restoration. The Close above drives dialogs, selectors, and auth
    /// interactions to a terminal outcome; each flow's host-lifetime capture
    /// keeps the state (the production ModalPresenter) alive until this wait
    /// returns, so no admitted coroutine can resume against a stopped
    /// presenter or a destroyed executor. The current Session's exit path
    /// already waited for prompt/User Bash/compaction settle, so its Close
    /// finalizes synchronously here.
    [[nodiscard]] boost::asio::awaitable<support::ExpectedVoid> finish() {
        // Cancel extracted modal/session flows before terminal restoration;
        // their host-lifetime captures then let their coroutines quiesce
        // without touching a stopped presenter.
        if (auth_flows_) auth_flows_->close();
        if (session_flows_) session_flows_->close();
        running_ = false;
        // Retire the action generation so late deliveries from captured
        // hooks are rejected after Close (ADR 0040).
        retire_current_session();
        // Await every admitted controller flow to quiesce (ADR 0040: no
        // resource is destroyed while an admitted operation can still use
        // it). The counter is executor-confined; the last flow to finish
        // cancels the timer and releases this wait.
        if (in_flight_flows_ > 0) {
            flows_settled_.expires_at(std::chrono::steady_clock::time_point::max());
            boost::system::error_code wait_error;
            co_await flows_settled_.async_wait(
                boost::asio::redirect_error(boost::asio::use_awaitable, wait_error));
        }
        const auto stopped = tui_.stop();
        tui_started_ = false;
        if (!completion_result_) completion_result_.emplace();
        if (!*completion_result_) {
            if (!stopped) {
                co_return std::unexpected(aggregate_presentation_errors(
                    completion_result_->error(),
                    stopped.error(),
                    "Native TUI failed and terminal restoration failed"));
            }
            co_return std::unexpected(completion_result_->error());
        }
        if (!stopped) {
            co_return std::unexpected(presentation_error(
                stopped.error(),
                "Native TUI terminal restoration failed"));
        }
        co_return support::ExpectedVoid{};
    }

private:
    /// The assembled main-editor keybinding action-id list (pi's shared
    /// `KeybindingsManager` catalog surface), shared by the startup catalog
    /// and the `/reload` re-catalog (#418).
    [[nodiscard]] std::vector<std::string> assemble_keybinding_actions() const {
        std::vector<std::string> actions{
            "app.interrupt",
            "app.clear",
            "app.exit",
            // pi's main-editor `app.suspend` (Ctrl+Z) and
            // `app.editor.external` (Ctrl+G).
            "app.suspend",
            "app.editor.external",
            "app.tools.expand",
            "app.thinking.toggle",
            "app.thinking.cycle",
            "app.model.cycleForward",
            "app.model.cycleBackward",
            "app.model.select",
            "app.message.followUp",
            "app.message.dequeue",
            // pi `app.session.*`: recognized-but-unbound in the main editor
            // (defaultKeys [] — a user-assigned keybinding triggers the
            // flow) and selector-scoped inside the session selector.
            "app.session.new",
            "app.session.tree",
            "app.session.fork",
            "app.session.resume",
            "app.session.toggleSort",
            "app.session.toggleNamedFilter",
            "app.session.togglePath",
            "app.session.rename",
            "app.session.delete",
            "app.session.deleteNoninvasive",
            // pi's main-editor `app.message.copy` (assembled with the tree
            // selector, which matches the same action through the shared
            // registry; the copy flows land with P14's clipboard writer).
            "app.message.copy",
            // Selector-scoped: the tree selector matches the eleven
            // `app.tree.*` actions through the same registry (pi's shared
            // KeybindingsManager), with the main editor leaving them
            // unbound.
            "app.tree.foldOrUp",
            "app.tree.unfoldOrDown",
            "app.tree.editLabel",
            "app.tree.toggleLabelTimestamp",
            "app.tree.filter.default",
            "app.tree.filter.noTools",
            "app.tree.filter.userOnly",
            "app.tree.filter.labeledOnly",
            "app.tree.filter.all",
            "app.tree.filter.cycleForward",
            "app.tree.filter.cycleBackward",
            // Selector-scoped: the scoped-models selector matches the six
            // `app.models.*` actions through the same registry (pi's shared
            // KeybindingsManager).
            "app.models.save",
            "app.models.enableAll",
            "app.models.clearAll",
            "app.models.toggleProvider",
            "app.models.reorderUp",
            "app.models.reorderDown",
        };
        if (clipboard_reader_) actions.push_back("app.clipboard.pasteImage");
        return actions;
    }

    /// `/reload` keybinding re-catalog (pi `KeybindingsManager.reload()`):
    /// re-run `load_keybindings_manager` with the same assembled action list
    /// and swap the shared slot + editor (ADR 0035). Diagnostics render like
    /// startup.
    [[nodiscard]] support::ExpectedVoid re_catalog_keybindings() {
        const auto actions = assemble_keybinding_actions();
        std::vector<std::string_view> action_views;
        action_views.reserve(actions.size());
        for (const auto& action : actions) {
            action_views.push_back(action);
        }
        if (auto definitions = app_keybinding_definitions(action_views);
            !definitions) {
            return std::unexpected(definitions.error());
        } else {
            KeybindingsManagerRequest request;
            request.agent_config_directory = agent_config_directory_;
            request.application_definitions = std::move(*definitions);
            if (auto manager = load_keybindings_manager(std::move(request)); !manager) {
                return std::unexpected(manager.error());
            } else {
                if (view_ != nullptr) {
                    view_->set_keybindings(manager->registry);
                    for (const auto& diagnostic : manager->diagnostics) {
                        view_->append_diagnostic(diagnostic.message);
                    }
                    tui_.invalidate();
                }
            }
        }
        return {};
    }

    [[nodiscard]] support::Expected<InteractiveStartupDiagnostics> load_startup_resources(
        const InteractiveModeConfig& config) {
        InteractiveStartupDiagnostics diagnostics;
        agent_config_directory_ = config.agent_config_directory;
        const auto actions = assemble_keybinding_actions();
        std::vector<std::string_view> action_views;
        action_views.reserve(actions.size());
        for (const auto& action : actions) {
            action_views.push_back(action);
        }
        if (auto definitions = app_keybinding_definitions(action_views); !definitions) {
            return std::unexpected(definitions.error());
        } else {
            KeybindingsManagerRequest request;
            request.agent_config_directory = config.agent_config_directory;
            request.application_definitions = std::move(*definitions);
            if (auto manager = load_keybindings_manager(std::move(request)); !manager) {
                return std::unexpected(manager.error());
            } else {
                // The shared slot exists before the view is composed; the
                // `/reload` re-catalog replaces through the same slot (ADR
                // 0035, #418).
                if (!keybindings_) {
                    keybindings_ = std::make_shared<SharedKeybindings>();
                }
                keybindings_->replace(manager->registry);
                diagnostics.keybindings = std::move(manager->diagnostics);
            }
        }

        // The Native TUI reads only the global settings scope (the theme is
        // global-only) and writes theme selections surgically through the
        // two-scope manager with the project scope untrusted. The manager
        // stays owned by the state: the scoped-models selector persists
        // `enabledModels` through it (pi `setEnabledModels`) and the theme
        // committer below references it.
        settings_manager_.emplace(coding_agent::SettingsManager::create(
            /* cwd */ {},
            config.agent_config_directory,
            /* project_trusted */ false));
        for (const auto& settings_error : settings_manager_->errors()) {
            if (settings_error.scope == coding_agent::SettingsScope::Global) {
                return std::unexpected(support::make_error(
                    support::ErrorCode::JsonParse,
                    "could not load global settings",
                    settings_error.message));
            }
        }
        // pi `init()`: the render settings load once at boot
        // (`settingsManager.getHideThinkingBlock()` / `getOutputPad()`) and
        // apply to the chat; changes persist through the settings manager
        // and re-apply live (the two G2-graduated fields).
        hide_thinking_block_ = settings_manager_->hide_thinking_block();
        output_pad_ = settings_manager_->output_pad();
        const auto capabilities = terminal_.capabilities();
        // pi interactive-mode ctor (`setRegisteredThemes` + the
        // `InteractiveThemeController`): the controller boots from the
        // global-scope theme setting (slash automatic-pair values read as
        // unset) against the env-only COLORFGBG terminal theme with pi's
        // silent dark fallback, and owns the live palette every component
        // renders through. Registered themes arrive with the boot session
        // (pi registers the resource loader's themes in the ctor; the C++
        // boot defers session creation until after the boot trust prompt,
        // so registration happens at bind and `applyFromSettings` re-applies
        // afterwards).
        const auto weak_controller = weak_from_this();
        theme_controller_.emplace(
            config.agent_config_directory.empty()
                ? std::filesystem::path{}
                : config.agent_config_directory / "themes",
            /* registered */ std::vector<RegisteredTheme>{},
            [manager = &*settings_manager_]() {
                return manager->global_settings().theme;
            },
            [manager = &*settings_manager_](std::string_view name) {
                return manager->set_theme(coding_agent::SettingsScope::Global, name);
            },
            capabilities.color,
            tui_,
            [weak_controller](std::string message) {
                if (const auto self = weak_controller.lock()) {
                    self->post_from_view([message = std::move(message)](InteractiveState& state) {
                        state.show_error(std::move(message));
                    });
                }
            },
            [weak_controller] {
                // pi `onChanged` → `updateEditorBorderColor`: the C++ editor
                // border re-derives from the live palette at render, so the
                // change notification requests a render (pi's
                // `ui.requestRender`); the controller itself invalidates.
                if (const auto self = weak_controller.lock()) {
                    self->post_invalidate();
                }
            });
        return diagnostics;
    }

    [[nodiscard]] std::unique_ptr<InteractiveView> make_interactive_view(
        std::weak_ptr<InteractiveState> weak) {
        InteractiveViewOptions options;
        options.keybindings = keybindings_;
        // Preserve the existing production hint: the application supplies
        // the clipboard action path even when the clipboard reader is
        // unavailable, so the hint remains part of the assembled Native TUI.
        options.clipboard_paste_available = true;
        // Render invalidation stays a separate coalescible request (not part
        // of the action seam).
        options.on_invalidate = [weak] {
            if (const auto self = weak.lock()) self->post_invalidate();
        };
        // One closed action seam (ADR 0040 shape): every main-screen action
        // is admitted through post_view_action, which captures the interrupt
        // prompt generation at admission and posts exactly once to the
        // serialized executor path.
        options.action_sink =
            [weak](ViewAction action) noexcept -> support::ExpectedVoid {
            if (const auto self = weak.lock()) {
                self->post_view_action(std::move(action));
            }
            return support::ExpectedVoid{};
        };
        options.footer_data_source = [weak] {
            if (const auto self = weak.lock(); self && self->running_) {
                return self->session_ui_->compute_footer_data();
            }
            return FooterData{};
        };
        options.hide_thinking_block = hide_thinking_block_;
        options.output_pad = output_pad_;
        options.user_bash_available = view_user_shell_available();
        options.autocomplete_provider = build_autocomplete_provider();
        options.autocomplete_debounce_timer =
            std::make_unique<AsioAutocompleteDebounceTimer>(executor_);
        options.autocomplete_render_request = [weak]() -> support::ExpectedVoid {
            if (const auto self = weak.lock()) self->post_invalidate();
            return {};
        };
        options.theme = &theme_controller_->live_theme();
        return std::make_unique<InteractiveView>(std::move(options));
    }

    /// The view's user-shell hint: the interactive host always provides a
    /// User Shell to the boot session (`provide_user_shell`), so the boot
    /// path reports it before the session exists.
    [[nodiscard]] bool view_user_shell_available() const {
        if (session_ != nullptr) {
            return detail::AgentSessionInteractiveAccess::has_user_shell(*session_);
        }
        return boot_request_ && boot_request_->provide_user_shell;
    }

    /// pi `createBaseAutocompleteProvider`: the combined provider over the
    /// effective commands, prompt templates, and (while the
    /// `enableSkillCommands` setting is enabled) `skill:` commands. Rebuilt
    /// after a setting change exactly like pi's `setupAutocompleteProvider`.
    /// The boot path builds with the boot workspace and no discovered
    /// resources until the session binds (`boot_session` rebuilds it).
    [[nodiscard]] std::unique_ptr<cch::tui::AutocompleteProvider>
    build_autocomplete_provider() {
        const bool include_skill_commands =
            settings_manager_ && settings_manager_->get_enable_skill_commands();
        static const std::vector<PromptTemplate> kEmptyTemplates;
        static const std::vector<Skill> kEmptySkills;
        const auto& templates = session_ != nullptr ? session_->templates() : kEmptyTemplates;
        const auto& skills = session_ != nullptr ? session_->skills() : kEmptySkills;
        const auto workspace = session_ != nullptr
            ? session_->workspace()
            : (boot_request_ ? boot_request_->workspace : std::filesystem::path{});
        return std::make_unique<cch::tui::CombinedAutocompleteProvider>(
            command_autocomplete_commands(
                templates,
                skills,
                model_flows_->model_completion(),
                include_skill_commands),
            workspace,
            find_executable_on_path("fd"));
    }

    /// pi `setupAutocompleteProvider` after a settings change: swap the
    /// editor's autocomplete provider for a freshly built one.
    void rebuild_autocomplete_provider() {
        if (view_ == nullptr) {
            return;
        }
        view_->set_autocomplete_provider(build_autocomplete_provider());
    }

    /// pi `showLoadedResources`: refresh the loaded-resources block from the
    /// live session (Context sources, skills, templates), the registered
    /// themes, and the per-kind diagnostics (loader read diagnostics plus the
    /// theme discovery diagnostics stashed at boot/reload). Called at view
    /// initialization, after `/reload`, and after session replacement.
    void refresh_loaded_resources() {
        if (view_ == nullptr || session_ == nullptr) {
            return;
        }
        LoadedResources::Data data;
        data.cwd = session_->workspace();
        data.home = coding_agent::home_directory();
        // pi `contextFiles`: `getSystemPromptSource()` then
        // `getAppendSystemPromptSources()` then `getAgentsFiles()`.
        if (const auto& source = session_->system_prompt_source()) {
            data.context_paths.push_back(*source);
        }
        for (const auto& source : session_->append_system_prompt_sources()) {
            data.context_paths.push_back(source);
        }
        for (const auto& file : session_->context_files()) {
            data.context_paths.push_back(file.path);
        }
        for (const auto& skill : session_->skills()) {
            data.skills.push_back(LoadedResources::SkillItem{
                .name = skill.name,
                .path = skill.filePath,
                .source_info = skill.sourceInfo,
            });
        }
        for (const auto& templ : session_->templates()) {
            data.templates.push_back(LoadedResources::TemplateItem{
                .name = templ.name,
                .path = templ.filePath,
                .source_info = templ.sourceInfo,
            });
        }
        // pi `getThemes().themes` filtered to `sourcePath` (custom only).
        if (theme_controller_) {
            for (const auto& registered : theme_controller_->registered_themes()) {
                if (!registered.source_path) {
                    continue;
                }
                data.themes.push_back(LoadedResources::ThemeItem{
                    .name = registered.theme.name,
                    .path = registered.source_path->string(),
                    .scope = registered.scope,
                });
            }
        }
        data.skill_diagnostics = session_->skill_diagnostics();
        data.prompt_diagnostics = session_->prompt_diagnostics();
        // Theme conflicts: the loader's theme read diagnostics plus the
        // discovery (parse/collision) diagnostics stashed at boot/reload.
        data.theme_diagnostics = session_->theme_diagnostics();
        data.theme_diagnostics.insert(
            data.theme_diagnostics.end(),
            loaded_theme_diagnostics_.begin(),
            loaded_theme_diagnostics_.end());
        view_->set_loaded_resources_data(std::move(data));
    }

    void initialize_view(const InteractiveStartupDiagnostics& diagnostics) {
        const auto snapshot = session_->snapshot();
        view_->initialize(snapshot);
        view_->set_pending_input(snapshot.agent_state.input_queues);
        refresh_loaded_resources();
        // pi `interactive-mode.ts` `init()`: the model fallback message shows
        // as a boot warning line (`showWarning`) before the initial prompt.
        if (model_fallback_message_) {
            view_->append_warning(*model_fallback_message_);
        }
        session_ui_->append_snapshot_diagnostics(snapshot.agent_state.diagnostics);
        for (const auto& diagnostic : diagnostics.keybindings) {
            view_->append_diagnostic(diagnostic.message);
        }
        for (const auto& diagnostic : diagnostics.themes) {
            view_->append_diagnostic(diagnostic.message);
        }
        // pi `renderProjectTrustWarningIfNeeded`: the untrusted-project
        // warning renders in the chat after the initial messages when the
        // project is untrusted and a trust-requiring resource exists.
        if (project_trust_warning_needed()) {
            view_->append_trust_warning(project_trust_warning_text());
        }
    }

    /// pi `renderProjectTrustWarningIfNeeded` condition: the session's
    /// project scope is untrusted AND a trust-requiring resource exists in
    /// the session workspace.
    [[nodiscard]] bool project_trust_warning_needed() {
        if (session_ == nullptr ||
            detail::AgentSessionInteractiveAccess::is_project_trusted(*session_)) {
            return false;
        }
        auto fs = harness::WorkspaceFileSystem::create(session_->workspace());
        if (!fs) {
            return false;
        }
        auto detection = detect_project_resources(
            *fs, coding_agent::home_directory() / ".agents" / "skills");
        return needs_project_trust_resolution(detection);
    }

    [[nodiscard]] support::ExpectedVoid fail_start(const support::Error& error) {
        running_ = false;
        if (session_ != nullptr) {
            session_->close();
        }
        support::ExpectedVoid stopped;
        if (tui_started_) stopped = tui_.stop();
        tui_started_ = false;
        if (!stopped) {
            return std::unexpected(aggregate_presentation_errors(
                error,
                stopped.error(),
                "Native TUI startup and terminal restoration failed"));
        }
        return std::unexpected(startup_error(error));
    }

    void post_invalidate() {
        const auto weak = weak_from_this();
        boost::asio::post(executor_, [weak] {
            if (const auto self = weak.lock(); self && self->running_) self->tui_.invalidate();
        });
    }

    /// Admit one closed main-screen action to the serialized executor path
    /// (ADR 0040 shape). The interrupt's prompt generation is captured at
    /// admission — matching the pre-seam `post_interrupt` timing — so a fast
    /// session switch cannot retarget it; every other action carries no
    /// generation. Exactly one executor hop, then `dispatch_view_action`.
    void post_view_action(ViewAction action) {
        const auto weak = weak_from_this();
        const auto prompt_generation = generation();
        boost::asio::post(
            executor_,
            [weak, prompt_generation, action = std::move(action)]() mutable {
                if (const auto self = weak.lock()) {
                    self->dispatch_view_action(std::move(action), prompt_generation);
                }
            });
    }

    /// Route one admitted view action to its domain behavior on the
    /// serialized path. Dispatch performs the domain call directly (no second
    /// hop), preserving admission order.
    void dispatch_view_action(ViewAction action, std::size_t prompt_generation) {
        std::visit(
            [this, prompt_generation](auto&& value) {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, InterruptAction>) {
                    request_interrupt(prompt_generation, value.request);
                } else {
                    dispatch(std::move(value));
                }
            },
            std::move(action));
    }

    void dispatch(SubmitAction action) {
        submit(
            std::move(action.request.text),
            action.submission,
            {},
            SubmissionOrigin::FocusedEditor,
            action.request.editor_revision);
    }

    void dispatch(ClipboardPasteAction) {
        if (clipboard_reader_ == nullptr || clipboard_read_active_) return;
        clipboard_read_active_ = true;
        const auto self = shared_from_this();
        auto bridged = ai::detail::make_async_result_on(
            executor_,
            [self]() mutable -> boost::asio::awaitable<support::ExpectedVoid> {
                co_return co_await self->paste_from_clipboard();
            });
        std::move(bridged).start(
            [weak = weak_from_this()](support::ExpectedVoid result) noexcept {
                const auto state = weak.lock();
                if (!state) return;
                state->clipboard_read_active_ = false;
                // Baseline clipboard failures are intentionally silent; the
                // private bridge already mapped a launch failure to the same
                // generic outcome.
                (void)result;
            });
    }

    void dispatch(DequeueAction) {
        dequeue_pending_input(true);
    }

    void dispatch(ExitAction) {
        request_exit();
    }

    void dispatch(CycleModelAction action) {
        model_flows_->cycle_model(
            action.direction == ModelCycleDirection::Forward ? "forward" : "backward");
    }

    void dispatch(CycleThinkingAction) {
        cycle_thinking_level();
    }

    void dispatch(ToggleThinkingAction) {
        toggle_thinking_block_visibility();
    }

    void dispatch(SelectModelAction) {
        model_flows_->show_model_selector(std::nullopt);
    }

    void dispatch(ResumeSessionAction) {
        session_flows_->open_resume();
    }

    void dispatch(ForkSessionAction) {
        session_flows_->open_fork();
    }

    void dispatch(NewSessionAction) {
        session_flows_->open_new();
    }

    void dispatch(CopyLastMessageAction) {
        handle_copy_last_message();
    }

    void dispatch(OpenTreeSelectorAction) {
        session_flows_->open_tree();
    }

    void dispatch(SuspendAction) {
        handle_suspend();
    }

    void dispatch(ExternalEditorAction) {
        const auto self = shared_from_this();
        auto bridged = ai::detail::make_async_result_on(
            executor_,
            [self]() mutable -> boost::asio::awaitable<support::ExpectedVoid> {
                co_await self->handle_open_external_editor();
                co_return support::ExpectedVoid{};
            });
        std::move(bridged).start([self](support::ExpectedVoid result) noexcept {
            if (!result) {
                // The bridge maps a launch/body failure to the generic
                // outcome; the strict build treats a failure as a Runtime
                // invariant (the coroutine converts its own failures).
                self->show_error("External editor failed");
            }
        });
    }

    [[nodiscard]] boost::asio::awaitable<support::ExpectedVoid> paste_from_clipboard() {
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
        try {
#endif
            auto image = co_await clipboard_reader_->read_image();
            if (image && *image && !(*image)->bytes.empty()) {
                const auto mime_type = sniff_supported_image_mime_type((*image)->bytes);
                const auto extension = mime_type
                    ? extension_for_image_mime_type(*mime_type)
                    : std::nullopt;
                if (extension) {
                    const auto path = write_clipboard_image((*image)->bytes, *extension);
                    if (path) {
                        if (running_ && view_ != nullptr) {
                            view_->insert_editor_text(path->string());
                            tui_.invalidate();
                        }
                        co_return support::ExpectedVoid{};
                    }
                }
            }
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
        } catch (const std::exception& error) {
            const auto ignored = support::make_error(
                support::ErrorCode::Unknown,
                "clipboard image read failed",
                error.what());
            (void)ignored;
        } catch (...) {
            const auto ignored = support::make_error(
                support::ErrorCode::Unknown,
                "clipboard image read failed");
            (void)ignored;
        }
#endif

#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
        try {
#endif
            auto text = co_await clipboard_reader_->read_text();
            if (text && *text && !(*text)->empty() && running_ && view_ != nullptr) {
                view_->insert_editor_text(std::move(**text));
                tui_.invalidate();
            }
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
        } catch (const std::exception& error) {
            const auto ignored = support::make_error(
                support::ErrorCode::Unknown,
                "clipboard text read failed",
                error.what());
            (void)ignored;
        } catch (...) {
            const auto ignored = support::make_error(
                support::ErrorCode::Unknown,
                "clipboard text read failed");
            (void)ignored;
        }
#endif
        co_return support::ExpectedVoid{};
    }

    void post_exit() {
        const auto weak = weak_from_this();
        boost::asio::post(executor_, [weak] {
            if (const auto self = weak.lock()) self->request_exit();
        });
    }

    void post_render() {
        const auto weak = weak_from_this();
        boost::asio::post(executor_, [weak] {
            if (const auto self = weak.lock()) self->render();
        });
    }

    void post_close_overlay() {
        const auto weak = weak_from_this();
        boost::asio::post(executor_, [weak] {
            if (const auto self = weak.lock()) self->close_overlay();
        });
    }

    /// Append one bounded presentation error to the chat diagnostic area.
    void append_command_error(const support::Error& error) {
        if (view_ == nullptr) return;
        view_->append_diagnostic(combined_error_text(error));
        tui_.invalidate();
    }

    [[nodiscard]] support::ExpectedVoid attach_overlay(
        std::unique_ptr<cch::tui::Overlay> overlay) {
        auto* overlay_pointer = overlay.get();
        if (auto attached = tui_.add_overlay(std::move(overlay)); !attached) {
            return std::unexpected(attached.error());
        }
        active_overlay_ = overlay_pointer;
        if (auto focused = tui_.set_focus(active_overlay_); !focused) {
            const auto focus_error = focused.error();
            if (auto removed = tui_.remove_overlay(active_overlay_); !removed) {
                return std::unexpected(aggregate_presentation_errors(
                    focus_error,
                    removed.error(),
                    "Native TUI overlay focus and cleanup failed"));
            }
            active_overlay_ = nullptr;
            return std::unexpected(focus_error);
        }
        tui_.invalidate();
        return {};
    }

    void close_overlay() override {
        if (!running_ || active_overlay_ == nullptr) return;
        if (auto removed = tui_.remove_overlay(active_overlay_); !removed) {
            append_command_error(removed.error());
            return;
        }
        active_overlay_ = nullptr;
        tui_.invalidate();
    }

    /// pi `showSettingsSelector`: the settings selector renders in the editor
    /// slot (pi's `showSelector` editorContainer swap) over the #327 settings
    /// subset plus the two graduated render settings. Changes persist through
    /// the settings manager (global scope, surgical field-level merge) and
    /// apply live; the Theme item opens the G5 single-mode ThemeSubmenu with
    /// in-memory preview, a global-scope settings commit on confirm, and
    /// cancel-does-not-revert.
    void show_settings_selector() {
        if (!running_ || view_ == nullptr || session_ == nullptr ||
            !session_->is_open() || !theme_controller_ || !keybindings_ ||
            !settings_manager_) {
            return;
        }
        if (active_overlay_ != nullptr) return;

        const auto snapshot = session_->snapshot();
        SettingsSelectorConfig config;
        config.hide_thinking_block = hide_thinking_block_;
        config.output_pad = output_pad_;
        config.enable_skill_commands =
            settings_manager_->get_enable_skill_commands();
        config.thinking_level = snapshot.agent_state.thinking_level;
        const auto supported = ai::get_supported_thinking_levels(snapshot.agent_state.model);
        config.available_thinking_levels.reserve(supported.size());
        for (const auto level : supported) {
            if (const auto name = ai::detail::model_thinking_level_name(level)) {
                config.available_thinking_levels.emplace_back(*name);
            }
        }
        config.default_project_trust =
            settings_manager_->default_project_trust().value_or(DefaultProjectTrust::Ask);
        // pi settings-selector.ts config: the raw theme setting (`|| "dark"`),
        // the active theme name (the `(current)` marker source), and the
        // sorted available themes.
        config.current_theme = settings_manager_->global_settings().theme.value_or("dark");
        config.active_theme = std::string{theme_controller_->active_theme_name()};
        config.available_themes = theme_controller_->available_theme_names();

        const auto weak = weak_from_this();
        SettingsSelectorCallbacks callbacks;
        callbacks.on_hide_thinking_block_change = [weak](bool hidden) {
            if (const auto self = weak.lock()) {
                self->post_from_view([hidden](InteractiveState& state) {
                    state.set_hide_thinking_block_setting(hidden);
                });
            }
        };
        callbacks.on_output_pad_change = [weak](std::size_t padding) {
            if (const auto self = weak.lock()) {
                self->post_from_view([padding](InteractiveState& state) {
                    state.set_output_pad_setting(padding);
                });
            }
        };
        callbacks.on_enable_skill_commands_change = [weak](bool enabled) {
            if (const auto self = weak.lock()) {
                self->post_from_view([enabled](InteractiveState& state) {
                    // pi `onEnableSkillCommandsChange`:
                    // `setEnableSkillCommands(enabled)` then
                    // `setupAutocompleteProvider()`.
                    if (auto saved =
                            state.settings_manager_->set_enable_skill_commands(enabled);
                        !saved) {
                        state.show_error(combined_error_text(saved.error()));
                    }
                    state.rebuild_autocomplete_provider();
                });
            }
        };
        callbacks.on_thinking_level_change = [weak](std::string level) {
            if (const auto self = weak.lock()) {
                self->post_from_view([level = std::move(level)](InteractiveState& state) mutable {
                    // pi `onThinkingLevelChange` → `session.setThinkingLevel`:
                    // the session persists the `thinking_level_change` entry
                    // and the global settings default itself.
                    auto applied = state.session_->set_thinking_level(level);
                    if (!applied) {
                        state.show_error(combined_error_text(applied.error()));
                    }
                });
            }
        };
        callbacks.on_default_project_trust_change = [weak](DefaultProjectTrust trust) {
            if (const auto self = weak.lock()) {
                self->post_from_view([trust](InteractiveState& state) {
                    if (auto saved =
                            state.settings_manager_->set_default_project_trust(trust);
                        !saved) {
                        state.show_error(combined_error_text(saved.error()));
                    }
                });
            }
        };
        callbacks.on_cancel = [weak] {
            if (const auto self = weak.lock()) {
                self->post_from_view([](InteractiveState& state) { state.restore_prompt_slot(); });
            }
        };
        callbacks.on_theme_change = [weak](std::string theme_setting) {
            if (const auto self = weak.lock()) {
                self->post_from_view([theme_setting = std::move(theme_setting)](InteractiveState& state) {
                    // pi `onThemeChange`: `settingsManager.setTheme(themeSetting)`
                    // then `themeController.applyFromSettings()`.
                    if (auto saved = state.settings_manager_->set_theme(
                            coding_agent::SettingsScope::Global,
                            theme_setting);
                        !saved) {
                        state.show_error(combined_error_text(saved.error()));
                    }
                    if (state.theme_controller_) {
                        state.theme_controller_->apply_from_settings();
                    }
                });
            }
        };
        callbacks.on_theme_preview = [weak](std::string theme_name) {
            if (const auto self = weak.lock()) {
                self->post_from_view([theme_name = std::move(theme_name)](InteractiveState& state) {
                    if (state.theme_controller_) {
                        state.theme_controller_->preview(theme_name);
                    }
                });
            }
        };

        auto selector = std::make_shared<SettingsSelectorComponent>(
            theme_controller_->live_theme(),
            keybindings_->get(),
            std::move(config),
            std::move(callbacks));
        replace_prompt_slot(std::move(selector));
    }

    /// pi `setHideThinkingBlock` + live chat rebuild: persist the global
    /// `hideThinkingBlock` setting and rebuild the chat from the session
    /// snapshot so the assistant messages re-render with the new visibility.
    void set_hide_thinking_block_setting(bool hidden) {
        hide_thinking_block_ = hidden;
        if (settings_manager_) {
            if (auto persisted = settings_manager_->set_hide_thinking_block(hidden);
                !persisted) {
                show_error(combined_error_text(persisted.error()));
            }
        }
        rebuild_chat();
    }

    /// pi `setOutputPad` + live chat rebuild: persist the global `outputPad`
    /// setting and rebuild the chat so user/assistant messages re-render with
    /// the new padding.
    void set_output_pad_setting(std::size_t padding) {
        output_pad_ = padding;
        if (settings_manager_) {
            if (auto persisted = settings_manager_->set_output_pad(padding);
                !persisted) {
                show_error(combined_error_text(persisted.error()));
            }
        }
        rebuild_chat();
    }

    /// pi `toggleThinkingBlockVisibility`: flip the local render setting,
    /// persist it through the settings manager, rebuild the chat from the
    /// session (streaming message included), and report the pi status line.
    void toggle_thinking_block_visibility() {
        hide_thinking_block_ = !hide_thinking_block_;
        if (settings_manager_) {
            if (auto persisted =
                    settings_manager_->set_hide_thinking_block(hide_thinking_block_);
                !persisted) {
                show_error(combined_error_text(persisted.error()));
            }
        }
        rebuild_chat();
        show_status(
            "Thinking blocks: " +
            std::string{hide_thinking_block_ ? "hidden" : "visible"});
    }

    /// Rebuild the chat from the authoritative session snapshot (pi
    /// `rebuildChatFromMessages`): the render settings apply first, the
    /// streaming assistant message re-renders with them, and the
    /// pending-input queue display is restored.
    void rebuild_chat() {
        if (view_ == nullptr) return;
        view_->apply_render_settings(hide_thinking_block_, output_pad_);
        const auto snapshot = session_->snapshot();
        view_->initialize(snapshot);
        view_->set_pending_input(snapshot.agent_state.input_queues);
        tui_.invalidate();
    }

    void show_help_command() {
        if (view_ == nullptr) return;
        view_->append_frontend_message(
            "Available commands:\n"
            "/clear /new /quit /exit /q /copy /session /hotkeys /settings\n"
            "/help /commands /name /model /models /scoped-models /thinking\n"
            "/login /logout /resume /fork /tree /reload /compact /trust");
        tui_.invalidate();
    }

    void open_hotkeys() {
        if (active_overlay_ != nullptr || !keybindings_) return;
        cch::tui::OverlayOptions options;
        options.position = cch::tui::OverlayPosition::TopLeft;
        options.size_constraints.max_width = 90;
        options.size_constraints.max_height = 26;
        options.z_index = 100;
        auto overlay = std::make_unique<cch::tui::Overlay>(std::move(options));
        const auto weak = weak_from_this();
        auto content = std::make_unique<DismissibleView>(
            make_hotkey_help_view(keybindings_->get()),
            keybindings_->get(),
            [weak] {
                if (const auto self = weak.lock()) self->post_close_overlay();
            });
        if (auto attached = overlay->add_child(std::move(content)); !attached) {
            append_command_error(attached.error());
            return;
        }
        show_overlay(std::move(overlay));
    }

    // ── Executor and detached-flow host seam ─────────────────────────────

    /// Post one view-thread action to the executor.
    void post_from_view(std::move_only_function<void(InteractiveState&)> action) {
        const auto weak = weak_from_this();
        boost::asio::post(executor_, [weak, action = std::move(action)]() mutable {
            if (const auto self = weak.lock(); self && self->running_) action(*self);
        });
    }

    /// Spawn one detached executor flow; a frame failure becomes a chat
    /// diagnostic (the user-bash precedent; the login flows use this too).
    /// Every admitted flow is counted so `finish()` can await quiescence
    /// (ADR 0040: terminal restoration never races a controller coroutine).
    void spawn_flow(
        std::move_only_function<boost::asio::awaitable<void>()> start,
        std::string failure_label) {
        ++in_flight_flows_;
        const auto weak = weak_from_this();
        // The coroutine lambda's frame may reference its closure (the
        // `start` move_only_function), so keep the closure alive until the
        // spawned coroutine reaches its terminal completion (the bridge holds
        // the factory for the coroutine's whole lifetime; ADR 0040
        // §Behavior mechanisms).
        auto start_owner =
            std::make_shared<std::move_only_function<boost::asio::awaitable<void>()>>(
                std::move(start));
        auto bridged = ai::detail::make_async_result_on(
            executor_,
            [start_owner]() mutable -> boost::asio::awaitable<support::ExpectedVoid> {
                co_await (*start_owner)();
                co_return support::ExpectedVoid{};
            });
        std::move(bridged).start(
            [weak, failure_label = std::move(failure_label)](support::ExpectedVoid result) noexcept {
                if (const auto self = weak.lock()) {
                    self->flow_finished();
                    if (!result && self->running_ && self->view_ != nullptr) {
                        self->view_->append_diagnostic(std::move(failure_label));
                        self->tui_.invalidate();
                    }
                }
            });
    }

    /// One admitted detached flow reached its terminal outcome. The count is
    /// executor-confined (spawn and completion both run on the host
    /// executor); reaching zero releases the `finish()` quiescence wait.
    void flow_finished() noexcept {
        if (in_flight_flows_ > 0) {
            --in_flight_flows_;
        }
        if (in_flight_flows_ == 0) {
            (void)flows_settled_.cancel();
        }
    }

    // ── ModalPresenter seam (#503): the interactive state is the
    // production presenter — overlays, prompt-slot swaps, and status text
    // land on the view/terminal it owns. ───────────────────────────────────

    void show_overlay(std::unique_ptr<cch::tui::Overlay> overlay) override {
        if (auto attached = attach_overlay(std::move(overlay)); !attached) {
            append_command_error(attached.error());
        }
    }

    /// pi `showSelector` editorContainer swap: a modal component replaces
    /// the editor slot.
    void replace_prompt_slot(std::shared_ptr<cch::tui::Component> component) override {
        if (view_ == nullptr) return;
        view_->set_editor_replacement(std::move(component));
        tui_.invalidate();
    }

    void restore_prompt_slot() override {
        if (view_ == nullptr) return;
        view_->restore_editor();
        tui_.invalidate();
    }

    /// pi `showStatus`: one dim status line in the chat.
    void show_status(std::string text) override {
        if (view_ == nullptr) return;
        view_->append_status_message(std::move(text));
        tui_.invalidate();
    }

    /// pi `showError`: one `Error: <text>` chat line.
    void show_error(std::string text) override {
        if (view_ == nullptr) return;
        view_->append_diagnostic(std::move(text));
        tui_.invalidate();
    }

    /// pi `ui.requestRender`: one coalescible re-render request, safe from
    /// any thread (posts the render to the executor).
    void request_render() override {
        post_invalidate();
    }

    /// Mark the frame dirty without rendering immediately.
    void invalidate() override {
        tui_.invalidate();
    }

    // ── Model flows (pi interactive-mode.ts, #407; extraction #503) ──────
    // The `/model`, `/models`, and Ctrl+P flows live in ModelFlowController;
    // the state is its ModalPresenter and wires the host hooks here.

    /// Wire the production host hooks for the model flows: gates read this
    /// state's running/view facts, executor hops reuse post_from_view and
    /// spawn_flow, and the current session resolves at execution time so
    /// session replacement applies to flows in flight. All hooks capture the
    /// state weakly; nothing here extends the state's lifetime.
    [[nodiscard]] std::shared_ptr<ModelFlowController> make_model_flow_controller() {
        const auto weak = weak_from_this();
        ModelFlowHostHooks hooks;
        hooks.is_live = [weak] {
            const auto self = weak.lock();
            return self && self->running_ && self->view_ != nullptr;
        };
        hooks.post_on_executor = [weak](std::move_only_function<void()> action) mutable {
            if (const auto self = weak.lock()) {
                self->post_from_view(
                    [action = std::move(action)](InteractiveState&) mutable { action(); });
            }
        };
        hooks.spawn_flow =
            [weak](std::move_only_function<boost::asio::awaitable<void>()> start,
                   std::string failure_label) mutable {
                if (const auto self = weak.lock()) {
                    self->spawn_flow(std::move(start), std::move(failure_label));
                }
            };
        hooks.current_session = [weak]() -> AgentSession* {
            const auto self = weak.lock();
            return self != nullptr ? self->session_ : nullptr;
        };
        // The controller is created after `theme_controller_` is emplaced and
        // the optional is never reset, so the pointer stays valid for the
        // state's lifetime (flows reach it only under the gates above).
        hooks.live_theme = [theme = &*theme_controller_]() -> const LiveTheme& {
            return theme->live_theme();
        };
        return std::make_shared<ModelFlowController>(
            executor_,
            *this,
            std::weak_ptr<void>{weak_from_this()},
            std::move(hooks),
            keybindings_,
            settings_manager_ ? &*settings_manager_ : nullptr);
    }

    /// Wire the authentication controller through the same executor,
    /// lifetime, and generation seams as ModelFlowController. Auth dialogs
    /// never receive the InteractiveState or terminal directly.
    [[nodiscard]] std::shared_ptr<AuthFlowController> make_auth_flow_controller() {
        const auto weak = weak_from_this();
        AuthFlowHostHooks hooks;
        hooks.post_on_executor = [weak](std::move_only_function<void()> action) mutable {
            if (const auto self = weak.lock()) {
                self->post_from_view(
                    [action = std::move(action)](InteractiveState&) mutable { action(); });
            }
        };
        hooks.spawn_flow =
            [weak](std::move_only_function<boost::asio::awaitable<void>()> start,
                   std::string failure_label) mutable {
                if (const auto self = weak.lock()) {
                    self->spawn_flow(std::move(start), std::move(failure_label));
                }
            };
        hooks.current_session = [weak]() -> AgentSession* {
            const auto self = weak.lock();
            return self != nullptr ? self->session_ : nullptr;
        };
        hooks.live_theme = [theme = &*theme_controller_]() -> const LiveTheme& {
            return theme->live_theme();
        };
        hooks.action_generation = [weak] {
            const auto self = weak.lock();
            return self != nullptr ? self->action_generation_ : 0;
        };
        hooks.open_browser = [weak](std::size_t generation, std::string url) {
            if (const auto self = weak.lock()) {
                (void)self->deliver_action(
                    generation,
                    TuiActionVariant{OpenBrowserAction{std::move(url)}});
            }
        };
        return std::make_shared<AuthFlowController>(
            executor_,
            *this,
            std::weak_ptr<void>{weak_from_this()},
            std::move(hooks),
            keybindings_);
    }

    /// Wire the session controller through explicit host hooks. The adapter
    /// below owns the remaining InteractiveView/resource mechanics while the
    /// controller owns every multi-step session flow.
    [[nodiscard]] std::shared_ptr<SessionFlowController> make_session_flow_controller() {
        const auto weak = weak_from_this();
        SessionFlowHostHooks hooks;
        hooks.is_live = [weak] {
            const auto self = weak.lock();
            return self && self->running_ && self->view_ != nullptr;
        };
        hooks.post_on_executor = [weak](std::move_only_function<void()> action) mutable {
            if (const auto self = weak.lock()) {
                self->post_from_view(
                    [action = std::move(action)](InteractiveState&) mutable { action(); });
            }
        };
        hooks.spawn_flow =
            [weak](std::move_only_function<boost::asio::awaitable<void>()> start,
                   std::string failure_label) mutable {
                if (const auto self = weak.lock()) {
                    self->spawn_flow(std::move(start), std::move(failure_label));
                }
            };
        hooks.current_session = [weak]() -> AgentSession* {
            const auto self = weak.lock();
            return self != nullptr ? self->session_ : nullptr;
        };
        hooks.live_theme = [theme = &*theme_controller_]() -> const LiveTheme& {
            return theme->live_theme();
        };
        hooks.terminal_rows = [weak] {
            const auto self = weak.lock();
            return self != nullptr ? self->terminal_.dimensions().rows : 0;
        };
        hooks.action_generation = [weak] {
            const auto self = weak.lock();
            return self != nullptr ? self->action_generation_ : 0;
        };
        hooks.make_session_request = [weak](
            std::filesystem::path workspace,
            SessionTarget target) {
            if (const auto self = weak.lock()) {
                return self->make_session_request(
                    std::move(workspace), std::move(target));
            }
            runtime::AgentSessionCreationRequest request;
            request.workspace = std::move(workspace);
            request.session_target = std::move(target);
            return request;
        };
        hooks.request_session_replacement = [weak](
            std::size_t generation,
            runtime::AgentSessionCreationRequest request)
            -> support::Expected<coding_agent::CreateAgentSessionResult> {
            if (const auto self = weak.lock()) {
                return self->request_session_replacement(
                    generation, std::move(request));
            }
            return std::unexpected(support::make_error(
                support::ErrorCode::Cancelled,
                "Session flow host is no longer active"));
        };
        hooks.replace_session = [weak](std::unique_ptr<AgentSession> next)
            -> support::ExpectedVoid {
            if (const auto self = weak.lock()) {
                return self->replace_session(std::move(next));
            }
            return std::unexpected(support::make_error(
                support::ErrorCode::Cancelled,
                "Session flow host is no longer active"));
        };
        hooks.show_warning = [weak](std::string text) {
            if (const auto self = weak.lock(); self && self->view_ != nullptr) {
                self->view_->append_warning(std::move(text));
            }
        };
        hooks.show_frontend_message = [weak](std::string text) {
            if (const auto self = weak.lock(); self && self->view_ != nullptr) {
                self->view_->append_frontend_message(std::move(text));
            }
        };
        hooks.clear_status_indicator = [weak] {
            if (const auto self = weak.lock(); self && self->view_ != nullptr) {
                self->view_->clear_status_indicator();
            }
        };
        hooks.set_editor_text = [weak](std::string text) {
            if (const auto self = weak.lock(); self && self->view_ != nullptr) {
                self->view_->set_editor_text(std::move(text));
            }
        };
        hooks.editor_text = [weak] {
            if (const auto self = weak.lock(); self && self->view_ != nullptr) {
                return self->view_->editor_text();
            }
            return std::string{};
        };
        hooks.copy_to_clipboard = [weak](std::string text) {
            const auto self = weak.lock();
            return self != nullptr && self->write_clipboard_text_sink(text);
        };
        hooks.dequeue_pending_input = [weak] {
            if (const auto self = weak.lock()) self->dequeue_pending_input(false);
        };
        hooks.rebuild_chat = [weak] {
            if (const auto self = weak.lock()) self->rebuild_chat();
        };
        hooks.apply_reload_result = [weak](runtime::AgentSessionReloadResult result)
            -> support::ExpectedVoid {
            const auto self = weak.lock();
            if (!self) {
                return std::unexpected(support::make_error(
                    support::ErrorCode::Cancelled,
                    "Session flow host is no longer active"));
            }
            if (auto rebind = self->re_catalog_keybindings(); !rebind) {
                return std::unexpected(rebind.error());
            }
            if (self->theme_controller_) {
                auto discovery = coding_agent::tui::discover_themes(
                    std::move(result.themes));
                self->loaded_theme_diagnostics_ = std::move(discovery.diagnostics);
                self->theme_controller_->set_registered_themes(
                    std::move(discovery.themes));
                self->theme_controller_->apply_from_settings();
            }
            if (self->settings_manager_) {
                self->hide_thinking_block_ =
                    self->settings_manager_->hide_thinking_block();
                self->output_pad_ = self->settings_manager_->output_pad();
            }
            if (self->view_ != nullptr) {
                self->view_->apply_render_settings(
                    self->hide_thinking_block_, self->output_pad_);
            }
            self->rebuild_autocomplete_provider();
            self->refresh_loaded_resources();
            if (auto runtime = self->session_->model_runtime()) {
                if (auto error = runtime->get_error(); error && !error->empty()) {
                    self->show_error("models.json error: " + *error);
                }
            }
            return {};
        };
        hooks.set_compaction_active = [weak](bool active) {
            if (const auto self = weak.lock()) self->compaction_active_ = active;
        };
        hooks.signal_exit = [weak] {
            if (const auto self = weak.lock(); self && self->exit_requested_ &&
                !self->prompt_active_ && !self->user_bash_active_ &&
                !self->compaction_active_) {
                self->signal_exit();
            }
        };
        hooks.request_exit = [weak] {
            if (const auto self = weak.lock()) self->post_exit();
        };
        hooks.report_boot_diagnostics = [weak](
            std::size_t generation,
            std::vector<SessionDiagnostic> diagnostics) {
            if (const auto self = weak.lock()) {
                (void)self->deliver_action(
                    generation,
                    TuiActionVariant{ReportBootDiagnosticsAction{
                        std::move(diagnostics)}});
            }
        };
        return std::make_shared<SessionFlowController>(
            executor_,
            *this,
            std::weak_ptr<void>{weak_from_this()},
            std::move(hooks),
            keybindings_,
            settings_manager_ ? &*settings_manager_ : nullptr);
    }

    /// Wire the session UI binding's host hooks: the binding owns the Agent
    /// Session subscriptions, the streaming-event translation, the retry
    /// countdown, and the footer data computation; the gates read this
    /// state's running/view facts and the presentation channels route through
    /// the ModalPresenter methods. All hooks capture the state weakly;
    /// nothing here extends the state's lifetime.
    [[nodiscard]] std::shared_ptr<SessionUiBinding> make_session_ui_binding() {
        const auto weak = weak_from_this();
        SessionUiBindingHooks hooks;
        hooks.is_live = [weak] {
            const auto self = weak.lock();
            return self && self->running_;
        };
        hooks.view = [weak]() -> InteractiveView* {
            const auto self = weak.lock();
            return self != nullptr ? self->view_ : nullptr;
        };
        hooks.prompt_active = [weak] {
            const auto self = weak.lock();
            return self && self->prompt_active_;
        };
        hooks.invalidate = [weak] {
            if (const auto self = weak.lock()) self->tui_.invalidate();
        };
        hooks.show_status = [weak](std::string text) {
            if (const auto self = weak.lock()) self->show_status(std::move(text));
        };
        hooks.show_error = [weak](std::string text) {
            if (const auto self = weak.lock()) self->show_error(std::move(text));
        };
        hooks.boot_workspace = [weak]() -> std::filesystem::path {
            const auto self = weak.lock();
            if (self != nullptr && self->boot_request_) {
                return self->boot_request_->workspace;
            }
            return {};
        };
        hooks.auto_compact_enabled = [weak]() -> std::optional<bool> {
            const auto self = weak.lock();
            if (!self || !self->settings_manager_) return std::nullopt;
            const auto compaction = self->settings_manager_->settings().compaction;
            return !compaction || compaction->enabled.value_or(true);
        };
        return std::make_shared<SessionUiBinding>(executor_, std::move(hooks));
    }

    /// pi `handleCtrlZ`: stop the TUI (restore the terminal), ignore SIGINT
    /// while suspended, keep the process alive, and stop the process group
    /// with SIGTSTP; the SIGCONT handler restarts the TUI and forces a
    /// re-render (pi's `ui.start()` + `requestRender(true)`).

    void handle_suspend() {
        if (suspend_signals_) return;
        // pi `handleCtrlZ`: stop the TUI first so the terminal is restored
        // before the process group stops; the exit-wait timer keeps the
        // io_context alive while suspended (pi's keep-alive interval).
        const auto stopped = tui_.stop();
        if (!stopped) {
            completion_result_ = std::unexpected(presentation_error(
                stopped.error(),
                "Native TUI suspension failed"));
            request_exit();
            return;
        }

        // Ignore SIGINT while suspended so Ctrl+C in the terminal does not
        // kill the backgrounded process; the handler is removed on resume
        // (pi's `process.on("SIGINT", ignoreSigint)`). SIGCONT restores the
        // TUI and re-renders (pi's `process.once("SIGCONT", ...)`). The
        // shared wait re-arms after a swallowed SIGINT so the resume
        // handler stays registered.
        auto signals = std::make_shared<boost::asio::signal_set>(executor_, SIGCONT);
        signals->add(SIGINT);
        const auto weak = weak_from_this();
        auto arm = std::make_shared<std::move_only_function<void()>>();
        *arm = [weak, signals, arm] {
            signals->async_wait([weak, signals, arm](const boost::system::error_code& error, int fired) {
                // Terminal outcomes (cancellation on resume/teardown, or
                // SIGCONT) clear the re-arm function to break its
                // self-capture cycle; only the swallowed-SIGINT path re-arms
                // (ASan, issue #473).
                if (error) {
                    *arm = nullptr;
                    return;
                }
                if (fired != SIGCONT) {
                    // SIGINT while suspended: swallowed; keep waiting.
                    (*arm)();
                    return;
                }
                *arm = nullptr;
                const auto self = weak.lock();
                if (self) self->resume_after_suspend();
            });
        };
        (*arm)();
        suspend_signals_ = std::move(signals);

        // pi `process.kill(0, "SIGTSTP")` through the closed action seam; a
        // null host sends SIGTSTP to the process group directly.
        (void)deliver_action(
            action_generation_,
            TuiActionVariant{SuspendProcessAction{}});
    }

    /// pi's SIGCONT handler body: restore the TUI and request a full render.
    void resume_after_suspend() {
        suspend_signals_.reset();
        if (!running_) return;
        if (auto started = tui_.start(); !started) {
            completion_result_ = std::unexpected(presentation_error(
                started.error(),
                "Native TUI resume after suspend failed"));
            request_exit();
            return;
        }
        if (auto rendered = tui_.render(); !rendered) {
            completion_result_ = std::unexpected(startup_error(rendered.error()));
            request_exit();
            return;
        }
    }

    /// pi `handleOpenExternalEditor`: stop the TUI, run the external editor
    /// over the expanded editor content, restore the TUI, and replace the
    /// editor content on a clean exit. Cleanup of the temp prompt file is
    /// best effort (pi `external-editor.ts`).

    [[nodiscard]] boost::asio::awaitable<void> handle_open_external_editor() {
        if (view_ == nullptr) co_return;
        const auto command = external_editor_command();
        const auto content = view_->editor_expanded_text();
        const auto stopped = tui_.stop();
        if (!stopped) {
            completion_result_ = std::unexpected(presentation_error(
                stopped.error(),
                "Native TUI external editor stop failed"));
            request_exit();
            co_return;
        }
        auto result = co_await edit_in_external_editor(command, content);
        // Restore the TUI on every exit path (pi's `finally`).
        if (auto started = tui_.start(); !started) {
            completion_result_ = std::unexpected(presentation_error(
                started.error(),
                "Native TUI external editor resume failed"));
            request_exit();
            co_return;
        }
        if (auto rendered = tui_.render(); !rendered) {
            completion_result_ = std::unexpected(startup_error(rendered.error()));
            request_exit();
            co_return;
        }
        if (result && *result && view_ != nullptr) {
            view_->set_editor_text(std::move(**result));
            tui_.invalidate();
        }
    }

    /// pi `handleCopyCommand`: copy the last assistant message's text and
    /// report the pi statuses.
    void handle_copy_last_message() {
        const auto text = session_->last_assistant_text();
        if (!text || text->empty()) {
            show_error("No agent messages to copy yet.");
            return;
        }
        if (!write_clipboard_text_sink(*text)) {
            show_error("Failed to copy to clipboard");
            return;
        }
        show_status("Copied last agent message to clipboard");
    }

    /// pi `handleNameCommand`: `/name <name>` sanitizes and persists the
    /// `session_info` entry and reports pi's statuses; a bare `/name` shows
    /// the current name or the usage warning.
    void handle_name_command(std::string name) {
        if (name.empty()) {
            const auto current = session_->session_name();
            if (current && !current->empty()) {
                view_->append_frontend_message(
                    std::format("Session name: {}", *current));
            } else {
                view_->append_warning("Usage: /name <name>");
            }
            tui_.invalidate();
            return;
        }
        auto stored = session_->set_session_name(name);
        if (!stored) {
            append_command_error(stored.error());
            return;
        }
        if (stored->has_value() && *stored != name) {
            // pi `showWarning("Session name was normalized from
            // ${JSON.stringify(name)} to ${JSON.stringify(sessionName)}")`.
            view_->append_warning(std::format(
                "Session name was normalized from {} to {}",
                json_quote_string(name),
                json_quote_string(**stored)));
        }
        view_->append_frontend_message(
            std::format("Session name set: {}", stored->value_or(name)));
        tui_.invalidate();
    }

    /// pi `handleSessionCommand`: the Session Info chat block over the
    /// session name, file, id, message counts, and token totals (pi
    /// `getSessionStats` shape; the C++ subset renders the data the session
    /// exposes).
    void handle_session_command() {
        // pi `handleSessionCommand` shape: Name (when set), File, ID, the
        // Messages breakdown, and the Tokens totals. Workspace/provider/model
        // are not pi fields and are intentionally absent (strict subset).
        const auto name = session_->session_name();
        const auto path = session_->session_path();
        const auto stats = session_->session_stats();
        std::string info = "Session Info\n\n";
        if (name && !name->empty()) {
            info += std::format("Name: {}\n", *name);
        }
        info += std::format("File: {}\n", path ? path->string() : std::string{"In-memory"});
        info += std::format("ID: {}\n\n", session_->session_id());
        info += "Messages\n";
        info += std::format("Total: {}\n", stats.total_messages);
        info += std::format("User: {}\n", stats.user_messages);
        info += std::format("Assistant: {}\n", stats.assistant_messages);
        info += std::format("Tools: {} calls, {} results\n", stats.tool_calls, stats.tool_results);
        info += "\nTokens\n";
        // pi: "Input" is the full prompt volume (input + cached + written);
        // the C++ subset renders the provider-independent split.
        const auto prompt_tokens = stats.input_tokens + stats.cache_read + stats.cache_write;
        info += std::format("Input: {}\n", prompt_tokens);
        if (prompt_tokens > 0 && (stats.cache_read > 0 || stats.cache_write > 0)) {
            info += std::format("Cached: {}\n", stats.cache_read);
            info += std::format("Uncached: {}\n", stats.input_tokens + stats.cache_write);
        }
        info += std::format("Output: {}\n", stats.output_tokens);
        info += std::format("Total: {}\n", prompt_tokens + stats.output_tokens);
        view_->append_frontend_message(std::move(info));
        tui_.invalidate();
    }

    /// The configured clipboard writer (pi `copyToClipboard` platform-tools
    /// path; tests inject a recorder).
    [[nodiscard]] bool write_clipboard_text_sink(const std::string& text) {
        auto result = deliver_action(
            action_generation_,
            TuiActionVariant{WriteClipboardAction{std::move(text)}});
        if (!result) {
            return false;
        }
        const auto* wrote = std::get_if<bool>(&*result);
        return wrote != nullptr && *wrote;
    }

    /// Build one in-session session creation request from the CLI-owned facts
    /// (pi `createRuntime` re-resolves the CLI options against the target
    /// cwd).
    [[nodiscard]] runtime::AgentSessionCreationRequest make_session_request(
        std::filesystem::path workspace,
        SessionTarget target) const {
        runtime::AgentSessionCreationRequest request;
        request.provide_user_shell = true;
        // pi `projectTrustByCwd`: the CLI override wins; otherwise the boot
        // decision applies to the boot workspace (a session-only trust
        // choice leaves no store entry and must survive in-session
        // replacement).
        request.project_trust_override =
            session_facts_.project_trust_override.has_value()
                ? session_facts_.project_trust_override
                : (resolved_boot_trust_ &&
                          resolved_boot_trust_->first == workspace
                      ? std::optional<bool>{resolved_boot_trust_->second}
                      : std::nullopt);
        request.no_skills = session_facts_.no_skills;
        request.no_prompt_templates = session_facts_.no_prompt_templates;
        request.prompt_template_paths = session_facts_.prompt_template_paths;
        request.skill_paths = session_facts_.skill_paths;
        request.workspace = std::move(workspace);
        request.session_target = std::move(target);
        request.provider = session_facts_.provider;
        request.model = session_facts_.model;
        request.models = session_facts_.models;
        request.api_key = session_facts_.api_key;
        return request;
    }

    // ── Closed action delivery (ADR 0040) ──────────────────────────────────

    /// The error a null host returns for `ReplaceSessionAction`.
    [[nodiscard]] static support::Error session_replacement_unavailable_error() {
        return support::make_error(
            support::ErrorCode::Unknown,
            "Session switching is not available in this host");
    }

    /// Carry one closed application-level action to the composition host with
    /// the generation that admitted it. A delivery from a retired generation
    /// (the session was replaced or the mode closed) is rejected, so a late
    /// action cannot reach the host; `open_browser_hook()` is the one
    /// captured vector and drops those rejections. A null host applies the
    /// TUI-local platform default for the environment operations. Render
    /// state may coalesce, but this path never drops an admitted action.
    [[nodiscard]] support::Expected<TuiActionResultVariant> deliver_action(
        std::size_t captured_generation,
        TuiActionVariant action) {
        if (captured_generation != action_generation_) {
            return std::unexpected(support::make_error(
                support::ErrorCode::Cancelled,
                "Native TUI action rejected",
                "retired session generation"));
        }
        if (action_sink_) {
            // The action is stamped with the generation that admitted it so
            // the host can reject a delivery from a retired generation.
            return action_sink_(action_generation_, std::move(action));
        }
        // Null host: TUI-local platform defaults for environment operations;
        // diagnostics and reporting are silent; replacement is unavailable.
        return std::visit(
            [](auto&& payload) -> support::Expected<TuiActionResultVariant> {
                using T = std::decay_t<decltype(payload)>;
                if constexpr (std::is_same_v<T, OpenBrowserAction>) {
                    open_browser(std::move(payload.url));
                    return TuiActionResultVariant{std::monostate{}};
                } else if constexpr (std::is_same_v<T, WriteClipboardAction>) {
                    return TuiActionResultVariant{
                        write_clipboard_text(payload.text)};
                } else if constexpr (std::is_same_v<T, SuspendProcessAction>) {
                    (void)::kill(0, SIGTSTP);
                    return TuiActionResultVariant{std::monostate{}};
                } else if constexpr (std::is_same_v<T, ReplaceSessionAction>) {
                    return TuiActionResultVariant{
                        support::Expected<coding_agent::CreateAgentSessionResult>{
                            std::unexpected(
                                session_replacement_unavailable_error())}};
                } else {
                    return TuiActionResultVariant{std::monostate{}};
                }
            },
            std::move(action));
    }

    /// Create and return a replacement/boot session through the composition
    /// host (pi `createRuntime`); a null host reports it as unavailable.
    [[nodiscard]] support::Expected<coding_agent::CreateAgentSessionResult>
    request_session_replacement(
        std::size_t captured_generation,
        runtime::AgentSessionCreationRequest request) {
        auto result = deliver_action(
            captured_generation,
            TuiActionVariant{ReplaceSessionAction{std::move(request)}});
        if (!result) {
            return std::unexpected(result.error());
        }
        auto* created =
            std::get_if<support::Expected<coding_agent::CreateAgentSessionResult>>(
                &*result);
        if (created == nullptr) {
            return std::unexpected(session_replacement_unavailable_error());
        }
        return std::move(*created);
    }

    /// pi `AgentSessionRuntime.apply` + `rebindCurrentSession` subset: swap
    /// the live session, resubscribe, and rebuild the presentation from the
    /// new session's snapshot. The host-owned view stays in place; the chat
    /// re-renders like pi's `renderCurrentSessionState`. Retires the action
    /// generation so actions admitted by the previous session are rejected.
    ///
    /// Admission design (ADR 0040, issue #466): replacement first closes the
    /// previous current Session synchronously — `close()` stops prompt
    /// admission and requests cancellation of active work (issue #467
    /// semantics) — then installs the new one. The old Session's admitted
    /// work quiesces asynchronously on the shared Runtime root and is never
    /// awaited by the replacement (pi installs the new Session immediately;
    /// the #466 tests assert a fresh prompt starts while the retired run is
    /// still settling). Safety comes from three mechanisms: the old Session
    /// is retained (in `retired_sessions_`, pruned once its Close finalizes)
    /// so a detached flow cannot dereference a destroyed Session; late
    /// completions are retired by the generation stamp
    /// (`prompt_finished`/`user_bash_finished`); and the old Session's
    /// subscriptions are detached before the new one binds.
    [[nodiscard]] support::ExpectedVoid replace_session(
        std::unique_ptr<AgentSession> next) {
        retire_current_session();
        // The retired Session's in-flight prompt and User Bash can no longer
        // gate or render as the current Session: invalidate their interrupt
        // generations and clear the active-work facts, the working indicator,
        // and any pending-bash progress block (their stale completions are
        // dropped by the generation check).
        if (prompt_active_ || user_bash_active_ || compaction_active_) {
            note_prompt_finished();
            prompt_active_ = false;
            user_bash_active_ = false;
            // A retired Session's compaction keeps no current-Session event
            // subscription, so its end can never clear this fact here.
            compaction_active_ = false;
            if (view_ != nullptr) {
                view_->clear_status_indicator();
                view_->clear_user_bash_progress();
            }
        }
        // Retain the replaced Session while its admitted work is still
        // settling; drop any whose Close already finalised (Closed with no
        // in-flight work), so retention is bounded by real quiescence rather
        // than growing with every replacement (ADR 0040).
        std::erase_if(retired_sessions_, [](const std::unique_ptr<AgentSession>& retired) {
            return !retired->is_open() && !retired->is_busy();
        });
        if (owned_session_) retired_sessions_.push_back(std::move(owned_session_));
        owned_session_ = std::move(next);
        session_ = owned_session_.get();
        model_flows_->update_model_completion();
        auto subscribed = session_ui_->bind(*session_);
        if (!subscribed) {
            return std::unexpected(subscribed.error());
        }
        // The new session's resources replace the loaded-resources block
        // (pi `showLoadedResources` after `renderCurrentSessionState`). The
        // theme re-registration gap (replacement drops the created session's
        // theme documents) is pre-existing and out of scope; the themes
        // section keeps the registered set.
        refresh_loaded_resources();
        rebuild_chat();
        return {};
    }

    /// pi `cycleThinkingLevel` presentation: `Current model does not support
    /// thinking` when the model has no reasoning, else
    /// `Thinking level: <level>`.
    void cycle_thinking_level() {
        auto level = session_->cycle_thinking_level();
        if (!level) {
            show_error(combined_error_text(level.error()));
            return;
        }
        if (!*level) {
            show_status("Current model does not support thinking");
            return;
        }
        show_status("Thinking level: " + **level);
    }

    /// Dynamic prompt-template and skill invocations remain ordinary Agent
    /// Prompt submissions after built-in routing. Built-in names win over
    /// resources with the same spelling, matching the autocomplete collision
    /// rule.
    [[nodiscard]] bool is_dynamic_slash_command(std::string_view command) const {
        if (session_ == nullptr) return false;
        for (const auto& prompt_template : session_->templates()) {
            if (prompt_template.name == command) return true;
        }
        if (!settings_manager_ || !settings_manager_->get_enable_skill_commands() ||
            !command.starts_with("skill:")) {
            return false;
        }
        const auto skill_name = command.substr(std::string_view{"skill:"}.size());
        if (skill_name.empty()) return false;
        for (const auto& skill : session_->skills()) {
            if (skill.name == skill_name) return true;
        }
        return false;
    }

    [[nodiscard]] support::ExpectedVoid execute_immediate_slash_command(
        const SlashCommandInvocation& invocation) {
        switch (invocation.command) {
        case SlashCommandId::Clear:
            if (session_ == nullptr) {
                return std::unexpected(support::make_error(
                    support::ErrorCode::Session,
                    "No active session for /clear"));
            }
            session_flows_->open_new();
            return {};
        case SlashCommandId::Quit:
            if (view_ != nullptr) {
                tui_.invalidate();
                render();
            }
            request_exit();
            return {};
        case SlashCommandId::Copy:
            if (session_ == nullptr) {
                return std::unexpected(support::make_error(
                    support::ErrorCode::Session,
                    "No active session for /copy"));
            }
            handle_copy_last_message();
            return {};
        case SlashCommandId::Session:
            if (session_ == nullptr) {
                return std::unexpected(support::make_error(
                    support::ErrorCode::Session,
                    "No active session for /session"));
            }
            handle_session_command();
            return {};
        case SlashCommandId::Hotkeys:
            open_hotkeys();
            return {};
        case SlashCommandId::Settings:
            show_settings_selector();
            return {};
        case SlashCommandId::Help:
            show_help_command();
            return {};
        case SlashCommandId::Name:
            if (session_ == nullptr) {
                return std::unexpected(support::make_error(
                    support::ErrorCode::Session,
                    "No active session for /name"));
            }
            handle_name_command(invocation.argument);
            return {};
        case SlashCommandId::Model:
        case SlashCommandId::Models:
        case SlashCommandId::Thinking:
        case SlashCommandId::Login:
        case SlashCommandId::Logout:
        case SlashCommandId::Resume:
        case SlashCommandId::Fork:
        case SlashCommandId::Tree:
        case SlashCommandId::Reload:
        case SlashCommandId::Compact:
        case SlashCommandId::Trust:
            return std::unexpected(support::make_error(
                support::ErrorCode::Validation,
                "Command is not an immediate slash command"));
        }
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "Unknown immediate slash command"));
    }

    void dispatch_modal_slash_command(SlashCommandInvocation invocation) {
        switch (invocation.command) {
        case SlashCommandId::Model:
            model_flows_->open_model_selector(std::move(invocation.argument));
            return;
        case SlashCommandId::Models:
            model_flows_->open_scoped_models_selector();
            return;
        case SlashCommandId::Thinking:
            if (!invocation.argument.empty()) {
                if (session_ == nullptr) {
                    show_error("No active session for /thinking");
                    return;
                }
                if (auto applied = session_->set_thinking_level(invocation.argument);
                    !applied) {
                    show_error(combined_error_text(applied.error()));
                    return;
                }
            }
            show_settings_selector();
            return;
        case SlashCommandId::Login:
            auth_flows_->open_login(std::move(invocation.argument));
            return;
        case SlashCommandId::Logout:
            auth_flows_->open_logout();
            return;
        case SlashCommandId::Resume:
            session_flows_->open_resume();
            return;
        case SlashCommandId::Fork:
            session_flows_->open_fork();
            return;
        case SlashCommandId::Tree:
            session_flows_->open_tree();
            return;
        case SlashCommandId::Reload:
            session_flows_->open_reload();
            return;
        case SlashCommandId::Compact:
            session_flows_->open_compact(std::move(invocation.argument));
            return;
        case SlashCommandId::Trust:
            session_flows_->open_trust();
            return;
        case SlashCommandId::Clear:
        case SlashCommandId::Quit:
        case SlashCommandId::Copy:
        case SlashCommandId::Session:
        case SlashCommandId::Hotkeys:
        case SlashCommandId::Settings:
        case SlashCommandId::Help:
        case SlashCommandId::Name:
            show_error(
                "Immediate slash command was routed as a modal command");
            return;
        }
    }

    /// Route built-in slash commands through the deep SlashCommandRouter. The
    /// router executes in-place commands through one small context seam and
    /// returns modal requests as passive values; this method only binds those
    /// values to the existing Native TUI flows.
    [[nodiscard]] bool dispatch_command(std::string_view text) {
        if (!running_ || view_ == nullptr) return false;

        SlashCommandExecutionContext context;
        context.execute_immediate = [this](const SlashCommandInvocation& invocation) {
            return execute_immediate_slash_command(invocation);
        };
        context.allow_unrecognized = [this](std::string_view command) {
            // The router removes only the command prefix. Absolute paths such
            // as clipboard image paths inserted into the editor retain an
            // internal slash in the token and are ordinary prompt text, not
            // command tokens.
            return command.find('/') != std::string_view::npos ||
                is_dynamic_slash_command(command);
        };

        auto routed = slash_command_router_.route(text, context);
        if (std::holds_alternative<SlashCommandPassThrough>(routed)) {
            return false;
        }
        if (auto* error = std::get_if<SlashCommandRouteError>(&routed)) {
            show_error(std::move(error->message));
            return true;
        }
        if (auto* modal = std::get_if<SlashCommandModalResult>(&routed)) {
            dispatch_modal_slash_command(std::move(modal->invocation));
        }
        return true;
    }

    // ── Interrupt admission (pi onEscape precedence, folded from the
    //    deleted InterruptAdmission) ──────────────────────────────────────

    /// The prompt generation captured when an input-thread request is posted.
    [[nodiscard]] std::size_t generation() const noexcept {
        return prompt_generation_.load();
    }

    /// Advances admission state immediately before a new Agent prompt starts.
    void note_prompt_started() noexcept {
        (void)prompt_generation_.fetch_add(1);
        interrupt_requested_generation_.reset();
    }

    /// Invalidates requests captured before the active Agent prompt finished.
    void note_prompt_finished() noexcept {
        (void)prompt_generation_.fetch_add(1);
        interrupt_requested_generation_.reset();
    }

    /// Reports whether the active prompt generation already admitted an abort.
    [[nodiscard]] bool interrupt_requested() const noexcept {
        return interrupt_requested_generation_ == prompt_generation_.load();
    }

    /// Admits a current interrupt request and selects its pi-ordered target:
    /// an active Agent run aborts first, then a running User Bash cancels,
    /// then a pending User Bash submission clears the editor.
    [[nodiscard]] InterruptRoute admit_interrupt(
        std::size_t captured_generation,
        bool pending_bash) noexcept {
        if (captured_generation != prompt_generation_.load()) return InterruptRoute::None;
        if (prompt_active_) {
            if (interrupt_requested_generation_ == prompt_generation_.load()) {
                return InterruptRoute::None;
            }
            interrupt_requested_generation_ = prompt_generation_.load();
            return InterruptRoute::AbortAgentRun;
        }
        if (user_bash_active_) return InterruptRoute::CancelUserBash;
        if (pending_bash) return InterruptRoute::ClearPendingBash;
        return InterruptRoute::None;
    }

    // ── Submission routing (pi setupEditorSubmitHandler, folded from the
    //    deleted InteractionPolicy) ───────────────────────────────────────

    [[nodiscard]] bool dispatch_user_bash(const std::string& text, SubmissionOrigin origin) {
        if (origin != SubmissionOrigin::FocusedEditor) return false;
        if (!detail::AgentSessionInteractiveAccess::has_user_shell(*session_)) return false;
        auto invocation = parse_user_bash_invocation(text);
        if (!invocation) return false;
        if (user_bash_active_) {
            // pi: "A bash command is already running..." and setText(text).
            view_->restore_submitted_text(trim_editor_submission(text));
            view_->append_user_bash_diagnostic(
                "A User Bash command is already in flight");
            tui_.invalidate();
            return true;
        }

        user_bash_active_ = true;
        // The original trimmed submission is what failure restores to the
        // editor (pi setText(text), ADR 0028) — never a re-serialized form.
        auto recall = trim_editor_submission(text);
        const auto self = shared_from_this();
        const std::size_t started_generation = action_generation_;
        auto bridged = ai::detail::make_async_result_on(
            executor_,
            [self,
             invocation = std::move(invocation),
             recall = std::move(recall),
             started_generation]() mutable -> boost::asio::awaitable<support::ExpectedVoid> {
                auto result = co_await detail::AgentSessionInteractiveAccess::run_user_bash(
                    *self->session_,
                    std::move(invocation->command),
                    invocation->exclude_from_context,
                    [self](
                        const runtime::UserBashProgress& progress) -> support::ExpectedVoid {
                        if (self->running_ && self->view_ != nullptr) {
                            self->view_->set_user_bash_progress(progress);
                            self->tui_.invalidate();
                        }
                        return {};
                    });
                self->user_bash_finished(started_generation, std::move(result), recall);
                co_return support::ExpectedVoid{};
            });
        std::move(bridged).start(
            [weak = weak_from_this(), started_generation](support::ExpectedVoid result) noexcept {
                if (result) return;
                if (const auto self = weak.lock()) {
                    if (self->generation_retired(started_generation)) {
                        // A launch failure from a retired Session generation
                        // cannot mutate or render as the current Session
                        // (issue #466).
                        return;
                    }
                    self->user_bash_active_ = false;
                    if (self->view_ != nullptr && self->running_) {
                        self->view_->clear_user_bash_progress();
                        self->view_->append_user_bash_diagnostic(
                            "Native TUI User Bash coroutine failed");
                        self->tui_.invalidate();
                    }
                    if (self->exit_requested_) self->signal_exit();
                }
            });
        return true;
    }

    void request_interrupt(
        std::size_t prompt_generation,
        const EditorInterruptRequest& request) {
        if (!running_ || exit_requested_) return;
        switch (admit_interrupt(prompt_generation, request.pending_bash)) {
        case InterruptRoute::AbortAgentRun:
            // pi restores queued input before aborting the Agent run.
            dequeue_pending_input(false);
            session_->abort();
            return;
        case InterruptRoute::CancelUserBash:
            detail::AgentSessionInteractiveAccess::cancel_user_bash(*session_);
            return;
        case InterruptRoute::ClearPendingBash:
            cleared_editor_revision_ = request.editor_revision;
            if (view_ != nullptr) {
                view_->clear_pending_bash(request);
                tui_.invalidate();
            }
            return;
        case InterruptRoute::None:
            // pi's `onEscape` tail: an idle editor with no text runs the
            // double-escape window (`doubleEscapeAction` default "tree", 500
            // ms); the settings field stays out of the subset, so the tree
            // trigger is hard-coded exactly like pi's default.
            if (trim_editor_submission(request.pending_bash_text).empty()) {
                const auto now = std::chrono::steady_clock::now();
                if (now - last_escape_time_ < std::chrono::milliseconds{500}) {
                    last_escape_time_ = {};
                    session_flows_->open_tree();
                } else {
                    last_escape_time_ = now;
                }
            }
            return;
        }
    }

    void submit(
        std::string text,
        InputSubmission submission,
        PromptOptions options = {},
        SubmissionOrigin origin = SubmissionOrigin::FocusedEditor,
        std::optional<std::size_t> editor_revision = std::nullopt) {
        if (!running_ || view_ == nullptr || text.empty()) return;
        if (origin == SubmissionOrigin::FocusedEditor && editor_revision &&
            cleared_editor_revision_ == editor_revision) {
            return;
        }
        // pi handleFollowUp: while a run is active, Alt+Enter queues the
        // trimmed text directly as follow-up input — the editor chain (User
        // Bash parse, slash dispatch) does not run, and prompt-template
        // expansion happens inside the session admission; when idle,
        // Alt+Enter acts like regular Enter and runs the full editor chain.
        const bool follow_up_while_active =
            submission == InputSubmission::FollowUp && prompt_active_;
        if (!follow_up_while_active) {
            if (dispatch_user_bash(text, origin)) return;
            if (dispatch_command(text)) return;
        }

        if (prompt_active_) {
            if (interrupt_requested()) {
                // The active run was already asked to abort; queued input
                // would die with it, so the text returns to the editor.
                view_->restore_submitted_text(text);
                view_->append_diagnostic("A prompt is already in flight");
                tui_.invalidate();
                return;
            }
            if (submission == InputSubmission::FollowUp) {
                if (auto admitted = session_->follow_up(text); !admitted) {
                    view_->restore_submitted_text(text);
                    view_->append_diagnostic(bounded_redacted_presentation(std::format(
                        "Unable to queue follow-up input: {}",
                        combined_error_text(admitted.error()))));
                }
            } else {
                if (auto admitted = session_->steer(text); !admitted) {
                    view_->restore_submitted_text(text);
                    view_->append_diagnostic(bounded_redacted_presentation(std::format(
                        "Unable to queue steering input: {}",
                        combined_error_text(admitted.error()))));
                }
            }
            session_ui_->sync_pending_input();
            tui_.invalidate();
            return;
        }

        note_prompt_started();
        prompt_active_ = true;
        const auto self = shared_from_this();
        const std::size_t started_generation = action_generation_;
        auto bridged = ai::detail::make_async_result_on(
            executor_,
            [self,
             text = std::move(text),
             options = std::move(options),
             started_generation]() mutable -> boost::asio::awaitable<support::ExpectedVoid> {
                support::ExpectedVoid result;
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
                try {
#endif
                    result = co_await self->session_->prompt(text, std::move(options));
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
                } catch (const std::exception& error) {
                    result = std::unexpected(support::make_error(
                        support::ErrorCode::Unknown,
                        "Native TUI prompt failed",
                        error.what()));
                } catch (...) {
                    result = std::unexpected(support::make_error(
                        support::ErrorCode::Unknown,
                        "Native TUI prompt failed",
                        "unknown exception"));
                }
#endif
                self->prompt_finished(started_generation, std::move(result), text);
                co_return support::ExpectedVoid{};
            });
        std::move(bridged).start(
            [weak = weak_from_this(), started_generation](support::ExpectedVoid result) noexcept {
                if (result) return;
                if (const auto self = weak.lock()) {
                    self->prompt_launch_failed(started_generation);
                }
            });
    }

    void dequeue_pending_input(bool announce) {
        if (!running_ || view_ == nullptr || !session_->is_open()) return;
        const auto snapshot = session_->snapshot();
        std::vector<std::string> restored;
        if (auto collected = queued_editor_texts(snapshot.agent_state.input_queues);
            !collected) {
            view_->append_diagnostic(collected.error().message);
            tui_.invalidate();
            return;
        } else {
            restored = std::move(*collected);
        }
        if (restored.empty()) {
            if (announce) view_->append_frontend_message("No queued messages to restore");
            session_ui_->sync_pending_input();
            tui_.invalidate();
            return;
        }
        if (auto cleared = session_->clear_input_queues(); !cleared) {
            view_->append_diagnostic(bounded_redacted_presentation(std::format(
                "Unable to restore queued input: {}",
                combined_error_text(cleared.error()))));
            session_ui_->sync_pending_input();
            tui_.invalidate();
            return;
        }
        view_->restore_queued_text(restored);
        if (announce) {
            view_->append_frontend_message(std::format(
                "Restored {} queued message{} to editor",
                restored.size(),
                restored.size() == 1 ? "" : "s"));
        }
        session_ui_->sync_pending_input();
        tui_.invalidate();
    }

    void prompt_launch_failed(std::size_t started_generation) {
        if (generation_retired(started_generation)) {
            // A launch failure from a retired Session generation cannot
            // mutate or render as the current Session (issue #466).
            return;
        }
        note_prompt_finished();
        prompt_active_ = false;
        if (view_ != nullptr && running_) {
            view_->append_diagnostic("Native TUI prompt failed");
            tui_.invalidate();
        }
        if (exit_requested_) signal_exit();
    }

    void prompt_finished(
        std::size_t started_generation,
        support::ExpectedVoid result,
        const std::string& submitted_text) {
        if (generation_retired(started_generation)) {
            // A completion from a retired Session generation (the Session was
            // replaced or closed): the old Session's close already retired its
            // interrupt generation and cleared the active-work facts, so this
            // late completion must not render, mutate, or un-gate the current
            // Session (issue #466).
            return;
        }
        note_prompt_finished();
        prompt_active_ = false;
        session_ui_->sync_session_observations();
        if (!result && view_ != nullptr && running_) {
            view_->append_diagnostic(combined_error_text(result.error()));
            view_->restore_submitted_text(submitted_text);
            tui_.invalidate();
        }
        if (exit_requested_ && !user_bash_active_) signal_exit();
    }

    void user_bash_finished(
        std::size_t started_generation,
        support::Expected<runtime::UserBashCompletion> result,
        const std::string& recall) {
        if (generation_retired(started_generation)) {
            // A completion from a retired Session generation (the Session was
            // replaced or closed): the old Session's close already retired its
            // active-work facts, so this late completion must not commit or
            // render as the current Session (issue #466).
            return;
        }
        user_bash_active_ = false;
        if (view_ != nullptr && running_) {
            if (result) {
                view_->commit_user_bash(
                    ai::MessageVariant{std::move(result->message)});
                if (result->diagnostic) {
                    view_->append_user_bash_diagnostic(
                        combined_error_text(*result->diagnostic));
                }
            } else {
                view_->clear_user_bash_progress();
                view_->append_user_bash_diagnostic(combined_error_text(result.error()));
                if (!recall.empty()) {
                    view_->restore_submitted_text(recall);
                }
            }
            tui_.invalidate();
        }
        if (exit_requested_ && !prompt_active_) signal_exit();
    }

    void render() {
        if (!running_) return;
        if (auto rendered = tui_.render(); !rendered) {
            completion_result_ = std::unexpected(startup_error(rendered.error()));
            request_exit();
        }
    }

    void request_exit() {
        if (!running_ || exit_requested_) return;
        exit_requested_ = true;
        if (session_ != nullptr) session_->close();
        if (!prompt_active_ && !user_bash_active_ && !compaction_active_) {
            signal_exit();
        }
    }

    void signal_exit() {
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
        try {
#endif
            (void)exit_wait_.cancel();
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
        } catch (...) {
            if (!completion_result_) {
                completion_result_ = std::unexpected(support::make_error(
                    support::ErrorCode::Unknown,
                    "Native TUI exit notification failed"));
            }
        }
#endif
    }

    AgentSession* session_; // must outlive this interactive run.
    /// Owned replacement session (pi `AgentSessionRuntime.switchSession` /
    /// `newSession` / `fork`): the in-session flows recreate the session
    /// through the config factory and keep the replacement alive here. The
    /// initial session stays borrowed from the host.
    std::unique_ptr<AgentSession> owned_session_;
    /// Replaced sessions whose admitted work is still settling stay owned
    /// here (so a detached flow that borrowed one stays safe across its
    /// final await), and are pruned once their Close finalizes; see
    /// replace_session().
    std::vector<std::unique_ptr<AgentSession>> retired_sessions_;
    cch::tui::Terminal& terminal_; // must outlive this interactive run.
    cch::tui::Tui tui_;
    /// pi's mutable shared KeybindingsManager consumption shape (ADR 0035,
    /// #418): every durable view component observes this slot; `/reload`
    /// replaces the current registry so all consumers see the new bindings
    /// live. Selectors take an ephemeral `get()` snapshot.
    std::shared_ptr<SharedKeybindings> keybindings_;
    /// The agent config directory the keybinding catalog was assembled
    /// under, retained for the `/reload` re-catalog.
    std::filesystem::path agent_config_directory_;
    /// Two-scope settings manager (global scope only; the project scope stays
    /// untrusted in the Native TUI). The theme committer and the
    /// scoped-models selector persist through it. Declared before
    /// `theme_controller_` so the controller's committer reference stays
    /// valid through destruction.
    std::optional<coding_agent::SettingsManager> settings_manager_{std::nullopt};
    /// pi `hideThinkingBlock` / `outputPad` render settings, loaded once at
    /// boot from the merged settings and mutated by `app.thinking.toggle` and
    /// the settings selector. The view's chat renders with these values.
    bool hide_thinking_block_{false};
    std::size_t output_pad_{1};
    /// The extracted model, authentication, and session flows, created once
    /// startup resources (live theme, settings manager, keybindings) exist.
    std::shared_ptr<ModelFlowController> model_flows_;
    std::shared_ptr<AuthFlowController> auth_flows_;
    std::shared_ptr<SessionFlowController> session_flows_;
    /// The session synchronization adapter (#505): owns the Agent Session
    /// event subscriptions, the streaming/retry/compaction event
    /// translation, and the footer data computation. Created with the flow
    /// controllers; `bind()`/`detach()` follow the session lifecycle.
    std::shared_ptr<SessionUiBinding> session_ui_;
    std::optional<ThemeController> theme_controller_;
    std::unique_ptr<AsyncClipboardReader> clipboard_reader_;
    /// One move-only sink carrying closed application-level actions to the
    /// composition host (ADR 0040); null applies TUI-local platform defaults
    /// for the environment operations.
    TuiActionSink action_sink_{nullptr};
    std::optional<std::string> model_fallback_message_;
    /// CLI-owned facts reused for in-session session replacement requests.
    InteractiveSessionFacts session_facts_;
    /// Boot path (pi main.ts `createRuntime` + `resolveProjectTrust`): the
    /// base creation request the interactive host supplies; the boot creates
    /// the session after the boot trust prompt resolves. Empty outside the
    /// boot entry.
    std::optional<runtime::AgentSessionCreationRequest> boot_request_{std::nullopt};
    /// pi `projectTrustByCwd`: the boot-resolved trust decision for the boot
    /// workspace, reused by in-session session creations in the same
    /// workspace (a session-only choice leaves no store entry).
    std::optional<std::pair<std::filesystem::path, bool>>
        resolved_boot_trust_{std::nullopt};
    /// Action-generation counter for the closed action seam (ADR 0040):
    /// every action is delivered with the generation that admitted it, and
    /// `retire_action_generation()` rejects later deliveries from a retired
    /// session generation. Executor-confined; captured by `open_browser_hook`
    /// at hook creation.
    std::size_t action_generation_{0};

    /// Retire the current action generation (session replacement or Close):
    /// later deliveries admitted by the retired generation are rejected.
    void retire_action_generation() noexcept { ++action_generation_; }

    /// Retire the current action generation, detach the current Session's
    /// subscriptions, and request Session close (ADR 0040, issue #466): the
    /// previous current Session's prompt admission stops before a replacement
    /// is installed or the mode closes, while its admitted work quiesces
    /// asynchronously on the shared Runtime root.
    void retire_current_session() noexcept {
        retire_action_generation();
        session_ui_->detach();
        if (session_ != nullptr) {
            session_->close();
        }
    }

    /// Reports whether a prompt/User Bash completion was admitted by a
    /// retired Session generation (the Session was replaced or closed). Such
    /// late completions must not mutate or render as the current Session
    /// (issue #466).
    [[nodiscard]] bool generation_retired(
        std::size_t started_generation) const noexcept {
        return started_generation != action_generation_;
    }

    /// Startup diagnostics stashed by the boot `start()` until the boot
    /// session binds and `initialize_view` renders them (pi
    /// `renderInitialMessages` after the trust prompt).
    InteractiveStartupDiagnostics startup_diagnostics_{};
    /// Theme discovery (parse/collision) diagnostics for the loaded-resources
    /// `[Theme conflicts]` section (pi `getThemes().diagnostics`), stashed at
    /// boot bind and refreshed by `/reload` (#418).
    std::vector<ResourceDiagnostic> loaded_theme_diagnostics_;
    /// Initial prompt stashed by the boot `start()` until the boot session
    /// binds (pi main.ts `initialMessage` submitted after runtime creation).
    std::optional<std::string> initial_prompt_{std::nullopt};
    PromptOptions initial_prompt_options_{};
    boost::asio::any_io_executor executor_;
    boost::asio::steady_timer exit_wait_;
    /// Detached-flow quiescence (ADR 0040): the number of admitted
    /// controller flows still in flight. `finish()` awaits `flows_settled_`
    /// until this reaches zero so terminal restoration never races an
    /// admitted coroutine. Executor-confined; see spawn_flow().
    std::size_t in_flight_flows_{0};
    boost::asio::steady_timer flows_settled_;
    /// SIGCONT/SIGINT registration while suspended (pi's suspend signal
    /// handlers); reset restores the previous handlers on resume.
    std::shared_ptr<boost::asio::signal_set> suspend_signals_;
    InteractiveView* view_{nullptr}; // aliases the child owned by tui_.
    cch::tui::Overlay* active_overlay_{nullptr}; // aliases an overlay owned by tui_.
    SlashCommandRouter slash_command_router_;
    std::atomic<bool> running_{false};
    std::atomic<bool> prompt_active_{false};
    std::atomic<bool> user_bash_active_{false};
    /// A manual /compact flow was admitted and has not returned yet. Exit
    /// defers on it exactly like prompt/User Bash work: the Session Close
    /// requested by request_exit() finalizes only after the compaction
    /// settles, and tearing the loop down earlier would destroy Session
    /// resources the compaction still uses (issue #467, ADR 0040).
    /// Executor-confined like the flows that set and clear it.
    std::atomic<bool> compaction_active_{false};
    /// pi `lastEscapeTime`: the double-escape window base (500 ms, empty
    /// editor, `doubleEscapeAction` default "tree"). Executor-confined.
    std::chrono::steady_clock::time_point last_escape_time_{};
    // Prompt-generation staleness for interrupt requests (pi onEscape
    // routing; the deleted InterruptAdmission's generation). The generation
    // is read from the input thread at post time, so it stays atomic; the
    // admitted-generation marker is executor-confined.
    std::atomic<std::size_t> prompt_generation_{0};
    std::optional<std::size_t> interrupt_requested_generation_;
    // Suppresses a submission already decoded from Bash text cleared by an
    // earlier key-time interrupt decision.
    std::optional<std::size_t> cleared_editor_revision_;
    bool tui_started_{false};
    bool exit_requested_{false};
    bool clipboard_read_active_{false};
    std::optional<support::ExpectedVoid> completion_result_;
};

} // namespace

boost::asio::awaitable<support::ExpectedVoid> run_interactive_mode(
    AgentSession& session,
    cch::tui::Terminal& terminal,
    InteractiveModeConfig config) {
    const auto executor = co_await boost::asio::this_coro::executor;
    auto state = std::make_shared<InteractiveState>(&session, terminal, executor);
    if (auto started = state->start(std::move(config)); !started) {
        co_return std::unexpected(started.error());
    }

    boost::system::error_code wait_error;
    co_await state->exit_wait().async_wait(
        boost::asio::redirect_error(boost::asio::use_awaitable, wait_error));
    co_return co_await state->finish();
}

boost::asio::awaitable<support::ExpectedVoid> run_interactive_mode_boot(
    cch::tui::Terminal& terminal,
    InteractiveModeConfig config) {
    const auto executor = co_await boost::asio::this_coro::executor;
    auto state = std::make_shared<InteractiveState>(nullptr, terminal, executor);
    if (auto started = state->start(std::move(config)); !started) {
        co_return std::unexpected(started.error());
    }
    if (auto booted = co_await state->boot_session(); !booted) {
        // The boot-created session failed before bind; `boot_session`
        // already stopped the TUI (the creation-failure sink printed pi's
        // message).
        co_return std::unexpected(booted.error());
    }

    boost::system::error_code wait_error;
    co_await state->exit_wait().async_wait(
        boost::asio::redirect_error(boost::asio::use_awaitable, wait_error));
    co_return co_await state->finish();
}

} // namespace cch::coding_agent::tui
