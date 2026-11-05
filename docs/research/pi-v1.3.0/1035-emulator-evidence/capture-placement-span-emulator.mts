#!/usr/bin/env tsx
/**
 * Real-terminal (Kitty) capture of the frozen `audit-1035` placement-span scenario (#1035).
 *
 * Adapted from `fixtures/pi-v1.3.0/audit-1035/capture-placement-span.mts` (the frozen byte
 * capture, which drove pi's `TuiAltScreen` over pi's `VirtualTerminal`). That capture pinned
 * the WezTerm trigger branch with `WEZTERM_PANE` over a *virtual* terminal; issue #1035's
 * reopened criterion requires the observation in a real Kitty/WezTerm emulator instead of
 * inferring placement/rendering from emitted bytes. This script keeps the same scenario and
 * the same frozen guards, but mirrors every `TuiAltScreen` write to the real stdout (a PTY
 * owned by the real Kitty window this process runs in), so the emulator performs the actual
 * placement, damage and repaint.
 *
 * Usage (inside a real Kitty window, stdout = PTY):
 *   WEZTERM_PANE=1 ../pi/node_modules/.bin/tsx \
 *     docs/research/pi-v1.3.0/1035-emulator-evidence/capture-placement-span-emulator.mts \
 *     wezterm <outDir>            # second-tier span trigger arm
 *   # omit WEZTERM_PANE for the plain-terminal control arm.
 *
 * Capabilities are detected from the real environment (no `setCapabilities` pinning); the
 * detected set is asserted to be Kitty and recorded in the evidence JSON. A real PNG (not
 * the frozen "AAAA" placeholder) is required so the emulator has pixels to place; the same
 * file is used for the C++ run.
 */

import { writeFileSync, writeSync } from "node:fs";
import path from "node:path";

const scenario = process.argv[2] ?? "wezterm";
const outDir = process.argv[3] ?? ".";
if (scenario !== "wezterm" && scenario !== "control") {
	throw new Error(`usage: capture-placement-span-emulator.mts <wezterm|control> <outDir>`);
}

const piCheckout = process.env.PI_CHECKOUT ?? path.resolve(process.cwd(), "../pi");
const source = (relative: string) =>
	new URL(`file://${path.join(piCheckout, "packages/tui", relative).replaceAll(path.sep, "/")}`).href;

const terminalImage = await import(source("src/terminal-image.ts"));
const { TuiAltScreen } = await import(source("src/tui-alt-screen.ts"));
const { VirtualTerminal } = await import(source("test/virtual-terminal.ts"));
const { getCapabilities, encodeKitty, registerKittyImageMetadata } = terminalImage;
const { execFileSync } = await import("node:child_process");

// The real grid must match the frozen 20x4 scenario: kitty adopts a child-set winsize
// (probed on this host), so size the real PTY before the TUI starts painting.
execFileSync("stty", ["rows", "4", "cols", "20"], { stdio: "inherit" });
await new Promise((resolve) => setTimeout(resolve, 300));

// Mirror the frozen capture's env pinning, except KITTY_WINDOW_ID/TERM stay real: the
// observation must run under the actual emulator environment.
if (scenario === "wezterm") process.env.WEZTERM_PANE = "1";
else delete process.env.WEZTERM_PANE;
delete process.env.TERM_PROGRAM;

const capabilities = getCapabilities();
if (capabilities.images !== "kitty") {
	throw new Error(`real Kitty detection failed: images=${JSON.stringify(capabilities.images)}`);
}

const IMAGE_ID = 7301;
const imageBase64 = process.env.PLACEMENT_IMAGE_B64 ?? "";
if (imageBase64.length < 100) {
	throw new Error("PLACEMENT_IMAGE_B64 must carry the real shared PNG (same file as the C++ run)");
}

const terminal = new VirtualTerminal(20, 4);
const writes: string[] = [];
const original = terminal.write.bind(terminal);
terminal.write = (data: string) => {
	writes.push(data);
	original(data);
	try {
		// The real PTY: the emulator renders these bytes.
		writeSync(1, data);
	} catch {
		// The VirtualTerminal model must stay consistent even if the PTY is gone.
	}
};

registerKittyImageMetadata({
	imageId: IMAGE_ID,
	columns: 2,
	rows: 3,
	widthPx: 100,
	heightPx: 100,
});
const imageLine = encodeKitty(imageBase64, {
	columns: 2,
	rows: 3,
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

const sleep = (ms: number) => new Promise((resolve) => setTimeout(resolve, ms));
const phaseDir = process.env.PHASE_DIR;
const markPhase = (name: string) => {
	if (!phaseDir) return;
	try {
		writeFileSync(path.join(phaseDir, `${Date.now()}-${name}.txt`), `${name}\n`);
	} catch {
		// Markers only correlate screenshots; a failed marker must not fail the capture.
	}
};
const phases: any[] = [];
let cursor = 0;
const observe = async (action: string) => {
	await terminal.waitForRender();
	phases.push({
		action,
		viewport: terminal.getViewport().map((line: string) => line.trimEnd()),
		writes: writes.slice(cursor),
	});
	markPhase(action);
	cursor = writes.length;
};

tui.start();
await observe("initial");
await sleep(1200);

coveredLine = "changed";
tui.requestRender();
await observe("covered-text-change");
await sleep(1200);

tailLine = "done";
tui.requestRender();
await observe("outside-span-change");
await sleep(1200);
tui.stop();

// Frozen guards: the real-terminal byte stream must still satisfy the frozen contract.
{
	const frame = phases[1].writes.join("");
	const eraseAt = frame.indexOf("\x1b[2;1H\x1b[2K");
	const textAt = frame.indexOf("\x1b[2;1Hchanged");
	const placementAt = frame.indexOf("\x1b_Ga=p,q=2");
	if (scenario === "wezterm") {
		if (!frame.includes("\x1b_Ga=d,d=a,q=2\x1b\\"))
			throw new Error("WezTerm span repaint must delete all placements before the frame writes");
		if (!(eraseAt >= 0 && textAt > eraseAt && placementAt > textAt))
			throw new Error("WezTerm span repaint must emit erase pass, then text rows, then image bytes last");
		if (frame.includes("a=T")) throw new Error("WezTerm span repaint must reuse the upload with a=p, not retransmit");
		if (phases[2].writes.join("").includes("\x1b_G"))
			throw new Error("a change outside every placement span must not repaint images, even under WezTerm");
	} else {
		if (frame.includes("\x1b_G"))
			throw new Error("the non-WezTerm control must not emit any image command for a span-covered change");
		if (!frame.includes("\x1b[2;1H\x1b[2Kchanged"))
			throw new Error("the non-WezTerm control must rewrite only the changed covered row");
	}
}

const evidence = {
	capturedAt: new Date().toISOString(),
	scenario,
	runtime: "frozen-pi-v1.3.0",
	revision: "71023aec1546d6d677a65669c5665c0927ce0dd0",
	environment: {
		TERM: process.env.TERM,
		KITTY_WINDOW_ID: process.env.KITTY_WINDOW_ID ?? null,
		WEZTERM_PANE: process.env.WEZTERM_PANE ?? null,
		node: process.version,
		detectedCapabilities: capabilities,
	},
	phases,
};
const evidencePath = path.join(outDir, `pi-${scenario}.json`);
writeFileSync(evidencePath, `${JSON.stringify(evidence, null, 2)}\n`);
writeFileSync(
	path.join(outDir, `pi-${scenario}.raw`),
	Buffer.from(phases.map((phase: any) => phase.writes.join("")).join(""), "utf8"),
);
console.log(`captured pi ${scenario} arm -> ${evidencePath}`);
