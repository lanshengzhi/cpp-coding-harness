#include "ModelSelector.hpp"

#include "DynamicBorder.hpp"
#include "KeybindingHints.hpp"
#include "ModelSearch.hpp"
#include "Theme.hpp"

#include <cch/tui/Text.hpp>
#include <cch/tui/Utils.hpp>

#include <cch/support/Error.hpp>
#include "support/AsyncResultBridge.hpp"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/post.hpp>

#include <algorithm>
#include <exception>
#include <utility>

namespace cch::coding_agent::tui {
namespace {

/// The list window height mirrors the pre-migration selector (pi shows ten
/// rows); the SelectList centers the window on the selection and renders the
/// `  (n/m)` scroll indicator when clipped.
constexpr std::size_t kMaxVisible = 10;

} // namespace

ModelSelectorComponent::ModelSelectorComponent(const LiveTheme& theme,
        std::shared_ptr<const cch::tui::KeybindingRegistry> keybindings,
        const ai::Model* current_model,
        std::shared_ptr<cch::coding_agent::ModelRuntime> runtime,
        boost::asio::any_io_executor executor,
        std::vector<cch::coding_agent::ScopedModel> scoped_models,
        ModelSelectorSelectSink on_select,
        ModelSelectorCancelSink on_cancel,
        ModelSelectorInvalidateSink on_invalidate,
        std::optional<std::string> initial_search_input)
    : theme_(theme), keybindings_(std::move(keybindings)), runtime_(std::move(runtime)), executor_(std::move(executor)),
      on_select_(std::move(on_select)), on_cancel_(std::move(on_cancel)), on_invalidate_(std::move(on_invalidate)),
      // The list/search presentation lives in the shared SelectList; sinks
      // only re-enter this component (the SelectList outlives neither).
      select_list_(std::vector<cch::tui::SelectItem>{},
              cch::tui::SelectListOptions{
                      .max_visible = kMaxVisible,
                      .theme = theme_.select_list_theme(),
                      .on_select = [this](const cch::tui::SelectItem& item) -> support::ExpectedVoid {
                          confirm_selection(item);
                          return {};
                      },
                      .on_cancel = [this]() -> support::ExpectedVoid {
                          cancel_selection();
                          return {};
                      },
                      .keybindings = keybindings_,
                      .enable_search = true,
                      .initial_search = initial_search_input,
                      .no_match_text = "  No matching models",
              }) {
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

    // Render the current snapshot immediately; the background refresh starts
    // on the first render (shared_from_this is unavailable inside the
    // constructor, and the host may never render before destroying). Items
    // are pushed eagerly so navigation and search before the first render
    // act on the real model list; later rebuilds (refresh result, scope
    // toggle) land at render time through the revision gate below.
    load_models_from_snapshot();
    select_list_.set_items(build_select_items());
    if (!initial_search_input || initial_search_input->empty()) {
        for (std::size_t index = 0; index < active_models_.size(); ++index) {
            if (is_current(active_models_[index].model)) {
                select_list_.set_selected_index(index);
                break;
            }
        }
    }
    applied_items_revision_ = items_revision_;
}

ModelSelectorComponent::~ModelSelectorComponent() {
    std::lock_guard lock(mutex_);
    close();
}

void ModelSelectorComponent::load_models_from_snapshot() {
    // Callers hold the mutex (the constructor runs before publication).
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
    ++items_revision_;
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
        // The completion is fire-and-forget: refresh failures are diagnostic
        // observations, and the selector stays usable with the snapshot list.
        // Invalidate on both success and failure paths so the status/error
        // lines land (the non-null exception_ptr a detached handler could
        // receive is a Runtime invariant in the no-exception build).
        auto on_invalidate = std::move(self->on_invalidate_);
        boost::asio::co_spawn(
                self->executor_,
                [runtime, self, on_invalidate = std::move(on_invalidate)]() mutable -> boost::asio::awaitable<void> {
                    auto available = co_await support::detail::await_async_result(runtime->get_available());
                    {
                        std::lock_guard lock(self->mutex_);
                        if (self->closed_) co_return;
                        self->refresh_status_message_.clear();
                        if (!available) {
                            self->error_message_ = "Could not refresh model catalogs; showing cached models.";
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
                        // Re-sort/re-resolve against the fresh snapshot; the bumped
                        // revision pushes the rebuilt rows into the SelectList at the
                        // next render, re-applying the live query.
                        self->load_models_from_snapshot();
                    }
                    if (on_invalidate) {
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
                        try {
#endif
                            on_invalidate();
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
                        } catch (...) {
                        }
#endif
                    }
                },
                boost::asio::detached);
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
    ++items_revision_;
}

std::vector<cch::tui::SelectItem> ModelSelectorComponent::build_select_items() const {
    // Callers hold the mutex (or run in the ctor).
    std::vector<cch::tui::SelectItem> items;
    items.reserve(active_models_.size());
    for (const auto& item : active_models_) {
        const bool current = is_current(item.model);
        // The row text mirrors the pre-migration hand-rolled rows exactly
        // (`id [provider]`, with the current marker appended), including the
        // muted provider badge and success marker styling: SelectList renders
        // styled rows with the same byte footprint the TUI pads to a full
        // line width, so the composed screen stays cell-identical to the
        // legacy presentation.
        std::string label = item.id + " " + theme_.foreground(ThemeToken::Muted, "[" + item.provider + "]");
        if (current) label += theme_.foreground(ThemeToken::Success, " ✓");
        items.push_back(cch::tui::SelectItem{
                .value = item.provider + "/" + item.id,
                .label = std::move(label),
                .search_text = get_model_selector_search_text(ModelSearchItem{
                        .id = item.id,
                        .provider = item.provider,
                        .name = item.model.name.empty() ? std::nullopt : std::optional<std::string>{item.model.name},
                }),
        });
    }
    return items;
}

void ModelSelectorComponent::confirm_selection(const cch::tui::SelectItem& item) {
    ai::Model model;
    bool found = false;
    {
        std::lock_guard lock(mutex_);
        close();
        for (const auto& candidate : active_models_) {
            if (candidate.provider + "/" + candidate.id == item.value) {
                model = candidate.model;
                found = true;
                break;
            }
        }
    }
    if (found && on_select_) on_select_(std::move(model));
}

void ModelSelectorComponent::cancel_selection() {
    {
        std::lock_guard lock(mutex_);
        close();
    }
    if (on_cancel_) on_cancel_();
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

support::Expected<cch::tui::RenderResult> ModelSelectorComponent::render(std::size_t width) {
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
    const auto append = [&result, width](cch::tui::Component& component) -> support::ExpectedVoid {
        auto rendered = component.render(width);
        if (!rendered) return std::unexpected(rendered.error());
        for (auto& line : rendered->lines) result.lines.push_back(std::move(line));
        return {};
    };
    // Bounded emission for the component-owned rows under the list (mirrors
    // the pre-migration `updateList` tail). ANSI styling is preserved because
    // truncate_text operates over terminal tokens; a truncation failure is
    // propagated rather than falling back to the (potentially over-wide) line.
    const auto emit = [&result, width](std::string line) -> support::ExpectedVoid {
        auto truncated = cch::tui::truncate_text(std::move(line), width, "");
        if (!truncated) return std::unexpected(truncated.error());
        result.lines.push_back(std::move(*truncated));
        return {};
    };

    // The pending item set (refresh result or scope toggle) lands in the
    // SelectList before drawing: the rebuild re-ranks against the live query
    // and restores the selection by `provider/id`, so a refresh or scope
    // switch never drops the user's place.
    if (applied_items_revision_ != items_revision_) {
        select_list_.set_items(build_select_items());
        applied_items_revision_ = items_revision_;
    }

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

    // The SelectList renders its search row first (no chrome framing here),
    // directly followed by the item rows or the empty-state row. The rows
    // emitted above it fix the cursor offset for this render pass.
    cursor_row_offset_ = result.lines.size();
    {
        auto rendered = select_list_.render(width);
        if (!rendered) return std::unexpected(rendered.error());
        for (auto& line : rendered->lines)
            result.lines.push_back(std::move(line));
    }

    // pi `updateList` tail: diagnostics/status rows under the list, rendered
    // by this component because their presence changes as the background
    // refresh settles (SelectList title/hint are fixed at construction).
    const auto selection = select_list_.selected_item();
    if (error_message_) {
        // pi: the error renders in the error color, split over lines.
        std::size_t begin = 0;
        while (begin < error_message_->size()) {
            const auto newline = error_message_->find('\n', begin);
            const auto line_end = newline == std::string::npos ? error_message_->size() : newline;
            if (auto pushed =
                            emit(theme_.foreground(ThemeToken::Error, error_message_->substr(begin, line_end - begin)));
                    !pushed)
                return std::unexpected(pushed.error());
            if (newline == std::string::npos) break;
            begin = newline + 1;
        }
    } else if (selection) {
        // The selected model's name line renders below the list (pi).
        std::string name;
        for (const auto& item : active_models_) {
            if (item.provider + "/" + item.id == selection->value) {
                name = item.model.name;
                break;
            }
        }
        if (auto pushed = emit(""); !pushed) return std::unexpected(pushed.error());
        if (auto pushed = emit(theme_.foreground(ThemeToken::Muted, "  Model Name: " + name)); !pushed)
            return std::unexpected(pushed.error());
    }
    if (!refresh_status_message_.empty()) {
        if (auto pushed = emit(""); !pushed) return std::unexpected(pushed.error());
        if (auto pushed = emit(theme_.foreground(
                    refresh_status_success_ ? ThemeToken::Success : ThemeToken::Muted, "  " + refresh_status_message_));
                !pushed)
            return std::unexpected(pushed.error());
    }
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
    if (key != nullptr && key->type == cch::tui::KeyEventType::Release) return;

    if (key != nullptr && keybindings_->matches(*key, "tui.input.tab")) {
        // pi: the scope toggle only exists when scoped models are present,
        // and it pre-empts SelectList so the scope marker re-accents and the
        // item set rebuilds on the next render.
        std::lock_guard lock(mutex_);
        if (scoped_model_items_.empty()) return;
        set_scope(!scope_scoped_);
        return;
    }

    // Everything else — navigation, confirm/cancel, and search editing —
    // belongs to the SelectList.
    select_list_.handle_input(input);
}

void ModelSelectorComponent::set_focused(bool focused) {
    focused_ = focused;
    select_list_.set_focused(focused);
}

bool ModelSelectorComponent::focused() const {
    return focused_;
}

std::optional<cch::tui::CursorPosition> ModelSelectorComponent::cursor_location() const {
    const auto cursor = select_list_.cursor_location();
    if (!cursor) return std::nullopt;
    return cch::tui::CursorPosition{
            .column = cursor->column,
            .row = cursor->row + cursor_row_offset_,
    };
}

} // namespace cch::coding_agent::tui
