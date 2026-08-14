#include "ModelSelector.hpp"

#include "DynamicBorder.hpp"
#include "KeybindingHints.hpp"
#include "ModelSearch.hpp"
#include "Theme.hpp"

#include <cch/tui/Fuzzy.hpp>
#include <cch/tui/Text.hpp>
#include <cch/tui/Utils.hpp>

#include <cch/util/Error.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/post.hpp>

#include <algorithm>
#include <exception>
#include <iterator>
#include <utility>

namespace cch::coding_agent::tui {
namespace {

constexpr std::size_t kMaxVisible = 10;

[[nodiscard]] std::string provider_badge(const LiveTheme& theme, std::string_view provider) {
    return theme.foreground(ThemeToken::Muted, "[" + std::string{provider} + "]");
}

} // namespace

ModelSelectorComponent::ModelSelectorComponent(
    const LiveTheme& theme,
    std::shared_ptr<const cch::tui::KeybindingRegistry> keybindings,
    const ai::Model* current_model,
    std::shared_ptr<cch::coding_agent::ModelRuntime> runtime,
    boost::asio::any_io_executor executor,
    std::vector<cch::coding_agent::ScopedModel> scoped_models,
    ModelSelectorSelectSink on_select,
    ModelSelectorCancelSink on_cancel,
    ModelSelectorInvalidateSink on_invalidate,
    std::optional<std::string> initial_search_input)
    : theme_(theme),
      keybindings_(std::move(keybindings)),
      runtime_(std::move(runtime)),
      executor_(std::move(executor)),
      on_select_(std::move(on_select)),
      on_cancel_(std::move(on_cancel)),
      on_invalidate_(std::move(on_invalidate)),
      search_input_(cch::tui::InputOptions{.keybindings = keybindings_}) {
    if (current_model != nullptr) current_model_ = *current_model;
    for (auto& scoped : scoped_models) {
        scoped_model_items_.push_back(ModelItem{
            .provider = scoped.model.provider,
            .id = scoped.model.id,
            .model = std::move(scoped.model),
        });
    }
    // pi: the initial scope is "scoped" only when scoped models exist.
    scope_scoped_ = !scoped_model_items_.empty();
    if (initial_search_input) {
        search_input_.set_value(*initial_search_input);
    }

    // Render the current snapshot immediately; the background refresh starts
    // on the first render (shared_from_this is unavailable inside the
    // constructor, and the host may never render before destroying).
    load_models_from_snapshot();
    filter_models(search_input_.value());
}

ModelSelectorComponent::~ModelSelectorComponent() {
    std::lock_guard lock(mutex_);
    close();
}

void ModelSelectorComponent::load_models_from_snapshot() {
    const auto snapshot = runtime_ ? runtime_->get_available_snapshot() : std::vector<ai::Model>{};
    std::vector<ModelItem> all;
    all.reserve(snapshot.size());
    for (const auto& model : snapshot) {
        all.push_back(ModelItem{
            .provider = model.provider,
            .id = model.id,
            .model = model,
        });
    }
    // pi `sortModels`: current model first, then by provider.
    std::stable_sort(all.begin(), all.end(), [this](const ModelItem& left, const ModelItem& right) {
        const bool left_current = is_current(left.model);
        const bool right_current = is_current(right.model);
        if (left_current != right_current) return left_current;
        return left.provider < right.provider;
    });
    all_models_ = std::move(all);

    // pi: re-resolve the scoped models against the live runtime so refreshed
    // identity wins; entries that no longer exist stay as-is.
    for (auto& scoped : scoped_model_items_) {
        if (runtime_) {
            if (auto refreshed = runtime_->model(scoped.provider, scoped.id)) {
                scoped.model = *refreshed;
            }
        }
    }
    active_models_ = scope_scoped_ ? scoped_model_items_ : all_models_;
    const auto current_index = std::find_if(
        active_models_.begin(),
        active_models_.end(),
        [this](const ModelItem& item) { return is_current(item.model); });
    if (current_index != active_models_.end()) {
        selected_index_ = static_cast<std::size_t>(current_index - active_models_.begin());
    } else {
        selected_index_ = std::min(selected_index_, active_models_.empty() ? 0 : active_models_.size() - 1);
    }
}

void ModelSelectorComponent::start_refresh() {
    if (!runtime_) return;
    // Keep the component (and the runtime) alive through the refresh: the
    // editor slot may restore (destroying the last host reference) while the
    // availability refresh is still in flight. The spawn is posted so the
    // coroutine never starts inline under a held component mutex (co_spawn
    // uses dispatch semantics on the current executor).
    const auto self = shared_from_this();
    const auto runtime = runtime_;
    boost::asio::post(executor_, [self, runtime] {
        boost::asio::co_spawn(
            self->executor_,
            [runtime, self]() -> boost::asio::awaitable<void> {
                auto available = co_await runtime->get_available();
                std::lock_guard lock(self->mutex_);
                if (self->closed_) co_return;
                self->refresh_status_message_.clear();
                if (!available) {
                    self->error_message_ =
                        "Could not refresh model catalogs; showing cached models.";
                } else {
                    // pi: after a clean refresh, the runtime's own error
                    // (models.json diagnostics) still surfaces; otherwise the
                    // success status.
                    self->error_message_ = runtime->get_error();
                    if (!self->error_message_) {
                        self->refresh_status_message_ = "Model catalogs refreshed.";
                        self->refresh_status_success_ = true;
                    }
                }
                self->load_models_from_snapshot();
                self->filter_models(self->search_query_);
            },
            [self, on_invalidate = std::move(self->on_invalidate_)](std::exception_ptr) mutable {
                // A refresh failure is a diagnostic observation; the selector
                // stays usable with the snapshot list. Invalidate on both
                // paths so the status/error lines land.
                if (on_invalidate) {
                    try {
                        on_invalidate();
                    } catch (...) {
                    }
                }
                (void)self;
            });
    });
}

void ModelSelectorComponent::close() {
    closed_ = true;
}

void ModelSelectorComponent::set_scope(bool scoped) {
    // Callers hold the mutex (the constructor and the Tab handler).
    if (scope_scoped_ == scoped) return;
    scope_scoped_ = scoped;
    active_models_ = scoped ? scoped_model_items_ : all_models_;
    const auto current_index = std::find_if(
        active_models_.begin(),
        active_models_.end(),
        [this](const ModelItem& item) { return is_current(item.model); });
    selected_index_ = current_index != active_models_.end()
        ? static_cast<std::size_t>(current_index - active_models_.begin())
        : 0;
    filter_models(search_input_.value());
}

void ModelSelectorComponent::filter_models(std::string query) {
    search_query_ = query;
    // pi `filterModels`: fuzzy over the active scope; the query moves the
    // selection to the top row, clearing it clamps back to the list length.
    if (query.empty()) {
        filtered_models_ = active_models_;
        selected_index_ = std::min(selected_index_, filtered_models_.empty() ? 0 : filtered_models_.size() - 1);
    } else {
        filtered_models_ = cch::tui::fuzzy_filter(
            active_models_,
            query,
            [](const ModelItem& item) {
                return get_model_selector_search_text(ModelSearchItem{
                    .id = item.id,
                    .provider = item.provider,
                    .name = item.model.name.empty() ? std::nullopt : std::optional<std::string>{item.model.name},
                });
            });
        selected_index_ = 0;
    }
}

bool ModelSelectorComponent::is_current(const ai::Model& model) const {
    return current_model_.has_value() &&
        current_model_->provider == model.provider &&
        current_model_->id == model.id;
}

std::string ModelSelectorComponent::scope_text() const {
    // pi: the active scope renders accent, the inactive one muted.
    const auto all_label = scope_scoped_
        ? theme_.foreground(ThemeToken::Muted, "all")
        : theme_.foreground(ThemeToken::Accent, "all");
    const auto scoped_label = scope_scoped_
        ? theme_.foreground(ThemeToken::Accent, "scoped")
        : theme_.foreground(ThemeToken::Muted, "scoped");
    return theme_.foreground(ThemeToken::Muted, "Scope: ") + all_label +
        theme_.foreground(ThemeToken::Muted, " | ") + scoped_label;
}

std::string ModelSelectorComponent::scope_hint_text() const {
    return key_hint(theme_, *keybindings_, "tui.input.tab", "scope") +
        theme_.foreground(ThemeToken::Muted, " (all/scoped)");
}

util::ExpectedVoid ModelSelectorComponent::update_list(std::vector<std::string>& out_lines, std::size_t width) const {
    // pi `updateList`: the visible slice centers the selection. Every emitted
    // line is bounded to the render width (the SessionSelector pattern): the
    // render path asserts each line's visible width <= the bound, so a single
    // untruncated line would abort the TUI. ANSI styling is preserved because
    // truncate_text operates over terminal tokens; a truncation failure is
    // propagated rather than falling back to the (potentially over-wide) line.
    std::vector<std::string> lines;
    const auto emit = [&lines, width](std::string line) -> util::ExpectedVoid {
        auto truncated = cch::tui::truncate_text(line, width, "");
        if (!truncated) return std::unexpected(truncated.error());
        lines.push_back(std::move(*truncated));
        return {};
    };

    const std::size_t count = filtered_models_.size();
    const std::size_t start = count <= kMaxVisible
        ? 0
        : std::min(selected_index_ > kMaxVisible / 2 ? selected_index_ - kMaxVisible / 2 : 0, count - kMaxVisible);
    const std::size_t end = std::min(start + kMaxVisible, count);

    for (std::size_t index = start; index < end; ++index) {
        const auto& item = filtered_models_[index];
        const bool selected = index == selected_index_;
        const bool current = is_current(item.model);
        std::string line;
        if (selected) {
            line = theme_.foreground(ThemeToken::Accent, "→ ") +
                theme_.foreground(ThemeToken::Accent, item.id) + " " +
                provider_badge(theme_, item.provider);
        } else {
            line = "  " + item.id + " " + provider_badge(theme_, item.provider);
        }
        if (current) {
            line += theme_.foreground(ThemeToken::Success, " ✓");
        }
        if (auto pushed = emit(std::move(line)); !pushed) return std::unexpected(pushed.error());
    }

    if (start > 0 || end < count) {
        if (auto pushed = emit(theme_.foreground(
                ThemeToken::Muted,
                "  (" + std::to_string(selected_index_ + 1) + "/" + std::to_string(count) + ")"));
            !pushed) return std::unexpected(pushed.error());
    }

    if (error_message_) {
        // pi: the error renders in the error color, split over lines.
        std::size_t begin = 0;
        while (begin < error_message_->size()) {
            const auto newline = error_message_->find('\n', begin);
            const auto line_end = newline == std::string::npos ? error_message_->size() : newline;
            if (auto pushed = emit(theme_.foreground(
                    ThemeToken::Error,
                    error_message_->substr(begin, line_end - begin)));
                !pushed) return std::unexpected(pushed.error());
            if (newline == std::string::npos) break;
            begin = newline + 1;
        }
    } else if (count == 0) {
        if (auto pushed = emit(theme_.foreground(ThemeToken::Muted, "  No matching models"));
            !pushed) return std::unexpected(pushed.error());
    } else {
        const auto& selected = filtered_models_[selected_index_];
        if (auto pushed = emit(""); !pushed) return std::unexpected(pushed.error());
        if (auto pushed = emit(theme_.foreground(
                ThemeToken::Muted,
                "  Model Name: " + selected.model.name));
            !pushed) return std::unexpected(pushed.error());
    }
    if (!refresh_status_message_.empty()) {
        if (auto pushed = emit(""); !pushed) return std::unexpected(pushed.error());
        if (auto pushed = emit(theme_.foreground(
                refresh_status_success_ ? ThemeToken::Success : ThemeToken::Muted,
                "  " + refresh_status_message_));
            !pushed) return std::unexpected(pushed.error());
    }
    out_lines.insert(
        out_lines.end(),
        std::make_move_iterator(lines.begin()),
        std::make_move_iterator(lines.end()));
    return {};
}

util::Expected<cch::tui::RenderResult> ModelSelectorComponent::render(std::size_t width) {
    bool start_refresh_now = false;
    {
        std::lock_guard lock(mutex_);
        if (!refresh_started_ && runtime_) {
            // Start the background availability refresh exactly once, outside
            // the lock below.
            refresh_started_ = true;
            start_refresh_now = true;
        }
    }
    if (start_refresh_now) start_refresh();
    std::lock_guard lock(mutex_);
    cch::tui::RenderResult result;
    const auto append = [&result, width](cch::tui::Component& component) -> util::ExpectedVoid {
        auto rendered = component.render(width);
        if (!rendered) return std::unexpected(rendered.error());
        for (auto& line : rendered->lines) result.lines.push_back(std::move(line));
        return {};
    };

    DynamicBorder top_border(theme_.foreground_hook(ThemeToken::Border));
    if (auto appended = append(top_border); !appended) return std::unexpected(appended.error());
    {
        cch::tui::Text spacer("", 1, 0);
        if (auto appended = append(spacer); !appended) return std::unexpected(appended.error());
    }
    if (scoped_model_items_.empty()) {
        cch::tui::Text hint(
            theme_.foreground(
                ThemeToken::Warning,
                "Only showing models from configured providers. Use /login to add providers."),
            1, 0);
        if (auto appended = append(hint); !appended) return std::unexpected(appended.error());
    } else {
        cch::tui::Text scope(scope_text(), 1, 0);
        if (auto appended = append(scope); !appended) return std::unexpected(appended.error());
        cch::tui::Text hint(scope_hint_text(), 1, 0);
        if (auto appended = append(hint); !appended) return std::unexpected(appended.error());
    }
    {
        cch::tui::Text spacer("", 1, 0);
        if (auto appended = append(spacer); !appended) return std::unexpected(appended.error());
    }
    if (auto appended = append(search_input_); !appended) return std::unexpected(appended.error());
    {
        cch::tui::Text spacer("", 1, 0);
        if (auto appended = append(spacer); !appended) return std::unexpected(appended.error());
    }
    if (auto updated = update_list(result.lines, width); !updated) return std::unexpected(updated.error());
    {
        cch::tui::Text spacer("", 1, 0);
        if (auto appended = append(spacer); !appended) return std::unexpected(appended.error());
    }
    DynamicBorder bottom_border(theme_.foreground_hook(ThemeToken::Border));
    if (auto appended = append(bottom_border); !appended) return std::unexpected(appended.error());
    return result;
}

void ModelSelectorComponent::handle_input(const cch::tui::InputEventVariant& input) {
    const auto* key = std::get_if<cch::tui::KeyEvent>(&input);
    if (key == nullptr || key->type == cch::tui::KeyEventType::Release) return;

    if (keybindings_->matches(*key, "tui.input.tab")) {
        // pi: the scope toggle only exists when scoped models are present.
        std::lock_guard lock(mutex_);
        if (scoped_model_items_.empty()) return;
        set_scope(!scope_scoped_);
        return;
    }
    if (keybindings_->matches(*key, "tui.select.up")) {
        std::lock_guard lock(mutex_);
        if (filtered_models_.empty()) return;
        selected_index_ = selected_index_ == 0 ? filtered_models_.size() - 1 : selected_index_ - 1;
        return;
    }
    if (keybindings_->matches(*key, "tui.select.down")) {
        std::lock_guard lock(mutex_);
        if (filtered_models_.empty()) return;
        selected_index_ = selected_index_ == filtered_models_.size() - 1 ? 0 : selected_index_ + 1;
        return;
    }
    if (keybindings_->matches(*key, "tui.select.confirm")) {
        ai::Model selected;
        {
            std::lock_guard lock(mutex_);
            if (selected_index_ >= filtered_models_.size()) return;
            selected = filtered_models_[selected_index_].model;
            close();
        }
        if (on_select_) on_select_(std::move(selected));
        return;
    }
    if (keybindings_->matches(*key, "tui.select.cancel")) {
        close();
        if (on_cancel_) on_cancel_();
        return;
    }

    // Everything else goes to the search input (pi: pass-through + refilter).
    search_input_.handle_input(input);
    std::lock_guard lock(mutex_);
    filter_models(search_input_.value());
}

void ModelSelectorComponent::set_focused(bool focused) {
    focused_ = focused;
    search_input_.set_focused(focused);
}

bool ModelSelectorComponent::focused() const {
    return focused_;
}

std::optional<cch::tui::CursorPosition> ModelSelectorComponent::cursor_location() const {
    return search_input_.cursor_location();
}

} // namespace cch::coding_agent::tui
