#!/usr/bin/env tsx
/** Independent named TUI capture (#947). Never imports Pike or historical fixtures.
 * PI_CHECKOUT must be a clean checkout at the registry's full revision.
 * Later tickets extend the cases below and add artifacts with the same envelope.
 */
import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import { mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { createRequire } from "node:module";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const scriptPath = fileURLToPath(import.meta.url);
const root = path.resolve(path.dirname(scriptPath), "..");
const registryPath = path.join(root, "baselines.json");
const registry = JSON.parse(readFileSync(registryPath, "utf8"));
if (registry.schema !== "pike/tui-baselines/1") throw new Error("invalid TUI baseline registry");
const baseline = process.env.PI_BASELINE ?? registry.default;
const selected = registry.baselines[baseline];
if (!selected) throw new Error(`unknown TUI baseline: ${baseline}`);
if (!selected.bundle.startsWith("bundles/") || selected.bundle.split("/").includes("..")) {
	throw new Error("capture requires a distinct named bundle");
}
const pi = process.env.PI_CHECKOUT ?? path.resolve(root, "../../../pi");
const revision = execFileSync("git", ["-C", pi, "rev-parse", "HEAD"], { encoding: "utf8" }).trim();
if (revision !== selected.revision) throw new Error(`pi checkout must be at ${selected.revision}, found ${revision}`);
if (execFileSync("git", ["-C", pi, "status", "--porcelain"], { encoding: "utf8" }).trim()) {
	throw new Error("pi capture requires an unmodified source checkout");
}
const glibc = (process.report.getReport() as any).header.glibcVersionRuntime;
if (process.platform !== "linux" || process.arch !== "x64" || !glibc) {
	throw new Error("TUI capture requires Linux x86-64/glibc");
}
// Pin capability inputs before importing pi; HOME is unused by these cases.
const disabled = ["TERM_PROGRAM", "TERM_PROGRAM_VERSION", "KITTY_WINDOW_ID", "WEZTERM_PANE", "TMUX", "TERMUX_VERSION", "PI_TUI_WRITE_LOG", "PI_HARDWARE_CURSOR", "COLORTERM"];
for (const variable of disabled) delete process.env[variable];
process.env.TERM = "xterm-256color";
const environment = {
	platform: process.platform, arch: process.arch, libc: "glibc", glibcVersion: glibc,
	node: process.version, TERM: process.env.TERM, disabledVariables: disabled,
};
const requirePi = createRequire(path.join(pi, "packages/tui/package.json"));
const xtermVersion = requirePi("@xterm/headless/package.json").version;
const packageMetadata = JSON.parse(readFileSync(path.join(pi, "packages/tui/package.json"), "utf8"));
if (xtermVersion !== packageMetadata.devDependencies["@xterm/headless"]) {
	throw new Error("installed xterm version differs from frozen pi package declaration");
}
Object.assign(environment, { dependencies: { "@xterm/headless": xtermVersion, tsx: requirePi("tsx/package.json").version } });
const capturedAt = new Date().toISOString();
const sha256 = (bytes: Buffer) => createHash("sha256").update(bytes).digest("hex");
const src = (relative: string) => pathToFileURL(path.join(pi, "packages/tui", relative)).href;
const { parseKey, isKeyRepeat, isKeyRelease, setKittyProtocolActive } = await import(src("src/keys.ts"));
const { visibleWidth, wrapTextWithAnsi, sliceByColumn, truncateToWidth, getOsc8LinkAtColumn } =
	await import(src("src/utils.ts"));
const { Input } = await import(src("src/components/input.ts"));
const { Text } = await import(src("src/components/text.ts"));
const { TuiMainScreen } = await import(src("src/tui-main-screen.ts"));
const { CURSOR_MARKER } = await import(src("src/tui.ts"));
const { VirtualTerminal } = await import(src("test/virtual-terminal.ts"));
const { Terminal: XtermTerminal } = requirePi("@xterm/headless");

function envelope(sourceEndpoints: string[], scenarios: unknown[]) {
	return { baseline, revision, capturedAt, environment, sourceEndpoints, scenarios };
}

// Input parsing and actual Input editing/submission are independent observations.
setKittyProtocolActive(false);
const sequences = ["\x1b[D", "\x1b[97;5:2u", "\x1b[97;5:3u"];
const edits = ["a", "b", "\x1b[D", "c", "\r"];
const input = new Input();
const submitted: string[] = [];
input.onSubmit = (value: string) => submitted.push(value);
const values = edits.map((data) => { input.handleInput(data); return input.getValue(); });
const inputArtifact = envelope([
	"packages/tui/src/keys.ts:parseKey,isKeyRepeat,isKeyRelease",
	"packages/tui/src/components/input.ts:handleInput,getValue,onSubmit",
], [{
	name: "keys-and-input-editing", dimensions: { columns: 16, rows: 6 },
	inputs: { kittyActive: false, sequences, edits },
	expected: {
		keys: sequences.map((data) => ({
			id: parseKey(data) ?? null,
			eventType: isKeyRelease(data) ? "release" : isKeyRepeat(data) ? "repeat" : "press",
		})), values, submitted,
	},
}]);

// Include child mutation so a render label or one cached frame cannot stand in for output.
const textInputs = { text: "\x1b[31mhello\x1b[0m world", changedText: "\x1b[32mnext\x1b[0m text", paddingX: 1, paddingY: 0 };
const text = new Text(textInputs.text, textInputs.paddingX, textInputs.paddingY);
const first = [...text.render(12)];
text.setText(textInputs.changedText);
const componentArtifact = envelope(["packages/tui/src/components/text.ts:render,setText"], [{
	name: "styled-text-mutation", dimensions: { columns: 12, rows: 6 },
	inputs: textInputs, expected: { first, changed: [...text.render(12)] },
}]);

// Use the frozen VirtualTerminal as the renderer adapter. Feed its exact output to a
// second public xterm instance to inspect full cell attributes without private pi hooks.
class CaptureTerminal extends VirtualTerminal {
	readonly emulator: any;
	cursorVisible = true;
	constructor(columns: number, rows: number) {
		super(columns, rows);
		this.emulator = new XtermTerminal({ cols: columns, rows, allowProposedApi: true });
	}
	override write(data: string) {
		if ([...data.matchAll(/\x1b\]8;[^;]*;([^\x07\x1b]*)/g)].some((match) => match[1])) throw new Error("OSC 8 cell capture needs link-aware evidence before adding linked scenarios");
		super.write(data); this.emulator.write(data);
	}
	override hideCursor() { super.hideCursor(); this.cursorVisible = false; }
	override showCursor() { super.showCursor(); this.cursorVisible = true; }
	async snapshot() {
		await this.flush();
		await new Promise<void>((resolve) => this.emulator.write("", resolve));
		const buffer = this.emulator.buffer.active;
		const screen = this.getViewport();
		const observedScreen = Array.from({ length: this.rows }, (_, row) => buffer.getLine(buffer.viewportY + row).translateToString(true));
		const observedCursor = this.getCursorPosition();
		if (JSON.stringify(screen) !== JSON.stringify(observedScreen) || observedCursor.x !== buffer.cursorX || observedCursor.y !== buffer.cursorY) {
			throw new Error("cell observer diverged from frozen VirtualTerminal; extend the public terminal adapter before recording this scenario");
		}
		const cells = Array.from({ length: this.rows }, (_, row) => Array.from({ length: this.columns }, (_, col) => {
			const cell = buffer.getLine(buffer.viewportY + row).getCell(col);
			// Palette indices map to canonical SGR color values; default colors remain empty.
			const color = (foreground: boolean) => {
				const mode = foreground ? cell.isFgPalette() : cell.isBgPalette();
				const rgb = foreground ? cell.isFgRGB() : cell.isBgRGB();
				if (rgb) {
					const rgbValue = foreground ? cell.getFgColor() : cell.getBgColor();
					return `${foreground ? 38 : 48};2;${rgbValue >> 16};${(rgbValue >> 8) & 255};${rgbValue & 255}`;
				}
				if (!mode) return "";
				const value = foreground ? cell.getFgColor() : cell.getBgColor();
				return value < 8 ? String((foreground ? 30 : 40) + value) : `${foreground ? 38 : 48};5;${value}`;
			};
			if (cell.isOverline()) throw new Error("overline cells need explicit C++ representation evidence before capture");
			return {
				grapheme: cell.getChars(), continuation: cell.getWidth() === 0,
				style: {
					bold: !!cell.isBold(), dim: !!cell.isDim(), italic: !!cell.isItalic(),
					underline: !!cell.isUnderline(), blink: !!cell.isBlink(), inverse: !!cell.isInverse(),
					hidden: !!cell.isInvisible(), strikethrough: !!cell.isStrikethrough(),
					fg_color: color(true), bg_color: color(false), hyperlink: "", hyperlink_params: "",
				},
			};
		}));
		return { screen, cells, cursor: { column: buffer.cursorX, row: buffer.cursorY }, cursorVisible: this.cursorVisible, scrollback: this.getScrollBuffer().slice(0, buffer.baseY) };
	}
}
const screenInputs = {
	frames: [["\x1b[1;31mred\x1b[0m", "tail"], ["\x1b[4;32mgo\x1b[0m", "tail"]],
	cursor: { column: 2, row: 1 }, showHardwareCursor: false,
};
const terminal = new CaptureTerminal(16, 6);
const renderer = new TuiMainScreen(terminal, screenInputs.showHardwareCursor);
let lines = screenInputs.frames[0];
const child = {
	focused: false,
	render: () => lines.map((line, row) => row === screenInputs.cursor.row
		? line.slice(0, screenInputs.cursor.column) + CURSOR_MARKER + line.slice(screenInputs.cursor.column)
		: line),
	invalidate: () => {},
};
renderer.addChild(child);
renderer.start();
renderer.setFocus(child);
renderer.renderNow();
const initial = await terminal.snapshot();
lines = screenInputs.frames[1];
renderer.renderNow();
const changed = await terminal.snapshot();
renderer.stop();
terminal.emulator.dispose();
const screenArtifact = envelope([
	"packages/tui/src/tui-main-screen.ts:TuiMainScreen.renderNow",
	"packages/tui/src/tui.ts:setFocus,CURSOR_MARKER",
	"packages/tui/test/virtual-terminal.ts:VirtualTerminal",
	"packages/tui/package.json:@xterm/headless",
], [{ name: "styled-frame-and-focused-cursor", dimensions: { columns: 16, rows: 6 }, inputs: screenInputs, expected: { initial, changed } }]);


// Capability accounting observation: freeze the public index export set and ledger classifications.
// The committed docs/research ledger is hashed as the C++/Pike accounting artifact; export names are
// observed directly from the frozen index so a stale/partial ledger cannot pass silently.
const indexSource = readFileSync(path.join(pi, "packages/tui/src/index.ts"), "utf8");
const indexExports = [...indexSource.matchAll(/export\s+(?:type\s+)?\{([^}]+)\}\s+from\s+"([^"]+)"/gs)].flatMap((match) =>
	match[1].split(",").map((raw) => {
		let name = raw.trim().replace(/^type\s+/, "");
		if (!name) return "";
		if (name.includes(" as ")) name = name.split(" as ").at(-1)?.trim() ?? "";
		return name;
	}).filter(Boolean),
);
const ledgerPath = path.resolve(root, "../../docs/research/tui-v1.0.4-capability-ledger.json");
const ledger = JSON.parse(readFileSync(ledgerPath, "utf8"));
if (ledger.baseline !== baseline || ledger.revision !== revision) {
	throw new Error("capability ledger revision/baseline mismatch");
}
const exportRows = ledger.rows.filter((row: any) => row.member == null && !["product-entry", "diagnostic-capability", "internal-helper"].includes(row.kind));
const ledgerNames = exportRows.map((row: any) => row.export).sort();
const observedNames = [...indexExports].sort();
if (JSON.stringify(ledgerNames) !== JSON.stringify(observedNames)) {
	throw new Error("capability ledger export set diverges from frozen index");
}
if (exportRows.some((row: any) => row.classification === "included" && /Deferred/.test(JSON.stringify(row)))) {
	throw new Error("included capability rows must not be Deferred");
}
const capabilityArtifact = envelope([
	"packages/tui/src/index.ts:export-surface",
], [{
	name: "index-export-accounting",
	dimensions: { columns: 1, rows: 1 },
	inputs: {
		indexSha256: sha256(Buffer.from(indexSource)),
		exportCount: observedNames.length,
		ledgerPath: "docs/research/tui-v1.0.4-capability-ledger.json",
		ledgerSha256: sha256(readFileSync(ledgerPath)),
	},
	expected: {
		exports: observedNames,
		classifications: Object.fromEntries(exportRows.map((row: any) => [row.export, row.classification])),
		crossReferences: ["831", "811", "809", "749", "830"],
	},
}]);

// Public visible-width and dependent Text/Input placement (#956).
const narrowText = new Text("काक्ष", 0, 0);
const narrowInput = new Input();
narrowInput.setValue("काक्ष");
const utilsWidthArtifact = envelope([
	"packages/tui/src/utils.ts:visibleWidth",
	"packages/tui/src/components/text.ts:render",
	"packages/tui/src/components/input.ts:render,setValue",
], [{
	name: "visible-width-and-narrow-placement",
	dimensions: { columns: 6, rows: 4 },
	inputs: {
		visibleWidth: [
			{ name: "ka", input: "का", output: visibleWidth("का") },
			{ name: "ksha", input: "क्ष", output: visibleWidth("क्ष") },
			{ name: "ka-newline-ksha", input: "का\nक्ष", output: visibleWidth("का\nक्ष") },
			{ name: "hello-newline-world", input: "hello\nworld", output: visibleWidth("hello\nworld") },
			{
				name: "ansi-newline-ansi",
				input: "\x1b[31mred\x1b[0m\n\x1b[32mtail\x1b[0m",
				output: visibleWidth("\x1b[31mred\x1b[0m\n\x1b[32mtail\x1b[0m"),
			},
			{ name: "emoji-combining-flag", input: "a😀b", output: visibleWidth("a😀b") },
		],
		narrowText: { text: "काक्ष", width: 2 },
		narrowInput: { value: "काक्ष", width: 6 },
	},
	expected: {
		visibleWidth: [
			{ name: "ka", input: "का", output: visibleWidth("का") },
			{ name: "ksha", input: "क्ष", output: visibleWidth("क्ष") },
			{ name: "ka-newline-ksha", input: "का\nक्ष", output: visibleWidth("का\nक्ष") },
			{ name: "hello-newline-world", input: "hello\nworld", output: visibleWidth("hello\nworld") },
			{
				name: "ansi-newline-ansi",
				input: "\x1b[31mred\x1b[0m\n\x1b[32mtail\x1b[0m",
				output: visibleWidth("\x1b[31mred\x1b[0m\n\x1b[32mtail\x1b[0m"),
			},
			{ name: "emoji-combining-flag", input: "a😀b", output: visibleWidth("a😀b") },
		],
		narrowText: { width2: [...narrowText.render(2)], width4: [...narrowText.render(4)] },
		narrowInput: { width6: [...narrowInput.render(6)] },
	},
}]);

// Public ANSI/OSC 8 boundary order for wrap, slice, truncate and link lookup (#957).
const belLink = (url: string) => `\x1b]8;;${url}\x07`;
const stLink = (url: string) => `\x1b]8;;${url}\x1b\\`;
const wrapCases = [
	{ name: "staged-control-after-content", input: "中文\x1b[31mABCDEFGHIJ", width: 4 },
	{ name: "staged-underline-after-content", input: "中文\x1b[4mABCDEFGH", width: 4 },
	{ name: "staged-hyperlink-after-content", input: `中文${belLink("u")}ABCDEFGH`, width: 4 },
	{ name: "st-terminated-link-across-break", input: `${stLink("u")}hello world${stLink("")}`, width: 7 },
	{ name: "link-cjk-styled-span", input: `ab ${belLink("u")}\x1b[31m中文\x1b[0m${belLink("")} cd`, width: 4 },
	{ name: "control-before-logical-newline", input: "abc\x1b[31m\ndef", width: 20 },
];
const truncateCases = [
	{ name: "fits-open-underline-pads-inside", input: "\x1b[4mabc", width: 8, ellipsis: "", pad: true },
	{ name: "pending-control-styles-dropped-text", input: "\x1b[4ma\x1b[31mbcdef", width: 4, ellipsis: "...", pad: false },
	{ name: "pending-link-styles-dropped-text", input: `\x1b[4ma${belLink("u")}bcdef`, width: 4, ellipsis: "...", pad: false },
	{ name: "st-terminated-link-closes-before-reset", input: `${stLink("u")}abcdefgh`, width: 4, ellipsis: "...", pad: false },
	{ name: "ellipsis-does-not-fit", input: "abcdef", width: 2, ellipsis: "中", pad: false },
	{ name: "ellipsis-clipped-to-fit", input: "abcdef", width: 2, ellipsis: "abc", pad: false },
	{ name: "ellipsis-clipped-away", input: "abcdef", width: 1, ellipsis: "中中", pad: false },
];
const sliceCases = [
	{ name: "slice-start-code-order", input: "\x1b[32mfoo\x1b[39m bar", start: 3, length: 4, strict: true },
	{ name: "slice-inside-open-link", input: `${belLink("u")}abcdefgh${belLink("")}`, start: 2, length: 3, strict: false },
	{ name: "slice-spanning-cjk-link", input: `${belLink("u")}a你bcd${belLink("")}`, start: 1, length: 3, strict: false },
];
const linkColumns = [
	{ name: "cjk-link-first-cell", input: `a ${belLink("https://x")}你b${belLink("")} c`, column: 2 },
	{ name: "cjk-link-second-cell", input: `a ${belLink("https://x")}你b${belLink("")} c`, column: 3 },
	{ name: "styled-link-cell", input: `\x1b[31m${belLink("https://x")}ab${belLink("")}\x1b[0m`, column: 0 },
	{ name: "cell-after-link-close", input: `${belLink("https://x")}ab${belLink("")}`, column: 3 },
];
const utilsAnsiArtifact = envelope([
	"packages/tui/src/utils.ts:wrapTextWithAnsi",
	"packages/tui/src/utils.ts:sliceByColumn",
	"packages/tui/src/utils.ts:truncateToWidth",
	"packages/tui/src/utils.ts:getOsc8LinkAtColumn",
], [{
	name: "ansi-boundary-order-and-hyperlinks",
	dimensions: { columns: 20, rows: 4 },
	inputs: { wrap: wrapCases, truncate: truncateCases, slice: sliceCases, linkColumns },
	expected: {
		wrap: wrapCases.map((entry) => ({ ...entry, output: wrapTextWithAnsi(entry.input, entry.width) })),
		truncate: truncateCases.map((entry) => ({
			...entry, output: truncateToWidth(entry.input, entry.width, entry.ellipsis, entry.pad),
		})),
		slice: sliceCases.map((entry) => ({
			...entry, output: sliceByColumn(entry.input, entry.start, entry.length, entry.strict),
		})),
		linkColumns: linkColumns.map((entry) => ({
			...entry, link: getOsc8LinkAtColumn(entry.input, entry.column) ?? null,
		})),
	},
}]);

// No output is touched before source guards and all observations complete.
const bundle = path.join(root, selected.bundle);
mkdirSync(bundle, { recursive: true });
const artifacts = [
	["input.json", "input", inputArtifact],
	["component.json", "component", componentArtifact],
	["screen-state.json", "screen-state", screenArtifact],
	["capability-ledger.json", "capability-ledger", capabilityArtifact],
	["utils-width.json", "utils-width", utilsWidthArtifact],
	["utils-ansi.json", "utils-ansi", utilsAnsiArtifact],
] as const;
const records = artifacts.map(([file, family, artifact]) => {
	const bytes = Buffer.from(JSON.stringify(artifact, null, 2) + "\n");
	writeFileSync(path.join(bundle, file), bytes);
	return { path: file, family, bytes: bytes.length, sha256: sha256(bytes), sourceEndpoints: artifact.sourceEndpoints };
});
const manifest = {
	schema: "pike/tui-evidence/1", baseline, revision, capturedAt, environment,
	generator: { path: "capture/capture-named-tui.mts", sha256: sha256(readFileSync(scriptPath)) },
	artifacts: records,
};
const manifestBytes = Buffer.from(JSON.stringify(manifest, null, 2) + "\n");
writeFileSync(path.join(bundle, "manifest.json"), manifestBytes);
selected.manifestSha256 = sha256(manifestBytes);
writeFileSync(registryPath, JSON.stringify(registry, null, 2) + "\n");
console.log(`captured ${records.length} independent TUI families for ${baseline} at ${revision}`);
