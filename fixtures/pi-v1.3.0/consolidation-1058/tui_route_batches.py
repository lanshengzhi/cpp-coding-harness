#!/usr/bin/env python3
"""Build and check the #1182 TUI route-batch index.

The #1182 verification packet reconciles the 920 supported `cch_tui` rows with their current
delivery evidence. A row either carries a complete whole-row comparison receipt, or it is assigned
to exactly one bounded comparison batch named here. This module owns that assignment so the index
cannot drift from the consolidated ledger: `build` writes it deterministically, `check` rebuilds and
compares it and rejects a batch that is unnamed, empty, or missing its owner, public caller,
scenario, normal/discriminating case, dependency or implementation obligation.

Nothing here verifies a capability. Membership, implementation status and verification status stay
with the ledger and the delivery receipts; this file only names the remaining comparison work.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
LEDGER_PATH = ROOT / "docs/research/pi-v1.3.0/1058-consolidated-capability-ledger.json"
DELIVERIES_PATH = ROOT / "docs/research/pi-v1.3.0/1058-deliveries.json"
INDEX_PATH = ROOT / "docs/research/pi-v1.3.0/1182-tui-route-batches.json"

SCHEMA = "pike/pi-v1.3.0-tui-route-batches/1"

# Who owns each batch's remaining work. An obligation with an open owner ticket names it; where no
# implementation ticket exists, the bounded batch itself is the named owner (the ticket accepts "a
# bounded issue or explicit batch"), so no obligation is left unnamed for #1126 to absorb.
OBLIGATION_OWNERS = {
    "B1": "batch B1",
    "B2": "batch B2",
    "B3": "batch B3",
    "B4": "batch B4",
    "B5": "batch B5",
    "B6": "#1035",
    "B7": "batch B7",
    "B8": "batch B8",
    "B9": "batch B9",
    "B10": "batch B10",
    "B11": "batch B11",
    "B12": "batch B12",
    "B13": "batch B13",
    "B14": "#1035",
    "B15": "batch B15",
    "B16": "batch B16",
    "B17": "#831 and #1035",
}
OWNER = "cch_tui"
TUI_PREFIX = "cch_tui:"

# One bounded comparison batch per frozen module family. `selectors` assigns a supported row to a
# batch: the first selector whose module list contains the row's frozen source file wins, and a
# `symbol` selector additionally requires the row's symbol (the part after the owner prefix) to
# match. Ordered selectors resolve the large alt-screen and terminal modules into bounded families;
# there is no catch-all, so an unassigned row fails the build.
BATCHES: list[dict] = [
    {
        "id": "B1",
        "title": "Root TUI lifecycle, root mutation and diagnostic callbacks",
        "disposition": "comparison",
        "caller": "cch::tui::Tui (src/tui/Tui.cpp) through ITui",
        "captureFamilies": ["screen-state.json", "renderer-switch.json", "product-cursor.json"],
        "normalCase": "tests/tui/ScreenStateGoldenTest.cpp and tests/tui/TuiTest.cpp replay the common-root "
                      "add/remove/focus/input/clear/stop sequence and the regular renderer frames",
        "discriminatingCase": "a stopped root that still paints, or a clear that leaves the deferred-wrap cursor, "
                              "fails; the diagnostic-callback rows need a write-log capture so a callback that never "
                              "runs separates from one that runs late",
        "implementationObligation": "Fake-cursor marker contract: C++ has no renderFakeCursor marker pair (CURSOR_MARKER, tui:fake-cursor-marker-constants, renderFakeCursor, tui:resolveFakeCursors) and keeps cursor positioning out of band; C++ overlay composition is not compared against TUI.showOverlay's cursor-preservation case",
        "dependency": "frozen pi-v1.3.0 checkout with tsx to re-capture screen-state.json at 71023aec; "
                      "tests/tui/ScreenStateGoldenTest.cpp already replays the historical bundle",
        "selectors": [{"modules": ["packages/tui/src/tui.ts"]}],
    },
    {
        "id": "B2",
        "title": "Regular renderer, frame commit and render-state transfer",
        "disposition": "comparison",
        "caller": "cch::tui::TuiMainScreen (src/tui/TuiMainScreen.cpp) and ITui::render",
        "captureFamilies": ["renderer-switch.json", "product-cursor-layout.json", "fullscreen-exit-output.json"],
        "normalCase": "tests/tui/RenderDifferentialTest.cpp and tests/tui/TuiTest.cpp drive MainScreen render, "
                      "invalidate and stop through a VirtualTerminal",
        "discriminatingCase": "a regular frame that keeps the layout root attached after a fullscreen switch, or a "
                              "render-state transfer that starts the new root, fails",
        "implementationObligation": 'The same marker contract: TuiMainScreen.doRender has no fake-cursor resolution stage and no v1.3.0 comparison',
        "dependency": "frozen pi-v1.3.0 checkout with tsx to re-capture renderer-switch.json; the product rows in "
                      "B17 own the application-level switch",
        "selectors": [{"modules": ["packages/tui/src/tui-main-screen.ts"]}],
    },
    {
        "id": "B3",
        "title": "Alternate-screen viewport: frames, scroll algebra, scrollbar and search",
        "disposition": "comparison",
        "caller": "cch::tui::AltScreen (src/tui/AltScreen.cpp) scroll/scrollbar/search surface",
        "captureFamilies": ["scroll-view.json", "scrollbar.json", "alt-screen-search.json", "alt-screen-nav.json",
                            "hardware-cursor.json"],
        "normalCase": "tests/tui/ScrollViewEvidenceTest.cpp, tests/tui/ScrollbarEvidenceTest.cpp and "
                      "tests/tui/AltScreenSearchTest.cpp replay the frozen scroll, scrollbar and search scenarios",
        "discriminatingCase": "a followed window that stops following on append, a scrollbar reservation that "
                              "changes the transcript width, or a search match painted without its highlight style, "
                              "fails",
        "implementationObligation": 'The same marker contract for the alt-screen frame: TuiAltScreen.render draws the focused fake cursor as inline SGR',
        "dependency": "frozen pi-v1.3.0 checkout with tsx to re-capture the alt-screen families; #1035 owns the "
                      "image placement-span frames inside the same viewport",
        "selectors": [{"modules": ["packages/tui/src/tui-alt-screen.ts"]}],
    },
    {
        "id": "B4",
        "title": "Alternate-screen transcript selection, multi-click and copy",
        "disposition": "comparison",
        "caller": "cch::tui::AltScreen selection API (apply_selection, copy_active_selection_to_clipboard)",
        "captureFamilies": ["selection.json", "mouse.json", "mouse-components.json"],
        "normalCase": "tests/tui/SelectionEvidenceTest.cpp replays the word/line cycle, wrapped-row geometry and the "
                      "copy-on-select default",
        "discriminatingCase": "a third click that selects a word instead of the line, a drag that keeps the "
                              "double-click history after a reset, or a copy that returns the joined cell range "
                              "instead of the frozen text, fails",
        "implementationObligation": 'C++ has no public resetTextSelection() combining the selection clear with the multi-click history reset; clear_text_selection() keeps the history and lastClick has no public reset',
        "dependency": "frozen pi-v1.3.0 checkout with tsx to re-capture selection.json; the styled CJK scenario is "
                      "already registered and verified by this packet",
        "selectors": [
            {
                "modules": ["packages/tui/src/tui-alt-screen.ts"],
                "symbol": r"Selection|selection|Click|click|CopyOnSelect|copyOnSelect|copySelection|copyText|"
                          r"mousePressPoint",
            }
        ],
    },
    {
        "id": "B5",
        "title": "Alternate-screen overlay, wheel and mouse routing",
        "disposition": "comparison",
        "caller": "cch::tui::AltScreen::handle_viewport_input / route_wheel and OverlayCompositor",
        "captureFamilies": ["mouse.json", "scroll-view.json", "overlay-routing.json"],
        "normalCase": "tests/tui/MouseRoutingTest.cpp, tests/tui/OverlayTest.cpp and tests/tui/MouseEvidenceTest.cpp "
                      "replay the frozen wheel consumption, overlay hit testing and mouse capture rules",
        "discriminatingCase": "a focused overlay that still forwards an unhandled wheel to the transcript (the "
                              "recorded partial), or a wheel over an overlay that scrolls the transcript underneath, "
                              "fails",
        "implementationObligation": 'C++ AltScreen still defers an unhandled wheel to the focused overlay (shouldDeferViewportInputToOverlay gates handleViewportInput) instead of scrolling the transcript',
        "dependency": "the C++ overlay wheel deferral is recorded partial in the ledger; a capture at pi-v1.3.0 plus "
                      "the implementation fix is required before this batch can compare",
        "selectors": [
            {
                "modules": ["packages/tui/src/tui-alt-screen.ts"],
                "symbol": r"Overlay|ViewportInput|routeWheel|Wheel|wheel|Mouse|mouse|dispatch|MouseGesture|"
                          r"MouseEvent|RightClickPaste",
            }
        ],
    },
    {
        "id": "B6",
        "title": "Alternate-screen kitty image placement and eviction",
        "disposition": "emulator",
        "caller": "cch::tui::AltScreen kitty placement lifecycle over a real terminal",
        "captureFamilies": ["alt-screen-image.json", "iterm2-image.json"],
        "normalCase": "tests/tui/AltScreenImageEvidenceTest.cpp replays crop/restore, anchor move, removal and "
                      "eviction through VirtualTerminal",
        "discriminatingCase": "an image re-emitted as a full transmission instead of a placement-only a=p command, "
                              "or a session teardown that leaves the placement on screen, fails",
        "dependency": "#1035 owns the placement-span repaint and its remaining emulator observation; #831 owns the "
                      "dock footer frames",
        "selectors": [
            {
                "modules": ["packages/tui/src/tui-alt-screen.ts"],
                "symbol": r"Kitty|kitty|imageProtocol|prepareKittyScreen|uploadedKittyImages|deleteKittyImages",
            }
        ],
    },
    {
        "id": "B7",
        "title": "Editor: layout, history, undo and kill/yank",
        "disposition": "comparison",
        "caller": "cch::tui::Editor (src/tui/Editor.cpp) and detail::EditorLayout",
        "captureFamilies": ["editor-autocomplete.json", "screen-state.json", "mouse-components.json"],
        "normalCase": "tests/tui/EditorTest.cpp and tests/tui/EditorLayoutTest.cpp drive editing, wrapping, history, "
                      "undo and the frozen word navigation",
        "discriminatingCase": "an undo that leaves the cursor on the pre-edit column, a kill ring that yanks the "
                              "wrong entry, or a wrapped click that maps to the previous grapheme, fails",
        "implementationObligation": 'Editor.render emits the drawn cursor as raw reverse video instead of renderFakeCursor markers; the editor family has no v1.3.0 comparison',
        "dependency": "frozen pi-v1.3.0 checkout with tsx to re-capture editor-autocomplete.json and the screen-state "
                      "editor scenarios; the fake-cursor rows stay with the cursor batch",
        "selectors": [
            {
                "modules": [
                    "packages/tui/src/components/editor.ts",
                    "packages/tui/src/editor-component.ts",
                    "packages/tui/src/kill-ring.ts",
                    "packages/tui/src/undo-stack.ts",
                    "packages/tui/src/alt-screen-search.ts",
                ]
            }
        ],
    },
    {
        "id": "B8",
        "title": "Input: editing, paste and submit",
        "disposition": "comparison",
        "caller": "cch::tui::Input (src/tui/Input.cpp)",
        "captureFamilies": ["input.json", "mouse-components.json"],
        "normalCase": "tests/tui/InputTest.cpp replays the frozen editing, placeholder, narrow-scroll and submit "
                      "observations",
        "discriminatingCase": "a paste that submits mid-fragment, or a rebound submit key that still inserts a newline, "
                              "fails",
        "implementationObligation": 'Input.render emits the drawn cursor as raw reverse video instead of renderFakeCursor markers; the input family has no v1.3.0 comparison',
        "dependency": "frozen pi-v1.3.0 checkout with tsx to re-capture input.json",
        "selectors": [{"modules": ["packages/tui/src/components/input.ts"]}],
    },
    {
        "id": "B9",
        "title": "Markdown, tables and math rendering",
        "disposition": "comparison",
        "caller": "cch::tui::Markdown (src/tui/Markdown.cpp) and cch::tui::render_latex",
        "captureFamilies": ["latex.json", "screen-state.json"],
        "normalCase": "tests/tui/MarkdownTest.cpp and tests/tui/LatexTest.cpp render headings, lists, tables, CJK and "
                      "the frozen inline/display latex grammar",
        "discriminatingCase": "a table column that ignores a wide grapheme, or a latex fraction rendered from a "
                              "different grammar, fails; the markdown family has no historical capture, so a new "
                              "frozen capture is required",
        "dependency": "a new frozen pi-v1.3.0 markdown capture (no historical markdown artifact exists) plus the "
                      "existing latex.json re-capture",
        "selectors": [{"modules": ["packages/tui/src/components/markdown.ts", "packages/tui/src/latex.ts"]}],
    },
    {
        "id": "B10",
        "title": "Color parsing, palettes and terminal color capability",
        "disposition": "comparison",
        "caller": "cch::tui::ColorUtils (src/tui/ColorUtils.cpp) and the theme entry points",
        "captureFamilies": ["utils-ansi.json"],
        "normalCase": "tests/tui/ColorUtilsTest.cpp and tests/tui/TerminalColorsTest.cpp parse the frozen hex, rgb, "
                      "ansi256 and oklch/okhsl spellings",
        "discriminatingCase": "a color that silently falls back instead of returning the frozen failure value, or an "
                              "oklab mix that rounds through sRGB, fails; no historical color capture exists, so a new "
                              "frozen capture is required",
        "dependency": "a new frozen pi-v1.3.0 color capture (the historical route was never captured) and the "
                      "unchanged-source diff",
        "selectors": [
            {"modules": ["packages/tui/src/colors.ts", "packages/tui/src/terminal-colors.ts",
                         "packages/tui/src/oklab.ts"]}
        ],
    },
    {
        "id": "B11",
        "title": "SelectList and SettingsList",
        "disposition": "comparison",
        "caller": "cch::tui::SelectList and cch::tui::SettingsList",
        "captureFamilies": ["screen-state.json", "mouse-components.json", "fullscreen-settings.json"],
        "normalCase": "tests/tui/SelectListTest.cpp and tests/tui/SettingsListTest.cpp replay the windowing, "
                      "description truncation, hover and click activation scenarios",
        "discriminatingCase": "a five-row window that centers on the wrong row, a description cut mid-grapheme, or a "
                              "separator row that activates a setting, fails",
        "dependency": "frozen pi-v1.3.0 checkout with tsx to re-capture the select-list and settings-list scenarios",
        "selectors": [
            {"modules": ["packages/tui/src/components/select-list.ts", "packages/tui/src/components/settings-list.ts"]}
        ],
    },
    {
        "id": "B12",
        "title": "Layout algebra, scroll containers and the small text components",
        "disposition": "comparison",
        "caller": "cch::tui::detail::LayoutEngine, ScrollView, VStack/HStack, Container/Box/Text/Spacer/TruncatedText",
        "captureFamilies": ["layout.json", "scroll-view.json", "component.json", "utils-width.json"],
        "normalCase": "tests/tui/StackLayoutEvidenceTest.cpp, tests/tui/HStackEvidenceTest.cpp, "
                      "tests/tui/ScrollViewEvidenceTest.cpp and tests/tui/ContainerTest.cpp replay the frozen frames",
        "discriminatingCase": "an auto basis measured at the frame width instead of its natural width, a dock whose "
                              "growth drops the footer row, or a padding change that only re-clamps without "
                              "re-wrapping, fails",
        "implementationObligation": 'Box has no set_padding_x (padding only at construction) and shares the fake-cursor marker boundary obligation through utils:extractSegments',
        "dependency": "frozen pi-v1.3.0 checkout with tsx to re-capture layout.json and component.json; #831 owns the "
                      "dock growth frames",
        "selectors": [
            {
                "modules": [
                    "packages/tui/src/components/scroll-view.ts",
                    "packages/tui/src/components/v-stack.ts",
                    "packages/tui/src/components/h-stack.ts",
                    "packages/tui/src/components/box.ts",
                    "packages/tui/src/components/text.ts",
                    "packages/tui/src/components/container.ts",
                    "packages/tui/src/components/spacer.ts",
                    "packages/tui/src/components/truncated-text.ts",
                    "packages/tui/src/layout.ts",
                    "packages/tui/src/wheel-scroll.ts",
                    "packages/tui/src/utils.ts",
                ]
            }
        ],
    },
    {
        "id": "B13",
        "title": "Terminal IO: process terminal, stdin framing, keys and keybindings",
        "disposition": "comparison",
        "caller": "cch::tui::ProcessTerminal, StdinBuffer, parse_key/matches_key and KeybindingsManager",
        "captureFamilies": ["input.json", "keys-kitty-text.json", "utils-ansi.json"],
        "normalCase": "tests/tui/ProcessTerminalTest.cpp, tests/tui/StdinBufferTest.cpp and tests/tui/KeysTest.cpp "
                      "replay the frozen startup, framing and key-identity observations",
        "discriminatingCase": "a fragmented terminal reply that leaks into typed input, a Shift-modified symbol key "
                              "that misses its produced character, or a keybinding context that converges on the "
                              "wrong binding, fails",
        "implementationObligation": 'C++ ProcessTerminal.stop() omits the frozen `ESC[>4;0m` modifyOtherKeys restore whenever modifyOtherKeys was never active, so the registered audit-1050 program-status-detection stop sequence is not reproduced byte for byte (#1182 finding); C++ parse_key/matches_key keep the explicit Shift flag on a produced symbol; StdinBuffer flushes a partial OSC/APC/kitty reply tail as typed input at the timeout; the remaining terminal families have no v1.3.0 comparison',
        "dependency": "frozen pi-v1.3.0 checkout with tsx to re-capture input.json and keys-kitty-text.json",
        "selectors": [
            {
                "modules": [
                    "packages/tui/src/terminal.ts",
                    "packages/tui/src/stdin-buffer.ts",
                    "packages/tui/src/keys.ts",
                    "packages/tui/src/keybindings.ts",
                ]
            }
        ],
    },
    {
        "id": "B14",
        "title": "Image dimensions, protocol detection and the Image component",
        "disposition": "emulator",
        "caller": "cch::tui::TerminalImage (src/tui/TerminalImage.cpp) and cch::tui::Image",
        "captureFamilies": ["utils-width.json", "alt-screen-image.json", "iterm2-image.json"],
        "normalCase": "tests/tui/TerminalImageTest.cpp and tests/tui/ImageTest.cpp compare the protocol matrix, cell "
                      "dimensions and the fallback rows",
        "discriminatingCase": "a capability detected from the wrong environment variable, or a fallback row emitted "
                              "after the image sequence, fails; the rendered placement needs a real emulator",
        "implementationObligation": 'C++ terminal image detection has no Herdr branch and no Herdr-before-Kitty ordering',
        "dependency": "a frozen pi-v1.3.0 image dimension/protocol capture plus a real emulator observation; #1035 "
                      "owns the placement-span repaint",
        "selectors": [
            {"modules": ["packages/tui/src/terminal-image.ts", "packages/tui/src/components/image.ts"]}
        ],
    },
    {
        "id": "B15",
        "title": "Autocomplete providers and fuzzy ranking",
        "disposition": "comparison",
        "caller": "cch::tui::CombinedAutocompleteProvider / AutocompleteProvider and fuzzy_match/fuzzy_filter",
        "captureFamilies": ["autocomplete.json", "editor-autocomplete.json", "fuzzy.json"],
        "normalCase": "tests/tui/AutocompleteTest.cpp, tests/tui/AutocompleteRankingTest.cpp and tests/tui/FuzzyTest.cpp "
                      "replay the frozen completion contexts, ranking and UTF-16 offsets",
        "discriminatingCase": "a provider that completes at the wrong token boundary, or a fuzzy score that keeps the "
                              "UTF-16 offset instead of the frozen UTF-8 one, fails",
        "dependency": "frozen pi-v1.3.0 checkout with tsx to re-capture autocomplete.json and fuzzy.json",
        "selectors": [
            {"modules": ["packages/tui/src/autocomplete.ts", "packages/tui/src/fuzzy.ts"]}
        ],
    },
    {
        "id": "B16",
        "title": "Loader, mouse regions and residual small components",
        "disposition": "comparison",
        "caller": "cch::tui::Loader/CancellableLoader, MouseRegion and the program-status helpers",
        "captureFamilies": ["screen-state.json", "mouse-components.json", "product-progress.json"],
        "normalCase": "tests/tui/LoaderTest.cpp and tests/tui/MouseRoutingTest.cpp replay the frozen spinner frames "
                      "and mouse-region routing",
        "discriminatingCase": "a loader that repaints only on the animation tick and misses a synchronous frame, or a "
                              "mouse region that keeps routing after removal, fails",
        "dependency": "frozen pi-v1.3.0 checkout with tsx to re-capture the loader and mouse-region scenarios",
        "selectors": [
            {
                "modules": [
                    "packages/tui/src/components/loader.ts",
                    "packages/tui/src/components/cancellable-loader.ts",
                    "packages/tui/src/components/mouse-region.ts",
                    "packages/tui/src/program-status.ts",
                ]
            }
        ],
    },
    {
        "id": "B17",
        "title": "Application product entries and the selected emulator scenarios",
        "disposition": "product",
        "caller": "the ordinary application Session (coding_agent InteractiveMode) over a real terminal",
        "captureFamilies": ["product-differential.json", "product-cursor.json", "product-progress.json",
                            "fullscreen-settings.json", "chat-viewport.json", "clipboard-osc52.json"],
        "normalCase": "tests/coding_agent/tui/NativeTuiDifferentialTest.cpp drives the dual-runtime scenario cells and "
                      "tests/coding_agent/tui/ProductCursorEvidenceTest.cpp replays the frozen cursor frames",
        "discriminatingCase": "a product row whose scenario is only observed by the external dual-runtime harness "
                              "(skipped without the capture environment), or a row with no recorded comparison "
                              "scenario at all (the #831/#1035 residuals), cannot be verified and must stay open",
        "rows": [
            "cch_tui:hardware-cursor",
            "cch_tui:images-placement-herdr",
            "cch_tui:program-status-protocol",
            "cch_tui:prompt-input-decoder",
            "cch_tui:residual-1035-image-placement",
            "cch_tui:residual-831-dock-growth-footer",
            "cch_tui:transcript-selection-reset",
            "cch_tui:wheel-over-overlay",
        ],
        "implementationObligation": 'The three scenario-less residual rows (hardware-cursor, residual-1035-image-placement, residual-831-dock-growth-footer) cannot carry a whole-row receipt until a comparison scenario is registered; program-status-protocol, transcript-selection-reset and wheel-over-overlay have no C++ implementation',
        "dependency": "#831 and #1035 own their exact remaining emulator scenarios; the native differential cells "
                      "need the separately selected dual-runtime capture environment; the clipboard and hyperlink "
                      "outcomes need a real emulator observation, not PTY bytes",
        "selectors": [{"modules": []}],
    },
]

def load_ledger() -> dict:
    return json.loads(LEDGER_PATH.read_text(encoding="utf-8"))


def load_deliveries() -> dict:
    return json.loads(DELIVERIES_PATH.read_text(encoding="utf-8"))


def supported_rows(ledger: dict) -> list[dict]:
    return [row for row in ledger["rows"] if row["id"].startswith(TUI_PREFIX) and row["membership"] == "supported"]


def delivered_rows(deliveries: dict) -> set[str]:
    ids: set[str] = set()
    for record in deliveries.get("records", []):
        for claim in record.get("rows", []):
            ids.add(claim["id"])
    return ids


def row_module(row: dict) -> str:
    """The row's frozen source module, or an empty string for a row that names no packages/ path."""
    for source in row["source"]:
        path = source.split(":", 1)[0]
        if path.startswith("packages/"):
            return path
    return ""


def row_symbol(row: dict) -> str:
    return row["id"].removeprefix(TUI_PREFIX)


def batch_for(row: dict) -> str:
    module = row_module(row)
    symbol = row_symbol(row)
    # A symbol-scoped selector is more specific than a module-only one, so the alt-screen families
    # resolve before the module default that also covers them.
    ordered = [batch for batch in BATCHES if any("symbol" in selector for selector in batch["selectors"])]
    ordered += [batch for batch in BATCHES if not any("symbol" in selector for selector in batch["selectors"])]
    for batch in ordered:
        for selector in batch["selectors"]:
            modules = selector["modules"]
            if module in modules:
                pattern = selector.get("symbol")
                if pattern is None or re.search(pattern, symbol):
                    return batch["id"]
            elif not modules and not module:
                # The product batch owns rows whose frozen pointer is not a packages/tui source
                # (the application entries and the historical issue-body residuals).
                return batch["id"]
    raise ValueError(f"{row['id']}: no batch selector covers module {module or '<none>'}")


def build_index(ledger: dict, deliveries: dict) -> dict:
    rows = supported_rows(ledger)
    delivered = delivered_rows(deliveries)
    assigned: dict[str, str] = {}
    batches = []
    for batch in BATCHES:
        entry = {
            "id": batch["id"],
            "title": batch["title"],
            "disposition": batch["disposition"],
            "owner": OWNER,
            "caller": batch["caller"],
            "captureFamilies": list(batch["captureFamilies"]),
            "normalCase": batch["normalCase"],
            "discriminatingCase": batch["discriminatingCase"],
            "dependency": batch["dependency"],
            "obligationOwner": OBLIGATION_OWNERS[batch["id"]],
            "implementationObligation": batch.get("implementationObligation"),
            "rows": [],
            "scenarioLabels": [],
            "implementationRows": [],
        }
        batches.append(entry)
    by_batch = {entry["id"]: entry for entry in batches}
    explicit = {row_id: batch["id"] for batch in BATCHES for row_id in batch.get("rows", [])}
    for row in rows:
        if row["id"] in delivered:
            continue
        batch_id = explicit.get(row["id"], batch_for(row))
        if row["id"] in assigned:
            raise ValueError(f"{row['id']}: assigned to {assigned[row['id']]} and {batch_id}")
        assigned[row["id"]] = batch_id
        entry = by_batch[batch_id]
        entry["rows"].append(row["id"])
        if row["implementation"] != "implemented":
            entry["implementationRows"].append(row["id"])
        for label in row["comparisonScenarios"]:
            if label not in entry["scenarioLabels"]:
                entry["scenarioLabels"].append(label)
    for entry in batches:
        entry["rows"].sort()
        entry["implementationRows"].sort()
        entry["scenarioLabels"].sort()
    unassigned = sorted(row["id"] for row in rows if row["id"] not in assigned and row["id"] not in delivered)
    if unassigned:
        raise ValueError(f"supported rows without a batch: {unassigned[:10]}")
    return {
        "schema": SCHEMA,
        "ticket": "1182",
        "baseline": ledger["baseline"],
        "revision": ledger["revision"],
        "supportedRows": len(rows),
        "deliveredRows": sorted(row["id"] for row in rows if row["id"] in delivered),
        "batches": batches,
    }


def validate(index: dict, ledger: dict, deliveries: dict) -> list[str]:
    """Reject an index that leaves a row unnamed or a batch unusable. Returns review notes."""
    rows = {row["id"]: row for row in supported_rows(ledger)}
    delivered = delivered_rows(deliveries)
    notes: list[str] = []
    seen: dict[str, str] = {}
    for batch in index["batches"]:
        for field in ("id", "title", "disposition", "owner", "caller", "normalCase", "discriminatingCase",
                      "dependency", "obligationOwner"):
            value = batch.get(field)
            if not isinstance(value, str) or not value.strip():
                raise ValueError(f"{batch.get('id', '<unnamed>')}: missing {field}")
        if batch["owner"] != OWNER:
            raise ValueError(f"{batch['id']}: owner is {batch['owner']}, not {OWNER}")
        if batch["disposition"] not in ("comparison", "implementation", "emulator", "product"):
            raise ValueError(f"{batch['id']}: unknown disposition {batch['disposition']}")
        if not batch["captureFamilies"]:
            raise ValueError(f"{batch['id']}: names no capture family")
        if not batch["rows"]:
            raise ValueError(f"{batch['id']}: covers no supported row")
        obligation = batch.get("implementationObligation")
        if bool(batch["implementationRows"]) != bool(isinstance(obligation, str) and obligation.strip()):
            raise ValueError(f"{batch['id']}: implementation rows need an implementation obligation and vice versa")
        for row_id in batch["rows"]:
            if row_id in seen:
                raise ValueError(f"{row_id}: assigned to {seen[row_id]} and {batch['id']}")
            seen[row_id] = batch["id"]
            row = rows.get(row_id)
            if row is None:
                raise ValueError(f"{batch['id']}: {row_id} is not a supported cch_tui row")
            if row_id in delivered:
                raise ValueError(f"{batch['id']}: {row_id} already carries a delivery receipt")
            expected_labels = sorted(set(row["comparisonScenarios"]))
            if not set(expected_labels) <= set(batch["scenarioLabels"]):
                raise ValueError(f"{batch['id']}: recorded scenario labels for {row_id} are not named")
            expected_impl = row["implementation"] != "implemented"
            if expected_impl != (row_id in batch["implementationRows"]):
                raise ValueError(f"{batch['id']}: implementation obligation for {row_id} is wrong")
    missing = sorted(set(rows) - set(seen) - delivered)
    if missing:
        raise ValueError(f"supported rows without a batch: {missing[:10]}")
    tui_delivered = sorted(set(rows) & delivered)
    notes.append(f"{len(rows)} supported rows: {len(tui_delivered)} delivered, {len(seen)} batched")
    notes.append(f"{len(index['batches'])} batches: "
                 + ", ".join(f"{batch['id']}={len(batch['rows'])}" for batch in index["batches"]))
    return notes


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("build", "check"))
    args = parser.parse_args()
    ledger = load_ledger()
    deliveries = load_deliveries()
    index = build_index(ledger, deliveries)
    payload = (json.dumps(index, indent=1, ensure_ascii=False) + "\n").encode("utf-8")
    if args.command == "build":
        INDEX_PATH.write_bytes(payload)
        print(f"wrote {INDEX_PATH.relative_to(ROOT)} ({len(payload)} bytes, {len(index['batches'])} batches)")
        for batch in index["batches"]:
            print(f"  {batch['id']:4s} rows={len(batch['rows']):4d} "
                  f"implementation={len(batch['implementationRows']):3d} "
                  f"labels={len(batch['scenarioLabels']):2d} {batch['title']}")
        return 0
    if INDEX_PATH.read_bytes() != payload:
        raise ValueError(f"{INDEX_PATH.name} differs from a deterministic rebuild; run build")
    notes = validate(index, ledger, deliveries)
    print("tui route batches check: PASS")
    for note in notes:
        print(f"  - {note}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"tui route batches check: FAIL: {error}", file=sys.stderr)
        sys.exit(1)
