#!/usr/bin/env tsx
/**
 * Public-call scenarios for the `audit-1050` toolkit capability inventory (#1050).
 *
 * Every observation below comes from frozen `pi-v1.3.0` public toolkit calls: the package entry
 * (`index.ts`), the public component classes, `TuiMainScreen`/`TuiAltScreen`, `StdinBuffer`,
 * `detectCapabilities` and the `ProcessTerminal` host boundary. The scenarios target the toolkit
 * behavior that changed between `pi-v1.0.4` and `pi-v1.3.0` (fake cursor markers, dynamic
 * horizontal padding, selection reset, overlay cursor/wheel routing, shifted-symbol keys and
 * fragmented terminal-color replies) plus program-status formatting/detection/override and Herdr
 * capability detection. They are frozen observations, not a C++ capability claim.
 */

import path from "node:path";
import { pathToFileURL } from "node:url";

type SourceEndpointList = string[];

function moduleUrl(piCheckout: string, relative: string): string {
    return pathToFileURL(path.join(piCheckout, "packages/tui", relative)).href;
}

function delay(milliseconds: number): Promise<void> {
    return new Promise((resolve) => setTimeout(resolve, milliseconds));
}

function normalizeUndefined(value: unknown): unknown {
    if (Array.isArray(value)) return value.map(normalizeUndefined);
    if (value && typeof value === "object") {
        const result: Record<string, unknown> = {};
        for (const [key, entry] of Object.entries(value)) {
            result[key] = entry === undefined ? null : normalizeUndefined(entry);
        }
        return result;
    }
    return value;
}

const ENVIRONMENT_VARIABLES = [
    "TERM_PROGRAM",
    "TERM_PROGRAM_VERSION",
    "TERM",
    "COLORTERM",
    "KITTY_WINDOW_ID",
    "GHOSTTY_RESOURCES_DIR",
    "WEZTERM_PANE",
    "ITERM_SESSION_ID",
    "TMUX",
    "TERMUX_VERSION",
    "PI_HYPERLINKS",
    "PI_IMAGE_PROTOCOL",
    "PI_TRUE_COLOR",
    "PI_PROGRAM_STATUS",
    "PI_TUI_WRITE_LOG",
    "PI_HARDWARE_CURSOR",
];

/// Pin the capability inputs the toolkit reads from the host so the capture is reproducible.
export function pinScenarioEnvironment(): void {
    for (const name of ENVIRONMENT_VARIABLES) delete process.env[name];
    process.env.TERM = "xterm-256color";
}

/// Apply a controlled environment for one capability scenario, resetting the module cache.
async function withEnvironment<T>(
    piCheckout: string,
    values: Record<string, string | undefined>,
    body: () => Promise<T> | T,
): Promise<T> {
    const { resetCapabilitiesCache } = await import(moduleUrl(piCheckout, "src/terminal-image.ts"));
    const saved: Record<string, string | undefined> = {};
    for (const [name, value] of Object.entries(values)) {
        saved[name] = process.env[name];
        if (value === undefined) delete process.env[name];
        else process.env[name] = value;
    }
    resetCapabilitiesCache();
    try {
        return await body();
    } finally {
        for (const [name, value] of Object.entries(saved)) {
            if (value === undefined) delete process.env[name];
            else process.env[name] = value;
        }
        resetCapabilitiesCache();
    }
}

class RecordingTerminal {
    readonly writes: string[] = [];
    readonly cursorEvents: Array<"show" | "hide"> = [];
    private readonly inner: any;

    constructor(VirtualTerminal: any, columns: number, rows: number) {
        this.inner = new VirtualTerminal(columns, rows);
        const self = this;
        this.inner.write = (data: string) => {
            self.writes.push(data);
            VirtualTerminal.prototype.write.call(self.inner, data);
        };
        this.inner.showCursor = () => {
            self.cursorEvents.push("show");
            VirtualTerminal.prototype.showCursor.call(self.inner);
        };
        this.inner.hideCursor = () => {
            self.cursorEvents.push("hide");
            VirtualTerminal.prototype.hideCursor.call(self.inner);
        };
    }

    get terminal(): any {
        return this.inner;
    }

    take(): { writes: string[]; cursorEvents: Array<"show" | "hide"> } {
        const result = { writes: [...this.writes], cursorEvents: [...this.cursorEvents] };
        this.writes.length = 0;
        this.cursorEvents.length = 0;
        return result;
    }
}

async function newHost(piCheckout: string, columns: number, rows: number) {
    const { VirtualTerminal } = await import(moduleUrl(piCheckout, "test/virtual-terminal.ts"));
    return new RecordingTerminal(VirtualTerminal, columns, rows);
}

/// Cursor/fake-cursor composition, standalone component handling and overlay-adjacent cursor.
export async function captureCursorComposition(piCheckout: string) {
    const source = (relative: string) => moduleUrl(piCheckout, relative);
    const { Input } = await import(source("src/components/input.ts"));
    const { Editor } = await import(source("src/components/editor.ts"));
    const { TuiMainScreen } = await import(source("src/tui-main-screen.ts"));
    const { CURSOR_MARKER, renderFakeCursor } = await import(source("src/tui.ts"));
    const { stripTerminalSequences, sliceByColumn, visibleWidth } = await import(source("src/utils.ts"));
    const { defaultEditorTheme } = await import(source("test/test-themes.ts"));

    const sourceEndpoints: SourceEndpointList = [
        "packages/tui/src/components/input.ts:render",
        "packages/tui/src/components/editor.ts:render",
        "packages/tui/src/tui.ts:CURSOR_MARKER,renderFakeCursor,TuiBase.resolveFakeCursors",
        "packages/tui/src/tui-main-screen.ts:render",
        "packages/tui/src/utils.ts:stripTerminalSequences,sliceByColumn,visibleWidth",
        "packages/tui/src/terminal.ts:showCursor,hideCursor,moveBy",
        "packages/tui/test/tui-render.test.ts:TUI fake cursor",
    ];
    const scenarios: any[] = [];

    // Standalone Input: the v1.2.0 rendering contract wraps the drawn cursor in zero-width markers.
    {
        const input = new Input();
        input.setValue("abc");
        (input as any).focused = true;
        const rawLine = input.render(10)[0] ?? "";
        scenarios.push({
            name: "standalone-input-fake-cursor-is-strippable",
            dimensions: { columns: 10, rows: 1 },
            inputs: { component: "Input", value: "abc", focused: true, width: 10 },
            expected: {
                rawLine,
                strippedLine: stripTerminalSequences(rawLine),
                visibleWidth: visibleWidth(rawLine),
                fakeCursorMarkerCount: (rawLine.match(/\x1b_pi:fc\x07/g) ?? []).length,
            },
        });
    }

    // Standalone Editor: constructed with a TUI for its row budget, rendered outside a running TUI.
    {
        const probeHost = await newHost(piCheckout, 20, 5);
        const probeTui = new TuiMainScreen(probeHost.terminal);
        const editor = new Editor(probeTui, defaultEditorTheme);
        editor.setText("hi");
        const rawLines = editor.render(10);
        scenarios.push({
            name: "standalone-editor-fake-cursor-is-strippable",
            dimensions: { columns: 10, rows: 3 },
            inputs: { component: "Editor", text: "hi", width: 10, tuiStarted: false },
            expected: {
                rawLines,
                strippedLines: rawLines.map(stripTerminalSequences),
                visibleWidths: rawLines.map(visibleWidth),
                fakeCursorMarkerCount: rawLines.join("").match(/\x1b_pi:fc\x07/g)?.length ?? 0,
            },
        });
    }

    async function focusedInputRun(showHardwareCursor: boolean) {
        const host = await newHost(piCheckout, 20, 5);
        const tui = new TuiMainScreen(host.terminal, showHardwareCursor);
        const input = new Input();
        input.setValue("abc");
        tui.addChild(input);
        tui.setFocus(input);
        tui.start();
        await host.terminal.waitForRender();
        const observed = host.take();
        const screen = host.terminal.getViewport();
        tui.stop();
        return { observed, screen, showHardwareCursor };
    }

    // Focused Input with the hardware cursor disabled: the fake cursor is drawn in reverse video.
    {
        const { observed, screen } = await focusedInputRun(false);
        scenarios.push({
            name: "main-screen-focused-fake-cursor-drawn",
            dimensions: { columns: 20, rows: 5 },
            inputs: { root: "MainScreen", preference: "default-false", component: "Input", value: "abc" },
            expected: {
                writes: observed.writes,
                cursorEvents: observed.cursorEvents,
                screen,
                fakeCursorVisible: observed.writes.join("").includes("\x1b[7ma\x1b[27m"),
                hardwareCursorPositioned: observed.writes.join("").includes("\x1b[3G"),
            },
        });
    }

    // Focused Input with the hardware cursor enabled: only the terminal cursor is shown.
    {
        const { observed, screen } = await focusedInputRun(true);
        scenarios.push({
            name: "main-screen-focused-hardware-cursor-omits-fake-cursor",
            dimensions: { columns: 20, rows: 5 },
            inputs: { root: "MainScreen", preference: "enabled", component: "Input", value: "abc" },
            expected: {
                writes: observed.writes,
                cursorEvents: observed.cursorEvents,
                screen,
                fakeCursorVisible: observed.writes.join("").includes("\x1b[7ma\x1b[27m"),
                hardwareCursorPositioned: observed.writes.join("").includes("\x1b[3G"),
            },
        });
    }

    // An unfocused Input keeps its fake cursor even when the hardware cursor is enabled.
    {
        const host = await newHost(piCheckout, 20, 5);
        const tui = new TuiMainScreen(host.terminal, true);
        const input = new Input();
        input.setValue("abc");
        tui.addChild(input);
        tui.start();
        await host.terminal.waitForRender();
        const observed = host.take();
        const screen = host.terminal.getViewport();
        tui.stop();
        scenarios.push({
            name: "main-screen-unfocused-fake-cursor-stays-visible",
            dimensions: { columns: 20, rows: 5 },
            inputs: { root: "MainScreen", preference: "enabled", component: "Input", focused: false },
            expected: {
                writes: observed.writes,
                cursorEvents: observed.cursorEvents,
                screen,
                fakeCursorVisible: observed.writes.join("").includes("\x1b[7ma\x1b[27m"),
                hardwareCursorPositioned: observed.writes.join("").includes("\x1b[3G"),
            },
        });
    }

    // Truncating the line right after the cursor cell drops the closing marker.
    {
        const host = await newHost(piCheckout, 20, 5);
        const tui = new TuiMainScreen(host.terminal, true);
        const component = {
            render: (_width: number) => [sliceByColumn(`ab${CURSOR_MARKER}${renderFakeCursor("c")}def`, 0, 3, true)],
            invalidate: () => {},
        };
        tui.addChild(component);
        tui.start();
        await host.terminal.waitForRender();
        const observed = host.take();
        tui.stop();
        const writes = observed.writes.join("");
        scenarios.push({
            name: "truncated-line-drops-unterminated-fake-cursor",
            dimensions: { columns: 20, rows: 5 },
            inputs: { root: "MainScreen", preference: "enabled", line: "ab<CURSOR_MARKER><fake:c>def", slice: [0, 3] },
            expected: {
                writes: observed.writes,
                cursorEvents: observed.cursorEvents,
                fakeCursorVisible: writes.includes("\x1b[7m"),
                hardwareCursorPositioned: writes.endsWith("\x1b[3G"),
            },
        });
    }

    // An overlay ending directly left of the cursor must not drop the cursor.
    for (const showHardwareCursor of [false, true]) {
        const host = await newHost(piCheckout, 20, 5);
        const tui = new TuiMainScreen(host.terminal, showHardwareCursor);
        tui.addChild({
            render: (_width: number) => [`abc${CURSOR_MARKER}${renderFakeCursor("d")}ef`],
            invalidate: () => {},
        });
        tui.showOverlay(
            { render: (_width: number) => ["XXX"], invalidate: () => {} },
            { row: 0, col: 0, width: 3 },
        );
        tui.start();
        await host.terminal.waitForRender();
        const observed = host.take();
        tui.stop();
        const writes = observed.writes.join("");
        scenarios.push({
            name: `overlay-left-of-cursor-keeps-cursor-${showHardwareCursor ? "hardware" : "fake"}`,
            dimensions: { columns: 20, rows: 5 },
            inputs: {
                root: "MainScreen",
                showHardwareCursor,
                base: "abc<CURSOR_MARKER><fake:d>ef",
                overlay: "XXX",
                overlayBounds: { row: 0, col: 0, width: 3 },
            },
            expected: {
                writes: observed.writes,
                cursorEvents: observed.cursorEvents,
                overlayText: writes.includes("XXX"),
                fakeCursorVisible: writes.includes("\x1b[7md"),
                hardwareCursorPositioned: writes.endsWith("\x1b[4G"),
            },
        });
    }

    return { sourceEndpoints, scenarios };
}

/// Dynamic horizontal padding on `Box`/`Text` and `TuiAltScreen.resetTextSelection()`.
export async function capturePaddingAndSelectionReset(piCheckout: string) {
    const source = (relative: string) => moduleUrl(piCheckout, relative);
    const { Box } = await import(source("src/components/box.ts"));
    const { Text } = await import(source("src/components/text.ts"));
    const { TuiAltScreen } = await import(source("src/tui-alt-screen.ts"));

    const sourceEndpoints: SourceEndpointList = [
        "packages/tui/src/components/box.ts:Box.setPaddingX,render",
        "packages/tui/src/components/text.ts:Text.setPaddingX,render",
        "packages/tui/src/tui-alt-screen.ts:TuiAltScreen.resetTextSelection,clearTextSelection",
        "packages/tui/test/tui-alt-screen.test.ts:resetTextSelection",
    ];
    const scenarios: any[] = [];

    {
        const box = new Box(2, 0);
        box.addChild(new Text("hey", 0, 0));
        const atTwo = box.render(10);
        box.setPaddingX(3);
        const atThree = box.render(10);
        box.setPaddingX(0);
        const atZero = box.render(10);
        scenarios.push({
            name: "box-set-padding-x-rerenders-dynamic-width",
            dimensions: { columns: 10, rows: 1 },
            inputs: { component: "Box", child: "Text(hey)", transitions: [2, 3, 0], width: 10 },
            expected: { atTwo, atThree, atZero },
        });
    }

    {
        const text = new Text("hello", 1, 0);
        const atOne = text.render(10);
        text.setPaddingX(3);
        const atThree = text.render(10);
        text.setPaddingX(0);
        const atZero = text.render(10);
        scenarios.push({
            name: "text-set-padding-x-rerenders-dynamic-width",
            dimensions: { columns: 10, rows: 2 },
            inputs: { component: "Text", text: "hello", transitions: [1, 3, 0], width: 10 },
            expected: { atOne, atThree, atZero },
        });
    }

    const click = async (terminal: any, x: number, y: number) => {
        terminal.sendInput(`\x1b[<0;${x};${y}M`);
        terminal.sendInput(`\x1b[<0;${x};${y}m`);
        await terminal.waitForRender();
    };
    const drag = async (terminal: any, x: number, y: number, toX: number) => {
        terminal.sendInput(`\x1b[<0;${x};${y}M`);
        terminal.sendInput(`\x1b[<32;${toX};${y}M`);
        terminal.sendInput(`\x1b[<3;${toX};${y}m`);
        await terminal.waitForRender();
    };

    // A double click selects a word. resetTextSelection() must drop both the selection and the
    // multi-click history: the next press is a character click again, not a line click.
    {
        const { VirtualTerminal } = await import(moduleUrl(piCheckout, "test/virtual-terminal.ts"));
        const terminal = new VirtualTerminal(20, 4);
        const tui = new TuiAltScreen(terminal, false, undefined, { copyOnSelect: false });
        tui.addChild(new Text("alpha beta gamma", 0, 0));
        tui.start();
        await terminal.waitForRender();
        await click(terminal, 7, 1);
        await click(terminal, 7, 1);
        const afterDoubleClick = tui.getScreenLines();
        tui.resetTextSelection();
        tui.requestRender();
        await terminal.waitForRender();
        const afterReset = tui.getScreenLines();
        await drag(terminal, 7, 1, 9);
        const dragAfterReset = tui.getScreenLines();
        tui.stop();
        scenarios.push({
            name: "selection-reset-drops-selection-and-multiclick-history",
            dimensions: { columns: 20, rows: 4 },
            inputs: {
                root: "AltScreen",
                copyOnSelect: false,
                text: "alpha beta gamma",
                steps: ["click(7,1)", "click(7,1)", "resetTextSelection", "drag(7,1 -> 9,1)"],
            },
            expected: { afterDoubleClick, afterReset, dragAfterReset },
        });
    }

    // Control: without the reset, the third consecutive press selects the whole line.
    {
        const { VirtualTerminal } = await import(moduleUrl(piCheckout, "test/virtual-terminal.ts"));
        const terminal = new VirtualTerminal(20, 4);
        const tui = new TuiAltScreen(terminal, false, undefined, { copyOnSelect: false });
        tui.addChild(new Text("alpha beta gamma", 0, 0));
        tui.start();
        await terminal.waitForRender();
        await click(terminal, 7, 1);
        await click(terminal, 7, 1);
        terminal.sendInput("\x1b[<0;7;1M");
        terminal.sendInput("\x1b[<32;9;1M");
        await terminal.waitForRender();
        const thirdClickDrag = tui.getScreenLines();
        tui.stop();
        scenarios.push({
            name: "third-consecutive-click-selects-line-control",
            dimensions: { columns: 20, rows: 4 },
            inputs: {
                root: "AltScreen",
                copyOnSelect: false,
                text: "alpha beta gamma",
                steps: ["click(7,1)", "click(7,1)", "press(7,1)", "drag(9,1)"],
            },
            expected: { thirdClickDrag },
        });
    }

    return { sourceEndpoints, scenarios };
}

/// Overlay cursor/wheel routing in the alternate screen.
export async function captureOverlayRouting(piCheckout: string) {
    const source = (relative: string) => moduleUrl(piCheckout, relative);
    const { TuiAltScreen } = await import(source("src/tui-alt-screen.ts"));
    const { Text } = await import(source("src/components/text.ts"));
    const { VirtualTerminal } = await import(source("test/virtual-terminal.ts"));

    const sourceEndpoints: SourceEndpointList = [
        "packages/tui/src/tui-alt-screen.ts:TuiAltScreen.handleWheelReport,routeWheel,shouldDeferViewportInputToOverlay,handleViewportInput",
        "packages/tui/test/tui-alt-screen.test.ts:scrolls the transcript on wheel input a focused overlay does not handle",
        "packages/tui/test/tui-alt-screen.test.ts:gives viewport keys to a focused overlay",
    ];
    const scenarios: any[] = [];

    class InputOverlay {
        focused = false;
        inputs: string[] = [];
        handleInput(data: string): void {
            this.inputs.push(data);
        }
        render(): string[] {
            return ["overlay"];
        }
        invalidate(): void {}
    }

    {
        const terminal = new VirtualTerminal(20, 6);
        const tui = new TuiAltScreen(terminal);
        tui.addChild(new Text(Array.from({ length: 12 }, (_, index) => `line ${index + 1}`).join("\n"), 0, 0));
        const overlay = new InputOverlay();
        tui.start();
        await terminal.waitForRender();
        const topBefore = tui.viewportTop;
        tui.showOverlay(overlay, { anchor: "top-left", width: 10 });
        await terminal.waitForRender();
        const topAfterOverlay = tui.viewportTop;
        terminal.sendInput("\x1b[<64;3;1M");
        await terminal.waitForRender();
        const topAfterWheel = tui.viewportTop;
        const screenAfterWheel = tui.getScreenLines();
        terminal.sendInput("\x1b[6~");
        await terminal.waitForRender();
        const topAfterPageDown = tui.viewportTop;
        const overlayInputs = [...overlay.inputs];
        tui.stop();
        scenarios.push({
            name: "overlay-unhandled-wheel-scrolls-transcript",
            dimensions: { columns: 20, rows: 6 },
            inputs: {
                root: "AltScreen",
                contentRows: 12,
                overlay: "focused InputOverlay",
                events: ["showOverlay(top-left,10)", "wheel-up(3,1)", "pageDown"],
            },
            expected: {
                overlayFocused: overlay.focused,
                viewportTop: { before: topBefore, afterOverlay: topAfterOverlay, afterWheel: topAfterWheel, afterPageDown: topAfterPageDown },
                overlayInputs,
                screenAfterWheel,
            },
        });
    }

    return { sourceEndpoints, scenarios };
}

/// Shifted-symbol keys and fragmented terminal reply buffering.
export async function captureKeysAndTerminalReplies(piCheckout: string) {
    const source = (relative: string) => moduleUrl(piCheckout, relative);
    const { matchesKey, parseKey, setKittyProtocolActive } = await import(source("src/keys.ts"));
    const { StdinBuffer } = await import(source("src/stdin-buffer.ts"));
    const { TuiMainScreen } = await import(source("src/tui-main-screen.ts"));

    const sourceEndpoints: SourceEndpointList = [
        "packages/tui/src/keys.ts:parseKey,matchesKey,parseKeyId,matchesKittySequence,matchesPrintableModifyOtherKeys",
        "packages/tui/src/stdin-buffer.ts:StdinBuffer.process,flush,extractCompleteSequences",
        "packages/tui/src/tui.ts:TUI.queryTerminalColors",
        "packages/tui/test/keys.test.ts:shifted symbols",
        "packages/tui/test/stdin-buffer.test.ts:Terminal Reply Strings",
        "packages/tui/test/terminal-colors.test.ts:delayed Ghostty color fragments",
    ];
    const scenarios: any[] = [];

    {
        const sequences = {
            kitty_shift_plus: "\x1b[61:43;2u",
            kitty_ctrl_shift_plus: "\x1b[61:43;6u",
            modify_other_keys_ctrl_plus: "\x1b[27;6;43~",
            legacy_ctrl_slash: "\x1b[27;5;47~",
        };
        setKittyProtocolActive(true);
        const observations: Record<string, unknown> = {};
        for (const [name, sequence] of Object.entries(sequences)) {
            observations[name] = {
                parseKey: parseKey(sequence),
                matchesPlus: matchesKey(sequence, "+"),
                matchesShiftEquals: matchesKey(sequence, "shift+="),
                matchesCtrlPlus: matchesKey(sequence, "ctrl++"),
                matchesCtrlSlash: matchesKey(sequence, "ctrl+/"),
            };
        }
        const plainPlus = parseKey("+");
        const plainPlusMatches = matchesKey("+", "+");
        setKittyProtocolActive(false);
        scenarios.push({
            name: "shifted-symbol-keys-match-produced-character",
            dimensions: { columns: 80, rows: 24 },
            inputs: { sequences, plainPlus: "+" },
            expected: { observations, plainPlus, plainPlusMatches },
        });
    }

    {
        const buffer = new StdinBuffer({ timeout: 20, escapeTimeout: 5 });
        const sequences: string[] = [];
        buffer.on("data", (sequence: string) => sequences.push(sequence));
        buffer.process("\x1b]4;1;rgb:eaea/6c");
        await delay(80);
        const afterHeaderTimeout = [...sequences];
        const bufferedAfterHeaderTimeout = buffer.getBuffer();
        buffer.process("6c/6d6d\x07");
        await delay(20);
        const afterTail = [...sequences];
        buffer.process("\x07");
        await delay(20);
        const afterRealCtrlG = [...sequences];
        buffer.destroy();
        scenarios.push({
            name: "fragmented-color-reply-never-leaks-or-opens-editor",
            dimensions: { columns: 80, rows: 24 },
            inputs: {
                reply: "\x1b]4;1;rgb:eaea/6c6c/6d6d\x07",
                split: [15, 22],
                timeoutMs: 20,
                tailWaitMs: 80,
            },
            expected: {
                afterHeaderTimeout,
                bufferedAfterHeaderTimeout,
                afterTail,
                afterTailCtlGMatch: afterTail.some((sequence) => matchesKey(sequence, "ctrl+g")),
                afterRealCtrlG,
                realCtrlGDelivered: matchesKey(afterRealCtrlG.at(-1) ?? "", "ctrl+g"),
            },
        });
    }

    {
        const buffer = new StdinBuffer({ timeout: 20, escapeTimeout: 5 });
        const sequences: string[] = [];
        buffer.on("data", (sequence: string) => sequences.push(sequence));
        buffer.process("\x1b]a");
        await delay(20);
        buffer.process("x\r");
        await delay(40);
        const recovered = [...sequences];
        buffer.destroy();
        scenarios.push({
            name: "ambiguous-alt-bracket-input-recovers-within-sequence-timeout",
            dimensions: { columns: 80, rows: 24 },
            inputs: { prefix: "\x1b]", batchedTail: "x\r", timeoutMs: 20 },
            expected: { recovered },
        });
    }

    // The higher-level query: a delayed Ghostty color fragment must not reach the prompt.
    {
        class TestTerminal {
            private inputHandler?: (data: string) => void;
            private resizeHandler?: () => void;
            readonly writes: string[] = [];
            constructor(private readonly columnCount = 80, private readonly rowCount = 24) {}
            start(onInput: (data: string) => void, onResize: () => void): void {
                this.inputHandler = onInput;
                this.resizeHandler = onResize;
            }
            stop(): void {
                this.inputHandler = undefined;
                this.resizeHandler = undefined;
            }
            async drainInput(): Promise<void> {}
            write(data: string): void {
                this.writes.push(data);
            }
            get columns(): number {
                return this.columnCount;
            }
            get rows(): number {
                return this.rowCount;
            }
            get kittyProtocolActive(): boolean {
                return false;
            }
            moveBy(): void {}
            hideCursor(): void {}
            showCursor(): void {}
            clearLine(): void {}
            clearFromCursor(): void {}
            clearScreen(): void {}
            setTitle(): void {}
            setProgress(): void {}
            setProgramStatus(): void {}
            sendInput(data: string): void {
                this.inputHandler?.(data);
            }
        }
        class InputRecorder {
            readonly inputs: string[] = [];
            render(): string[] {
                return [];
            }
            handleInput(data: string): void {
                this.inputs.push(data);
            }
            invalidate(): void {}
        }
        const terminal = new TestTerminal();
        const tui = new TuiMainScreen(terminal as any);
        const recorder = new InputRecorder();
        tui.addChild(recorder as any);
        tui.setFocus(recorder as any);
        tui.start();
        const buffer = new StdinBuffer();
        buffer.on("data", (data: string) => terminal.sendInput(data));
        const query = tui.queryTerminalColors({ timeoutMs: 1000 });
        buffer.process("\x1b]4;1;rgb:eaea/6c");
        await delay(100);
        buffer.process("6c/6d6d\x07\x1b[?62;4;52c");
        const colors = normalizeUndefined(await query);
        const inputsAfterQuery = [...recorder.inputs];
        buffer.process("\x07");
        const inputsAfterRealCtrlG = [...recorder.inputs];
        buffer.destroy();
        tui.stop();
        scenarios.push({
            name: "fragmented-color-query-keeps-prompt-clean",
            dimensions: { columns: 80, rows: 24 },
            inputs: {
                query: "queryTerminalColors({timeoutMs:1000})",
                fragments: ["\x1b]4;1;rgb:eaea/6c", "6c/6d6d\x07", "\x1b[?62;4;52c"],
                fragmentDelayMs: 100,
            },
            expected: { colors, inputsAfterQuery, inputsAfterRealCtrlG },
        });
    }

    return { sourceEndpoints, scenarios };
}

/// Program-status formatting and reply recognition.
export async function captureProgramStatusFormatting(piCheckout: string) {
    const { formatProgramStatus, isProgramStatusReply } = await import(moduleUrl(piCheckout, "src/program-status.ts"));

    const sourceEndpoints: SourceEndpointList = [
        "packages/tui/src/program-status.ts:formatProgramStatus,isProgramStatusReply,PROGRAM_STATUS_QUERY",
        "packages/tui/test/program-status.test.ts:formatProgramStatus,isProgramStatusReply",
    ];
    const scenarios: any[] = [];

    {
        const blocked = formatProgramStatus({
            state: "blocked",
            app: "pi",
            kind: "permission",
            message: "Allow bash?",
        });
        const clear = formatProgramStatus({ state: "clear" });
        const omitted = formatProgramStatus({
            state: "working",
            app: "my app",
            kind: "auth",
            message: " \n ",
        });
        const controlCharacters = formatProgramStatus({
            state: "error",
            message: "first\nsecond\x1b[31m\u009bthird\t",
        });
        const longMessage = formatProgramStatus({ state: "working", app: "pi", message: "é".repeat(2000) });
        const decodeMessage = (sequence: string): string | null => {
            const match = /:msg=([A-Za-z0-9+/=]*)/.exec(sequence);
            return match?.[1] === undefined ? null : Buffer.from(match[1], "base64").toString("utf8");
        };
        scenarios.push({
            name: "format-program-status-encodes-fields-and-limits",
            dimensions: { columns: 80, rows: 24 },
            inputs: { states: ["blocked/permission", "clear", "working/invalid-app/empty-message", "error/control-characters", "working/2000-é"] },
            expected: {
                blocked,
                clear,
                omitted,
                controlCharacters,
                controlCharactersMessage: decodeMessage(controlCharacters),
                longMessageBytes: Buffer.byteLength(longMessage),
                longMessageDecodedLength: decodeMessage(longMessage)?.length ?? null,
                invalidAppRowOmitted: !omitted.includes("app="),
                kindOmittedOutsideBlocked: !omitted.includes("kind="),
            },
        });
    }

    {
        const replies = {
            query_echo_st: "\x1b]7501;?\x1b\\",
            query_echo_bel: "\x1b]7501;?\x07",
            query_echo_future_pairs: "\x1b]7501;?version=2\x1b\\",
            status_report: "\x1b]7501;state=idle\x1b\\",
            color_report: "\x1b]11;rgb:0000/0000/0000\x07",
        };
        const recognized: Record<string, boolean> = {};
        for (const [name, reply] of Object.entries(replies)) recognized[name] = isProgramStatusReply(reply);
        scenarios.push({
            name: "program-status-reply-recognition",
            dimensions: { columns: 80, rows: 24 },
            inputs: { replies },
            expected: { recognized },
        });
    }

    return { sourceEndpoints, scenarios };
}

/// Herdr's hyperlink and Kitty-image capability detection.
export async function captureTerminalCapabilities(piCheckout: string) {
    const { detectCapabilities } = await import(moduleUrl(piCheckout, "src/terminal-image.ts"));

    const sourceEndpoints: SourceEndpointList = [
        "packages/tui/src/terminal-image.ts:detectCapabilities,detectCapabilitiesFromEnvironment",
        "packages/tui/test/terminal-image.test.ts:enables hyperlinks and Kitty images for Herdr",
    ];
    const scenarios: any[] = [];

    const cases: Array<{ name: string; env: Record<string, string | undefined> }> = [
        {
            name: "herdr-detected-before-outer-terminal",
            env: { TERM_PROGRAM: "herdr", TERM: "xterm-256color", COLORTERM: "truecolor" },
        },
        {
            name: "herdr-with-leaked-kitty-variable",
            env: { TERM_PROGRAM: "herdr", TERM: "xterm-256color", COLORTERM: "truecolor", KITTY_WINDOW_ID: "1" },
        },
        {
            name: "herdr-hyperlink-override-disabled",
            env: { TERM_PROGRAM: "herdr", TERM: "xterm-256color", PI_HYPERLINKS: "0" },
        },
        {
            name: "kitty-control",
            env: { TERM_PROGRAM: "kitty", KITTY_WINDOW_ID: "1", TERM: "xterm-256color" },
        },
        {
            name: "unknown-terminal-control",
            env: { TERM: "xterm-256color", COLORTERM: "truecolor" },
        },
    ];
    for (const testCase of cases) {
        const capabilities = await withEnvironment(piCheckout, testCase.env, () => detectCapabilities());
        scenarios.push({
            name: testCase.name,
            dimensions: { columns: 80, rows: 24 },
            inputs: { env: testCase.env },
            expected: { capabilities },
        });
    }

    return { sourceEndpoints, scenarios };
}
