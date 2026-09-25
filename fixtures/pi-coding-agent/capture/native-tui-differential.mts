#!/usr/bin/env tsx
/**
 * Deterministic dual-runtime Native TUI evidence harness (issue #800).
 *
 * The default mode runs the frozen pi checkout and the rebuilt Pike test
 * binary through the same scenario/input records, then writes a sanitized
 * structural report. `--verify` is the CTest mode: it regenerates the
 * projections and compares them with the checked-in report. The compared
 * boundary is visible/scrollback cell text plus the normalized, ordered SGR
 * token structure; raw ANSI bytes are retained as captured evidence only and
 * are never compared, so the harness makes no full raw-ANSI byte-parity
 * claim. Theme parity compares the canonical semantic role mapping (exact RGB
 * is retained as evidence); a palette difference that preserves the role
 * partition is not a regression (issue #797).
 *
 * `--runtime pi` is an internal per-scenario child mode. It exists so each pi
 * capture gets a fresh session, terminal, and faux Provider; the parent still
 * owns scenario definitions and comparison policy.
 *
 * Offline is the default and is enforced by the parent environment. A live
 * manual run is intentionally separate: use the documented command in
 * fixtures/pi-coding-agent/README.md with a real PTY and your own provider
 * settings. No live provider is contacted by this harness.
 */

import assert from "node:assert/strict";
import { execFileSync, spawnSync } from "node:child_process";
import { existsSync, mkdirSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { createHash } from "node:crypto";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const scriptPath = fileURLToPath(import.meta.url);
const scriptDir = path.dirname(scriptPath);
const fixtureDir = path.resolve(scriptDir, "..");
const repoRoot = path.resolve(fixtureDir, "../..");
const differentialDir = path.join(fixtureDir, "differential");
const reportPath = path.join(differentialDir, "report.json");
const frozenCommit = "f07218c4d4bbc12bef056a7058c3dd49dfe41abe";
const packageName = "@earendil-works/pi-coding-agent";
const packageVersion = "0.87.1";
const semanticThemeRoles = Object.freeze([
	"text",
	"muted",
	"border",
	"accent",
	"success",
	"warning",
	"error",
	"selectedBg",
]);
// Semantic role mapping is the parity authority (issue #797): exact-token
// match and a partition-preserving palette difference both satisfy the
// contract; only a broken role mapping is a Supported Capability regression.
const acceptableThemeParityClassifications = new Set(["match", "semantic-role-match-palette-difference"]);
const acceptableThemeRoleClassifications = new Set(["match", "semantic-role-preserved-palette-difference"]);
const profile = Object.freeze({
	home: "/home/tester",
	userProfile: "/home/tester",
	xdgConfigHome: "/tmp/cpp-harness-pike-differential-<scenario>/xdg/config",
	agentDirectory: "/tmp/cpp-harness-pike-differential-<scenario>/xdg/config/pike/agent",
	workspace: "<deterministic-workspace>",
	projectResourceDirectory: "<deterministic-workspace>/.pi",
	userAgentsDirectory: "/home/tester/.agents",
	viewportRows: 24,
	settings: { theme: "dark", scope: "in-memory" },
	theme: { name: "dark", source: "built-in", colorCapability: "truecolor" },
	locale: { LANG: "C.UTF-8", LC_ALL: "C.UTF-8" },
	terminalVariables: {
		TERM: "xterm-256color",
		COLORTERM: "truecolor",
		COLORFGBG: "15;0",
		NO_COLOR: undefined,
	},
	configuration: {
		credentials: "in-memory faux provider",
		extensions: "disabled",
		userSettings: "in-memory dark theme",
		projectResources: "empty deterministic .pi directory",
	},
	provider: "deterministic-faux",
});

type Scenario = {
	id: string;
	width: number;
	inputs: string[];
	resize?: string;
	omissions: string[];
	omissionPatterns?: string[];
};

const scenarios: Scenario[] = [
	{ id: "boot-72", width: 72, inputs: [], omissions: [] },
	{ id: "boot-100", width: 100, inputs: [], omissions: [] },
	{ id: "boot-120", width: 120, inputs: [], omissions: [] },
	{ id: "boot-41", width: 41, inputs: [], omissions: [] },
	{ id: "model-selector", width: 100, inputs: ["\x0c", "\r"], omissions: [] },
	{ id: "settings-selector", width: 100, inputs: ["/settings", "\r", "\r", "\x1b"], omissions: ["pi-only settings outside the Supported Capability subset are not projected as Pike capabilities"], omissionPatterns: ["Auto-compact", "Auto-resize", "Block images", "Show hardware cursor", "Editor padding", "Autocomplete max items", "Clear on shrink", "Terminal progress", "Automatically compact context", "extension", "Extension", "package", "Package"] },
	{ id: "thinking-selector", width: 100, inputs: ["/thinking", "\r", "\r"], omissions: [] },
	{ id: "editor-long", width: 72, inputs: ["A long editor line that must wrap without losing the fixture-authored suffix", "\x15"], omissions: [] },
	{ id: "editor-cjk", width: 72, inputs: ["混在文本 mixed 日本語 ✅", "\x15"], omissions: [] },
	{ id: "editor-token", width: 41, inputs: ["UNBREAKABLE_TOKEN_abcdefghijklmnopqrstuvwxyz0123456789", "\x15"], omissions: [] },
	{ id: "user-message", width: 72, inputs: ["deterministic user prompt\r"], omissions: [] },
	{ id: "tool-result", width: 100, inputs: ["run the deterministic tool\r"], omissions: [] },
	{ id: "status-footer", width: 100, inputs: ["deterministic status prompt\r"], omissions: ["pi provider-catalog and extension-status chrome is outside the Supported Capability subset"], omissionPatterns: ["provider", "Provider", "extension", "Extension", "cache", "Cache"] },
	{ id: "resize-72-41", width: 72, inputs: [], resize: "41x30->72x30", omissions: [] },
	{ id: "resize-100-72", width: 100, inputs: [], resize: "72x40->100x40", omissions: [] },
	{ id: "resize-120-100", width: 120, inputs: [], resize: "100x50->120x50", omissions: [] },
	{ id: "scrollback", width: 72, inputs: ["line one\r", "line two\r", "line three\r", "line four\r", "line five\r", "line six\r"], omissions: [] },
];

const volatileModelProse = [
	"deterministic assistant reply",
	"deterministic status reply",
	"deterministic tool answer",
	"deterministic scrollback reply 1",
	"deterministic scrollback reply 2",
	"deterministic scrollback reply 3",
	"deterministic scrollback reply 4",
	"deterministic scrollback reply 5",
	"deterministic scrollback reply 6",
];

const differentialModels = [
	{
		id: "faux-1",
		name: "Faux Reasoning",
		reasoning: true,
		input: ["text", "image"] as ("text" | "image")[],
		cost: { input: 0, output: 0, cacheRead: 0, cacheWrite: 0 },
		contextWindow: 128000,
		maxTokens: 16384,
	},
	{
		id: "faux-2",
		name: "Faux Plain",
		reasoning: false,
		input: ["text", "image"] as ("text" | "image")[],
		cost: { input: 0, output: 0, cacheRead: 0, cacheWrite: 0 },
		contextWindow: 128000,
		maxTokens: 16384,
	},
];

const usageProfileScenarios = new Set(["user-message", "status-footer", "tool-result"]);

// These rows identify the pi runtime or its startup documentation. They are
// profile evidence, not Native TUI capability cells, so the diagnostic
// profile projection removes only these exact forms. The strict projection
// below remains the classification authority.
// debt: this is a frozen v0.87.1 row allowlist; update it only when the differential baseline advances.
const profileCellRowProjections = Object.freeze([
	{ name: "runtime-identity", pattern: /^\s*pi v\d+\.\d+\.\d+\s*$/ },
	{ name: "startup-documentation", pattern: /^\s*Pi can explain its own features and look up its docs\. Ask it how to(?: use or extend Pi\.)?\s*$/ },
	{ name: "startup-documentation", pattern: /^\s*use or extend Pi\.\s*$/ },
]);

function childEnvironment(extra: Record<string, string>): NodeJS.ProcessEnv {
	const environment: NodeJS.ProcessEnv = {
		PATH: `${path.dirname(process.execPath)}:/usr/bin:/bin`,
		LANG: "C.UTF-8",
		LC_ALL: "C.UTF-8",
		TZ: "UTC",
	};
	return {
		...environment,
		HOME: profile.home,
		USERPROFILE: profile.userProfile,
		PI_OFFLINE: "1",
		PI_SKIP_VERSION_CHECK: "1",
		TERM: profile.terminalVariables.TERM,
		COLORTERM: profile.terminalVariables.COLORTERM,
		COLORFGBG: profile.terminalVariables.COLORFGBG,
		...extra,
	};
}

function repositoryRootFromEnvironment(): string {
	return process.env.CCH_SOURCE_DIR ?? repoRoot;
}

function piCheckout(): string {
	return process.env.PI_CHECKOUT ?? path.resolve(repositoryRootFromEnvironment(), "../pi");
}

function frozenCheckoutOrSkip(): string {
	const checkout = piCheckout();
	const tsx = path.join(checkout, "node_modules/.bin/tsx");
	if (!existsSync(checkout) || !existsSync(tsx)) {
		throw new SkipError(`pi checkout or tsx is unavailable at ${checkout}`);
	}
	const head = execFileSync("git", ["-C", checkout, "rev-parse", "HEAD"], { encoding: "utf8" }).trim();
	if (head !== frozenCommit) throw new Error(`pi checkout must be ${frozenCommit}, found ${head}`);
	const packageJson = JSON.parse(readFileSync(path.join(checkout, "packages/coding-agent/package.json"), "utf8")) as {
		name: string;
		version: string;
	};
	if (packageJson.name !== packageName || packageJson.version !== packageVersion) {
		throw new Error(`pi checkout must declare ${packageName}@${packageVersion}`);
	}
	return checkout;
}

class SkipError extends Error {}

function parseDimensions(value: string): { columns: number; rows: number } {
	const match = /^(\d+)x(\d+)$/.exec(value);
	if (match === null) throw new Error(`invalid differential dimensions: ${value}`);
	return { columns: Number(match[1]), rows: Number(match[2]) };
}

function parseResizeSequence(value: string): Array<{ columns: number; rows: number }> {
	const sequence = value.split("->").map(parseDimensions);
	if (sequence.length === 0) throw new Error(`invalid differential resize sequence: ${value}`);
	return sequence;
}

function writeJson(pathname: string, value: unknown, pretty = false): void {
	const serialized = pretty ? JSON.stringify(value, null, 2) : JSON.stringify(value);
	writeFileSync(pathname, `${serialized}\n`);
}

function captureTsconfig(checkout: string): string {
	if (existsSync(path.join(checkout, "node_modules/grok-mermaid"))) {
		return path.join(checkout, "tsconfig.json");
	}
	const rootConfig = JSON.parse(readFileSync(path.join(checkout, "tsconfig.json"), "utf8")) as {
		compilerOptions?: { paths?: Record<string, string[]> };
	};
	const paths = { ...(rootConfig.compilerOptions?.paths ?? {}) };
	paths["grok-mermaid"] = [path.resolve(scriptDir, "grok-mermaid-stub.mts")];
	const capturePath = path.join(checkout, "tsconfig.capture.json");
	writeFileSync(
		capturePath,
		`${JSON.stringify({
			compilerOptions: {
				target: "ES2022",
				module: "NodeNext",
				moduleResolution: "NodeNext",
				allowImportingTsExtensions: true,
				rewriteRelativeImportExtensions: true,
				esModuleInterop: true,
				skipLibCheck: true,
				types: ["node"],
				baseUrl: ".",
				paths,
			},
		}, null, 2)}\n`,
	);
	return capturePath;
}

function hex(value: string): string {
	return Buffer.from(value, "utf8").toString("hex");
}

function scenarioPaths(scenario: Scenario): { workspace: string; xdgConfigHome: string; agentDirectory: string } {
	const root = `/tmp/cpp-harness-pike-differential-${process.pid}-${scenario.id}`;
	return {
		workspace: root,
		xdgConfigHome: `${root}/xdg/config`,
		agentDirectory: `${root}/xdg/config/pike/agent`,
	};
}

function scenarioEnvironment(scenario: Scenario, output: string): Record<string, string> {
	const paths = scenarioPaths(scenario);
	return {
		CCH_DIFFERENTIAL_SCENARIO: scenario.id,
		CCH_DIFFERENTIAL_WORKSPACE: paths.workspace,
		CCH_DIFFERENTIAL_WIDTH: String(scenario.width),
		CCH_DIFFERENTIAL_INPUTS: scenario.inputs.map((input) => hex(input) || "-").join("|"),
		CCH_DIFFERENTIAL_OUTPUT: output,
		CCH_DIFFERENTIAL_AGENT_DIRECTORY: paths.agentDirectory,
		XDG_CONFIG_HOME: paths.xdgConfigHome,
		PI_CODING_AGENT_DIR: paths.agentDirectory,
		...(scenario.resize === undefined ? {} : { CCH_DIFFERENTIAL_RESIZE: scenario.resize }),
	};
}

function resetScenarioRoot(scenario: Scenario): void {
	const paths = scenarioPaths(scenario);
	rmSync(path.dirname(paths.xdgConfigHome), { recursive: true, force: true });
}

function runPike(scenario: Scenario, output: string): Record<string, unknown> {
	resetScenarioRoot(scenario);
	const binary = process.env.CCH_DIFFERENTIAL_BINARY ?? path.join(repositoryRootFromEnvironment(), "build/cch_tests_coding_agent_interactive");
	if (!existsSync(binary)) throw new Error(`Pike differential test binary not found at ${binary}`);
	const pikeEnvironment = scenarioEnvironment(scenario, output);
	try {
		execFileSync(binary, ["[issue800]"], {
			cwd: repositoryRootFromEnvironment(),
			env: childEnvironment(pikeEnvironment),
			stdio: ["ignore", "pipe", "pipe"],
			encoding: "utf8",
		});
	} catch (error) {
		const detail = error instanceof Error ? error.message : String(error);
		throw new Error(`Pike differential scenario ${scenario.id} failed: ${detail}`);
	}
	return JSON.parse(readFileSync(output, "utf8")) as Record<string, unknown>;
}

async function runPi(scenario: Scenario, output: string): Promise<Record<string, unknown>> {
	resetScenarioRoot(scenario);
	const checkout = frozenCheckoutOrSkip();
	const tsx = path.join(checkout, "node_modules/.bin/tsx");
	const result = execFileSync(tsx, ["--tsconfig", captureTsconfig(checkout), scriptPath, "--runtime", "pi"], {
		cwd: repositoryRootFromEnvironment(),
		env: childEnvironment({
			...scenarioEnvironment(scenario, output),
			PI_CHECKOUT: checkout,
		}),
		stdio: ["ignore", "pipe", "pipe"],
		encoding: "utf8",
	});
	void result;
	return JSON.parse(readFileSync(output, "utf8")) as Record<string, unknown>;
}

function replaceFixturePaths(value: string, workspace: string, repositoryRoot: string, scenario?: Scenario): string {
	const scenarioWorkspace = scenario === undefined
		? undefined
		: `/tmp/cpp-harness-pike-differential-${scenario.id}`;
	return value
		.replaceAll(workspace, "<deterministic-workspace>")
		.replaceAll(scenarioWorkspace ?? "/tmp/cpp-harness-pike-differential-unused", "<deterministic-workspace>")
		.replaceAll(/\/tmp\/cpp-harness-pike-differential-[0-9]+[A-Za-z0-9_.-]*/g, "<deterministic-workspace>")
		.replaceAll("/tmp/cpp-harness-pike-differential", "<deterministic-workspace>")
		.replaceAll("/tmp/cpp-harness-pike-differential/", "<deterministic-workspace>/")
		.replaceAll(repositoryRoot, "<repository-root>")
		// debt: branch text is normalized only when attached to the repository root; replace this with a typed cell projection if branch syntax expands.
		.replaceAll(/<repository-root> \([^()\r\n]*\)+/g, "<repository-root> (<branch>)")
		.replaceAll(/\/home\/[A-Za-z0-9_.-]+/g, "<home>")
		.replaceAll(/<home>\/Work\/github\/coding-agent\/[A-Za-z0-9_.-]+/g, "<repository-root>")
		.replaceAll(/\/tmp\/pi-suite-[A-Za-z0-9_-]+/g, "<deterministic-workspace>")
		.replaceAll("faux-1", "<model>")
		.replaceAll("faux-2", "<model>")
		.replaceAll("fake-model", "<model>");
}

function normalizeCellText(value: string, workspace: string, repositoryRoot: string, scenario?: Scenario): string {
	return replaceFixturePaths(value, workspace, repositoryRoot, scenario)
		.split("\n")
		.map((line) => {
			let normalized = line.trimEnd();
			for (const prose of volatileModelProse) normalized = normalized.replaceAll(prose, "<model-prose>");
			return normalized.replace(/^(\s*)<repository-root> \(<branch>\)+$/, "$1<deterministic-workspace>");
		})
		.join("\n");
}

// The differential profile is truecolor on both runtimes. Preserve exact RGB
// values as SGR evidence; only canonicalize the colon/semicolon encoding.
function normalizeSgrToken(token: string): string {
	return token
		.replace(
			/\x1b\[(38|48):2:(?:0)?:(\d+):(\d+):(\d+)m/g,
			(_match, role: string, red: string, green: string, blue: string) =>
				`\x1b[${role};2;${Number(red)};${Number(green)};${Number(blue)}m`,
		)
		.replace(/\x1b\[(38|48):5:(\d+)m/g, (_match, role: string, index: string) =>
			`\x1b[${role};5;${Number(index)}m`);
}

function verifySgrNormalization(): void {
	assert.equal(normalizeSgrToken("\x1b[38;2;95;135;175m"), "\x1b[38;2;95;135;175m");
	assert.equal(normalizeSgrToken("\x1b[48;2;95;135;175m"), "\x1b[48;2;95;135;175m");
	assert.equal(normalizeSgrToken("\x1b[38:2::95:135:175m"), "\x1b[38;2;95;135;175m");
	assert.equal(normalizeSgrToken("\x1b[48:2:0:95:135:175m"), "\x1b[48;2;95;135;175m");
	assert.equal(normalizeSgrToken("\x1b[38:5:67m"), "\x1b[38;5;67m");
	assert.equal(normalizeSgrToken("\x1b[38:5:67m"), "\x1b[38;5;67m");
}

function sgrTokens(ansi: string): string[] {
	return [...ansi.matchAll(/\x1b\[[0-9;:]*m/g)].map((match) => match[0]);
}

type ThemeEvidence = {
	name: string;
	colorCapability: string;
	colors: Record<string, string>;
};

function normalizeThemeEvidence(value: unknown, label: string): ThemeEvidence {
	if (typeof value !== "object" || value === null) throw new Error(`${label} theme evidence must be an object`);
	const theme = value as Record<string, unknown>;
	if (theme.name !== profile.theme.name || theme.colorCapability !== profile.theme.colorCapability) {
		throw new Error(`${label} theme evidence does not use the canonical dark truecolor profile`);
	}
	if (typeof theme.colors !== "object" || theme.colors === null) throw new Error(`${label} theme colors must be an object`);
	const colors: Record<string, string> = {};
	for (const [role, color] of Object.entries(theme.colors as Record<string, unknown>)) {
		if (typeof color !== "string" || !/^#[0-9a-f]{6}$/i.test(color)) {
			throw new Error(`${label} theme role ${role} must be canonical RGB evidence`);
		}
		colors[role] = color.toLowerCase();
	}
	for (const role of semanticThemeRoles) {
		if (colors[role] === undefined) throw new Error(`${label} theme is missing semantic role ${role}`);
	}
	return { name: theme.name as string, colorCapability: theme.colorCapability as string, colors };
}

// Semantic role partners: the set of canonical roles sharing role's color.
// The parity contract compares this mapping, not raw RGB identity (issue
// #797): a palette difference that preserves the partition is evidence, not a
// Supported Capability regression.
function themeRolePartners(colors: Record<string, string>, role: string): string[] {
	return semanticThemeRoles.filter((other) => colors[other] === colors[role]);
}

function themeRoleComparison(pike: ThemeEvidence, pi: ThemeEvidence): Record<string, unknown> {
	const roles = semanticThemeRoles.map((role) => {
		const pikePartners = themeRolePartners(pike.colors, role);
		const piPartners = themeRolePartners(pi.colors, role);
		const exactTokenMatch = pike.colors[role] === pi.colors[role];
		const partnersPreserved = JSON.stringify(pikePartners) === JSON.stringify(piPartners);
		// Exact-token equality short-circuits the per-role verdict: when a
		// partition break leaves a role's own color unchanged (its partner moved
		// onto it), the role whose color changed is the one flagged, so a broken
		// mapping always surfaces as at least one regression row.
		const classification = exactTokenMatch
			? "match"
			: partnersPreserved
				? "semantic-role-preserved-palette-difference"
				: "supported-capability-regression";
		return {
			role,
			pike: pike.colors[role],
			pi: pi.colors[role],
			exactTokenMatch,
			partnersPreserved,
			classification,
		};
	});
	const supportedCapabilityMismatches = roles.filter((role) => role.classification === "supported-capability-regression");
	const paletteDifferences = roles.filter((role) => role.classification === "semantic-role-preserved-palette-difference");
	const deferredFallbacks: Record<string, string> = {
		scrollbarThumb: "selectedBg",
		scrollbarTrack: "muted",
		searchMatchBg: "selectedBg",
		searchMatchText: "text",
	};
	const deferredTokenPolicy = Object.entries(deferredFallbacks).map(([role, fallback]) => {
		const pikeColor = pike.colors[role] ?? pike.colors[fallback];
		const piColor = pi.colors[role];
		return {
			role,
			pike: pikeColor,
			pi: piColor,
			fallback,
			classification: pikeColor === piColor ? "aligned-in-canonical-profile" : "deferred-capability-difference",
			rationale: "the corresponding pi surface is Deferred and is not promoted by this evidence profile",
		};
	});
	return {
		canonicalTheme: { name: profile.theme.name, source: profile.theme.source, colorCapability: profile.theme.colorCapability },
		roles,
		classification: supportedCapabilityMismatches.length > 0
			? "supported-capability-regression"
			: paletteDifferences.length > 0
				? "semantic-role-match-palette-difference"
				: "match",
		supportedCapabilityMismatches,
		paletteDifferences,
		intentionalDivergences: [],
		deferredTokenPolicy,
		deferredDifferences: deferredTokenPolicy.filter((token) => token.classification === "deferred-capability-difference"),
	};
}

function verifyThemeParityPolicy(): void {
	const colors = Object.fromEntries(semanticThemeRoles.map((role) => [role, "#123456"]));
	const pike = { name: "dark", colorCapability: "truecolor", colors };
	const pi = { name: "dark", colorCapability: "truecolor", colors: { ...colors } };
	const matching = themeRoleComparison(pike, pi);
	assert.equal(matching.classification, "match");
	assert.equal((matching.supportedCapabilityMismatches as unknown[]).length, 0);
	// Exact-token equality is retained as evidence on every role row.
	for (const role of matching.roles as Record<string, unknown>[]) assert.equal(role.exactTokenMatch, true);
	const mismatched = themeRoleComparison(pike, { ...pi, colors: { ...colors, accent: "#654321" } });
	assert.equal(mismatched.classification, "supported-capability-regression");
	assert.equal((mismatched.supportedCapabilityMismatches as Record<string, unknown>[])[0]?.role, "accent");
	// A palette difference that preserves the semantic role partition is
	// recorded as evidence, never as a false Supported Capability regression:
	// this is the case an exact-hex stand-in misclassifies (issue #797).
	const shiftedPalette = Object.fromEntries(semanticThemeRoles.map((role) => [role, "#abcdef"]));
	const shifted = themeRoleComparison(pike, { ...pi, colors: shiftedPalette });
	assert.equal(shifted.classification, "semantic-role-match-palette-difference");
	assert.equal((shifted.supportedCapabilityMismatches as unknown[]).length, 0);
	assert.equal((shifted.paletteDifferences as unknown[]).length, semanticThemeRoles.length);
	assert.equal(((shifted.roles as Record<string, unknown>[])[0]).exactTokenMatch, false);
	// Collapsing two roles the canonical profile distinguishes is a regression
	// even though every pi color remains a plausible palette entry: this is the
	// case a palette-membership stand-in would let through.
	const trulyCollapsed = themeRoleComparison(
		{ name: "dark", colorCapability: "truecolor", colors: { ...colors, text: "#111111" } },
		{ name: "dark", colorCapability: "truecolor", colors: { ...colors, text: "#111111", muted: "#111111" } },
	);
	assert.equal(trulyCollapsed.classification, "supported-capability-regression");
	const collapsedRoles = (trulyCollapsed.supportedCapabilityMismatches as Record<string, unknown>[]).map((role) => role.role);
	// The collapsed role is the one whose color moved onto text's; text itself
	// still renders the same color, so exact-token evidence stays a match.
	assert.deepEqual(collapsedRoles, ["muted"]);
	const mutedRow = (trulyCollapsed.roles as Record<string, unknown>[]).find((role) => role.role === "muted");
	assert.equal(mutedRow?.partnersPreserved, false);
}

function verifyAnsiBoundary(): void {
	// The compared projection is visible/scrollback cell text plus the
	// normalized, ordered SGR token structure. Raw ANSI bytes are retained as
	// captured evidence only and never enter the projection (issue #800).
	const scenario = scenarios.find((item) => item.id === "boot-72");
	if (!scenario) throw new Error("ANSI boundary verification requires the boot-72 scenario");
	const theme = {
		name: "dark",
		colorCapability: "truecolor",
		colors: Object.fromEntries(semanticThemeRoles.map((role) => [role, "#123456"])),
	};
	const makeCapture = (ansi: string): Record<string, unknown> => ({
		runtime: "evidence",
		scenario: scenario.id,
		width: scenario.width,
		workspace: "/tmp/cpp-harness-pike-differential",
		inputs: [],
		theme,
		snapshots: [{ visible: ["row"], scrollback: [], ansi }],
	});
	const projection = stableProjection(makeCapture("\x1b[2J\x1b[H\x1b[38;2;18;52;86mrow\x1b[0m"), scenario, repoRoot);
	assert.deepEqual(Object.keys((projection.snapshots as Record<string, unknown>[])[0]).sort(), ["scrollback", "sgr", "visible"]);
	// Same cell text and same normalized SGR structure, different raw framing
	// bytes (cursor moves, erase sequences): not a difference.
	const reframed = compare(
		makeCapture("\x1b[2J\x1b[H\x1b[38;2;18;52;86mrow\x1b[0m\x1b[1;1H"),
		makeCapture("\x1b[1;1H\x1b[38;2;18;52;86mrow\x1b[0m"),
		scenario,
		repoRoot,
	);
	assert.equal(reframed.classification, "match");
	// Same cell text with a different normalized SGR structure: a regression.
	const restyled = compare(
		makeCapture("\x1b[2J\x1b[H\x1b[38;2;18;52;86mrow\x1b[0m"),
		makeCapture("\x1b[2J\x1b[H\x1b[38;2;99;99;99mrow\x1b[0m"),
		scenario,
		repoRoot,
	);
	assert.equal(restyled.classification, "supported-capability-regression");
	assert.equal((restyled.projection as Record<string, unknown>).sgrStructureEqual, false);
	assert.equal((restyled.projection as Record<string, unknown>).visibleCellTextEqual, true);
}

function normalizeCapture(capture: Record<string, unknown>, scenario: Scenario, repositoryRoot: string): Record<string, unknown> {
	const workspace = typeof capture.workspace === "string" ? capture.workspace : "/tmp/cpp-harness-pike-differential";
	const snapshots = Array.isArray(capture.snapshots) ? capture.snapshots : [];
	return {
		runtime: capture.runtime,
		scenario: capture.scenario,
		width: capture.width,
		workspace: typeof capture.workspace === "string" ? replaceFixturePaths(capture.workspace, workspace, repositoryRoot, scenario) : undefined,
		inputs: capture.inputs,
		theme: normalizeThemeEvidence(capture.theme, `${String(capture.runtime)}/${scenario.id}`),
		snapshots: snapshots.map((item) => {
			const snapshot = item as Record<string, unknown>;
			const ansi = typeof snapshot.ansi === "string" ? replaceFixturePaths(snapshot.ansi, workspace, repositoryRoot, scenario) : "";
			const visible = Array.isArray(snapshot.visible)
				? snapshot.visible.map((line) => normalizeCellText(String(line), workspace, repositoryRoot, scenario))
				: [];
			return {
				visible,
				scrollback: Array.isArray(snapshot.scrollback)
					? snapshot.scrollback.map((line) => normalizeCellText(String(line), workspace, repositoryRoot, scenario))
					: [],
				ansi,
				sgr: sgrTokens(ansi),
			};
		}),
	};
}

// Diagnostic-only usage projection; the strict cell projection intentionally retains footer differences.
// debt: replace this text projection with typed footer evidence when the product exposes one.
function projectDiagnosticFooterRows(rows: string[], scenario: Pick<Scenario, "id">): string[] {
	if (!usageProfileScenarios.has(scenario.id)) return rows;
	return rows.map((line) =>
		line.replace(/^(\s*)(?:[↑↓]\S+\s+)*\d+(?:\.\d+)?%\/128k \(auto\)/, "$1<footer-stats>"),
	);
}

function stableProjection(capture: Record<string, unknown>, scenario: Scenario, repositoryRoot: string): Record<string, unknown> {
	const normalized = normalizeCapture(capture, scenario, repositoryRoot);
	return {
		runtime: normalized.runtime,
		scenario: normalized.scenario,
		width: normalized.width,
		workspace: normalized.workspace,
		inputs: normalized.inputs,
		snapshots: (normalized.snapshots as Record<string, unknown>[]).map((snapshot) => ({
			visible: snapshot.visible,
			scrollback: snapshot.scrollback,
			sgr: (snapshot.sgr as string[]).map(normalizeSgrToken),
		})),
	};
}

function projectProfileCellRows(rows: string[]): { rows: string[]; removed: Record<string, number> } {
	const removed: Record<string, number> = {};
	for (const projection of profileCellRowProjections) removed[projection.name] = 0;
	const retained = rows.flatMap((line) => {
		const projection = profileCellRowProjections.find((candidate) => candidate.pattern.test(line));
		if (projection !== undefined) {
			removed[projection.name] += 1;
			return [];
		}
		return line.trim() === "" ? [] : [line];
	});
	return { rows: retained, removed };
}

function compareProfileProjection(pike: Record<string, unknown>, pi: Record<string, unknown>, scenario: Pick<Scenario, "id">): Record<string, unknown> {
	const pikeSnapshots = pike.snapshots as Record<string, unknown>[];
	const piSnapshots = pi.snapshots as Record<string, unknown>[];
	const pikeVisible = pikeSnapshots.map((snapshot) =>
		projectProfileCellRows(projectDiagnosticFooterRows(snapshot.visible as string[], scenario)));
	const piVisible = piSnapshots.map((snapshot) =>
		projectProfileCellRows(projectDiagnosticFooterRows(snapshot.visible as string[], scenario)));
	const pikeScrollback = pikeSnapshots.map((snapshot) =>
		projectProfileCellRows(projectDiagnosticFooterRows(snapshot.scrollback as string[], scenario)));
	const piScrollback = piSnapshots.map((snapshot) =>
		projectProfileCellRows(projectDiagnosticFooterRows(snapshot.scrollback as string[], scenario)));
	return {
		visibleCellTextEqual: JSON.stringify(pikeVisible.map((item) => item.rows)) === JSON.stringify(piVisible.map((item) => item.rows)),
		scrollbackStructureEqual: JSON.stringify(pikeScrollback.map((item) => item.rows)) === JSON.stringify(piScrollback.map((item) => item.rows)),
		footerStatsProjected: usageProfileScenarios.has(String(pike.scenario)),
		removedCellRows: {
			pike: pikeVisible.map((item) => item.removed),
			pi: piVisible.map((item) => item.removed),
		},
		role: "diagnostic-only; strict projection remains the classification authority",
	};
}

function omitProjectionLines(projection: Record<string, unknown>, patterns: string[]): Record<string, unknown> {
	if (patterns.length === 0) return projection;
	const snapshots = projection.snapshots as Record<string, unknown>[];
	return {
		...projection,
		snapshots: snapshots.map((snapshot) => ({
			visible: (snapshot.visible as string[]).filter((line) => !patterns.some((pattern) => line.includes(pattern))),
			scrollback: (snapshot.scrollback as string[]).filter((line) => !patterns.some((pattern) => line.includes(pattern))),
			sgr: snapshot.sgr,
		})),
	};
}

function omissionTextEqual(left: Record<string, unknown>, right: Record<string, unknown>): boolean {
	const leftSnapshots = left.snapshots as Record<string, unknown>[];
	const rightSnapshots = right.snapshots as Record<string, unknown>[];
	const text = (snapshot: Record<string, unknown>) => ({
		visible: snapshot.visible,
		scrollback: snapshot.scrollback,
	});
	return JSON.stringify(leftSnapshots.map(text)) === JSON.stringify(rightSnapshots.map(text));
}

function omissionSgrEqual(left: Record<string, unknown>, right: Record<string, unknown>): boolean {
	const leftSnapshots = left.snapshots as Record<string, unknown>[];
	const rightSnapshots = right.snapshots as Record<string, unknown>[];
	return JSON.stringify(leftSnapshots.map((snapshot) => snapshot.sgr)) === JSON.stringify(rightSnapshots.map((snapshot) => snapshot.sgr));
}

function isIntentionalSubsetOmission(scenario: Pick<Scenario, "omissions">, omissionProjectionEqual: boolean, pikeObserved: boolean, piObserved: boolean): boolean {
	return scenario.omissions.length > 0 && piObserved && !pikeObserved && omissionProjectionEqual;
}

function verifyProjectionPolicy(): void {
	const scenario = { omissions: ["pi-only chrome"] };
	assert.equal(isIntentionalSubsetOmission(scenario, true, false, true), true);
	assert.equal(isIntentionalSubsetOmission(scenario, false, false, true), false);
	assert.equal(isIntentionalSubsetOmission(scenario, true, true, true), false);
	assert.equal(isIntentionalSubsetOmission(scenario, true, false, false), false);

	const left = {
		snapshots: [{ visible: ["same", "pi-only chrome"], scrollback: [], sgr: ["\x1b[31m"] }],
	};
	const right = {
		snapshots: [{ visible: ["same", "pi-only chrome"], scrollback: [], sgr: ["\x1b[31m"] }],
	};
	const filteredLeft = omitProjectionLines(left, ["pi-only"]);
	const filteredRight = omitProjectionLines(right, ["pi-only"]);
	assert.equal(omissionTextEqual(filteredLeft, filteredRight), true);
	assert.equal(omissionSgrEqual(filteredLeft, filteredRight), true);
	assert.equal(omissionSgrEqual(filteredLeft, { snapshots: [{ ...(filteredRight.snapshots[0] as Record<string, unknown>), sgr: ["\x1b[32m"] }] }), false);

	const profile = projectProfileCellRows([" pi v0.87.1", "Pi can explain its own features and look up its docs. Ask it how to", "use or extend Pi.", "kept"]);
	assert.deepEqual(profile.rows, ["kept"]);
	assert.deepEqual(profile.removed, { "runtime-identity": 1, "startup-documentation": 2 });
	assert.equal(normalizeCellText("/repository (main)", "/tmp/workspace", "/repository"), "<deterministic-workspace>");
	assert.equal(replaceFixturePaths("/repository (main) suffix", "/tmp/workspace", "/repository"), "<repository-root> (<branch>) suffix");
	assert.deepEqual(projectDiagnosticFooterRows(["0.0%/128k (auto) <model>", "kept"], { id: "user-message" }), ["<footer-stats> <model>", "kept"]);
	assert.deepEqual(projectDiagnosticFooterRows(["0.0%/128k (auto) <model>"], { id: "boot-72" }), ["0.0%/128k (auto) <model>"]);
}

function compare(pike: Record<string, unknown>, pi: Record<string, unknown>, scenario: Scenario, repositoryRoot: string): Record<string, unknown> {
	const pikeProjection = stableProjection(pike, scenario, repositoryRoot);
	const piProjection = stableProjection(pi, scenario, repositoryRoot);
	const pikeSnapshots = pikeProjection.snapshots as Record<string, unknown>[];
	const piSnapshots = piProjection.snapshots as Record<string, unknown>[];
	const workspaceEqual = pikeProjection.workspace === piProjection.workspace;
	const visibleEqual = JSON.stringify(pikeSnapshots.map((item) => item.visible)) === JSON.stringify(piSnapshots.map((item) => item.visible));
	const scrollbackEqual = JSON.stringify(pikeSnapshots.map((item) => item.scrollback)) === JSON.stringify(piSnapshots.map((item) => item.scrollback));
	const sgrEqual = JSON.stringify(pikeSnapshots.map((item) => item.sgr)) === JSON.stringify(piSnapshots.map((item) => item.sgr));
	const equal = workspaceEqual && visibleEqual && scrollbackEqual && sgrEqual;
	const omissionProjection = omitProjectionLines(pikeProjection, scenario.omissionPatterns ?? []);
	const piOmissionProjection = omitProjectionLines(piProjection, scenario.omissionPatterns ?? []);
	const omissionText = omissionTextEqual(omissionProjection, piOmissionProjection);
	const omissionSgr = omissionSgrEqual(omissionProjection, piOmissionProjection);
	const omissionProjectionEqual = omissionText && omissionSgr;
	const pikeText = JSON.stringify(pikeProjection.snapshots);
	const piText = JSON.stringify(piProjection.snapshots);
	const pikeObserved = (scenario.omissionPatterns ?? []).some((pattern) => pikeText.includes(pattern));
	const piObserved = (scenario.omissionPatterns ?? []).some((pattern) => piText.includes(pattern));
	const omissionOnly = isIntentionalSubsetOmission(scenario, omissionProjectionEqual, pikeObserved, piObserved);
	return {
		projection: {
			workspaceProfileEqual: workspaceEqual,
			visibleCellTextEqual: visibleEqual,
			scrollbackStructureEqual: scrollbackEqual,
			sgrStructureEqual: sgrEqual,
			omissionTextEqual: omissionText,
			omissionSgrStructureEqual: omissionSgr,
			omissionProjectionEqual,
		},
		profileProjection: compareProfileProjection(pikeProjection, piProjection, scenario),
		classification: equal ? "match" : omissionOnly ? "intentional-subset-omission" : "supported-capability-regression",
		omissions: scenario.omissions,
		omissionEvidence: {
			patterns: scenario.omissionPatterns ?? [],
			pikeObserved,
			piObserved,
			status: omissionOnly ? "intentional-subset-omission" : piObserved && !pikeObserved ? "requires-review" : "not-observed",
		},
	};
}

function digest(value: unknown): string {
	return createHash("sha256").update(JSON.stringify(value)).digest("hex");
}

function runLiveManual(): number {
	const outputDirectory = process.env.CCH_DIFFERENTIAL_LIVE_OUTPUT_DIR ?? "/tmp/cpp-harness-pike-differential-live";
	mkdirSync(outputDirectory, { recursive: true });
	const pikeBinary = process.env.CCH_DIFFERENTIAL_LIVE_PIKE ?? path.join(repositoryRootFromEnvironment(), "build/pike");
	const piCheckoutPath = frozenCheckoutOrSkip();
	const quote = (part: string) => `'${part.replaceAll("'", "'\\''")}'`;
	const piCommand = [path.join(piCheckoutPath, "node_modules/.bin/tsx"), path.join(piCheckoutPath, "packages/coding-agent/src/cli.ts"), ...(process.env.CCH_DIFFERENTIAL_LIVE_PI_ARGS?.split(" ") ?? [])].map(quote).join(" ");
	const commands = [
		{ name: "pike", command: `${quote(pikeBinary)} ${process.env.CCH_DIFFERENTIAL_LIVE_PIKE_ARGS ?? "--no-session --no-skills --no-prompt-templates --no-approve"}` },
		{ name: "pi", command: piCommand },
	];
	for (const scenario of scenarios) {
		for (const item of commands) {
			const resize = scenario.resize === undefined ? "" : (() => {
				const dimensions = parseResizeSequence(scenario.resize as string).at(-1)!;
				return `stty cols ${dimensions.columns} rows ${dimensions.rows}; `;
			})();
			const result = spawnSync("script", ["-qfec", `${resize}${item.command}`, "/dev/null"], {
				cwd: repositoryRootFromEnvironment(),
				env: { ...process.env, TERM: "xterm-256color", COLORTERM: "truecolor" },
				input: `${scenario.inputs.join("")}\x04`,
				encoding: "buffer",
				timeout: 30_000,
			});
			if (result.error) throw result.error;
			writeFileSync(path.join(outputDirectory, `${item.name}-${scenario.id}.raw`), result.stdout);
			console.log(`${item.name}/${scenario.id}: captured ${result.stdout.length} bytes in ${outputDirectory}`);
		}
	}
	console.log("Manual captures are raw local evidence; review and sanitize them before sharing.");
	return 0;
}

function reportMetadata(report: Record<string, unknown>): Record<string, unknown> {
	return {
		schema: report.schema,
		baseline: report.baseline,
		profile: report.profile,
		structuralProjection: report.structuralProjection,
		themeParity: report.themeParity,
	};
}

function requireStringArray(value: unknown, label: string): asserts value is string[] {
	if (!Array.isArray(value) || value.some((item) => typeof item !== "string")) throw new Error(`${label} must be an array of strings`);
}

function validateReportCaptures(report: Record<string, unknown>): void {
	if (!Array.isArray(report.scenarios)) throw new Error("differential report scenarios must be an array");
	for (const item of report.scenarios as Record<string, unknown>[]) {
		requireStringArray(item.inputs, `differential report inputs for ${String(item.id)}`);
		const captures = item.captures as Record<string, unknown> | undefined;
		if (!captures || typeof captures !== "object") throw new Error(`differential report is missing captures for ${String(item.id)}`);
		for (const runtime of ["pike", "pi"]) {
			const capture = captures[runtime] as Record<string, unknown> | undefined;
			if (!capture || typeof capture !== "object" || !Array.isArray(capture.snapshots) || capture.snapshots.length === 0) {
				throw new Error(`differential report is missing ${runtime} snapshots for ${String(item.id)}`);
			}
			if (capture.runtime !== runtime || capture.scenario !== item.id || capture.width !== item.width || JSON.stringify(capture.inputs) !== JSON.stringify(item.inputs)) {
				throw new Error(`differential report has mismatched ${runtime} capture identity for ${String(item.id)}`);
			}
			normalizeThemeEvidence(capture.theme, `checked-in ${runtime}/${String(item.id)}`);
			for (const snapshot of capture.snapshots as Record<string, unknown>[]) {
				requireStringArray(snapshot.visible, `differential report visible cells for ${runtime}/${String(item.id)}`);
				requireStringArray(snapshot.scrollback, `differential report scrollback for ${runtime}/${String(item.id)}`);
				requireStringArray(snapshot.sgr, `differential report SGR tokens for ${runtime}/${String(item.id)}`);
				if (typeof snapshot.ansi !== "string") throw new Error(`differential report is missing ANSI for ${runtime}/${String(item.id)}`);
				if (JSON.stringify(snapshot.sgr) !== JSON.stringify(sgrTokens(snapshot.ansi))) {
					throw new Error(`differential report SGR tokens do not match ANSI for ${runtime}/${String(item.id)}`);
				}
			}
		}
	}
}

function validateThemeParity(report: Record<string, unknown>): void {
	const parity = report.themeParity as Record<string, unknown> | undefined;
	// Semantic role mapping is the authority: exact-token match and a
	// partition-preserving palette difference both satisfy the contract; only a
	// broken role mapping is a Supported Capability regression (issue #797).
	if (!parity || !acceptableThemeParityClassifications.has(String(parity.classification))) {
		throw new Error("canonical Native TUI theme parity is not a semantic-role match");
	}
	if (!Array.isArray(parity.roles) || parity.roles.length !== semanticThemeRoles.length) {
		throw new Error("canonical theme parity must record every semantic role");
	}
	for (const role of parity.roles as Record<string, unknown>[]) {
		if (!semanticThemeRoles.includes(String(role.role)) || !acceptableThemeRoleClassifications.has(String(role.classification))) {
			throw new Error(`canonical theme role ${String(role.role)} breaks the semantic role mapping`);
		}
		if (typeof role.pike !== "string" || typeof role.pi !== "string" || typeof role.exactTokenMatch !== "boolean" || typeof role.partnersPreserved !== "boolean") {
			throw new Error(`canonical theme role ${String(role.role)} is missing exact-token evidence`);
		}
	}
	if (!Array.isArray(parity.supportedCapabilityMismatches) || parity.supportedCapabilityMismatches.length !== 0) {
		throw new Error("canonical theme parity has an unclassified Supported Capability mismatch");
	}
	if (!Array.isArray(parity.paletteDifferences)) {
		throw new Error("canonical theme parity must record palette differences as evidence");
	}
	if (!Array.isArray(parity.deferredTokenPolicy) || parity.deferredTokenPolicy.length === 0) {
		throw new Error("canonical theme parity must record deferred token policy");
	}
}

function validateStableDigests(report: Record<string, unknown>, repositoryRoot: string): void {
	const byId = new Map(scenarios.map((scenario) => [scenario.id, scenario]));
	for (const item of report.scenarios as Record<string, unknown>[]) {
		const scenario = byId.get(String(item.id));
		if (!scenario) throw new Error(`differential report contains unknown scenario ${String(item.id)}`);
		const captures = item.captures as Record<string, Record<string, unknown>>;
		const actualDigest = digest({
			pike: stableProjection(captures.pike, scenario, repositoryRoot),
			pi: stableProjection(captures.pi, scenario, repositoryRoot),
		});
		if (item.stableDigest !== actualDigest) throw new Error(`differential report stable digest is stale for ${scenario.id}`);
	}
}

async function runParent(): Promise<number> {
	const checkout = frozenCheckoutOrSkip();
	const repositoryRoot = repositoryRootFromEnvironment();
	const temporaryDirectory = path.join(differentialDir, `.verify-${process.pid}`);
	rmSync(temporaryDirectory, { recursive: true, force: true });
	mkdirSync(temporaryDirectory, { recursive: true });
	const report: Record<string, unknown> = {
		schema: 2,
		baseline: {
			piCommit: frozenCommit,
			artifact: `${packageName}@${packageVersion}`,
		},
		profile,
		structuralProjection: {
			cellText: "trimmed visible cells with fixture paths, model ids, model prose, and the deterministic workspace/branch projection applied",
			ansi: "raw ANSI retained as captured evidence only; verification compares cell text plus the normalized ordered SGR token structure, never raw ANSI byte parity",
			sgr: "truecolor RGB values are preserved; only colon/semicolon SGR encoding is canonicalized, so palette or style differences remain visible",
			screenshot: "themeParity.renderedScreenshots retains the rendered terminal cell rows as text screenshots",
			scrollback: "scrollback cell rows are compared separately from the visible viewport",
			profile: "profileProjection is diagnostic-only: it removes exact pi runtime-identity/startup-documentation rows and projects footer usage only for user-message, status-footer, and tool-result; strict classification never uses this projection",
			omission: "omissionTextEqual covers visible/scrollback cell text after declared pi-only lines are removed; omissionSgrStructureEqual and omissionProjectionEqual keep omitted SGR visible, so styled omissions remain requires-review until cell-style projection exists",
			verification: "--verify checks reproducible report digests and stable classifications; it does not assert that every scenario is a parity match",
		},
		scenarios: [] as Record<string, unknown>[],
	};
	try {
		for (const scenario of scenarios) {
			const pikeOutput = path.join(temporaryDirectory, `${scenario.id}.pike.json`);
			const piOutput = path.join(temporaryDirectory, `${scenario.id}.pi.json`);
			const pike = runPike(scenario, pikeOutput);
			const pi = await runPi(scenario, piOutput);
			const comparison = compare(pike, pi, scenario, repositoryRoot);
			const pikeCapture = normalizeCapture(pike, scenario, repositoryRoot);
			const piCapture = normalizeCapture(pi, scenario, repositoryRoot);
			(report.scenarios as Record<string, unknown>[]).push({
				id: scenario.id,
				width: scenario.width,
				inputs: scenario.inputs,
				resize: scenario.resize,
				...comparison,
				captures: { pike: pikeCapture, pi: piCapture },
				stableDigest: digest({
					pike: stableProjection(pikeCapture, scenario, repositoryRoot),
					pi: stableProjection(piCapture, scenario, repositoryRoot),
				}),
			});
			console.log(`${scenario.id}: ${comparison.classification}`);
		}
	} finally {
		rmSync(temporaryDirectory, { recursive: true, force: true });
		void checkout;
	}
	const firstScenario = (report.scenarios as Record<string, unknown>[])[0];
	const firstCaptures = firstScenario.captures as Record<string, Record<string, unknown>>;
	const themeParity = themeRoleComparison(
		normalizeThemeEvidence(firstCaptures.pike.theme, "Pike"),
		normalizeThemeEvidence(firstCaptures.pi.theme, "pi"),
	);
	themeParity.renderedScreenshots = (report.scenarios as Record<string, unknown>[])
		.filter((item) => typeof item.id === "string" && item.id.startsWith("boot-"))
		.map((item) => {
			const captures = item.captures as Record<string, Record<string, unknown>>;
			const pikeSnapshots = captures.pike.snapshots as Record<string, unknown>[];
			const piSnapshots = captures.pi.snapshots as Record<string, unknown>[];
			return {
				id: item.id,
				width: item.width,
				pike: pikeSnapshots[pikeSnapshots.length - 1]?.visible,
				pi: piSnapshots[piSnapshots.length - 1]?.visible,
			};
		});
	report.themeParity = themeParity;
	if (process.argv.includes("--write")) {
		mkdirSync(differentialDir, { recursive: true });
		writeJson(reportPath, report, true);
		console.log(`wrote ${reportPath}`);
		return 0;
	}
	if (!existsSync(reportPath)) throw new Error(`checked-in report is missing: ${reportPath}; run with --write`);
	const expected = JSON.parse(readFileSync(reportPath, "utf8")) as Record<string, unknown>;
	if (JSON.stringify(reportMetadata(report)) !== JSON.stringify(reportMetadata(expected))) {
		throw new Error("Native TUI differential report metadata changed; inspect the checked-in evidence.");
	}
	validateReportCaptures(report);
	validateReportCaptures(expected);
	validateThemeParity(report);
	validateThemeParity(expected);
	validateStableDigests(expected, repositoryRoot);
	const actualStable = (report.scenarios as Record<string, unknown>[]).map((item) => ({
		id: item.id,
		width: item.width,
		inputs: item.inputs,
		classification: item.classification,
		projection: item.projection,
		profileProjection: item.profileProjection,
		omissionEvidence: item.omissionEvidence,
		stableDigest: item.stableDigest,
	}));
	const expectedStable = (expected.scenarios as Record<string, unknown>[]).map((item) => ({
		id: item.id,
		width: item.width,
		inputs: item.inputs,
		classification: item.classification,
		projection: item.projection,
		profileProjection: item.profileProjection,
		omissionEvidence: item.omissionEvidence,
		stableDigest: item.stableDigest,
	}));
	if (JSON.stringify(actualStable) !== JSON.stringify(expectedStable)) {
		console.error("Native TUI differential projection changed; inspect the report and classify the delta.");
		console.error("Use --write only after reviewing Supported Capability regressions versus intentional omissions.");
		return 1;
	}
	console.log(`verified ${actualStable.length} Native TUI differential report digests against ${reportPath}; classifications remain unchanged`);
	return 0;
}

async function runPiChild(): Promise<number> {
	const scenario = scenarios.find((item) => item.id === process.env.CCH_DIFFERENTIAL_SCENARIO);
	const output = process.env.CCH_DIFFERENTIAL_OUTPUT;
	if (!scenario || !output) throw new Error("pi child requires CCH_DIFFERENTIAL_SCENARIO and CCH_DIFFERENTIAL_OUTPUT");
	const checkout = piCheckout();
	const source = (relative: string) => pathToFileURL(path.join(checkout, relative)).href;
	const { createHarness } = await import(source("packages/coding-agent/test/suite/harness.ts"));
	const { fauxAssistantMessage, fauxText, fauxToolCall } = await import(source("packages/ai/src/providers/faux.ts"));
	const { AgentSessionRuntime } = await import(source("packages/coding-agent/src/core/agent-session-runtime.ts"));
	const { InteractiveMode } = await import(source("packages/coding-agent/src/modes/interactive/interactive-mode.ts"));
	const { getResolvedThemeColors } = await import(source("packages/coding-agent/src/modes/interactive/theme/theme.ts"));
	const { VirtualTerminal } = await import(source("packages/tui/test/virtual-terminal.ts"));

	const workspace = process.env.CCH_DIFFERENTIAL_WORKSPACE;
	if (workspace === undefined || workspace === "") throw new Error("pi child requires CCH_DIFFERENTIAL_WORKSPACE");
	mkdirSync(workspace, { recursive: true });
	mkdirSync(path.join(workspace, ".pi"), { recursive: true });
	writeFileSync(path.join(workspace, "notes.txt"), "alpha\n");
	// pi's in-memory SessionManager defaults its cwd to process.cwd(). Set the
	// profile workspace before constructing the harness so the footer, tools,
	// and captured identity use the same workspace as Pike.
	process.chdir(workspace);

	const harness = await createHarness({
		models: differentialModels,
		settings: { theme: "dark" },
	});
	// AgentSession's pi tool context is rooted in the harness temp directory,
	// while the captured/profile workspace is the parent scenario directory.
	// Keep the same deterministic fixture content at both fixture roots.
	writeFileSync(path.join(harness.tempDir, "notes.txt"), "alpha\n");
	const response = (text: string) => fauxAssistantMessage(text);
	if (scenario.id === "user-message") harness.setResponses([response("deterministic assistant reply")]);
	if (scenario.id === "status-footer") harness.setResponses([response("deterministic status reply")]);
	if (scenario.id === "tool-result") {
		harness.setResponses([
			fauxAssistantMessage([fauxText("I will read the deterministic fixture."), fauxToolCall("read", { path: "notes.txt" })], { stopReason: "toolUse" }),
			response("deterministic tool answer"),
		]);
	}
	if (scenario.id === "scrollback") {
		harness.setResponses(Array.from({ length: 6 }, (_, index) => response(`deterministic scrollback reply ${index + 1}`)));
	}

	class RecordingTerminal extends VirtualTerminal {
		ansi = "";

		override write(data: string): void {
			this.ansi += data;
			super.write(data);
		}
	}

	const terminal = new RecordingTerminal(scenario.width, profile.viewportRows);
	const agentDirectory = process.env.PI_CODING_AGENT_DIR ?? `/tmp/cpp-harness-pike-differential-${scenario.id}`;
	mkdirSync(agentDirectory, { recursive: true });
	const services = {
		cwd: workspace,
		agentDir: agentDirectory,
		modelRuntime: harness.session.modelRuntime,
		settingsManager: harness.settingsManager,
		resourceLoader: harness.session.resourceLoader,
		diagnostics: [],
	};
	const runtimeHost = new AgentSessionRuntime(harness.session, services, async () => {
		throw new Error("session replacement is not used by the differential capture");
	});
	const mode = new InteractiveMode(runtimeHost, {
		terminal,
		tuiMode: "regular",
		initialThemeSetting: "dark",
	});
	void mode.run().catch((error: unknown) => {
		console.error(error);
		process.exitCode = 1;
	});
	await waitForTerminalText(terminal, "Press ctrl+o");
	let ansiOffset = 0;
	const snapshots: Record<string, unknown>[] = [await capture(terminal, workspace, ansiOffset)];
	ansiOffset = terminal.ansi.length;
	if (scenario.resize !== undefined) {
		for (const dimensions of parseResizeSequence(scenario.resize)) {
			const resizeOffset = terminal.ansi.length;
			terminal.resize(dimensions.columns, dimensions.rows);
			await waitForTerminalAnsi(terminal, resizeOffset);
			snapshots.push(await capture(terminal, workspace, ansiOffset));
			ansiOffset = terminal.ansi.length;
		}
	}
	let responseIndex = 0;
	for (const input of scenario.inputs) {
		const inputOffset = terminal.ansi.length;
		await sendInputSequence(terminal, input);
		if (scenario.id === "tool-result" && input.endsWith("\r")) {
			await waitForTerminalText(terminal, "I will read the deterministic fixture.");
			snapshots.push(await capture(terminal, workspace, ansiOffset));
			ansiOffset = terminal.ansi.length;
			await waitForEventText(harness.events, "alpha");
			await waitForTerminalText(terminal, "deterministic tool answer");
			if (terminal.getViewport().join("\n").includes("ENOENT")) {
				throw new Error("pi differential tool fixture was not readable from the AgentSession cwd");
			}
		}
		if (input.endsWith("\r") && ["user-message", "tool-result", "status-footer", "scrollback"].includes(scenario.id)) {
			const expected = scenario.id === "user-message"
				? "deterministic assistant reply"
				: scenario.id === "status-footer"
					? "deterministic status reply"
					: scenario.id === "tool-result"
						? "deterministic tool answer"
						: `deterministic scrollback reply ${responseIndex + 1}`;
			await waitForTerminalText(terminal, expected);
			responseIndex += 1;
		} else if (scenario.id === "settings-selector" && input === "\x1b") {
			await waitForTerminalTextAbsent(terminal, "Type to search");
		} else {
			await waitForTerminalAnsi(terminal, inputOffset);
		}
		snapshots.push(await capture(terminal, workspace, ansiOffset));
		ansiOffset = terminal.ansi.length;
	}
	const report = {
		runtime: "pi",
		scenario: scenario.id,
		width: scenario.width,
		workspace,
		inputs: scenario.inputs,
		theme: {
			name: profile.theme.name,
			colorCapability: profile.theme.colorCapability,
			colors: getResolvedThemeColors(),
		},
		snapshots,
	};
	writeJson(output, report);
	harness.cleanup();
	await delay(10);
	// The Native TUI owns interval-driven render work. The child has already
	// written its complete capture, so terminate this isolated evidence
	// process instead of allowing the live renderer to keep the event loop
	// alive after the comparison boundary.
	process.exit(0);
}

async function capture(terminal: { flush(): Promise<void>; getViewport(): string[]; getScrollBuffer(): string[]; ansi: string }, workspace: string, ansiOffset: number): Promise<Record<string, unknown>> {
	await terminal.flush();
	const visible = terminal.getViewport();
	const buffer = terminal.getScrollBuffer();
	return {
		visible,
		scrollback: buffer.slice(0, Math.max(0, buffer.length - visible.length)),
		ansi: terminal.ansi.slice(ansiOffset),
		workspace,
	};
}

async function sendInputSequence(terminal: { sendInput(data: string): void }, input: string): Promise<void> {
	let remaining = input;
	while (remaining.length > 0) {
		const enter = remaining.indexOf("\r");
		if (enter < 0) {
			terminal.sendInput(remaining);
			return;
		}
		if (enter > 0) terminal.sendInput(remaining.slice(0, enter));
		terminal.sendInput("\r");
		remaining = remaining.slice(enter + 1);
	}
}

async function waitForTerminalText(terminal: { flush(): Promise<void>; getViewport(): string[] }, expected: string): Promise<void> {
	const deadline = Date.now() + 3_000;
	while (Date.now() < deadline) {
		await terminal.flush();
		if (terminal.getViewport().join("\n").includes(expected)) return;
		await delay(20);
	}
	throw new Error(`pi differential scenario did not render expected text: ${expected}\n${terminal.getViewport().join("\n")}`);
}

async function waitForTerminalTextAbsent(terminal: { flush(): Promise<void>; getViewport(): string[] }, unexpected: string): Promise<void> {
	const deadline = Date.now() + 3_000;
	while (Date.now() < deadline) {
		await terminal.flush();
		if (!terminal.getViewport().join("\n").includes(unexpected)) return;
		await delay(20);
	}
	throw new Error(`pi differential scenario retained expected-to-leave text: ${unexpected}`);
}

async function waitForEventText(events: readonly unknown[], expected: string): Promise<void> {
	const deadline = Date.now() + 3_000;
	while (Date.now() < deadline) {
		if (events.some((event) => JSON.stringify(event).includes(expected))) return;
		await delay(20);
	}
	throw new Error(`pi differential session events did not contain expected text: ${expected}`);
}

async function waitForTerminalAnsi(terminal: { flush(): Promise<void>; ansi: string }, offset: number): Promise<void> {
	const deadline = Date.now() + 3_000;
	while (Date.now() < deadline) {
		await terminal.flush();
		if (terminal.ansi.length > offset) return;
		await delay(20);
	}
	throw new Error(`pi differential scenario did not render after input at ANSI offset ${offset}`);
}

function delay(milliseconds: number): Promise<void> {
	return new Promise((resolve) => setTimeout(resolve, milliseconds));
}

async function main(): Promise<number> {
	try {
		if (process.argv.includes("--runtime")) return await runPiChild();
		if (process.argv.includes("--live")) return runLiveManual();
		verifySgrNormalization();
		verifyProjectionPolicy();
		verifyThemeParityPolicy();
		verifyAnsiBoundary();
		return await runParent();
	} catch (error) {
		if (error instanceof SkipError) {
			console.log(`SKIP: ${error.message}`);
			return 77;
		}
		console.error(error);
		if (process.argv.includes("--runtime")) process.exit(1);
		return 1;
	}
}

process.exitCode = await main();
