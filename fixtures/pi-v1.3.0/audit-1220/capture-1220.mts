#!/usr/bin/env tsx
/**
 * Capture the `audit-1220` frozen observations: route batch B12 (issue
 * #1220, Layout algebra, scroll containers and the small text components)
 * recaptured at the frozen pi v1.3.0 checkout.
 * Every value below comes from executing the frozen checkout's own modules
 * through their public seams; nothing is derived from C++.
 *
 * Usage (from the repository root):
 *   PI_CHECKOUT=<private frozen checkout> PI_BASELINE=pi-v1.3.0 \
 *     <checkout>/node_modules/.bin/tsx fixtures/pi-v1.3.0/audit-1220/capture-1220.mts
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
const family = "audit-1220";
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

const captureDir = path.join(repositoryRoot, "fixtures/pi-tui/capture");
const { captureStackLayout } = await import(
	pathToFileURL(path.join(captureDir, "stack-layout-scenarios.mts")).href
);
const { captureScrollView } = await import(
	pathToFileURL(path.join(captureDir, "scroll-view-scenarios.mts")).href
);
const { captureScrollbar } = await import(
	pathToFileURL(path.join(captureDir, "scrollbar-scenarios.mts")).href
);
const { captureComponents } = await import(
	pathToFileURL(path.join(captureDir, "component-scenarios.mts")).href
);
const { captureUtilsWidth } = await import(
	pathToFileURL(path.join(captureDir, "utils-width-scenarios.mts")).href
);

const artifacts: Array<{ path: string; sourceEndpoints: string[]; scenarios: any[] }> = [];

// Family 1: layout.json (Constrained and unbounded stack layout algebra, HStack/VStack)
{
	const observation = await captureStackLayout(piCheckout);
	artifacts.push({
		path: "layout.json",
		sourceEndpoints: observation.sourceEndpoints,
		scenarios: observation.scenarios,
	});
}

// Family 2: scroll-view.json (Scroll viewports, scroll algebra, and scrollbar presentation)
{
	const scrollObservation = await captureScrollView(piCheckout);
	const scrollbarObservation = await captureScrollbar(piCheckout);
	const combinedSourceEndpoints = Array.from(
		new Set([...scrollObservation.sourceEndpoints, ...scrollbarObservation.sourceEndpoints]),
	);
	// Select the key scrollbar scenarios covering modes, reservation, and transient timeline
	const scrollbarScenarios = scrollbarObservation.scenarios.filter((s: any) =>
		["scrollbar-modes-and-reservation", "scrollbar-transient-timeline"].includes(s.name),
	);
	artifacts.push({
		path: "scroll-view.json",
		sourceEndpoints: combinedSourceEndpoints,
		scenarios: [...scrollObservation.scenarios, ...scrollbarScenarios],
	});
}

// Family 3: component.json (Text, Box, Spacer, TruncatedText)
{
	const observation = await captureComponents(piCheckout);
	artifacts.push({
		path: "component.json",
		sourceEndpoints: observation.sourceEndpoints,
		scenarios: observation.scenarios,
	});
}

// Family 4: utils-width.json (Visible width, wrap, truncate, slice, links, strip sequences)
{
	const observation = await captureUtilsWidth(piCheckout);
	artifacts.push({
		path: "utils-width.json",
		sourceEndpoints: observation.sourceEndpoints,
		scenarios: observation.scenarios,
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
const scenarioSources = [
	"stack-layout-scenarios.mts",
	"scroll-view-scenarios.mts",
	"scrollbar-scenarios.mts",
	"component-scenarios.mts",
	"utils-width-scenarios.mts",
].map((name) => {
	const relative = `fixtures/pi-tui/capture/${name}`;
	return { path: relative, sha256: sha256(readFileSync(path.join(repositoryRoot, relative))) };
});

const manifest = {
	schema: "pike/pi-v1.3.0-audit-evidence/1",
	baseline: baselineName,
	revision: baseline.revision,
	ticket: "1220",
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
