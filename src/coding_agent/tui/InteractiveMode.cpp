#include "InteractiveMode.hpp"

#include <cch/agent/AgentContext.hpp>
#include <cch/agent/AgentEvent.hpp>
#include <cch/ai/Auth.hpp>
#include <cch/ai/Content.hpp>
#include "coding_agent/AgentSession.hpp"
#include "ai/AsyncResultBridge.hpp"
#include "ai/ModelThinkingLevel.hpp"
#include <cch/ai/Model.hpp>
#include <cch/coding_agent/Settings.hpp>
#include <cch/coding_agent/ModelResolver.hpp>
#include <cch/coding_agent/ProjectResources.hpp>
#include <cch/coding_agent/ProjectTrust.hpp>
#include <cch/coding_agent/AgentConfigDir.hpp>
#include "harness/WorkspaceFileSystem.hpp"
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
#include "coding_agent/SessionCwd.hpp"
#include "coding_agent/SessionDiscovery.hpp"
#include "coding_agent/SessionPathPolicy.hpp"
#include "coding_agent/prompt/BuiltinSlashCommands.hpp"
#include "coding_agent/runtime/AgentSessionInteractiveAccess.hpp"
#include "coding_agent/runtime/AgentSessionRuntime.hpp"
#include "coding_agent/tui/BashExecutionComponent.hpp"
#include "coding_agent/tui/ChatContainer.hpp"
#include "coding_agent/tui/ClipboardWrite.hpp"
#include "coding_agent/tui/ExternalEditor.hpp"
#include "coding_agent/tui/Footer.hpp"
#include "coding_agent/tui/FooterDataProvider.hpp"
#include "coding_agent/tui/KeybindingsManager.hpp"
#include "coding_agent/tui/StatusIndicator.hpp"
#include "coding_agent/tui/KeybindingHints.hpp"
#include "coding_agent/tui/LoadedResources.hpp"
#include "coding_agent/tui/ReloadBox.hpp"
#include "coding_agent/tui/SharedKeybindings.hpp"
#include "coding_agent/tui/LoginDialog.hpp"
#include "coding_agent/tui/LoginPresentation.hpp"
#include "coding_agent/tui/ModelSearch.hpp"
#include "coding_agent/tui/ModelSelector.hpp"
#include "coding_agent/tui/OAuthSelector.hpp"
#include "coding_agent/tui/OpenBrowser.hpp"
#include "coding_agent/tui/PromptSlot.hpp"
#include "coding_agent/tui/ScopedModelsSelector.hpp"
#include "coding_agent/tui/SessionSelector.hpp"
#include "coding_agent/tui/SettingsSelector.hpp"
#include "coding_agent/tui/StringListSelector.hpp"
#include "coding_agent/tui/ThemeController.hpp"
#include "coding_agent/tui/TreeSelector.hpp"
#include "coding_agent/tui/UserMessageSelector.hpp"
#include "util/UniqueFd.hpp"
#include "util/TerminalText.hpp"

#include <cch/coding_agent/AgentConfigDir.hpp>
#include "harness/compaction/Compaction.hpp"
#include <cch/agent/harness/session/JsonlSessionStore.hpp>

#include <cch/util/Error.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
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

#if defined(__unix__) || defined(__APPLE__)
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace cch::coding_agent::tui {
namespace {

using ActionSink = std::move_only_function<void()>;

struct EditorSubmissionRequest {
    std::string text;
    std::size_t editor_revision{0};
};

struct EditorInterruptRequest {
    std::string pending_bash_text;
    std::size_t editor_revision{0};
    bool pending_bash{false};
};

using InterruptSink = std::move_only_function<void(EditorInterruptRequest)>;
using SubmitSink = std::move_only_function<void(EditorSubmissionRequest)>;

// ── Focused-editor User Bash syntax (ADR 0026) ────────────────────────────
// Folded from the deleted UserBashSyntax module: only a direct focused
// Native TUI editor submission interprets the `!`/`!!` prefixes.

struct UserBashInvocation {
    std::string command;
    bool exclude_from_context{false};
};

/// Trims ASCII whitespace from both ends of one editor submission.
[[nodiscard]] std::string trim_editor_submission(std::string text) {
    const auto first = std::find_if_not(text.begin(), text.end(), [](unsigned char value) {
        return std::isspace(value) != 0;
    });
    const auto last = std::find_if_not(text.rbegin(), text.rend(), [](unsigned char value) {
        return std::isspace(value) != 0;
    }).base();
    if (first >= last) return {};
    return {first, last};
}

/// The argument after an exact slash prefix (pi `text.slice(N).trim()` for
/// the space-delimited `startsWith("/x ")` branch): `"/name  foo"` with
/// `"/name "` yields `"foo"`.
[[nodiscard]] std::string slash_argument(std::string_view text, std::string_view prefix) {
    return trim_editor_submission(std::string{text.substr(prefix.size())});
}

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
[[nodiscard]] bool user_bash_editor_mode(
    std::string text,
    bool user_bash_available) {
    return user_bash_available &&
        trim_editor_submission(std::move(text)).starts_with('!');
}

// ── Submission kinds (folded from the deleted InteractionPolicy) ──────────

enum class InputSubmission { Ordinary, FollowUp };
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

[[nodiscard]] std::string combined_error_text(const util::Error& error) {
    std::string text = error.message;
    if (!error.detail.empty() && error.detail != error.message) {
        text = std::format("{}: {}", text, error.detail);
    }
    if (error.context && !error.context->empty()) {
        text = std::format("{} ({})", text, *error.context);
    }
    return text;
}

[[nodiscard]] std::string editor_text_after_interrupt(
    std::string_view sampled_text,
    std::string_view current_text) {
    std::size_t prefix = 0;
    while (prefix < sampled_text.size() && prefix < current_text.size() &&
        sampled_text[prefix] == current_text[prefix]) {
        ++prefix;
    }

    std::size_t suffix = 0;
    while (suffix < sampled_text.size() - prefix &&
        suffix < current_text.size() - prefix &&
        sampled_text[sampled_text.size() - suffix - 1] ==
            current_text[current_text.size() - suffix - 1]) {
        ++suffix;
    }
    return std::string{current_text.substr(
        prefix,
        current_text.size() - prefix - suffix)};
}

[[nodiscard]] util::Error presentation_error(
    const util::Error& error,
    std::string message) {
    return util::make_error(
        error.code,
        std::move(message),
        bounded_redacted_presentation(combined_error_text(error)));
}

/// pi `theme.ts` `getThinkingBorderColor`: the editor border token for a
/// thinking-level wire name ("off".."max"); unknown levels fall back to
/// `thinkingOff` like pi's default branch.
[[nodiscard]] ThemeToken thinking_border_token_for(std::string_view level) {
    if (level == "minimal") return ThemeToken::ThinkingMinimal;
    if (level == "low") return ThemeToken::ThinkingLow;
    if (level == "medium") return ThemeToken::ThinkingMedium;
    if (level == "high") return ThemeToken::ThinkingHigh;
    if (level == "xhigh") return ThemeToken::ThinkingXhigh;
    if (level == "max") return ThemeToken::ThinkingMax;
    return ThemeToken::ThinkingOff;
}

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

[[nodiscard]] util::Expected<std::filesystem::path> write_clipboard_image(
    std::span<const std::uint8_t> bytes,
    std::string_view extension) {
#if defined(__unix__) || defined(__APPLE__)
    std::error_code temp_error;
    const auto temp_directory = std::filesystem::temp_directory_path(temp_error);
    if (temp_error || temp_directory.empty()) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Process,
            "clipboard temporary directory is unavailable",
            temp_error.message()));
    }

    for (std::size_t attempt = 0; attempt < 16; ++attempt) {
        std::filesystem::path path;
        try {
            path = temp_directory /
                std::format("pi-clipboard-{}{}", clipboard_uuid(), extension);
        } catch (const std::exception& error) {
            return std::unexpected(util::make_error(
                util::ErrorCode::Process,
                "could not generate a clipboard image path",
                error.what()));
        } catch (...) {
            return std::unexpected(util::make_error(
                util::ErrorCode::Process,
                "could not generate a clipboard image path"));
        }
        util::UniqueFd fd(::open(
            path.c_str(),
            O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
            0600));
        if (!fd) {
            if (errno == EEXIST) continue;
            return std::unexpected(util::make_error(
                util::ErrorCode::Process,
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
                return std::unexpected(util::make_error(
                    util::ErrorCode::Process,
                    "could not write clipboard image file",
                    std::error_code(write_error, std::generic_category()).message()));
            }
            written += static_cast<std::size_t>(count);
        }
        if (fd.close() != 0) {
            const auto close_error = errno;
            std::error_code remove_error;
            std::filesystem::remove(path, remove_error);
            return std::unexpected(util::make_error(
                util::ErrorCode::Process,
                "could not finish clipboard image file",
                std::error_code(close_error, std::generic_category()).message()));
        }
        return path;
    }
    return std::unexpected(util::make_error(
        util::ErrorCode::Process,
        "could not allocate a unique clipboard image path"));
#else
    (void)bytes;
    (void)extension;
    return std::unexpected(util::make_error(
        util::ErrorCode::Process,
        "clipboard image files are unavailable on this platform"));
#endif
}

[[nodiscard]] std::optional<std::string> queued_editor_text(
    const ai::MessageVariant& message) {
    const auto* user = std::get_if<ai::UserMessage>(&message);
    if (user == nullptr) {
        return std::nullopt;
    }
    std::string text;
    if (const auto* value = std::get_if<std::string>(&user->content)) {
        text = *value;
    } else {
        const auto& blocks = std::get<std::vector<ai::Content>>(user->content);
        if (std::any_of(
                blocks.begin(),
                blocks.end(),
                [](const auto& block) {
                    return !std::holds_alternative<ai::TextContent>(block);
                })) {
            return std::nullopt;
        }
        text = ai::text_from_content(blocks);
    }
    if (text.empty()) return std::nullopt;
    return text;
}

[[nodiscard]] util::Expected<std::vector<std::string>> queued_editor_texts(
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
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "queued input contains content that the editor cannot restore"));
    }
    return restored;
}

[[nodiscard]] util::Error startup_error(const util::Error& error) {
    return presentation_error(error, "Native TUI startup failed");
}

/// pi's stable login-cancellation error: the cancelled kind travels on the
/// error so the login flows suppress failure UI on kind, not string (#328).
[[nodiscard]] util::Error prompt_cancelled_error() {
    return util::make_error(util::ErrorCode::Cancelled, "Login cancelled");
}

/// pi `formatProjectTrustPrompt` (`core/project-trust.ts`): the boot trust
/// prompt title, with the C++ binary's own identity and the absent
/// packages/extensions clause dropped (the loader subset has no such
/// surfaces).
[[nodiscard]] std::string format_project_trust_prompt(
    const std::filesystem::path& cwd) {
    return std::format(
        "Trust project folder?\n{}\n\nThis allows cch to load .pi settings "
        "and resources.",
        cwd.string());
}

/// pi `reportDiagnostics` subset for the boot trust resolution: convert the
/// resolution's diagnostics to session diagnostics, with the `trust:` code
/// prefix marking their source.
[[nodiscard]] std::vector<SessionDiagnostic> convert_trust_diagnostics(
    const std::vector<ProjectTrustDiagnostic>& diagnostics) {
    std::vector<SessionDiagnostic> converted;
    converted.reserve(diagnostics.size());
    for (const auto& diagnostic : diagnostics) {
        converted.push_back(SessionDiagnostic{
            .severity =
                diagnostic.severity == ProjectTrustDiagnosticSeverity::Error
                ? SessionDiagnostic::Severity::Error
                : SessionDiagnostic::Severity::Warning,
            .code = "trust:" + diagnostic.code,
            .message = diagnostic.message,
            .path = diagnostic.path,
        });
    }
    return converted;
}

/// pi `renderProjectTrustWarningIfNeeded` chat warning text, with the C++
/// binary's own identity and the absent packages clause dropped.
[[nodiscard]] std::string project_trust_warning_text() {
    return "This project is not trusted. Project .pi resources are ignored. "
           "Use /trust to save a trust decision, then restart cch.";
}

/// pi `isUnknownModel`: the unresolved placeholder identity.
[[nodiscard]] bool is_unknown_model(const ai::Model& model) {
    return model.provider == agent::detail::kDefaultModel.provider &&
        model.id == agent::detail::kDefaultModel.id &&
        model.api == agent::detail::kDefaultModel.api;
}

/// One-shot prompt resolution channel for the login flows: producer threads
/// resolve (first wins); the send is posted to the consumer executor so the
/// channel is only ever touched from one thread.
struct AuthPromptSlot : std::enable_shared_from_this<AuthPromptSlot> {
    explicit AuthPromptSlot(boost::asio::any_io_executor executor)
        : executor(std::move(executor)), channel(this->executor, 1) {}

    void resolve(util::Expected<std::string> value) {
        if (resolved.exchange(true)) return;
        const auto self = shared_from_this();
        boost::asio::post(executor, [self, value = std::move(value)]() mutable {
            self->channel.try_send(boost::system::error_code{}, std::move(value));
        });
    }

    boost::asio::any_io_executor executor;
    boost::asio::experimental::concurrent_channel<
        void(boost::system::error_code, util::Expected<std::string>)>
        channel;

private:
    std::atomic<bool> resolved{false};
};

[[nodiscard]] util::Error aggregate_presentation_errors(
    const util::Error& primary,
    const util::Error& restoration,
    std::string message) {
    return util::make_error(
        primary.code,
        std::move(message),
        bounded_redacted_presentation(std::format(
            "primary: {}; restoration: {}",
            combined_error_text(primary),
            combined_error_text(restoration))));
}

/// One immutable model-completion candidate. The `/model` argument
/// completion reads a shared immutable snapshot so the autocomplete request
/// thread never races the executor-confined session state (the snapshot is
/// replaced on the executor whenever the candidate set changes).
struct ModelCompletionItem {
    std::string id;
    std::string provider;
    std::string name;
};
using ModelCompletionSnapshot = std::vector<ModelCompletionItem>;

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

/// pi `showModelsSelector` initial enabled ids: the session scope when one
/// exists, else the configured scope's resolved ids, with `no-match` pattern
/// ids appended as unavailable entries (pi's `currentEnabledIds` assembly).
[[nodiscard]] std::optional<std::vector<std::string>> initial_selector_enabled_ids(
    const std::vector<cch::coding_agent::ScopedModel>& session_scoped_models,
    const std::optional<ModelScopeResolution>& configured_scope) {
    std::optional<std::vector<std::string>> ids;
    if (!session_scoped_models.empty()) {
        ids = std::vector<std::string>{};
        for (const auto& entry : session_scoped_models) {
            ids->push_back(entry.model.provider + "/" + entry.model.id);
        }
    } else if (configured_scope) {
        ids = std::vector<std::string>{};
        for (const auto& scoped : configured_scope->scoped_models) {
            ids->push_back(scoped.model.provider + "/" + scoped.model.id);
        }
    }
    for (const auto& diagnostic : configured_scope ? configured_scope->diagnostics
                                                   : std::vector<ModelScopeDiagnostic>{}) {
        if (diagnostic.code != "no-match") continue;
        if (!ids) ids = std::vector<std::string>{};
        if (std::find(ids->begin(), ids->end(), diagnostic.pattern) == ids->end()) {
            ids->push_back(diagnostic.pattern);
        }
    }
    return ids;
}

/// Resolve an executable on PATH (pi's `ensureTool`); nullopt when absent so
/// `@`/`#` completion degrades gracefully to empty file suggestions.
[[nodiscard]] std::optional<std::filesystem::path> find_executable_on_path(std::string_view name) {
#if defined(__unix__) || defined(__APPLE__)
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
#endif
    return std::nullopt;
}

/// Executor-bound one-shot debounce timer for the editor's autocomplete
/// requests. All timer state is confined to the executor thread via posts;
/// the shared state keeps the wait handler safe after the editor is gone.
/// pi `CountdownTimer`: the retry indicator's one-second countdown over the
/// backoff delay. Each tick delivers the remaining seconds; the timer stops
/// itself at zero. Executor-driven like the autocomplete debounce so the
/// countdown is observable on the interactive executor.
class RetryCountdown final : public std::enable_shared_from_this<RetryCountdown> {
public:
    RetryCountdown(
        boost::asio::any_io_executor executor,
        int attempt,
        int max_attempts)
        : executor_(std::move(executor)),
          attempt_(attempt),
          max_attempts_(max_attempts) {}

    /// Begin the countdown from `seconds` (pi's `remainingSeconds`), with
    /// the first tick one second later.
    void start(int seconds, std::move_only_function<void(int)> on_tick) {
        const auto self = shared_from_this();
        boost::asio::post(executor_, [self, seconds, on_tick = std::move(on_tick)]() mutable {
            self->remaining_ = std::max(0, seconds);
            self->on_tick_ = std::move(on_tick);
            self->schedule_tick();
        });
    }

    [[nodiscard]] int attempt() const { return attempt_; }
    [[nodiscard]] int max_attempts() const { return max_attempts_; }

private:
    void schedule_tick() {
        const auto self = shared_from_this();
        timer_.expires_after(std::chrono::seconds(1));
        timer_.async_wait([self](const boost::system::error_code& error) {
            if (error || self->remaining_ <= 0) return;
            self->remaining_--;
            if (self->on_tick_) self->on_tick_(self->remaining_);
            if (self->remaining_ > 0) self->schedule_tick();
        });
    }

    boost::asio::any_io_executor executor_;
    boost::asio::steady_timer timer_{executor_};
    std::move_only_function<void(int)> on_tick_;
    int attempt_{0};
    int max_attempts_{0};
    int remaining_{0};
};

class AsioAutocompleteDebounceTimer final : public cch::tui::AutocompleteDebounceTimer {
public:
    explicit AsioAutocompleteDebounceTimer(boost::asio::any_io_executor executor)
        : state_(std::make_shared<State>(std::move(executor))) {}

    void start(std::chrono::milliseconds delay, std::move_only_function<void()> on_fire) override {
        const auto state = state_;
        boost::asio::post(state->executor, [state, delay, on_fire = std::move(on_fire)]() mutable {
            const auto generation = ++state->generation;
            state->active_callback = std::move(on_fire);
            state->timer.expires_after(delay);
            state->timer.async_wait([state, generation](boost::system::error_code error) {
                if (generation != state->generation) return;
                auto callback = std::move(state->active_callback);
                state->active_callback = nullptr;
                if (!error && callback) callback();
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
        explicit State(boost::asio::any_io_executor executor) : timer(std::move(executor)) {}
        boost::asio::any_io_executor executor;
        boost::asio::steady_timer timer;
        std::size_t generation{0};
        std::move_only_function<void()> active_callback;
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

    [[nodiscard]] util::Expected<cch::tui::RenderResult> render(std::size_t width) override {
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
        try {
            on_cancel_();
        } catch (const std::exception& error) {
            callback_error_ = util::make_error(
                util::ErrorCode::Unknown,
                "Hotkey help cancellation failed",
                error.what());
        } catch (...) {
            callback_error_ = util::make_error(
                util::ErrorCode::Unknown,
                "Hotkey help cancellation failed");
        }
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
    std::optional<util::Error> callback_error_;
    bool focused_{false};
};

/// The pi main-screen composition: header (keybinding hints only, no logo),
/// chat, pending-messages, status, editor, and footer containers, stacked in
/// pi's order with the chat absorbing the flexible space. The status and
/// footer containers are placeholders whose content lands with the
/// footer/status ticket (P15); the editor, chat, pending display, header
/// hints, and the interrupt binding follow pi's interactive-mode routing.
class InteractiveView final
    : public cch::tui::Component,
      public cch::tui::InputHandler,
      public cch::tui::Focusable,
      public cch::tui::ViewportAware {
public:
    InteractiveView(
        std::shared_ptr<SharedKeybindings> keybindings,
        ActionSink on_invalidate,
        SubmitSink on_submit,
        SubmitSink on_follow_up,
        ActionSink on_clipboard_paste,
        ActionSink on_dequeue,
        InterruptSink on_interrupt,
        ActionSink on_exit,
        ActionSink on_cycle_model_forward,
        ActionSink on_cycle_model_backward,
        ActionSink on_select_model,
        ActionSink on_cycle_thinking,
        ActionSink on_toggle_thinking,
        ActionSink on_resume_session,
        ActionSink on_fork_session,
        ActionSink on_new_session,
        ActionSink on_copy_last_message,
        ActionSink on_open_tree_selector,
        ActionSink on_suspend,
        ActionSink on_external_editor,
        std::move_only_function<FooterData()> footer_data_source,
        bool hide_thinking_block,
        std::size_t output_pad,
        bool user_bash_available,
        std::unique_ptr<cch::tui::AutocompleteProvider> autocomplete_provider,
        std::unique_ptr<cch::tui::AutocompleteDebounceTimer> autocomplete_debounce_timer,
        cch::tui::EditorRenderRequestSink autocomplete_render_request,
        const LiveTheme& theme)
        : keybindings_(std::move(keybindings)),
          on_invalidate_(std::move(on_invalidate)),
          on_submit_(std::move(on_submit)),
          on_follow_up_(std::move(on_follow_up)),
          on_clipboard_paste_(std::move(on_clipboard_paste)),
          on_dequeue_(std::move(on_dequeue)),
          on_interrupt_(std::move(on_interrupt)),
          on_exit_(std::move(on_exit)),
          on_cycle_model_forward_(std::move(on_cycle_model_forward)),
          on_cycle_model_backward_(std::move(on_cycle_model_backward)),
          on_select_model_(std::move(on_select_model)),
          on_cycle_thinking_(std::move(on_cycle_thinking)),
          on_toggle_thinking_(std::move(on_toggle_thinking)),
          on_resume_session_(std::move(on_resume_session)),
          on_fork_session_(std::move(on_fork_session)),
          on_new_session_(std::move(on_new_session)),
          on_copy_last_message_(std::move(on_copy_last_message)),
          on_open_tree_selector_(std::move(on_open_tree_selector)),
          on_suspend_(std::move(on_suspend)),
          on_external_editor_(std::move(on_external_editor)),
          footer_data_source_(std::move(footer_data_source)),
          user_bash_available_(user_bash_available),
          header_(theme, keybindings_, user_bash_available, on_clipboard_paste_ != nullptr),
          resources_(theme),
          chat_(theme, keybindings_),
          footer_(theme),
          theme_(&theme),
          editor_(
              cch::tui::EditorOptions{
                  .keybindings = keybindings_->get(),
                  .autocomplete_debounce_timer = std::move(autocomplete_debounce_timer),
                  .autocomplete_render_request = std::move(autocomplete_render_request),
              },
              [this](std::string text) {
                  // Editor submission clears before invoking its submit sink;
                  // keep that notification on the sampled text's revision.
                  if (!text.empty()) ++editor_revision_;
                  invoke_action(on_invalidate_, "Native TUI invalidation callback failed");
              },
              [this](std::string text) {
                  invoke_submit(EditorSubmissionRequest{
                      .text = std::move(text),
                      .editor_revision = editor_revision_,
                  });
              }) {
        chat_.set_hide_thinking_block(hide_thinking_block);
        chat_.set_output_pad(output_pad);
        editor_.set_autocomplete_provider(std::move(autocomplete_provider));
    }
    InteractiveView(InteractiveView&&) = delete;
    InteractiveView& operator=(InteractiveView&&) = delete;
    ~InteractiveView() override = default;
    InteractiveView(const InteractiveView&) = delete;
    InteractiveView& operator=(const InteractiveView&) = delete;

    void initialize(const AgentSessionSnapshot& snapshot) {
        std::lock_guard lock(mutex_);
        chat_.initialize(snapshot);
    }

    void apply_render_settings(bool hide_thinking_block, std::size_t output_pad) {
        std::lock_guard lock(mutex_);
        chat_.set_hide_thinking_block(hide_thinking_block);
        chat_.set_output_pad(output_pad);
    }

    /// Replace the editor's autocomplete provider (pi
    /// `setupAutocompleteProvider` after a settings change).
    void set_autocomplete_provider(
        std::unique_ptr<cch::tui::AutocompleteProvider> provider) {
        std::lock_guard lock(mutex_);
        editor_.set_autocomplete_provider(std::move(provider));
    }

    /// `/reload` keybinding re-catalog (pi `KeybindingsManager.reload()` →
    /// shared-manager mutation, ADR 0035): swap the shared slot every durable
    /// component observes and rebind the editor's snapshot. Serialized under
    /// the view mutex so concurrent render/input on the terminal thread never
    /// observes a torn registry.
    void set_keybindings(
        std::shared_ptr<const cch::tui::KeybindingRegistry> registry) {
        std::lock_guard lock(mutex_);
        keybindings_->replace(registry);
        editor_.set_keybindings(std::move(registry));
    }

    void apply_event(const agent::AgentLifecycleEvent& event) {
        std::lock_guard lock(mutex_);
        chat_.apply_event(event);
    }

    void append_committed_message(ai::MessageVariant message) {
        std::lock_guard lock(mutex_);
        chat_.append_committed_message(std::move(message));
    }

    void clear_transcript() {
        std::lock_guard lock(mutex_);
        chat_.clear();
    }

    void append_frontend_message(std::string text) {
        std::lock_guard lock(mutex_);
        chat_.append_frontend_message(std::move(text));
    }

    void append_diagnostic(std::string text) {
        std::lock_guard lock(mutex_);
        chat_.append_diagnostic(std::move(text));
    }

    void append_warning(std::string text) {
        std::lock_guard lock(mutex_);
        chat_.append_warning(std::move(text));
    }

    void append_trust_warning(std::string text) {
        std::lock_guard lock(mutex_);
        chat_.append_trust_warning(std::move(text));
    }

    void append_status_message(std::string text) {
        std::lock_guard lock(mutex_);
        chat_.append_status_message(std::move(text));
    }

    /// pi `showStatusIndicator` surface over the status container: the
    /// active indicator replaces the previous one (Working/Compaction on
    /// accent, Retry on warning) and animates through the TUI Loader.
    void show_status_working(std::string message = "Working...") {
        std::lock_guard lock(mutex_);
        // pi `setWorkingVisible`: an already-active Working indicator is
        // kept (per-message re-shows must not restart the loader).
        if (status_indicator_ != nullptr &&
            status_indicator_->kind() == StatusIndicator::Kind::Working) {
            return;
        }
        replace_status_indicator(
            StatusIndicator::Kind::Working,
            working_status_message(std::move(message)));
    }

    void show_status_compaction(std::string_view reason) {
        std::lock_guard lock(mutex_);
        replace_status_indicator(
            StatusIndicator::Kind::Compaction,
            compaction_status_message(keybindings_->registry(), reason));
    }

    void show_status_retry(int attempt, int max_attempts, int seconds) {
        std::lock_guard lock(mutex_);
        replace_status_indicator(
            StatusIndicator::Kind::Retry,
            retry_status_message(keybindings_->registry(), attempt, max_attempts, seconds));
    }

    /// pi `RetryStatusIndicator` countdown tick: rewrite the retry message
    /// without replacing the loader.
    void set_status_retry_message(int attempt, int max_attempts, int seconds) {
        std::lock_guard lock(mutex_);
        if (status_indicator_ == nullptr ||
            status_indicator_->kind() != StatusIndicator::Kind::Retry) {
            return;
        }
        status_indicator_->set_message(
            retry_status_message(keybindings_->registry(), attempt, max_attempts, seconds));
    }

    /// pi `showLoadedResources`: replace the loaded-resources block (the
    /// startup container between the header and the chat). A no-resources
    /// data renders zero lines.
    void set_loaded_resources_data(LoadedResources::Data data) {
        std::lock_guard lock(mutex_);
        resources_.set_data(std::move(data));
    }

    /// pi `clearStatusIndicator`: back to the two-row idle status.
    void clear_status_indicator() {
        std::lock_guard lock(mutex_);
        status_indicator_.reset();
    }

    /// Replace the active status indicator (pi `showStatusIndicator`
    /// disposes the previous one first); the loader's render requests flow
    /// through the view's invalidate sink.
    void replace_status_indicator(StatusIndicator::Kind kind, std::string message) {
        status_indicator_ = std::make_unique<StatusIndicator>(
            kind,
            *theme_,
            [this] {
                if (!on_invalidate_) return;
                try {
                    on_invalidate_();
                } catch (...) {
                    record_callback_error(
                        "Native TUI status indicator render request failed");
                }
            },
            std::move(message));
    }

    /// The login presentation's editor slot (pi's `editorContainer` swap):
    /// while a replacement is set it renders and receives input in place of
    /// the editor, exactly like pi's focused dialog/selector.
    void set_editor_replacement(std::shared_ptr<cch::tui::Component> component) {
        std::lock_guard lock(mutex_);
        editor_replacement_ = std::move(component);
        if (editor_replacement_) {
            if (auto* focusable = dynamic_cast<cch::tui::Focusable*>(editor_replacement_.get())) {
                focusable->set_focused(editor_.focused());
            }
        }
    }

    void restore_editor() {
        std::lock_guard lock(mutex_);
        editor_replacement_.reset();
    }

    void append_user_bash_diagnostic(std::string text) {
        std::lock_guard lock(mutex_);
        chat_.append_user_bash_diagnostic(std::move(text));
    }

    void restore_submitted_text(const std::string& text) {
        std::lock_guard lock(mutex_);
        restore_editor_text({text});
    }

    void clear_pending_bash(const EditorInterruptRequest& request) {
        std::lock_guard lock(mutex_);
        if (editor_revision_ == request.editor_revision) {
            editor_.set_text({});
            return;
        }
        editor_.set_text(editor_text_after_interrupt(
            request.pending_bash_text,
            editor_.expanded_text()));
    }

    void insert_editor_text(std::string text) {
        std::lock_guard lock(mutex_);
        editor_.insert_text_at_cursor(std::move(text));
    }

    /// The raw editor text (pi `editor.getText()` — the tree navigation
    /// pre-fill and fork flows check emptiness before replacing it).
    [[nodiscard]] std::string editor_text() const {
        std::lock_guard lock(mutex_);
        return editor_.text();
    }

    /// The expanded editor text (pi `editor.getExpandedText()` — the
    /// external-editor flow sends the expanded content like pi).
    [[nodiscard]] std::string editor_expanded_text() const {
        std::lock_guard lock(mutex_);
        return editor_.expanded_text();
    }

    /// Replace the whole editor content (pi `editor.setText` — the fork
    /// flow's `selectedText` pre-fill).
    void set_editor_text(std::string text) {
        std::lock_guard lock(mutex_);
        editor_.set_text(std::move(text));
    }

    void restore_queued_text(const std::vector<std::string>& messages) {
        std::lock_guard lock(mutex_);
        restore_editor_text(messages);
    }

    void set_pending_input(const agent::AgentInputQueues& queues) {
        std::lock_guard lock(mutex_);
        pending_steering_.clear();
        pending_follow_up_.clear();
        for (const auto& message : queues.steering.messages) {
            pending_steering_.push_back(
                queued_editor_text(message).value_or("[unsupported queued input]"));
        }
        for (const auto& message : queues.follow_up.messages) {
            pending_follow_up_.push_back(
                queued_editor_text(message).value_or("[unsupported queued input]"));
        }
    }

    void set_user_bash_progress(runtime::UserBashProgress progress) {
        std::lock_guard lock(mutex_);
        if (!pending_bash_) {
            pending_bash_ = std::make_unique<BashExecutionComponent>(
                *theme_,
                keybindings_,
                progress.command,
                progress.exclude_from_context);
            pending_bash_->start_loader([this] {
                if (!on_invalidate_) return;
                try {
                    on_invalidate_();
                } catch (...) {
                }
            });
            last_bash_output_size_ = 0;
            bash_outcome_set_ = false;
        }
        if (progress.output.size() > last_bash_output_size_) {
            pending_bash_->append_output(progress.output.substr(last_bash_output_size_));
        }
        last_bash_output_size_ = progress.output.size();
        if (progress.awaiting_commitment && !bash_outcome_set_) {
            bash_outcome_set_ = true;
            pending_bash_->set_complete(
                progress.exit_code,
                progress.cancelled,
                progress.truncated,
                progress.full_output_path);
        }
        pending_bash_->set_expanded(chat_.tools_expanded());
    }

    void clear_user_bash_progress() {
        std::lock_guard lock(mutex_);
        pending_bash_.reset();
        last_bash_output_size_ = 0;
        bash_outcome_set_ = false;
    }

    /// Replaces the pending block with its committed transcript entry in one
    /// step, so the clear-pending-before-append ordering cannot drift apart
    /// at call sites.
    void commit_user_bash(ai::MessageVariant message) {
        std::lock_guard lock(mutex_);
        pending_bash_.reset();
        last_bash_output_size_ = 0;
        bash_outcome_set_ = false;
        chat_.append_committed_message(std::move(message));
    }

    [[nodiscard]] util::Expected<cch::tui::RenderResult> render(std::size_t width) override {
        std::lock_guard lock(mutex_);
        if (callback_error_) return std::unexpected(*callback_error_);

        // Header (keybinding hints), the loaded-resources block, pending-
        // messages, status, and footer render first so the editor and
        // autocomplete capacities account for their fixed rows (pi's dock
        // below the chat).
        std::vector<std::string> header_lines;
        if (auto header = header_.render(width); !header) {
            return std::unexpected(header.error());
        } else {
            header_lines = std::move(header->lines);
        }
        std::vector<std::string> resources_lines;
        if (auto resources = resources_.render(width); !resources) {
            return std::unexpected(resources.error());
        } else {
            resources_lines = std::move(resources->lines);
        }
        // Status and footer containers are part of the composition: the
        // status container holds the active Working/Compaction/Retry
        // indicator (two rows: one spacer + the loader line, pi's Loader) or
        // the two-row IdleStatus; the footer renders pi's two-line layout
        // from the live footer data source.
        std::vector<std::string> status_lines;
        std::vector<std::string> footer_lines;
        if (footer_data_source_) {
            current_footer_data_ = footer_data_source_();
        }
        if (status_indicator_) {
            if (auto rendered = status_indicator_->render(width); !rendered) {
                return std::unexpected(rendered.error());
            } else {
                status_lines = std::move(rendered->lines);
            }
        } else {
            // pi's IdleStatus: the two-row empty status container.
            auto idle = idle_status_.render(width);
            if (!idle) return std::unexpected(idle.error());
            status_lines = std::move(idle->lines);
        }
        footer_.set_data(current_footer_data_);
        if (auto rendered = footer_.render(width); !rendered) {
            return std::unexpected(rendered.error());
        } else {
            footer_lines = std::move(rendered->lines);
        }

        std::vector<std::string> pending_lines;
        pending_lines.reserve(pending_steering_.size() + pending_follow_up_.size() + 3);
        if (pending_bash_) {
            // One Bash block while pending; it becomes an ordinary chat entry
            // through the same component after commitment.
            pending_bash_->set_expanded(chat_.tools_expanded());
            auto block = pending_bash_->render(width);
            if (!block) return std::unexpected(block.error());
            pending_lines.insert(
                pending_lines.end(),
                std::make_move_iterator(block->lines.begin()),
                std::make_move_iterator(block->lines.end()));
        }
        for (const auto& message : pending_steering_) {
            cch::tui::TruncatedText item{"Steering: " + message};
            if (auto rendered = item.render(width); !rendered) {
                return std::unexpected(rendered.error());
            } else if (!rendered->lines.empty()) {
                pending_lines.push_back(std::move(rendered->lines.front()));
            }
        }
        for (const auto& message : pending_follow_up_) {
            cch::tui::TruncatedText item{"Follow-up: " + message};
            if (auto rendered = item.render(width); !rendered) {
                return std::unexpected(rendered.error());
            } else if (!rendered->lines.empty()) {
                pending_lines.push_back(std::move(rendered->lines.front()));
            }
        }
        if (!pending_lines.empty()) {
            const auto hint = keybindings_->registry().key_text("app.message.dequeue");
            cch::tui::TruncatedText item{std::format(
                "↳ {} to edit all queued messages",
                hint.empty() ? "Unbound" : format_key_text(hint, true))};
            if (auto rendered = item.render(width); !rendered) {
                return std::unexpected(rendered.error());
            } else if (!rendered->lines.empty()) {
                pending_lines.push_back(std::move(rendered->lines.front()));
            }
        }
        // The dock (pending + status + editor + footer) and a minimum chat
        // slice stay visible on every terminal; the loaded-resources block
        // yields the space it needs. pi's transcript scroll keeps the dock
        // fixed while the loaded-resources content scrolls above the chat, so
        // a huge startup diagnostic (e.g. an invalid `--theme` document's
        // full missing-color-token list, #425) must not push the footer and
        // editor off a small screen and freeze the boot view.
        constexpr std::size_t kMinDockEditorRows = 3;
        constexpr std::size_t kMinChatRows = 3;
        const auto dock_budget = pending_lines.size() + status_lines.size() +
            kMinDockEditorRows + footer_lines.size();
        const auto top_budget = available_rows_ > dock_budget + kMinChatRows
            ? available_rows_ - dock_budget - kMinChatRows
            : 0;
        const auto resources_budget =
            top_budget > header_lines.size() ? top_budget - header_lines.size() : 0;
        if (resources_lines.size() > resources_budget) {
            resources_lines.resize(resources_budget);
        }
        const auto fixed_rows =
            header_lines.size() + resources_lines.size() + pending_lines.size() +
            status_lines.size() + footer_lines.size();

        std::vector<std::string> editor_lines;
        std::vector<std::string> autocomplete_lines;
        if (editor_replacement_) {
            // pi's editorContainer swap: the login dialog/selector renders in
            // the editor slot with no autocomplete rows.
            if (auto replaced = editor_replacement_->render(width); !replaced) {
                return std::unexpected(replaced.error());
            } else {
                editor_lines = std::move(replaced->lines);
            }
        } else {
        editor_.set_available_height(available_rows_ > fixed_rows
            ? available_rows_ - fixed_rows
            : 1);
        // The editor enters Bash mode as soon as the trimmed input begins
        // with `!` (where User Bash dispatch is available). The border color
        // follows the same transition: bash mode uses the bashMode token,
        // otherwise the thinking-level token (pi `updateEditorBorderColor`
        // `getBashModeBorderColor` / `getThinkingBorderColor`).
        const auto thinking_border_token = thinking_border_token_for(
            current_footer_data_.thinking_level);
        cch::tui::EditorTheme editor_theme;
        if (unsubmitted_bash_mode()) {
            editor_theme.text = theme_->foreground_hook(ThemeToken::BashMode);
            editor_theme.border = theme_->foreground_hook(ThemeToken::BashMode);
        } else {
            editor_theme.text = theme_->editor_theme().text;
            editor_theme.border = theme_->foreground_hook(thinking_border_token);
        }
        editor_.set_theme(std::move(editor_theme));
        if (auto editor = editor_.render(width); !editor) {
            return std::unexpected(editor.error());
        } else {
            editor_lines = std::move(editor->lines);
        }

        const auto autocomplete = editor_.autocomplete_items();
        const auto selected = editor_.autocomplete_selected_index();
        constexpr std::size_t kMaxAutocompleteRows = 5;
        const auto autocomplete_capacity = available_rows_ > fixed_rows + editor_lines.size()
            ? std::min(kMaxAutocompleteRows, available_rows_ - fixed_rows - editor_lines.size())
            : 0;
        const auto first_autocomplete = selected < autocomplete_capacity || autocomplete_capacity == 0
            ? 0
            : selected - autocomplete_capacity + 1;
        const auto autocomplete_count = std::min(
            autocomplete_capacity,
            autocomplete.size() - std::min(first_autocomplete, autocomplete.size()));
        autocomplete_lines.reserve(autocomplete_count);
        for (std::size_t offset = 0; offset < autocomplete_count; ++offset) {
            const auto index = first_autocomplete + offset;
            std::string text = index == selected ? "> /" : "  /";
            text += autocomplete[index].label;
            if (!autocomplete[index].description.empty()) {
                text += " — " + autocomplete[index].description;
            }
            cch::tui::TruncatedText item{std::move(text)};
            if (auto rendered = item.render(width); !rendered) {
                return std::unexpected(rendered.error());
            } else if (!rendered->lines.empty()) {
                autocomplete_lines.push_back(std::move(rendered->lines.front()));
            }
        }
        }
        // The full conversation passes through to the terminal's native
        // scrollback (pi TuiMainScreen): only the dock (pending/status/
        // editor/autocomplete/footer) stays fixed on screen, and earlier chat
        // lines scroll away into the terminal's scroll history as the
        // conversation grows past one screen. Inline images keep their
        // absolute buffer rows so they scroll with the content that produced
        // them (fork-B image-follows-content).
        cch::tui::RenderResult chat_result;
        if (auto rendered = chat_.render(width); !rendered) {
            return std::unexpected(rendered.error());
        } else {
            chat_result = std::move(*rendered);
        }

        cch::tui::RenderResult transcript_result;
        transcript_result.lines = std::move(header_lines);
        transcript_result.lines.insert(
            transcript_result.lines.end(),
            std::make_move_iterator(resources_lines.begin()),
            std::make_move_iterator(resources_lines.end()));
        for (auto& image : chat_result.images) {
            image.region.row += transcript_result.lines.size();
            transcript_result.images.push_back(std::move(image));
        }
        transcript_result.lines.insert(
            transcript_result.lines.end(),
            std::make_move_iterator(chat_result.lines.begin()),
            std::make_move_iterator(chat_result.lines.end()));
        transcript_result.lines.insert(
            transcript_result.lines.end(),
            std::make_move_iterator(pending_lines.begin()),
            std::make_move_iterator(pending_lines.end()));
        transcript_result.lines.insert(
            transcript_result.lines.end(),
            std::make_move_iterator(status_lines.begin()),
            std::make_move_iterator(status_lines.end()));
        editor_row_offset_ = transcript_result.lines.size();
        transcript_result.lines.insert(
            transcript_result.lines.end(),
            std::make_move_iterator(editor_lines.begin()),
            std::make_move_iterator(editor_lines.end()));
        transcript_result.lines.insert(
            transcript_result.lines.end(),
            std::make_move_iterator(autocomplete_lines.begin()),
            std::make_move_iterator(autocomplete_lines.end()));
        transcript_result.lines.insert(
            transcript_result.lines.end(),
            std::make_move_iterator(footer_lines.begin()),
            std::make_move_iterator(footer_lines.end()));
        return transcript_result;
    }

    void invalidate() override {
        std::lock_guard lock(mutex_);
        editor_.invalidate();
        if (editor_replacement_) editor_replacement_->invalidate();
    }

    void handle_input(const cch::tui::InputEventVariant& input) override {
        std::lock_guard lock(mutex_);
        if (editor_replacement_) {
            // pi routes every key to the focused dialog/selector; app-level
            // bindings resume when the editor is restored. pi's TUI
            // re-renders after each input event, so the view invalidates.
            if (auto* handler = dynamic_cast<cch::tui::InputHandler*>(editor_replacement_.get())) {
                handler->handle_input(input);
                invoke_action(on_invalidate_, "Native TUI invalidation callback failed");
            }
            return;
        }
        const auto* key = std::get_if<cch::tui::KeyEvent>(&input);
        if (key != nullptr && key->type != cch::tui::KeyEventType::Release) {
            // One registry reference for the whole dispatch cascade (the
            // shared slot, ADR 0035); `replace` is serialized under this
            // view mutex.
            const auto& keys = keybindings_->registry();
            if (keys.matches(*key, "app.exit") && editor_.expanded_text().empty()) {
                invoke_action(on_exit_, "Native TUI exit callback failed");
                return;
            }
            const auto editor_cancels_interrupt =
                editor_.autocomplete_open() &&
                keys.matches(*key, "tui.select.cancel");
            if (keys.matches(*key, "app.interrupt") &&
                !editor_cancels_interrupt) {
                // Autocomplete cancellation stays in the view. Interrupt
                // precedence is pi's onEscape chain and owns every later
                // decision; it receives Bash mode as it existed at key-press
                // time.
                invoke_interrupt(EditorInterruptRequest{
                    .pending_bash_text = editor_.expanded_text(),
                    .editor_revision = editor_revision_,
                    .pending_bash = unsubmitted_bash_mode(),
                });
                return;
            }
            if (keys.matches(*key, "app.message.followUp")) {
                invoke_follow_up();
                return;
            }
            if (keys.matches(*key, "app.clipboard.pasteImage")) {
                invoke_action(
                    on_clipboard_paste_,
                    "Native TUI clipboard callback failed");
                return;
            }
            if (keys.matches(*key, "app.message.dequeue")) {
                invoke_action(on_dequeue_, "Native TUI dequeue callback failed");
                return;
            }
            if (keys.matches(*key, "app.clear")) {
                editor_.set_text({});
                return;
            }
            if (keys.matches(*key, "app.suspend")) {
                invoke_action(
                    on_suspend_,
                    "Native TUI suspend callback failed");
                return;
            }
            if (keys.matches(*key, "app.editor.external")) {
                invoke_action(
                    on_external_editor_,
                    "Native TUI external editor callback failed");
                return;
            }
            if (keys.matches(*key, "app.tools.expand")) {
                chat_.toggle_tool_output();
                header_.set_expanded(chat_.tools_expanded());
                resources_.set_expanded(chat_.tools_expanded());
                invoke_action(on_invalidate_, "Native TUI invalidation callback failed");
                return;
            }
            if (keys.matches(*key, "app.thinking.toggle")) {
                invoke_action(
                    on_toggle_thinking_,
                    "Native TUI thinking toggle callback failed");
                return;
            }
            // pi's main-editor `app.model.*` / `app.thinking.cycle` bindings:
            // the cycle actions and the model selector post to the executor
            // like every session-touching action.
            if (keys.matches(*key, "app.model.cycleForward")) {
                invoke_action(on_cycle_model_forward_, "Native TUI model cycle callback failed");
                return;
            }
            if (keys.matches(*key, "app.model.cycleBackward")) {
                invoke_action(on_cycle_model_backward_, "Native TUI model cycle callback failed");
                return;
            }
            if (keys.matches(*key, "app.model.select")) {
                invoke_action(on_select_model_, "Native TUI model selector callback failed");
                return;
            }
            if (keys.matches(*key, "app.thinking.cycle")) {
                invoke_action(on_cycle_thinking_, "Native TUI thinking cycle callback failed");
                return;
            }
            // pi `app.session.*`: recognized-but-unbound actions (defaultKeys
            // []) — a user-assigned keybinding triggers the flow; the
            // selector-scoped `app.session.*` bindings are matched inside the
            // SessionSelectorComponent itself.
            if (keys.matches(*key, "app.session.resume")) {
                invoke_action(on_resume_session_, "Native TUI session resume callback failed");
                return;
            }
            if (keys.matches(*key, "app.session.fork")) {
                invoke_action(on_fork_session_, "Native TUI session fork callback failed");
                return;
            }
            if (keys.matches(*key, "app.session.new")) {
                invoke_action(on_new_session_, "Native TUI new-session callback failed");
                return;
            }
            if (keys.matches(*key, "app.session.tree")) {
                invoke_action(on_open_tree_selector_, "Native TUI tree selector callback failed");
                return;
            }
            // pi's main-editor `app.message.copy` binding (P14: the tree
            // selector matches the same action through the shared registry).
            if (keys.matches(*key, "app.message.copy")) {
                invoke_action(on_copy_last_message_, "Native TUI copy callback failed");
                return;
            }
        }
        const auto autocomplete_was_open = editor_.autocomplete_open();
        const auto previous_selection = editor_.autocomplete_selected_index();
        editor_.handle_input(input);
        if (autocomplete_was_open != editor_.autocomplete_open() ||
            previous_selection != editor_.autocomplete_selected_index()) {
            invoke_action(on_invalidate_, "Native TUI invalidation callback failed");
        }
    }

    [[nodiscard]] bool accepts_key_releases() const override {
        return false;
    }

    void set_focused(bool focused) override {
        std::lock_guard lock(mutex_);
        if (editor_replacement_) {
            if (auto* focusable = dynamic_cast<cch::tui::Focusable*>(editor_replacement_.get())) {
                focusable->set_focused(focused);
                return;
            }
        }
        editor_.set_focused(focused);
    }

    [[nodiscard]] bool focused() const override {
        std::lock_guard lock(mutex_);
        if (editor_replacement_) {
            if (auto* focusable = dynamic_cast<cch::tui::Focusable*>(editor_replacement_.get())) {
                return focusable->focused();
            }
        }
        return editor_.focused();
    }

    [[nodiscard]] std::optional<cch::tui::CursorPosition> cursor_location() const override {
        std::lock_guard lock(mutex_);
        std::optional<cch::tui::CursorPosition> cursor;
        if (editor_replacement_) {
            if (auto* focusable = dynamic_cast<cch::tui::Focusable*>(editor_replacement_.get())) {
                cursor = focusable->cursor_location();
            }
        } else {
            cursor = editor_.cursor_location();
        }
        if (cursor) {
            // Return the true buffer-relative row; Tui::render() clamps it to
            // the visible viewport when positioning the hardware cursor (pi
            // positionHardwareCursor tracks the scroll rather than scrolling
            // the terminal).
            cursor->row += editor_row_offset_;
        }
        return cursor;
    }

    void set_available_height(std::size_t rows) override {
        std::lock_guard lock(mutex_);
        available_rows_ = std::max<std::size_t>(1, rows);
    }

private:
    void record_callback_error(
        std::string message,
        std::string detail = {}) {
        callback_error_ = util::make_error(
            util::ErrorCode::Unknown,
            std::move(message),
            std::move(detail));
    }

    void invoke_action(ActionSink& action, std::string_view failure_message) {
        if (!action) return;
        try {
            action();
        } catch (const std::exception& error) {
            record_callback_error(std::string(failure_message), error.what());
        } catch (...) {
            record_callback_error(std::string(failure_message));
        }
    }

    void invoke_submit(EditorSubmissionRequest request) {
        invoke_submission(
            on_submit_,
            std::move(request),
            "Native TUI submit callback failed");
    }

    void invoke_follow_up() {
        auto text = trim_editor_submission(editor_.expanded_text());
        if (text.empty()) return;
        // pi handleFollowUp: the accepted text enters editor history before
        // the editor clears and the follow-up admission is posted, matching
        // the Enter path where the editor records the submission itself.
        editor_.add_to_history(text);
        editor_.set_text({});
        invoke_submission(
            on_follow_up_,
            EditorSubmissionRequest{
                .text = std::move(text),
                .editor_revision = editor_revision_,
            },
            "Native TUI follow-up callback failed");
    }

    void invoke_submission(
        SubmitSink& sink,
        EditorSubmissionRequest request,
        std::string_view failure_message) {
        if (!sink) return;
        try {
            sink(std::move(request));
        } catch (const std::exception& error) {
            record_callback_error(std::string(failure_message), error.what());
        } catch (...) {
            record_callback_error(std::string(failure_message));
        }
    }

    void invoke_interrupt(EditorInterruptRequest request) {
        if (!on_interrupt_) return;
        try {
            on_interrupt_(std::move(request));
        } catch (const std::exception& error) {
            record_callback_error(
                "Native TUI interrupt callback failed",
                error.what());
        } catch (...) {
            record_callback_error("Native TUI interrupt callback failed");
        }
    }

    void restore_editor_text(const std::vector<std::string>& messages) {
        std::string restored;
        for (const auto& message : messages) {
            if (message.empty()) continue;
            if (!restored.empty()) restored += "\n\n";
            restored += message;
        }
        auto current = editor_.expanded_text();
        if (!current.empty()) {
            if (!restored.empty()) restored += "\n\n";
            restored += current;
        }
        editor_.set_text(std::move(restored));
    }

    [[nodiscard]] bool unsubmitted_bash_mode() const {
        return user_bash_editor_mode(editor_.expanded_text(), user_bash_available_);
    }

    /// The shared keybinding slot (ADR 0035, #418): `set_keybindings`
    /// replaces it under the view mutex so the header/chat/execution
    /// components and the editor all observe the new registry live.
    std::shared_ptr<SharedKeybindings> keybindings_;
    ActionSink on_invalidate_;
    SubmitSink on_submit_;
    SubmitSink on_follow_up_;
    ActionSink on_clipboard_paste_;
    ActionSink on_dequeue_;
    InterruptSink on_interrupt_;
    ActionSink on_exit_;
    ActionSink on_cycle_model_forward_;
    ActionSink on_cycle_model_backward_;
    ActionSink on_select_model_;
    ActionSink on_cycle_thinking_;
    ActionSink on_toggle_thinking_;
    ActionSink on_resume_session_;
    ActionSink on_fork_session_;
    ActionSink on_new_session_;
    ActionSink on_copy_last_message_;
    ActionSink on_open_tree_selector_;
    ActionSink on_suspend_;
    ActionSink on_external_editor_;
    /// Footer data source (pi footer.ts render inputs); polled on every
    /// render by the view's footer container. Installed by the state; the
    /// source must not re-enter the view.
    std::move_only_function<FooterData()> footer_data_source_;
    bool user_bash_available_{false};
    std::optional<util::Error> callback_error_;
    mutable std::mutex mutex_;
    // pi's main-screen containers.
    KeybindingHints header_;
    /// The loaded-resources startup block (pi's `loadedResourcesContainer`,
    /// between the header and the chat; #418).
    LoadedResources resources_;
    ChatContainer chat_;
    Footer footer_;
    const LiveTheme* theme_; // must outlive the view: controller-owned live theme.
    cch::tui::Editor editor_;
    std::size_t editor_revision_{0};
    // The status container's active indicator (pi's statusContainer child);
    // null renders the two-row IdleStatus.
    std::unique_ptr<StatusIndicator> status_indicator_;
    IdleStatus idle_status_;
    // The latest footer data (polled from the data source during render);
    // also feeds the editor border thinking token.
    FooterData current_footer_data_;
    std::vector<std::string> pending_steering_;
    std::vector<std::string> pending_follow_up_;
    // The live pending User Bash block (pi's pendingMessagesContainer).
    std::unique_ptr<BashExecutionComponent> pending_bash_;
    // The login presentation's editor-slot occupant (pi's editorContainer
    // swap); null renders the ordinary editor.
    std::shared_ptr<cch::tui::Component> editor_replacement_;
    std::size_t last_bash_output_size_{0};
    bool bash_outcome_set_{false};
    std::size_t available_rows_{24};
    std::size_t editor_row_offset_{0};
};

class InteractiveState final : public std::enable_shared_from_this<InteractiveState> {
public:
    InteractiveState(
        AgentSession* session,
        cch::tui::Terminal& terminal,
        boost::asio::any_io_executor executor)
        : session_(session),
          terminal_(terminal),
          tui_(terminal),
          executor_(std::move(executor)),
          exit_wait_(executor_) {
        exit_wait_.expires_at(std::chrono::steady_clock::time_point::max());
    }
    InteractiveState(InteractiveState&&) = delete;
    InteractiveState& operator=(InteractiveState&&) = delete;
    ~InteractiveState() = default;
    InteractiveState(const InteractiveState&) = delete;
    InteractiveState& operator=(const InteractiveState&) = delete;

    [[nodiscard]] util::ExpectedVoid start(InteractiveModeConfig config) {
        clipboard_reader_ = std::move(config.clipboard_reader);
        model_fallback_message_ = std::move(config.model_fallback_message);
        action_sink_ = std::move(config.action_sink);
        session_facts_ = std::move(config.session_facts);
        boot_request_ = std::move(config.boot_request);
        const bool booting = boot_request_.has_value();
        if (!booting) {
            update_model_completion();
        }

        InteractiveStartupDiagnostics diagnostics;
        if (auto loaded = load_startup_resources(config); !loaded) {
            return fail_start(loaded.error());
        } else {
            diagnostics = std::move(*loaded);
        }

        const auto weak = weak_from_this();
        auto view = make_interactive_view(weak);
        view_ = view.get();
        if (auto attached = tui_.add_child(std::move(view)); !attached) {
            return fail_start(attached.error());
        }
        if (!booting) {
            if (auto subscribed = subscribe_to_session(weak); !subscribed) {
                return fail_start(subscribed.error());
            }
        }

        tui_.set_render_request_sink([weak] {
            if (const auto self = weak.lock()) self->post_render();
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
    [[nodiscard]] boost::asio::awaitable<util::ExpectedVoid> boot_session() {
        // pi main.ts `autoTrustOnReloadCwd`: no `--approve`-style override
        // AND the boot workspace had no trust-requiring resources → the
        // implicit-trust save may fire on a later `/reload` (when the
        // workspace gains resources and the session is trusted).
        if (!boot_request_->project_trust_override.has_value()) {
            if (auto fs = harness::WorkspaceFileSystem::create(boot_request_->workspace); fs) {
                auto detection = detect_project_resources(
                    *fs, coding_agent::home_directory() / ".agents" / "skills");
                if (!needs_project_trust_resolution(detection)) {
                    auto_trust_on_reload_cwd_ = boot_request_->workspace;
                }
            }
        }
        // 1. Resolve boot trust (pi resolveProjectTrusted): override → no
        //    trust-requiring resources → saved decision → default
        //    always/never → ask prompt (the generic string-list selector
        //    overlay; G2 record).
        auto decision = co_await resolve_boot_trust();
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
            const util::Error failure = created.error();
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
        update_model_completion();
        rebuild_autocomplete_provider();
        const auto weak = weak_from_this();
        if (auto subscribed = subscribe_to_session(weak); !subscribed) {
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
        co_return util::ExpectedVoid{};
    }

    /// pi `resolveProjectTrusted` + `selectProjectTrustOption` for the boot:
    /// override → no trust-requiring resources → saved store entry → default
    /// always/never → ask prompt (the generic string-list selector overlay on
    /// the main TUI, G2 record). Returns the decided trust for the boot
    /// session.
    [[nodiscard]] boost::asio::awaitable<bool> resolve_boot_trust() {
        // pi `resolveProjectTrusted`: an override decides first, before any
        // resource detection or store walk.
        if (boot_request_->project_trust_override.has_value()) {
            co_return *boot_request_->project_trust_override;
        }
        const auto workspace = boot_request_->workspace;
        auto fs = harness::WorkspaceFileSystem::create(workspace);
        ProjectResourceDetectionResult detection;
        if (fs) {
            detection = detect_project_resources(
                *fs, coding_agent::home_directory() / ".agents" / "skills");
        }
        const bool trust_needed = needs_project_trust_resolution(detection);
        ProjectTrustStore store{coding_agent::trust_store_file_path()};
        const auto default_trust = settings_manager_
            ? settings_manager_->default_project_trust().value_or(DefaultProjectTrust::Ask)
            : DefaultProjectTrust::Ask;
        auto resolved = resolve_project_trust(
            workspace,
            trust_needed,
            store,
            default_trust,
            boot_request_->project_trust_override);
        // Surface the boot resolution's diagnostics (e.g. an unreadable
        // trust store) like pi's `reportDiagnostics`; SessionFactory skips
        // re-resolution because the boot passes the decided override.
        if (!resolved.diagnostics.empty()) {
            (void)deliver_action(
                action_generation_,
                TuiActionVariant{ReportBootDiagnosticsAction{
                    convert_trust_diagnostics(std::move(resolved.diagnostics))}});
        }
        if (resolved.source != ProjectTrustSource::DefaultAskNoUi) {
            co_return resolved.decision == ProjectTrustDecision::Trusted;
        }
        // ask + UI: the generic string-list selector overlay (pi
        // `selectProjectTrustOption`; `includeSessionOnly: true`).
        auto option = co_await show_boot_trust_prompt(workspace);
        if (!option) {
            co_return false;
        }
        if (!option->updates.empty()) {
            if (auto saved = store.setMany(option->updates); !saved) {
                show_error(combined_error_text(saved.error()));
            }
        }
        co_return option->trusted;
    }

    /// pi `selectProjectTrustOption`: the boot trust prompt as the generic
    /// string-list selector in the editor slot (the main-TUI overlay, G2
    /// record). Returns the chosen option, or nullopt on cancel (untrusted);
    /// the caller persists the option's updates.
    [[nodiscard]] boost::asio::awaitable<std::optional<ProjectTrustOption>>
    show_boot_trust_prompt(const std::filesystem::path& workspace) {
        auto options = get_project_trust_options(
            workspace, /*include_session_only*/ true);
        auto slot = std::make_shared<AuthPromptSlot>(executor_);
        std::vector<std::string> labels;
        labels.reserve(options.size());
        for (const auto& option : options) {
            labels.push_back(option.label);
        }
        const auto weak = weak_from_this();
        auto selector = std::make_shared<StringListSelector>(
            theme_controller_->live_theme(),
            keybindings_->get(),
            format_project_trust_prompt(workspace),
            std::move(labels),
            [weak, slot](std::string label) {
                if (const auto self = weak.lock()) {
                    self->post_from_view(
                        [slot, label = std::move(label)](InteractiveState& self) mutable {
                            self.restore_editor_slot();
                            slot->resolve(std::move(label));
                        });
                }
            },
            [weak, slot] {
                if (const auto self = weak.lock()) {
                    self->post_from_view([slot](InteractiveState& self) {
                        self.restore_editor_slot();
                        slot->resolve(std::unexpected(prompt_cancelled_error()));
                    });
                }
            });
        place_editor_replacement(std::move(selector));
        auto selected = co_await slot->channel.async_receive(
            boost::asio::use_awaitable);
        if (!selected) {
            co_return std::nullopt;
        }
        for (auto& option : options) {
            if (option.label == *selected) {
                co_return option;
            }
        }
        co_return std::nullopt;
    }

    [[nodiscard]] boost::asio::steady_timer& exit_wait() {
        return exit_wait_;
    }

    [[nodiscard]] util::ExpectedVoid finish() {
        running_ = false;
        // Retire the action generation so late deliveries from captured
        // hooks are rejected after Close (ADR 0040).
        retire_action_generation();
        subscription_.reset();
        if (session_ != nullptr) {
            session_->close();
        }
        const auto stopped = tui_.stop();
        tui_started_ = false;
        if (!completion_result_) completion_result_.emplace();
        if (!*completion_result_) {
            if (!stopped) {
                return std::unexpected(aggregate_presentation_errors(
                    completion_result_->error(),
                    stopped.error(),
                    "Native TUI failed and terminal restoration failed"));
            }
            return std::unexpected(completion_result_->error());
        }
        if (!stopped) {
            return std::unexpected(presentation_error(
                stopped.error(),
                "Native TUI terminal restoration failed"));
        }
        return {};
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
            // pi's main-editor `app.suspend` (Ctrl+Z; unavailable on
            // Windows/Other per the catalog's platform gate) and
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
    [[nodiscard]] util::ExpectedVoid re_catalog_keybindings() {
        const auto actions = assemble_keybinding_actions();
        std::vector<std::string_view> action_views;
        action_views.reserve(actions.size());
        for (const auto& action : actions) {
            action_views.push_back(action);
        }
        if (auto definitions = app_keybinding_definitions(action_views, keybinding_platform_);
            !definitions) {
            return std::unexpected(definitions.error());
        } else {
            KeybindingsManagerRequest request;
            request.agent_config_directory = agent_config_directory_;
            request.application_definitions = std::move(*definitions);
            request.platform = keybinding_platform_;
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

    [[nodiscard]] util::Expected<InteractiveStartupDiagnostics> load_startup_resources(
        const InteractiveModeConfig& config) {
        InteractiveStartupDiagnostics diagnostics;
        keybinding_platform_ = config.platform;
        agent_config_directory_ = config.agent_config_directory;
        const auto actions = assemble_keybinding_actions();
        std::vector<std::string_view> action_views;
        action_views.reserve(actions.size());
        for (const auto& action : actions) {
            action_views.push_back(action);
        }
        if (auto definitions = app_keybinding_definitions(action_views, config.platform); !definitions) {
            return std::unexpected(definitions.error());
        } else {
            KeybindingsManagerRequest request;
            request.agent_config_directory = config.agent_config_directory;
            request.application_definitions = std::move(*definitions);
            request.platform = config.platform;
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
                return std::unexpected(util::make_error(
                    util::ErrorCode::JsonParse,
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
        return std::make_unique<InteractiveView>(
            keybindings_,
            [weak] {
                if (const auto self = weak.lock()) self->post_invalidate();
            },
            [weak](EditorSubmissionRequest request) {
                if (const auto self = weak.lock()) {
                    self->post_submit(
                        std::move(request),
                        InputSubmission::Ordinary);
                }
            },
            [weak](EditorSubmissionRequest request) {
                if (const auto self = weak.lock()) {
                    self->post_submit(
                        std::move(request),
                        InputSubmission::FollowUp);
                }
            },
            [weak] {
                if (const auto self = weak.lock()) self->post_clipboard_paste();
            },
            [weak] {
                if (const auto self = weak.lock()) self->post_dequeue();
            },
            [weak](EditorInterruptRequest request) {
                if (const auto self = weak.lock()) {
                    self->post_interrupt(std::move(request));
                }
            },
            [weak] {
                if (const auto self = weak.lock()) self->post_exit();
            },
            [weak] {
                if (const auto self = weak.lock()) self->post_cycle_model("forward");
            },
            [weak] {
                if (const auto self = weak.lock()) self->post_cycle_model("backward");
            },
            [weak] {
                if (const auto self = weak.lock()) self->post_open_model_selector();
            },
            [weak] {
                if (const auto self = weak.lock()) self->post_cycle_thinking();
            },
            [weak] {
                if (const auto self = weak.lock()) self->post_toggle_thinking();
            },
            [weak] {
                if (const auto self = weak.lock()) self->post_resume_session();
            },
            [weak] {
                if (const auto self = weak.lock()) self->post_fork_session();
            },
            [weak] {
                if (const auto self = weak.lock()) self->post_new_session();
            },
            [weak] {
                if (const auto self = weak.lock()) self->post_copy_last_message();
            },
            [weak] {
                if (const auto self = weak.lock()) self->post_open_tree_selector();
            },
            [weak] {
                if (const auto self = weak.lock()) self->post_suspend();
            },
            [weak] {
                if (const auto self = weak.lock()) self->post_external_editor();
            },
            [weak] {
                if (const auto self = weak.lock(); self && self->running_) {
                    return self->compute_footer_data();
                }
                return FooterData{};
            },
            hide_thinking_block_,
            output_pad_,
            view_user_shell_available(),
            build_autocomplete_provider(),
            std::make_unique<AsioAutocompleteDebounceTimer>(executor_),
            [weak] {
                if (const auto self = weak.lock()) self->post_invalidate();
            },
            theme_controller_->live_theme());
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
                model_completion_,
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

    [[nodiscard]] util::ExpectedVoid subscribe_to_session(
        std::weak_ptr<InteractiveState> weak) {
        if (auto subscribed = session_->subscribe(
                [weak](const agent::AgentLifecycleEvent& event) -> util::ExpectedVoid {
                    if (const auto self = weak.lock()) self->on_event(event);
                    return {};
                });
            !subscribed) {
            return std::unexpected(subscribed.error());
        } else {
            subscription_.emplace(std::move(*subscribed));
        }
        if (auto subscribed = session_->subscribe_session(
                [weak](const AgentSessionEvent& event) -> util::ExpectedVoid {
                    if (const auto self = weak.lock()) self->on_session_event(event);
                    return {};
                });
            !subscribed) {
            return std::unexpected(subscribed.error());
        } else {
            session_event_subscription_.emplace(std::move(*subscribed));
        }
        return {};
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
        for (const auto& diagnostic : snapshot.agent_state.diagnostics) {
            auto text = combined_error_text(diagnostic);
            view_->append_diagnostic(text);
            displayed_agent_diagnostics_.push_back(std::move(text));
        }
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

    [[nodiscard]] util::ExpectedVoid fail_start(const util::Error& error) {
        running_ = false;
        if (session_ != nullptr) {
            session_->close();
        }
        util::ExpectedVoid stopped;
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

    void post_submit(EditorSubmissionRequest request, InputSubmission submission) {
        const auto weak = weak_from_this();
        boost::asio::post(
            executor_,
            [weak, request = std::move(request), submission]() mutable {
                if (const auto self = weak.lock()) {
                    self->submit(
                        std::move(request.text),
                        submission,
                        {},
                        SubmissionOrigin::FocusedEditor,
                        request.editor_revision);
                }
            });
    }

    void post_clipboard_paste() {
        const auto weak = weak_from_this();
        boost::asio::post(executor_, [weak] {
            const auto self = weak.lock();
            if (!self || !self->running_ || self->clipboard_reader_ == nullptr ||
                self->clipboard_read_active_) {
                return;
            }
            self->clipboard_read_active_ = true;
            boost::asio::co_spawn(
                self->executor_,
                self->paste_from_clipboard(),
                [weak](std::exception_ptr exception, util::ExpectedVoid result) {
                    const auto state = weak.lock();
                    if (!state) return;
                    state->clipboard_read_active_ = false;
                    std::optional<util::Error> ignored_failure;
                    if (exception) {
                        try {
                            std::rethrow_exception(exception);
                        } catch (const std::exception& error) {
                            ignored_failure = util::make_error(
                                util::ErrorCode::Unknown,
                                "clipboard paste failed",
                                error.what());
                        } catch (...) {
                            ignored_failure = util::make_error(
                                util::ErrorCode::Unknown,
                                "clipboard paste failed");
                        }
                    } else if (!result) {
                        ignored_failure = std::move(result.error());
                    }
                    // Baseline clipboard failures are intentionally silent.
                    (void)ignored_failure;
                });
        });
    }

    [[nodiscard]] boost::asio::awaitable<util::ExpectedVoid> paste_from_clipboard() {
        try {
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
                        co_return util::ExpectedVoid{};
                    }
                }
            }
        } catch (const std::exception& error) {
            const auto ignored = util::make_error(
                util::ErrorCode::Unknown,
                "clipboard image read failed",
                error.what());
            (void)ignored;
        } catch (...) {
            const auto ignored = util::make_error(
                util::ErrorCode::Unknown,
                "clipboard image read failed");
            (void)ignored;
        }

        try {
            auto text = co_await clipboard_reader_->read_text();
            if (text && *text && !(*text)->empty() && running_ && view_ != nullptr) {
                view_->insert_editor_text(std::move(**text));
                tui_.invalidate();
            }
        } catch (const std::exception& error) {
            const auto ignored = util::make_error(
                util::ErrorCode::Unknown,
                "clipboard text read failed",
                error.what());
            (void)ignored;
        } catch (...) {
            const auto ignored = util::make_error(
                util::ErrorCode::Unknown,
                "clipboard text read failed");
            (void)ignored;
        }
        co_return util::ExpectedVoid{};
    }

    void post_dequeue() {
        const auto weak = weak_from_this();
        boost::asio::post(executor_, [weak] {
            if (const auto self = weak.lock()) self->dequeue_pending_input(true);
        });
    }

    void post_interrupt(EditorInterruptRequest request) {
        const auto weak = weak_from_this();
        const auto prompt_generation = generation();
        boost::asio::post(
            executor_,
            [weak, prompt_generation, request = std::move(request)] {
                if (const auto self = weak.lock()) {
                    self->request_interrupt(prompt_generation, request);
                }
            });
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
    void append_command_error(const util::Error& error) {
        if (view_ == nullptr) return;
        view_->append_diagnostic(combined_error_text(error));
        tui_.invalidate();
    }

    [[nodiscard]] util::ExpectedVoid attach_overlay(
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

    void close_overlay() {
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
        if (!running_ || view_ == nullptr || !session_->is_open() ||
            !theme_controller_ || !keybindings_ || !settings_manager_) {
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
                self->post_from_view([](InteractiveState& state) { state.restore_editor_slot(); });
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
        place_editor_replacement(std::move(selector));
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
        if (auto attached = attach_overlay(std::move(overlay)); !attached) {
            append_command_error(attached.error());
        }
    }

    // ── Login presentation (pi interactive-mode.ts login flows, #328) ────

    /// Post one login-presentation action to the executor from a view-thread
    /// selector/dialog sink.
    void post_from_view(std::move_only_function<void(InteractiveState&)> action) {
        const auto weak = weak_from_this();
        boost::asio::post(executor_, [weak, action = std::move(action)]() mutable {
            if (const auto self = weak.lock(); self && self->running_) action(*self);
        });
    }

    /// Spawn one detached executor flow; a frame failure becomes a chat
    /// diagnostic (the user-bash precedent; the login flows use this too).
    void spawn_flow(
        std::move_only_function<boost::asio::awaitable<void>()> start,
        std::string failure_label) {
        const auto weak = weak_from_this();
        // The coroutine lambda's frame may reference its closure (the
        // `start` move_only_function), so keep the closure alive until the
        // spawned coroutine reaches its terminal completion (same pattern as
        // make_model_stream / ADR 0040 §Behavior mechanisms).
        auto start_owner =
            std::make_shared<std::move_only_function<boost::asio::awaitable<void>()>>(
                std::move(start));
        boost::asio::co_spawn(
            executor_,
            (*start_owner)(),
            [weak, start_owner, failure_label = std::move(failure_label)](
                std::exception_ptr exception) {
                if (!exception) return;
                if (const auto self = weak.lock();
                    self && self->running_ && self->view_ != nullptr) {
                    self->view_->append_diagnostic(std::move(failure_label));
                    self->tui_.invalidate();
                }
            });
    }

    void place_editor_replacement(std::shared_ptr<cch::tui::Component> component) {
        if (view_ == nullptr) return;
        view_->set_editor_replacement(std::move(component));
        tui_.invalidate();
    }

    void restore_editor_slot() {
        if (view_ == nullptr) return;
        view_->restore_editor();
        tui_.invalidate();
    }

    /// pi `showStatus`: one dim status line in the chat.
    void show_status(std::string text) {
        if (view_ == nullptr) return;
        view_->append_status_message(std::move(text));
        tui_.invalidate();
    }

    /// pi `showError`: one `Error: <text>` chat line.
    void show_error(std::string text) {
        if (view_ == nullptr) return;
        view_->append_diagnostic(std::move(text));
        tui_.invalidate();
    }

    // ── Model selector / cycling (pi interactive-mode.ts, #407) ───────────

    /// Rebuild the shared immutable `/model` completion snapshot on the
    /// executor (pi's candidate set: session scoped models when a scope
    /// exists, else the availability snapshot). The snapshot is only ever
    /// replaced here, so autocomplete readers see one consistent list.
    void update_model_completion() {
        auto runtime = session_->model_runtime();
        if (!runtime) {
            model_completion_ = std::make_shared<const ModelCompletionSnapshot>();
            return;
        }
        auto snapshot = std::make_shared<ModelCompletionSnapshot>();
        const auto scoped = session_->scoped_models();
        if (!scoped.empty()) {
            snapshot->reserve(scoped.size());
            for (const auto& entry : scoped) {
                snapshot->push_back(ModelCompletionItem{
                    .id = entry.model.id,
                    .provider = entry.model.provider,
                    .name = entry.model.name,
                });
            }
        } else {
            for (const auto& model : runtime->get_available_snapshot()) {
                snapshot->push_back(ModelCompletionItem{
                    .id = model.id,
                    .provider = model.provider,
                    .name = model.name,
                });
            }
        }
        model_completion_ = std::move(snapshot);
    }

    /// Post one model/thinking action to the executor from the input thread.
    void post_cycle_model(std::string direction) {
        const auto weak = weak_from_this();
        boost::asio::post(executor_, [weak, direction = std::move(direction)]() mutable {
            if (const auto self = weak.lock(); self && self->running_) {
                self->spawn_flow(
                    [self, direction = std::move(direction)]() mutable
                    -> boost::asio::awaitable<void> {
                        co_await self->cycle_model(std::move(direction));
                    },
                    "Native TUI model cycle failed");
            }
        });
    }

    void post_cycle_thinking() {
        const auto weak = weak_from_this();
        boost::asio::post(executor_, [weak] {
            if (const auto self = weak.lock(); self && self->running_) {
                self->cycle_thinking_level();
            }
        });
    }

    void post_toggle_thinking() {
        const auto weak = weak_from_this();
        boost::asio::post(executor_, [weak] {
            if (const auto self = weak.lock(); self && self->running_) {
                self->toggle_thinking_block_visibility();
            }
        });
    }

    void post_open_model_selector() {
        const auto weak = weak_from_this();
        boost::asio::post(executor_, [weak] {
            if (const auto self = weak.lock(); self && self->running_) {
                self->show_model_selector(std::nullopt);
            }
        });
    }

    void post_open_model_selector(std::string search_term) {
        const auto weak = weak_from_this();
        boost::asio::post(executor_, [weak, search_term = std::move(search_term)]() mutable {
            if (const auto self = weak.lock(); self && self->running_) {
                self->spawn_flow(
                    [self, search_term = std::move(search_term)]() mutable
                    -> boost::asio::awaitable<void> {
                        co_await self->handle_model_command(std::move(search_term));
                    },
                    "Native TUI model command failed");
            }
        });
    }

    void post_open_scoped_models_selector() {
        const auto weak = weak_from_this();
        boost::asio::post(executor_, [weak] {
            if (const auto self = weak.lock(); self && self->running_) {
                self->spawn_flow(
                    [self]() -> boost::asio::awaitable<void> {
                        co_await self->show_scoped_models_selector();
                    },
                    "Native TUI scoped-models selector failed");
            }
        });
    }

    // ── Session selector and fork flows (pi interactive-mode.ts G3) ──────

    /// Post the in-session session selector (`app.session.resume`, unbound).
    void post_resume_session() {
        const auto weak = weak_from_this();
        boost::asio::post(executor_, [weak] {
            if (const auto self = weak.lock(); self && self->running_) {
                self->show_session_selector();
            }
        });
    }

    /// Post the user-message fork selector (`app.session.fork`, unbound).
    void post_fork_session() {
        const auto weak = weak_from_this();
        boost::asio::post(executor_, [weak] {
            if (const auto self = weak.lock(); self && self->running_) {
                self->show_user_message_selector();
            }
        });
    }

    /// Post the new-session flow (`app.session.new`, unbound; pi
    /// `handleClearCommand`).
    void post_new_session() {
        const auto weak = weak_from_this();
        boost::asio::post(executor_, [weak] {
            if (const auto self = weak.lock(); self && self->running_) {
                self->spawn_flow(
                    [self]() -> boost::asio::awaitable<void> {
                        co_await self->handle_new_session();
                    },
                    "Native TUI new-session flow failed");
            }
        });
    }

    /// Post the main-editor `app.message.copy` flow (pi `handleCopyCommand`).
    void post_copy_last_message() {
        const auto weak = weak_from_this();
        boost::asio::post(executor_, [weak] {
            if (const auto self = weak.lock(); self && self->running_) {
                self->handle_copy_last_message();
            }
        });
    }

    /// pi `handleCtrlZ`: stop the TUI (restore the terminal), ignore SIGINT
    /// while suspended, keep the process alive, and stop the process group
    /// with SIGTSTP; the SIGCONT handler restarts the TUI and forces a
    /// re-render (pi's `ui.start()` + `requestRender(true)`).
    void post_suspend() {
        const auto weak = weak_from_this();
        boost::asio::post(executor_, [weak] {
            if (const auto self = weak.lock(); self && self->running_) {
                self->handle_suspend();
            }
        });
    }

    void handle_suspend() {
#if defined(_WIN32)
        show_status("Suspend to background is not supported on Windows");
        return;
#else
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
                if (error) return;
                if (fired != SIGCONT) {
                    // SIGINT while suspended: swallowed; keep waiting.
                    (*arm)();
                    return;
                }
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
#endif
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
    void post_external_editor() {
        const auto weak = weak_from_this();
        boost::asio::post(executor_, [weak] {
            if (const auto self = weak.lock(); self && self->running_) {
                boost::asio::co_spawn(
                    self->executor_,
                    self->handle_open_external_editor(),
                    [weak](std::exception_ptr exception) {
                        const auto self = weak.lock();
                        if (!self) return;
                        if (exception) {
                            std::string detail = "unknown exception";
                            try {
                                std::rethrow_exception(exception);
                            } catch (const std::exception& error) {
                                detail = error.what();
                            } catch (...) {
                            }
                            self->show_error(std::format(
                                "External editor failed: {}", detail));
                        }
                    });
            }
        });
    }

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
    void handle_name_command(std::string_view text) {
        // pi `text.replace(/^\/name\s*/, "").trim()`.
        const auto name = text.starts_with("/name ")
            ? slash_argument(text, "/name ")
            : std::string{};
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

    /// pi `handleCompactCommand`: clear the status indicator, run the
    /// session compaction with the optional custom instructions, and ignore
    /// failures (they surface through the session events, pi's "Ignore, will
    /// be emitted as an event").
    void post_compact(std::string custom_instructions) {
        const auto weak = weak_from_this();
        boost::asio::post(executor_, [weak, custom_instructions = std::move(custom_instructions)]() mutable {
            if (const auto self = weak.lock(); self && self->running_) {
                self->spawn_flow(
                    [self, custom_instructions = std::move(custom_instructions)]() mutable
                    -> boost::asio::awaitable<void> {
                        co_await self->handle_compact_command(std::move(custom_instructions));
                    },
                    "Native TUI compact flow failed");
            }
        });
    }

    /// pi `handleCompactCommand`: clear the status indicator, then compact;
    /// errors are ignored (they surface through the compaction session
    /// events).
    [[nodiscard]] boost::asio::awaitable<void> handle_compact_command(
        std::string custom_instructions) {
        // pi `handleCompactCommand`: clearStatusIndicator() first.
        if (view_ != nullptr) {
            view_->clear_status_indicator();
            tui_.invalidate();
        }
        // pi: failures are ignored — they surface through the compaction
        // session events (`compaction_end`), so no error is reported here.
        static_cast<void>(
            co_await session_->compact(std::move(custom_instructions)));
    }

    /// pi `showTrustSelector`: spawn the `/trust` flow (the generic
    /// string-list selector over `getProjectTrustOptions` with no
    /// session-only variants, matching the boot trust prompt pattern).
    void show_trust_selector() {
        if (!running_ || view_ == nullptr || !session_->is_open() || !theme_controller_) {
            return;
        }
        const auto self = shared_from_this();
        spawn_flow(
            [self]() -> boost::asio::awaitable<void> {
                co_await self->run_trust_selector();
            },
            "Native TUI trust flow failed");
    }

    /// The `/trust` selector body: `getProjectTrustOptions` (no session-only
    /// variants), the pi status after persisting, and the selector's
    /// cancel/session-only paths.
    [[nodiscard]] boost::asio::awaitable<void> run_trust_selector() {
        if (!running_ || view_ == nullptr || !session_->is_open() || !theme_controller_) {
            co_return;
        }
        const auto cwd = session_->workspace();
        auto options = get_project_trust_options(cwd, /*include_session_only*/ false);
        auto slot = std::make_shared<AuthPromptSlot>(executor_);
        std::vector<std::string> labels;
        labels.reserve(options.size());
        for (const auto& option : options) {
            labels.push_back(option.label);
        }
        const auto weak = weak_from_this();
        auto selector = std::make_shared<StringListSelector>(
            theme_controller_->live_theme(),
            keybindings_->get(),
            format_project_trust_prompt(cwd),
            std::move(labels),
            [weak, slot](std::string label) {
                if (const auto self = weak.lock()) {
                    self->post_from_view(
                        [slot, label = std::move(label)](InteractiveState& self) mutable {
                            self.restore_editor_slot();
                            slot->resolve(std::move(label));
                        });
                }
            },
            [weak, slot] {
                if (const auto self = weak.lock()) {
                    self->post_from_view([slot](InteractiveState& self) {
                        self.restore_editor_slot();
                        slot->resolve(std::unexpected(prompt_cancelled_error()));
                    });
                }
            });
        place_editor_replacement(std::move(selector));

        boost::system::error_code error;
        auto selected = co_await slot->channel.async_receive(
            boost::asio::redirect_error(boost::asio::use_awaitable, error));
        if (error) {
            co_return;
        }
        restore_editor_slot();
        if (!selected) {
            co_return;
        }
        for (auto& option : options) {
            if (option.label != *selected) continue;
            if (!option.updates.empty()) {
                ProjectTrustStore store{coding_agent::trust_store_file_path()};
                if (auto saved = store.setMany(option.updates); !saved) {
                    append_command_error(saved.error());
                }
            }
            // pi: `Saved trust decision: trusted|untrusted. Restart pi for
            // this to take effect.` with the C++ identity.
            show_status(std::format(
                "Saved trust decision: {}. Restart cch for this to take effect.",
                option.trusted ? "trusted" : "untrusted"));
            break;
        }
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
    [[nodiscard]] static util::Error session_replacement_unavailable_error() {
        return util::make_error(
            util::ErrorCode::Unknown,
            "Session switching is not available in this host");
    }

    /// Carry one closed application-level action to the composition host with
    /// the generation that admitted it. A delivery from a retired generation
    /// (the session was replaced or the mode closed) is rejected, so a late
    /// action cannot reach the host; `open_browser_hook()` is the one
    /// captured vector and drops those rejections. A null host applies the
    /// TUI-local platform default for the environment operations. Render
    /// state may coalesce, but this path never drops an admitted action.
    [[nodiscard]] util::Expected<TuiActionResultVariant> deliver_action(
        std::size_t captured_generation,
        TuiActionVariant action) {
        if (captured_generation != action_generation_) {
            return std::unexpected(util::make_error(
                util::ErrorCode::Cancelled,
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
            [](auto&& payload) -> util::Expected<TuiActionResultVariant> {
                using T = std::decay_t<decltype(payload)>;
                if constexpr (std::is_same_v<T, OpenBrowserAction>) {
                    open_browser(std::move(payload.url));
                    return TuiActionResultVariant{std::monostate{}};
                } else if constexpr (std::is_same_v<T, WriteClipboardAction>) {
                    return TuiActionResultVariant{
                        write_clipboard_text(payload.text)};
                } else if constexpr (std::is_same_v<T, SuspendProcessAction>) {
#if !defined(_WIN32)
                    (void)::kill(0, SIGTSTP);
#endif
                    return TuiActionResultVariant{std::monostate{}};
                } else if constexpr (std::is_same_v<T, ReplaceSessionAction>) {
                    return TuiActionResultVariant{
                        util::Expected<coding_agent::CreateAgentSessionResult>{
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
    [[nodiscard]] util::Expected<coding_agent::CreateAgentSessionResult>
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
            std::get_if<util::Expected<coding_agent::CreateAgentSessionResult>>(
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
    [[nodiscard]] util::ExpectedVoid replace_session(
        std::unique_ptr<AgentSession> next) {
        retire_action_generation();
        subscription_.reset();
        owned_session_ = std::move(next);
        session_ = owned_session_.get();
        model_completion_ = nullptr;
        update_model_completion();
        auto subscribed = subscribe_to_session(weak_from_this());
        if (!subscribed) {
            return std::unexpected(subscribed.error());
        }
        displayed_agent_diagnostics_.clear();
        // The new session's resources replace the loaded-resources block
        // (pi `showLoadedResources` after `renderCurrentSessionState`). The
        // theme re-registration gap (replacement drops the created session's
        // theme documents) is pre-existing and out of scope; the themes
        // section keeps the registered set.
        refresh_loaded_resources();
        rebuild_chat();
        return {};
    }

    /// pi `handleResumeSession`: switch to the target session file, with the
    /// in-session missing-cwd Continue/Cancel prompt (pi
    /// `promptForMissingSessionCwd`) and the `Resumed session` /
    /// `Resumed session in current cwd` / `Resume cancelled` statuses.
    [[nodiscard]] boost::asio::awaitable<void> handle_resume_session(
        std::string session_path) {
        if (!action_sink_) {
            show_error("Session switching is not available in this host");
            co_return;
        }
        // pi `SessionManager.open` reads the header cwd first; a stored cwd
        // that no longer exists prompts (assertSessionCwdExists).
        const auto target = std::filesystem::path{session_path};
        std::optional<std::filesystem::path> header_cwd;
        if (auto info = session_discovery::build_session_info(target);
            info && !info->cwd.empty()) {
            header_cwd = std::filesystem::path{info->cwd};
        }
        std::optional<std::filesystem::path> cwd_override;
        if (header_cwd) {
            std::error_code exists_ec;
            if (!std::filesystem::exists(*header_cwd, exists_ec)) {
                auto chosen = co_await prompt_for_missing_session_cwd(
                    *header_cwd, session_->workspace());
                if (!chosen) {
                    show_status("Resume cancelled");
                    co_return;
                }
                cwd_override = chosen;
            }
        }

        auto request = make_session_request(
            cwd_override ? *cwd_override : session_->workspace(),
            ExplicitOpenOrCreateSessionTarget{target});
        request.resume_cwd_override = cwd_override;
        auto created = request_session_replacement(action_generation_, std::move(request));
        if (!created) {
            show_error(combined_error_text(created.error()));
            co_return;
        }
        if (auto replaced = replace_session(std::move(created->session)); !replaced) {
            show_error(combined_error_text(replaced.error()));
            co_return;
        }
        show_status(
            cwd_override ? "Resumed session in current cwd" : "Resumed session");
    }

    /// pi `handleClearCommand` subset: a new persisted (or in-memory, when
    /// the current session is in-memory) session in the current session's
    /// directory, then the `✓ New session started` chat line.
    [[nodiscard]] boost::asio::awaitable<void> handle_new_session() {
        if (!action_sink_) {
            show_error("Session switching is not available in this host");
            co_return;
        }
        auto request = make_session_request(
            session_->workspace(),
            session_->session_path()
                ? SessionTarget{DefaultPersistedSessionTarget{}}
                : SessionTarget{InMemorySessionTarget{}});
        if (session_->session_path()) {
            request.session_dir = session_->session_path()->parent_path().string();
        }
        auto created = request_session_replacement(action_generation_, std::move(request));
        if (!created) {
            show_error(combined_error_text(created.error()));
            co_return;
        }
        if (auto replaced = replace_session(std::move(created->session)); !replaced) {
            show_error(combined_error_text(replaced.error()));
            co_return;
        }
        if (view_ != nullptr) {
            view_->append_frontend_message("✓ New session started");
            tui_.invalidate();
        }
    }

    /// pi `showUserMessageSelector` flow: the fork picker with the last user
    /// message preselected; `No messages to fork from` when there are none;
    /// on selection, `prepare_fork` + session replacement with the
    /// `selectedText` editor pre-fill and the `Forked to new session` status.
    void show_user_message_selector() {
        if (!running_ || view_ == nullptr || !session_->is_open() || !theme_controller_) {
            return;
        }
        const auto messages = session_->get_user_messages_for_forking();
        if (messages.empty()) {
            show_status("No messages to fork from");
            return;
        }
        const auto initial_selected_id = messages.back().entry_id;
        std::vector<UserForkItem> items;
        items.reserve(messages.size());
        for (const auto& message : messages) {
            items.push_back(UserForkItem{
                .entry_id = message.entry_id,
                .text = message.text,
            });
        }

        const auto weak = weak_from_this();
        auto selector = std::make_shared<UserMessageSelectorComponent>(
            theme_controller_->live_theme(),
            keybindings_->get(),
            std::move(items),
            initial_selected_id,
            [weak](std::string entry_id) {
                if (const auto self = weak.lock()) {
                    self->post_from_view(
                        [entry_id = std::move(entry_id)](InteractiveState& state) mutable {
                            state.restore_editor_slot();
                            const auto shared = state.shared_from_this();
                            state.spawn_flow(
                                [shared, entry_id = std::move(entry_id)]() mutable
                                -> boost::asio::awaitable<void> {
                                    co_await shared->handle_fork_session(
                                        std::move(entry_id));
                                },
                                "Native TUI session fork flow failed");
                        });
                }
            },
            [weak] {
                if (const auto self = weak.lock()) {
                    self->post_from_view([](InteractiveState& state) { state.restore_editor_slot(); });
                }
            },
            [weak] {
                if (const auto self = weak.lock()) self->post_invalidate();
            });
        place_editor_replacement(std::move(selector));
    }

    /// pi `AgentSessionRuntime.fork` presentation: prepare the branch, create
    /// the replacement session, pre-fill the editor with `selectedText`, and
    /// report `Forked to new session`.
    [[nodiscard]] boost::asio::awaitable<void> handle_fork_session(
        std::string entry_id) {
        if (!action_sink_) {
            show_error("Session switching is not available in this host");
            co_return;
        }
        auto prepared = session_->prepare_fork(entry_id, runtime::ForkPosition::Before);
        if (!prepared) {
            show_error(combined_error_text(prepared.error()));
            co_return;
        }

        std::optional<std::string> selected_text = std::move(prepared->selected_text);
        auto request = make_session_request(
            session_->workspace(),
            prepared->branched_path
                ? SessionTarget{ExplicitOpenOrCreateSessionTarget{
                      *prepared->branched_path}}
                : SessionTarget{InMemorySessionTarget{}});
        if (prepared->in_memory_seed) {
            request.in_memory_branch_seed = std::move(prepared->in_memory_seed);
        }
        auto created = request_session_replacement(action_generation_, std::move(request));
        if (!created) {
            show_error(combined_error_text(created.error()));
            co_return;
        }
        if (auto replaced = replace_session(std::move(created->session)); !replaced) {
            show_error(combined_error_text(replaced.error()));
            co_return;
        }
        if (view_ != nullptr && selected_text && !selected_text->empty()) {
            view_->set_editor_text(std::move(*selected_text));
        }
        show_status("Forked to new session");
    }

    /// pi `promptForMissingSessionCwd`: the generic string-list selector over
    /// `Session cwd not found\n<formatMissingSessionCwdPrompt>` with
    /// `Yes`/`No`; the selected cwd (or nullopt on cancel) resolves through
    /// an asio channel.
    [[nodiscard]] boost::asio::awaitable<std::optional<std::filesystem::path>>
    prompt_for_missing_session_cwd(
        std::filesystem::path session_cwd,
        std::filesystem::path fallback_cwd) {
        if (!running_ || view_ == nullptr || !theme_controller_) {
            co_return std::nullopt;
        }
        const auto executor = co_await boost::asio::this_coro::executor;
        auto slot = std::make_shared<PromptSlot>(executor);
        // pi `promptForMissingSessionCwd`: `Session cwd not found\n` over
        // `formatMissingSessionCwdPrompt` (session-cwd.ts), verbatim.
        const auto prompt_text = format_missing_session_cwd_prompt(
            MissingSessionCwdIssue{
                .session_file = {},
                .session_cwd = session_cwd,
                .fallback_cwd = fallback_cwd,
            });
        const auto title = "Session cwd not found\n" + prompt_text;
        const auto weak = weak_from_this();
        auto selector = std::make_shared<StringListSelector>(
            theme_controller_->live_theme(),
            keybindings_->get(),
            title,
            std::vector<std::string>{"Yes", "No"},
            [slot, fallback_cwd](std::string selected) {
                slot->resolve(selected == "Yes"
                    ? util::Expected<std::string>{fallback_cwd.string()}
                    : std::unexpected(prompt_cancelled_error()));
            },
            [slot] { slot->resolve(std::unexpected(prompt_cancelled_error())); });
        place_editor_replacement(std::move(selector));

        boost::system::error_code error;
        auto result = co_await slot->channel.async_receive(
            boost::asio::redirect_error(boost::asio::use_awaitable, error));
        if (error) {
            restore_editor_slot();
            co_return std::nullopt;
        }
        restore_editor_slot();
        if (!result) {
            co_return std::nullopt;
        }
        co_return std::filesystem::path{*result};
    }

    /// pi `showSessionSelector`: the session selector in the editor slot with
    /// the current-folder and all-scope loaders (pi `SessionManager.list` /
    /// `listAll` with the `usesDefaultSessionDir` branch).
    void show_session_selector() {
        if (!running_ || view_ == nullptr || !session_->is_open() || !theme_controller_) {
            return;
        }
        const auto workspace = session_->workspace();
        std::optional<std::filesystem::path> session_dir;
        if (auto path = session_->session_path()) {
            session_dir = path->parent_path();
        }
        const auto sessions_root = coding_agent::sessions_root_path();
        const auto default_dir =
            sessions_root / session_paths::encode_workspace_key(workspace);
        const bool uses_default =
            session_dir && *session_dir == default_dir;
        const auto cwd_filter = [&]() -> std::optional<std::filesystem::path> {
            if (!session_dir) return std::nullopt;
            return uses_default ? std::nullopt
                                : std::optional<std::filesystem::path>{workspace};
        }();
        const auto loader_dir = session_dir.value_or(std::filesystem::path{});
        const auto current_loader =
            [loader_dir, cwd_filter]() -> std::vector<session_discovery::SessionInfo> {
                if (loader_dir.empty()) return {};
                return session_discovery::list_sessions_info(loader_dir, cwd_filter);
            };
        const auto all_loader =
            [sessions_root, session_dir, uses_default]()
            -> std::vector<session_discovery::SessionInfo> {
                if (!session_dir) return {};
                return session_discovery::list_all_sessions_info(
                    sessions_root,
                    uses_default ? std::nullopt : session_dir);
            };

        const auto weak = weak_from_this();
        auto selector = std::make_shared<SessionSelectorComponent>(
            theme_controller_->live_theme(),
            keybindings_->get(),
            current_loader,
            all_loader,
            session_->session_path(),
            [weak](std::string session_path) {
                if (const auto self = weak.lock()) {
                    self->post_from_view(
                        [session_path = std::move(session_path)](InteractiveState& state) mutable {
                            state.restore_editor_slot();
                            const auto shared = state.shared_from_this();
                            state.spawn_flow(
                                [shared, session_path = std::move(session_path)]() mutable
                                -> boost::asio::awaitable<void> {
                                    co_await shared->handle_resume_session(
                                        std::move(session_path));
                                },
                                "Native TUI session resume flow failed");
                        });
                }
            },
            [weak] {
                if (const auto self = weak.lock()) {
                    self->post_from_view([](InteractiveState& state) { state.restore_editor_slot(); });
                }
            },
            [weak] {
                if (const auto self = weak.lock()) self->post_exit();
            },
            [weak](std::string session_path, std::string name) -> util::ExpectedVoid {
                // pi `renameSession`: open the session manager and append the
                // trimmed session_info entry.
                auto opened = harness::session::JsonlSessionStore::open_existing(
                    std::filesystem::path{session_path});
                if (!opened) {
                    return std::unexpected(opened.error());
                }
                return opened->append_session_info(std::nullopt, name);
            },
            [weak] {
                if (const auto self = weak.lock()) self->post_invalidate();
            });
        place_editor_replacement(std::move(selector));
    }

    /// pi `showTreeSelector`: the session tree overlay in the editor slot
    /// (pi's `showSelector` editorContainer swap). The tree renders the
    /// session topology with the filters and the eleven `app.tree.*` actions
    /// bound inside the component; selecting a node runs the navigateTree
    /// flow, `shift+l` edits labels through the session, and `app.message.copy`
    /// copies the selected entry.
    void show_tree_selector() {
        if (!running_ || view_ == nullptr || !session_->is_open() || !theme_controller_) {
            return;
        }
        auto topology = session_->session_tree();
        if (!topology) {
            show_error(combined_error_text(topology.error()));
            return;
        }
        if (topology->roots.empty()) {
            show_status("No entries in session");
            return;
        }
        const auto leaf_id = topology->leaf_id;
        const auto weak = weak_from_this();
        auto selector = std::make_shared<TreeSelectorComponent>(
            theme_controller_->live_theme(),
            keybindings_->get(),
            std::move(topology->roots),
            topology->leaf_id,
            terminal_.dimensions().rows,
            [weak, leaf_id](std::string entry_id) {
                if (const auto self = weak.lock()) {
                    self->post_from_view(
                        [entry_id = std::move(entry_id), leaf_id](InteractiveState& state) mutable {
                            // pi: selecting the current leaf is a no-op
                            // (nothing can move the leaf while the selector
                            // is open, so the open-time leaf is live).
                            state.restore_editor_slot();
                            if (entry_id == leaf_id) {
                                state.show_status("Already at this point");
                                return;
                            }
                            const auto shared = state.shared_from_this();
                            state.spawn_flow(
                                [shared, entry_id = std::move(entry_id)]() mutable
                                -> boost::asio::awaitable<void> {
                                    co_await shared->handle_tree_navigation(
                                        std::move(entry_id));
                                },
                                "Native TUI tree navigation flow failed");
                        });
                }
            },
            [weak] {
                if (const auto self = weak.lock()) {
                    self->post_from_view([](InteractiveState& state) { state.restore_editor_slot(); });
                }
            },
            [weak](std::string entry_id, std::optional<std::string> label) {
                // pi `onLabelChange` → `appendLabelChange`: the label write
                // posts to the session executor; the component display
                // already shows the committed label.
                if (const auto self = weak.lock()) {
                    self->post_from_view(
                        [entry_id = std::move(entry_id), label = std::move(label)](
                            InteractiveState& state) mutable {
                            if (auto applied = state.session_->set_entry_label(
                                    entry_id, std::move(label));
                                !applied) {
                                state.show_error(combined_error_text(applied.error()));
                            }
                        });
                }
            },
            [weak](std::optional<std::string> text) {
                if (const auto self = weak.lock()) {
                    self->post_from_view(
                        [text = std::move(text)](InteractiveState& state) mutable {
                            state.handle_tree_copy(std::move(text));
                        });
                }
            },
            [weak] {
                if (const auto self = weak.lock()) self->post_invalidate();
            });
        place_editor_replacement(std::move(selector));
    }

    /// pi tree `onCopy`: copy the selected entry's text with the pi statuses.
    void handle_tree_copy(std::optional<std::string> text) {
        if (!text || text->empty()) {
            show_error("Selected entry has no text to copy");
            return;
        }
        if (!write_clipboard_text_sink(*text)) {
            show_error("Failed to copy to clipboard");
            return;
        }
        show_status("Copied selected message to clipboard");
    }

    /// pi `navigateTree` presentation: switch the active path, rebuild the
    /// chat from the new session context, pre-fill the editor with the
    /// target message's text (pi: only when the editor is empty), and report
    /// `Navigated to selected point`. The summary prompt loop stays absent
    /// with branch summarization generation (G2).
    [[nodiscard]] boost::asio::awaitable<void> handle_tree_navigation(
        std::string entry_id) {
        // pi stops the active response first (restore queued input, abort,
        // wait for settle) before navigating; the runtime guard rejects a
        // still-active run with pi's verbatim error.
        if (session_->is_busy()) {
            dequeue_pending_input(false);
            session_->abort();
            (void)co_await session_->wait_for_idle();
        }
        auto result = session_->navigate_tree(entry_id);
        if (!result) {
            show_error(combined_error_text(result.error()));
            co_return;
        }
        rebuild_chat();
        if (result->editor_text && !result->editor_text->empty() &&
            view_ != nullptr) {
            auto current = view_->editor_text();
            const auto first = current.find_first_not_of(" \t\r\n");
            if (first == std::string::npos) {
                view_->set_editor_text(std::move(*result->editor_text));
            }
        }
        show_status("Navigated to selected point");
    }

    /// Post the in-session tree selector (`app.session.tree`, unbound; the
    /// double-escape trigger posts directly).
    void post_open_tree_selector() {
        const auto weak = weak_from_this();
        boost::asio::post(executor_, [weak] {
            if (const auto self = weak.lock(); self && self->running_) {
                self->show_tree_selector();
            }
        });
    }

    /// pi `cycleModel` presentation: `Only one model in scope` / `Only one
    /// model available` when the cycle cannot move, otherwise the
    /// `Switched to <name> (thinking: <level>)` status; errors surface as
    /// `Error: <text>` lines.
    [[nodiscard]] boost::asio::awaitable<void> cycle_model(std::string direction) {
        auto result = co_await session_->cycle_model(std::move(direction));
        if (!result) {
            show_error(combined_error_text(result.error()));
            co_return;
        }
        if (!*result) {
            show_status(
                session_->scoped_models().empty() ? "Only one model available"
                                                 : "Only one model in scope");
            co_return;
        }
        const auto& cycle = **result;
        update_model_completion();
        const auto thinking_str =
            cycle.model.reasoning && cycle.thinking_level != "off"
            ? " (thinking: " + cycle.thinking_level + ")"
            : "";
        const auto label = cycle.model.name.empty() ? cycle.model.id : cycle.model.name;
        show_status("Switched to " + label + thinking_str);
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

    /// pi `showModelSelector`: the model selector renders in the editor slot
    /// (pi's `showSelector` editorContainer swap). Selecting a model runs
    /// `session.setModel` on the executor and reports `Model: <id>`; the
    /// settings default write rides the session path.
    void show_model_selector(std::optional<std::string> initial_search_input) {
        if (!running_ || view_ == nullptr || !session_->is_open() || !theme_controller_) return;
        const auto current_model = session_->snapshot().agent_state.model;
        const auto weak = weak_from_this();
        auto selector = std::make_shared<ModelSelectorComponent>(
            theme_controller_->live_theme(),
            keybindings_->get(),
            &current_model,
            session_->model_runtime(),
            executor_,
            session_->scoped_models(),
            [weak](ai::Model model) {
                // Input-thread sink: post the session switch to the executor.
                if (const auto self = weak.lock()) {
                    self->post_from_view([model = std::move(model)](InteractiveState& state) mutable {
                        state.spawn_flow(
                            [state_self = state.shared_from_this(), model = std::move(model)]() mutable
                            -> boost::asio::awaitable<void> {
                                auto switched = co_await state_self->session_->set_model(std::move(model));
                                if (!switched) {
                                    state_self->show_error(combined_error_text(switched.error()));
                                    co_return;
                                }
                                state_self->update_model_completion();
                                state_self->restore_editor_slot();
                                state_self->show_status("Model: " + state_self->session_->snapshot().agent_state.model.id);
                            },
                            "Native TUI model selection failed");
                    });
                }
            },
            [weak] {
                if (const auto self = weak.lock()) {
                    self->post_from_view([](InteractiveState& state) { state.restore_editor_slot(); });
                }
            },
            [weak] {
                if (const auto self = weak.lock()) self->post_invalidate();
            },
            std::move(initial_search_input));
        place_editor_replacement(std::move(selector));
    }

    /// pi `handleModelCommand`: no search term opens the selector; an exact
    /// provider/model reference switches immediately (`Model: <id>`); anything
    /// else opens the selector pre-filtered with the term.
    [[nodiscard]] boost::asio::awaitable<void> handle_model_command(
        std::string search_term) {
        const auto term = trim_editor_submission(std::move(search_term));
        if (term.empty()) {
            show_model_selector(std::nullopt);
            co_return;
        }

        // pi `getModelCandidates`: the scoped set when present, else a live
        // availability refresh.
        std::vector<ai::Model> candidates;
        const auto scoped = session_->scoped_models();
        if (!scoped.empty()) {
            candidates.reserve(scoped.size());
            for (const auto& entry : scoped) candidates.push_back(entry.model);
        } else {
            auto runtime = session_->model_runtime();
            if (!runtime) co_return;
            (void)runtime->refresh();
            auto available = co_await runtime->get_available();
            if (!available) {
                show_error(combined_error_text(available.error()));
                co_return;
            }
            candidates = std::move(*available);
        }

        if (const auto matched = find_exact_model_reference_match(term, candidates)) {
            const auto model = *matched;
            auto switched = co_await session_->set_model(model);
            if (!switched) {
                show_error(combined_error_text(switched.error()));
                co_return;
            }
            update_model_completion();
            show_status("Model: " + model.id);
            co_return;
        }
        show_model_selector(term);
    }

    /// pi `showModelsSelector` / `updateSessionModels`/`onPersist`: the
    /// scoped-models selector starts from the session scope, else the
    /// settings `enabledModels` patterns resolved over the live availability
    /// (no-match diagnostics become unavailable ids); changes stay
    /// session-only until `app.models.save` persists them.
    [[nodiscard]] boost::asio::awaitable<void> show_scoped_models_selector() {
        if (!running_ || view_ == nullptr || !session_->is_open() || !theme_controller_) co_return;
        auto runtime = session_->model_runtime();
        if (!runtime) co_return;
        // pi: refresh() then getAvailable().
        (void)runtime->refresh();
        auto available = co_await runtime->get_available();
        if (!available) {
            show_error(combined_error_text(available.error()));
            co_return;
        }
        const auto all_models = std::move(*available);
        std::set<std::string, std::less<>> all_model_ids;
        for (const auto& model : all_models) {
            all_model_ids.insert(model.provider + "/" + model.id);
        }
        const std::vector<std::string>* configured_patterns =
            settings_manager_ && settings_manager_->settings().enabled_models
            ? &*settings_manager_->settings().enabled_models
            : nullptr;
        const auto& session_scoped_models = session_->scoped_models();
        if (all_models.empty() &&
            (configured_patterns == nullptr || configured_patterns->empty()) &&
            session_scoped_models.empty()) {
            show_status("No models available");
            co_return;
        }

        std::optional<ModelScopeResolution> configured_scope;
        if (configured_patterns != nullptr && !configured_patterns->empty()) {
            configured_scope =
                resolve_model_scope_with_diagnostics(*configured_patterns, all_models);
        }
        auto current_enabled_ids = initial_selector_enabled_ids(
            session_scoped_models, configured_scope);

        const auto weak = weak_from_this();
        const auto all_models_shared = std::make_shared<const std::vector<ai::Model>>(all_models);
        const auto all_model_ids_shared = std::make_shared<const std::set<std::string, std::less<>>>(all_model_ids);
        auto selector = std::make_shared<ScopedModelsSelectorComponent>(
            theme_controller_->live_theme(),
            keybindings_->get(),
            all_models,
            std::move(current_enabled_ids),
            [weak, all_models = all_models_shared, ids = all_model_ids_shared](
                std::optional<std::vector<std::string>> enabled_ids) {
                // Input-thread sink: apply session-only scope changes on the
                // executor (pi `updateSessionModels`).
                if (const auto self = weak.lock()) {
                    self->post_from_view(
                        [enabled_ids = std::move(enabled_ids),
                         all_models = std::move(all_models),
                         ids = std::move(ids)](InteractiveState& state) mutable {
                            state.apply_scoped_model_change(
                                std::move(enabled_ids), *all_models, *ids);
                        });
                }
            },
            [weak, all_models = all_models_shared, ids = all_model_ids_shared](
                std::optional<std::vector<std::string>> enabled_ids) {
                // Input-thread sink: persist to settings on the executor (pi
                // `onPersist`).
                if (const auto self = weak.lock()) {
                    self->post_from_view(
                        [enabled_ids = std::move(enabled_ids),
                         all_models = std::move(all_models),
                         ids = std::move(ids)](InteractiveState& state) mutable {
                            state.persist_scoped_models(
                                std::move(enabled_ids), *all_models, *ids);
                        });
                }
            },
            [weak] {
                if (const auto self = weak.lock()) {
                    self->post_from_view([](InteractiveState& state) { state.restore_editor_slot(); });
                }
            });
        place_editor_replacement(std::move(selector));
    }

    /// pi `updateSessionModels` (executor): session-only scope changes from
    /// the scoped-models selector. An explicit list with at least one
    /// available model and not every available model enabled resolves to the
    /// session scope; otherwise the scope clears (all enabled / none enabled
    /// = no filter).
    void apply_scoped_model_change(
        std::optional<std::vector<std::string>> enabled_ids,
        const std::vector<ai::Model>& all_models,
        const std::set<std::string, std::less<>>& all_model_ids) {
        const bool has_enabled_available =
            enabled_ids && std::any_of(
                               enabled_ids->begin(),
                               enabled_ids->end(),
                               [&](const std::string& id) { return all_model_ids.contains(id); });
        const bool all_available_enabled =
            enabled_ids && std::all_of(
                               all_model_ids.begin(),
                               all_model_ids.end(),
                               [&](const std::string& id) {
                                   return std::find(
                                              enabled_ids->begin(),
                                              enabled_ids->end(),
                                              id) != enabled_ids->end();
                               });
        if (enabled_ids && has_enabled_available && !all_available_enabled) {
            session_->set_scoped_models(
                resolve_model_scope(*enabled_ids, all_models));
        } else {
            session_->set_scoped_models({});
        }
        update_model_completion();
        tui_.invalidate();
    }

    /// pi `onPersist` (executor): persist the current selection to the global
    /// `enabledModels` settings field; an all-enabled selection clears the
    /// field (pi writes `undefined`).
    void persist_scoped_models(
        std::optional<std::vector<std::string>> enabled_ids,
        const std::vector<ai::Model>& all_models,
        const std::set<std::string, std::less<>>& all_model_ids) {
        const bool all_enabled =
            enabled_ids && enabled_ids->size() == all_models.size() &&
            std::all_of(
                enabled_ids->begin(),
                enabled_ids->end(),
                [&](const std::string& id) { return all_model_ids.contains(id); });
        const auto new_patterns =
            !enabled_ids || all_enabled ? std::nullopt : std::move(enabled_ids);
        if (settings_manager_) {
            (void)settings_manager_->set_enabled_models(std::move(new_patterns));
        }
        show_status("Model selection saved to settings");
    }

    /// One captured open-browser delivery for the login dialog's auth-URL
    /// view. The hook captures the generation that admitted it; a delivery
    /// from a retired generation (the session was replaced or the mode
    /// closed) is rejected and safely dropped, so a late auth-URL open from
    /// an old dialog cannot reach the host.
    [[nodiscard]] OpenBrowserSink open_browser_hook() {
        const auto weak = weak_from_this();
        const std::size_t captured_generation = action_generation_;
        return [weak, captured_generation](std::string url) {
            if (const auto self = weak.lock();
                self && captured_generation == self->action_generation_) {
                (void)self->deliver_action(
                    captured_generation,
                    TuiActionVariant{OpenBrowserAction{std::move(url)}});
            }
        };
    }

    [[nodiscard]] LoginDialogActionSink dialog_invalidate_hook() {
        const auto weak = weak_from_this();
        return [weak] {
            if (const auto self = weak.lock()) self->post_invalidate();
        };
    }

    void open_login(std::string provider_ref) {
        if (!running_ || view_ == nullptr || !session_->is_open()) return;
        const auto self = shared_from_this();
        spawn_flow(
            [self, provider_ref = std::move(provider_ref)]() mutable
                -> boost::asio::awaitable<void> {
                co_await self->handle_login_command(std::move(provider_ref));
            },
            "Native TUI login flow failed");
    }

    void open_logout() {
        if (!running_ || view_ == nullptr || !session_->is_open()) return;
        const auto self = shared_from_this();
        spawn_flow(
            [self]() -> boost::asio::awaitable<void> {
                co_await self->run_logout();
            },
            "Native TUI logout flow failed");
    }

    /// pi `handleLoginCommand`.
    [[nodiscard]] boost::asio::awaitable<void> handle_login_command(std::string provider_ref) {
        auto runtime = session_->model_runtime();
        if (!runtime) co_return;
        // pi awaits getAvailable() before presenting login options.
        static_cast<void>(co_await runtime->get_available());
        const auto ref = trim_editor_submission(std::move(provider_ref));
        if (ref.empty()) {
            show_login_auth_type_selector(std::nullopt);
            co_return;
        }
        auto matches = find_login_provider_options(
            compute_login_provider_options(*runtime), ref);
        if (matches.size() == 1) {
            co_await start_provider_login(matches.front());
            co_return;
        }
        if (matches.size() > 1) {
            const auto same_provider = std::all_of(
                matches.begin(), matches.end(), [&](const auto& option) {
                    return option.id == matches.front().id;
                });
            if (same_provider) {
                show_login_auth_type_selector(std::move(matches));
                co_return;
            }
        }
        show_login_provider_selector(std::nullopt, ref);
    }

    /// pi `showLoginAuthTypeSelector` over the generic string-list selector.
    void show_login_auth_type_selector(
        std::optional<std::vector<AuthSelectorProvider>> provider_options) {
        const std::string subscription_label{login_subscription_label()};
        const std::string api_key_label{login_api_key_label()};
        std::vector<std::string> options;
        bool has_oauth = true;
        bool has_api_key = true;
        if (provider_options) {
            has_oauth = std::any_of(
                provider_options->begin(), provider_options->end(), [](const auto& option) {
                    return option.auth_type == AuthSelectorType::OAuth;
                });
            has_api_key = std::any_of(
                provider_options->begin(), provider_options->end(), [](const auto& option) {
                    return option.auth_type == AuthSelectorType::ApiKey;
                });
        }
        if (has_oauth) options.push_back(subscription_label);
        if (has_api_key) options.push_back(api_key_label);
        if (options.empty()) {
            show_status(std::string{login_methods_empty_message()});
            return;
        }
        if (provider_options && options.size() == 1 && !provider_options->empty()) {
            const auto self = shared_from_this();
            const auto option = provider_options->front();
            spawn_flow(
                [self, option]() -> boost::asio::awaitable<void> {
                    co_await self->start_provider_login(option);
                },
                "Native TUI login flow failed");
            return;
        }
        const std::string title = provider_options && !provider_options->empty()
            ? "Select authentication method for " + provider_options->front().name + ":"
            : "Select authentication method:";
        const auto weak = weak_from_this();
        auto selector = std::make_shared<StringListSelector>(
            theme_controller_->live_theme(),
            keybindings_->get(),
            title,
            options,
            [weak, provider_options, subscription_label](std::string selected) {
                if (const auto self = weak.lock()) {
                    self->post_from_view(
                        [provider_options, subscription_label, selected = std::move(selected)](
                            InteractiveState& self) mutable {
                            self.restore_editor_slot();
                            const auto type = selected == subscription_label
                                ? AuthSelectorType::OAuth
                                : AuthSelectorType::ApiKey;
                            if (provider_options) {
                                const auto found = std::find_if(
                                    provider_options->begin(),
                                    provider_options->end(),
                                    [type](const auto& option) {
                                        return option.auth_type == type;
                                    });
                                if (found == provider_options->end()) return;
                                const auto option = *found;
                                const auto shared = self.shared_from_this();
                                self.spawn_flow(
                                    [shared, option]() -> boost::asio::awaitable<void> {
                                        co_await shared->start_provider_login(option);
                                    },
                                    "Native TUI login flow failed");
                                return;
                            }
                            self.show_login_provider_selector(type, "");
                        });
                }
            },
            [weak] {
                if (const auto self = weak.lock()) {
                    self->post_from_view([](InteractiveState& self) {
                        self.restore_editor_slot();
                    });
                }
            });
        place_editor_replacement(std::move(selector));
    }

    /// pi `showLoginProviderSelector` over the OAuth selector.
    void show_login_provider_selector(
        std::optional<AuthSelectorType> filter,
        std::string initial_search) {
        auto runtime = session_->model_runtime();
        if (!runtime) return;
        auto options = compute_login_provider_options(*runtime, filter);
        if (options.empty()) {
            show_status(std::string{login_provider_selector_empty_message(filter)});
            return;
        }
        const auto weak = weak_from_this();
        auto selector = std::make_shared<OAuthSelectorComponent>(
            theme_controller_->live_theme(),
            keybindings_->get(),
            AuthSelectorMode::Login,
            options,
            [weak, options](std::string provider_id, AuthSelectorType type) {
                if (const auto self = weak.lock()) {
                    self->post_from_view(
                        [options, provider_id = std::move(provider_id), type](
                            InteractiveState& self) mutable {
                            self.restore_editor_slot();
                            const auto found = std::find_if(
                                options.begin(), options.end(), [&](const auto& option) {
                                    return option.id == provider_id && option.auth_type == type;
                                });
                            if (found == options.end()) return;
                            const auto option = *found;
                            const auto shared = self.shared_from_this();
                            self.spawn_flow(
                                [shared, option]() -> boost::asio::awaitable<void> {
                                    co_await shared->start_provider_login(option);
                                },
                                "Native TUI login flow failed");
                        });
                }
            },
            [weak, filter] {
                if (const auto self = weak.lock()) {
                    self->post_from_view([filter](InteractiveState& self) {
                        self.restore_editor_slot();
                        // pi: cancelling a filtered picker returns to the
                        // auth-type picker.
                        if (filter) self.show_login_auth_type_selector(std::nullopt);
                    });
                }
            },
            std::move(initial_search));
        place_editor_replacement(std::move(selector));
    }

    /// pi `startProviderLogin`: OAuth takes the OAuth dialog branch; an
    /// api-key method with a login hook takes the API-key dialog branch; an
    /// ambient-only method shows the info dialog.
    [[nodiscard]] boost::asio::awaitable<void> start_provider_login(
        AuthSelectorProvider option) {
        if (option.auth_type == AuthSelectorType::OAuth) {
            co_await run_login_dialog(option.id, option.name, ai::AuthType::OAuth);
            co_return;
        }
        if (option.has_login) {
            co_await run_login_dialog(option.id, option.name, ai::AuthType::ApiKey);
            co_return;
        }
        show_ambient_auth_dialog(option);
    }

    /// pi `showAmbientAuthDialog` (api-key auth configured outside the
    /// binary, e.g. an environment-only key).
    void show_ambient_auth_dialog(const AuthSelectorProvider& option) {
        const auto weak = weak_from_this();
        auto dialog = std::make_shared<LoginDialogComponent>(
            theme_controller_->live_theme(),
            keybindings_->get(),
            option.name + " setup",
            dialog_invalidate_hook(),
            open_browser_hook(),
            [weak] {
                if (const auto self = weak.lock()) {
                    self->post_from_view([](InteractiveState& self) {
                        self.restore_editor_slot();
                    });
                }
            });
        dialog->show_info(
            option.method_name.value_or("Authentication") +
                " is configured outside cch.",
            {},
            true);
        place_editor_replacement(std::move(dialog));
    }

    /// pi `showLoginDialog` / `showApiKeyLoginDialog`: run the provider login
    /// flow against the editor-slot dialog, then complete authentication.
    [[nodiscard]] boost::asio::awaitable<void> run_login_dialog(
        std::string provider_id,
        std::string provider_name,
        ai::AuthType type) {
        auto runtime = session_->model_runtime();
        if (!runtime) co_return;
        const auto previous_model = session_->snapshot().agent_state.model;
        auto dialog = std::make_shared<LoginDialogComponent>(
            theme_controller_->live_theme(),
            keybindings_->get(),
            "Login to " + provider_name,
            dialog_invalidate_hook(),
            open_browser_hook());
        place_editor_replacement(dialog);

        ai::AuthInteraction interaction;
        interaction.stop_token = dialog->stop_token();
        const auto self = shared_from_this();
        interaction.prompt = [self, dialog](ai::AuthPrompt prompt)
            -> cch::support::AsyncResult<std::string> {
            return cch::ai::detail::make_async_result(
                [self, dialog, prompt = std::move(prompt)]() mutable
                    -> boost::asio::awaitable<util::Expected<std::string>> {
                    co_return co_await self->show_auth_prompt(dialog, std::move(prompt));
                });
        };
        interaction.notify = [self, dialog](const ai::AuthEvent& event) {
            self->notify_auth_dialog(*dialog, event);
        };
        const auto completion_provider_id = provider_id;
        auto result = co_await runtime->login(
            std::move(provider_id), type, std::move(interaction));
        restore_editor_slot();
        if (result) {
            co_await complete_provider_authentication(
                completion_provider_id, provider_name, type, previous_model);
            co_return;
        }
        // Login Cancellation suppresses failure UI on the stable cancelled
        // kind (#328); every other failure shows pi's failure text.
        if (result.error().code != util::ErrorCode::Cancelled) {
            show_error(
                (type == ai::AuthType::OAuth
                     ? "Failed to login to " + provider_name
                     : "Failed to save API key for " + provider_name) +
                ": " + combined_error_text(result.error()));
        }
    }

    /// pi `completeProviderAuthentication`: refresh availability, auto-select
    /// the provider's default model only when the current model is the
    /// unknown placeholder, and report through status + selection errors.
    [[nodiscard]] boost::asio::awaitable<void> complete_provider_authentication(
        std::string provider_id,
        std::string provider_name,
        ai::AuthType type,
        ai::Model previous_model) {
        auto runtime = session_->model_runtime();
        if (!runtime) co_return;
        auto available = co_await runtime->get_available();
        const auto selector_type = type == ai::AuthType::OAuth
            ? AuthSelectorType::OAuth
            : AuthSelectorType::ApiKey;
        const auto action_label = login_action_label(selector_type, provider_name);
        std::optional<ai::Model> selected_model;
        std::optional<std::string> selection_error;
        if (is_unknown_model(previous_model)) {
            std::vector<ai::Model> provider_models;
            if (available) {
                for (const auto& model : *available) {
                    if (model.provider == provider_id) provider_models.push_back(model);
                }
            }
            const auto default_id = ModelRuntime::default_model_for_provider(provider_id);
            if (!default_id) {
                selection_error =
                    login_selection_error_no_default_model(action_label, provider_id);
            } else if (provider_models.empty()) {
                selection_error = login_selection_error_no_models(action_label);
            } else {
                const auto found = std::find_if(
                    provider_models.begin(), provider_models.end(), [&](const auto& model) {
                        return model.id == *default_id;
                    });
                if (found == provider_models.end()) {
                    selection_error =
                        login_selection_error_default_unavailable(action_label, *default_id);
                } else {
                    auto set = co_await session_->set_model(*found);
                    if (set) {
                        selected_model = *found;
                    } else {
                        selection_error = login_selection_error_select_failed(
                            action_label, set.error().message);
                    }
                }
            }
        }
        // pi's updateAvailableProviderCount/footer invalidate/editor border
        // hooks land with the footer/editor-chrome ticket (P15); the
        // availability refresh above is their data effect here.
        const auto auth_path = auth_path_display(runtime->agent_dir());
        show_status(login_success_status(
            action_label,
            selected_model ? std::optional{selected_model->id} : std::nullopt,
            auth_path));
        if (selection_error) show_error(*selection_error);
    }

    /// pi `showAuthSelect`: a `select`-type AuthPrompt resolves through the
    /// generic string-list selector swapped into the editor slot.
    [[nodiscard]] boost::asio::awaitable<util::Expected<std::string>> show_auth_select(
        std::shared_ptr<LoginDialogComponent> dialog,
        ai::AuthPromptSelect select,
        std::optional<std::stop_token> per_prompt) {
        const auto executor = co_await boost::asio::this_coro::executor;
        auto slot = std::make_shared<AuthPromptSlot>(executor);
        std::vector<std::string> labels;
        labels.reserve(select.options.size());
        for (const auto& option : select.options) labels.push_back(option.label);
        const auto weak = weak_from_this();
        auto selector = std::make_shared<StringListSelector>(
            theme_controller_->live_theme(),
            keybindings_->get(),
            select.message,
            std::move(labels),
            [weak, slot, dialog, options = select.options](std::string label) {
                if (const auto self = weak.lock()) {
                    self->post_from_view(
                        [slot, dialog, label = std::move(label), options = std::move(options)](
                            InteractiveState& self) mutable {
                            // pi restoreDialog, then resolve the option id.
                            self.place_editor_replacement(dialog);
                            const auto found = std::find_if(
                                options.begin(), options.end(), [&](const auto& option) {
                                    return option.label == label;
                                });
                            if (found != options.end()) {
                                slot->resolve(found->id);
                            } else {
                                slot->resolve(std::unexpected(prompt_cancelled_error()));
                            }
                        });
                }
            },
            [weak, slot, dialog] {
                if (const auto self = weak.lock()) {
                    self->post_from_view([slot, dialog](InteractiveState& self) mutable {
                        self.place_editor_replacement(dialog);
                        slot->resolve(std::unexpected(prompt_cancelled_error()));
                    });
                }
            });
        place_editor_replacement(std::move(selector));

        // pi's per-prompt race: an aborted per-prompt token rejects without
        // touching the slot's UI (the flow's unwind restores the editor).
        std::optional<std::stop_callback<std::move_only_function<void()>>> on_abort;
        if (per_prompt) {
            on_abort.emplace(*per_prompt, [slot] {
                slot->resolve(std::unexpected(prompt_cancelled_error()));
            });
        }
        boost::system::error_code error;
        auto result = co_await slot->channel.async_receive(
            boost::asio::redirect_error(boost::asio::use_awaitable, error));
        if (error) {
            co_return std::unexpected(util::make_error(
                util::ErrorCode::Unknown,
                "login select channel failed",
                error.message()));
        }
        co_return std::move(result);
    }

    /// pi `showAuthPrompt`: select → the generic selector; manual_code → the
    /// manual input; text/secret → the prompt view. The optional per-prompt
    /// token rejects with the stable cancelled error (the Codex
    /// callback-vs-manual-input race).
    [[nodiscard]] boost::asio::awaitable<util::Expected<std::string>> show_auth_prompt(
        std::shared_ptr<LoginDialogComponent> dialog,
        ai::AuthPrompt prompt) {
        auto per_prompt = std::move(prompt.stop_token);
        if (per_prompt && per_prompt->stop_requested()) {
            co_return std::unexpected(prompt_cancelled_error());
        }
        if (auto* select = std::get_if<ai::AuthPromptSelect>(&prompt.kind)) {
            co_return co_await show_auth_select(
                std::move(dialog), std::move(*select), std::move(per_prompt));
        }
        if (const auto* manual = std::get_if<ai::AuthPromptManualCode>(&prompt.kind)) {
            if (!per_prompt) co_return co_await dialog->show_manual_input(manual->message);
            std::stop_callback on_abort(*per_prompt, [&dialog] {
                dialog->cancel_pending_prompt();
            });
            co_return co_await dialog->show_manual_input(manual->message);
        }
        std::string message;
        std::optional<std::string> placeholder;
        if (const auto* text = std::get_if<ai::AuthPromptText>(&prompt.kind)) {
            message = text->message;
            placeholder = text->placeholder;
        } else if (const auto* secret = std::get_if<ai::AuthPromptSecret>(&prompt.kind)) {
            message = secret->message;
            placeholder = secret->placeholder;
        }
        if (!per_prompt) {
            co_return co_await dialog->show_prompt(
                std::move(message), std::move(placeholder));
        }
        std::stop_callback on_abort(*per_prompt, [&dialog] {
            dialog->cancel_pending_prompt();
        });
        co_return co_await dialog->show_prompt(
            std::move(message), std::move(placeholder));
    }

    /// pi `notifyAuthDialog`: AuthEvent → the matching dialog view.
    void notify_auth_dialog(LoginDialogComponent& dialog, const ai::AuthEvent& event) {
        if (const auto* url = std::get_if<ai::AuthUrl>(&event.kind)) {
            dialog.show_auth(url->url, url->instructions);
            return;
        }
        if (const auto* device = std::get_if<ai::AuthDeviceCode>(&event.kind)) {
            dialog.show_device_code(device->user_code, device->verification_uri);
            dialog.show_waiting("Waiting for authentication...");
            return;
        }
        if (const auto* info = std::get_if<ai::AuthInfo>(&event.kind)) {
            std::vector<std::pair<std::string, std::optional<std::string>>> links;
            links.reserve(info->links.size());
            for (const auto& link : info->links) {
                links.emplace_back(link.url, link.label);
            }
            dialog.show_info(info->message, std::move(links));
            return;
        }
        if (const auto* progress = std::get_if<ai::AuthProgress>(&event.kind)) {
            dialog.show_progress(progress->message);
            return;
        }
    }

    /// pi `showOAuthSelector("logout")`: the stored-credential picker.
    [[nodiscard]] boost::asio::awaitable<void> run_logout() {
        auto runtime = session_->model_runtime();
        if (!runtime) co_return;
        auto credentials = co_await runtime->list_credentials();
        if (!credentials) {
            show_error("Logout failed: " + combined_error_text(credentials.error()));
            co_return;
        }
        auto options = compute_logout_provider_options(*runtime, std::move(*credentials));
        if (options.empty()) {
            show_status(std::string{logout_no_credentials_message()});
            co_return;
        }
        const auto weak = weak_from_this();
        auto selector = std::make_shared<OAuthSelectorComponent>(
            theme_controller_->live_theme(),
            keybindings_->get(),
            AuthSelectorMode::Logout,
            options,
            [weak, options](std::string provider_id, AuthSelectorType) {
                if (const auto self = weak.lock()) {
                    self->post_from_view(
                        [options, provider_id = std::move(provider_id)](
                            InteractiveState& self) mutable {
                            self.restore_editor_slot();
                            const auto found = std::find_if(
                                options.begin(), options.end(), [&](const auto& option) {
                                    return option.id == provider_id;
                                });
                            if (found == options.end()) return;
                            const auto option = *found;
                            const auto shared = self.shared_from_this();
                            self.spawn_flow(
                                [shared, option]() -> boost::asio::awaitable<void> {
                                    co_await shared->run_logout_provider(option);
                                },
                                "Native TUI logout flow failed");
                        });
                }
            },
            [weak] {
                if (const auto self = weak.lock()) {
                    self->post_from_view([](InteractiveState& self) {
                        self.restore_editor_slot();
                    });
                }
            });
        place_editor_replacement(std::move(selector));
    }

    /// pi's logout selection handler: local removal, availability refresh,
    /// and the verbatim oauth/api_key status messages.
    [[nodiscard]] boost::asio::awaitable<void> run_logout_provider(
        AuthSelectorProvider option) {
        auto runtime = session_->model_runtime();
        if (!runtime) co_return;
        if (auto logged_out = co_await runtime->logout(option.id); !logged_out) {
            show_error("Logout failed: " + combined_error_text(logged_out.error()));
            co_return;
        }
        // pi: updateAvailableProviderCount after logout.
        static_cast<void>(co_await runtime->get_available());
        show_status(logout_success_message(option.auth_type, option.name));
    }

    /// pi `maybeSaveImplicitProjectTrustAfterReload`: when the boot armed
    /// `autoTrustOnReloadCwd` and the session is trusted but the reloaded
    /// workspace NOW has trust-requiring resources with no saved decision,
    /// persist the implicit trust decision. Returns whether the status line
    /// gains pi's `"; saved project trust"` suffix.
    [[nodiscard]] bool maybe_save_implicit_project_trust_after_reload() {
        // The armed workspace is consumed by exactly one decision attempt
        // (pi clears `autoTrustOnReloadCwd` on every exit path); a successful
        // save returns true for the "; saved project trust" status suffix.
        const auto arm = std::exchange(auto_trust_on_reload_cwd_, std::nullopt);
        if (!arm || session_ == nullptr) {
            return false;
        }
        const auto& cwd = session_->workspace();
        if (*arm != cwd) {
            return false;
        }
        if (!detail::AgentSessionInteractiveAccess::is_project_trusted(*session_)) {
            return false;
        }
        auto fs = harness::WorkspaceFileSystem::create(cwd);
        if (!fs) {
            return false;
        }
        auto detection = detect_project_resources(
            *fs, coding_agent::home_directory() / ".agents" / "skills");
        if (!needs_project_trust_resolution(detection)) {
            return false;
        }
        ProjectTrustStore store{coding_agent::trust_store_file_path()};
        auto entry = store.getEntry(cwd);
        if (!entry) {
            if (view_ != nullptr) {
                view_->append_warning(
                    "Could not save project trust after reload: " +
                    entry.error().message);
                tui_.invalidate();
            }
            return false;
        }
        if (entry->has_value()) {
            return false;
        }
        if (auto saved = store.setMany(std::vector<ProjectTrustUpdate>{
                ProjectTrustUpdate{
                    .path = cwd.string(),
                    .decision = ProjectTrustDecision::Trusted,
                },
            });
            !saved) {
            if (view_ != nullptr) {
                view_->append_warning(
                    "Could not save project trust after reload: " +
                    saved.error().message);
                tui_.invalidate();
            }
            return false;
        }
        return true;
    }

    /// pi `handleReloadCommand` (#418): refuse while streaming/compacting
    /// with pi's verbatim warnings, swap the reload box into the editor
    /// slot, re-read settings/resources through the session, re-catalog
    /// keybindings, re-register themes, re-apply render settings, rebuild
    /// autocomplete, refresh the loaded-resources presentation, surface the
    /// models.json error, and report the pi status line. Any failure
    /// restores the editor and reports `Reload failed: <msg>`.
    [[nodiscard]] boost::asio::awaitable<void> handle_reload() {
        if (!running_ || view_ == nullptr || session_ == nullptr) {
            co_return;
        }
        if (session_->is_streaming()) {
            view_->append_warning(
                "Wait for the current response to finish before reloading.");
            tui_.invalidate();
            co_return;
        }
        if (session_->is_compacting()) {
            view_->append_warning(
                "Wait for compaction to finish before reloading.");
            tui_.invalidate();
            co_return;
        }

        place_editor_replacement(make_reload_box(theme_controller_->live_theme()));
        bool dismissed = false;
        const auto dismiss = [this, &dismissed] {
            if (dismissed) {
                return;
            }
            dismissed = true;
            restore_editor_slot();
        };

        // 1. pi `session.reload()`: settings + resources re-read, System
        //    Prompt rebuild (trust preserved).
        auto result = co_await session_->reload();
        if (!result) {
            dismiss();
            show_error("Reload failed: " + result.error().message);
            co_return;
        }

        // 2. Keybindings re-catalog (ADR 0035 registry read semantics);
        //    diagnostics render like startup.
        if (auto rebind = re_catalog_keybindings(); !rebind) {
            dismiss();
            show_error("Reload failed: " + rebind.error().message);
            co_return;
        }

        // 3. pi `setRegisteredThemes(...)` + `applyFromSettings()`: re-run
        //    theme discovery and re-apply with dark fallback; stash the
        //    discovery diagnostics for the loaded-resources Themes conflicts
        //    section.
        if (theme_controller_) {
            auto discovery = coding_agent::tui::discover_themes(
                std::move(result->themes));
            loaded_theme_diagnostics_ = std::move(discovery.diagnostics);
            theme_controller_->set_registered_themes(std::move(discovery.themes));
            theme_controller_->apply_from_settings();
        }

        // 4. pi `applyRuntimeSettings`: re-read the render settings and
        //    re-apply to the chat.
        if (settings_manager_) {
            hide_thinking_block_ = settings_manager_->hide_thinking_block();
            output_pad_ = settings_manager_->output_pad();
        }
        if (view_ != nullptr) {
            view_->apply_render_settings(hide_thinking_block_, output_pad_);
        }

        // 5. pi `setupAutocompleteProvider()`: rebuild the editor's provider
        //    (slash commands, templates, skill commands from the new set).
        rebuild_autocomplete_provider();

        // 6. Refresh the loaded-resources presentation.
        refresh_loaded_resources();

        // 7. pi `modelRuntime.getError()` → `models.json error: <e>`.
        if (auto runtime = session_->model_runtime()) {
            if (auto error = runtime->get_error(); error && !error->empty()) {
                show_error("models.json error: " + *error);
            }
        }

        // 8. pi `maybeSaveImplicitProjectTrustAfterReload` + status line
        //    (trimmed of "extensions"; the implicit-trust save appends
        //    pi's `"; saved project trust"` suffix).
        const bool saved_implicit_trust =
            maybe_save_implicit_project_trust_after_reload();
        show_status(
            saved_implicit_trust
                ? "Reloaded keybindings, skills, prompts, themes, and context files; saved project trust"
                : "Reloaded keybindings, skills, prompts, themes, and context files");
        dismiss();
    }

    /// pi `setupEditorSubmitHandler` slash dispatch: the if-chain over the
    /// 17 Supported builtins (ADR 0036 G4), each binding to its component or
    /// runtime capability with pi's verbatim strings. `/export` `/import`
    /// `/share` `/changelog` `/clone`, `/debug`, the easter eggs, and any
    /// other unrecognized slash text return false and pass through as an
    /// ordinary Agent Prompt (pi has no general slash parser). The editor has
    /// already cleared on submit, so no branch clears it (pi's
    /// `editor.setText("")` is the editor's own submit behavior).
    [[nodiscard]] bool dispatch_command(std::string_view text) {
        if (!running_ || view_ == nullptr) return false;
        if (text == "/settings") {
            show_settings_selector();
            return true;
        }
        if (text == "/scoped-models") {
            post_open_scoped_models_selector();
            return true;
        }
        if (text == "/model" || text.starts_with("/model ")) {
            const auto search_term =
                text.starts_with("/model ") ? slash_argument(text, "/model ") : std::string{};
            post_open_model_selector(std::move(search_term));
            return true;
        }
        if (text == "/copy") {
            post_copy_last_message();
            return true;
        }
        if (text == "/name" || text.starts_with("/name ")) {
            handle_name_command(text);
            return true;
        }
        if (text == "/session") {
            handle_session_command();
            return true;
        }
        if (text == "/hotkeys") {
            open_hotkeys();
            return true;
        }
        if (text == "/fork") {
            post_fork_session();
            return true;
        }
        if (text == "/tree") {
            post_open_tree_selector();
            return true;
        }
        if (text == "/trust") {
            show_trust_selector();
            return true;
        }
        if (text == "/login" || text.starts_with("/login ")) {
            const auto provider_ref =
                text.starts_with("/login ") ? slash_argument(text, "/login ") : std::string{};
            open_login(std::move(provider_ref));
            return true;
        }
        if (text == "/logout") {
            open_logout();
            return true;
        }
        if (text == "/new") {
            post_new_session();
            return true;
        }
        if (text == "/compact" || text.starts_with("/compact ")) {
            const auto custom_instructions =
                text.starts_with("/compact ")
                ? slash_argument(text, "/compact ")
                : std::string{};
            post_compact(std::move(custom_instructions));
            return true;
        }
        if (text == "/resume") {
            post_resume_session();
            return true;
        }
        if (text == "/reload") {
            const auto shared = shared_from_this();
            spawn_flow(
                [shared]() -> boost::asio::awaitable<void> {
                    co_await shared->handle_reload();
                },
                "Native TUI reload flow failed");
            return true;
        }
        if (text == "/quit") {
            if (view_ != nullptr) {
                tui_.invalidate();
                render();
            }
            request_exit();
            return true;
        }
        return false;
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
        boost::asio::co_spawn(
            executor_,
            [self,
             invocation = std::move(invocation),
             recall = std::move(recall)]() mutable -> boost::asio::awaitable<void> {
                auto result = co_await detail::AgentSessionInteractiveAccess::run_user_bash(
                    *self->session_,
                    std::move(invocation->command),
                    invocation->exclude_from_context,
                    [self](
                        const runtime::UserBashProgress& progress) -> util::ExpectedVoid {
                        if (self->running_ && self->view_ != nullptr) {
                            self->view_->set_user_bash_progress(progress);
                            self->tui_.invalidate();
                        }
                        return {};
                    });
                self->user_bash_finished(std::move(result), recall);
            },
            [weak = weak_from_this()](std::exception_ptr exception) {
                if (!exception) return;
                if (const auto self = weak.lock()) {
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
                    show_tree_selector();
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
            sync_pending_input();
            tui_.invalidate();
            return;
        }

        note_prompt_started();
        prompt_active_ = true;
        const auto self = shared_from_this();
        boost::asio::co_spawn(
            executor_,
            [self,
             text = std::move(text),
             options = std::move(options)]() mutable -> boost::asio::awaitable<void> {
                util::ExpectedVoid result;
                try {
                    result = co_await self->session_->prompt(text, std::move(options));
                } catch (const std::exception& error) {
                    result = std::unexpected(util::make_error(
                        util::ErrorCode::Unknown,
                        "Native TUI prompt failed",
                        error.what()));
                } catch (...) {
                    result = std::unexpected(util::make_error(
                        util::ErrorCode::Unknown,
                        "Native TUI prompt failed",
                        "unknown exception"));
                }
                self->prompt_finished(std::move(result), text);
            },
            [weak = weak_from_this()](std::exception_ptr exception) {
                if (exception) {
                    if (const auto self = weak.lock()) self->prompt_launch_failed(exception);
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
            sync_pending_input();
            tui_.invalidate();
            return;
        }
        if (auto cleared = session_->clear_input_queues(); !cleared) {
            view_->append_diagnostic(bounded_redacted_presentation(std::format(
                "Unable to restore queued input: {}",
                combined_error_text(cleared.error()))));
            sync_pending_input();
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
        sync_pending_input();
        tui_.invalidate();
    }

    void prompt_launch_failed(std::exception_ptr exception) {
        note_prompt_finished();
        prompt_active_ = false;
        std::string detail = "unknown exception";
        try {
            std::rethrow_exception(exception);
        } catch (const std::exception& error) {
            detail = error.what();
        } catch (...) {
        }
        if (view_ != nullptr && running_) {
            view_->append_diagnostic(std::format("Native TUI prompt failed: {}", detail));
            tui_.invalidate();
        }
        if (exit_requested_) signal_exit();
    }

    void prompt_finished(util::ExpectedVoid result, const std::string& submitted_text) {
        note_prompt_finished();
        prompt_active_ = false;
        sync_session_observations();
        if (!result && view_ != nullptr && running_) {
            view_->append_diagnostic(combined_error_text(result.error()));
            view_->restore_submitted_text(submitted_text);
            tui_.invalidate();
        }
        if (exit_requested_ && !user_bash_active_) signal_exit();
    }

    void user_bash_finished(
        util::Expected<runtime::UserBashCompletion> result,
        const std::string& recall) {
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

    void sync_pending_input() {
        if (!running_ || view_ == nullptr || !session_->is_open()) return;
        view_->set_pending_input(session_->snapshot().agent_state.input_queues);
    }

    void sync_session_observations() {
        if (!running_ || view_ == nullptr || !session_->is_open()) return;
        const auto snapshot = session_->snapshot();
        view_->set_pending_input(snapshot.agent_state.input_queues);

        std::vector<std::string> current;
        current.reserve(snapshot.agent_state.diagnostics.size());
        for (const auto& diagnostic : snapshot.agent_state.diagnostics) {
            current.push_back(combined_error_text(diagnostic));
        }

        auto overlap = std::min(displayed_agent_diagnostics_.size(), current.size());
        while (overlap > 0 && !std::equal(
                displayed_agent_diagnostics_.end() - static_cast<std::ptrdiff_t>(overlap),
                displayed_agent_diagnostics_.end(),
                current.begin())) {
            --overlap;
        }
        for (auto index = overlap; index < current.size(); ++index) {
            view_->append_diagnostic(current[index]);
        }
        displayed_agent_diagnostics_ = std::move(current);
    }

    void on_event(const agent::AgentLifecycleEvent& event) {
        if (!running_ || view_ == nullptr) return;
        view_->apply_event(event);
        // pi's working indicator: shown while the agent run streams. The
        // retry/compaction indicators replace it through the session-event
        // path and are cleared by their own end events.
        if (std::holds_alternative<agent::AgentStartEvent>(event)) {
            view_->show_status_working();
        } else if (std::holds_alternative<agent::AgentEndEvent>(event)) {
            view_->clear_status_indicator();
        } else if (std::holds_alternative<agent::MessageStartEvent>(event) &&
                   prompt_active_) {
            view_->show_status_working();
        }
        sync_session_observations();
        tui_.invalidate();
    }

    /// pi's `session.on("auto_retry_start"...` / `compaction_start...`
    /// handlers: the Retry indicator with the backoff countdown, the
    /// Compaction indicator with the reason wording, and the end-event
    /// cleanup with pi's statuses.
    void on_session_event(const AgentSessionEvent& event) {
        if (!running_ || view_ == nullptr) return;
        if (const auto* retry = std::get_if<AutoRetryStartEvent>(&event)) {
            cancel_retry_countdown();
            const auto seconds = static_cast<int>(std::max<std::int64_t>(
                1, (retry->delay_ms + 999) / 1000));
            view_->show_status_retry(
                retry->attempt, retry->max_attempts, seconds);
            start_retry_countdown(retry->attempt, retry->max_attempts, seconds);
        } else if (const auto* retry_end = std::get_if<AutoRetryEndEvent>(&event)) {
            cancel_retry_countdown();
            view_->clear_status_indicator();
            // pi auto_retry_end: only the final failure reports (success
            // shows the ordinary response).
            if (!retry_end->success) {
                show_error(std::format(
                    "Retry failed after {} attempts: {}",
                    retry_end->attempt,
                    retry_end->final_error.value_or("Unknown error")));
            }
        } else if (const auto* compaction = std::get_if<CompactionStartEvent>(&event)) {
            view_->show_status_compaction(compaction->reason);
        } else if (const auto* compaction_end = std::get_if<CompactionEndEvent>(&event)) {
            view_->clear_status_indicator();
            if (compaction_end->aborted) {
                if (compaction_end->reason == "manual") {
                    show_error("Compaction cancelled");
                } else {
                    show_status("Auto-compaction cancelled");
                }
            } else if (compaction_end->error_message) {
                if (compaction_end->reason == "manual") {
                    show_error(*compaction_end->error_message);
                } else {
                    view_->append_diagnostic(*compaction_end->error_message);
                }
            } else {
                // pi compaction_end with a result: rebuild the chat from the
                // fresh snapshot (the compaction summary renders as the
                // latest entry) and refresh the footer's usage totals.
                const auto snapshot = session_->snapshot();
                view_->initialize(snapshot);
                view_->set_pending_input(snapshot.agent_state.input_queues);
            }
        }
        tui_.invalidate();
    }

    /// pi `CountdownTimer` for the retry indicator: one-second ticks rewrite
    /// the `Retrying (n/m) in Ns...` message until the delay elapses. The
    /// tick captures the countdown itself so a tick racing `cancel` (which
    /// resets the state member) never dereferences a reset pointer.
    void start_retry_countdown(int attempt, int max_attempts, int seconds) {
        auto countdown = std::make_shared<RetryCountdown>(
            executor_, attempt, max_attempts);
        retry_countdown_ = countdown;
        countdown->start(seconds, [weak = weak_from_this(), countdown](int remaining) {
            if (const auto self = weak.lock(); self && self->running_ &&
                self->view_ != nullptr) {
                self->view_->set_status_retry_message(
                    countdown->attempt(),
                    countdown->max_attempts(),
                    remaining);
                self->tui_.invalidate();
            }
        });
    }

    void cancel_retry_countdown() {
        retry_countdown_.reset();
    }

    /// pi footer.ts render inputs computed from the live session snapshot
    /// and the model runtime (usage totals over the message history, the
    /// latest assistant cache hit rate, the context estimate, the model and
    /// thinking level, the subscription marker, and the available-provider
    /// count).
    [[nodiscard]] FooterData compute_footer_data() {
        if (session_ == nullptr) {
            // Boot path: the main screen renders while the boot trust prompt
            // overlay is up, before the session binds; the footer shows the
            // boot workspace like pi's startup TUI.
            FooterData data;
            data.cwd = boot_request_ ? boot_request_->workspace
                                     : std::filesystem::path{};
            return data;
        }
        FooterData data;
        const auto snapshot = session_->snapshot();
        data.cwd = session_->workspace();
        footer_data_provider_.set_cwd(data.cwd);
        data.git_branch = footer_data_provider_.git_branch();

        // Usage totals: every assistant message's usage accumulates (pi
        // `addUsageToTotals` over the session entries; the C++ message
        // history is the in-memory entry equivalent, and toolResult/
        // compaction usage is not carried on the C++ message values).
        for (const auto& message : snapshot.agent_state.messages) {
            const auto* assistant = std::get_if<ai::AssistantMessage>(&message);
            if (assistant == nullptr) continue;
            data.input += assistant->usage.input;
            data.output += assistant->usage.output;
            data.cache_read += assistant->usage.cache_read;
            data.cache_write += assistant->usage.cache_write;
            data.cost += assistant->usage.cost.total;
            // pi keeps the latest assistant message's prompt hit rate.
            const auto prompt_tokens =
                assistant->usage.input +
                assistant->usage.cache_read +
                assistant->usage.cache_write;
            if (prompt_tokens > 0) {
                data.cache_hit_rate =
                    (static_cast<double>(assistant->usage.cache_read) /
                     static_cast<double>(prompt_tokens)) * 100.0;
            } else {
                data.cache_hit_rate.reset();
            }
        }

        // Context usage (pi `getContextUsage` subset): the model's context
        // window with the estimated tokens; after a compaction, tokens are
        // unknown until a valid assistant usage lands after the boundary.
        const auto& model = snapshot.agent_state.model;
        data.context_window = static_cast<std::size_t>(model.context_window);
        data.model_id = model.id;
        data.provider = model.provider;
        data.model_reasoning = model.reasoning;
        data.thinking_level = snapshot.agent_state.thinking_level.empty()
            ? std::string{"off"}
            : snapshot.agent_state.thinking_level;
        if (data.context_window > 0) {
            const auto& messages = snapshot.agent_state.messages;
            std::optional<std::size_t> latest_compaction;
            for (std::size_t index = 0; index < messages.size(); ++index) {
                if (std::holds_alternative<ai::CompactionSummaryMessage>(messages[index])) {
                    latest_compaction = index;
                }
            }
            bool post_compaction_usage = false;
            if (latest_compaction) {
                for (std::size_t index = *latest_compaction + 1;
                     index < messages.size();
                     ++index) {
                    const auto* assistant =
                        std::get_if<ai::AssistantMessage>(&messages[index]);
                    if (assistant == nullptr) continue;
                    if (assistant->stop_reason != ai::AssistantStopReason::Aborted &&
                        assistant->stop_reason != ai::AssistantStopReason::Error &&
                        harness::session::calculate_context_tokens(assistant->usage) > 0) {
                        post_compaction_usage = true;
                        break;
                    }
                }
                if (!post_compaction_usage) {
                    data.context_tokens = std::nullopt;
                }
            }
            if (!latest_compaction || post_compaction_usage) {
                data.context_tokens =
                    harness::session::estimate_context_tokens(messages).tokens;
            }
        }

        // pi `usingSubscription`: kimi-coding, or any provider authenticating
        // through an OAuth credential. The runtime may be absent on
        // focused-test sessions; both markers stay off then.
        const auto runtime = session_->model_runtime();
        if (!model.id.empty() && runtime) {
            data.using_subscription =
                model.provider == "kimi-coding" ||
                runtime->is_using_oauth(model.provider);
        }

        // pi `updateAvailableProviderCount`: unique providers in the scoped
        // set, or in the runtime's availability snapshot.
        const auto& scoped = session_->scoped_models();
        std::set<std::string> providers;
        if (!scoped.empty()) {
            for (const auto& entry : scoped) providers.insert(entry.model.provider);
        } else if (runtime) {
            for (const auto& available : runtime->get_available_snapshot()) {
                providers.insert(available.provider);
            }
        }
        data.available_provider_count = providers.size();

        if (settings_manager_) {
            const auto compaction = settings_manager_->settings().compaction;
            data.auto_compact_enabled =
                !compaction || compaction->enabled.value_or(true);
        }
        return data;
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
        session_->close();
        if (!prompt_active_ && !user_bash_active_) signal_exit();
    }

    void signal_exit() {
        try {
            (void)exit_wait_.cancel();
        } catch (...) {
            if (!completion_result_) {
                completion_result_ = std::unexpected(util::make_error(
                    util::ErrorCode::Unknown,
                    "Native TUI exit notification failed"));
            }
        }
    }

    AgentSession* session_; // must outlive this interactive run.
    /// Owned replacement session (pi `AgentSessionRuntime.switchSession` /
    /// `newSession` / `fork`): the in-session flows recreate the session
    /// through the config factory and keep the replacement alive here. The
    /// initial session stays borrowed from the host.
    std::unique_ptr<AgentSession> owned_session_;
    cch::tui::Terminal& terminal_; // must outlive this interactive run.
    cch::tui::Tui tui_;
    /// pi's mutable shared KeybindingsManager consumption shape (ADR 0035,
    /// #418): every durable view component observes this slot; `/reload`
    /// replaces the current registry so all consumers see the new bindings
    /// live. Selectors take an ephemeral `get()` snapshot.
    std::shared_ptr<SharedKeybindings> keybindings_;
    /// The platform and agent config directory the keybinding catalog was
    /// assembled under, retained for the `/reload` re-catalog.
    cch::tui::KeybindingPlatform keybinding_platform_{cch::tui::native_keybinding_platform()};
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
    /// Immutable `/model` completion snapshot, replaced on the executor by
    /// `update_model_completion()` whenever the candidate set changes.
    std::shared_ptr<const ModelCompletionSnapshot> model_completion_{};
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

    /// Startup diagnostics stashed by the boot `start()` until the boot
    /// session binds and `initialize_view` renders them (pi
    /// `renderInitialMessages` after the trust prompt).
    InteractiveStartupDiagnostics startup_diagnostics_{};
    /// Theme discovery (parse/collision) diagnostics for the loaded-resources
    /// `[Theme conflicts]` section (pi `getThemes().diagnostics`), stashed at
    /// boot bind and refreshed by `/reload` (#418).
    std::vector<ResourceDiagnostic> loaded_theme_diagnostics_;
    /// pi `autoTrustOnReloadCwd` (main.ts): the boot workspace where the boot
    /// had no trust override and no trust-requiring resources. After a
    /// `/reload`, when the workspace NOW has trust-requiring resources and
    /// the session is trusted with no saved decision, the implicit trust
    /// decision persists automatically (pi `maybeSaveImplicitProjectTrustAfterReload`,
    /// `"; saved project trust"` status suffix).
    std::optional<std::filesystem::path> auto_trust_on_reload_cwd_;
    /// Initial prompt stashed by the boot `start()` until the boot session
    /// binds (pi main.ts `initialMessage` submitted after runtime creation).
    std::optional<std::string> initial_prompt_{std::nullopt};
    PromptOptions initial_prompt_options_{};
    boost::asio::any_io_executor executor_;
    boost::asio::steady_timer exit_wait_;
    std::optional<EventSubscription> subscription_;
    /// Session-assembly event subscription (pi's `session.on(...)` for
    /// auto-retry and compaction events).
    std::optional<SessionEventSubscription> session_event_subscription_;
    /// SIGCONT/SIGINT registration while suspended (pi's suspend signal
    /// handlers); reset restores the previous handlers on resume.
    std::shared_ptr<boost::asio::signal_set> suspend_signals_;
    /// The active retry countdown (pi `CountdownTimer`); null while no retry
    /// backoff is pending.
    std::shared_ptr<RetryCountdown> retry_countdown_;
    /// Git branch source for the footer's pwd line (pi
    /// `FooterDataProvider` subset).
    FooterDataProvider footer_data_provider_{std::filesystem::path{}};
    InteractiveView* view_{nullptr}; // aliases the child owned by tui_.
    cch::tui::Overlay* active_overlay_{nullptr}; // aliases an overlay owned by tui_.
    std::atomic<bool> running_{false};
    std::atomic<bool> prompt_active_{false};
    std::atomic<bool> user_bash_active_{false};
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
    std::vector<std::string> displayed_agent_diagnostics_;
    bool tui_started_{false};
    bool exit_requested_{false};
    bool clipboard_read_active_{false};
    std::optional<util::ExpectedVoid> completion_result_;
};

} // namespace

boost::asio::awaitable<util::ExpectedVoid> run_interactive_mode(
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
    co_return state->finish();
}

boost::asio::awaitable<util::ExpectedVoid> run_interactive_mode_boot(
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
    co_return state->finish();
}

} // namespace cch::coding_agent::tui
