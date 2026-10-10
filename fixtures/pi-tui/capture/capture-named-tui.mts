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
const src = (relative: string) => pathToFileURL(path.join(pi, "packages/tui", relative)).href;
const { parseKey, isKeyRepeat, isKeyRelease, setKittyProtocolActive } = await import(src("src/keys.ts"));
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

// No output is touched before source guards and all observations complete.
const bundle = path.join(root, selected.bundle);
mkdirSync(bundle, { recursive: true });
const sha256 = (bytes: Buffer) => createHash("sha256").update(bytes).digest("hex");
const artifacts = [
	["input.json", "input", inputArtifact],
	["component.json", "component", componentArtifact],
	["screen-state.json", "screen-state", screenArtifact],
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
