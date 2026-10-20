#!/usr/bin/env tsx
/**
 * Deterministic dual-runtime Native TUI evidence harness (issue #800).
 *
 * The default mode runs the selected pi checkout and the rebuilt Pike test
 * binary through the same scenario/input records, then writes a sanitized
 * structural report. `--verify` is the CTest mode: it regenerates the
 * projections and compares them with the checked-in report. The compared
 * boundary is resolved visible/scrollback cell text plus styled-cell
 * presentation; raw ANSI and ordered SGR tokens are retained as diagnostic
 * evidence only, so the harness makes no full raw-ANSI or SGR-cadence parity
 * claim. Theme parity compares the canonical semantic role mapping (exact RGB
 * is retained as evidence); a palette difference that preserves the role
 * partition is not a regression (issue #797). The styled-cell presentation
 * comparison resolves each cell color to the same semantic role on the
 * runtime that emitted it, so the palette and the cells stay on one contract:
 * only an exact-RGB difference inside the same role compares equal.
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
import { registerHooks } from "node:module";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const scriptPath = fileURLToPath(import.meta.url);
const scriptDir = path.dirname(scriptPath);
const fixtureDir = path.resolve(scriptDir, "..");
const repoRoot = path.resolve(fixtureDir, "../..");
const differentialDir = path.join(fixtureDir, "differential");
// The checked-in report is evidence about ONE baseline (ADR 0053 keeps pi as
// reference material, not a specification), so recording against a newer pi
// must not overwrite the report an older one describes.
// `CCH_DIFFERENTIAL_REPORT` writes to a separate path and leaves
// `report.json` untouched; unset keeps the historical single-report
// behaviour, which is what the CTest adapter's `--verify` mode compares.
const reportPath = process.env.CCH_DIFFERENTIAL_REPORT
	? path.resolve(process.env.CCH_DIFFERENTIAL_REPORT)
	: path.join(differentialDir, "report.json");
// ADR 0053 makes pi optional reference material rather than a specification,
// so the comparison target is the pi checkout this machine actually has
// instead of a hardcoded revision. The resolved commit and version are still
// recorded in the report as provenance, and `PI_BASELINE_COMMIT` pins an exact
// revision for anyone who needs to reproduce a specific comparison.
const packageName = "@earendil-works/pi-coding-agent";
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
		FORCE_COLOR: "3",
		NO_COLOR: undefined,
	},
	configuration: {
		credentials: "in-memory faux provider",
		extensions: "disabled",
		userSettings: "in-memory dark theme",
		projectResources: "empty deterministic .pi directory",
		generatedAzureCatalog: "empty JSON import stub; faux scenarios do not consume provider catalog data",
	},
	provider: "deterministic-faux",
});

type TriageClassification =
	| "match"
	| "product-defect"
	| "intentional-subset-omission"
	| "environment-configuration"
	| "harness-projection-defect"
	| "intentional-divergence"
	| "capture-timing-artifact"
	| "pi-only-deferred-capability";

type Triage = {
	classification: TriageClassification;
	evidence: string[];
	rationale: string;
	acceptedDifferences?: string[];
};

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
	{
		id: "settings-selector",
		width: 100,
		inputs: ["/settings", "\r", "Show hardware cursor", "\r", "\x15", "Terminal progress", "\r", "\x15", "\x1b"],
		omissions: ["pi-only settings outside the Supported Capability subset are not projected as Pike capabilities"],
		omissionPatterns: [
			"Auto-compact",
			"Auto-resize images",
			"Block images",
			"Editor padding",
			"Autocomplete max items",
			"Clear on shrink",
			"Automatically compact context when it gets too large",
			"extension",
			"Extension",
			"package",
		],
	},
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

// Versioned expectation adaptations bridge the checked-in historical evidence
// to the current product behaviour without editing the report (ADR 0053 keeps
// one report as evidence about one baseline, and the report bytes, schema,
// baseline, and recorded digests stay untouched). Each adaptation is narrow by
// construction: it names the scenario, runtime, snapshot, the exact historical
// row text it replaces, the exact current row text and style that replace it,
// the retired literals the current capture must not resurrect, and the
// authority that approved the change. Anything outside an adaptation still
// compares exactly, and an adaptation whose historical anchor or current
// requirement is not met fails closed instead of drifting open.
type VersionedExpectationAdaptation = {
	runtime: "pike" | "pi";
	snapshot: number;
	historicalText: string;
	currentText: string;
	// The canonical style the notice row must carry: ThemeToken::Warning is
	// this exact truecolor value in the canonical dark profile, and every
	// flag slot stays false.
	requiredForeground: string;
	retiredLiterals: string[];
	authority: string;
};

const expectationAdaptationVersion = 1;

const versionedExpectationAdaptations: Record<string, readonly VersionedExpectationAdaptation[]> = Object.freeze({
	"model-selector": Object.freeze([{
		runtime: "pike",
		snapshot: 1,
		historicalText: "Only showing models from configured providers. Use /login to add providers.",
		currentText: "Only showing models from the local Magpie gateway.",
		requiredForeground: "#cd9a22",
		retiredLiterals: ["/login", "configured providers"],
		authority: "ADR 0071 / ADR 0075",
	} satisfies VersionedExpectationAdaptation]),
});

const triageByScenario: Record<string, Triage> = Object.fromEntries([
	["boot-72", { classification: "product-defect", evidence: ["report.json:boot-72"], rationale: "At 72 columns the shared startup help wraps at a different segment boundary: pi keeps `! bash` together and starts the next row with `· ctrl+o more`, while Pike leaves the separator at the end of the first row. The difference remains after pi's runtime banner and onboarding copy are projected out." }],
	["boot-100", { classification: "match", evidence: ["report.json:boot-100"], rationale: "The profile-stripped shared regular Native TUI header matches pi at 100 columns." }],
	["boot-120", { classification: "match", evidence: ["report.json:boot-120"], rationale: "The profile-stripped shared regular Native TUI header matches pi at 120 columns." }],
	["boot-41", { classification: "match", evidence: ["report.json:boot-41"], rationale: "The profile-stripped shared regular Native TUI header matches pi at the narrow width." }],
	["model-selector", { classification: "match", evidence: ["#808", "report.json:model-selector"], rationale: "The supported model row marker and order now match pi: the row leads with pi's marker cell instead of a trailing success-styled check, the provider hint renders unpadded, and the save hint uses pi's dim role." }],
	["settings-selector", {
		classification: "product-defect",
		evidence: ["#809", "report.json:settings-selector"],
		rationale: "Refreshed against pi v1.0.4. Show hardware cursor and Terminal progress are no longer omission patterns: the scenario searches both rows and captures their false and true states, whose rendered labels, values and styles match pi. Editor padding and the other out-of-scope pi-only rows stay explicit omissions. The residual product-defect delta is the retained-omission viewport consequence: pi's first viewport carries four of its pi-only rows, its row counter reads (1/32) against Pike's (1/13), and each side's description line follows its own selected row, so the omission projection cannot equate the two frames. No supported capability is unverified by this row.",
		acceptedDifferences: [
			"#809 accepts Pike's registry-derived, effective-binding settings help wording. The raw help cells and styles remain in the capture and comparison; this wording is not filtered as an omission.",
		],
	}],
	["thinking-selector", { classification: "product-defect", evidence: ["#810", "report.json:thinking-selector"], rationale: "The supported thinking selector viewport and command-row presentation remain different." }],
	["editor-long", { classification: "product-defect", evidence: ["report.json:editor-long"], rationale: "The long editor text wraps identically, but the 72-column startup help retains the same segment-boundary difference recorded by boot-72 after pi's runtime banner and onboarding copy are projected out." }],
	["editor-cjk", { classification: "product-defect", evidence: ["report.json:editor-cjk"], rationale: "The mixed-width editor text wraps identically, but the 72-column startup help retains the same segment-boundary difference recorded by boot-72 after pi's runtime banner and onboarding copy are projected out." }],
	["editor-token", { classification: "match", evidence: ["report.json:editor-token"], rationale: "Narrow unbreakable editor input matches pi: the editor text is unstyled like pi's and the composition is equal." }],
	["user-message", { classification: "product-defect", evidence: ["report.json:user-message", "report.json:boot-72"], rationale: "After the declared footer-stats projection, the remaining visible difference is the 72-column startup-help segment boundary recorded by boot-72. The user prompt and response content match." }],
	["tool-result", { classification: "capture-timing-artifact", evidence: ["#812", "report.json:tool-result"], rationale: "The mid-stream barrier captures adjacent states: Pike has already painted the next prose row while pi still shows its Working spinner. The post-completion snapshot matches, and tool-result content and title styles are identical." }],
	["status-footer", { classification: "match", evidence: ["report.json:status-footer", "profile:footer-stats"], rationale: "Same footer-stats projection artifact as user-message: the right-aligned row kept the padding computed around the removed token-usage prefix. With the run after the token collapsed, this scenario observes as match." }],
	["resize-72-41", { classification: "product-defect", evidence: ["report.json:resize-72-41", "report.json:boot-72"], rationale: "After resizing, the narrow startup help wraps differently from pi at the same segment boundary recorded by boot-72; projecting the runtime banner and onboarding copy does not remove the difference." }],
	["resize-100-72", { classification: "product-defect", evidence: ["report.json:resize-100-72", "report.json:boot-72"], rationale: "After resizing to 72 columns, the startup help wraps differently from pi at the same segment boundary recorded by boot-72; the resize behavior matches." }],
	["resize-120-100", { classification: "match", evidence: ["report.json:resize-120-100"], rationale: "The profile-stripped shared regular Native TUI header matches pi after resizing." }],
	["scrollback", { classification: "product-defect", evidence: ["report.json:scrollback", "report.json:boot-72"], rationale: "After the declared startup and footer projections, the retained scrollback structure still differs from pi. The snapshots also retain the 72-column startup-help wrap difference recorded by boot-72; profileProjection.scrollbackStructureEqual=false confirms a separate retained-row delta." }],
] satisfies Array<[string, Triage]>);

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

const usageProfileScenarios = new Set(["user-message", "status-footer", "tool-result", "scrollback"]);

// These rows identify the pi runtime or its startup documentation. They are
// profile evidence, not Native TUI capability cells, so the diagnostic
// profile projection removes only these exact forms. The strict projection
// below remains the classification authority.
// debt: these patterns describe pi's startup rows. They are matched by shape
// (version, wording, block glyphs) rather than by revision, so a newer pi
// keeps projecting until its startup chrome actually changes; re-check them
// when it does.
type ProfileCellRowProjection = { name: string; pattern: RegExp; strip?: boolean };

// pi's startup banner. Through v0.87.1 it was one plain identity row
// (`pi v0.87.1`). Newer releases draw it in block glyphs, and the banner's
// second row merges the art with the shared help line, so the row cannot be
// dropped whole without also deleting chrome both runtimes render. A banner
// row that carries only the version is dropped; when the art shares its line
// with shared text the art prefix is stripped and the row is kept.
const bannerArt = "[\\u2580-\\u259f]";
/// What pi's banner row leaves behind once its art is removed: nothing, or the
/// version the banner was drawing. Either way the row named the runtime rather
/// than a Native TUI capability.
const bannerVersionRemainder = /^\s*v?\d+\.\d+\.\d+\s*$/;
const profileCellRowProjections: readonly ProfileCellRowProjection[] = Object.freeze([
	{ name: "runtime-identity", pattern: /^\s*pi v\d+\.\d+\.\d+\s*$/ },
	{
		name: "runtime-identity",
		pattern: new RegExp(`^\\s*${bannerArt}+(?:\\s+${bannerArt}+)*\\s+v\\d+\\.\\d+\\.\\d+\\s*$`),
	},
	{
		name: "runtime-identity",
		// Lookbehind keeps the banner block's own indent, which is chrome the
		// other runtime also renders; the lookahead keeps the following
		// character, so the strip removes the art and nothing else.
		pattern: new RegExp(`(?<=^\\s*)(${bannerArt}+(?:\\s+${bannerArt}+)*\\s+)(?=\\S)`),
		strip: true,
	},
	{ name: "workspace-branch", pattern: /<deterministic-workspace>/ },
	{ name: "startup-documentation", pattern: /^\s*Pi can explain its own features and look up its docs\. Ask it how to(?: use or extend Pi\.)?\s*$/ },
	{ name: "startup-documentation", pattern: /^\s*use or extend Pi\.\s*$/ },
	{ name: "startup-documentation", pattern: /^\s*Pi can explain its own features and\s*$/ },
	{ name: "startup-documentation", pattern: /^\s*look up its docs\. Ask it how to use or\s*$/ },
	{ name: "startup-documentation", pattern: /^\s*extend Pi\.\s*$/ },
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
		FORCE_COLOR: profile.terminalVariables.FORCE_COLOR,
		...extra,
	};
}

function repositoryRootFromEnvironment(): string {
	return process.env.CCH_SOURCE_DIR ?? repoRoot;
}

function piCheckout(): string {
	return process.env.PI_CHECKOUT ?? path.resolve(repositoryRootFromEnvironment(), "../pi");
}

interface PiBaseline {
	checkout: string;
	commit: string;
	version: string;
}

/// Resolve the pi this run compares against. The checkout's own HEAD is the
/// target; `PI_BASELINE_COMMIT` re-asserts a specific revision and fails loudly
/// when the checkout is not there, so a reproducible comparison stays available
/// without making every run depend on a particular historical revision.
function resolvePiBaseline(): PiBaseline {
	const checkout = piCheckout();
	if (!existsSync(checkout)) {
		throw new SkipError(`pi checkout is unavailable at ${checkout}`);
	}
	resolveTsxRunner(checkout);
	const head = execFileSync("git", ["-C", checkout, "rev-parse", "HEAD"], { encoding: "utf8" }).trim();
	const requested = process.env.PI_BASELINE_COMMIT;
	if (requested !== undefined && requested !== "" && head !== requested) {
		throw new Error(`PI_BASELINE_COMMIT=${requested} but the pi checkout is at ${head}`);
	}
	const packageJson = JSON.parse(readFileSync(path.join(checkout, "packages/coding-agent/package.json"), "utf8")) as {
		name: string;
		version: string;
	};
	if (packageJson.name !== packageName) {
		throw new Error(`pi checkout must declare ${packageName}, found ${packageJson.name}`);
	}
	return { checkout, commit: head, version: packageJson.version };
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

/// Resolve the TypeScript runner used to execute this capture script and the
/// pi sources it imports. The runner used to be borrowed from the pi checkout,
/// which only worked while pi declared tsx; pi v0.99 dropped it. An explicit
/// `CCH_DIFFERENTIAL_TSX` wins, then the checkout, then whatever is on PATH.
function resolveTsxRunner(checkout: string): string {
	const candidates = [
		process.env.CCH_DIFFERENTIAL_TSX,
		path.join(checkout, "node_modules/.bin/tsx"),
		"tsx",
	].filter((candidate): candidate is string => candidate !== undefined && candidate !== "");
	for (const candidate of candidates) {
		if (candidate === "tsx") return candidate;
		if (existsSync(candidate)) return candidate;
	}
	throw new SkipError(
		`no TypeScript runner for the pi capture: set CCH_DIFFERENTIAL_TSX or install tsx (checkout ${checkout})`,
	);
}

function captureTsconfig(checkout: string, output: string): string {
	if (existsSync(path.join(checkout, "node_modules/grok-mermaid"))) {
		return path.join(checkout, "tsconfig.json");
	}
	const rootConfig = JSON.parse(readFileSync(path.join(checkout, "tsconfig.json"), "utf8")) as {
		compilerOptions?: { paths?: Record<string, string[]> };
	};
	const paths = { ...(rootConfig.compilerOptions?.paths ?? {}) };
	paths["grok-mermaid"] = [path.resolve(scriptDir, "grok-mermaid-stub.mts")];
	// Write the generated config beside the per-scenario capture (a temporary
	// directory) rather than into the frozen pi checkout, whose cleanliness
	// gates reject untracked files. The checkout's `paths` values are
	// checkout-relative, so the base URL stays the checkout as an absolute path.
	const capturePath = `${output}.tsconfig.json`;
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
				baseUrl: checkout,
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
	const checkout = piCheckout();
	const tsx = resolveTsxRunner(checkout);
	const result = execFileSync(tsx, ["--tsconfig", captureTsconfig(checkout, output), scriptPath, "--runtime", "pi"], {
		cwd: repositoryRootFromEnvironment(),
		env: childEnvironment({
			...scenarioEnvironment(scenario, output),
			PI_CHECKOUT: checkout,
			FORCE_COLOR: profile.terminalVariables.FORCE_COLOR,
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
	// The resolved visible/scrollback cell presentation is the comparison
	// authority. Raw ANSI and ordered SGR tokens remain captured evidence but
	// do not decide classification when the resolved cells agree.
	const scenario = scenarios.find((item) => item.id === "boot-72");
	if (!scenario) throw new Error("ANSI boundary verification requires the boot-72 scenario");
	const cursor = { column: 4, row: 1 };
	const theme = {
		name: "dark",
		colorCapability: "truecolor",
		colors: Object.fromEntries(semanticThemeRoles.map((role) => [role, "#123456"])),
	};
	const style = (foreground: string) => ({
		bold: false,
		dim: false,
		italic: false,
		underline: false,
		blink: false,
		inverse: false,
		hidden: false,
		strikethrough: false,
		foreground,
		background: "",
	});
	const makeCapture = (ansi: string, foreground = "#123456"): Record<string, unknown> => ({
		runtime: "evidence",
		scenario: scenario.id,
		width: scenario.width,
		workspace: "/tmp/cpp-harness-pike-differential",
		inputs: [],
		theme,
		snapshots: [{
			visible: ["row"],
			scrollback: [],
			styledVisible: [[{ text: "row", style: style(foreground) }]],
			styledScrollback: [],
			cursor,
			cursorVisible: true,
			ansi,
		}],
	});
	const projection = stableProjection(makeCapture("\x1b[2J\x1b[H\x1b[38;2;18;52;86mrow\x1b[0m"), scenario, repoRoot);
	assert.deepEqual(
		Object.keys((projection.snapshots as Record<string, unknown>[])[0]).sort(),
		["cursor", "cursorVisible", "scrollback", "sgr", "styledScrollback", "styledVisible", "visible"],
	);
	// Same cell text and same normalized SGR structure, different raw framing
	// bytes (cursor moves, erase sequences): not a difference.
	const reframed = compare(
		makeCapture("\x1b[2J\x1b[H\x1b[38;2;18;52;86mrow\x1b[0m\x1b[1;1H"),
		makeCapture("\x1b[1;1H\x1b[38;2;18;52;86mrow\x1b[0m"),
		scenario,
		repoRoot,
	);
	assert.equal(reframed.classification, "match");
	const tokenOnly = compare(
		makeCapture("\x1b[2J\x1b[H\x1b[38;2;18;52;86mrow\x1b[0m", "#123456"),
		makeCapture("\x1b[2J\x1b[H\x1b[38;2;18;52;86mrow\x1b[39m", "#123456"),
		scenario,
		repoRoot,
	);
	assert.equal(tokenOnly.classification, "match");
	assert.equal((tokenOnly.projection as Record<string, unknown>).sgrStructureEqual, false);
	// Same cell text with a different resolved cell style: a regression.
	const restyled = compare(
		makeCapture("\x1b[2J\x1b[H\x1b[38;2;18;52;86mrow\x1b[0m", "#123456"),
		makeCapture("\x1b[2J\x1b[H\x1b[38;2;99;99;99mrow\x1b[0m", "#636363"),
		scenario,
		repoRoot,
	);
	assert.equal(restyled.classification, "supported-capability-regression");
	assert.equal((restyled.projection as Record<string, unknown>).sgrStructureEqual, false);
	assert.equal((restyled.projection as Record<string, unknown>).visibleCellTextEqual, true);
	assert.equal((restyled.projection as Record<string, unknown>).visibleCellPresentationEqual, false);
	const cursorMoved = compare(
		makeCapture("\x1b[2J\x1b[H\x1b[38;2;18;52;86mrow\x1b[0m"),
		{ ...makeCapture("\x1b[2J\x1b[H\x1b[38;2;18;52;86mrow\x1b[0m"), snapshots: [{
			visible: ["row"],
			scrollback: [],
			styledVisible: [[{ text: "row", style: style("#123456") }]],
			styledScrollback: [],
			cursor: { column: 5, row: 1 },
			cursorVisible: true,
			ansi: "\x1b[2J\x1b[H\x1b[38;2;18;52;86mrow\x1b[0m",
		}] },
		scenario,
		repoRoot,
	);
	assert.equal(cursorMoved.classification, "supported-capability-regression");
	assert.equal((cursorMoved.projection as Record<string, unknown>).cursorEqual, false);
	const cursorHidden = compare(
		makeCapture("\x1b[2J\x1b[H\x1b[38;2;18;52;86mrow\x1b[0m"),
		{ ...makeCapture("\x1b[2J\x1b[H\x1b[38;2;18;52;86mrow\x1b[0m"), snapshots: [{
			visible: ["row"],
			scrollback: [],
			styledVisible: [[{ text: "row", style: style("#123456") }]],
			styledScrollback: [],
			cursor,
			cursorVisible: false,
			ansi: "\x1b[2J\x1b[H\x1b[38;2;18;52;86mrow\x1b[0m",
		}] },
		scenario,
		repoRoot,
	);
	assert.equal(cursorHidden.classification, "supported-capability-regression");
	assert.equal((cursorHidden.projection as Record<string, unknown>).cursorEqual, false);
}

// canonicalStyledStyle's slot order. The presentation comparison resolves only
// the two color slots to a semantic role; every flag slot stays exact, so bold,
// dim, italic, underline, blink, inverse, hidden, and strikethrough keep
// comparing by value.
const canonicalStyledStyleSlots: readonly string[] = Object.freeze([
	"bold",
	"dim",
	"italic",
	"underline",
	"blink",
	"inverse",
	"hidden",
	"strikethrough",
	"foreground",
	"background",
]);
const canonicalStyledColorSlots: readonly string[] = Object.freeze(["foreground", "background"]);

// The styled-cell presentation comparison is a semantic role comparison, not an
// exact-RGB one and not an ignore-colors one: the role mapping themeParity
// already accepts (issue #797) is the same mapping the cells are compared
// through. Every case below is one the check must admit or still separate.
function verifyStyledRolePresentation(): void {
	const pikePalette = Object.fromEntries([
		["text", "#d4d4d4"],
		["muted", "#808080"],
		["border", "#5f87ff"],
		["accent", "#8abeb7"],
		["success", "#b5bd68"],
		["warning", "#ffff00"],
		["error", "#cc6666"],
		["selectedBg", "#3a3a4a"],
	]);
	// pi v0.99.2 moved every semantic role and preserved the partition.
	const piPalette = Object.fromEntries([
		["text", "#dee0e1"],
		["muted", "#9da5a9"],
		["border", "#5fa8cc"],
		["accent", "#9d8fe0"],
		["success", "#71c3b0"],
		["warning", "#f2e05c"],
		["error", "#dd7f6c"],
		["selectedBg", "#2f4568"],
	]);
	// The comparison sees the canonical style the report carries, not a raw
	// style object, so the fixture canonicalizes the way normalizeStyledRows does.
	const styledRun = (text: string, style: Record<string, unknown>) => ({ text, style: canonicalStyledStyle(style) });
	const styledCapture = (themeColors: Record<string, string>, style: Record<string, unknown>) => ({
		themeColors,
		snapshots: [{ styledVisible: [[styledRun("row", style)]], styledScrollback: [] }],
	});
	const styled = (foreground: string, extra: Record<string, unknown> = {}): Record<string, unknown> => ({ foreground, ...extra });
	// The same role carrying a different exact RGB is the palette difference
	// themeParity classifies as acceptable, so it is not a cell difference.
	assert.equal(styledPresentationEqual(styledCapture(pikePalette, styled("#d4d4d4")), styledCapture(piPalette, styled("#dee0e1"))), true);
	assert.equal(styledPresentationEqual(
		styledCapture(pikePalette, styled("#d4d4d4", { background: "#3a3a4a" })),
		styledCapture(piPalette, styled("#dee0e1", { background: "#2f4568" })),
	), true);
	// A different role stays unequal even when both colors are plausible
	// palette entries: the case an ignore-colors stand-in would let through.
	assert.equal(styledPresentationEqual(styledCapture(pikePalette, styled("#d4d4d4")), styledCapture(piPalette, styled("#9da5a9"))), false);
	// A color that resolves to no role on the cell's own runtime stays exact,
	// so a surface-specific or unknown color is never resolved into agreement.
	assert.equal(styledPresentationEqual(styledCapture(pikePalette, styled("#d4d4d4")), styledCapture(piPalette, styled("#123456"))), false);
	// A palette whose partition collapsed cannot be resolved into a match: muted
	// now shares text's color, so the shared color stays ambiguous and exact.
	assert.equal(styledPresentationEqual(styledCapture(pikePalette, styled("#808080")), styledCapture({ ...piPalette, muted: piPalette.text }, styled("#dee0e1"))), false);
	// Flags compare by value whatever the colors resolve to.
	for (const flag of ["bold", "italic", "underline", "inverse"] as const) {
		assert.equal(styledPresentationEqual(
			styledCapture(pikePalette, styled("#d4d4d4", { [flag]: false })),
			styledCapture(piPalette, styled("#dee0e1", { [flag]: true })),
		), false);
	}
	// The default foreground carries no color token and never becomes a role.
	assert.equal(styledPresentationEqual(styledCapture(pikePalette, { foreground: "" }), styledCapture(piPalette, styled("#dee0e1"))), false);
	// Without theme evidence on either side the comparison falls back to exact
	// RGB instead of admitting a difference it cannot classify.
	const themeLessCapture = (foreground: string) => ({
		snapshots: [{ styledVisible: [[styledRun("row", styled(foreground))]], styledScrollback: [] }],
	});
	assert.equal(styledPresentationEqual(themeLessCapture("#d4d4d4"), themeLessCapture("#d4d4d4")), true);
	assert.equal(styledPresentationEqual(themeLessCapture("#d4d4d4"), themeLessCapture("#dee0e1")), false);
}

function canonicalStyledStyle(value: unknown): unknown[] {
	const style = value !== null && typeof value === "object" ? value as Record<string, unknown> : {};
	const flagSlots = canonicalStyledStyleSlots.filter((slot) => !canonicalStyledColorSlots.includes(slot));
	return [
		...flagSlots.map((slot) => Boolean(style[slot])),
		...canonicalStyledColorSlots.map((slot) => String(style[slot] ?? "")),
	];
}

function styledColorSlotIndex(slot: string): number {
	const index = canonicalStyledStyleSlots.indexOf(slot);
	if (index < 0) throw new Error(`canonical styled style has no ${slot} slot`);
	return index;
}

// A styled cell's color is compared as the semantic role it plays on the
// runtime that emitted it, reusing the role mapping themeParity compares
// (issue #797): a palette difference that preserves the role partition is
// evidence, not a presentation difference. A color two roles share is
// ambiguous and stays exact, and a color no single role resolves stays exact,
// so a broken role mapping can never be resolved away by this projection.
function semanticRoleToken(color: string, colors: Record<string, string> | undefined): string {
	if (colors === undefined || color === "") return color;
	const roles = semanticThemeRoles.filter((role) => colors[role] === color);
	return roles.length === 1 ? `role:${roles[0]}` : color;
}

function semanticStyledRows(rows: unknown, colors: Record<string, string> | undefined): unknown[] {
	if (!Array.isArray(rows)) return [];
	if (colors === undefined) return rows;
	return rows.map((row) => {
		if (!Array.isArray(row)) return [];
		return row.map((run) => {
			const style = (run as Record<string, unknown>).style;
			if (!Array.isArray(style) || style.length !== canonicalStyledStyleSlots.length) return run;
			const resolved = [...style] as unknown[];
			for (const slot of canonicalStyledColorSlots) {
				const index = styledColorSlotIndex(slot);
				resolved[index] = semanticRoleToken(String(style[index] ?? ""), colors);
			}
			return { ...run, style: resolved };
		});
	});
}

function projectionThemeColors(projection: Record<string, unknown>): Record<string, string> | undefined {
	const colors = projection.themeColors;
	if (colors === null || typeof colors !== "object") return undefined;
	return colors as Record<string, string>;
}

/// The styled-cell presentation as compared: colors resolved to semantic roles.
/// Exact RGB stays in the report as evidence.
function semanticStyledProjection(projection: Record<string, unknown>): unknown[] {
	const colors = projectionThemeColors(projection);
	const snapshots = Array.isArray(projection.snapshots) ? projection.snapshots as Record<string, unknown>[] : [];
	return snapshots.map((snapshot) => ({
		visible: semanticStyledRows(snapshot.styledVisible, colors),
		scrollback: semanticStyledRows(snapshot.styledScrollback, colors),
	}));
}

function normalizeStyledRows(value: unknown, workspace: string, repositoryRoot: string, scenario: Scenario): unknown[] {
	if (!Array.isArray(value)) return [];
	return value.map((row) => {
		if (!Array.isArray(row)) return [];
		return row.map((run) => {
			const item = run as Record<string, unknown>;
			return {
				text: normalizeCellText(String(item.text ?? ""), workspace, repositoryRoot, scenario),
				style: canonicalStyledStyle(item.style),
			};
		}).filter((run) => run.text !== "");
	});
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
			const rawCursor = snapshot.cursor as Record<string, unknown> | undefined;
			const ansi = typeof snapshot.ansi === "string" ? replaceFixturePaths(snapshot.ansi, workspace, repositoryRoot, scenario) : "";
			const visible = Array.isArray(snapshot.visible)
				? snapshot.visible.map((line) => normalizeCellText(String(line), workspace, repositoryRoot, scenario))
				: [];
			return {
				visible,
				scrollback: Array.isArray(snapshot.scrollback)
					? snapshot.scrollback.map((line) => normalizeCellText(String(line), workspace, repositoryRoot, scenario))
					: [],
				styledVisible: normalizeStyledRows(snapshot.styledVisible, workspace, repositoryRoot, scenario),
				styledScrollback: normalizeStyledRows(snapshot.styledScrollback, workspace, repositoryRoot, scenario),
				cursor: rawCursor === undefined ? undefined : { column: rawCursor.column, row: rawCursor.row },
				cursorVisible: snapshot.cursorVisible,
				ansi,
				sgr: sgrTokens(ansi),
			};
		}),
	};
}

/// The projection the legacy stable digests cover. This path is frozen: the
/// checked-in report's recorded digests were computed over its exact output,
/// so it stays byte-stable even where re-normalizing already-canonical styled
/// rows would zero their colors; the comparison paths use
/// stableProjectionForComparison instead.
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
			styledVisible: snapshot.styledVisible,
			styledScrollback: snapshot.styledScrollback,
			cursor: snapshot.cursor,
			cursorVisible: snapshot.cursorVisible,
			sgr: (snapshot.sgr as string[]).map(normalizeSgrToken),
		})),
	};
}

// debt: replace this text projection with typed footer evidence when the product exposes one.
function projectDiagnosticFooterRows(rows: string[], scenario: Pick<Scenario, "id">): string[] {
	if (!usageProfileScenarios.has(scenario.id)) return rows;
	// The status row is right-aligned: usage stats on the left, model and
	// thinking state on the right edge. pi prefixes the stats with a token-usage
	// reading this projection drops, and the row is laid out around the length
	// of the whole stats token, so the padding that follows it is the width of
	// content the projection has already removed. Replacing only the token
	// leaves that padding behind and the projected row still reads as a
	// difference this profile declared out of scope (issue #800, footer
	// cluster). Collapsing the run that follows the token re-anchors the row
	// the same way the blank-row handling re-anchors the vertical axis, and it
	// still compares the right-aligned remainder exactly: a model name or
	// thinking-state difference is unaffected.
	return rows.map((line) => {
		const projected = line.replace(/^(\s*)(?:[↑↓]\S+\s+)*\d+(?:\.\d+)?%\/128k \(auto\)\s*/, "$1<footer-stats> ");
		// A row that held nothing but the stats token has no right-aligned
		// remainder, so it keeps no separator.
		return projected.replace(/<footer-stats> $/, "<footer-stats>");
	});
}

// normalizeCapture is frozen: the checked-in report's stable digests were
// recorded over its current output, and re-running it on an already-normalized
// capture would re-key canonical style arrays by slot name and zero every
// color. The comparison path therefore canonicalizes styled rows idempotently:
// an already-canonical style array passes through unchanged. Raw captures carry
// keyed style objects and normalize identically either way, so verify-mode
// behaviour is unchanged.
function idempotentNormalizeStyledRows(value: unknown, workspace: string, repositoryRoot: string, scenario: Scenario): unknown[] {
	if (!Array.isArray(value)) return [];
	return value.map((row) => {
		if (!Array.isArray(row)) return [];
		return row.map((run) => {
			const item = run as Record<string, unknown>;
			const style = Array.isArray(item.style) && item.style.length === canonicalStyledStyleSlots.length
				? item.style
				: canonicalStyledStyle(item.style);
			return {
				text: normalizeCellText(String(item.text ?? ""), workspace, repositoryRoot, scenario),
				style,
			};
		}).filter((kept) => kept.text !== "");
	});
}

function normalizeCaptureForComparison(capture: Record<string, unknown>, scenario: Scenario, repositoryRoot: string): Record<string, unknown> {
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
			const rawCursor = snapshot.cursor as Record<string, unknown> | undefined;
			const ansi = typeof snapshot.ansi === "string" ? replaceFixturePaths(snapshot.ansi, workspace, repositoryRoot, scenario) : "";
			const visible = Array.isArray(snapshot.visible)
				? snapshot.visible.map((line) => normalizeCellText(String(line), workspace, repositoryRoot, scenario))
				: [];
			return {
				visible,
				scrollback: Array.isArray(snapshot.scrollback)
					? snapshot.scrollback.map((line) => normalizeCellText(String(line), workspace, repositoryRoot, scenario))
					: [],
				styledVisible: idempotentNormalizeStyledRows(snapshot.styledVisible, workspace, repositoryRoot, scenario),
				styledScrollback: idempotentNormalizeStyledRows(snapshot.styledScrollback, workspace, repositoryRoot, scenario),
				cursor: rawCursor === undefined ? undefined : { column: rawCursor.column, row: rawCursor.row },
				cursorVisible: snapshot.cursorVisible,
				ansi,
				sgr: sgrTokens(ansi),
			};
		}),
	};
}

function stableProjectionForComparison(capture: Record<string, unknown>, scenario: Scenario, repositoryRoot: string): Record<string, unknown> {
	const normalized = normalizeCaptureForComparison(capture, scenario, repositoryRoot);
	return {
		runtime: normalized.runtime,
		scenario: normalized.scenario,
		width: normalized.width,
		workspace: normalized.workspace,
		inputs: normalized.inputs,
		snapshots: (normalized.snapshots as Record<string, unknown>[]).map((snapshot) => ({
			visible: snapshot.visible,
			scrollback: snapshot.scrollback,
			styledVisible: snapshot.styledVisible,
			styledScrollback: snapshot.styledScrollback,
			cursor: snapshot.cursor,
			cursorVisible: snapshot.cursorVisible,
			sgr: (snapshot.sgr as string[]).map(normalizeSgrToken),
		})),
	};
}

type ProfileCellProjection = { rows: string[]; removed: Record<string, number>; keptRowMap: number[] };

/// Whether a styled row opens with pi's block-glyph banner art. The glyph run
/// is its own styled segment, so this does not depend on the spacing a text
/// projection would see between segments.
function startsWithBannerArt(segments: unknown[]): boolean {
	const head = segments[0] as { text?: string } | undefined;
	const headText = (head?.text ?? "").trim();
	return headText.length > 0 && new RegExp(`^${bannerArt}+$`).test(headText);
}

function projectProfileCellRows(rows: string[]): ProfileCellProjection {
	const removed: Record<string, number> = {};
	for (const projection of profileCellRowProjections) removed[projection.name] = 0;
	// keptRowMap[rawIndex] is the row's projected index, or -1 when the
	// projection dropped it. The cursor row shares the visible-frame
	// coordinates and is mapped through this table.
	const keptRowMap: number[] = [];
	let retainedCount = 0;
	const retained = rows.flatMap((line) => {
		const projection = profileCellRowProjections.find((candidate) => candidate.pattern.test(line));
		if (projection !== undefined) {
			// A strip projection removes identity from a row that also carries
			// shared chrome, so the row survives and counts as no removal: the
			// scrollback discount must not absorb a row that is still compared.
			if (projection.strip !== true) {
				removed[projection.name] += 1;
				keptRowMap.push(-1);
				return [];
			}
			const stripped = line.replace(projection.pattern, "");
			if (stripped.trim() === "") {
				keptRowMap.push(-1);
				return [];
			}
			keptRowMap.push(retainedCount);
			retainedCount += 1;
			return [stripped];
		}
		if (line.trim() === "") {
			keptRowMap.push(-1);
			return [];
		}
		keptRowMap.push(retainedCount);
		retainedCount += 1;
		return [line];
	});
	return { rows: retained, removed, keptRowMap };
}

// Rows the profile projection removed from one composed buffer that the other
// runtime kept. A projection both runtimes share (the workspace/branch row)
// is never exclusive, so it never discounts anything.
function exclusiveProfileRemovals(mine: Record<string, number>, theirs: Record<string, number>): number {
	let total = 0;
	for (const name of new Set([...Object.keys(mine), ...Object.keys(theirs)])) {
		total += Math.max(0, (mine[name] ?? 0) - (theirs[name] ?? 0));
	}
	return total;
}

type ScrollbackGeometry = { retained: string[]; height: number; viewportRemoved: Record<string, number> };

// A snapshot is a fixed-height viewport plus the rows retained above it, so a
// taller composed buffer retains more rows for identical scroll behaviour. A
// retained-row count compared raw across two buffers of different height
// therefore reports every upstream height difference as a scrollback-structure
// difference (issue #815). The comparison is length-compensated: the height a
// buffer carries beyond the shorter of the two is discounted from its retained
// rows, but only up to the rows the profile projection removed from that
// runtime's viewport. Heights are measured before the projection, because the
// split is a property of the rendered buffer and the projected rows are exactly
// what the discount pays for. Rows removed above the split never need a
// discount (they are already out of the retained accounting), a projection
// both runtimes share never discounts, and product height (blank-row spacing,
// #814) is never discounted because no profile projection explains it. A
// genuine viewport-top or ordering difference therefore still compares
// unequal: the compensation is bounded, never a constant `true`.
// debt: the height term is a bound, not a second estimate: a projected
// viewport row is part of its own buffer, so it binds only when a capture
// claims a height it did not render. The discount trims the retained tail, so
// a difference confined to the trimmed rows is not compared. Revisit both
// terms if the profile projection ever removes rows a runtime did not render.
function scrollbackGeometry(snapshot: Record<string, unknown>, scenario: Pick<Scenario, "id">): ScrollbackGeometry {
	const scrollback = snapshot.scrollback as string[];
	const visible = snapshot.visible as string[];
	return {
		retained: projectProfileCellRows(projectDiagnosticFooterRows(scrollback, scenario)).rows,
		height: scrollback.length + visible.length,
		viewportRemoved: projectProfileCellRows(projectDiagnosticFooterRows(visible, scenario)).removed,
	};
}

function compensatedRetainedRows(retained: string[], discount: number): string[] {
	return retained.slice(0, Math.max(0, retained.length - discount));
}

function compareScrollbackStructure(
	pike: Record<string, unknown>,
	pi: Record<string, unknown>,
	scenario: Pick<Scenario, "id">,
): { equal: boolean; lengthCompensation: { pike: number[]; pi: number[] } } {
	const pikeSnapshots = pike.snapshots as Record<string, unknown>[];
	const piSnapshots = pi.snapshots as Record<string, unknown>[];
	const pikeCompensation: number[] = [];
	const piCompensation: number[] = [];
	const shared = Math.min(pikeSnapshots.length, piSnapshots.length);
	// Every barrier is measured, not just the equal prefix, so the recorded
	// compensation stays per-barrier evidence for an unequal scenario too.
	let equal = pikeSnapshots.length === piSnapshots.length;
	for (let index = 0; index < shared; index += 1) {
		const pikeGeometry = scrollbackGeometry(pikeSnapshots[index] as Record<string, unknown>, scenario);
		const piGeometry = scrollbackGeometry(piSnapshots[index] as Record<string, unknown>, scenario);
		const shortest = Math.min(pikeGeometry.height, piGeometry.height);
		const pikeDiscount = Math.min(pikeGeometry.height - shortest, exclusiveProfileRemovals(pikeGeometry.viewportRemoved, piGeometry.viewportRemoved));
		const piDiscount = Math.min(piGeometry.height - shortest, exclusiveProfileRemovals(piGeometry.viewportRemoved, pikeGeometry.viewportRemoved));
		pikeCompensation.push(pikeDiscount);
		piCompensation.push(piDiscount);
		const same = JSON.stringify(compensatedRetainedRows(pikeGeometry.retained, pikeDiscount)) === JSON.stringify(compensatedRetainedRows(piGeometry.retained, piDiscount));
		if (!same) equal = false;
	}
	return { equal, lengthCompensation: { pike: pikeCompensation, pi: piCompensation } };
}

function projectProfileStyledRows(rows: unknown, scenario: Pick<Scenario, "id">): unknown[] {
	if (!Array.isArray(rows)) return [];
	const retained: unknown[] = [];
	for (const row of rows) {
		const text = styledRowText(row);
		if (text.trim() === "") continue;
		if (usageProfileScenarios.has(String(scenario.id)) && text.includes("/128k")) continue;
		// The styled reconstruction carries no spacing between segments, so a
		// text pattern cannot see where the banner art ends. The art is styled
		// apart from the chrome beside it, so it is recognized by segment: drop
		// leading segments that are only block glyphs, and keep whatever the
		// row still holds. A row that was nothing but art becomes empty and is
		// dropped, which is the same verdict the text projection reaches.
		const segments = Array.isArray(row) ? [...row] : [];
		if (segments.length > 0 && startsWithBannerArt(segments)) {
			while (segments.length > 0 && startsWithBannerArt(segments)) segments.shift();
			// What is left after the art is either nothing or a bare version
			// token, both of which are runtime identity: the text projection
			// drops that row through its whole-row pattern, and the styled one
			// has to reach the same verdict. Anything else is shared chrome that
			// the art happened to share a line with.
			if (segments.length === 0 || bannerVersionRemainder.test(styledRowText(segments))) continue;
			retained.push(segments);
			continue;
		}
		if (profileCellRowProjections.some((projection) => projection.pattern.test(text))) continue;
		retained.push(row);
	}
	return retained;
}

function profileProjectionForComparison(projection: Record<string, unknown>, scenario: Pick<Scenario, "id">): Record<string, unknown> {
	const snapshots = projection.snapshots as Record<string, unknown>[];
	return {
		...projection,
		snapshots: snapshots.map((snapshot) => {
			const projectedVisible = projectProfileCellRows(projectDiagnosticFooterRows(snapshot.visible as string[], scenario));
			// The cursor row lives in the same viewport coordinate space as the
			// visible rows, so it is mapped through the projection's kept-row
			// table. Without this, every snapshot whose frame carries projected
			// startup rows reports a cursor row mismatch that is a projection
			// artifact, not a runtime difference.
			const rawCursor = snapshot.cursor as Record<string, unknown> | undefined;
			const cursorRow = typeof rawCursor?.row === "number" ? rawCursor.row : undefined;
			let projectedCursorRow: number | undefined;
			if (cursorRow !== undefined) {
				projectedCursorRow = 0;
				for (let index = Math.min(cursorRow, projectedVisible.keptRowMap.length - 1); index >= 0; index -= 1) {
					const mapped = projectedVisible.keptRowMap[index];
					if (mapped !== undefined && mapped >= 0) {
						projectedCursorRow = mapped;
						break;
					}
				}
			}
			return {
				visible: projectedVisible.rows,
				scrollback: projectProfileCellRows(projectDiagnosticFooterRows(snapshot.scrollback as string[], scenario)).rows,
				styledVisible: projectProfileStyledRows(snapshot.styledVisible, scenario),
				styledScrollback: projectProfileStyledRows(snapshot.styledScrollback, scenario),
				cursor: rawCursor === undefined || projectedCursorRow === undefined
					? rawCursor
					: { ...rawCursor, row: projectedCursorRow },
				cursorVisible: snapshot.cursorVisible,
				sgr: snapshot.sgr,
			};
		}),
	};
}

function compareProfileProjection(pike: Record<string, unknown>, pi: Record<string, unknown>, scenario: Pick<Scenario, "id">): Record<string, unknown> {
	const pikeProjected = profileProjectionForComparison(pike, scenario);
	const piProjected = profileProjectionForComparison(pi, scenario);
	const pikeSnapshots = pikeProjected.snapshots as Record<string, unknown>[];
	const piSnapshots = piProjected.snapshots as Record<string, unknown>[];
	const scrollback = compareScrollbackStructure(pike, pi, scenario);
	const cursorEqual = pikeSnapshots.length === piSnapshots.length && pikeSnapshots.every((snapshot, index) => {
		const other = piSnapshots[index];
		return JSON.stringify({ cursor: snapshot.cursor, cursorVisible: snapshot.cursorVisible }) ===
			JSON.stringify({ cursor: other.cursor, cursorVisible: other.cursorVisible });
	});
	return {
		visibleCellTextEqual: JSON.stringify(pikeSnapshots.map((item) => item.visible)) === JSON.stringify(piSnapshots.map((item) => item.visible)),
		visibleCellPresentationEqual: styledPresentationEqual(pikeProjected, piProjected),
		scrollbackStructureEqual: scrollback.equal,
		scrollbackLengthCompensation: scrollback.lengthCompensation,
		scrollbackCellPresentationEqual: styledRowsEqual(pikeProjected, piProjected, "scrollback"),
		cursorEqual,
		footerStatsProjected: usageProfileScenarios.has(String(pike.scenario)),
		removedCellRows: {
			pike: (pike.snapshots as Record<string, unknown>[]).map((snapshot) => projectProfileCellRows(projectDiagnosticFooterRows(snapshot.visible as string[], scenario)).removed),
			pi: (pi.snapshots as Record<string, unknown>[]).map((snapshot) => projectProfileCellRows(projectDiagnosticFooterRows(snapshot.visible as string[], scenario)).removed),
		},
		role: "diagnostic evidence; resolved comparison also uses styled cells after this profile",
	};
}

function styledRowText(row: unknown): string {
	if (!Array.isArray(row)) return "";
	return row.map((run) => String((run as Record<string, unknown>).text ?? "")).join("");
}

function omitProjectionLines(projection: Record<string, unknown>, patterns: string[]): Record<string, unknown> {
	if (patterns.length === 0) return projection;
	const snapshots = projection.snapshots as Record<string, unknown>[];
	return {
		...projection,
		snapshots: snapshots.map((snapshot) => ({
			visible: (snapshot.visible as string[]).filter((line) => !patterns.some((pattern) => line.includes(pattern))),
			scrollback: (snapshot.scrollback as string[]).filter((line) => !patterns.some((pattern) => line.includes(pattern))),
			styledVisible: (snapshot.styledVisible as unknown[]).filter((row) => !patterns.some((pattern) => styledRowText(row).includes(pattern))),
			styledScrollback: (snapshot.styledScrollback as unknown[]).filter((row) => !patterns.some((pattern) => styledRowText(row).includes(pattern))),
			cursor: snapshot.cursor,
			cursorVisible: snapshot.cursorVisible,
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

function styledRowsEqual(left: Record<string, unknown>, right: Record<string, unknown>, region: "visible" | "scrollback"): boolean {
	const leftRows = semanticStyledProjection(left).map((snapshot) => (snapshot as Record<string, unknown>)[region]);
	const rightRows = semanticStyledProjection(right).map((snapshot) => (snapshot as Record<string, unknown>)[region]);
	return JSON.stringify(leftRows) === JSON.stringify(rightRows);
}

function styledPresentationEqual(left: Record<string, unknown>, right: Record<string, unknown>): boolean {
	return styledRowsEqual(left, right, "visible") && styledRowsEqual(left, right, "scrollback");
}

function omissionSgrEqual(left: Record<string, unknown>, right: Record<string, unknown>): boolean {
	const leftSnapshots = left.snapshots as Record<string, unknown>[];
	const rightSnapshots = right.snapshots as Record<string, unknown>[];
	return JSON.stringify(leftSnapshots.map((snapshot) => snapshot.sgr)) === JSON.stringify(rightSnapshots.map((snapshot) => snapshot.sgr));
}

function isIntentionalSubsetOmission(
	scenario: Pick<Scenario, "omissions">,
	omissionProjectionEqual: boolean,
	pikeObserved: boolean,
	piObserved: boolean,
	cursorEqual: boolean,
): boolean {
	return scenario.omissions.length > 0 && piObserved && !pikeObserved && omissionProjectionEqual && cursorEqual;
}

function verifyProjectionPolicy(): void {
	const scenario = { omissions: ["pi-only chrome"] };
	assert.equal(isIntentionalSubsetOmission(scenario, true, false, true, true), true);
	assert.equal(isIntentionalSubsetOmission(scenario, false, false, true, true), false);
	assert.equal(isIntentionalSubsetOmission(scenario, true, true, true, true), false);
	assert.equal(isIntentionalSubsetOmission(scenario, true, false, false, true), false);
	assert.equal(isIntentionalSubsetOmission(scenario, true, false, true, false), false);

	const styled = (text: string) => [{ text, style: { foreground: "#ff0000" } }];
	const left = {
		snapshots: [{
			visible: ["same", "pi-only chrome"],
			scrollback: [],
			styledVisible: [styled("same"), styled("pi-only chrome")],
			styledScrollback: [],
			sgr: ["\x1b[31m"],
		}],
	};
	const right = {
		snapshots: [{
			visible: ["same", "pi-only chrome"],
			scrollback: [],
			styledVisible: [styled("same"), styled("pi-only chrome")],
			styledScrollback: [],
			sgr: ["\x1b[31m"],
		}],
	};
	const filteredLeft = omitProjectionLines(left, ["pi-only"]);
	const filteredRight = omitProjectionLines(right, ["pi-only"]);
	assert.equal(omissionTextEqual(filteredLeft, filteredRight), true);
	assert.equal(styledPresentationEqual(filteredLeft, filteredRight), true);
	assert.equal(omissionSgrEqual(filteredLeft, filteredRight), true);
	assert.equal(omissionSgrEqual(filteredLeft, { snapshots: [{ ...(filteredRight.snapshots[0] as Record<string, unknown>), sgr: ["\x1b[32m"] }] }), false);

	const profile = projectProfileCellRows([" pi v0.87.1", "Pi can explain its own features and look up its docs. Ask it how to", "use or extend Pi.", "kept"]);
	assert.deepEqual(profile.rows, ["kept"]);
	assert.deepEqual(profile.removed, { "runtime-identity": 1, "workspace-branch": 0, "startup-documentation": 2 });
	const narrowDocumentation = projectProfileCellRows([
		" Pi can explain its own features and",
		" look up its docs. Ask it how to use or",
		" extend Pi.",
		"kept",
	]);
	assert.deepEqual(narrowDocumentation.rows, ["kept"]);
	assert.deepEqual(narrowDocumentation.removed, { "runtime-identity": 0, "workspace-branch": 0, "startup-documentation": 3 });
	assert.equal(normalizeCellText("/repository (main)", "/tmp/workspace", "/repository"), "<deterministic-workspace>");
	assert.equal(replaceFixturePaths("/repository (main) suffix", "/tmp/workspace", "/repository"), "<repository-root> (<branch>) suffix");
	assert.deepEqual(projectDiagnosticFooterRows(["0.0%/128k (auto) <model>", "kept"], { id: "user-message" }), ["<footer-stats> <model>", "kept"]);
	assert.deepEqual(projectDiagnosticFooterRows(["0.0%/128k (auto) <model>"], { id: "boot-72" }), ["0.0%/128k (auto) <model>"]);
	// A row that is nothing but the stats token has no right-aligned remainder,
	// so the collapsed padding must not leave a trailing separator behind.
	assert.deepEqual(projectDiagnosticFooterRows(["0.0%/128k (auto)"], { id: "user-message" }), ["<footer-stats>"]);
	// The case the token-only replacement let through: pi prefixes the stats
	// with a token-usage reading, which lengthens the token and shortens the
	// right-aligned padding by the same amount. Two rows that differ only in
	// the content this projection removes must project to one row, or the
	// footer cluster reports a difference the profile already scoped out.
	const pikeFooterRow = "0.0%/128k (auto)".padEnd(37) + "<model> • thinking off";
	const piFooterRow = "↑1.4k ↓8 1.1%/128k (auto)".padEnd(37) + "<model> • thinking off";
	assert.notEqual(pikeFooterRow, piFooterRow);
	const projectedPikeFooter = projectDiagnosticFooterRows([pikeFooterRow], { id: "user-message" });
	const projectedPiFooter = projectDiagnosticFooterRows([piFooterRow], { id: "user-message" });
	assert.deepEqual(projectedPikeFooter, ["<footer-stats> <model> • thinking off"]);
	assert.deepEqual(projectedPiFooter, projectedPikeFooter);
	// Re-anchoring the padding must not swallow the right-aligned remainder:
	// a model or thinking-state difference is a real difference and has to
	// survive the projection. This is the case a padding-collapsing stand-in
	// would let through.
	const piFooterThinkingOn = "↑1.4k ↓8 1.1%/128k (auto)".padEnd(37) + "<model> • thinking on";
	assert.notDeepEqual(projectDiagnosticFooterRows([piFooterThinkingOn], { id: "user-message" }), projectedPikeFooter);
	// Outside the usage scenarios the projection is a no-op, so the same two
	// rows stay byte-different: the collapse is scoped to the footer usage
	// projection and never absorbs a stats difference elsewhere.
	assert.deepEqual(projectDiagnosticFooterRows([pikeFooterRow, piFooterRow], { id: "boot-72" }), [pikeFooterRow, piFooterRow]);

	const retainedSnapshot = (retained: string[], visible: string[]) => ({ snapshots: [{ scrollback: retained, visible }] });
	const sharedViewport = ["third", "fourth"];
	const pikeRetained = retainedSnapshot(["first", "second"], sharedViewport);
	// Two pi-only rows inside pi's viewport push two retained rows out of view.
	// A pure profile height difference must not read as a scrollback-structure
	// difference, and the verdict must not depend on which runtime is left.
	const piHeightOnly = retainedSnapshot(["first", "second", "third", "fourth"], [" pi v0.87.1", " use or extend Pi."]);
	const heightOnlyCompared = compareScrollbackStructure(pikeRetained, piHeightOnly, { id: "boot-72" });
	assert.equal(heightOnlyCompared.equal, true);
	assert.deepEqual(heightOnlyCompared.lengthCompensation, { pike: [0], pi: [2] });
	assert.equal(compareScrollbackStructure(piHeightOnly, pikeRetained, { id: "boot-72" }).equal, true);
	// A genuine viewport-top difference with no height to compensate stays unequal.
	const viewportTop = compareScrollbackStructure(pikeRetained, retainedSnapshot(["first", "second", "third"], sharedViewport), { id: "boot-72" });
	assert.equal(viewportTop.equal, false);
	assert.deepEqual(viewportTop.lengthCompensation, { pike: [0], pi: [0] });
	// Retained rows in a different order stay unequal.
	assert.equal(compareScrollbackStructure(pikeRetained, retainedSnapshot(["second", "first"], sharedViewport), { id: "boot-72" }).equal, false);
	// The compensation pays for the height a pi-only viewport row added, not a
	// retained-row difference: with the same two rows pushed out of view, a
	// reordered retained row still compares unequal.
	const masked = compareScrollbackStructure(pikeRetained, retainedSnapshot(["second", "first", "third", "fourth"], [" pi v0.87.1", " use or extend Pi."]), { id: "boot-72" });
	assert.equal(masked.equal, false);
	assert.deepEqual(masked.lengthCompensation, { pike: [0], pi: [2] });
	// A height difference no profile projection explains is product height, not
	// a compensation: it stays unequal even though the buffers differ in height.
	assert.equal(compareScrollbackStructure(retainedSnapshot(["first"], ["second"]), retainedSnapshot(["first", "second"], ["third"]), { id: "boot-72" }).equal, false);
	// A pi-only row above the split shortens the retained accounting by itself,
	// so it needs no compensation and the retained rows still match.
	const chromeAboveSplit = compareScrollbackStructure(pikeRetained, retainedSnapshot([" pi v0.87.1", "first", "second"], sharedViewport), { id: "boot-72" });
	assert.equal(chromeAboveSplit.equal, true);
	assert.deepEqual(chromeAboveSplit.lengthCompensation, { pike: [0], pi: [0] });
}

function verifyHistoricalComparisonPolicy(): void {
	// Synthetic single-scenario reports exercising the live regression gate.
	// These are the discriminating controls for the comparator contract: SGR
	// cadence and raw-ANSI framing differences are diagnostic-only, while a
	// settled text, style, or cursor change, a missing or restyled approved
	// notice, and any tampering with the stored evidence stay rejected.
	const makeTheme = () => ({
		name: "dark",
		colorCapability: "truecolor",
		colors: {
			text: "#123456",
			muted: "#234567",
			border: "#345678",
			accent: "#456789",
			success: "#56789a",
			warning: "#cd9a22",
			error: "#6789ab",
			selectedBg: "#789abc",
		},
	});
	const style = (foreground: string) => canonicalStyledStyle({ foreground });
	const run = (text: string, foreground = "#123456") => [{ text, style: style(foreground) }];
	const snapshot = (text: string, foreground = "#123456", cursor = { column: 1, row: 0 }, cursorVisible = true, ansi = ""): Record<string, unknown> => ({
		visible: [text],
		scrollback: [],
		styledVisible: [run(text, foreground)],
		styledScrollback: [],
		cursor,
		cursorVisible,
		ansi,
	});
	const makeCapture = (scenario: Scenario, runtime: string, captures: Record<string, unknown>[]): Record<string, unknown> => ({
		runtime,
		scenario: scenario.id,
		width: scenario.width,
		workspace: "/tmp/cpp-harness-pike-differential",
		inputs: scenario.inputs,
		theme: makeTheme(),
		snapshots: captures,
	});
	const makeItem = (scenario: Scenario, pike: Record<string, unknown>, pi: Record<string, unknown>) => {
		const comparison = compare(pike, pi, scenario, repoRoot);
		return {
			id: scenario.id,
			width: scenario.width,
			inputs: scenario.inputs,
			classification: triageByScenario[scenario.id].classification,
			observedClassification: comparison.classification,
			triage: triageByScenario[scenario.id],
			projection: comparison.projection,
			profileProjection: comparison.profileProjection,
			omissionEvidence: comparison.omissionEvidence,
			captures: { pike, pi },
			stableDigest: digest({
				pike: stableProjection(pike, scenario, repoRoot),
				pi: stableProjection(pi, scenario, repoRoot),
			}),
		};
	};
	const expectGateFailure = (action: () => void, detail: string): void => {
		try {
			action();
		} catch (error) {
			assert.ok(String((error as Error).message).includes(detail), `expected a failure naming '${detail}', got: ${String(error)}`);
			return;
		}
		throw new Error(`expected the historical comparison gate to fail with '${detail}'`);
	};
	const boot = scenarios.find((item) => item.id === "boot-72");
	const selector = scenarios.find((item) => item.id === "model-selector");
	assert.ok(boot !== undefined && selector !== undefined);

	// Contract control: an SGR cadence / raw-ANSI framing difference with
	// identical resolved cells is diagnostic-only. The legacy cross-report
	// digest comparison rejects this evidence; the resolved gate accepts it.
	const bootPike = makeCapture(boot, "pike", [snapshot("row", "#123456", { column: 1, row: 0 }, true, "\x1b[2J\x1b[H\x1b[38;2;18;52;86mrow\x1b[0m")]);
	const bootPikeReframed = makeCapture(boot, "pike", [snapshot("row", "#123456", { column: 1, row: 0 }, true, "\x1b[2J\x1b[H\x1b[38;2;18;52;86mr\x1b[0m\x1b[38;2;18;52;86mow\x1b[0m\x1b[1;1H")]);
	const bootPi = makeCapture(boot, "pi", [snapshot("row", "#123456", { column: 1, row: 0 }, true, "\x1b[2J\x1b[H\x1b[38;2;18;52;86mrow\x1b[0m")]);
	const bootExpected = makeItem(boot, bootPike, bootPi);
	const bootActual = makeItem(boot, bootPikeReframed, bootPi);
	assert.notEqual(bootExpected.stableDigest, bootActual.stableDigest);
	verifyScenarioAgainstHistory(bootActual, bootExpected, boot, repoRoot);

	// A changed settled cell is rejected even when it flips no comparison
	// output: the scenario already reads unequal on another row, so every
	// boolean stays identical while the resolved evidence still diverges. This
	// is the case an outputs-only stand-in lets through.
	const twoRowPike = makeCapture(boot, "pike", [{
		...snapshot("row", "#123456", { column: 1, row: 0 }, true, "\x1b[2J\x1b[H\x1b[38;2;18;52;86mrow\x1b[0m"),
		visible: ["row", "pike chrome"],
		styledVisible: [run("row"), run("pike chrome")],
	}]);
	const twoRowPi = makeCapture(boot, "pi", [{
		...snapshot("row", "#123456", { column: 1, row: 0 }, true, "\x1b[2J\x1b[H\x1b[38;2;18;52;86mrow\x1b[0m"),
		visible: ["row", "pi chrome"],
		styledVisible: [run("row"), run("pi chrome")],
	}]);
	const twoRowExpected = makeItem(boot, twoRowPike, twoRowPi);
	const twoRowActualPike = JSON.parse(JSON.stringify(twoRowPike)) as Record<string, unknown>;
	(twoRowActualPike.snapshots as Record<string, unknown>[])[0].visible = ["rows", "pike chrome"];
	(twoRowActualPike.snapshots as Record<string, unknown>[])[0].styledVisible = [run("rows"), run("pike chrome")];
	const twoRowActual = makeItem(boot, twoRowActualPike, twoRowPi);
	expectGateFailure(() => verifyScenarioAgainstHistory(twoRowActual, twoRowExpected, boot, repoRoot), "resolved evidence changed");

	// A changed cell style is rejected.
	const restyledActual = makeItem(boot, makeCapture(boot, "pike", [snapshot("row", "#654321", { column: 1, row: 0 }, true, "\x1b[2J\x1b[H\x1b[38;2;18;52;86mrow\x1b[0m")]), bootPi);
	expectGateFailure(() => verifyScenarioAgainstHistory(restyledActual, bootExpected, boot, repoRoot), "diverged from the adapted checked-in evidence");

	// A moved or hidden cursor is rejected.
	const movedCursorActual = makeItem(boot, makeCapture(boot, "pike", [snapshot("row", "#123456", { column: 2, row: 0 }, true, "\x1b[2J\x1b[H\x1b[38;2;18;52;86mrow\x1b[0m")]), bootPi);
	expectGateFailure(() => verifyScenarioAgainstHistory(movedCursorActual, bootExpected, boot, repoRoot), "diverged from the adapted checked-in evidence");
	const hiddenCursorActual = makeItem(boot, makeCapture(boot, "pike", [snapshot("row", "#123456", { column: 1, row: 0 }, false, "\x1b[2J\x1b[H\x1b[38;2;18;52;86mrow\x1b[0m")]), bootPi);
	expectGateFailure(() => verifyScenarioAgainstHistory(hiddenCursorActual, bootExpected, boot, repoRoot), "diverged from the adapted checked-in evidence");

	// The versioned model-selector adaptation (ADR 0071 / ADR 0075) accepts
	// exactly the approved notice replacement and nothing else.
	const historicalNotice = "Only showing models from configured providers. Use /login to add providers.";
	const gatewayNotice = "Only showing models from the local Magpie gateway.";
	const notice = (text: string): Record<string, unknown> => ({
		visible: [text],
		scrollback: [],
		styledVisible: [run(text, "#cd9a22")],
		styledScrollback: [],
		cursor: { column: 2, row: 6 },
		cursorVisible: false,
		ansi: "",
	});
	const selectorCapture = (runtime: string, noticeText: string, tailText: string) => makeCapture(selector, runtime, [
		snapshot("selector boot"),
		notice(noticeText),
		snapshot(tailText),
	]);
	const historicalPike = selectorCapture("pike", historicalNotice, "Model: <model>");
	const historicalPi = selectorCapture("pi", historicalNotice, "Model: <model>");
	const currentPike = selectorCapture("pike", gatewayNotice, "Model: <model>");
	const currentPi = selectorCapture("pi", historicalNotice, "Model: <model>");
	const selectorExpected = makeItem(selector, historicalPike, historicalPi);
	const selectorActual = makeItem(selector, currentPike, currentPi);
	verifyScenarioAgainstHistory(selectorActual, selectorExpected, selector, repoRoot);

	// A current capture missing the approved notice is rejected.
	const missingNoticeActual = makeItem(selector, selectorCapture("pike", "Some other notice", "Model: <model>"), currentPi);
	expectGateFailure(() => verifyScenarioAgainstHistory(missingNoticeActual, selectorExpected, selector, repoRoot), "must render exactly one");

	// A current capture with the right text but a wrong style is rejected.
	const wrongStylePike = selectorCapture("pike", gatewayNotice, "Model: <model>");
	(wrongStylePike.snapshots as Record<string, unknown>[])[1].styledVisible = [run(gatewayNotice, "#ff0000")];
	const wrongStyleActual = makeItem(selector, wrongStylePike, currentPi);
	expectGateFailure(() => verifyScenarioAgainstHistory(wrongStyleActual, selectorExpected, selector, repoRoot), "gateway notice style changed");

	// A current capture resurrecting a retired literal is rejected.
	const retiredPike = selectorCapture("pike", gatewayNotice, "Use /login to add providers again");
	const retiredActual = makeItem(selector, retiredPike, currentPi);
	expectGateFailure(() => verifyScenarioAgainstHistory(retiredActual, selectorExpected, selector, repoRoot), "resurrected retired literal");

	// Historical evidence that no longer carries the exact anchor row is
	// rejected: the adaptation cannot widen silently.
	const driftedExpected = makeItem(selector, selectorCapture("pike", "Unexpected old notice", "Model: <model>"), historicalPi);
	expectGateFailure(() => verifyScenarioAgainstHistory(selectorActual, driftedExpected, selector, repoRoot), "exactly one historical row");

	// Any other current delta beyond the approved notice row is rejected.
	const extraRowPike = selectorCapture("pike", gatewayNotice, "Model: <model>");
	(extraRowPike.snapshots as Record<string, unknown>[])[0].visible = ["selector boot", "unexpected extra row"];
	(extraRowPike.snapshots as Record<string, unknown>[])[0].styledVisible = [run("selector boot"), run("unexpected extra row")];
	const extraRowActual = makeItem(selector, extraRowPike, currentPi);
	// The adapted comparison already reads unequal on the notice row, so the
	// strict boolean outputs mask this extra delta; the resolved evidence
	// comparison is the layer that still rejects it.
	expectGateFailure(() => verifyScenarioAgainstHistory(extraRowActual, selectorExpected, selector, repoRoot), "resolved evidence changed");

	// The legacy stored-evidence digest still guards the checked-in report: a
	// tampered row is stale against the recorded digest.
	const tamperedExpected = JSON.parse(JSON.stringify(bootExpected)) as Record<string, unknown>;
	const tamperedCaptures = tamperedExpected.captures as Record<string, Record<string, unknown>>;
	(tamperedCaptures.pike.snapshots as Record<string, unknown>[])[0].visible = ["tampered row"];
	expectGateFailure(() => validateStableDigests({ scenarios: [tamperedExpected] }, repoRoot), "stale");

	// Even a tamper that recomputes its own digest is rejected: the stored
	// comparison outputs no longer reproduce from the tampered evidence.
	const recomputedTamper = JSON.parse(JSON.stringify(bootExpected)) as Record<string, unknown>;
	const recomputedCaptures = recomputedTamper.captures as Record<string, Record<string, unknown>>;
	(recomputedCaptures.pike.snapshots as Record<string, unknown>[])[0].visible = ["tampered row"];
	recomputedTamper.stableDigest = digest({
		pike: stableProjection(recomputedCaptures.pike, boot, repoRoot),
		pi: stableProjection(recomputedCaptures.pi, boot, repoRoot),
	});
	expectGateFailure(
		() => verifyScenarioAgainstHistory(makeItem(boot, bootPike, bootPi), recomputedTamper, boot, repoRoot),
		"does not reproduce its recorded comparison",
	);
}

function compare(pike: Record<string, unknown>, pi: Record<string, unknown>, scenario: Scenario, repositoryRoot: string): Record<string, unknown> {
	// The styled-cell comparison resolves a cell color through the theme the
	// capture carries. The colors ride on the comparison projection instead of
	// inside stableProjection so the stable digest stays the cell projection it
	// has always been; cursor position and visibility are included alongside cells.
	const pikeProjection: Record<string, unknown> = {
		...stableProjectionForComparison(pike, scenario, repositoryRoot),
		themeColors: normalizeThemeEvidence(pike.theme, `Pike/${scenario.id}`).colors,
	};
	const piProjection: Record<string, unknown> = {
		...stableProjectionForComparison(pi, scenario, repositoryRoot),
		themeColors: normalizeThemeEvidence(pi.theme, `pi/${scenario.id}`).colors,
	};
	const pikeSnapshots = pikeProjection.snapshots as Record<string, unknown>[];
	const piSnapshots = piProjection.snapshots as Record<string, unknown>[];
	const cursorEqual = pikeSnapshots.length === piSnapshots.length && pikeSnapshots.every((snapshot, index) => {
		const other = piSnapshots[index];
		return JSON.stringify({ cursor: snapshot.cursor, cursorVisible: snapshot.cursorVisible }) ===
			JSON.stringify({ cursor: other.cursor, cursorVisible: other.cursorVisible });
	});
	const workspaceEqual = pikeProjection.workspace === piProjection.workspace;
	const visibleEqual = JSON.stringify(pikeSnapshots.map((item) => item.visible)) === JSON.stringify(piSnapshots.map((item) => item.visible));
	const scrollbackEqual = JSON.stringify(pikeSnapshots.map((item) => item.scrollback)) === JSON.stringify(piSnapshots.map((item) => item.scrollback));
	const visiblePresentationEqual = styledRowsEqual(pikeProjection, piProjection, "visible");
	const scrollbackPresentationEqual = styledRowsEqual(pikeProjection, piProjection, "scrollback");
	const presentationEqual = visiblePresentationEqual && scrollbackPresentationEqual;
	const sgrEqual = JSON.stringify(pikeSnapshots.map((item) => item.sgr)) === JSON.stringify(piSnapshots.map((item) => item.sgr));
	const pikeResolved = profileProjectionForComparison(pikeProjection, scenario);
	const piResolved = profileProjectionForComparison(piProjection, scenario);
	const omissionProjection = omitProjectionLines(pikeResolved, scenario.omissionPatterns ?? []);
	const piOmissionProjection = omitProjectionLines(piResolved, scenario.omissionPatterns ?? []);
	const omissionText = omissionTextEqual(omissionProjection, piOmissionProjection);
	const omissionPresentation = styledPresentationEqual(omissionProjection, piOmissionProjection);
	const resolvedTextEqual = omissionText;
	const equal = workspaceEqual && resolvedTextEqual && omissionPresentation && cursorEqual;
	const omissionSgr = omissionSgrEqual(omissionProjection, piOmissionProjection);
	const omissionProjectionEqual = omissionText && omissionPresentation && cursorEqual;
	const pikeText = JSON.stringify(pikeProjection.snapshots);
	const piText = JSON.stringify(piProjection.snapshots);
	const pikeObserved = (scenario.omissionPatterns ?? []).some((pattern) => pikeText.includes(pattern));
	const piObserved = (scenario.omissionPatterns ?? []).some((pattern) => piText.includes(pattern));
	const omissionOnly = isIntentionalSubsetOmission(scenario, omissionProjectionEqual, pikeObserved, piObserved, cursorEqual);
	return {
		projection: {
			workspaceProfileEqual: workspaceEqual,
			visibleCellTextEqual: visibleEqual,
			visibleCellPresentationEqual: visiblePresentationEqual,
			scrollbackStructureEqual: scrollbackEqual,
			scrollbackCellPresentationEqual: scrollbackPresentationEqual,
			resolvedCellPresentationEqual: presentationEqual,
			cursorEqual,
			sgrStructureEqual: sgrEqual,
			omissionTextEqual: omissionText,
			omissionCellPresentationEqual: omissionPresentation,
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
	const piCheckoutPath = resolvePiBaseline().checkout;
	const piEntry = path.join(piCheckoutPath, "packages/coding-agent/src/cli.ts");
	const quote = (part: string) => `'${part.replaceAll("'", "'\\''")}'`;
	const piCommand = [resolveTsxRunner(piCheckoutPath), piEntry, ...(process.env.CCH_DIFFERENTIAL_LIVE_PI_ARGS?.split(" ") ?? [])].map(quote).join(" ");
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
	const base = report.captureBase as Record<string, unknown> | undefined;
	return {
		schema: report.schema,
		// The capture's repository HEAD is recorded provenance, but it must not
		// participate in metadata equality: any later, behaviour-preserving
		// commit would otherwise invalidate the checked-in evidence. The Pike
		// side of the comparison is guarded by the per-scenario stable digests,
		// and the required-fix set stays enforced at capture time and here.
		captureBase: base === undefined ? undefined : { requiredFixes: base.requiredFixes },
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
	const allowedClassifications = new Set<TriageClassification>([
		"match",
		"product-defect",
		"intentional-subset-omission",
		"environment-configuration",
		"harness-projection-defect",
		"intentional-divergence",
		"capture-timing-artifact",
		"pi-only-deferred-capability",
	]);
	if (!report.captureBase || typeof report.captureBase !== "object") {
		throw new Error("differential report is missing capture base provenance");
	}
	for (const item of report.scenarios as Record<string, unknown>[]) {
		const triage = item.triage as Record<string, unknown> | undefined;
		const id = String(item.id);
		if (!triage || !allowedClassifications.has(String(triage.classification) as TriageClassification)) {
			throw new Error(`differential report scenario ${id} has no valid triage classification`);
		}
		if (item.classification !== triage.classification) throw new Error(`differential report scenario ${id} has inconsistent classifications`);
		requireStringArray(triage.evidence, `differential report triage evidence for ${id}`);
		if ((triage.evidence as string[]).length === 0) throw new Error(`differential report scenario ${id} has no triage evidence`);
		if (typeof triage.rationale !== "string" || triage.rationale.length === 0) throw new Error(`differential report scenario ${id} has no triage rationale`);
		requireStringArray(item.inputs, `differential report inputs for ${id}`);
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
				const cursor = snapshot.cursor as Record<string, unknown> | undefined;
				if (!cursor || typeof cursor.column !== "number" || typeof cursor.row !== "number") {
					throw new Error(`differential report is missing cursor coordinates for ${runtime}/${String(item.id)}`);
				}
				if (typeof snapshot.cursorVisible !== "boolean") {
					throw new Error(`differential report is missing cursor visibility for ${runtime}/${String(item.id)}`);
				}
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

/// The verification gate shared by `--verify` (fresh captures) and
/// `--verify-replay` (a stored report): report metadata and structure, the
/// legacy stored-evidence digests on both reports, and the resolved-evidence
/// historical comparison. Any unrecognized delta fails closed.
function verifyReports(actual: Record<string, unknown>, expected: Record<string, unknown>, repositoryRoot: string): number {
	if (JSON.stringify(reportMetadata(actual)) !== JSON.stringify(reportMetadata(expected))) {
		throw new Error("Native TUI differential report metadata changed; inspect the checked-in evidence.");
	}
	validateReportCaptures(actual);
	validateReportCaptures(expected);
	validateThemeParity(actual);
	validateThemeParity(expected);
	// The legacy stored-evidence digests guard both reports: any missing or
	// tampered evidence (cells, styles, cursor, SGR stream, or digest) fails
	// here, byte-for-byte, before the live comparison runs.
	validateStableDigests(actual, repositoryRoot);
	validateStableDigests(expected, repositoryRoot);
	verifyResolvedEvidenceAgainstHistory(actual, expected, repositoryRoot);
	const count = (actual.scenarios as unknown[]).length;
	console.log(`verified ${count} Native TUI differential scenarios against ${reportPath}; stored digests, resolved evidence, and triage records are unchanged`);
	return 0;
}

/// `--verify-replay <report.json>` re-runs the verification gate against a
/// previously recorded report instead of a fresh capture, so a stored red run
/// can be triaged offline without recapturing both runtimes.
function runVerifyReplay(actualReportPath: string): number {
	const repositoryRoot = repositoryRootFromEnvironment();
	if (!existsSync(reportPath)) throw new Error(`checked-in report is missing: ${reportPath}; run with --write`);
	if (!existsSync(actualReportPath)) throw new Error(`--verify-replay report is missing: ${actualReportPath}`);
	const expected = JSON.parse(readFileSync(reportPath, "utf8")) as Record<string, unknown>;
	const actual = JSON.parse(readFileSync(actualReportPath, "utf8")) as Record<string, unknown>;
	console.log(`replaying stored differential report ${actualReportPath} against ${reportPath}`);
	return verifyReports(actual, expected, repositoryRoot);
}

function adaptationRulesFor(scenario: Pick<Scenario, "id">): readonly VersionedExpectationAdaptation[] {
	return versionedExpectationAdaptations[scenario.id] ?? [];
}

/// Replace a version-adapted historical row with the approved current row in a
/// stored capture. The historical anchor must match exactly once in the
/// snapshot's visible text and once in its styled projection, with the
/// approved canonical style, and the historical text must not linger in the
/// scrollback; anything else fails closed so an adaptation can never widen
/// silently.
function adaptVersionedExpectations(capture: Record<string, unknown>, scenario: Pick<Scenario, "id">): Record<string, unknown> {
	const rules = adaptationRulesFor(scenario).filter((rule) => rule.runtime === capture.runtime);
	if (rules.length === 0) return capture;
	const adapted = JSON.parse(JSON.stringify(capture)) as Record<string, unknown>;
	for (const rule of rules) {
		const label = `${scenario.id}/${rule.runtime} snapshot ${rule.snapshot} (${rule.authority}, adaptation v${expectationAdaptationVersion})`;
		const snapshots = adapted.snapshots as Record<string, unknown>[];
		const snapshot = snapshots[rule.snapshot];
		if (snapshot === undefined) throw new Error(`versioned expectation for ${label} is missing from the checked-in evidence`);
		const visible = snapshot.visible as string[];
		const visibleMatches = visible.filter((line) => line === rule.historicalText).length;
		if (visibleMatches !== 1) {
			throw new Error(`versioned expectation for ${label} requires exactly one historical row '${rule.historicalText}'; found ${visibleMatches}`);
		}
		snapshot.visible = visible.map((line) => (line === rule.historicalText ? rule.currentText : line));
		const styledVisible = snapshot.styledVisible as unknown[];
		const styledMatches = styledVisible.filter((row) => styledRowText(row) === rule.historicalText);
		if (styledMatches.length !== 1) {
			throw new Error(`versioned expectation for ${label} requires exactly one styled historical row; found ${styledMatches.length}`);
		}
		const runs = styledMatches[0] as Array<Record<string, unknown>>;
		const requiredStyle = canonicalStyledStyle({ foreground: rule.requiredForeground });
		if (runs.length !== 1 || JSON.stringify(runs[0]?.style) !== JSON.stringify(requiredStyle)) {
			throw new Error(`versioned expectation for ${label} requires the historical styled row to carry the exact '${rule.requiredForeground}' canonical style`);
		}
		runs[0] = { ...runs[0], text: rule.currentText };
		const scrollback = snapshot.scrollback as string[];
		if (scrollback.some((line) => line.includes(rule.historicalText))) {
			throw new Error(`versioned expectation for ${label} found the historical text in the scrollback; the adaptation would leave stale evidence`);
		}
		const styledScrollback = snapshot.styledScrollback as unknown[];
		if (styledScrollback.some((row) => styledRowText(row).includes(rule.historicalText))) {
			throw new Error(`versioned expectation for ${label} found the historical text in the styled scrollback; the adaptation would leave stale evidence`);
		}
	}
	return adapted;
}

/// Independent current-side assertions for a scenario with versioned
/// expectations: the current capture must render the approved notice row with
/// the approved exact canonical style, and must not resurrect any retired
/// literal anywhere. Assertions run on the normalized capture, so they see
/// exactly what the comparison sees.
function assertVersionedCurrentRequirements(capture: Record<string, unknown>, scenario: Scenario, repositoryRoot: string): void {
	const rules = adaptationRulesFor(scenario).filter((rule) => rule.runtime === capture.runtime);
	if (rules.length === 0) return;
	const normalized = normalizeCaptureForComparison(capture, scenario, repositoryRoot);
	for (const rule of rules) {
		const snapshots = normalized.snapshots as Record<string, unknown>[];
		const snapshot = snapshots[rule.snapshot];
		if (snapshot === undefined) throw new Error(`current ${scenario.id}/${rule.runtime} capture is missing snapshot ${rule.snapshot}; the approved notice cannot be verified`);
		const visible = snapshot.visible as string[];
		const noticeRows = visible.filter((line) => line === rule.currentText).length;
		if (noticeRows !== 1) {
			throw new Error(`current ${scenario.id}/${rule.runtime} snapshot ${rule.snapshot} must render exactly one '${rule.currentText}' row (${rule.authority}); found ${noticeRows}`);
		}
		const styledVisible = snapshot.styledVisible as unknown[];
		const styledNotice = styledVisible.filter((row) => styledRowText(row) === rule.currentText);
		if (styledNotice.length !== 1) {
			throw new Error(`current ${scenario.id}/${rule.runtime} snapshot ${rule.snapshot} must style the approved notice as one styled row; found ${styledNotice.length}`);
		}
		const runs = styledNotice[0] as Array<Record<string, unknown>>;
		const requiredStyle = canonicalStyledStyle({ foreground: rule.requiredForeground });
		if (runs.length !== 1 || JSON.stringify(runs[0]?.style) !== JSON.stringify(requiredStyle)) {
			throw new Error(`current ${scenario.id}/${rule.runtime} gateway notice style changed; expected the exact '${rule.requiredForeground}' canonical style (${rule.authority})`);
		}
		for (const current of snapshots) {
			const texts = [
				...(current.visible as string[]),
				...(current.scrollback as string[]),
				...(current.styledVisible as unknown[]).map(styledRowText),
				...(current.styledScrollback as unknown[]).map(styledRowText),
			].join("\n");
			for (const retired of rule.retiredLiterals) {
				if (texts.includes(retired)) {
					throw new Error(`current ${scenario.id}/${rule.runtime} capture resurrected retired literal '${retired}' (${rule.authority})`);
				}
			}
		}
	}
}

/// The resolved evidence projection: the comparison authority the report
/// declares (structuralProjection) applied to one capture — profile-projected
/// visible/scrollback cell text, styled cells resolved through the capture's
/// own theme palette, cursor state, and input identities. Raw ANSI and the
/// ordered SGR token stream stay diagnostic-only and are excluded, so
/// cumulative repaint cadence never decides the historical comparison.
function resolvedEvidenceProjection(capture: Record<string, unknown>, scenario: Scenario, repositoryRoot: string): Record<string, unknown> {
	const projection: Record<string, unknown> = {
		...stableProjectionForComparison(capture, scenario, repositoryRoot),
		themeColors: normalizeThemeEvidence(capture.theme, `resolved/${String(capture.runtime)}/${scenario.id}`).colors,
	};
	return omitProjectionLines(profileProjectionForComparison(projection, scenario), scenario.omissionPatterns ?? []);
}

function resolvedEvidenceEqual(left: Record<string, unknown>, right: Record<string, unknown>): boolean {
	const identity = (projection: Record<string, unknown>) => ({
		runtime: projection.runtime,
		scenario: projection.scenario,
		width: projection.width,
		workspace: projection.workspace,
		inputs: projection.inputs,
	});
	if (JSON.stringify(identity(left)) !== JSON.stringify(identity(right))) return false;
	const leftSnapshots = left.snapshots as Record<string, unknown>[];
	const rightSnapshots = right.snapshots as Record<string, unknown>[];
	if (leftSnapshots.length !== rightSnapshots.length) return false;
	const cells = (snapshot: Record<string, unknown>) => ({
		visible: snapshot.visible,
		scrollback: snapshot.scrollback,
		cursor: snapshot.cursor,
		cursorVisible: snapshot.cursorVisible,
	});
	for (let index = 0; index < leftSnapshots.length; index += 1) {
		if (JSON.stringify(cells(leftSnapshots[index])) !== JSON.stringify(cells(rightSnapshots[index]))) return false;
	}
	return styledPresentationEqual(left, right);
}

/// The projection fields the cross-report gate compares. The two raw-SGR
/// equality fields stay recorded evidence (the stored SGR stream remains
/// byte-guarded by the legacy stable digests), but raw SGR cadence is
/// diagnostic-only under the declared contract, so cumulative repaint
/// differences never decide the live comparison.
function gateComparisonProjection(projection: Record<string, unknown>): Record<string, unknown> {
	const { sgrStructureEqual, omissionSgrStructureEqual, ...gateFields } = projection;
	return gateFields;
}

function assertScenarioOutputsEqual(comparison: Record<string, unknown>, item: Record<string, unknown>, context: string): void {
	const id = String(item.id);
	if (JSON.stringify(gateComparisonProjection(comparison.projection as Record<string, unknown>)) !== JSON.stringify(gateComparisonProjection(item.projection as Record<string, unknown>))) {
		throw new Error(`${context} for ${id}: projection no longer matches`);
	}
	for (const field of ["profileProjection", "omissionEvidence"] as const) {
		if (JSON.stringify(comparison[field]) !== JSON.stringify(item[field])) {
			throw new Error(`${context} for ${id}: ${field} no longer matches`);
		}
	}
	if (comparison.classification !== item.observedClassification) {
		throw new Error(`${context} for ${id}: observed classification ${String(comparison.classification)} does not match the recorded ${String(item.observedClassification)}`);
	}
}

/// One scenario of the live regression gate. The curated triage record is
/// retained exactly; the checked-in evidence must reproduce its recorded
/// comparison under the current harness logic; the versioned expectations
/// bridge it to the current product; and the resolved evidence must match per
/// runtime under the declared authority. Any other delta fails closed.
function verifyScenarioAgainstHistory(
	actualItem: Record<string, unknown>,
	expectedItem: Record<string, unknown>,
	scenario: Scenario,
	repositoryRoot: string,
): void {
	if (String(actualItem.id) !== String(expectedItem.id)) throw new Error("differential scenario identity mismatch between the live and checked-in reports");
	if (JSON.stringify(actualItem.classification) !== JSON.stringify(expectedItem.classification)) {
		throw new Error(`differential scenario ${scenario.id} triage classification changed; review the delta instead of re-triaging silently`);
	}
	if (JSON.stringify(actualItem.triage) !== JSON.stringify(expectedItem.triage)) {
		throw new Error(`differential scenario ${scenario.id} triage record changed; review the delta instead of re-triaging silently`);
	}
	const expectedCaptures = expectedItem.captures as Record<string, Record<string, unknown>>;
	const actualCaptures = actualItem.captures as Record<string, Record<string, unknown>>;
	// The stored evidence must still reproduce its recorded comparison under the
	// current harness logic; a harness drift fails closed here.
	assertScenarioOutputsEqual(compare(expectedCaptures.pike, expectedCaptures.pi, scenario, repositoryRoot), expectedItem, "checked-in differential evidence does not reproduce its recorded comparison");
	// The current capture must independently satisfy every versioned expectation
	// before the adapted comparison runs, so a missing notice, a wrong style, or
	// a resurrected retired literal names its own failure.
	assertVersionedCurrentRequirements(actualCaptures.pike, scenario, repositoryRoot);
	assertVersionedCurrentRequirements(actualCaptures.pi, scenario, repositoryRoot);
	// Adapt only the declared, versioned expectations; everything else in the
	// stored evidence still compares exactly.
	const adaptedExpected = {
		pike: adaptVersionedExpectations(expectedCaptures.pike, scenario),
		pi: adaptVersionedExpectations(expectedCaptures.pi, scenario),
	};
	assertScenarioOutputsEqual(compare(adaptedExpected.pike, adaptedExpected.pi, scenario, repositoryRoot), actualItem, "live differential comparison diverged from the adapted checked-in evidence");
	for (const runtime of ["pike", "pi"] as const) {
		const expectedResolved = resolvedEvidenceProjection(adaptedExpected[runtime], scenario, repositoryRoot);
		const actualResolved = resolvedEvidenceProjection(actualCaptures[runtime], scenario, repositoryRoot);
		if (!resolvedEvidenceEqual(expectedResolved, actualResolved)) {
			throw new Error(`Native TUI differential resolved evidence changed for ${scenario.id}/${runtime}; the settled cells, styles, or cursor diverged from the checked-in evidence`);
		}
	}
}

/// The live current-vs-historical comparison: every scenario in the checked-in
/// report must be present in the live report and match under
/// verifyScenarioAgainstHistory. Unrecognized or new deltas fail closed.
function verifyResolvedEvidenceAgainstHistory(actual: Record<string, unknown>, expected: Record<string, unknown>, repositoryRoot: string): void {
	const actualScenarios = actual.scenarios as Record<string, unknown>[];
	const expectedScenarios = expected.scenarios as Record<string, unknown>[];
	if (actualScenarios.length !== expectedScenarios.length) {
		throw new Error("differential report scenario count changed between the live and checked-in reports");
	}
	const actualById = new Map(actualScenarios.map((item) => [String(item.id), item]));
	const scenarioById = new Map(scenarios.map((scenario) => [scenario.id, scenario]));
	for (const expectedItem of expectedScenarios) {
		const id = String(expectedItem.id);
		const scenario = scenarioById.get(id);
		const actualItem = actualById.get(id);
		if (!scenario) throw new Error(`differential report contains unknown scenario ${id}`);
		if (!actualItem) throw new Error(`differential report is missing scenario ${id}`);
		verifyScenarioAgainstHistory(actualItem, expectedItem, scenario, repositoryRoot);
	}
}

async function runParent(): Promise<number> {
	const scenarioIndex = process.argv.indexOf("--scenario");
	const scenarioFilter = scenarioIndex >= 0 ? process.argv[scenarioIndex + 1] : undefined;
	if (scenarioIndex >= 0 && scenarioFilter === undefined) {
		throw new Error("--scenario requires a differential scenario id");
	}
	if (scenarioFilter !== undefined && process.argv.includes("--write")) {
		throw new Error("--scenario is a debug capture mode and cannot be combined with --write");
	}
	const baseline = resolvePiBaseline();
	const checkout = baseline.checkout;
	const repositoryRoot = repositoryRootFromEnvironment();
	const captureBase = validateCaptureBase(repositoryRoot);
	const temporaryDirectory = path.join(differentialDir, `.verify-${process.pid}`);
	rmSync(temporaryDirectory, { recursive: true, force: true });
	mkdirSync(temporaryDirectory, { recursive: true });
	const report: Record<string, unknown> = {
		schema: 3,
		captureBase,
		baseline: {
			piCommit: baseline.commit,
			artifact: `${packageName}@${baseline.version}`,
		},
		profile,
		structuralProjection: {
			cellText: "trimmed visible cells with fixture paths, model ids, model prose, and the deterministic workspace/branch projection applied",
			ansi: "raw ANSI retained as captured evidence only; verification compares resolved cell presentation, never raw ANSI byte parity",
			sgr: "ordered SGR tokens are retained as diagnostic evidence; resolved cell style is the comparison authority, compared as the semantic theme role each color resolves to on its own runtime palette (issue #797), while a color no single semantic role resolves stays at its exact truecolor value",
			screenshot: "themeParity.renderedScreenshots retains the rendered terminal cell rows as text screenshots",
			scrollback: "scrollback cell rows are compared separately from the visible viewport; the profileProjection comparison is length-compensated, discounting a height surplus only up to the profile rows one runtime removed from its viewport, and records the per-barrier discount as scrollbackLengthCompensation",
			profile: "resolved projection removes exact pi runtime-identity/startup-documentation rows and isolates provider usage for user-message, status-footer, tool-result, and scrollback",
			omission: "declared pi-only rows are removed only with explicit patterns; the resolved styled-cell projection decides whether the remaining delta is intentional",
			verification: "--verify checks reproducible report digests and one explicit triage classification per scenario; product defects remain visible in child issues",
		},
		scenarios: [] as Record<string, unknown>[],
	};
	try {
		const activeScenarios = scenarioFilter === undefined
			? scenarios
			: scenarios.filter((scenario) => scenario.id === scenarioFilter);
		if (scenarioFilter !== undefined && activeScenarios.length === 0) {
			throw new Error(
				`--scenario '${scenarioFilter}' does not name a differential scenario (known: ${scenarios.map((scenario) => scenario.id).join(", ")})`,
			);
		}
		for (const scenario of activeScenarios) {
			const pikeOutput = path.join(temporaryDirectory, `${scenario.id}.pike.json`);
			const piOutput = path.join(temporaryDirectory, `${scenario.id}.pi.json`);
			const pike = runPike(scenario, pikeOutput);
			const pi = await runPi(scenario, piOutput);
			const comparison = compare(pike, pi, scenario, repositoryRoot);
			// The observed comparison is not the final classification: the
			// curated triage decides whether a delta is a defect, an accepted
			// omission, or a match. Print both so a coarse observed label cannot
			// read as an unreviewed regression.
			console.log(
				`${scenario.id}: observed ${comparison.classification} | triage ${triageByScenario[scenario.id].classification}`,
			);
			if (scenarioFilter !== undefined) continue;
			const pikeCapture = normalizeCapture(pike, scenario, repositoryRoot);
			const piCapture = normalizeCapture(pi, scenario, repositoryRoot);
			(report.scenarios as Record<string, unknown>[]).push({
				id: scenario.id,
				width: scenario.width,
				inputs: scenario.inputs,
				resize: scenario.resize,
				...comparison,
				classification: triageByScenario[scenario.id].classification,
				observedClassification: comparison.classification,
				triage: triageByScenario[scenario.id],
				captures: { pike: pikeCapture, pi: piCapture },
				stableDigest: digest({
					pike: stableProjection(pikeCapture, scenario, repositoryRoot),
					pi: stableProjection(piCapture, scenario, repositoryRoot),
				}),
			});
		}
		if (scenarioFilter !== undefined) {
			console.log(`--scenario ${scenarioFilter}: debug capture only; no report was written or verified`);
			return 0;
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
	return verifyReports(report, expected, repositoryRoot);
}

function inputStateBarrier(scenario: Scenario, inputIndex: number): {
	present?: string;
	presentRow?: { label: string; value: string };
	absent?: string;
} {
	if (scenario.id === "model-selector") return inputIndex === 0 ? { present: "Only showing models" } : { present: "Model:" };
	if (scenario.id === "thinking-selector") {
		return inputIndex === 0 ? { present: "Set thinking level" } : inputIndex === 1 ? { present: "No reasoning" } : { present: "Thinking level:" };
	}
	if (scenario.id === "settings-selector") {
		if (inputIndex === 0) return { present: "Open settings menu" };
		if (inputIndex === 1) return { present: "Type to search" };
		if (inputIndex === 2) return { presentRow: { label: "Show hardware cursor", value: "false" } };
		if (inputIndex === 3) return { presentRow: { label: "Show hardware cursor", value: "true" } };
		if (inputIndex === 4) return { present: "Type to search" };
		if (inputIndex === 5) return { presentRow: { label: "Terminal progress", value: "false" } };
		if (inputIndex === 6) return { presentRow: { label: "Terminal progress", value: "true" } };
		if (inputIndex === 7) return { present: "Type to search" };
		return { absent: "Type to search" };
	}
	if (["editor-long", "editor-cjk", "editor-token"].includes(scenario.id)) {
		const expected = scenario.inputs[0].slice(0, 20);
		return inputIndex === 0 ? { present: expected } : { absent: expected };
	}
	return {};
}

async function runPiChild(): Promise<number> {
	const scenario = scenarios.find((item) => item.id === process.env.CCH_DIFFERENTIAL_SCENARIO);
	const output = process.env.CCH_DIFFERENTIAL_OUTPUT;
	if (!scenario || !output) throw new Error("pi child requires CCH_DIFFERENTIAL_SCENARIO and CCH_DIFFERENTIAL_OUTPUT");
	const checkout = piCheckout();
	// The frozen, read-only pi checkout omits this gitignored generated catalog
	// file. The scenarios use only explicitly supplied faux models, so stub the
	// import in memory without writing generated data into the authority checkout.
	registerHooks({
		resolve(specifier, context, nextResolve) {
			if (specifier === "./data/azure.json" && context.parentURL?.includes("/packages/ai/src/providers/")) {
				return { url: new URL(specifier, context.parentURL).href, shortCircuit: true, format: "json" };
			}
			return nextResolve(specifier, context);
		},
		load(url, context, nextLoad) {
			if (url.endsWith("/providers/data/azure.json")) return { format: "json", source: "{}", shortCircuit: true };
			return nextLoad(url, context);
		},
	});
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
		cursorVisible = true;

		override write(data: string): void {
			this.ansi += data;
			for (const match of data.matchAll(/\x1b\[\?25([hl])/g)) this.cursorVisible = match[1] === "h";
			super.write(data);
		}

		// pi's Terminal contract routes cursor visibility through these two
		// methods, and the product calls them directly (never through write).
		// Tracking the write() stream alone misses every hide/show, which left
		// the mock reporting a permanently visible cursor.
		override hideCursor(): void {
			this.cursorVisible = false;
			super.hideCursor();
		}

		override showCursor(): void {
			this.cursorVisible = true;
			super.showCursor();
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
			await terminal.waitForRender();
			snapshots.push(await capture(terminal, workspace, ansiOffset));
			ansiOffset = terminal.ansi.length;
		}
	}
	let responseIndex = 0;
	for (let inputIndex = 0; inputIndex < scenario.inputs.length; inputIndex += 1) {
		const input = scenario.inputs[inputIndex];
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
		} else {
			await waitForTerminalAnsi(terminal, inputOffset);
			const barrier = inputStateBarrier(scenario, inputIndex);
			if (barrier.present !== undefined) await waitForTerminalText(terminal, barrier.present);
			if (barrier.presentRow !== undefined) {
				await waitForSettingsRow(terminal, barrier.presentRow);
			}
			if (barrier.absent !== undefined) await waitForTerminalTextAbsent(terminal, barrier.absent);
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

type PiHeadlessCell = {
	getWidth(): number;
	getChars(): string;
	isBold(): number;
	isDim(): number;
	isItalic(): number;
	isUnderline(): number;
	isBlink(): number;
	isInverse(): number;
	isInvisible(): number;
	isStrikethrough(): number;
	isFgRGB(): boolean;
	isFgPalette(): boolean;
	isBgRGB(): boolean;
	isBgPalette(): boolean;
	getFgColor(): number;
	getBgColor(): number;
	isAttributeDefault(): boolean;
};

type PiHeadlessLine = {
	length: number;
	getCell(column: number): PiHeadlessCell | undefined;
};

type PiHeadlessState = {
	rows: number;
	buffer: { active: { length: number; viewportY: number; getLine(index: number): PiHeadlessLine | undefined } };
};

type PiRecordingTerminal = {
	ansi: string;
	cursorVisible: boolean;
	flush(): Promise<void>;
	getViewport(): string[];
	getScrollBuffer(): string[];
	getCursorPosition(): { x: number; y: number };
};

const captureBase = Object.freeze({
	// The capture records the actual repository HEAD it ran at
	// (`validateCaptureBase`) and fails unless every listed fix commit is an
	// ancestor of that HEAD, so a report cannot be produced from a tree that
	// predates the fixes it claims to include.
	includedFixes: {
		issue811: "d467587cb5d00611aff3a642851c710069f11038",
		issue1024: "2664ecc15bed9c1ec6ffbe646edfd9bb8054c32b",
		issue1039: "82ea384f5a04b0d747b7cb2dee9be8c580e7f821",
	},
});

function validateCaptureBase(repositoryRoot: string): Record<string, unknown> {
	const head = execFileSync("git", ["rev-parse", "HEAD"], { cwd: repositoryRoot, encoding: "utf8" }).trim();
	for (const [name, commit] of Object.entries(captureBase.includedFixes)) {
		const result = spawnSync("git", ["merge-base", "--is-ancestor", commit, head], { cwd: repositoryRoot });
		if (result.status !== 0) throw new Error(`capture requires ${name} fix ${commit} to be an ancestor of HEAD ${head}`);
	}
	return { sha: head, requiredFixes: captureBase.includedFixes };
}

function piCellColor(cell: PiHeadlessCell, foreground: boolean): string {
	if (foreground ? cell.isFgRGB() : cell.isBgRGB()) {
		const value = (foreground ? cell.getFgColor() : cell.getBgColor()) & 0xffffff;
		return `#${value.toString(16).padStart(6, "0")}`;
	}
	if (foreground ? cell.isFgPalette() : cell.isBgPalette()) {
		return `palette:${foreground ? cell.getFgColor() : cell.getBgColor()}`;
	}
	return "";
}

function piCellStyle(cell: PiHeadlessCell): Record<string, unknown> {
	return {
		bold: cell.isBold() !== 0,
		dim: cell.isDim() !== 0,
		italic: cell.isItalic() !== 0,
		underline: cell.isUnderline() !== 0,
		blink: cell.isBlink() !== 0,
		inverse: cell.isInverse() !== 0,
		hidden: cell.isInvisible() !== 0,
		strikethrough: cell.isStrikethrough() !== 0,
		foreground: piCellColor(cell, true),
		background: piCellColor(cell, false),
	};
}

function piHeadlessState(terminal: PiRecordingTerminal): PiHeadlessState {
	return (terminal as unknown as { xterm: PiHeadlessState }).xterm;
}

function piStyledRows(terminal: PiRecordingTerminal, start: number, count: number): Array<Array<Record<string, unknown>>> {
	const state = piHeadlessState(terminal);
	const rows: Array<Array<Record<string, unknown>>> = [];
	for (let rowIndex = start; rowIndex < start + count; rowIndex += 1) {
		const line = state.buffer.active.getLine(rowIndex);
		const runs: Array<Record<string, unknown>> = [];
		if (line === undefined) {
			rows.push(runs);
			continue;
		}
		let occupied = 0;
		for (let column = 0; column < line.length; column += 1) {
			const cell = line.getCell(column);
			if (cell !== undefined && (cell.getChars() !== "" || cell.getWidth() === 0 || !cell.isAttributeDefault())) {
				occupied = column + 1;
			}
		}
		let text = "";
		let runStyle: Record<string, unknown> | undefined;
		for (let column = 0; column < occupied; column += 1) {
			const cell = line.getCell(column);
			if (cell === undefined || cell.getWidth() === 0) continue;
			const style = piCellStyle(cell);
			if (text !== "" && JSON.stringify(style) !== JSON.stringify(runStyle)) {
				runs.push({ text, style: runStyle });
				text = "";
			}
			if (text === "") runStyle = style;
			text += cell.getChars() === "" ? " " : cell.getChars();
		}
		if (text !== "") runs.push({ text, style: runStyle });
		rows.push(runs);
	}
	return rows;
}

async function capture(terminal: PiRecordingTerminal, workspace: string, ansiOffset: number): Promise<Record<string, unknown>> {
	await terminal.flush();
	const visible = terminal.getViewport();
	const buffer = terminal.getScrollBuffer();
	const scrollbackCount = Math.max(0, buffer.length - visible.length);
	const cursor = terminal.getCursorPosition();
	return {
		visible,
		scrollback: buffer.slice(0, scrollbackCount),
		styledVisible: piStyledRows(terminal, piHeadlessState(terminal).buffer.active.viewportY, visible.length),
		styledScrollback: piStyledRows(terminal, 0, scrollbackCount),
		cursor: { column: cursor.x, row: cursor.y },
		cursorVisible: terminal.cursorVisible,
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

async function waitForTerminalRowText(
	terminal: { flush(): Promise<void>; getViewport(): string[] },
	expected: { label: string; value: string },
	minimumViewportRows = 1,
): Promise<void> {
	const deadline = Date.now() + 3_000;
	while (Date.now() < deadline) {
		await terminal.flush();
		const viewport = terminal.getViewport();
		if (viewport.length >= minimumViewportRows &&
			viewport.some((line) => line.includes(expected.label) && line.includes(expected.value))) return;
		await delay(20);
	}
	throw new Error(
		`pi differential scenario did not render ${expected.label} with value ${expected.value}\n${terminal.getViewport().join("\n")}`,
	);
}

async function waitForSettingsRow(
	terminal: { flush(): Promise<void>; getViewport(): string[] },
	expected: { label: string; value: string },
): Promise<void> {
	const deadline = Date.now() + 3_000;
	while (Date.now() < deadline) {
		await terminal.flush();
		const viewport = terminal.getViewport();
		if (viewport.length >= profile.viewportRows &&
			viewport.some((line) => settingRowValue(line, expected.label) === expected.value)) return;
		await delay(20);
	}
	throw new Error(`pi differential scenario did not render ${expected.label}=${expected.value}\n${terminal.getViewport().join("\n")}`);
}

function settingRowValue(row: string, label: string): string | undefined {
	const labelPosition = row.indexOf(label);
	if (labelPosition < 0) return undefined;
	return row.slice(labelPosition + label.length).trim().split(/\s+/)[0];
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
		verifyHistoricalComparisonPolicy();
		verifyThemeParityPolicy();
		verifyAnsiBoundary();
		verifyStyledRolePresentation();
		const replayIndex = process.argv.indexOf("--verify-replay");
		if (replayIndex >= 0) {
			const replayArgument = process.argv[replayIndex + 1];
			if (replayArgument === undefined || replayArgument.startsWith("--")) throw new Error("--verify-replay requires a stored report path");
			if (process.argv.includes("--write") || process.argv.includes("--scenario")) {
				throw new Error("--verify-replay cannot be combined with --write or --scenario");
			}
			return runVerifyReplay(path.resolve(replayArgument));
		}
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
