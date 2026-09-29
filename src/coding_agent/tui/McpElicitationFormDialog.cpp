#include "coding_agent/tui/McpElicitationFormDialog.hpp"

#include "DynamicBorder.hpp"
#include "KeybindingHints.hpp"
#include "Theme.hpp"

#include <cch/coding_agent/McpElicitation.hpp>
#include <cch/support/Error.hpp>
#include <cch/tui/Text.hpp>

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::coding_agent::tui {
namespace {

using support::JsonValue;

/// The fields move with these, in addition to the tab key. The `Input` handles
/// left and right, so the vertical keys are free to mean "another field", and
/// they are the keys a user reaches for first in a list of things.
constexpr std::string_view kNextFieldAction{"tui.input.tab"};
constexpr std::string_view kNextFieldActionDown{"tui.select.down"};
constexpr std::string_view kPreviousFieldAction{"tui.select.up"};
/// The keys the hint line names, spelled as the terminals deliver them.
constexpr std::string_view kNextFieldKey{"tab"};
constexpr std::string_view kPreviousFieldKey{"shift+tab"};

[[nodiscard]] bool is_previous_field_key(const cch::tui::KeyEvent& key) {
    return key.shift && !key.ctrl && !key.alt && key.key == "tab";
}

/// The marker that tells the user this field cannot be left empty, and the
/// hint that tells them what a boolean answers as. Both are on the label,
/// because a field whose only constraint is invisible is a field the user
/// gets wrong.
[[nodiscard]] std::string field_marker(const McpElicitationField& field) {
    std::string marker;
    if (field.required) {
        marker = " (required)";
    }
    if (field.type == "boolean") {
        marker += " (true or false)";
    }
    return marker;
}

/// What the field offers, for a field whose description does not already say:
/// the `enum` choices, which are the field's whole constraint.
[[nodiscard]] std::string field_choices(const McpElicitationField& field) {
    std::string choices = "one of: ";
    bool first{true};
    for (const auto& choice : field.enum_values) {
        if (choice.empty()) {
            continue;
        }
        if (!first) {
            choices += ", ";
        }
        choices += choice;
        first = false;
    }
    return choices;
}

} // namespace

McpElicitationFormDialog::McpElicitationFormDialog(const LiveTheme& theme,
        std::shared_ptr<const cch::tui::KeybindingRegistry> keybindings,
        McpPendingElicitation request,
        McpElicitationAnswerSink on_answer,
        McpElicitationInvalidateSink on_invalidate)
    : theme_(theme), keybindings_(std::move(keybindings)), request_(std::move(request)),
      on_answer_(std::move(on_answer)), on_invalidate_(std::move(on_invalidate)),
      form_(read_form_schema(request_.form_schema)),
      input_(cch::tui::InputOptions{.keybindings = keybindings_}) {
    values_.assign(form_.fields.size(), std::string{});
    errors_.assign(form_.fields.size(), std::string{});
}

std::string McpElicitationFormDialog::server_id() const {
    const std::scoped_lock lock(mutex_);
    return request_.server_id;
}

std::optional<std::size_t> McpElicitationFormDialog::focused_field() const {
    const std::scoped_lock lock(mutex_);
    if (form_.fields.empty()) {
        return std::nullopt;
    }
    return focused_field_;
}

std::string McpElicitationFormDialog::field_value(std::size_t index) const {
    const std::scoped_lock lock(mutex_);
    return index < values_.size() ? values_[index] : std::string{};
}

std::string McpElicitationFormDialog::field_error(std::size_t index) const {
    const std::scoped_lock lock(mutex_);
    return index < errors_.size() ? errors_[index] : std::string{};
}

bool McpElicitationFormDialog::live() const {
    const std::scoped_lock lock(mutex_);
    return !settled_;
}

void McpElicitationFormDialog::withdraw() { retire(std::nullopt, {}); }

void McpElicitationFormDialog::settle(McpElicitationAction action, McpElicitationFormValues values) {
    retire(action, std::move(values));
}

void McpElicitationFormDialog::retire(
        std::optional<McpElicitationAction> action, McpElicitationFormValues values) {
    std::string elicitation_id;
    McpElicitationAnswerSink sink;
    {
        const std::scoped_lock lock(mutex_);
        if (settled_) {
            return; // first settlement wins: a keypress racing a close is harmless
        }
        settled_ = true;
        elicitation_id = request_.elicitation_id;
        // A withdrawal drops the sinks as well: a dialog that is gone must not
        // be able to answer a call being torn down.
        sink = action.has_value() && on_answer_ ? std::move(on_answer_) : McpElicitationAnswerSink{};
    }
    // The sink runs outside the lock: it answers the session, which posts back
    // into the very loop that took the lock.
    if (sink) {
        sink(*action, std::move(elicitation_id), std::move(values));
    }
    if (on_invalidate_) {
        on_invalidate_();
    }
}

void McpElicitationFormDialog::store_focused() {
    if (focused_field_ < values_.size()) {
        values_[focused_field_] = input_.value();
    }
}

void McpElicitationFormDialog::move_focus(int delta) {
    if (form_.fields.empty()) {
        return;
    }
    store_focused();
    const auto count = static_cast<int>(form_.fields.size());
    const auto next = (static_cast<int>(focused_field_) + delta + count) % count;
    focused_field_ = static_cast<std::size_t>(next);
    // The one `Input` now holds the newly focused field's value. Its cursor
    // goes to the end: a field the user is arriving at for the first time has
    // nothing to be in the middle of, and a value they came back to is
    // appended to, which is the only edit a form this size needs.
    input_.set_value(values_[focused_field_]);
    input_.move_cursor_to_end();
}

std::optional<McpElicitationFormValues> McpElicitationFormDialog::submit() {
    store_focused();
    McpElicitationFormValues values;
    std::optional<std::size_t> first_invalid;
    for (std::size_t index = 0; index < form_.fields.size(); ++index) {
        const auto coerced = coerce_field(form_.fields[index], values_[index]);
        errors_[index] = coerced.outcome == McpFieldOutcome::Invalid ? coerced.error : std::string{};
        if (coerced.outcome == McpFieldOutcome::Value) {
            values.emplace(form_.fields[index].name, coerced.value);
        } else if (coerced.outcome == McpFieldOutcome::Invalid && !first_invalid.has_value()) {
            first_invalid = index;
        }
    }
    if (!first_invalid.has_value()) {
        return values;
    }
    // The dialog stays up and the offending field takes focus: an answer
    // carrying a value the schema rejects is worse than no answer, because
    // the Upstream would have to guess what the user meant.
    const auto invalid = *first_invalid;
    if (invalid != focused_field_) {
        focused_field_ = invalid;
        input_.set_value(values_[invalid]);
        input_.move_cursor_to_end();
    }
    return std::nullopt;
}

cch::tui::InputAdmissionOutcome McpElicitationFormDialog::handle_input(
        const cch::tui::InputEventVariant& input) {
    if (const auto* paste = std::get_if<cch::tui::PasteEvent>(&input); paste != nullptr) {
        // Pasted text edits the focused field, the way it edits the login
        // dialog's prompt: the `Input` owns insertion, this dialog owns which
        // field is being inserted into.
        const std::scoped_lock lock(mutex_);
        if (settled_) {
            return cch::tui::InputAdmissionOutcome::Consumed;
        }
        static_cast<void>(input_.handle_input(input));
        store_focused();
        return cch::tui::InputAdmissionOutcome::Consumed;
    }
    const auto* key = std::get_if<cch::tui::KeyEvent>(&input);
    if (key == nullptr || !cch::tui::carries_press_behavior(key)) {
        return cch::tui::InputAdmissionOutcome::Unhandled;
    }
    std::optional<std::pair<McpElicitationAction, McpElicitationFormValues>> outcome;
    {
        const std::scoped_lock lock(mutex_);
        if (settled_) {
            return cch::tui::InputAdmissionOutcome::Consumed;
        }
        // A decline and a cancel carry no values: the user is refusing, and
        // the text they happened to have typed is not part of that.
        if (mcp_elicitation_control_matched(*key, *keybindings_, McpElicitationControl::Decline)) {
            outcome = std::pair{McpElicitationAction::Decline, McpElicitationFormValues{}};
        } else if (mcp_elicitation_control_matched(*key, *keybindings_, McpElicitationControl::Cancel)) {
            outcome = std::pair{McpElicitationAction::Cancel, McpElicitationFormValues{}};
        } else if (mcp_elicitation_control_matched(*key, *keybindings_, McpElicitationControl::Done) ||
                   keybindings_->matches(*key, "tui.input.submit")) {
            // Submitting is the only way an answer is ever produced. It
            // validates rather than settling, so a form that cannot be
            // answered never leaves the dialog, and the settlement happens
            // after this lock is released.
            if (auto values = submit(); values) {
                outcome = std::pair{McpElicitationAction::Accept, std::move(*values)};
            }
        } else if (keybindings_->matches(*key, kNextFieldAction) ||
                   keybindings_->matches(*key, kNextFieldActionDown) ||
                   is_previous_field_key(*key) || keybindings_->matches(*key, kPreviousFieldAction)) {
            const bool forward = keybindings_->matches(*key, kNextFieldAction) ||
                                 keybindings_->matches(*key, kNextFieldActionDown);
            move_focus(forward ? 1 : -1);
        } else {
            // Everything else is the focused field's: printable characters,
            // cursor movement, the kill ring, undo, and the `Input`'s own
            // handling of all of them. The dialog reimplements none of it.
            static_cast<void>(input_.handle_input(*key));
            store_focused();
            // Typing into a field clears that field's error: the message
            // described the text that is no longer there.
            if (focused_field_ < errors_.size()) {
                errors_[focused_field_].clear();
            }
        }
    }
    if (outcome.has_value()) {
        settle(outcome->first, std::move(outcome->second));
        return cch::tui::InputAdmissionOutcome::Consumed;
    }
    if (on_invalidate_) {
        on_invalidate_();
    }
    return cch::tui::InputAdmissionOutcome::Consumed;
}

support::Expected<cch::tui::RenderResult> McpElicitationFormDialog::render(std::size_t width) {
    std::scoped_lock lock(mutex_);
    cch::tui::RenderResult result;
    const auto append_text = [&result, width](const std::string& content) -> support::ExpectedVoid {
        cch::tui::Text line(content, 1, 0);
        auto rendered = line.render(width);
        if (!rendered) {
            return std::unexpected(rendered.error());
        }
        for (auto& rendered_line : rendered->lines) {
            result.lines.push_back(std::move(rendered_line));
        }
        return {};
    };

    // pi's composition: border / accent title / content / border. The same
    // chrome as the URL dialog, so the two are recognizably one question type.
    DynamicBorder top_border(theme_.foreground_hook(ThemeToken::Border));
    auto rendered_border = top_border.render(width);
    if (!rendered_border) {
        return std::unexpected(rendered_border.error());
    }
    for (auto& line : rendered_border->lines) {
        result.lines.push_back(std::move(line));
    }
    if (auto appended = append_text(theme_.foreground(
                ThemeToken::Accent, "\x1b[1m" + std::string{"An Upstream is asking for input"} + "\x1b[22m"));
        !appended) {
        return std::unexpected(appended.error());
    }
    // The Server Id is on screen for the same reason the URL dialog names it:
    // the user is answering one upstream's question.
    if (auto appended = append_text(theme_.foreground(ThemeToken::Dim, "Upstream: " + request_.server_id));
        !appended) {
        return std::unexpected(appended.error());
    }
    if (!request_.tool_name.empty()) {
        if (auto appended = append_text(theme_.foreground(ThemeToken::Dim, "Tool: " + request_.tool_name));
            !appended) {
            return std::unexpected(appended.error());
        }
    }
    if (!request_.message.empty()) {
        if (auto appended = append_text(request_.message); !appended) {
            return std::unexpected(appended.error());
        }
    }
    if (form_.title) {
        if (auto appended = append_text(theme_.foreground(ThemeToken::Accent, *form_.title)); !appended) {
            return std::unexpected(appended.error());
        }
    }
    if (form_.description) {
        if (auto appended = append_text(theme_.foreground(ThemeToken::Dim, *form_.description)); !appended) {
            return std::unexpected(appended.error());
        }
    }
    // The form is short of what the Upstream asked for, and the user is told
    // so on screen: an answer silently missing a required field is the one
    // failure the user cannot see coming.
    if (const auto notice = form_.notice(); notice) {
        if (auto appended = append_text(theme_.foreground(ThemeToken::Warning, *notice)); !appended) {
            return std::unexpected(appended.error());
        }
    }

    cursor_cache_.reset();
    for (std::size_t index = 0; index < form_.fields.size(); ++index) {
        const auto& field = form_.fields[index];
        const bool is_focused = index == focused_field_;
        if (auto appended = append_text(field.title + field_marker(field)); !appended) {
            return std::unexpected(appended.error());
        }
        if (field.description) {
            if (auto appended = append_text(theme_.foreground(ThemeToken::Dim, *field.description)); !appended) {
                return std::unexpected(appended.error());
            }
        } else if (!field.enum_values.empty()) {
            if (auto appended = append_text(theme_.foreground(ThemeToken::Dim, field_choices(field))); !appended) {
                return std::unexpected(appended.error());
            }
        }
        if (is_focused) {
            // The focused field is the one `Input`, rendered on its own row
            // and scrolled by the `Input` itself, so a long value is one row
            // rather than a dialog that grows without bound. The render comes
            // first because it is what teaches the `Input` its layout width,
            // which is what its cursor row is measured against.
            auto rendered_input = input_.render(width);
            if (!rendered_input) {
                return std::unexpected(rendered_input.error());
            }
            if (focused_) {
                const auto cursor = input_.cursor_location();
                if (cursor) {
                    cursor_cache_ = cch::tui::CursorPosition{
                        .column = cursor->column,
                        .row = result.lines.size() + cursor->row,
                    };
                }
            }
            for (auto& line : rendered_input->lines) {
                result.lines.push_back(std::move(line));
            }
        } else {
            const auto text = values_[index].empty() ? std::string{"(not answered)"} : values_[index];
            if (auto appended = append_text(theme_.foreground(ThemeToken::Muted, field.title + ": " + text));
                !appended) {
                return std::unexpected(appended.error());
            }
        }
        if (!errors_[index].empty()) {
            if (auto appended = append_text(theme_.foreground(ThemeToken::Error, "  " + errors_[index]));
                !appended) {
                return std::unexpected(appended.error());
            }
        }
    }

    std::string controls = mcp_elicitation_control_hint(
                                   theme_, *keybindings_, McpElicitationControl::Done, "done") +
                           "  " + mcp_elicitation_control_hint(
                                          theme_, *keybindings_, McpElicitationControl::Decline, "decline") +
                           "  " + mcp_elicitation_control_hint(
                                          theme_, *keybindings_, McpElicitationControl::Cancel, "cancel");
    if (form_.fields.size() > 1) {
        controls += "  " + raw_key_hint(theme_, kNextFieldKey, "next field") + "  " +
                    raw_key_hint(theme_, kPreviousFieldKey, "previous field");
    }
    if (auto appended = append_text(controls); !appended) {
        return std::unexpected(appended.error());
    }

    DynamicBorder bottom_border(theme_.foreground_hook(ThemeToken::Border));
    auto rendered_bottom = bottom_border.render(width);
    if (!rendered_bottom) {
        return std::unexpected(rendered_bottom.error());
    }
    for (auto& line : rendered_bottom->lines) {
        result.lines.push_back(std::move(line));
    }
    return result;
}

void McpElicitationFormDialog::invalidate() {
    const std::scoped_lock lock(mutex_);
    input_.invalidate();
}

void McpElicitationFormDialog::set_focused(bool focused) {
    const std::scoped_lock lock(mutex_);
    focused_ = focused;
    // The `Input` has its own focus, and it is what reports the cursor: a
    // dialog that is not focused has no caret to park in a field.
    input_.set_focused(focused);
}

bool McpElicitationFormDialog::focused() const {
    const std::scoped_lock lock(mutex_);
    return focused_;
}

std::optional<cch::tui::CursorPosition> McpElicitationFormDialog::cursor_location() const {
    const std::scoped_lock lock(mutex_);
    return cursor_cache_;
}

} // namespace cch::coding_agent::tui
