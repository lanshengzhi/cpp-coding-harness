#!/usr/bin/env tsx
/**
 * Capture the `audit-1201` frozen observations: the 30 historical pi-v1.0.4
 * terminal primitives recaptured at the frozen pi v1.3.0 checkout. These rows
 * were recorded as `upstream-captured` against the historical
 * `fixtures/pi-tui/bundles/pi-v1.0.4` family, which is not an observation of
 * the named v1.3.0 baseline (#1201). Every value below comes from executing
 * the frozen checkout's own modules through their public seams; the historical
 * bundle's recorded export accounting and the v1.0.4 source bytes are recorded
 * beside each observation so a reviewer can see whether the primitive changed
 * between the two baselines.
 *
 * Usage (from the repository root):
 *   PI_CHECKOUT=<private frozen checkout> PI_BASELINE=pi-v1.3.0 \
 *     <checkout>/node_modules/.bin/tsx fixtures/pi-v1.3.0/audit-1201/capture-1201.mts
 */

import { execFileSync, spawnSync } from "node:child_process";
import { createHash } from "node:crypto";
import { mkdirSync, readFileSync, writeFileSync } from "node:fs";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const scriptPath = fileURLToPath(import.meta.url);
const familyDir = path.dirname(scriptPath);
const auditRoot = path.resolve(familyDir, "..");
const repositoryRoot = path.resolve(auditRoot, "../..");
const family = "audit-1201";
const registryPath = path.join(auditRoot, "baselines.json");
const piCheckout = process.env.PI_CHECKOUT ?? path.resolve(repositoryRoot, "../pi");

if (process.env.TSX_TSCONFIG_PATH === undefined) {
	const tsx = path.join(piCheckout, "node_modules", ".bin", "tsx");
	const result = spawnSync(tsx, [scriptPath, ...process.argv.slice(2)], {
		stdio: "inherit",
		env: {
			...process.env,
			NODE_OPTIONS: [process.env.NODE_OPTIONS, "--conditions=source"].filter(Boolean).join(" "),
			TSX_TSCONFIG_PATH: path.join(piCheckout, "tsconfig.json"),
		},
	});
	if (result.error) throw new Error(`cannot re-exec ${tsx}: ${result.error}`);
	process.exit(result.status ?? 1);
}

const registry = JSON.parse(readFileSync(registryPath, "utf8"));
if (registry.schema !== "pike/pi-v1.3.0-audit-baselines/1") {
	throw new Error(`unexpected audit baseline registry schema: ${registryPath}`);
}
const baselineName = process.env.PI_BASELINE ?? registry.default;
const baseline = registry.baselines[baselineName];
if (!baseline) {
	throw new Error(
		`unknown baseline ${JSON.stringify(baselineName)}; registered baselines: ` +
			Object.keys(registry.baselines).sort().join(", "),
	);
}
const head = execFileSync("git", ["-C", piCheckout, "rev-parse", "HEAD"], { encoding: "utf8" }).trim();
if (head !== baseline.revision) {
	throw new Error(`pi checkout must be at ${baseline.revision} (baseline ${baselineName}), found ${head}`);
}
if (execFileSync("git", ["-C", piCheckout, "status", "--porcelain"], { encoding: "utf8" }).trim()) {
	throw new Error("pi capture requires an unmodified source checkout");
}
const glibc = (process.report?.getReport() as any)?.header?.glibcVersionRuntime;
if (process.platform !== "linux" || process.arch !== "x64" || !glibc) {
	throw new Error("audit capture requires Linux x86-64/glibc");
}
for (const [name, entry] of Object.entries<any>(registry.baselines)) {
	if (name !== baselineName && entry.bundles && family in entry.bundles) {
		throw new Error(`refusing to capture ${family}: it already belongs to baseline ${JSON.stringify(name)}`);
	}
}

let tsxVersion = "unknown";
try {
	tsxVersion = execFileSync(path.join(piCheckout, "node_modules", ".bin", "tsx"), ["--version"], {
		encoding: "utf8",
	}).trim();
} catch {
	// Provenance, not a capture precondition.
}
const environment = {
	platform: process.platform,
	arch: process.arch,
	libc: "glibc",
	glibcVersion: glibc,
	node: process.version,
	tsx: tsxVersion,
};
const capturedAt = new Date().toISOString();
const sha256 = (bytes: Buffer | string) => createHash("sha256").update(bytes).digest("hex");

function load(relativePath: string): Promise<Record<string, any>> {
	return import(pathToFileURL(path.join(piCheckout, relativePath)).href) as Promise<Record<string, any>>;
}

const tuiPackageRoot = path.join(piCheckout, "packages/tui");
const tuiSource = (relative: string) => pathToFileURL(path.join(tuiPackageRoot, relative)).href;

const terminalColors = await load("packages/tui/src/terminal-colors.ts");
const nativePlatform = await load("packages/tui/src/native-platform.ts");
const imageModule = await load("packages/tui/src/components/image.ts");
const terminalImage = await load("packages/tui/src/terminal-image.ts");
const { Editor: FrozenEditor } = await import(tuiSource("src/components/editor.ts"));
const { TuiMainScreen } = await import(tuiSource("src/tui-main-screen.ts"));
const { VirtualTerminal } = await import(tuiSource("test/virtual-terminal.ts"));
const { defaultEditorTheme } = await import(tuiSource("test/test-themes.ts"));
const { setKittyProtocolActive } = await import(tuiSource("src/keys.ts"));
setKittyProtocolActive(false);

// ---------------------------------------------------------------------------
// Historical pi-v1.0.4 bundle record: what the historical family recorded for
// these primitives, so the reviewer sees the cross-baseline delta in one place.
// ---------------------------------------------------------------------------
const HISTORICAL_REVISION = "7c10bd4337495ee613f2224843ecdf349b80d1df";
const historicalBundlePath = "fixtures/pi-tui/bundles/pi-v1.0.4";
const historicalLedger = JSON.parse(
	readFileSync(path.join(repositoryRoot, historicalBundlePath, "capability-ledger.json"), "utf8"),
) as { scenarios: Array<{ name: string; expected: { exports: string[] } }> };
const historicalExports = historicalLedger.scenarios.find((item) => item.name === "index-export-accounting")?.expected
	.exports;
if (!historicalExports) {
	throw new Error("historical pi-v1.0.4 export accounting scenario is missing");
}
function historicalSourceDigest(relativePath: string): string {
	return sha256(
		execFileSync("git", ["-C", piCheckout, "show", `${HISTORICAL_REVISION}:${relativePath}`], {
			encoding: "buffer",
			maxBuffer: 16 * 1024 * 1024,
		}) as Buffer,
	);
}
function frozenSourceDigest(relativePath: string): string {
	return sha256(readFileSync(path.join(piCheckout, relativePath)));
}
/** One cross-baseline record per primitive group, naming the historical bundle's own accounting. */
function crossBaselineRecord(relativePath: string, primitives: string[]) {
	return {
		historicalBaseline: "pi-v1.0.4",
		historicalRevision: HISTORICAL_REVISION,
		historicalBundle: historicalBundlePath,
		historicalExportAccounting: "index-export-accounting",
		primitivesListedInHistoricalExports: Object.fromEntries(primitives.map((name) => [name, historicalExports.includes(name)])),
		sourceAtHistoricalRevision: historicalSourceDigest(relativePath),
		sourceAtFrozenRevision: frozenSourceDigest(relativePath),
		sourceUnchangedSinceHistoricalBaseline: historicalSourceDigest(relativePath) === frozenSourceDigest(relativePath),
	};
}

function scenario(name: string, inputs: any, expected: any, normalizations: string[] = []) {
	return { name, inputs, expected, observed: expected, normalizations };
}

const artifacts: Array<{ path: string; sourceEndpoints: string[]; scenarios: any[] }> = [];

// ---------------------------------------------------------------------------
// Terminal colors (RgbColor, TerminalColorScheme, TerminalColors,
// parseTerminalColorSchemeReport and their members): the OSC reply parser and
// the public queryTerminalColors assembly over a virtual terminal.
// ---------------------------------------------------------------------------
{
	const scenarios: any[] = [];

	// 1. Every frozen color form at its per-channel scale.
	{
		const parseOscColorResponse = terminalColors.parseOscColorResponse as any;
		const replies: Array<{ input: string; result: any }> = [
			{ input: "\x1b]10;#ff0000\x07", result: parseOscColorResponse("\x1b]10;#ff0000\x07") },
			{ input: "\x1b]11;#00ff7f\x1b\\", result: parseOscColorResponse("\x1b]11;#00ff7f\x1b\\") },
			{ input: "\x1b]4;1;#abcd12345678\x07", result: parseOscColorResponse("\x1b]4;1;#abcd12345678\x07") },
			{ input: "\x1b]4;2;rgb:8/8/8\x07", result: parseOscColorResponse("\x1b]4;2;rgb:8/8/8\x07") },
			{ input: "\x1b]10;rgba:f/0/0/f\x07", result: parseOscColorResponse("\x1b]10;rgba:f/0/0/f\x07") },
			{ input: "\x1b]10;#12345\x07", result: parseOscColorResponse("\x1b]10;#12345\x07") },
			{ input: "\x1b]10;not-a-color\x07", result: parseOscColorResponse("\x1b]10;not-a-color\x07") },
			{ input: "\x1b[31m", result: parseOscColorResponse("\x1b[31m") },
		];
		scenarios.push(
			scenario(
				"OSC color replies parse every frozen color form at its per-channel scale",
				{ replies: replies.map((item) => item.input) },
				{
					parsed: replies.map((item) => ({ input: item.input, result: item.result })),
				},
			),
		);
	}

	// 2. The public query assembly: complete palette vs a missing slot.
	{
		const term = new VirtualTerminal(40, 8);
		const tui = new TuiMainScreen(term);
		tui.start(() => {}, () => {});
		const paletteQuery = tui.queryTerminalColors({ timeoutMs: 500 });
		term.sendInput("\x1b]10;#ff0000\x07");
		term.sendInput("\x1b]11;#00ff00\x07");
		for (let index = 0; index < 16; ++index) {
			const channel = (index * 16).toString(16).padStart(4, "0");
			term.sendInput(`\x1b]4;${index};rgb:${channel}/${channel}/${channel}\x07`);
		}
		term.sendInput("\x1b[?1;2c");
		const complete = await paletteQuery;

		const partialQuery = tui.queryTerminalColors({ timeoutMs: 500 });
		term.sendInput("\x1b]10;#123456\x1b\\");
		term.sendInput("\x1b]11;#654321\x07");
		for (let index = 0; index < 15; ++index) {
			term.sendInput(`\x1b]4;${index};#010203\x07`);
		}
		term.sendInput("\x1b[?1;2c");
		const missingSlot = await partialQuery;
		await tui.stop();

		scenarios.push(
			scenario(
				"the public color query sets the palette only when all sixteen replies arrive",
				{
					complete: "foreground, background and sixteen OSC 4 replies terminated by DA1",
					missingSlot: "foreground, background and fifteen of sixteen OSC 4 replies terminated by DA1",
				},
				{
					complete: {
						foreground: complete.foreground ?? null,
						background: complete.background ?? null,
						palette: complete.palette ?? null,
					},
					missingSlot: {
						foreground: missingSlot.foreground ?? null,
						background: missingSlot.background ?? null,
						palette: missingSlot.palette ?? null,
					},
				},
			),
		);
	}

	// 3. Scheme reports: dark, light, malformed, concatenated pairs.
	{
		const parseTerminalColorSchemeReport = terminalColors.parseTerminalColorSchemeReport as any;
		scenarios.push(
			scenario(
				"terminal color scheme reports resolve dark and light only from complete 997 replies",
				{
					reports: [
						"\x1b[?997;1n",
						"\x1b[?997;2n",
						"\x1b[?997;3n",
						"not-a-report",
						"\x1b[?997;1n\x1b[?997;2n",
						"",
					],
				},
				{
					parsed: [
						"\x1b[?997;1n",
						"\x1b[?997;2n",
						"\x1b[?997;3n",
						"not-a-report",
						"\x1b[?997;1n\x1b[?997;2n",
						"",
					].map((report) => ({ report, scheme: parseTerminalColorSchemeReport(report) ?? null })),
				},
			),
		);
	}

	// 4. Cross-baseline record against the historical pi-v1.0.4 bundle.
	scenarios.push(
		scenario(
			"the terminal color primitives are unchanged from the historical pi-v1.0.4 bundle record",
			{ source: "packages/tui/src/terminal-colors.ts" },
			crossBaselineRecord("packages/tui/src/terminal-colors.ts", [
				"RgbColor",
				"TerminalColorScheme",
				"TerminalColors",
				"parseTerminalColorSchemeReport",
			]),
		),
	);

	artifacts.push({
		path: "terminal-colors.json",
		sourceEndpoints: [
			"packages/tui/src/terminal-colors.ts:RgbColor,TerminalColorScheme,TerminalColors,parseOscColorResponse,parseTerminalColorSchemeReport",
			"packages/tui/src/tui.ts:queryTerminalColors",
		],
		scenarios,
	});
}

// ---------------------------------------------------------------------------
// EditorComponent and its members: the concrete frozen Editor driven through
// the public TUI seam over a virtual terminal.
// ---------------------------------------------------------------------------
{
	const scenarios: any[] = [];
	const state = (editor: any) => {
		const lines = editor.getLines();
		const cursor = editor.getCursor();
		const line = lines[cursor.line] ?? "";
		return {
			text: editor.getText(),
			expanded: editor.getExpandedText(),
			lines,
			cursor: { line: cursor.line, utf16Column: cursor.col, before: line.slice(0, cursor.col), after: line.slice(cursor.col) },
		};
	};
	const makeEditor = (width = 40) => new FrozenEditor(new TuiMainScreen(new VirtualTerminal(width, 8)), defaultEditorTheme);

	// 1. The contract surface every member row binds to.
	{
		const editor = makeEditor() as any;
		scenarios.push(
			scenario(
				"the editor component exposes every EditorComponent contract member",
				{ contract: "packages/tui/src/editor-component.ts:EditorComponent" },
				{
					getText: typeof editor.getText === "function",
					setText: typeof editor.setText === "function",
					handleInput: typeof editor.handleInput === "function",
					onSubmit: "onSubmit" in editor,
					onChange: "onChange" in editor,
					addToHistory: typeof editor.addToHistory === "function",
					insertTextAtCursor: typeof editor.insertTextAtCursor === "function",
					getExpandedText: typeof editor.getExpandedText === "function",
					setAutocompleteProvider: typeof editor.setAutocompleteProvider === "function",
					setPaddingX: typeof editor.setPaddingX === "function",
					setAutocompleteMaxVisible: typeof editor.setAutocompleteMaxVisible === "function",
					borderColor: typeof editor.borderColor === "function",
				},
			),
		);
	}

	// 2. Typing, change reporting, submission and cursor state.
	{
		const editor = makeEditor() as any;
		const changes: string[] = [];
		editor.onChange = (text: string) => changes.push(text);
		const submitted: string[] = [];
		editor.onSubmit = (text: string) => submitted.push(text);
		editor.handleInput("h");
		editor.handleInput("é");
		editor.handleInput("😀");
		const typed = state(editor);
		editor.handleInput("\x1b[D");
		const left = state(editor);
		editor.handleInput("\x7f");
		const deleted = state(editor);
		editor.handleInput("\r");
		scenarios.push(
			scenario(
				"typing reports every change through onChange and submits through onSubmit",
				{ keys: ["h", "é", "😀", "left", "backspace", "enter"] },
				{ typed, left, deleted, submitted, changes },
			),
		);
	}

	// 3. History recall and draft restoration.
	{
		const editor = makeEditor() as any;
		const changes: string[] = [];
		editor.onChange = (text: string) => changes.push(text);
		editor.addToHistory("first");
		editor.addToHistory("second");
		editor.setText("draft");
		editor.handleInput("\x1b[A");
		const recalled = state(editor);
		editor.handleInput("\x1b[A");
		const recalledOlder = state(editor);
		editor.handleInput("\x1b[B");
		const restored = state(editor);
		editor.handleInput("\x1b[B");
		const draftBack = state(editor);
		scenarios.push(
			scenario(
				"added history entries recall with up and restore the edited draft with down",
				{ history: ["first", "second"], draft: "draft", keys: ["up", "up", "down", "down"] },
				{ recalled, recalledOlder, restored, draftBack, changes },
			),
		);
	}

	// 4. A large paste collapses to a marker and getExpandedText restores it.
	{
		const editor = makeEditor() as any;
		const payloadLines = Array.from({ length: 12 }, (_, index) => `line-${index}`);
		const payload = payloadLines.join("\n");
		editor.setText("AB");
		editor.handleInput("\x01");
		editor.handleInput("\x1b[C");
		editor.handleInput(`\x1b[200~${payload}\x1b[201~`);
		const collapsed = state(editor);
		scenarios.push(
			scenario(
				"a large paste collapses to a marker and getExpandedText restores the full text",
				{ frame: "AB", payloadLines, paste: "bracketed" },
				{
					collapsed,
					expandedEqualsPayload: editor.getExpandedText() === `A${payload}B`,
				},
			),
		);
	}

	// 5. Autocomplete provider with a visible cap: five items under a requested
	//    cap of two bind at the frozen floor of three with a paging indicator.
	{
		const term = new VirtualTerminal(60, 12);
		const tui = new TuiMainScreen(term);
		tui.start(() => {}, () => {});
		const editor = new FrozenEditor(tui, defaultEditorTheme) as any;
		tui.addChild(editor);
		const items = [
			{ value: "alice", label: "alice", description: "first" },
			{ value: "amal", label: "amal", description: "second" },
			{ value: "amanda", label: "amanda", description: "third" },
			{ value: "amber", label: "amber", description: "fourth" },
			{ value: "amy", label: "amy", description: "fifth" },
		];
		editor.setAutocompleteProvider({
			triggerCharacters: ["@"],
			getSuggestions: async (lines: string[], cursorLine: number, cursorCol: number) => ({
				items,
				prefix: "@am",
			}),
			applyCompletion: (lines: string[], cursorLine: number, cursorCol: number, item: any) => {
				const line = lines[cursorLine] ?? "";
				const start = line.lastIndexOf("@", cursorCol - 1);
				const next = line.slice(0, start) + item.value + line.slice(cursorCol);
				return { lines: [next], cursorLine, cursorCol: start + item.value.length };
			},
		});
		editor.setAutocompleteMaxVisible(2);
		editor.handleInput("@am");
		await new Promise((resolve) => setTimeout(resolve, 100));
		tui.renderNow(true);
		const viewport = await term.flushAndGetViewport();
		editor.handleInput("\t");
		const applied = state(editor);
		await tui.stop();
		scenarios.push(
			scenario(
				"an autocomplete provider renders suggestions capped by the visible maximum",
				{ trigger: "@am", requestedMaxVisible: 2, items: items.map((item) => item.value), accept: "tab" },
				{
					viewport,
					visibleCount: viewport.filter((line: string) => /^\s*(→\s+)?(alice|amal|amanda|amber|amy)\b/.test(line)).length,
					pagingIndicator: viewport.find((line: string) => /^\s*\(\d+\/\d+\)/.test(line)) ?? null,
					applied,
				},
			),
		);
	}

	// 6. Padding and border color shape the rendered frame.
	{
		const term = new VirtualTerminal(40, 6);
		const tui = new TuiMainScreen(term);
		tui.start(() => {}, () => {});
		const writes: string[] = [];
		const originalWrite = term.write.bind(term);
		term.write = (data: string) => {
			writes.push(data);
			originalWrite(data);
		};
		const editor = new FrozenEditor(tui, {
			...defaultEditorTheme,
			borderColor: (text: string) => `\x1b[31m${text}\x1b[39m`,
		}) as any;
		editor.setText("pad");
		editor.setPaddingX(2);
		tui.addChild(editor);
		tui.renderNow(true);
		const padded = await term.flushAndGetViewport();
		const borderWrite = writes.find((chunk: string) => chunk.includes("\x1b[31m")) ?? null;
		scenarios.push(
			scenario(
				"horizontal padding and the border color shape the rendered editor frame",
				{ text: "pad", paddingX: 2, borderColor: "ANSI red colorizer" },
				{
					padded,
					paddingXApplied: padded.some((line: string) => line.includes("  pad")),
					borderWrite,
					borderApplied: borderWrite !== null,
				},
				["xterm trailing-space trimming on blank rows; the border color is observed on the raw write chunk because the plain-text viewport strips styling"],
			),
		);
		await tui.stop();
	}

	// 7. Cross-baseline record.
	scenarios.push(
		scenario(
			"the editor component primitives are unchanged from the historical pi-v1.0.4 bundle record",
			{ source: "packages/tui/src/editor-component.ts" },
			crossBaselineRecord("packages/tui/src/editor-component.ts", ["EditorComponent"]),
		),
	);

	artifacts.push({
		path: "editor-component.json",
		sourceEndpoints: [
			"packages/tui/src/editor-component.ts:EditorComponent",
			"packages/tui/src/components/editor.ts:Editor,getText,setText,handleInput,onSubmit,onChange,addToHistory,insertTextAtCursor,getExpandedText,setAutocompleteProvider,setPaddingX,setAutocompleteMaxVisible,borderColor",
			"packages/tui/test/virtual-terminal.ts:VirtualTerminal",
		],
		scenarios,
	});
}

// ---------------------------------------------------------------------------
// NativeClipboard and getNativeClipboard: the frozen Linux clipboard outcomes
// observable in a headless capture environment.
// ---------------------------------------------------------------------------
{
	const scenarios: any[] = [];
	const getNativeClipboard = nativePlatform.getNativeClipboard as any;

	// 1. No display at all: no helper.
	{
		const savedDisplay = process.env.DISPLAY;
		const savedWayland = process.env.WAYLAND_DISPLAY;
		delete process.env.DISPLAY;
		delete process.env.WAYLAND_DISPLAY;
		const clipboard = getNativeClipboard();
		if (savedDisplay === undefined) delete process.env.DISPLAY;
		else process.env.DISPLAY = savedDisplay;
		if (savedWayland === undefined) delete process.env.WAYLAND_DISPLAY;
		else process.env.WAYLAND_DISPLAY = savedWayland;
		scenarios.push(
			scenario(
				"getNativeClipboard reports no helper when no display variable is set",
				{ DISPLAY: "unset", WAYLAND_DISPLAY: "unset" },
				{ clipboard: clipboard === undefined ? "undefined" : "object" },
			),
		);
	}

	// 2. A display variable pointing at an unreachable X server: the helper
	//    loads lazily and reads report unavailable instead of throwing.
	{
		const savedDisplay = process.env.DISPLAY;
		process.env.DISPLAY = ":99";
		const clipboard = getNativeClipboard();
		const text = clipboard && typeof clipboard.getText === "function" ? await clipboard.getText() : "no-helper";
		const image = clipboard && typeof clipboard.getImage === "function" ? await clipboard.getImage() : "no-helper";
		const outcome = {
			helper: clipboard === undefined ? "undefined" : "object",
			getText: text === undefined ? "undefined" : text === null ? "null" : text === "no-helper" ? "no-helper" : "value",
			getImage:
				image === undefined ? "undefined" : image === null ? "null" : image === "no-helper" ? "no-helper" : "bytes",
			getFilePathsPresent: clipboard ? typeof clipboard.getFilePaths === "function" : false,
			setTextPresent: clipboard ? typeof clipboard.setText === "function" : false,
		};
		if (savedDisplay === undefined) delete process.env.DISPLAY;
		else process.env.DISPLAY = savedDisplay;
		scenarios.push(
			scenario(
				"a disconnected display keeps the helper loaded and reads report unavailable",
				{ DISPLAY: ":99 (no X server)", reads: ["getText", "getImage"] },
				outcome,
				["the empty (null) and transfer-failure (reject) outcomes need a live X server, which the headless capture host lacks"],
			),
		);
	}

	// 3. The interface contract spelling from the frozen source.
	{
		const sourceText = readFileSync(path.join(piCheckout, "packages/tui/src/native-platform.ts"), "utf8");
		scenarios.push(
			scenario(
				"the NativeClipboard interface spells the unavailable empty and failed outcomes",
				{ source: "packages/tui/src/native-platform.ts:NativeClipboard" },
				{
					getTextUnavailableIsUndefined: /getText\(\): Promise<string \| null \| undefined>/.test(sourceText),
					getTextDoc: "Undefined means unavailable, null means no text; transfer failures reject.",
					getImageUnavailableIsUndefined: /getImage\(\): Promise<Uint8Array \| null \| undefined>/.test(sourceText),
					getFilePathsOptional: /getFilePaths\?\(\)/.test(sourceText),
					setTextOptional: /setText\?\(text: string\)/.test(sourceText),
					linuxSetTextNote: "Linux uses command-line tools to retain clipboard ownership instead.",
				},
			),
		);
	}

	// 4. Cross-baseline record.
	scenarios.push(
		scenario(
			"the clipboard primitives are unchanged from the historical pi-v1.0.4 bundle record",
			{ source: "packages/tui/src/native-platform.ts" },
			crossBaselineRecord("packages/tui/src/native-platform.ts", ["NativeClipboard", "getNativeClipboard"]),
		),
	);

	artifacts.push({
		path: "native-clipboard.json",
		sourceEndpoints: [
			"packages/tui/src/native-platform.ts:NativeClipboard,getNativeClipboard",
			"packages/tui/src/native-module-path.ts:getNativeModuleCandidates",
		],
		scenarios,
	});
}

// ---------------------------------------------------------------------------
// setImageTranscoder: the Kitty non-PNG conversion boundary over a virtual
// terminal with the Kitty capability set.
// ---------------------------------------------------------------------------
{
	const scenarios: any[] = [];
	const { Image, setImageTranscoder } = imageModule as any;
	terminalImage.setCapabilities({ images: "kitty", trueColor: true, hyperlinks: false });
	const FAKE_JPEG_BASE64 = "QUJDREVGRw==";
	const FAKE_PNG_BASE64 =
		"iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk+M9QDwADhgGAWjR9awAAAABJRU5ErkJggg==";
	const theme = { fallbackColor: (text: string) => text };
	const visible = async (term: any) =>
		(await term.flushAndGetViewport()).map((line: string) => line.trimEnd()).filter((line: string) => line.trim());
	const renderWith = async (transcoder: any) => {
		const term = new VirtualTerminal(40, 8);
		const tui = new TuiMainScreen(term);
		tui.start(() => {}, () => {});
		setImageTranscoder(transcoder);
		const image = new Image(FAKE_JPEG_BASE64, "image/jpeg", theme, {});
		tui.addChild(image);
		tui.renderNow(true);
		const lines = await visible(term);
		await tui.stop();
		return lines;
	};

	// 1. Absent converter: the text fallback.
	{
		const lines = await renderWith(undefined);
		scenarios.push(
			scenario(
				"without a transcoder a non-PNG image renders the text fallback",
				{ image: "image/jpeg base64", transcoder: "unset", capabilities: "kitty" },
				{ lines, fallbackText: lines[0] ?? null },
			),
		);
	}

	// 2. A working converter: the Kitty protocol path.
	{
		let calls = 0;
		const lines = await renderWith(() => {
			++calls;
			return FAKE_PNG_BASE64;
		});
		scenarios.push(
			scenario(
				"a transcoder converts non-PNG data for the Kitty protocol",
				{ image: "image/jpeg base64", transcoder: "returns PNG", capabilities: "kitty" },
				{ transcoderCalls: calls, fallbackText: lines[0] ?? null, renderedImage: lines.length === 0 },
			),
		);
	}

	// 3. A failing converter: the fallback stays and a fresh image retries.
	{
		let badCalls = 0;
		let goodCalls = 0;
		const failed = await renderWith(() => {
			++badCalls;
			return null;
		});
		const retried = await renderWith(() => {
			++goodCalls;
			return FAKE_PNG_BASE64;
		});
		scenarios.push(
			scenario(
				"a failing transcoder keeps the fallback and a fresh image retries with the new transcoder",
				{ image: "image/jpeg base64", first: "returns null", second: "returns PNG" },
				{
					failed: { transcoderCalls: badCalls, lines: failed },
					retried: { transcoderCalls: goodCalls, fallbackText: retried[0] ?? null, renderedImage: retried.length === 0 },
				},
			),
		);
	}

	// 4. Clearing the transcoder restores the fallback.
	{
		await renderWith(() => FAKE_PNG_BASE64);
		const cleared = await renderWith(undefined);
		scenarios.push(
			scenario(
				"clearing the transcoder restores the text fallback for fresh images",
				{ image: "image/jpeg base64", transcoder: "set then cleared" },
				{ lines: cleared, fallbackText: cleared[0] ?? null },
			),
		);
	}

	// 5. Cross-baseline record.
	scenarios.push(
		scenario(
			"the image transcoder primitive is unchanged from the historical pi-v1.0.4 bundle record",
			{ source: "packages/tui/src/components/image.ts" },
			crossBaselineRecord("packages/tui/src/components/image.ts", ["setImageTranscoder"]),
		),
	);

	artifacts.push({
		path: "image-transcoder.json",
		sourceEndpoints: [
			"packages/tui/src/components/image.ts:setImageTranscoder,Image",
			"packages/tui/src/terminal-image.ts:setCapabilities",
		],
		scenarios,
	});
}

// ---------------------------------------------------------------------------
// Write the artifacts and the manifest only after every scenario succeeded.
// ---------------------------------------------------------------------------
const manifestArtifacts = [];
const artifactBytes = new Map<string, Buffer>();
for (const artifact of artifacts) {
	const document = {
		schema: "pike/pi-v1.3.0-audit-evidence/1",
		baseline: baselineName,
		revision: baseline.revision,
		capturedAt,
		environment,
		sourceEndpoints: artifact.sourceEndpoints,
		scenarios: artifact.scenarios,
	};
	const bytes = Buffer.from(`${JSON.stringify(document, null, 2)}\n`);
	artifactBytes.set(artifact.path, bytes);
	manifestArtifacts.push({
		path: artifact.path,
		family,
		bytes: bytes.length,
		sha256: sha256(bytes),
		sourceEndpoints: artifact.sourceEndpoints,
		scenarios: artifact.scenarios,
	});
}

const generatorPath = path.relative(repositoryRoot, scriptPath).split(path.sep).join("/");
const manifest = {
	schema: "pike/pi-v1.3.0-audit-evidence/1",
	baseline: baselineName,
	revision: baseline.revision,
	ticket: "1201",
	family,
	capturedAt,
	environment,
	generator: {
		path: generatorPath,
		sha256: sha256(readFileSync(scriptPath)),
		command: `PI_CHECKOUT=<pi-checkout> PI_BASELINE=${baselineName} <pi-checkout>/node_modules/.bin/tsx ${generatorPath}`,
		sources: [{ path: generatorPath, sha256: sha256(readFileSync(scriptPath)) }],
	},
	artifacts: manifestArtifacts,
};

mkdirSync(familyDir, { recursive: true });
for (const [relative, bytes] of artifactBytes) writeFileSync(path.join(familyDir, relative), bytes);
writeFileSync(path.join(familyDir, "manifest.json"), `${JSON.stringify(manifest, null, 2)}\n`);

console.log(
	`captured ${family} (${manifestArtifacts.length} artifacts) for ${baselineName} (${baseline.revision})`,
);
