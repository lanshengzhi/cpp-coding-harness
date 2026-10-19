#pragma once

// The product's two-region viewport (pi
// `packages/coding-agent/src/modes/interactive/chat-viewport.ts`,
// `createChatViewport`): the transcript document inside one scroll viewport,
// over a fixed dock of pending messages, status, widgets, editor and footer.
// The engine installs the returned root as the `AltScreen` layout root in
// Fullscreen mode (#1036); Regular mode keeps the main-screen composition and
// never calls this.
//
// Repository-private `cch_coding_agent` implementation header: not part of an
// Owner Interface, not installed, never exported.

#include <cch/tui/Component.hpp>
#include <cch/tui/ScrollView.hpp>
#include <cch/tui/VStack.hpp>

#include <cch/support/Error.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace cch::coding_agent::tui {

/// The entries `create_chat_viewport` assembles. Every unique pointer transfers
/// ownership into the returned root, so a caller that must keep driving a
/// region (the editor the engine focuses) keeps that instance addressable
/// instead of moving it away.
struct ChatViewportOptions {
    /// The scrollable transcript document: header, loaded resources and chat.
    std::unique_ptr<cch::tui::Component> document;
    std::unique_ptr<cch::tui::Component> pending_messages;
    std::unique_ptr<cch::tui::Component> status;
    /// The dock's editor region; the frozen entry carries `minSize: 3`.
    std::unique_ptr<cch::tui::Component> editor;
    std::unique_ptr<cch::tui::Component> footer;
    /// Extension widget rows; an absent entry is omitted from the dock
    /// (pi `widgetsAbove` / `widgetsBelow`).
    std::unique_ptr<cch::tui::Component> widgets_above;
    std::unique_ptr<cch::tui::Component> widgets_below;
    /// pi `scrollbar ?? "auto"`: a transcript built here defaults to the
    /// transient bar (the reusable toolkit viewport defaults to hidden, #1020
    /// replaces this constant with the persisted setting).
    cch::tui::ScrollbarMode scrollbar{cch::tui::ScrollbarMode::Auto};
    /// pi `scrollbarTrackStyle` / `scrollbarThumbStyle`, applied by the
    /// transcript viewport when present; an absent hook keeps the toolkit
    /// default.
    std::move_only_function<std::string(std::string_view)> scrollbar_track_style{};
    std::move_only_function<std::string(std::string_view)> scrollbar_thumb_style{};
};

/// The assembled viewport. `root` is the layout root to install; `transcript`
/// and the editor transfer seam stay addressable because scroll state and the
/// frozen dock slot belong to this retained tree.
struct ChatViewport {
    std::unique_ptr<cch::tui::Component> root;
    cch::tui::ScrollView* transcript{nullptr};

    /// Transfer the dock editor out while leaving its frozen slot and layout
    /// attributes in place. The returned component can be mounted in Regular.
    [[nodiscard]] std::unique_ptr<cch::tui::Component> take_editor();
    /// Reinstall the same editor instance in the retained dock slot.
    [[nodiscard]] support::ExpectedVoid set_editor(std::unique_ptr<cch::tui::Component> editor);

private:
    friend support::Expected<ChatViewport> create_chat_viewport(ChatViewportOptions options);
    class EditorSlot;
    EditorSlot* editor_slot_{nullptr};
};

/// Build the frozen two-region viewport (`chat-viewport.ts:22`):
///
/// ```text
/// root = VStack[
///   ScrollView(document, follow end, primary, overscroll chain,
///              scrollbar ?? auto, styles when provided)
///       { basis 0, grow 1, shrink 1, minSize 1 },
///   dock VStack(
///       pendingMessages { shrink 1, minSize 0 },
///       status          { shrink 1, minSize 0 },
///       widgetsAbove?   { shrink 1, minSize 0 },
///       editor          { shrink 1, minSize 3 },
///       widgetsBelow?   { shrink 1, minSize 0 },
///       footer          { shrink 1, minSize 0 })
///       { basis auto, grow 0, shrink 1, minSize 1 },
/// ]
/// ```
///
/// The document renders unbounded inside its viewport: the frame engine
/// measures the viewport's child without a height bound and paints the visible
/// window, so the transcript keeps its natural height and the viewport owns
/// clipping, the cursor and follow behaviour. Fails with `Validation` when the
/// document region is missing (the scroll viewport cannot exist without its one
/// child) or when the toolkit rejects a dock entry.
[[nodiscard]] support::Expected<ChatViewport> create_chat_viewport(ChatViewportOptions options);

} // namespace cch::coding_agent::tui
