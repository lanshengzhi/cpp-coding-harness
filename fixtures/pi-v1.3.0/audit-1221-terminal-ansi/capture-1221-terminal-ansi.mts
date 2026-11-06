#!/usr/bin/env tsx
/**
 * Capture selected route batch B13 startup, stop and ANSI utility observations
 * for issue #1221 at the frozen pi v1.3.0 checkout.
 * Every value below comes from executing the frozen checkout's own modules
 * through their public seams; nothing is derived from C++.
 *
 * Usage (from the repository root):
 *   PI_CHECKOUT=<private frozen checkout> PI_BASELINE=pi-v1.3.0 \
 *     <checkout>/node_modules/.bin/tsx fixtures/pi-v1.3.0/audit-1221-terminal-ansi/capture-1221-terminal-ansi.mts
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
const family = "audit-1221-terminal-ansi";
const registryPath = path.join(auditRoot, "baselines.json");
const piCheckout = process.env.PI_CHECKOUT
	? path.resolve(process.env.PI_CHECKOUT)
	: path.resolve(repositoryRoot, "../pi");

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
// Terminal behavior must not inherit a host-specific status override or write log.
delete process.env.PI_PROGRAM_STATUS;
delete process.env.PI_TUI_WRITE_LOG;
const environment = {
	terminalOverrides: { PI_PROGRAM_STATUS: null, PI_TUI_WRITE_LOG: null },
	platform: process.platform,
	arch: process.arch,
	libc: "glibc",
	glibcVersion: glibc,
	node: process.version,
	tsx: tsxVersion,
};
const capturedAt = new Date().toISOString();
const sha256 = (bytes: Buffer | string) => createHash("sha256").update(bytes).digest("hex");

const source = (file: string) => pathToFileURL(path.join(piCheckout, "packages/tui/src", file)).href;
const utils = await import(source("utils.ts"));
const { ProcessTerminal } = await import(source("terminal.ts"));
const ansiInputs = [
  {name: "plain", text: "hello world", width: 5, start: 1, length: 3},
  {name: "sgr-boundary", text: "\x1b[31mred\x1b[39m blue", width: 5, start: 2, length: 4},
  {name: "wide-hyperlink", text: "\x1b]8;;u\x07a你b\x1b]8;;\x07", width: 3, start: 1, length: 2},
];
const ansi = ansiInputs.map(({name, ...inputs}) => ({name, inputs, expected: {
  visibleWidth: utils.visibleWidth(inputs.text), strip: utils.stripTerminalSequences(inputs.text),
  wrap: utils.wrapTextWithAnsi(inputs.text, inputs.width),
  truncate: utils.truncateToWidth(inputs.text, inputs.width),
  slice: utils.sliceByColumn(inputs.text, inputs.start, inputs.length, true),
}, normalizations: []}));
const terminalCases = [];
for (const [name, reply] of [["no-reply", ""], ["device-attributes", "\x1b[?62;4;52c"], ["kitty-flags", "\x1b[?1u"]]) {
  const writes: string[] = [];
  const inputs: string[] = [];
  let handler: ((value: string) => void) | undefined;
  const oldWrite = process.stdout.write;
  const oldOn = process.stdin.on;
  const terminal = new ProcessTerminal();
  try {
    process.stdout.write = ((chunk: string | Uint8Array) => { writes.push(String(chunk)); return true; }) as typeof process.stdout.write;
    process.stdin.on = ((event: string | symbol, listener: (...args: unknown[]) => void) => {
      if (event === "data") handler = listener as (value: string) => void;
      return process.stdin;
    }) as typeof process.stdin.on;
    terminal.start(value => inputs.push(value), () => {});
    const startup = [...writes];
    if (reply) handler?.(reply);
    const negotiation = writes.slice(startup.length);
    const kittyActive = terminal.kittyProtocolActive;
    const beforeStop = writes.length;
    terminal.stop();
    terminalCases.push({name, inputs: {reply}, expected: {startup, negotiation, kittyActive, stop: writes.slice(beforeStop), input: inputs}, normalizations: []});
  } finally {
    process.stdout.write = oldWrite;
    process.stdin.on = oldOn;
  }
}
const artifacts = [
  {path: "utils-ansi.json", sourceEndpoints: ["packages/tui/src/utils.ts:visibleWidth,stripTerminalSequences,wrapTextWithAnsi,truncateToWidth,sliceByColumn"], scenarios: ansi},
  {path: "terminal-start-stop.json", sourceEndpoints: ["packages/tui/src/terminal.ts:ProcessTerminal.start,ProcessTerminal.stop"], scenarios: terminalCases},
];

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
const scenarioSources: Array<{path: string; sha256: string}> = [];

const manifest = {
	schema: "pike/pi-v1.3.0-audit-evidence/1",
	baseline: baselineName,
	revision: baseline.revision,
	ticket: "1221",
	family,
	capturedAt,
	environment,
	generator: {
		path: generatorPath,
		sha256: sha256(readFileSync(scriptPath)),
		command: `PI_CHECKOUT=<pi-checkout> PI_BASELINE=${baselineName} <pi-checkout>/node_modules/.bin/tsx ${generatorPath}`,
		sources: [{ path: generatorPath, sha256: sha256(readFileSync(scriptPath)) }, ...scenarioSources],
	},
	artifacts: manifestArtifacts,
};

mkdirSync(familyDir, { recursive: true });
for (const [relative, bytes] of artifactBytes) writeFileSync(path.join(familyDir, relative), bytes);
writeFileSync(path.join(familyDir, "manifest.json"), `${JSON.stringify(manifest, null, 2)}\n`);

console.log(
	`captured ${family} (${manifestArtifacts.length} artifacts, ` +
		`${manifestArtifacts.reduce((total, item) => total + item.scenarios.length, 0)} scenarios) ` +
		`for ${baselineName} (${baseline.revision})`,
);
