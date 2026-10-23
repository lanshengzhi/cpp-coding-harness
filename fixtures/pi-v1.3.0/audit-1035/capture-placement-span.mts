#!/usr/bin/env tsx
/**
 * Capture the `audit-1035` image placement-span repaint family against frozen `pi-v1.3.0` (#1035).
 *
 * The frozen trigger set has two tiers (pi `tui-alt-screen.ts:1703-1724`): a changed row at or
 * around an image anchor (an image line itself changed) repaints on every terminal, and — only
 * under WezTerm (`WEZTERM_PANE` or `TERM_PROGRAM=wezterm`) with the Kitty protocol — a changed
 * text row strictly inside a live placement span also repaints, re-placing the image AFTER every
 * erase and text write (pi `drawKittyImagesLast`, `:1748-1751`). The C++ port lacks the second
 * tier and the images-last emission order; this family pins the complete frame bytes for both
 * the WezTerm trigger and the non-WezTerm control so the C++ replay can compare the full write
 * stream, not only the image commands.
 *
 * Usage (from the repository root):
 *   PI_CHECKOUT=<private frozen checkout> PI_BASELINE=pi-v1.3.0 \
 *     <checkout>/node_modules/.bin/tsx fixtures/pi-v1.3.0/audit-1035/capture-placement-span.mts
 *
 * The script re-execs itself under the checkout's tsx so the frozen workspace `paths` resolve.
 * It refuses a checkout whose HEAD is not the registered baseline revision, a modified source
 * tree, a non-Linux/glibc host, and a family already registered to another baseline.
 */

import { spawnSync } from "node:child_process";
import { createHash } from "node:crypto";
import { mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { createRequire } from "node:module";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const scriptPath = fileURLToPath(import.meta.url);
const familyDir = path.dirname(scriptPath);
const auditRoot = path.resolve(familyDir, "..");
const repositoryRoot = path.resolve(auditRoot, "../..");
const family = "audit-1035";
const registryPath = path.join(auditRoot, "baselines.json");
const piCheckout = path.resolve(process.env.PI_CHECKOUT ?? path.resolve(repositoryRoot, "../pi"));

if (process.env.TSX_TSCONFIG_PATH === undefined) {
	const tsx = path.join(piCheckout, "node_modules", ".bin", "tsx");
	const result = spawnSync(tsx, [scriptPath, ...process.argv.slice(2)], {
		stdio: "inherit",
		env: { ...process.env, TSX_TSCONFIG_PATH: path.join(piCheckout, "tsconfig.json") },
	});
	if (result.error) {
		throw new Error(`cannot re-exec ${tsx} to resolve the pi checkout's workspace paths: ${result.error}`);
	}
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
const head = (await import("node:child_process")).execFileSync("git", ["-C", piCheckout, "rev-parse", "HEAD"], {
	encoding: "utf8",
}).trim();
if (head !== baseline.revision) {
	throw new Error(`pi checkout must be at ${baseline.revision} (baseline ${baselineName}), found ${head}`);
}
if ((await import("node:child_process")).execFileSync("git", ["-C", piCheckout, "status", "--porcelain"], { encoding: "utf8" }).trim()) {
	throw new Error("pi capture requires an unmodified source checkout");
}
const glibc = (process.report.getReport() as any).header.glibcVersionRuntime;
if (process.platform !== "linux" || process.arch !== "x64" || !glibc) {
	throw new Error("audit capture requires Linux x86-64/glibc");
}
for (const [name, entry] of Object.entries<any>(registry.baselines)) {
	if (name !== baselineName && entry.bundles && family in entry.bundles) {
		throw new Error(`refusing to capture ${family}: it already belongs to baseline ${JSON.stringify(name)}`);
	}
}

const requirePi = createRequire(path.join(piCheckout, "package.json"));
let tsxVersion = "unknown";
try {
	tsxVersion = requirePi("tsx/package.json").version;
} catch {
	// The tsx version is provenance, not a capture precondition; keep the sentinel explicit.
}

// Pin the terminal environment exactly like the other audit families: the WezTerm
// trigger keys off `process.env.WEZTERM_PANE` at render time, so the two scenarios
// flip only that variable inside one process and record it per scenario.
const pinnedVariables = [
	"TERM_PROGRAM",
	"TERM_PROGRAM_VERSION",
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
for (const name of pinnedVariables) delete process.env[name];
process.env.TERM = "xterm-256color";
const environment = {
	platform: process.platform,
	arch: process.arch,
	libc: "glibc",
	glibcVersion: glibc,
	node: process.version,
	tsx: tsxVersion,
	TERM: process.env.TERM,
	pinnedVariables,
};
const capturedAt = new Date().toISOString();

const sha256 = (bytes: Buffer) => createHash("sha256").update(bytes).digest("hex");
const source = (relative: string) => pathToFileURL(path.join(piCheckout, "packages/tui", relative)).href;
const { encodeKitty, registerKittyImageMetadata, setCapabilities, resetCapabilitiesCache } = await import(
	source("src/terminal-image.ts")
);
const { TuiAltScreen } = await import(source("src/tui-alt-screen.ts"));
const { VirtualTerminal } = await import(source("test/virtual-terminal.ts"));

const KITTY = { images: "kitty", trueColor: true, hyperlinks: false } as const;
const IMAGE_ID = 7301;
const IMAGE = {
	id: IMAGE_ID,
	columns: 2,
	rows: 3,
	widthPx: 100,
	heightPx: 100,
	mimeType: "image/png",
	data: "AAAA",
};
const NORMALIZATIONS = [
	// Kitty image ids are random by construction in production; the replay renames every
	// id to its first-appearance index on both streams before comparing.
	"rename-image-ids",
	// The frozen full redraw appends "\x1b[2J" right after the image cleanup; the C++
	// fullscreen path does not emit it (tracked separately from this ticket's deltas).
	"drop-full-redraw-2J",
	// Frozen re-asserts the hidden hardware cursor ("\x1b[?25l") in every frame; the C++
	// viewport asserts cursor visibility only when it changes.
	"drop-cursor-visibility-repeat",
];

/// One scenario run: the same layout root under one terminal environment, with every
/// write chunk recorded between action boundaries so the observation is the complete
/// frame byte stream, not a summary.
async function captureScenario(weztermPane: boolean) {
	if (weztermPane) process.env.WEZTERM_PANE = "1";
	else delete process.env.WEZTERM_PANE;
	setCapabilities(KITTY);
	try {
		const terminal = new VirtualTerminal(20, 4);
		const writes: string[] = [];
		const original = terminal.write.bind(terminal);
		terminal.write = (data: string) => {
			writes.push(data);
			original(data);
		};
		registerKittyImageMetadata({
			imageId: IMAGE_ID,
			columns: IMAGE.columns,
			rows: IMAGE.rows,
			widthPx: IMAGE.widthPx,
			heightPx: IMAGE.heightPx,
		});
		const imageLine = encodeKitty(IMAGE.data, {
			columns: IMAGE.columns,
			rows: IMAGE.rows,
			imageId: IMAGE_ID,
			moveCursor: false,
		});
		let coveredLine = "";
		let tailLine = "after";
		const tui = new TuiAltScreen(terminal);
		tui.setLayoutRoot({
			render: () => [imageLine, coveredLine, "", tailLine],
			invalidate: () => {},
		});
		tui.start();
		// The alt-screen enter sequence is not part of the frame contract: start slicing
		// after it so each phase carries exactly the frame writes of one render.
		let cursor = writes.length;
		await terminal.waitForRender();
		const phases: any[] = [];
		const observe = async (action: string) => {
			await terminal.waitForRender();
			phases.push({
				action,
				viewport: terminal.getViewport().map((line: string) => line.trimEnd()),
				writes: writes.slice(cursor),
			});
			cursor = writes.length;
		};
		await observe("initial");

		coveredLine = "changed";
		tui.requestRender();
		await observe("covered-text-change");

		tailLine = "done";
		tui.requestRender();
		await observe("outside-span-change");
		tui.stop();

		return phases;
	} finally {
		resetCapabilitiesCache();
		delete process.env.WEZTERM_PANE;
	}
}

const weztermPhases = await captureScenario(true);
const controlPhases = await captureScenario(false);

// Frozen ground truth for the overlay-inside-span divergence proofs: an
// overlay row composed at row 1 (inside the live placement span [0, 3)).
// Frozen keeps the placement and re-emits `a=p` AFTER the overlay text in
// both the shown and hidden frames; the bytes are captured into the artifact
// by the overlay scenario below.
const overlayPhases = await captureOverlayScenario();

/// A second run with an overlay composed over a covered row: the realistic
/// trigger — the overlay row appears INSIDE the live placement span, so the
/// frozen trigger must repaint the images around the overlay bytes.
async function captureOverlayScenario() {
	process.env.WEZTERM_PANE = "1";
	setCapabilities(KITTY);
	try {
		const terminal = new VirtualTerminal(20, 4);
		const writes: string[] = [];
		const original = terminal.write.bind(terminal);
		terminal.write = (data: string) => {
			writes.push(data);
			original(data);
		};
		registerKittyImageMetadata({
			imageId: IMAGE_ID,
			columns: IMAGE.columns,
			rows: IMAGE.rows,
			widthPx: IMAGE.widthPx,
			heightPx: IMAGE.heightPx,
		});
		const imageLine = encodeKitty(IMAGE.data, {
			columns: IMAGE.columns,
			rows: IMAGE.rows,
			imageId: IMAGE_ID,
			moveCursor: false,
		});
		const tui = new TuiAltScreen(terminal);
		tui.setLayoutRoot({
			render: () => [imageLine, "", "", "after"],
			invalidate: () => {},
		});
		tui.start();
		let cursor = writes.length;
		await terminal.waitForRender();
		const phases: any[] = [];
		const observe = async (action: string) => {
			await terminal.waitForRender();
			phases.push({
				action,
				viewport: terminal.getViewport().map((line: string) => line.trimEnd()),
				writes: writes.slice(cursor),
			});
			cursor = writes.length;
		};
		await observe("initial");

		// Row 1 is inside the placement span [0, 3): composing the overlay row
		// there is the frozen #10319 regression shape.
		const handle = tui.showOverlay(
			{ render: () => ["overlay!"], invalidate: () => {} },
			{ row: 1, col: 0, width: 20, maxHeight: 1 } as any,
		);
		await observe("overlay-shown");
		handle.hide();
		await observe("overlay-hidden");
		tui.stop();
		return phases;
	} finally {
		resetCapabilitiesCache();
		delete process.env.WEZTERM_PANE;
	}
}

// Guards: fail the capture unless the frozen binary really exhibits the two-tier trigger.
{
	const frame = weztermPhases[1].writes.join("");
	const eraseAt = frame.indexOf("\x1b[2;1H\x1b[2K");
	const textAt = frame.indexOf("\x1b[2;1Hchanged");
	const placementAt = frame.indexOf("\x1b_Ga=p,q=2");
	if (!frame.includes("\x1b_Ga=d,d=a,q=2\x1b\\"))
		throw new Error("WezTerm span repaint must delete all placements before the frame writes");
	if (!(eraseAt >= 0 && textAt > eraseAt && placementAt > textAt))
		throw new Error("WezTerm span repaint must emit erase pass, then text rows, then image bytes last");
	if (frame.includes("a=T")) throw new Error("WezTerm span repaint must reuse the upload with a=p, not retransmit");
	if (weztermPhases[2].writes.join("").includes("\x1b_G"))
		throw new Error("a change outside every placement span must not repaint images, even under WezTerm");
	const controlFrame = controlPhases[1].writes.join("");
	if (controlFrame.includes("\x1b_G"))
		throw new Error("the non-WezTerm control must not emit any image command for a span-covered change");
	if (!controlFrame.includes("\x1b[2;1H\x1b[2Kchanged"))
		throw new Error("the non-WezTerm control must rewrite only the changed covered row");
	for (const action of ["overlay-shown", "overlay-hidden"]) {
		const phase = overlayPhases.find((entry: any) => entry.action === action);
		const frame = phase.writes.join("");
		if (!frame.includes("\x1b_Ga=d,d=a,q=2\x1b\\"))
			throw new Error(`the overlay ${action} frame must delete all placements first`);
		const placementAt = frame.indexOf("\x1b_Ga=p,q=2");
		const textAt = frame.indexOf("overlay!");
		if (action === "overlay-shown" && !(textAt >= 0 && placementAt > textAt))
			throw new Error("the overlay text must be written before the image re-placement");
		if (placementAt < 0) throw new Error(`the overlay ${action} frame must re-place the image`);
	}
}

const sourceEndpoints = [
	"packages/tui/src/tui-alt-screen.ts:imageCellsNeedRedraw,drawKittyImagesLast,prepareKittyScreen,compositeOverlays,doRender",
	"packages/tui/src/tui.ts:showOverlay",
	"packages/tui/src/terminal-image.ts:encodeKitty,getKittyImagePlacement,getKittyImagePlacementRows,deleteAllKittyPlacements",
	"packages/tui/test/tui-alt-screen.test.ts:redraws WezTerm Kitty images after writes to covered rows",
];
const upstreamTest = {
	path: "packages/tui/test/tui-alt-screen.test.ts",
	case: "redraws WezTerm Kitty images after writes to covered rows",
};
const scenarios = [
	{
		name: "wezterm-placement-span-repaint",
		inputs: {
			dimensions: { columns: 20, rows: 4 },
			capabilities: "kitty",
			weztermPane: true,
			image: IMAGE,
			content: {
				covered: "",
				changedCovered: "changed",
				tail: "after",
				changedTail: "done",
				rows: ["image-anchor", "covered-text", "", "tail"],
			},
		},
		expected: { phases: weztermPhases },
		normalizations: NORMALIZATIONS,
		upstreamTest,
	},
	{
		name: "plain-terminal-placement-span-control",
		inputs: {
			dimensions: { columns: 20, rows: 4 },
			capabilities: "kitty",
			weztermPane: false,
			image: IMAGE,
			content: {
				covered: "",
				changedCovered: "changed",
				tail: "after",
				changedTail: "done",
				rows: ["image-anchor", "covered-text", "", "tail"],
			},
		},
		expected: { phases: controlPhases },
		normalizations: NORMALIZATIONS,
		upstreamTest,
	},
	{
		name: "wezterm-overlay-inside-placement-span",
		inputs: {
			dimensions: { columns: 20, rows: 4 },
			capabilities: "kitty",
			weztermPane: true,
			image: IMAGE,
			content: {
				rows: ["image-anchor", "covered-text", "", "tail"],
				overlay: { line: "overlay!", row: 1, column: 0, width: 20, maxHeight: 1 },
			},
		},
		expected: { phases: overlayPhases },
		normalizations: NORMALIZATIONS,
		upstreamTest,
	},
];

const artifactName = "placement-span.json";
const artifact = {
	schema: "pike/pi-v1.3.0-audit-evidence/1",
	baseline: baselineName,
	revision: baseline.revision,
	capturedAt,
	environment,
	sourceEndpoints,
	scenarios,
};
const artifactBytes = Buffer.from(`${JSON.stringify(artifact, null, 2)}\n`);
const generatorPath = path.relative(repositoryRoot, scriptPath).split(path.sep).join("/");
const generatorBytes = readFileSync(scriptPath);
const manifest = {
	schema: "pike/pi-v1.3.0-audit-evidence/1",
	baseline: baselineName,
	revision: baseline.revision,
	ticket: "1035",
	family,
	capturedAt,
	environment,
	generator: {
		path: generatorPath,
		sha256: sha256(generatorBytes),
		command: `PI_CHECKOUT=${piCheckout} PI_BASELINE=${baselineName} ${path.join(piCheckout, "node_modules/.bin/tsx")} ${generatorPath}`,
		sources: [{ path: generatorPath, sha256: sha256(generatorBytes) }],
	},
	artifacts: [
		{
			path: artifactName,
			family,
			bytes: artifactBytes.length,
			sha256: sha256(artifactBytes),
			sourceEndpoints,
			scenarios,
		},
	],
};

// Write only after every guard and observation succeeded.
mkdirSync(familyDir, { recursive: true });
writeFileSync(path.join(familyDir, artifactName), artifactBytes);
writeFileSync(path.join(familyDir, "manifest.json"), `${JSON.stringify(manifest, null, 2)}\n`);
console.log(`captured ${family} artifact ${artifactName} for ${baselineName} (${baseline.revision})`);
