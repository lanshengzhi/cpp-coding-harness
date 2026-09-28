#pragma once

#include <functional>
#include <string>

namespace cch::tui {

/// Generic terminal-text styling. Hooks may add supported ANSI styling but must
/// preserve the visible width of the supplied text.
using TextStyleHook = std::move_only_function<std::string(std::string)>;
using SelectionStyleHook = std::move_only_function<std::string(std::string, bool)>;

/// The SGR and OSC 8 attributes a terminal cell can carry. The single
/// definition of the tracked style state: the width module's ANSI tracker
/// layers its style processing on this value, and the virtual terminal's
/// cells hold it directly, so a tracked style converts by copy rather than
/// by a field-by-field translation that a new attribute can silently miss.
struct TerminalStyle {
    bool bold{false};
    bool dim{false};
    bool italic{false};
    bool underline{false};
    bool blink{false};
    bool inverse{false};
    bool hidden{false};
    bool strikethrough{false};
    std::string fg_color;
    std::string bg_color;
    std::string hyperlink;
    std::string hyperlink_params;

    bool operator==(const TerminalStyle&) const = default;
};

} // namespace cch::tui
