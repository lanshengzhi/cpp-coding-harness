#include "InteractiveMode.hpp"

#include <cch/agent/AgentContext.hpp>
#include <cch/agent/AgentEvent.hpp>
#include <cch/ai/Auth.hpp>
#include <cch/ai/Content.hpp>
#include "coding_agent/AgentSession.hpp"
#include <cch/coding_agent/Settings.hpp>
#include <cch/coding_agent/ModelResolver.hpp>
#include <cch/tui/Autocomplete.hpp>
#include <cch/tui/Editor.hpp>
#include <cch/tui/Fuzzy.hpp>
#include <cch/tui/Loader.hpp>
#include <cch/tui/Overlay.hpp>
#include <cch/tui/Terminal.hpp>
#include <cch/tui/TruncatedText.hpp>
#include <cch/tui/Tui.hpp>

#include "coding_agent/BoundedText.hpp"
#include "coding_agent/CommandRegistry.hpp"
#include "coding_agent/ImageInput.hpp"
#include "coding_agent/prompt/SlashCommandParser.hpp"
#include "coding_agent/runtime/AgentSessionInteractiveAccess.hpp"
#include "coding_agent/tui/BashExecutionComponent.hpp"
#include "coding_agent/tui/ChatContainer.hpp"
#include "coding_agent/tui/KeybindingCatalog.hpp"
#include "coding_agent/tui/KeybindingHelp.hpp"
#include "coding_agent/tui/KeybindingHints.hpp"
#include "coding_agent/tui/LoginDialog.hpp"
#include "coding_agent/tui/LoginPresentation.hpp"
#include "coding_agent/tui/ModelSearch.hpp"
#include "coding_agent/tui/ModelSelector.hpp"
#include "coding_agent/tui/OAuthSelector.hpp"
#include "coding_agent/tui/OpenBrowser.hpp"
#include "coding_agent/tui/ScopedModelsSelector.hpp"
#include "coding_agent/tui/StringListSelector.hpp"
#include "coding_agent/tui/ThemeCatalog.hpp"
#include "util/UniqueFd.hpp"
#include "util/TerminalText.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
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
    std::vector<ThemeDiagnostic> themes;
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

[[nodiscard]] bool protocol_supports_image(
    cch::tui::InlineImageProtocol protocol,
    std::string_view mime_type) {
    if (protocol == cch::tui::InlineImageProtocol::Kitty) return mime_type == "image/png";
    if (protocol == cch::tui::InlineImageProtocol::ITerm2) {
        return mime_type == "image/png" || mime_type == "image/jpeg" ||
            mime_type == "image/gif" || mime_type == "image/webp";
    }
    return false;
}

[[nodiscard]] std::size_t estimated_image_rows(
    const cch::tui::InlineImageRenderRegion& image,
    const cch::tui::TerminalCapabilities& capabilities,
    std::size_t width) {
    if (!capabilities.cell_pixels || capabilities.cell_pixels->width == 0 ||
        capabilities.cell_pixels->height == 0 ||
        !protocol_supports_image(capabilities.inline_images, image.mime_type) ||
        image.pixel_width == 0 || image.pixel_height == 0 || image.region.column >= width) {
        return 1;
    }
    const auto available_width = width - image.region.column;
    const auto default_width = std::min<std::size_t>(width > 2 ? width - 2 : 1, 60);
    const auto max_width = std::max<std::size_t>(
        1,
        std::min(available_width, image.max_width.value_or(default_width)));
    const auto& cells = *capabilities.cell_pixels;
    const auto default_height = std::max<std::size_t>(
        1,
        (max_width * cells.width + cells.height - 1) / cells.height);
    const auto max_height = std::max<std::size_t>(
        1, image.max_height.value_or(default_height));
    const auto width_scale =
        static_cast<long double>(max_width * cells.width) / image.pixel_width;
    const auto height_scale =
        static_cast<long double>(max_height * cells.height) / image.pixel_height;
    const auto scale = std::min(width_scale, height_scale);
    return std::max<std::size_t>(
        1,
        std::min(max_height, static_cast<std::size_t>(std::ceil(
            static_cast<long double>(image.pixel_height) * scale / cells.height))));
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
[[nodiscard]] util::Error login_cancelled_error() {
    return util::make_error(util::ErrorCode::Cancelled, "Login cancelled");
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

/// Build the editor autocomplete command list: the registered slash commands
/// as plain items, prompt templates, and skills — plus the `model` command as
/// a `SlashCommand` whose argument completion resolves pi's `model-search`
/// text over the current candidate snapshot (scoped models when the session
/// carries a scope, else the availability snapshot).
[[nodiscard]] std::vector<std::variant<cch::tui::SlashCommand, cch::tui::AutocompleteItem>>
command_autocomplete_commands(
    const CommandRegistry& commands,
    std::span<const PromptTemplate> prompt_templates,
    std::span<const Skill> skills,
    std::shared_ptr<const ModelCompletionSnapshot> model_completion) {
    std::vector<std::variant<cch::tui::SlashCommand, cch::tui::AutocompleteItem>> items;
    std::set<std::string, std::less<>> names;
    for (const auto& command : commands.list_commands()) {
        std::string description = command.description;
        if (!command.argument_hint.empty()) {
            description = description.empty()
                ? command.argument_hint
                : std::format("{} — {}", command.argument_hint, description);
        }
        if (command.name == "model") {
            // pi `createBaseAutocompleteProvider`: `/model` argument
            // completion over `getModelSearchText`, value `provider/id`,
            // label `id`, description `provider`. The description stays
            // plain: the combined provider prepends the argument hint.
            cch::tui::SlashCommand slash;
            slash.name = command.name;
            slash.description = command.description;
            slash.argument_hint = command.argument_hint;
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
                .value = command.name,
                .label = command.name,
                .description = std::move(description),
            });
        }
        names.insert(command.name);
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
            .description = std::move(description),
        });
    }
    for (const auto& skill : skills) {
        auto name = "skill:" + skill.name;
        if (!names.insert(name).second) continue;
        items.push_back(cch::tui::AutocompleteItem{
            .value = name,
            .label = name,
            .description = skill.description,
        });
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
        std::shared_ptr<const cch::tui::KeybindingRegistry> keybindings,
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
        bool user_bash_available,
        std::unique_ptr<cch::tui::AutocompleteProvider> autocomplete_provider,
        std::unique_ptr<cch::tui::AutocompleteDebounceTimer> autocomplete_debounce_timer,
        cch::tui::EditorRenderRequestSink autocomplete_render_request,
        cch::tui::TerminalCapabilities terminal_capabilities,
        const LiveTheme& theme)
        : keybindings_(std::move(keybindings)),
          terminal_capabilities_(std::move(terminal_capabilities)),
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
          user_bash_available_(user_bash_available),
          header_(theme, *keybindings_, user_bash_available, on_clipboard_paste_ != nullptr),
          chat_(theme, *keybindings_),
          theme_(&theme),
          editor_(
              cch::tui::EditorOptions{
                  .keybindings = keybindings_,
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

    void append_status_message(std::string text) {
        std::lock_guard lock(mutex_);
        chat_.append_status_message(std::move(text));
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
                *keybindings_,
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

        // Header (keybinding hints), pending-messages, status, and footer
        // render first so the editor and autocomplete capacities account for
        // their fixed rows (pi's dock below the chat).
        std::vector<std::string> header_lines;
        if (auto header = header_.render(width); !header) {
            return std::unexpected(header.error());
        } else {
            header_lines = std::move(header->lines);
        }
        // Status and footer containers are part of the composition; their
        // content lands with the footer/status ticket (P15).
        std::vector<std::string> status_lines;
        std::vector<std::string> footer_lines;

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
            const auto hint = keybindings_->key_text("app.message.dequeue");
            cch::tui::TruncatedText item{std::format(
                "↳ {} to edit all queued messages",
                hint.empty() ? "Unbound" : format_key_text(hint, true))};
            if (auto rendered = item.render(width); !rendered) {
                return std::unexpected(rendered.error());
            } else if (!rendered->lines.empty()) {
                pending_lines.push_back(std::move(rendered->lines.front()));
            }
        }
        const auto fixed_rows =
            header_lines.size() + pending_lines.size() + status_lines.size() +
            footer_lines.size();

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
        // with `!` (where User Bash dispatch is available).
        editor_.set_theme(unsubmitted_bash_mode()
            ? cch::tui::EditorTheme{
                .text = theme_->foreground_hook(ThemeToken::BashMode),
            }
            : theme_->editor_theme());
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
        const auto chat_capacity = available_rows_ > fixed_rows + editor_lines.size() + autocomplete_lines.size()
            ? available_rows_ - fixed_rows - editor_lines.size() - autocomplete_lines.size()
            : 0;

        cch::tui::RenderResult chat_result;
        if (chat_capacity > 0) {
            auto rendered = chat_.render(width);
            if (!rendered) return std::unexpected(rendered.error());

            std::vector<std::size_t> materialized_rows(rendered->lines.size(), 1);
            for (const auto& image : rendered->images) {
                if (image.region.row >= materialized_rows.size()) continue;
                materialized_rows[image.region.row] +=
                    estimated_image_rows(image, terminal_capabilities_, width) - 1;
            }

            auto first_row = rendered->lines.size();
            std::size_t used_rows = 0;
            std::optional<std::size_t> fallback_only_row;
            while (first_row > 0) {
                const auto candidate_row = first_row - 1;
                const auto row_cost = materialized_rows[candidate_row];
                if (row_cost > chat_capacity - used_rows) {
                    if (used_rows < chat_capacity) {
                        first_row = candidate_row;
                        fallback_only_row = candidate_row;
                    }
                    break;
                }
                first_row = candidate_row;
                used_rows += row_cost;
            }
            chat_result.lines.assign(
                rendered->lines.begin() + static_cast<std::ptrdiff_t>(first_row),
                rendered->lines.end());
            for (auto& image : rendered->images) {
                const auto image_end = image.region.row + image.region.rows;
                if (image.region.row < first_row || image_end > rendered->lines.size() ||
                    (fallback_only_row && image.region.row == *fallback_only_row)) {
                    continue;
                }
                image.region.row -= first_row;
                chat_result.images.push_back(std::move(image));
            }
        }

        cch::tui::RenderResult transcript_result;
        transcript_result.lines = std::move(header_lines);
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
            if (keybindings_->matches(*key, "app.exit") && editor_.expanded_text().empty()) {
                invoke_action(on_exit_, "Native TUI exit callback failed");
                return;
            }
            const auto editor_cancels_interrupt =
                editor_.autocomplete_open() &&
                keybindings_->matches(*key, "tui.select.cancel");
            if (keybindings_->matches(*key, "app.interrupt") &&
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
            if (keybindings_->matches(*key, "app.message.followUp")) {
                invoke_follow_up();
                return;
            }
            if (keybindings_->matches(*key, "app.clipboard.pasteImage")) {
                invoke_action(
                    on_clipboard_paste_,
                    "Native TUI clipboard callback failed");
                return;
            }
            if (keybindings_->matches(*key, "app.message.dequeue")) {
                invoke_action(on_dequeue_, "Native TUI dequeue callback failed");
                return;
            }
            if (keybindings_->matches(*key, "app.clear")) {
                editor_.set_text({});
                return;
            }
            if (keybindings_->matches(*key, "app.tools.expand")) {
                chat_.toggle_tool_output();
                header_.set_expanded(chat_.tools_expanded());
                invoke_action(on_invalidate_, "Native TUI invalidation callback failed");
                return;
            }
            if (keybindings_->matches(*key, "app.thinking.toggle")) {
                chat_.toggle_thinking();
                invoke_action(on_invalidate_, "Native TUI invalidation callback failed");
                return;
            }
            // pi's main-editor `app.model.*` / `app.thinking.cycle` bindings:
            // the cycle actions and the model selector post to the executor
            // like every session-touching action.
            if (keybindings_->matches(*key, "app.model.cycleForward")) {
                invoke_action(on_cycle_model_forward_, "Native TUI model cycle callback failed");
                return;
            }
            if (keybindings_->matches(*key, "app.model.cycleBackward")) {
                invoke_action(on_cycle_model_backward_, "Native TUI model cycle callback failed");
                return;
            }
            if (keybindings_->matches(*key, "app.model.select")) {
                invoke_action(on_select_model_, "Native TUI model selector callback failed");
                return;
            }
            if (keybindings_->matches(*key, "app.thinking.cycle")) {
                invoke_action(on_cycle_thinking_, "Native TUI thinking cycle callback failed");
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
            cursor->row += editor_row_offset_;
            // The header/chat may overflow tiny terminals; keep the IME
            // cursor on the last visible row so rendering never fails.
            cursor->row = std::min(cursor->row, available_rows_ - 1);
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

    std::shared_ptr<const cch::tui::KeybindingRegistry> keybindings_;
    cch::tui::TerminalCapabilities terminal_capabilities_;
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
    bool user_bash_available_{false};
    std::optional<util::Error> callback_error_;
    mutable std::mutex mutex_;
    // pi's main-screen containers.
    KeybindingHints header_;
    ChatContainer chat_;
    const LiveTheme* theme_; // must outlive the view: controller-owned live theme.
    cch::tui::Editor editor_;
    std::size_t editor_revision_{0};
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
        AgentSession& session,
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
        if (config.open_browser_sink) {
            open_browser_sink_ = std::move(config.open_browser_sink);
        } else {
            open_browser_sink_ = [](std::string url) { open_browser(std::move(url)); };
        }
        update_model_completion();
        if (auto registered = register_commands(); !registered) {
            return fail_start(registered.error());
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
        if (auto subscribed = subscribe_to_session(weak); !subscribed) {
            return fail_start(subscribed.error());
        }

        tui_.set_render_request_sink([weak] {
            if (const auto self = weak.lock()) self->post_render();
        });
        if (auto started = tui_.start(); !started) return fail_start(started.error());
        tui_started_ = true;
        running_ = true;

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
        return {};
    }

    [[nodiscard]] boost::asio::steady_timer& exit_wait() {
        return exit_wait_;
    }

    [[nodiscard]] util::ExpectedVoid finish() {
        running_ = false;
        subscription_.reset();
        session_.close();
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
    [[nodiscard]] util::ExpectedVoid register_commands() {
        if (auto registered = register_builtin_commands(commands_); !registered) {
            return std::unexpected(registered.error());
        }
        return register_native_tui_commands(commands_);
    }

    [[nodiscard]] util::Expected<InteractiveStartupDiagnostics> load_startup_resources(
        const InteractiveModeConfig& config) {
        InteractiveStartupDiagnostics diagnostics;
        std::vector<std::string_view> actions{
            "app.interrupt",
            "app.clear",
            "app.exit",
            "app.tools.expand",
            "app.thinking.toggle",
            "app.thinking.cycle",
            "app.model.cycleForward",
            "app.model.cycleBackward",
            "app.model.select",
            "app.message.followUp",
            "app.message.dequeue",
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
        if (auto definitions = baseline_application_keybindings(actions, config.platform); !definitions) {
            return std::unexpected(definitions.error());
        } else {
            KeybindingCatalogRequest request;
            request.agent_config_directory = config.agent_config_directory;
            request.application_definitions = std::move(*definitions);
            request.platform = config.platform;
            if (auto catalog = load_keybinding_catalog(std::move(request)); !catalog) {
                return std::unexpected(catalog.error());
            } else {
                keybindings_ = catalog->registry;
                diagnostics.keybindings = std::move(catalog->diagnostics);
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
        const auto capabilities = terminal_.capabilities();
        ThemeCatalogRequest request;
        request.agent_config_directory = config.agent_config_directory;
        request.user_active_theme = settings_manager_->global_settings().theme;
        request.terminal_capabilities = capabilities;
        if (auto catalog = load_theme_catalog(std::move(request)); !catalog) {
            return std::unexpected(catalog.error());
        } else {
            diagnostics.themes = catalog->diagnostics;
            ThemeSelectionCommitter committer;
            if (!config.agent_config_directory.empty()) {
                committer = [manager = settings_manager_.has_value()
                                 ? &*settings_manager_
                                 : nullptr](std::string_view name) mutable {
                    return manager != nullptr
                        ? manager->set_theme(coding_agent::SettingsScope::Global, name)
                        : util::ExpectedVoid{};
                };
            }
            theme_controller_.emplace(
                std::move(*catalog),
                tui_,
                capabilities.color,
                std::move(committer));
        }
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
            detail::AgentSessionInteractiveAccess::has_user_shell(session_),
            std::make_unique<cch::tui::CombinedAutocompleteProvider>(
                [&] {
                    auto commands = command_autocomplete_commands(
                        commands_,
                        session_.templates(),
                        session_.skills(),
                        model_completion_);
                    return commands;
                }(),
                session_.workspace(),
                find_executable_on_path("fd")),
            std::make_unique<AsioAutocompleteDebounceTimer>(executor_),
            [weak] {
                if (const auto self = weak.lock()) self->post_invalidate();
            },
            terminal_.capabilities(),
            theme_controller_->live_theme());
    }

    [[nodiscard]] util::ExpectedVoid subscribe_to_session(
        std::weak_ptr<InteractiveState> weak) {
        if (auto subscribed = session_.subscribe(
                [weak](const agent::AgentLifecycleEvent& event) -> util::ExpectedVoid {
                    if (const auto self = weak.lock()) self->on_event(event);
                    return {};
                });
            !subscribed) {
            return std::unexpected(subscribed.error());
        } else {
            subscription_.emplace(std::move(*subscribed));
        }
        return {};
    }

    void initialize_view(const InteractiveStartupDiagnostics& diagnostics) {
        const auto snapshot = session_.snapshot();
        view_->initialize(snapshot);
        view_->set_pending_input(snapshot.agent_state.input_queues);
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
    }

    [[nodiscard]] util::ExpectedVoid fail_start(const util::Error& error) {
        running_ = false;
        session_.close();
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

    [[nodiscard]] CommandContext command_context() const {
        const auto& path = session_.session_path();
        return CommandContext{
            .session_id = session_.session_id(),
            .session_path = path
                ? std::optional<std::string>{path->string()}
                : std::nullopt,
            .workspace_path = session_.workspace().string(),
            .provider = session_.provider(),
            .model = session_.model(),
            .message_count = session_.message_count(),
            .available_commands = commands_.list_commands(),
            .user_bash_available =
                detail::AgentSessionInteractiveAccess::has_user_shell(session_),
        };
    }

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

    void open_settings() {
        if (active_overlay_ != nullptr || !theme_controller_ || !keybindings_) return;
        const auto weak = weak_from_this();
        if (auto overlay = make_theme_settings_overlay(
                *theme_controller_,
                keybindings_,
                [weak] {
                    if (const auto self = weak.lock()) self->post_close_overlay();
                });
            !overlay) {
            append_command_error(overlay.error());
        } else if (auto attached = attach_overlay(std::move(*overlay)); !attached) {
            append_command_error(attached.error());
        }
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
            make_hotkey_help_view(keybindings_),
            keybindings_,
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
        boost::asio::co_spawn(
            executor_,
            start(),
            [weak, failure_label = std::move(failure_label)](std::exception_ptr exception) {
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
        auto runtime = session_.model_runtime();
        if (!runtime) {
            model_completion_ = std::make_shared<const ModelCompletionSnapshot>();
            return;
        }
        auto snapshot = std::make_shared<ModelCompletionSnapshot>();
        const auto scoped = session_.scoped_models();
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

    /// pi `cycleModel` presentation: `Only one model in scope` / `Only one
    /// model available` when the cycle cannot move, otherwise the
    /// `Switched to <name> (thinking: <level>)` status; errors surface as
    /// `Error: <text>` lines.
    [[nodiscard]] boost::asio::awaitable<void> cycle_model(std::string direction) {
        auto result = co_await session_.cycle_model(std::move(direction));
        if (!result) {
            show_error(combined_error_text(result.error()));
            co_return;
        }
        if (!*result) {
            show_status(
                session_.scoped_models().empty() ? "Only one model available"
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
        auto level = session_.cycle_thinking_level();
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
        if (!running_ || view_ == nullptr || !session_.is_open() || !theme_controller_) return;
        const auto current_model = session_.snapshot().agent_state.model;
        const auto weak = weak_from_this();
        auto selector = std::make_shared<ModelSelectorComponent>(
            theme_controller_->live_theme(),
            keybindings_,
            &current_model,
            session_.model_runtime(),
            executor_,
            session_.scoped_models(),
            [weak](ai::Model model) {
                // Input-thread sink: post the session switch to the executor.
                if (const auto self = weak.lock()) {
                    self->post_from_view([model = std::move(model)](InteractiveState& state) mutable {
                        state.spawn_flow(
                            [state_self = state.shared_from_this(), model = std::move(model)]() mutable
                            -> boost::asio::awaitable<void> {
                                auto switched = co_await state_self->session_.set_model(std::move(model));
                                if (!switched) {
                                    state_self->show_error(combined_error_text(switched.error()));
                                    co_return;
                                }
                                state_self->update_model_completion();
                                state_self->restore_editor_slot();
                                state_self->show_status("Model: " + state_self->session_.snapshot().agent_state.model.id);
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
        const auto scoped = session_.scoped_models();
        if (!scoped.empty()) {
            candidates.reserve(scoped.size());
            for (const auto& entry : scoped) candidates.push_back(entry.model);
        } else {
            auto runtime = session_.model_runtime();
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
            auto switched = co_await session_.set_model(model);
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
        if (!running_ || view_ == nullptr || !session_.is_open() || !theme_controller_) co_return;
        auto runtime = session_.model_runtime();
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
        const auto& session_scoped_models = session_.scoped_models();
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
            keybindings_,
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
            session_.set_scoped_models(
                resolve_model_scope(*enabled_ids, all_models));
        } else {
            session_.set_scoped_models({});
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

    [[nodiscard]] OpenBrowserSink open_browser_hook() {
        const auto weak = weak_from_this();
        return [weak](std::string url) {
            if (const auto self = weak.lock(); self && self->open_browser_sink_) {
                self->open_browser_sink_(std::move(url));
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
        if (!running_ || view_ == nullptr || !session_.is_open()) return;
        const auto self = shared_from_this();
        spawn_flow(
            [self, provider_ref = std::move(provider_ref)]() mutable
                -> boost::asio::awaitable<void> {
                co_await self->handle_login_command(std::move(provider_ref));
            },
            "Native TUI login flow failed");
    }

    void open_logout() {
        if (!running_ || view_ == nullptr || !session_.is_open()) return;
        const auto self = shared_from_this();
        spawn_flow(
            [self]() -> boost::asio::awaitable<void> {
                co_await self->run_logout();
            },
            "Native TUI logout flow failed");
    }

    /// pi `handleLoginCommand`.
    [[nodiscard]] boost::asio::awaitable<void> handle_login_command(std::string provider_ref) {
        auto runtime = session_.model_runtime();
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
            keybindings_,
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
        auto runtime = session_.model_runtime();
        if (!runtime) return;
        auto options = compute_login_provider_options(*runtime, filter);
        if (options.empty()) {
            show_status(std::string{login_provider_selector_empty_message(filter)});
            return;
        }
        const auto weak = weak_from_this();
        auto selector = std::make_shared<OAuthSelectorComponent>(
            theme_controller_->live_theme(),
            keybindings_,
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
            keybindings_,
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
        auto runtime = session_.model_runtime();
        if (!runtime) co_return;
        const auto previous_model = session_.snapshot().agent_state.model;
        auto dialog = std::make_shared<LoginDialogComponent>(
            theme_controller_->live_theme(),
            keybindings_,
            "Login to " + provider_name,
            dialog_invalidate_hook(),
            open_browser_hook());
        place_editor_replacement(dialog);

        ai::AuthInteraction interaction;
        interaction.stop_token = dialog->stop_token();
        const auto self = shared_from_this();
        interaction.prompt = [self, dialog](ai::AuthPrompt prompt)
            -> boost::asio::awaitable<util::Expected<std::string>> {
            co_return co_await self->show_auth_prompt(dialog, std::move(prompt));
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
        auto runtime = session_.model_runtime();
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
                    auto set = co_await session_.set_model(*found);
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
            keybindings_,
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
                                slot->resolve(std::unexpected(login_cancelled_error()));
                            }
                        });
                }
            },
            [weak, slot, dialog] {
                if (const auto self = weak.lock()) {
                    self->post_from_view([slot, dialog](InteractiveState& self) mutable {
                        self.place_editor_replacement(dialog);
                        slot->resolve(std::unexpected(login_cancelled_error()));
                    });
                }
            });
        place_editor_replacement(std::move(selector));

        // pi's per-prompt race: an aborted per-prompt token rejects without
        // touching the slot's UI (the flow's unwind restores the editor).
        std::optional<std::stop_callback<std::move_only_function<void()>>> on_abort;
        if (per_prompt) {
            on_abort.emplace(*per_prompt, [slot] {
                slot->resolve(std::unexpected(login_cancelled_error()));
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
            co_return std::unexpected(login_cancelled_error());
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
        auto runtime = session_.model_runtime();
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
            keybindings_,
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
        auto runtime = session_.model_runtime();
        if (!runtime) co_return;
        if (auto logged_out = co_await runtime->logout(option.id); !logged_out) {
            show_error("Logout failed: " + combined_error_text(logged_out.error()));
            co_return;
        }
        // pi: updateAvailableProviderCount after logout.
        static_cast<void>(co_await runtime->get_available());
        show_status(logout_success_message(option.auth_type, option.name));
    }

    void apply_command_result(CommandResult result) {
        if (view_ != nullptr) view_->append_frontend_message(std::move(result.display_text));
        switch (result.effect) {
        case CommandEffect::None:
            tui_.invalidate();
            return;
        case CommandEffect::ClearScreen:
            if (auto cleared = tui_.clear_screen(); !cleared) {
                append_command_error(cleared.error());
            } else {
                if (view_ != nullptr) view_->clear_transcript();
                tui_.invalidate();
            }
            return;
        case CommandEffect::OpenSettings:
            open_settings();
            return;
        case CommandEffect::OpenHotkeys:
            open_hotkeys();
            return;
        case CommandEffect::OpenLogin:
            open_login(std::move(result.effect_argument));
            return;
        case CommandEffect::OpenLogout:
            open_logout();
            return;
        case CommandEffect::OpenModelSelector:
            post_open_model_selector(std::move(result.effect_argument));
            return;
        case CommandEffect::OpenScopedModelsSelector:
            post_open_scoped_models_selector();
            return;
        case CommandEffect::Shutdown:
            if (view_ != nullptr) {
                tui_.invalidate();
                render();
            }
            request_exit();
            return;
        }
    }

    [[nodiscard]] bool dispatch_command(std::string_view text) {
        const auto parsed = prompt::try_parse_slash_command(text);
        if (!parsed) return false;
        try {
            if (auto result = commands_.dispatch(
                    parsed->first,
                    command_context(),
                    parsed->second);
                !result) {
                return false;
            } else {
                apply_command_result(std::move(*result));
            }
            return true;
        } catch (const std::exception& error) {
            append_command_error(util::make_error(
                util::ErrorCode::Unknown,
                "Command handler failed",
                error.what()));
            return true;
        } catch (...) {
            append_command_error(util::make_error(
                util::ErrorCode::Unknown,
                "Command handler failed"));
            return true;
        }
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
        if (!detail::AgentSessionInteractiveAccess::has_user_shell(session_)) return false;
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
                    self->session_,
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
            session_.abort();
            return;
        case InterruptRoute::CancelUserBash:
            detail::AgentSessionInteractiveAccess::cancel_user_bash(session_);
            return;
        case InterruptRoute::ClearPendingBash:
            cleared_editor_revision_ = request.editor_revision;
            if (view_ != nullptr) {
                view_->clear_pending_bash(request);
                tui_.invalidate();
            }
            return;
        case InterruptRoute::None:
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
                if (auto admitted = session_.follow_up(text); !admitted) {
                    view_->restore_submitted_text(text);
                    view_->append_diagnostic(bounded_redacted_presentation(std::format(
                        "Unable to queue follow-up input: {}",
                        combined_error_text(admitted.error()))));
                }
            } else {
                if (auto admitted = session_.steer(text); !admitted) {
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
                    result = co_await self->session_.prompt(text, std::move(options));
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
        if (!running_ || view_ == nullptr || !session_.is_open()) return;
        const auto snapshot = session_.snapshot();
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
        if (auto cleared = session_.clear_input_queues(); !cleared) {
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
        if (!running_ || view_ == nullptr || !session_.is_open()) return;
        view_->set_pending_input(session_.snapshot().agent_state.input_queues);
    }

    void sync_session_observations() {
        if (!running_ || view_ == nullptr || !session_.is_open()) return;
        const auto snapshot = session_.snapshot();
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
        sync_session_observations();
        tui_.invalidate();
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
        session_.close();
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

    AgentSession& session_; // must outlive this interactive run.
    cch::tui::Terminal& terminal_; // must outlive this interactive run.
    cch::tui::Tui tui_;
    CommandRegistry commands_;
    std::shared_ptr<const cch::tui::KeybindingRegistry> keybindings_;
    /// Two-scope settings manager (global scope only; the project scope stays
    /// untrusted in the Native TUI). The theme committer and the
    /// scoped-models selector persist through it. Declared before
    /// `theme_controller_` so the controller's committer reference stays
    /// valid through destruction.
    std::optional<coding_agent::SettingsManager> settings_manager_{std::nullopt};
    /// Immutable `/model` completion snapshot, replaced on the executor by
    /// `update_model_completion()` whenever the candidate set changes.
    std::shared_ptr<const ModelCompletionSnapshot> model_completion_{};
    std::optional<ThemeController> theme_controller_;
    std::unique_ptr<AsyncClipboardReader> clipboard_reader_;
    std::move_only_function<void(std::string)> open_browser_sink_;
    std::optional<std::string> model_fallback_message_;
    boost::asio::any_io_executor executor_;
    boost::asio::steady_timer exit_wait_;
    std::optional<EventSubscription> subscription_;
    InteractiveView* view_{nullptr}; // aliases the child owned by tui_.
    cch::tui::Overlay* active_overlay_{nullptr}; // aliases an overlay owned by tui_.
    std::atomic<bool> running_{false};
    std::atomic<bool> prompt_active_{false};
    std::atomic<bool> user_bash_active_{false};
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
    auto state = std::make_shared<InteractiveState>(session, terminal, executor);
    if (auto started = state->start(std::move(config)); !started) {
        co_return std::unexpected(started.error());
    }

    boost::system::error_code wait_error;
    co_await state->exit_wait().async_wait(
        boost::asio::redirect_error(boost::asio::use_awaitable, wait_error));
    co_return state->finish();
}

} // namespace cch::coding_agent::tui
