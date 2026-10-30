#!/usr/bin/env tsx
/**
 * Capture the `audit-1106` tool-duration observations for the shared pi-v1.3.0 evidence registry.
 *
 * The capture imports frozen package entries, drives public `runToolCall`, and writes the
 * artifacts plus manifest only after the guards and observations succeed. It refuses a checkout whose HEAD is not the registered baseline
 * revision, a modified source tree, a non-Linux/glibc host, and a family already registered to
 * another baseline.
 *
 * Usage (from the repository root):
 *   PI_CHECKOUT=<private frozen checkout> PI_BASELINE=pi-v1.3.0 \
 *     <checkout>/node_modules/.bin/tsx fixtures/pi-v1.3.0/audit-1106/capture-duration.mts
 *
 * The script re-execs itself under the checkout's tsx so the frozen workspace `paths` resolve.
 */

import { execFileSync, spawnSync } from "node:child_process";
import { createHash } from "node:crypto";
import { mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { createRequire } from "node:module";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const scriptPath = fileURLToPath(import.meta.url);
const familyDir = path.dirname(scriptPath);
const auditRoot = path.resolve(familyDir, "..");
const repositoryRoot = path.resolve(auditRoot, "../..");
const family = "audit-1106";
const registryPath = path.join(auditRoot, "baselines.json");
const piCheckout = process.env.PI_CHECKOUT ?? path.resolve(repositoryRoot, "../pi");

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
const head = execFileSync("git", ["-C", piCheckout, "rev-parse", "HEAD"], { encoding: "utf8" }).trim();
if (head !== baseline.revision) {
	throw new Error(`pi checkout must be at ${baseline.revision} (baseline ${baselineName}), found ${head}`);
}
if (execFileSync("git", ["-C", piCheckout, "status", "--porcelain"], { encoding: "utf8" }).trim()) {
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
const environment = {
	platform: process.platform,
	arch: process.arch,
	libc: "glibc",
	glibcVersion: glibc,
	node: process.version,
	tsx: tsxVersion,
};
const capturedAt = new Date().toISOString();

// The package entries are the public surface: Agent and runToolCall from agent-core,
// createAssistantMessageEventStream from the AI transport, and the application-owned nested
// runner and session manager. Import by checkout path so the frozen workspace is authoritative.
const agent = await import(pathToFileURL(path.join(piCheckout, "packages/agent/src/index.ts")).href);
const eventStream = await import(
	pathToFileURL(path.join(piCheckout, "packages/ai/src/utils/event-stream.ts")).href
);
const { captureScenarios } = await import(pathToFileURL(path.join(familyDir, "scenarios.mts")).href);

const captureApi = {
	Agent: agent.Agent,
	runToolCall: agent.runToolCall,
	createAssistantMessageEventStream: eventStream.createAssistantMessageEventStream,
};
const artifacts = await captureScenarios(captureApi, piCheckout);

const sha256 = (bytes: Buffer) => createHash("sha256").update(bytes).digest("hex");
const manifestArtifacts: any[] = [];
const artifactFiles: { path: string; bytes: Buffer }[] = [];
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
	artifactFiles.push({ path: artifact.path, bytes });
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
const scenariosPath = path.relative(repositoryRoot, path.join(familyDir, "scenarios.mts")).split(path.sep).join("/");
const generatorBytes = readFileSync(scriptPath);
const scenariosBytes = readFileSync(path.join(familyDir, "scenarios.mts"));
const manifest = {
	schema: "pike/pi-v1.3.0-audit-evidence/1",
	baseline: baselineName,
	revision: baseline.revision,
	ticket: "1106",
	family,
	capturedAt,
	environment,
	generator: {
		path: generatorPath,
		sha256: sha256(generatorBytes),
		command: `PI_CHECKOUT=<pi-checkout> PI_BASELINE=${baselineName} <pi-checkout>/node_modules/.bin/tsx ${generatorPath}`,
		sources: [
			{ path: generatorPath, sha256: sha256(generatorBytes) },
			{ path: scenariosPath, sha256: sha256(scenariosBytes) },
		],
	},
	artifacts: manifestArtifacts,
};

// Write only after every guard and scenario succeeded.
mkdirSync(familyDir, { recursive: true });
for (const artifact of artifactFiles) {
	writeFileSync(path.join(familyDir, artifact.path), artifact.bytes);
}
writeFileSync(path.join(familyDir, "manifest.json"), `${JSON.stringify(manifest, null, 2)}\n`);
console.log(
	`captured ${family} (${artifactFiles.length} artifacts, ${manifestArtifacts.reduce(
		(total, artifact) => total + artifact.scenarios.length,
		0,
	)} scenarios) for ${baselineName} (${baseline.revision})`,
);
