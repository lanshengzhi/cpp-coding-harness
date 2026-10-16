#include "coding_agent/tui/ChatViewport.hpp"

#include <cch/tui/Stack.hpp>
#include <cch/tui/VStack.hpp>

#include <utility>

namespace cch::coding_agent::tui {
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
    if (auto added = add_dock_region(*dock, std::move(options.editor), kEditorMinRows); !added) {
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

    return ChatViewport{.root = std::move(root), .transcript = transcript_pointer};
}

} // namespace cch::coding_agent::tui
