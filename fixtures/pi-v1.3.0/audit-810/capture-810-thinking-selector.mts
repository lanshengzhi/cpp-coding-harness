#!/usr/bin/env tsx
/**
 * Capture the matched-map thinking-selector viewport from frozen pi v1.3.0 (#810).
 *
 * The faux reasoning model omits thinkingLevelMap, matching the C++ differential fixture and
 * pi's native five-level default. The bundle is intentionally unregistered; the serial merger
 * owns the shared baselines.json pin.
 *
 * Usage:
 *   NODE_OPTIONS=--conditions=source PI_CHECKOUT=<private frozen checkout> PI_BASELINE=pi-v1.3.0 \
 *     <checkout>/node_modules/.bin/tsx fixtures/pi-v1.3.0/audit-810/capture-810-thinking-selector.mts
 */

import { execFileSync, spawnSync } from "node:child_process";
import { createHash } from "node:crypto";
import { mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { createRequire } from "node:module";
import { tmpdir } from "node:os";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const scriptPath = fileURLToPath(import.meta.url);
const familyDir = path.dirname(scriptPath);
const auditRoot = path.resolve(familyDir, "..");
const repositoryRoot = path.resolve(auditRoot, "../..");
const family = "audit-810";
const registryPath = path.join(auditRoot, "baselines.json");
const piCheckout = path.resolve(process.env.PI_CHECKOUT ?? path.resolve(repositoryRoot, "../pi"));
const nodeOptions = process.env.NODE_OPTIONS ?? "";
if (!nodeOptions.split(/\s+/).includes("--conditions=source")) {
	throw new Error("capture requires NODE_OPTIONS=--conditions=source");
}

if (process.env.TSX_TSCONFIG_PATH === undefined) {
	const tsx = path.join(piCheckout, "node_modules", ".bin", "tsx");
	const result = spawnSync(tsx, [scriptPath, ...process.argv.slice(2)], {
		stdio: "inherit",
		env: { ...process.env, NODE_OPTIONS: nodeOptions, TSX_TSCONFIG_PATH: path.join(piCheckout, "tsconfig.json") },
	});
	if (result.error) throw new Error(`cannot re-exec ${tsx}: ${result.error}`);
	process.exit(result.status ?? 1);
}

const registry = JSON.parse(readFileSync(registryPath, "utf8"));
if (registry.schema !== "pike/pi-v1.3.0-audit-baselines/1") throw new Error("unexpected baseline registry schema");
const baselineName = process.env.PI_BASELINE ?? registry.default;
const baseline = registry.baselines[baselineName];
if (!baseline) throw new Error(`unknown baseline ${JSON.stringify(baselineName)}`);
const requiredRevision = "71023aec1546d6d677a65669c5665c0927ce0dd0";
if (baseline.revision !== requiredRevision) throw new Error(`registered ${baselineName} revision changed: ${baseline.revision}`);
const head = execFileSync("git", ["-C", piCheckout, "rev-parse", "HEAD"], { encoding: "utf8" }).trim();
if (head !== requiredRevision) throw new Error(`pi checkout must be exactly ${requiredRevision}, found ${head}`);
if (execFileSync("git", ["-C", piCheckout, "status", "--porcelain"], { encoding: "utf8" }).trim()) {
	throw new Error("pi capture requires a clean checkout, including untracked files");
}
for (const [name, entry] of Object.entries<any>(registry.baselines)) {
	if (name !== baselineName && entry.bundles && family in entry.bundles) {
		throw new Error(`refusing to capture ${family}: it belongs to baseline ${JSON.stringify(name)}`);
	}
}
const glibc = (process.report.getReport() as any).header.glibcVersionRuntime;
if (process.platform !== "linux" || process.arch !== "x64" || !glibc) {
	throw new Error("audit capture requires Linux x86-64/glibc");
}

const requirePi = createRequire(path.join(piCheckout, "package.json"));
const tsxVersion = requirePi("tsx/package.json").version;
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
const source = (relative: string) => pathToFileURL(path.join(piCheckout, relative)).href;
const aiModels = await import(source("packages/ai/src/models.ts"));
const fauxReasoningModel = { id: "faux-1", reasoning: true };
if (Object.hasOwn(fauxReasoningModel, "thinkingLevelMap")) throw new Error("faux model unexpectedly has thinkingLevelMap");
const supportedThinkingLevels = aiModels.getSupportedThinkingLevels(fauxReasoningModel) as string[];
const expectedLevels = ["off", "minimal", "low", "medium", "high"];
if (JSON.stringify(supportedThinkingLevels) !== JSON.stringify(expectedLevels)) {
	throw new Error(`pi's absent-map thinking levels changed: ${JSON.stringify(supportedThinkingLevels)}`);
}

const captureRoot = mkdtempSync(path.join(tmpdir(), "audit-810-thinking-"));
const workspace = path.join(captureRoot, "workspace");
const agentDirectory = path.join(captureRoot, "agent");
const outputPath = path.join(captureRoot, "capture.json");
mkdirSync(workspace, { recursive: true });
mkdirSync(path.join(workspace, ".pi"), { recursive: true });
mkdirSync(agentDirectory, { recursive: true });
const captureScript = "fixtures/pi-coding-agent/capture/native-tui-differential.mts";
const runner = path.join(repositoryRoot, captureScript);
const runnerStub = "fixtures/pi-coding-agent/capture/grok-mermaid-stub.mts";
const rootConfig = JSON.parse(readFileSync(path.join(piCheckout, "tsconfig.json"), "utf8")) as {
	compilerOptions?: { paths?: Record<string, string[]> };
};
const paths = { ...(rootConfig.compilerOptions?.paths ?? {}) };
paths["grok-mermaid"] = [path.join(repositoryRoot, runnerStub)];
const captureTsconfig = path.join(captureRoot, "tsconfig.json");
writeFileSync(captureTsconfig, `${JSON.stringify({
	compilerOptions: {
		target: "ES2022",
		module: "NodeNext",
		moduleResolution: "NodeNext",
		allowImportingTsExtensions: true,
		rewriteRelativeImportExtensions: true,
		esModuleInterop: true,
		skipLibCheck: true,
		types: ["node"],
		baseUrl: piCheckout,
		paths,
	},
}, null, 2)}\\n`);
const hex = (value: string) => Buffer.from(value, "utf8").toString("hex") || "-";
const tsx = path.join(piCheckout, "node_modules", ".bin", "tsx");
const child = spawnSync(tsx, ["--tsconfig", captureTsconfig, runner, "--runtime", "pi"], {
	cwd: repositoryRoot,
	encoding: "utf8",
	timeout: 120_000,
	env: {
		PATH: `${path.dirname(process.execPath)}:/usr/bin:/bin`,
		LANG: "C.UTF-8",
		LC_ALL: "C.UTF-8",
		TZ: "UTC",
		HOME: path.join(captureRoot, "home"),
		USERPROFILE: path.join(captureRoot, "home"),
		PI_OFFLINE: "1",
		PI_SKIP_VERSION_CHECK: "1",
		TERM: "xterm-256color",
		COLORTERM: "truecolor",
		COLORFGBG: "15;0",
		FORCE_COLOR: "3",
		NODE_OPTIONS: nodeOptions,
		PI_CHECKOUT: piCheckout,
		PI_BASELINE: baselineName,
		CCH_DIFFERENTIAL_SCENARIO: "thinking-selector",
		CCH_DIFFERENTIAL_WORKSPACE: workspace,
		CCH_DIFFERENTIAL_WIDTH: "100",
		CCH_DIFFERENTIAL_INPUTS: ["/thinking\r", "\r"].map(hex).join("|"),
		CCH_DIFFERENTIAL_OUTPUT: outputPath,
		CCH_DIFFERENTIAL_AGENT_DIRECTORY: agentDirectory,
		XDG_CONFIG_HOME: path.join(captureRoot, "xdg", "config"),
		PI_CODING_AGENT_DIR: agentDirectory,
	},
});
if (child.error) throw new Error(`pi Native TUI capture failed to start: ${child.error}`);
if (child.status !== 0) throw new Error(`pi Native TUI capture failed (${child.status}):\n${child.stdout}\n${child.stderr}`);
const report = JSON.parse(readFileSync(outputPath, "utf8"));
if (report.runtime !== "pi" || report.scenario !== "thinking-selector" || report.width !== 100) {
	throw new Error("pi Native TUI capture returned the wrong scenario");
}
const selectorSnapshot = report.snapshots[2];
if (!selectorSnapshot) throw new Error("pi Native TUI capture omitted the selector snapshot");
const visible = selectorSnapshot.visible as string[];
const searchIndex = visible.findIndex((row) => row.trim() === ">");
if (searchIndex < 0) throw new Error("pi selector search prompt was not captured");
const levelRows = visible.filter((row) => /(?:→ ✓\s+|^ {4})(off|minimal|low|medium|high)\s/.test(row));
if (levelRows.length !== expectedLevels.length || !levelRows[3].includes("· default")) {
	throw new Error(`pi selector viewport does not show the five expected levels: ${JSON.stringify(levelRows)}`);
}
const viewportRows = visible.slice(searchIndex, searchIndex + 2 + levelRows.length);
if (viewportRows[1].trim() !== "" || viewportRows[2].trim() !== levelRows[0].trim()) {
	throw new Error(`pi selector search spacer or first level moved: ${JSON.stringify(viewportRows)}`);
}

const sourceEndpoints = [
	"packages/ai/src/models.ts:getSupportedThinkingLevels",
	"packages/coding-agent/src/modes/interactive/components/thinking-selector.ts:ThinkingSelector",
	"packages/coding-agent/test/suite/harness.ts:createHarness",
	"packages/tui/test/virtual-terminal.ts:VirtualTerminal",
];
const scenarios = [{
	name: "thinking selector with the absent thinkingLevelMap at 100 columns",
	inputs: {
		model: { id: "faux-1", reasoning: true, thinkingLevelMap: "absent" },
		width: 100,
		keystrokes: ["/thinking", "\\r", "\\r"],
	},
	expected: {
		thinkingLevelMap: { present: false },
		supportedThinkingLevels,
		viewportRows,
		levelRows,
		defaultLevel: "medium",
	},
	normalizations: ["the private capture root is replaced with <deterministic-workspace>"],
	upstreamTest: { path: captureScript, scenario: "thinking-selector" },
}];
const artifactName = "thinking-selector.json";
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
const sourcePaths = [generatorPath, captureScript, runnerStub];
const sources = sourcePaths.map((relative) => ({
	path: relative,
	sha256: sha256(readFileSync(path.join(repositoryRoot, relative))),
}));
const manifest = {
	schema: "pike/pi-v1.3.0-audit-evidence/1",
	baseline: baselineName,
	revision: baseline.revision,
	ticket: "810",
	family,
	capturedAt,
	environment,
	generator: {
		path: generatorPath,
		sha256: sources[0].sha256,
		command: `NODE_OPTIONS=--conditions=source PI_CHECKOUT=${piCheckout} PI_BASELINE=${baselineName} ${tsx} ${generatorPath}`,
		sources,
	},
	artifacts: [{
		path: artifactName,
		family,
		bytes: artifactBytes.length,
		sha256: sha256(artifactBytes),
		sourceEndpoints,
		scenarios,
	}],
};

rmSync(captureRoot, { recursive: true, force: true });
mkdirSync(familyDir, { recursive: true });
writeFileSync(path.join(familyDir, artifactName), artifactBytes);
writeFileSync(path.join(familyDir, "manifest.json"), `${JSON.stringify(manifest, null, 2)}\n`);
console.log(`captured ${family} thinking-selector viewport for ${baselineName} (${baseline.revision})`);
