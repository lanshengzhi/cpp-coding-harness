#!/usr/bin/env python3
"""Build the #1050 pi-v1.3.0 Linux TUI toolkit capability ledger.

The historical `pi-v1.0.4` ledger is the stable export/member identity source. This builder
re-verifies every identity against the frozen v1.3.0 package entry and source tree, records which
source files changed since v1.0.4, adds the v1.3.0 additions, and applies the curated change map
for changed capabilities. It writes only the ticket-owned ledger under `docs/research/pi-v1.3.0/`.

Usage:
    python3 fixtures/pi-v1.3.0/audit-1050/build-ledger.py \
        --pi-checkout /tmp/cch-1050-pi-frozen \
        --output docs/research/pi-v1.3.0/1050-toolkit-capability-ledger.json
"""

from __future__ import annotations

import argparse
from collections import Counter, OrderedDict
import json
from pathlib import Path
import re
import subprocess
import sys

HISTORICAL_REVISION = "7c10bd4337495ee613f2224843ecdf349b80d1df"
BASELINE = "pi-v1.3.0"
REVISION = "71023aec1546d6d677a65669c5665c0927ce0dd0"
HISTORICAL_LEDGER = "docs/research/tui-v1.0.4-capability-ledger.json"
HISTORICAL_BUNDLE = "fixtures/pi-tui/bundles/pi-v1.0.4/manifest.json"
AUDIT_BUNDLE = "fixtures/pi-v1.3.0/audit-1050"


def run_git(pi_checkout: Path, *args: str) -> str:
    result = subprocess.run(
        ["git", "-C", str(pi_checkout), *args],
        capture_output=True,
        text=True,
        env={"GIT_CONFIG_NOSYSTEM": "1", "GIT_CONFIG_GLOBAL": "/dev/null", "PATH": "/usr/bin:/bin"},
    )
    if result.returncode != 0:
        raise SystemExit(f"git {' '.join(args)} failed: {result.stderr}")
    return result.stdout


def index_exports(index_text: str) -> "OrderedDict[str, str]":
    """Export name -> module path (relative to packages/tui/src)."""
    exports: "OrderedDict[str, str]" = OrderedDict()
    for match in re.finditer(r"export\s+(?:type\s+)?\{([^}]+)\}\s+from\s+\"([^\"]+)\"", index_text, re.S):
        module = match.group(2)
        if module.startswith("./"):
            module = module[2:]
        for raw in match.group(1).split(","):
            raw = raw.strip()
            if not raw:
                continue
            raw = re.sub(r"^type\s+", "", raw).strip()
            if " as " in raw:
                raw = raw.split(" as ")[-1].strip()
            exports[raw] = f"packages/tui/src/{module}"
    return exports


def endpoint(path: str, symbol: str) -> str:
    if not path.startswith("packages/"):
        path = "packages/tui/src/index.ts"
    return f"{path}:{symbol}"


# ---------------------------------------------------------------------------
# Curated v1.3.0 changes. Keys are "Export" or "Export.member".
# ---------------------------------------------------------------------------

CURSOR_ARTIFACT = f"{AUDIT_BUNDLE}/cursor-composition.json"
PADDING_ARTIFACT = f"{AUDIT_BUNDLE}/padding-and-selection-reset.json"
OVERLAY_ARTIFACT = f"{AUDIT_BUNDLE}/overlay-routing.json"
KEYS_ARTIFACT = f"{AUDIT_BUNDLE}/keys-and-terminal-replies.json"
PROGRAM_FORMAT_ARTIFACT = f"{AUDIT_BUNDLE}/program-status-format.json"
PROGRAM_DETECTION_ARTIFACT = f"{AUDIT_BUNDLE}/program-status-detection.json"
TERMINAL_CAPS_ARTIFACT = f"{AUDIT_BUNDLE}/terminal-capabilities.json"
HARDWARE_ARTIFACT = f"{AUDIT_BUNDLE}/hardware-cursor.json"
INPUT_ARTIFACT = f"{AUDIT_BUNDLE}/input-standalone.json"
SELECTION_ARTIFACT = f"{AUDIT_BUNDLE}/selection-multiclick.json"

CHANGE_MAP: dict[str, dict] = {
    "Editor.render": {
        "implementation": "partial",
        "artifact": CURSOR_ARTIFACT,
        "scenario": "standalone-editor-fake-cursor-is-strippable",
        "result": (
            "frozen Editor.render wraps the drawn cursor in zero-width APC markers and the TUI draws "
            "reverse video or omits a focused fake cursor when showHardwareCursor is enabled; C++ "
            "EditorLayout inserts raw ESC[7m/ESC[27m and has no marker contract"
        ),
        "gaps": [
            "C++ Editor.render emits the drawn cursor as raw reverse video, not renderFakeCursor markers",
            "C++ does not suppress the focused fake cursor when showHardwareCursor is enabled",
            "C++ has no v1.3.0 comparison for the standalone-marker or hardware-cursor outcomes",
        ],
        "consumer": "cch_coding_agent interactive editor (InteractiveView/ExtensionUiHost)",
        "tests": ["tests/tui/EditorTest.cpp", "tests/tui/EditorLayoutTest.cpp"],
        "defaults": "paddingX=0; fake cursor drawn at the cursor cell; hardware cursor hidden by default",
    },
    "Input.render": {
        "implementation": "partial",
        "artifact": CURSOR_ARTIFACT,
        "scenario": "standalone-input-fake-cursor-is-strippable",
        "result": (
            "frozen Input.render wraps the placeholder/cursor cell in APC markers that strip to the same "
            "visible text; C++ Input renders raw reverse video and strips nothing"
        ),
        "gaps": [
            "C++ Input.render emits the drawn cursor as raw reverse video, not renderFakeCursor markers",
            "C++ does not suppress the focused fake cursor when showHardwareCursor is enabled",
            "C++ has no v1.3.0 comparison for the standalone-marker outcome",
        ],
        "consumer": "cch_coding_agent prompt input and selector search inputs",
        "tests": ["tests/tui/InputTest.cpp"],
        "defaults": "prompt='> '; fake cursor drawn at the grapheme or trailing space",
    },
    "TuiAltScreen.render": {
        "implementation": "partial",
        "artifact": CURSOR_ARTIFACT,
        "scenario": "overlay-left-of-cursor-keeps-cursor-fake",
        "result": (
            "frozen fullscreen render resolves fake cursors before search highlighting and overlay "
            "compositing; an overlay that ends left of the cursor keeps the fake cursor at column 4"
        ),
        "gaps": [
            "C++ AltScreen draws the fake cursor as inline SGR and has no fake-cursor resolution stage",
            "C++ has no v1.3.0 comparison for marker-adjacent overlay composition",
        ],
        "consumer": "cch_coding_agent fullscreen transcript renderer",
        "tests": ["tests/tui/AltScreenTest.cpp", "tests/tui/SelectionEvidenceTest.cpp"],
    },
    "TuiAltScreen.applySelection": {
        "implementation": "implemented",
        "artifact": SELECTION_ARTIFACT,
        "scenario": "styled-cjk-wrapped-selection",
        "result": (
            "selection styling is unchanged and re-captured at v1.3.0; v1.3.0 resolves fake cursors "
            "before applySelection, but the cursor/selection overlap itself is not separately captured"
        ),
        "gaps": [
            "no v1.3.0 C++ comparison recorded",
            "fake-cursor/selection overlap is inferred from the render order, not captured as its own scenario",
        ],
        "consumer": "cch_coding_agent fullscreen text selection",
    },
    "TUI.showOverlay": {
        "implementation": "partial",
        "artifact": CURSOR_ARTIFACT,
        "scenario": "overlay-left-of-cursor-keeps-cursor-fake",
        "result": (
            "frozen overlay compositing keeps the cursor when the overlay ends directly left of it; "
            "C++ has an overlay compositing path but no v1.3.0 marker-aware comparison"
        ),
        "gaps": [
            "C++ overlay composition is not compared against the v1.3.0 cursor-preservation case",
        ],
        "consumer": "cch_coding_agent fullscreen selectors/dialogs and custom overlays",
        "tests": ["tests/tui/OverlayTest.cpp"],
    },
    "TuiAltScreen.handleViewportInput": {
        "implementation": "partial",
        "artifact": OVERLAY_ARTIFACT,
        "scenario": "overlay-unhandled-wheel-scrolls-transcript",
        "result": (
            "frozen routes an unhandled wheel event to the transcript even while a focused overlay owns "
            "viewport keys; C++ still returns early through should_defer_viewport_input_to_overlay()"
        ),
        "gaps": [
            "C++ AltScreen still defers the wheel to the focused overlay instead of scrolling the transcript",
            "C++ has no v1.3.0 comparison for the focused-overlay wheel outcome",
        ],
        "consumer": "cch_coding_agent fullscreen wheel scrolling with selectors/dialog overlays",
        "tests": ["tests/tui/AltScreenTest.cpp", "tests/tui/OverlayTest.cpp"],
    },
    "TuiAltScreen.shouldDeferViewportInputToOverlay": {
        "implementation": "partial",
        "artifact": OVERLAY_ARTIFACT,
        "scenario": "overlay-unhandled-wheel-scrolls-transcript",
        "result": (
            "frozen keeps the deferral for viewport keys only; the wheel path no longer calls it before "
            "scrolling the transcript"
        ),
        "gaps": ["C++ helper still gates the unhandled wheel path"],
        "consumer": "cch_tui AltScreen input routing",
    },
    "TuiAltScreen.clearTextSelection": {
        "implementation": "partial",
        "artifact": PADDING_ARTIFACT,
        "scenario": "selection-reset-drops-selection-and-multiclick-history",
        "result": (
            "frozen resetTextSelection() drops both the selection and lastClick; v1.1.0 exposes it for a "
            "host that replaces the transcript"
        ),
        "gaps": [
            "C++ clear_text_selection() clears the selection but keeps the multi-click history",
            "C++ has no public combined resetTextSelection() equivalent",
        ],
        "consumer": "cch_coding_agent fullscreen transcript replacement (no C++ caller yet)",
        "tests": ["tests/tui/SelectionEvidenceTest.cpp", "tests/tui/AltScreenTest.cpp"],
    },
    "TuiAltScreen.lastClick": {
        "implementation": "partial",
        "artifact": PADDING_ARTIFACT,
        "scenario": "third-consecutive-click-selects-line-control",
        "result": "frozen lastClick survives clearTextSelection and is only dropped by resetTextSelection",
        "gaps": ["C++ selection click history has no public reset"],
        "consumer": "cch_tui AltScreen multi-click selection",
    },
    "TuiMainScreen.doRender": {
        "implementation": "partial",
        "artifact": CURSOR_ARTIFACT,
        "scenario": "main-screen-focused-fake-cursor-drawn",
        "result": (
            "frozen main-screen render resolves fake cursors before overlay compositing; C++ composes "
            "overlays and draws the cursor without a fake-cursor resolution stage"
        ),
        "gaps": [
            "C++ has no fake-cursor marker/resolution stage",
            "C++ has no v1.3.0 comparison for overlay-adjacent fake cursor composition",
        ],
        "consumer": "cch_tui MainScreen renderer",
        "tests": ["tests/tui/HardwareCursorEvidenceTest.cpp", "tests/tui/RenderDifferentialTest.cpp"],
    },
    "StdinBuffer.process": {
        "implementation": "partial",
        "artifact": KEYS_ARTIFACT,
        "scenario": "fragmented-color-reply-never-leaks-or-opens-editor",
        "result": (
            "frozen keeps a recognized terminal-reply header buffered past the sequence timeout and "
            "re-parses ambiguous Alt input; C++ flushes the fragment as input"
        ),
        "gaps": [
            "C++ StdinBuffer flushes a partial OSC/APC/kitty reply at the timeout, leaking its tail as text",
            "C++ has no v1.3.0 comparison for fragmented terminal replies or the ambiguous Alt recovery",
        ],
        "consumer": "cch_tui ProcessTerminal input edge",
        "tests": ["tests/tui/StdinBufferTest.cpp", "tests/tui/KeysLegacyTest.cpp"],
        "defaults": "timeout=50ms, escapeTimeout=10ms",
    },
    "StdinBuffer.flush": {
        "implementation": "partial",
        "artifact": KEYS_ARTIFACT,
        "scenario": "fragmented-color-reply-never-leaks-or-opens-editor",
        "result": (
            "frozen does not flush a recognized reply header; it recovers only ambiguous Alt-key input "
            "by re-emitting the ESC pair and reparsing the tail"
        ),
        "gaps": ["C++ flush emits the partial reply as one data sequence"],
        "consumer": "cch_tui ProcessTerminal input edge",
    },
    "ProcessTerminal.start": {
        "implementation": "partial",
        "artifact": PROGRAM_DETECTION_ARTIFACT,
        "scenario": "default-reply-before-device-attributes",
        "result": (
            "frozen start() queries OSC 7501 before DA1, honors PI_PROGRAM_STATUS=1|0 and re-sends a "
            "pending status once support is confirmed; C++ start() has no program-status query"
        ),
        "gaps": [
            "C++ ProcessTerminal has no program-status query, override or deferred report",
            "C++ has no v1.3.0 comparison for program-status negotiation",
        ],
        "consumer": "cch_coding_agent terminal host (program-status reporter, #1051 product identity)",
        "tests": ["tests/tui/ProcessTerminalTest.cpp"],
    },
    "ProcessTerminal.stop": {
        "implementation": "partial",
        "artifact": PROGRAM_DETECTION_ARTIFACT,
        "scenario": "default-reply-before-device-attributes",
        "result": "frozen stop() clears a supported program status before disabling bracketed paste",
        "gaps": ["C++ stop() has no program-status clear"],
        "consumer": "cch_coding_agent terminal host",
    },
    "ProcessTerminal.handleKeyboardProtocolNegotiationSequence": {
        "implementation": "partial",
        "artifact": PROGRAM_DETECTION_ARTIFACT,
        "scenario": "default-device-attributes-before-reply",
        "result": (
            "frozen owns the DA sentinel counter so a DA reply ends the program-status query and a late "
            "reply is swallowed"
        ),
        "gaps": ["C++ negotiation handling has no program-status pending sentinel"],
        "consumer": "cch_tui ProcessTerminal",
    },
    "parseKey": {
        "implementation": "partial",
        "artifact": KEYS_ARTIFACT,
        "scenario": "shifted-symbol-keys-match-produced-character",
        "result": (
            "frozen parseKey() returns the produced shifted symbol (e.g. '+' for Shift+=) and removes the "
            "implicit Shift; C++ keeps shift=true on the '+' identity"
        ),
        "gaps": [
            "C++ parse_key keeps the implicit Shift modifier on a shifted symbol identity",
            "C++ key_id cannot round-trip ctrl++ from a shifted-symbol report",
            "C++ has no v1.3.0 comparison for shifted-symbol key reports",
        ],
        "consumer": "cch_coding_agent keybinding context and product shortcuts",
        "tests": ["tests/tui/KeysTest.cpp", "tests/tui/KeysKittyTextTest.cpp", "tests/tui/KeysLegacyTest.cpp"],
    },
    "matchesKey": {
        "implementation": "partial",
        "artifact": KEYS_ARTIFACT,
        "scenario": "shifted-symbol-keys-match-produced-character",
        "result": (
            "frozen matchesKey() matches '+' and 'shift+=' for a Kitty shifted '=' report and 'ctrl++' "
            "for ctrl+shift+= or modifyOtherKeys; C++ matches only the physical spelling"
        ),
        "gaps": [
            "C++ matches_key compares the explicit shift flag, so ctrl++/shift-symbol bindings fail",
            "C++ has no v1.3.0 comparison for shifted-symbol matching",
        ],
        "consumer": "cch_coding_agent keybinding context",
        "tests": ["tests/tui/KeysTest.cpp", "tests/tui/KeysLegacyTest.cpp"],
    },
    "detectCapabilities": {
        "implementation": "partial",
        "artifact": TERMINAL_CAPS_ARTIFACT,
        "scenario": "herdr-detected-before-outer-terminal",
        "result": (
            "frozen detects TERM_PROGRAM=herdr before outer-terminal/KITTY_WINDOW_ID detection and reports "
            "Kitty images plus OSC 8 hyperlinks; C++ has no Herdr branch"
        ),
        "gaps": [
            "C++ detect_image_capabilities has no Herdr branch; a plain Herdr pane reports no images/hyperlinks",
            "C++ does not check Herdr before the leaked-KITTY_WINDOW_ID path",
            "C++ has no v1.3.0 comparison for Herdr capability detection",
        ],
        "consumer": "cch_tui ProcessTerminal capabilities; application hyperlink()/renderImage consumers (#1051)",
        "tests": ["tests/tui/TerminalImageTest.cpp"],
    },
    "CURSOR_MARKER": {
        "implementation": "partial",
        "artifact": CURSOR_ARTIFACT,
        "scenario": "overlay-left-of-cursor-keeps-cursor-fake",
        "result": (
            "frozen CURSOR_MARKER remains a zero-width APC marker; v1.2.0 adds FAKE_CURSOR_START/END and "
            "resolves them in the TUI; C++ uses out-of-band cursor_location() metadata instead"
        ),
        "gaps": [
            "C++ representation keeps cursor positioning out of band and has no fake-cursor marker pair",
            "C++ has no v1.3.0 comparison for marker-adjacent overlay composition",
        ],
        "boundary": "C++ representation: out-of-band cursor_location() metadata replaces CURSOR_MARKER/fake-cursor strings",
        "consumer": "cch_tui Input/Editor cursor metadata; cch_coding_agent custom editors",
        "tests": ["tests/tui/HardwareCursorEvidenceTest.cpp", "tests/tui/OverlayTest.cpp"],
    },
    "TUI.getShowHardwareCursor": {
        "implementation": "implemented",
        "artifact": HARDWARE_ARTIFACT,
        "scenario": "main-screen-default-hidden",
        "result": "hardware cursor hidden by default; the same frozen scenario is re-captured at v1.3.0",
        "gaps": ["no v1.3.0 C++ comparison recorded"],
        "consumer": "cch_coding_agent settings and InteractiveEngine",
        "tests": ["tests/tui/HardwareCursorEvidenceTest.cpp"],
        "defaults": "false",
    },
    "TUI.setShowHardwareCursor": {
        "implementation": "implemented",
        "artifact": HARDWARE_ARTIFACT,
        "scenario": "main-screen-toggle",
        "result": "preference toggles cursor visibility; the same frozen scenario is re-captured at v1.3.0",
        "gaps": ["no v1.3.0 C++ comparison recorded"],
        "consumer": "cch_coding_agent settings and InteractiveEngine",
        "tests": ["tests/tui/HardwareCursorEvidenceTest.cpp"],
    },
}

NEW_EXPORTS = {
    "renderFakeCursor": {
        "path": "packages/tui/src/tui.ts",
        "kind": "function",
        "membership": "supported",
        "implementation": "missing",
        "verification": "upstream-captured",
        "owner": "cch_tui",
        "consumer": "none (no C++ caller; C++ draws the cursor as raw reverse video)",
        "boundary": "C++ representation: out-of-band cursor_location() metadata; no fake-cursor marker contract",
        "implementation_ticket": "#1058-draft",
        "artifact": CURSOR_ARTIFACT,
        "scenario": "standalone-input-fake-cursor-is-strippable",
        "result": "frozen renderFakeCursor wraps the cursor cell in zero-width APC markers that strip cleanly",
        "gaps": [
            "C++ has no renderFakeCursor equivalent or marker contract",
            "C++ Editor/Input still draw raw reverse-video cursors",
            "no v1.3.0 C++ comparison recorded",
        ],
        "tests": [],
        "notes": "Frozen consumer: Editor, Input and the extension overlay example.",
    },
    "formatProgramStatus": {
        "path": "packages/tui/src/program-status.ts",
        "kind": "function",
        "membership": "supported",
        "implementation": "missing",
        "verification": "upstream-captured",
        "owner": "cch_tui",
        "consumer": "none (C++ Terminal has no set_program_status; application reporter owned by #1051)",
        "boundary": None,
        "implementation_ticket": "#1058-draft",
        "artifact": PROGRAM_FORMAT_ARTIFACT,
        "scenario": "format-program-status-encodes-fields-and-limits",
        "result": "frozen formatProgramStatus encodes state/app/kind/base64 message; control characters become spaces and the message is truncated at a UTF-8 boundary to 2048 bytes",
        "gaps": [
            "C++ has no OSC 7501 formatter",
            "C++ Terminal/ProcessTerminal have no program-status report path",
            "no v1.3.0 C++ comparison recorded",
        ],
        "tests": [],
        "defaults": "no report until setProgramStatus(); clear removes it",
    },
    "ProgramStatus": {
        "path": "packages/tui/src/program-status.ts",
        "kind": "type",
        "membership": "supported",
        "implementation": "missing",
        "verification": "upstream-captured",
        "owner": "cch_tui",
        "consumer": "none (no C++ program-status type)",
        "boundary": None,
        "implementation_ticket": "#1058-draft",
        "artifact": PROGRAM_FORMAT_ARTIFACT,
        "scenario": "format-program-status-encodes-fields-and-limits",
        "result": "frozen ProgramStatus is {state: idle|working|blocked|done|error|clear, app?, kind?, message?}",
        "gaps": ["C++ has no ProgramStatus equivalent"],
        "tests": [],
    },
}

NEW_MEMBERS = [
    {
        "export": "Box",
        "member": "setPaddingX",
        "path": "packages/tui/src/components/box.ts",
        "kind": "method/field",
        "membership": "supported",
        "implementation": "missing",
        "verification": "upstream-captured",
        "owner": "cch_tui",
        "consumer": "none (C++ Box has no setter; application outputPad callers are #1051 rows)",
        "boundary": None,
        "implementation_ticket": "#1058-draft",
        "artifact": PADDING_ARTIFACT,
        "scenario": "box-set-padding-x-rerenders-dynamic-width",
        "result": "frozen setPaddingX invalidates the cache and re-renders the padded box at the new width",
        "gaps": [
            "C++ Box only takes padding at construction; no set_padding_x",
            "C++ application outputPad/tool renderers cannot update Box padding dynamically",
            "no v1.3.0 C++ comparison recorded",
        ],
        "tests": ["tests/tui/ContainerTest.cpp"],
        "defaults": "paddingX=1, paddingY=1",
    },
    {
        "export": "Text",
        "member": "setPaddingX",
        "path": "packages/tui/src/components/text.ts",
        "kind": "method/field",
        "membership": "supported",
        "implementation": "implemented",
        "verification": "upstream-captured",
        "owner": "cch_tui",
        "consumer": "none yet (C++ Text::set_padding_x exists; no product caller)",
        "boundary": None,
        "implementation_ticket": None,
        "artifact": PADDING_ARTIFACT,
        "scenario": "text-set-padding-x-rerenders-dynamic-width",
        "result": "frozen setPaddingX invalidates and re-wraps at the new horizontal padding",
        "gaps": ["no v1.3.0 C++ comparison recorded"],
        "tests": ["tests/tui/ContainerTest.cpp"],
        "defaults": "paddingX=1, paddingY=1",
    },
    {
        "export": "TuiAltScreen",
        "member": "resetTextSelection",
        "path": "packages/tui/src/tui-alt-screen.ts",
        "kind": "method/field",
        "membership": "supported",
        "implementation": "missing",
        "verification": "upstream-captured",
        "owner": "cch_tui",
        "consumer": "none (C++ clear_text_selection does not reset the multi-click history)",
        "boundary": None,
        "implementation_ticket": "#1058-draft",
        "artifact": PADDING_ARTIFACT,
        "scenario": "selection-reset-drops-selection-and-multiclick-history",
        "result": "frozen resetTextSelection drops the selection and lastClick, so the next click is a fresh character click",
        "gaps": [
            "C++ has no public resetTextSelection() combining selection and multi-click reset",
            "C++ has no v1.3.0 comparison for the reset outcome",
        ],
        "tests": ["tests/tui/SelectionEvidenceTest.cpp"],
        "defaults": "multi-click interval 500ms; selection granularity starts as character",
    },
    {
        "export": "Terminal",
        "member": "setProgramStatus",
        "path": "packages/tui/src/terminal.ts",
        "kind": "method/field",
        "membership": "supported",
        "implementation": "missing",
        "verification": "upstream-captured",
        "owner": "cch_tui",
        "consumer": "none (C++ Terminal interface has no program-status method)",
        "boundary": None,
        "implementation_ticket": "#1058-draft",
        "artifact": PROGRAM_DETECTION_ARTIFACT,
        "scenario": "default-reply-before-device-attributes",
        "result": "frozen Terminal implementations must provide setProgramStatus(status) (no-op without OSC 7501)",
        "gaps": ["C++ Terminal has no set_program_status capability"],
        "tests": [],
    },
    {
        "export": "ProcessTerminal",
        "member": "setProgramStatus",
        "path": "packages/tui/src/terminal.ts",
        "kind": "method/field",
        "membership": "supported",
        "implementation": "missing",
        "verification": "upstream-captured",
        "owner": "cch_tui",
        "consumer": "none (C++ ProcessTerminal has no program-status report)",
        "boundary": None,
        "implementation_ticket": "#1058-draft",
        "artifact": PROGRAM_DETECTION_ARTIFACT,
        "scenario": "default-reply-before-device-attributes",
        "result": "frozen setProgramStatus stores the latest status and writes it only when support is confirmed",
        "gaps": ["C++ ProcessTerminal has no program-status state or write"],
        "tests": [],
    },
    {
        "export": "ProcessTerminal",
        "member": "queryAndEnableKittyProtocol",
        "path": "packages/tui/src/terminal.ts",
        "kind": "method/field",
        "membership": "supported",
        "implementation": "missing",
        "verification": "upstream-captured",
        "owner": "cch_tui",
        "consumer": "none (no C++ program-status query)",
        "boundary": None,
        "implementation_ticket": "#1058-draft",
        "artifact": PROGRAM_DETECTION_ARTIFACT,
        "scenario": "default-reply-before-device-attributes",
        "result": "frozen sends the OSC 7501 query before DA1 unless PI_PROGRAM_STATUS=1|0",
        "gaps": ["C++ ProcessTerminal query path is missing"],
        "tests": ["tests/tui/ProcessTerminalTest.cpp"],
        "defaults": "query sent; PI_PROGRAM_STATUS unset",
    },
    {
        "export": "ProcessTerminal",
        "member": "setupStdinBuffer",
        "path": "packages/tui/src/terminal.ts",
        "kind": "method/field",
        "membership": "supported",
        "implementation": "missing",
        "verification": "upstream-captured",
        "owner": "cch_tui",
        "consumer": "none",
        "boundary": None,
        "implementation_ticket": "#1058-draft",
        "artifact": PROGRAM_DETECTION_ARTIFACT,
        "scenario": "default-reply-before-device-attributes",
        "result": "frozen consumes an OSC 7501 reply before key decoding and writes the pending status once",
        "gaps": ["C++ input edge has no program-status reply interception"],
        "tests": [],
    },
    {
        "export": "ProcessTerminal",
        "member": "writeProgramStatus",
        "path": "packages/tui/src/terminal.ts",
        "kind": "method/field",
        "membership": "supported",
        "implementation": "missing",
        "verification": "upstream-captured",
        "owner": "cch_tui",
        "consumer": "none",
        "boundary": None,
        "implementation_ticket": "#1058-draft",
        "artifact": PROGRAM_DETECTION_ARTIFACT,
        "scenario": "override-enabled",
        "result": "frozen re-sends the pending status when support is confirmed or after restart",
        "gaps": ["C++ has no pending-status write"],
        "tests": [],
    },
]

# The seven application product-entry rows are not observed by the #1051 application lane. Three
# #1051 capability rows are adjacent in the same product area; only two product entries name one as
# an adjacent counterpart, and neither observes the 1050 setting, while the other five product
# entries have no #1051 row for their own operation. Each gap states that per row, so this lane
# never claims a 1051 audit it does not have. The key set is checked against the historical
# product-entry ids below, so a new entry cannot inherit a stale claim.
PRODUCT_ENTRY_GAPS = {
    "product.tuiMode": (
        "no #1051 row observes the tuiMode setting or the regular/fullscreen default; "
        "v1.3.0 product behavior is unaudited"
    ),
    "product.--tui-mode": (
        "no #1051 row observes the --tui-mode CLI flag; v1.3.0 product behavior is unaudited"
    ),
    "product.runInteractiveMode": (
        "no #1051 row observes runInteractiveMode assembly; the mode-switch-renderer row observes a "
        "live renderer switch, not this entry; v1.3.0 product behavior is unaudited"
    ),
    "product.fullscreenExitOutput": (
        "only the adjacent #1051 exit-restoration row touches exit output and terminal "
        "restoration, and it does not observe this setting; v1.3.0 product behavior is unaudited"
    ),
    "product.fullscreenCopyOnSelect": (
        "no #1051 row observes copy-on-select; v1.3.0 product behavior is unaudited"
    ),
    "product.fullscreenWheelScrollLines": (
        "no #1051 row observes the fullscreenWheelScrollLines setting; v1.3.0 product behavior is "
        "unaudited"
    ),
    "product.showHardwareCursor": (
        "only the adjacent #1051 hardware-cursor row touches the toolkit cursor switch, and it "
        "does not observe this setting; v1.3.0 product behavior is unaudited"
    ),
}

CPP_INTERFACE_OVERRIDE = {
    "Editor.render": "cch_tui::Editor::render / cch_tui::EditorLayout",
    "Input.render": "cch_tui::Input::render",
    "TuiAltScreen.render": "cch_tui::AltScreen::render_now",
    "TuiAltScreen.applySelection": "cch_tui::AltScreen::apply_text_selection",
    "TuiAltScreen.handleViewportInput": "cch_tui::AltScreen::handle_wheel_report",
    "TuiAltScreen.shouldDeferViewportInputToOverlay": "cch_tui::AltScreen::should_defer_viewport_input_to_overlay",
    "TuiAltScreen.clearTextSelection": "cch_tui::AltScreen::clear_text_selection",
    "TuiAltScreen.lastClick": "cch_tui::AltScreen multi-click selection state",
    "TuiMainScreen.doRender": "cch_tui::MainScreen::render",
    "StdinBuffer.process": "cch_tui::StdinBuffer::process",
    "StdinBuffer.flush": "cch_tui::StdinBuffer::flush",
    "ProcessTerminal.start": "cch_tui::ProcessTerminal::start",
    "ProcessTerminal.stop": "cch_tui::ProcessTerminal::stop",
    "ProcessTerminal.handleKeyboardProtocolNegotiationSequence": "cch_tui::ProcessTerminal keyboard-protocol negotiation",
    "parseKey": "cch_tui::parse_key",
    "matchesKey": "cch_tui::matches_key",
    "detectCapabilities": "cch_tui::detect_image_capabilities",
    "CURSOR_MARKER": "cch_tui::Component::cursor_location (out-of-band)",
    "TUI.getShowHardwareCursor": "cch_tui::ITui::show_hardware_cursor / get_show_hardware_cursor",
    "TUI.setShowHardwareCursor": "cch_tui::ITui::set_show_hardware_cursor",
    "TUI.showOverlay": "cch_tui::MainScreen::show_overlay / cch_tui::AltScreen::show_overlay",
    "renderFakeCursor": "none (C++ draws raw reverse video; cursor_location() is out of band)",
    "formatProgramStatus": "none (no C++ OSC 7501 formatter)",
    "ProgramStatus": "none (no C++ program-status type)",
    "Box.setPaddingX": "cch_tui::Box (no set_padding_x)",
    "Text.setPaddingX": "cch_tui::Text::set_padding_x",
    "TuiAltScreen.resetTextSelection": "cch_tui::AltScreen (no reset_text_selection)",
    "Terminal.setProgramStatus": "cch_tui::Terminal (no set_program_status)",
    "ProcessTerminal.setProgramStatus": "cch_tui::ProcessTerminal (no set_program_status)",
    "ProcessTerminal.queryAndEnableKittyProtocol": "cch_tui::ProcessTerminal (no OSC 7501 query)",
    "ProcessTerminal.setupStdinBuffer": "cch_tui::ProcessTerminal input edge (no reply interception)",
    "ProcessTerminal.writeProgramStatus": "none (no pending program-status write)",
    "cch_tui:tui:resolveFakeCursors": "none (no C++ fake-cursor resolution stage)",
    "cch_tui:tui:fake-cursor-marker-constants": "none (C++ has no marker constants)",
    "cch_tui:utils:extractSegments": "cch_tui::slice_by_column",
    "cch_tui:terminal-image:detectCapabilitiesFromEnvironment": "cch_tui::detect_image_capabilities",
    "cch_tui:keys:shifted-symbol-matching": "cch_tui::parse_key / cch_tui::matches_key",
    "cch_tui:stdin-buffer:fragmented-terminal-replies": "cch_tui::StdinBuffer",
    "cch_tui:program-status:detection-override": "none (C++ Terminal/ProcessTerminal have no OSC 7501 path)",
}

EXTRA_ROWS = [
    {
        "id": "cch_tui:tui:resolveFakeCursors",
        "capability": "TuiBase.resolveFakeCursors",
        "source": ["packages/tui/src/tui.ts:TuiBase.resolveFakeCursors"],
        "kind": "internal-helper",
        "membership": "supported",
        "implementation": "missing",
        "verification": "upstream-captured",
        "owner": "cch_tui",
        "consumer": "none (C++ TUI roots have no fake-cursor resolution stage)",
        "boundary": "C++ representation: out-of-band cursor metadata",
        "implementation_ticket": "#1058-draft",
        "artifact": CURSOR_ARTIFACT,
        "scenario": "truncated-line-drops-unterminated-fake-cursor",
        "result": "frozen resolves markers to reverse video and drops a focused fake cursor without its end marker",
        "gaps": ["C++ has no marker resolution or unterminated-marker handling"],
        "tests": ["tests/tui/InputTest.cpp"],
    },
    {
        "id": "cch_tui:tui:fake-cursor-marker-constants",
        "capability": "FAKE_CURSOR_START / FAKE_CURSOR_END",
        "source": ["packages/tui/src/tui.ts:FAKE_CURSOR_START,FAKE_CURSOR_END"],
        "kind": "const",
        "membership": "supported",
        "implementation": "missing",
        "verification": "upstream-captured",
        "owner": "cch_tui",
        "consumer": "none (C++ has no marker strings)",
        "boundary": "C++ representation: out-of-band cursor metadata",
        "implementation_ticket": "#1058-draft",
        "artifact": CURSOR_ARTIFACT,
        "scenario": "standalone-input-fake-cursor-is-strippable",
        "result": "frozen markers are zero-width APC sequences \\x1b_pi:fc\\x07 and \\x1b_pi:/fc\\x07",
        "gaps": ["C++ does not expose or consume fake-cursor marker constants"],
        "tests": [],
    },
    {
        "id": "cch_tui:utils:extractSegments",
        "capability": "extractSegments APC boundary",
        "source": ["packages/tui/src/utils.ts:extractSegments"],
        "kind": "internal-helper",
        "membership": "supported",
        "implementation": "partial",
        "verification": "upstream-captured",
        "owner": "cch_tui",
        "consumer": "cch_tui sliceByColumn/truncateToWidth callers",
        "boundary": "C++ representation: no zero-width APC cursor marker in text",
        "implementation_ticket": "#1058-draft",
        "artifact": CURSOR_ARTIFACT,
        "scenario": "truncated-line-drops-unterminated-fake-cursor",
        "result": "frozen keeps a zero-width APC marker at the slice boundary so the cursor position survives truncation",
        "gaps": [
            "C++ slice_by_column has no marker-boundary case because the cursor is out of band",
            "no v1.3.0 C++ comparison for the truncated-cursor outcome",
        ],
        "tests": ["tests/tui/UtilsTest.cpp", "tests/tui/UtilsAnsiTest.cpp"],
    },
    {
        "id": "cch_tui:terminal-image:detectCapabilitiesFromEnvironment",
        "capability": "detectCapabilitiesFromEnvironment Herdr branch",
        "source": ["packages/tui/src/terminal-image.ts:detectCapabilitiesFromEnvironment"],
        "kind": "internal-helper",
        "membership": "supported",
        "implementation": "missing",
        "verification": "upstream-captured",
        "owner": "cch_tui",
        "consumer": "cch_tui detectCapabilities",
        "boundary": None,
        "implementation_ticket": "#1058-draft",
        "artifact": TERMINAL_CAPS_ARTIFACT,
        "scenario": "herdr-detected-before-outer-terminal",
        "result": "frozen returns {images:'kitty', trueColor:<COLORTERM>, hyperlinks:true} for TERM_PROGRAM=herdr",
        "gaps": ["C++ detection has no Herdr branch and no Herdr-before-Kitty ordering"],
        "tests": ["tests/tui/TerminalImageTest.cpp"],
    },
    {
        "id": "cch_tui:keys:shifted-symbol-matching",
        "capability": "Shifted-symbol key matching",
        "source": [
            "packages/tui/src/keys.ts:parseKeyId,matchesKittySequence,matchesPrintableModifyOtherKeys,formatParsedKey",
        ],
        "kind": "capability",
        "membership": "supported",
        "implementation": "partial",
        "verification": "upstream-captured",
        "owner": "cch_tui",
        "consumer": "cch_coding_agent keybinding context",
        "boundary": None,
        "implementation_ticket": "#1058-draft",
        "artifact": KEYS_ARTIFACT,
        "scenario": "shifted-symbol-keys-match-produced-character",
        "result": "frozen matches the produced symbol and consumes the implicit Shift for Kitty/modifyOtherKeys reports",
        "gaps": [
            "C++ keeps the explicit Shift flag on the produced symbol",
            "C++ parse_key_id cannot express ctrl++ identities from a shifted-symbol report",
        ],
        "tests": ["tests/tui/KeysTest.cpp", "tests/tui/KeysLegacyTest.cpp"],
    },
    {
        "id": "cch_tui:stdin-buffer:fragmented-terminal-replies",
        "capability": "Fragmented terminal reply buffering",
        "source": ["packages/tui/src/stdin-buffer.ts:isControlString,TERMINAL_REPLY_HEADER_PATTERN,extractCompleteSequences"],
        "kind": "capability",
        "membership": "supported",
        "implementation": "partial",
        "verification": "upstream-captured",
        "owner": "cch_tui",
        "consumer": "cch_tui ProcessTerminal input edge; cch_coding_agent prompt/editor",
        "boundary": None,
        "implementation_ticket": "#1058-draft",
        "artifact": KEYS_ARTIFACT,
        "scenario": "fragmented-color-reply-never-leaks-or-opens-editor",
        "result": "frozen buffers OS C 4/10/11/7501, XTVersion and Kitty graphics replies until their terminator",
        "gaps": [
            "C++ flushes recognized reply headers at the timeout",
            "C++ delivers the reply tail and BEL as prompt text/Ctrl+G",
        ],
        "tests": ["tests/tui/StdinBufferTest.cpp"],
    },
    {
        "id": "cch_tui:program-status:detection-override",
        "capability": "Program status detection and override",
        "source": [
            "packages/tui/src/program-status.ts:PROGRAM_STATUS_QUERY,isProgramStatusReply",
            "packages/tui/src/terminal.ts:ProcessTerminal.queryAndEnableKittyProtocol,setProgramStatus,stop",
        ],
        "kind": "capability",
        "membership": "supported",
        "implementation": "missing",
        "verification": "upstream-captured",
        "owner": "cch_tui",
        "consumer": "none (cch_coding_agent program-status reporter is a #1051 product consumer)",
        "boundary": None,
        "implementation_ticket": "#1058-draft",
        "artifact": PROGRAM_DETECTION_ARTIFACT,
        "scenario": "default-reply-before-device-attributes",
        "result": "frozen queries OSC 7501 before DA1, honors PI_PROGRAM_STATUS=1|0, swallows late replies and clears on stop",
        "gaps": [
            "C++ has no OSC 7501 query, override, pending-status write or stop clear",
            "no v1.3.0 C++ comparison recorded",
        ],
        "tests": [],
        "defaults": "query sent when PI_PROGRAM_STATUS is unset; reports only after a supporting reply",
    },
]


def historical_evidence(scenario: str, result: str, route: str = "unchanged-frozen-source") -> dict:
    return {
        "artifact": HISTORICAL_BUNDLE,
        "scenario": scenario,
        "result": result,
        "historicalBaseline": "pi-v1.0.4",
        "historicalRevision": HISTORICAL_REVISION,
        "route": route,
    }


def make_evidence(artifact: str, scenario: str, result: str) -> dict:
    return {"artifact": artifact, "scenario": scenario, "result": result, "baseline": BASELINE, "revision": REVISION}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pi-checkout", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=Path("docs/research/pi-v1.3.0/1050-toolkit-capability-ledger.json"))
    args = parser.parse_args()

    pi = args.pi_checkout.resolve()
    head = run_git(pi, "rev-parse", "HEAD").strip()
    if head != REVISION:
        raise SystemExit(f"pi checkout must be at {REVISION}, found {head}")
    if run_git(pi, "status", "--porcelain").strip():
        raise SystemExit("pi ledger build requires an unmodified frozen checkout")

    index_text = run_git(pi, "show", f"{REVISION}:packages/tui/src/index.ts")
    exports = index_exports(index_text)
    historical = json.loads(Path(HISTORICAL_LEDGER).read_text())
    changed_files = {
        line
        for line in run_git(
            pi, "diff", "--name-only", HISTORICAL_REVISION, REVISION, "--", "packages/tui", "packages/coding-agent"
        ).splitlines()
        if line.startswith("packages/tui/src/") or line.startswith("packages/coding-agent/src/")
    }

    def file_changed(path: str) -> bool:
        return path in changed_files

    diff_cache: dict[str, str] = {}

    def diff_text(path: str) -> str:
        if path not in diff_cache:
            diff_cache[path] = run_git(pi, "diff", "-U0", HISTORICAL_REVISION, REVISION, "--", path)
        return diff_cache[path]

    def mentioned_in_diff(path: str, symbol: str) -> bool:
        return bool(re.search(r"(?<![A-Za-z0-9_])" + re.escape(symbol) + r"(?![A-Za-z0-9_])", diff_text(path)))

    def cpp_owner(historical_owner: str) -> str:
        if historical_owner == "cch_tui":
            return "cch_tui"
        if historical_owner == "cch_coding_agent":
            return "cch_coding_agent"
        if historical_owner.startswith("frontend_") or "frontend_" in historical_owner:
            return "cch_coding_agent"
        return "cch_tui"

    # Export name -> historical index export row (member is None). Product/internal/diagnostic
    # rows are accounted separately below because they are not package entry exports.
    INDEX_EXPORT_KINDS = {"function", "interface", "type", "class", "const", "excluded-reexport"}
    historical_exports = {
        row["export"]: row
        for row in historical["rows"]
        if row.get("member") is None and row["kind"] in INDEX_EXPORT_KINDS
    }
    historical_non_index = [
        row for row in historical["rows"] if row.get("member") is None and row["kind"] not in INDEX_EXPORT_KINDS
    ]
    product_entry_ids = {
        row["id"] for row in historical_non_index if row["path"].startswith("packages/coding-agent/")
    }
    if product_entry_ids != set(PRODUCT_ENTRY_GAPS):
        raise SystemExit(
            "product-entry gap set drifted: "
            f"missing={sorted(product_entry_ids - set(PRODUCT_ENTRY_GAPS))} "
            f"extra={sorted(set(PRODUCT_ENTRY_GAPS) - product_entry_ids)}"
        )
    if set(historical_exports) != set(exports):
        missing = sorted(set(exports) - set(historical_exports))
        extra = sorted(set(historical_exports) - set(exports))
        new = [name for name in missing if name in NEW_EXPORTS]
        if set(missing) - set(new) or extra:
            raise SystemExit(f"export inventory drifted: missing={missing} extra={extra}")

    tests_root = Path("tests")
    test_cache: dict[str, list[str]] = {}

    def test_pointers(name: str) -> list[str]:
        if name in test_cache:
            return test_cache[name]
        pattern = re.compile(r"(?<![A-Za-z0-9_])" + re.escape(name) + r"(?![A-Za-z0-9_])")
        found = []
        for path in sorted(tests_root.glob("**/*.cpp")):
            try:
                text = path.read_text(errors="ignore")
            except OSError:
                continue
            if pattern.search(text):
                found.append(str(path))
        test_cache[name] = found[:3]
        return test_cache[name]

    rows: list[dict] = []
    export_order = list(exports)

    def base_row(
        row_id: str,
        capability: str,
        sources: list[str],
        kind: str,
        membership: str,
        implementation: str,
        verification: str,
        owner: str,
        consumer: str,
        boundary,
        implementation_ticket,
        artifact: str | None,
        scenario: str | None,
        result: str | None,
        gaps: list[str],
        notes: list[str],
        tests: list[str],
        export: str | None = None,
        member: str | None = None,
        classification: str | None = None,
        historical_tickets=None,
        public_operations=None,
        type_only: bool = False,
        source_changed: bool = False,
        defaults: str | None = None,
        comparison_route: str = "not-applicable",
        source_owner: str | None = None,
    ) -> dict:
        payload = {
            "id": row_id,
            "capability": capability,
            "source": sources,
            "membership": membership,
            "implementation": implementation,
            "verification": verification,
            "owner": owner,
            "sourceOwner": source_owner,
            "consumer": consumer,
            "boundary": boundary,
            "implementation_ticket": implementation_ticket,
            "scenario": scenario,
            "evidence": make_evidence(artifact, scenario, result) if artifact and scenario and result else None,
            "gaps": gaps,
            "notes": " ".join(note for note in notes if note),
            "kind": kind,
            "export": export,
            "member": member,
            "historicalClassification": classification,
            "historicalTickets": historical_tickets or [],
            "historicalEvidence": None,
            "publicOperations": public_operations or [],
            "typeOnly": type_only,
            "sourceChangedSinceV1.0.4": source_changed,
            "comparisonRoute": comparison_route,
            "cpp": {"interface": None, "consumer": consumer, "tests": tests},
        }
        if defaults:
            payload["defaults"] = defaults
        return payload

    for export_name in export_order:
        historical_export = historical_exports.get(export_name)
        module = exports[export_name]
        export_changed = file_changed(module)
        if historical_export is None:
            spec = NEW_EXPORTS[export_name]
            row = base_row(
                row_id=export_name,
                capability=f"{export_name} ({spec['kind']})",
                sources=[endpoint(spec["path"], export_name)],
                kind=spec["kind"],
                membership=spec["membership"],
                implementation=spec["implementation"],
                verification=spec["verification"],
                owner=spec["owner"],
                consumer=spec["consumer"],
                boundary=spec["boundary"],
                implementation_ticket=spec["implementation_ticket"],
                artifact=spec["artifact"],
                scenario=spec["scenario"],
                result=spec["result"],
                gaps=spec["gaps"],
                notes=["v1.3.0 addition; absent from the v1.0.4 index."],
                tests=spec.get("tests", []),
                export=export_name,
                source_changed=True,
                defaults=spec.get("defaults"),
                comparison_route="v1.3.0-observation",
            )
            row["cpp"]["interface"] = "missing"
            rows.append(row)
            continue

        classification = historical_export["classification"]
        membership = "excluded" if classification == "approved-exclusion" else "supported"
        implementation = "missing" if membership == "excluded" else "implemented"
        verification = "not-applicable" if membership == "excluded" else "unverified"
        historical_owner = historical_export["owning_package"]
        owner = cpp_owner(historical_owner)
        notes = [historical_export.get("notes", "")]
        boundary = historical_export.get("cpp_contract") or None
        artifact = HISTORICAL_BUNDLE if membership == "supported" else None
        scenario = historical_export.get("evidence_scenario", "")
        result = (
            "historical pi-v1.0.4 evidence family; this row has no dedicated v1.3.0 observation"
            if membership == "supported"
            else historical_export.get("notes") or "approved exclusion"
        )
        gaps: list[str] = []
        tests = test_pointers(export_name)
        defaults = None
        implementation_ticket = None
        if membership == "excluded":
            notes.append("v1.0.4 approved exclusion retained; no frozen-source change in this row.")
        elif export_changed and mentioned_in_diff(module, export_name):
            notes.append("Frozen source file changed since v1.0.4 and the diff names this export; no dedicated v1.3.0 observation was captured for its unchanged operations.")
            gaps = [
                "no v1.3.0 C++ comparison recorded",
                "export name appears in changed source hunks; not individually re-audited against v1.3.0",
            ]
        elif export_changed:
            notes.append("Frozen source file changed since v1.0.4, but the v1.0.4..v1.3.0 diff does not name this export; historical implementation carried with the diff as comparison route.")
            gaps = [
                "no v1.3.0 C++ comparison recorded",
                "no dedicated v1.3.0 observation; comparison route is the frozen diff, which does not touch this export",
            ]
        else:
            notes.append("Frozen source file is identical between v1.0.4 and v1.3.0; implementation state is carried from the v1.0.4 delivery without relabelling its observation.")
            gaps = [
                "no v1.3.0 C++ comparison recorded",
                "no separate v1.3.0 observation; comparison route is the unchanged frozen source plus the historical v1.0.4 bundle",
            ]
        override = CHANGE_MAP.get(export_name)
        if override and membership == "supported":
            implementation = override["implementation"]
            verification = "upstream-captured"
            artifact = override["artifact"]
            scenario = override["scenario"]
            result = override["result"]
            gaps = list(override["gaps"])
            boundary = override.get("boundary", boundary)
            defaults = override.get("defaults")
            tests = override.get("tests", tests)
            notes.append("v1.3.0 behavior change applied from the curated change map.")
            if implementation in ("partial", "missing"):
                implementation_ticket = "#1058-draft"
        row = base_row(
            row_id=historical_export["id"],
            capability=f"{export_name} ({historical_export['kind']})",
            sources=[endpoint(historical_export["path"], export_name)],
            kind=historical_export["kind"],
            membership=membership,
            implementation=implementation,
            verification=verification,
            owner=owner,
            consumer=historical_export["consumer"],
            boundary=boundary,
            implementation_ticket=implementation_ticket,
            artifact=artifact,
            scenario=scenario or None,
            result=result,
            gaps=gaps,
            notes=notes,
            tests=tests,
            export=export_name,
            classification=classification,
            historical_tickets=historical_export.get("tickets", []),
            public_operations=list(historical_export.get("public_operations", [])),
            type_only=bool(historical_export.get("type_only")),
            source_changed=export_changed,
            defaults=defaults,
            source_owner=historical_owner,
            comparison_route=(
                "not-applicable"
                if membership == "excluded"
                else "v1.3.0-observation"
                if override
                else "member-absent-from-diff"
                if export_changed and not mentioned_in_diff(module, export_name)
                else "unchanged-frozen-source"
                if not export_changed
                else "not-audited"
            ),
        )
        if membership == "excluded":
            row["evidence"] = None
            row["cpp"]["interface"] = "none (approved exclusion)"
        elif override:
            row["evidence"] = make_evidence(artifact, scenario, result)
        else:
            route_is_historical = not export_changed or not mentioned_in_diff(module, export_name)
            row["evidence"] = (
                historical_evidence(
                    scenario,
                    result,
                    route="member-absent-from-diff" if export_changed else "unchanged-frozen-source",
                )
                if scenario and route_is_historical
                else None
            )
        rows.append(row)

        # Member rows for this export.
        member_rows = [item for item in historical["rows"] if item.get("member") is not None and item["export"] == export_name]
        for historical_member in member_rows:
            member = historical_member["member"]
            member_key = f"{export_name}.{member}"
            override = CHANGE_MAP.get(member_key)
            member_classification = historical_member["classification"]
            member_membership = "excluded" if member_classification == "approved-exclusion" else "supported"
            member_implementation = "missing" if member_membership == "excluded" else "implemented"
            member_verification = "not-applicable" if member_membership == "excluded" else "unverified"
            member_artifact = None
            member_scenario = historical_member.get("evidence_scenario", "")
            member_result = None
            member_gaps: list[str] = []
            member_notes = [historical_member.get("notes", "")]
            member_consumer = historical_member["consumer"]
            member_tests = test_pointers(export_name)
            member_boundary = historical_member.get("cpp_contract") or None
            member_defaults = None
            member_implementation_ticket = None
            if member_membership == "excluded":
                member_notes.append("v1.0.4 approved exclusion retained.")
            elif override:
                member_implementation = override["implementation"]
                member_verification = "upstream-captured"
                member_artifact = override["artifact"]
                member_scenario = override["scenario"]
                member_result = override["result"]
                member_gaps = list(override["gaps"])
                member_consumer = override.get("consumer", member_consumer)
                member_tests = override.get("tests", member_tests)
                member_boundary = override.get("boundary", member_boundary)
                member_defaults = override.get("defaults")
                member_notes.append("v1.3.0 behavior change applied from the curated change map.")
                if member_implementation in ("partial", "missing"):
                    member_implementation_ticket = "#1058-draft"
            elif export_changed and mentioned_in_diff(historical_member["path"], member):
                member_notes.append("Frozen source file changed since v1.0.4 and the diff names this member; no dedicated v1.3.0 observation was captured for it.")
                member_gaps = [
                    "no v1.3.0 C++ comparison recorded",
                    "member name appears in changed source hunks; not individually re-audited against v1.3.0",
                ]
            elif export_changed:
                member_notes.append("Frozen source file changed since v1.0.4, but the diff does not name this member; historical implementation carried with the diff as comparison route.")
                member_gaps = [
                    "no v1.3.0 C++ comparison recorded",
                    "no dedicated v1.3.0 observation; comparison route is the frozen diff, which does not touch this member",
                ]
            else:
                member_notes.append("Frozen source file unchanged since v1.0.4; historical implementation/evidence carried.")
                member_gaps = ["no v1.3.0 C++ comparison recorded"]
            if member_membership == "supported" and not member_result:
                member_artifact = HISTORICAL_BUNDLE if not (export_changed and mentioned_in_diff(historical_member["path"], member)) else None
                member_result = (
                    "historical pi-v1.0.4 evidence family; no dedicated v1.3.0 observation for this member"
                    if not export_changed
                    else "historical pi-v1.0.4 evidence family; the v1.0.4..v1.3.0 diff does not name this member"
                    if member_artifact
                    else "file changed since v1.0.4 and the diff names this member; behavior not individually captured at v1.3.0"
                )
            row = base_row(
                row_id=historical_member["id"],
                capability=f"{export_name}.{member}",
                sources=[endpoint(historical_member["path"], f"{export_name}.{member}")],
                kind=historical_member["kind"],
                membership=member_membership,
                implementation=member_implementation,
                verification=member_verification,
                owner=owner,
                consumer=member_consumer,
                boundary=member_boundary,
                implementation_ticket=member_implementation_ticket,
                artifact=member_artifact,
                scenario=member_scenario or None,
                result=member_result,
                gaps=member_gaps if member_membership == "supported" else [],
                notes=member_notes,
                tests=member_tests,
                export=export_name,
                member=member,
                classification=member_classification,
                historical_tickets=historical_member.get("tickets", []),
                source_changed=export_changed,
                defaults=member_defaults,
                source_owner=historical_member.get("owning_package", historical_owner),
                comparison_route=(
                    "not-applicable"
                    if member_membership == "excluded"
                    else "v1.3.0-observation"
                    if override
                    else "member-absent-from-diff"
                    if export_changed and not mentioned_in_diff(historical_member["path"], member)
                    else "unchanged-frozen-source"
                    if not export_changed
                    else "not-audited"
                ),
            )
            if member_membership == "excluded":
                row["evidence"] = None
                row["cpp"]["interface"] = "none (approved exclusion)"
            elif override:
                row["evidence"] = make_evidence(member_artifact, member_scenario, member_result)
            else:
                row["evidence"] = (
                    historical_evidence(
                        member_scenario,
                        member_result,
                        route="member-absent-from-diff" if export_changed else "unchanged-frozen-source",
                    )
                    if member_artifact and member_scenario and member_result
                    else None
                )
            rows.append(row)

        # New members for this export.
        for spec in NEW_MEMBERS:
            if spec["export"] != export_name:
                continue
            row = base_row(
                row_id=f"{export_name}.{spec['member']}",
                capability=f"{export_name}.{spec['member']}",
                sources=[endpoint(spec["path"], f"{export_name}.{spec['member']}")],
                kind=spec["kind"],
                membership=spec["membership"],
                implementation=spec["implementation"],
                verification=spec["verification"],
                owner=spec["owner"],
                consumer=spec["consumer"],
                boundary=spec["boundary"],
                implementation_ticket=spec["implementation_ticket"],
                artifact=spec["artifact"],
                scenario=spec["scenario"],
                result=spec["result"],
                gaps=spec["gaps"],
                notes=["v1.3.0 addition; absent from the v1.0.4 ledger."],
                tests=spec.get("tests", []),
                export=export_name,
                member=spec["member"],
                source_changed=True,
                defaults=spec.get("defaults"),
                comparison_route="v1.3.0-observation",
            )
            row["cpp"]["interface"] = (
                "cch_tui::Box" if export_name == "Box" else
                "cch_tui::Text" if export_name == "Text" else
                "cch_tui::AltScreen" if export_name == "TuiAltScreen" else
                "cch_tui::Terminal" if export_name == "Terminal" else
                "cch_tui::ProcessTerminal"
            )
            rows.append(row)

    # Historical Linux-usable non-index capabilities (product entries, internal helpers,
    # diagnostics). They keep their v1.0.4 identity; every row source is checked against the
    # complete v1.0.4..v1.3.0 diff, including application sources.
    for historical_extra in historical_non_index:
        classification = historical_extra["classification"]
        membership = "excluded" if classification == "approved-exclusion" else "supported"
        implementation = "missing" if membership == "excluded" else "implemented"
        verification = "not-applicable" if membership == "excluded" else "unverified"
        source_changed = file_changed(historical_extra["path"])
        notes = [historical_extra.get("notes", "")]
        gaps: list[str] = []
        if membership == "excluded":
            notes.append("v1.0.4 approved platform exclusion retained.")
        elif source_changed:
            notes.append(
                "Frozen source file changed since v1.0.4; this non-index capability was not individually re-audited."
            )
            gaps = ["non-index capability not individually re-audited against v1.3.0"]
            if historical_extra["path"].startswith("packages/coding-agent/"):
                gaps.append(PRODUCT_ENTRY_GAPS[historical_extra["id"]])
        else:
            notes.append("Frozen source file unchanged since v1.0.4; historical implementation/evidence carried.")
            gaps = [
                "no v1.3.0 C++ comparison recorded",
                "no separate v1.3.0 observation; comparison route is the unchanged frozen source plus the historical v1.0.4 bundle",
            ]
        row = base_row(
            row_id=historical_extra["id"],
            capability=f"{historical_extra['id']} ({historical_extra['kind']})",
            sources=[endpoint(historical_extra["path"], historical_extra["id"])],
            kind=historical_extra["kind"],
            membership=membership,
            implementation=implementation,
            verification=verification,
            owner=cpp_owner(historical_extra["owning_package"]),
            consumer=historical_extra["consumer"],
            boundary=historical_extra.get("cpp_contract") or None,
            implementation_ticket=None,
            artifact=HISTORICAL_BUNDLE if membership == "supported" and not source_changed else None,
            scenario=historical_extra.get("evidence_scenario") or None,
            result=(
                "historical pi-v1.0.4 evidence family; this non-index capability has no dedicated v1.3.0 observation"
                if membership == "supported" and not source_changed
                else "source changed since v1.0.4; no dedicated v1.3.0 observation for this non-index capability"
                if membership == "supported"
                else historical_extra.get("notes") or "approved exclusion"
            ),
            gaps=gaps,
            notes=notes,
            tests=test_pointers(historical_extra["id"]),
            export=historical_extra["id"],
            source_changed=source_changed,
            source_owner=historical_extra["owning_package"],
            comparison_route=(
                "not-applicable" if membership == "excluded" else "not-audited" if source_changed else "unchanged-frozen-source"
            ),
        )
        if membership == "excluded":
            row["evidence"] = None
            row["cpp"]["interface"] = "none (approved exclusion)"
        elif source_changed:
            row["evidence"] = None
            if historical_extra.get("evidence_scenario"):
                row["historicalEvidence"] = historical_evidence(
                    historical_extra["evidence_scenario"],
                    "historical pi-v1.0.4 observation retained as provenance only; the frozen source changed, so it is not comparison evidence for v1.3.0",
                    route="historical-reference-only",
                )
        else:
            row["evidence"] = (
                historical_evidence(
                    historical_extra.get("evidence_scenario", ""),
                    row["evidence"]["result"],
                    route="unchanged-frozen-source",
                )
                if historical_extra.get("evidence_scenario")
                else None
            )
        rows.append(row)

    # Explicit capability/internal rows that have no historical export identity.
    for spec in EXTRA_ROWS:
        row = base_row(
            row_id=spec["id"],
            capability=spec["capability"],
            sources=spec["source"],
            kind=spec["kind"],
            membership=spec["membership"],
            implementation=spec["implementation"],
            verification=spec["verification"],
            owner=spec["owner"],
            consumer=spec["consumer"],
            boundary=spec["boundary"],
            implementation_ticket=spec["implementation_ticket"],
            artifact=spec["artifact"],
            scenario=spec["scenario"],
            result=spec["result"],
            gaps=spec["gaps"],
            notes=["v1.3.0 change row without a v1.0.4 export/member identity."],
            tests=spec.get("tests", []),
            export=None,
            source_changed=True,
            defaults=spec.get("defaults"),
            comparison_route="v1.3.0-observation",
        )
        row["cpp"]["interface"] = "missing" if spec["implementation"] == "missing" else "partial"
        rows.append(row)

    # Aggregate export rows from their members (curated rows are authoritative).
    by_export: dict[str, list[dict]] = {}
    for row in rows:
        if row.get("member") is not None and row.get("export"):
            by_export.setdefault(row["export"], []).append(row)
    rank = {"missing": 2, "partial": 1, "implemented": 0}
    verify_rank = {"verified": 3, "upstream-captured": 2, "unverified": 1, "not-applicable": 0}
    for row in rows:
        if row.get("member") is not None or not row.get("export"):
            continue
        members = by_export.get(row["export"], [])
        if not members:
            continue
        if row["membership"] == "supported":
            statuses = [row["implementation"]] + [item["implementation"] for item in members]
            if all(status == "missing" for status in statuses):
                row["implementation"] = "missing"
            elif any(status != "implemented" for status in statuses):
                row["implementation"] = "partial"
            else:
                row["implementation"] = "implemented"
            if row["implementation"] != "implemented" and not row["implementation_ticket"]:
                row["implementation_ticket"] = "#1058-draft"
        best_evidence = max(members, key=lambda item: verify_rank.get(item["verification"], 0))
        if verify_rank.get(best_evidence["verification"], 0) > verify_rank.get(row["verification"], 0):
            row["verification"] = best_evidence["verification"]
            row["comparisonRoute"] = best_evidence["comparisonRoute"]
            if best_evidence.get("evidence"):
                row["scenario"] = best_evidence["scenario"]
                row["evidence"] = best_evidence["evidence"]
        if row["membership"] == "supported":
            generic_gaps = {
                "no v1.3.0 C++ comparison recorded",
                "member not individually re-audited against v1.3.0",
            }
            member_gaps = {gap for item in members for gap in item["gaps"]} - generic_gaps
            for gap in sorted(member_gaps):
                if gap not in row["gaps"]:
                    row["gaps"].append(gap)

    # Stable output order: export rows and their members in index order, then extra rows.
    ordered: list[dict] = []
    grouped: dict[str, list[dict]] = {}
    for row in rows:
        grouped.setdefault(row.get("export") or "", []).append(row)
    for export_name in export_order:
        ordered.extend(grouped.get(export_name, []))
    ordered.extend(grouped.get("", []))
    non_index_ids = [row["id"] for row in historical_non_index]
    ordered.extend(row for row in rows if row["id"] in non_index_ids)
    seen_ids: set[str] = set()
    historical_contract = {
        row["id"]: (row.get("cpp_contract") or "none") for row in historical["rows"]
    }
    for row in ordered:
        if row["id"] in seen_ids:
            raise SystemExit(f"duplicate ledger row id: {row['id']}")
        seen_ids.add(row["id"])
        historical_reference = row.pop("historicalEvidence", None)
        if historical_reference is not None:
            row["historicalEvidence"] = historical_reference
        interface = CPP_INTERFACE_OVERRIDE.get(row["id"])
        if interface is None:
            interface = historical_contract.get(row["id"])
        if not interface:
            interface = row["cpp"].get("interface") or "none"
        row["cpp"]["interface"] = interface
        if row["comparisonRoute"] == "not-applicable":
            row["comparisonCase"] = "not-applicable (approved exclusion)"
        elif row["comparisonRoute"] == "not-audited":
            row["comparisonCase"] = "none (not-audited; no discriminating v1.3.0 capture)"
        elif row.get("scenario"):
            row["comparisonCase"] = row["scenario"]
        else:
            row["comparisonCase"] = "none (no discriminating capture)"

    stats = {
        "rows": len(ordered),
        "indexExportRows": sum(
            1
            for row in ordered
            if row.get("member") is None and row.get("export") and row["kind"] in INDEX_EXPORT_KINDS
        ),
        "nonIndexCapabilityRows": sum(
            1
            for row in ordered
            if row.get("member") is None and not (row.get("export") and row["kind"] in INDEX_EXPORT_KINDS)
        ),
        "memberRows": sum(1 for row in ordered if row.get("member") is not None),
        "changeCapabilityRows": sum(1 for row in ordered if not row.get("export")),
        "linuxUsableRows": sum(1 for row in ordered if row["membership"] == "supported"),
        "gappedRows": sum(1 for row in ordered if row["membership"] == "supported" and row["gaps"]),
        "unimplementedRows": sum(
            1 for row in ordered if row["membership"] == "supported" and row["implementation"] in ("partial", "missing")
        ),
        "membership": dict(Counter(row["membership"] for row in ordered)),
        "implementation": dict(Counter(row["implementation"] for row in ordered)),
        "verification": dict(Counter(row["verification"] for row in ordered)),
        "sourceChangedSinceV1.0.4": sum(1 for row in ordered if row["sourceChangedSinceV1.0.4"]),
        "v130ObservationRows": sum(1 for row in ordered if row["verification"] == "upstream-captured"),
        "historicalRouteRows": sum(
            1
            for row in ordered
            if row.get("evidence") and row["evidence"].get("historicalBaseline") == "pi-v1.0.4"
        ),
    }
    ledger = {
        "schema": "pike/pi-v1.3.0-capability-ledger/1",
        "ticket": "1050",
        "baseline": BASELINE,
        "revision": REVISION,
        "platform": "linux-x86_64-glibc",
        "owner": "cch_tui",
        "generatedFrom": {
            "index": "packages/tui/src/index.ts",
            "package": "@earendil-works/pi-tui",
            "historicalLedger": HISTORICAL_LEDGER,
            "historicalRevision": HISTORICAL_REVISION,
            "changedSourceFiles": sorted(changed_files),
            "method": (
                "frozen index export enumeration + historical export/member identity carry + "
                "curated v1.3.0 change map + C++ source/test inventory"
            ),
            "command": (
                "python3 fixtures/pi-v1.3.0/audit-1050/build-ledger.py "
                "--pi-checkout /tmp/cch-1050-pi-frozen "
                "--output docs/research/pi-v1.3.0/1050-toolkit-capability-ledger.json"
            ),
        },
        "evidenceBundle": f"{AUDIT_BUNDLE}/manifest.json",
        "stats": stats,
        "rows": ordered,
    }
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(ledger, indent=2, ensure_ascii=False) + "\n")
    print(json.dumps(stats, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
