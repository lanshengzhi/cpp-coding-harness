#include "coding_agent/tui/ChatViewport.hpp"

#include <cch/tui/Stack.hpp>
#include <cch/tui/VStack.hpp>

#include <utility>

namespace cch::coding_agent::tui {
class ChatViewport::EditorSlot final : public cch::tui::Component {
public:
    explicit EditorSlot(std::unique_ptr<cch::tui::Component> editor) : editor_(std::move(editor)) {}

    [[nodiscard]] support::Expected<cch::tui::RenderResult> render(std::size_t width) override {
        if (!editor_) return cch::tui::RenderResult{};
        return editor_->render(width);
    }

    void invalidate() override {
        if (editor_) editor_->invalidate();
    }

    [[nodiscard]] std::optional<cch::tui::CursorPosition> cursor_location() const override {
        return editor_ ? editor_->cursor_location() : std::nullopt;
    }

    [[nodiscard]] bool contains(const cch::tui::Component* target) const override {
        return this == target || (editor_ && editor_->contains(target));
    }

    [[nodiscard]] std::unique_ptr<cch::tui::Component> take() { return std::move(editor_); }

    [[nodiscard]] support::ExpectedVoid set(std::unique_ptr<cch::tui::Component> editor) {
        if (!editor) {
            return std::unexpected(support::make_error(
                    support::ErrorCode::Validation, "Chat viewport editor cannot be null"));
        }
        if (editor_) {
            return std::unexpected(support::make_error(
                    support::ErrorCode::Validation, "Chat viewport editor slot is already occupied"));
        }
        editor_ = std::move(editor);
        return {};
    }

private:
    std::unique_ptr<cch::tui::Component> editor_;
};

std::unique_ptr<cch::tui::Component> ChatViewport::take_editor() {
    if (!editor_slot_) return nullptr;
    return editor_slot_->take();
}

support::ExpectedVoid ChatViewport::set_editor(std::unique_ptr<cch::tui::Component> editor) {
    if (!editor_slot_) {
        return std::unexpected(support::make_error(
                support::ErrorCode::Validation, "Chat viewport has no editor slot"));
    }
    if (auto restored = editor_slot_->set(std::move(editor)); !restored) return restored;
    return {};
}

namespace {

/// The frozen dock entry for a non-editor region (`{ shrink: 1, minSize: 0 }`).
[[nodiscard]] constexpr cch::tui::StackEntryOptions dock_entry_options(std::size_t min_size) {
    return cch::tui::StackEntryOptions{.shrink = 1, .min_size = min_size};
}

/// Attach one dock region; a missing entry is the toolkit's null-child
/// Validation error.
[[nodiscard]] support::ExpectedVoid add_dock_region(
        cch::tui::VStack& dock, std::unique_ptr<cch::tui::Component> region, std::size_t min_size) {
    if (auto added = dock.add_child(std::move(region), dock_entry_options(min_size)); !added) {
        return std::unexpected(added.error());
    }
    return {};
}

} // namespace

support::Expected<ChatViewport> create_chat_viewport(ChatViewportOptions options) {
    // `editor: { shrink: 1, minSize: 3 }` is the one dock entry with a floor:
    // the prompt keeps three rows (pi's bordered editor) while the dock
    // shrinks.
    constexpr std::size_t kEditorMinRows = 3;

    // The scroll viewport owns exactly one child and is unrepresentable
    // without it (`ScrollView` terminates on a null child), so the document is
    // rejected before anything is constructed. Every other region is a plain
    // stack entry whose absence the toolkit already reports the same way.
    if (options.document == nullptr) {
        return std::unexpected(
                support::make_error(support::ErrorCode::Validation, "Chat viewport requires a document component"));
    }

    cch::tui::ScrollViewOptions transcript_options{
            .follow_end = true,
            .primary = true,
            .overscroll = cch::tui::ScrollOverscroll::Chain,
            .scrollbar = options.scrollbar,
            .scrollbar_track_style = std::move(options.scrollbar_track_style),
            .scrollbar_thumb_style = std::move(options.scrollbar_thumb_style),
    };
    auto transcript =
            std::make_unique<cch::tui::ScrollView>(std::move(options.document), std::move(transcript_options));
    auto* transcript_pointer = transcript.get();

    auto dock = std::make_unique<cch::tui::VStack>();
    ChatViewport result;
    if (auto added = add_dock_region(*dock, std::move(options.pending_messages), 0); !added) {
        return std::unexpected(added.error());
    }
    if (auto added = add_dock_region(*dock, std::move(options.status), 0); !added) {
        return std::unexpected(added.error());
    }
    if (options.widgets_above != nullptr) {
        if (auto added = add_dock_region(*dock, std::move(options.widgets_above), 0); !added) {
            return std::unexpected(added.error());
        }
    }
    auto editor_slot = std::make_unique<ChatViewport::EditorSlot>(std::move(options.editor));
    result.editor_slot_ = editor_slot.get();
    if (auto added = add_dock_region(*dock, std::move(editor_slot), kEditorMinRows); !added) {
        return std::unexpected(added.error());
    }
    if (options.widgets_below != nullptr) {
        if (auto added = add_dock_region(*dock, std::move(options.widgets_below), 0); !added) {
            return std::unexpected(added.error());
        }
    }
    if (auto added = add_dock_region(*dock, std::move(options.footer), 0); !added) {
        return std::unexpected(added.error());
    }

    auto root = std::make_unique<cch::tui::VStack>();
    // The transcript takes the flexible space (`basis 0, grow 1`) but never
    // shrinks below one row; the dock keeps its measured height (`basis auto,
    // grow 0`) and yields only when the frame is short (`shrink 1`).
    if (auto added = root->add_child(
                std::move(transcript), cch::tui::StackEntryOptions{.basis = 0, .grow = 1, .shrink = 1, .min_size = 1});
            !added) {
        return std::unexpected(added.error());
    }
    if (auto added = root->add_child(
                std::move(dock), cch::tui::StackEntryOptions{.grow = 0, .shrink = 1, .min_size = 1});
            !added) {
        return std::unexpected(added.error());
    }

    result.root = std::move(root);
    result.transcript = transcript_pointer;
    return result;
}

} // namespace cch::coding_agent::tui
