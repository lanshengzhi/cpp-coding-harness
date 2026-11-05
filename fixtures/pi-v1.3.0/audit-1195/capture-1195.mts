#!/usr/bin/env tsx
/**
 * Capture the `audit-1195` frozen observations: the discovery globals'
 * absent-name spelling (JS `undefined`) and the host-timezone semantics of
 * non-deterministic `execute()` versus deterministic `start()`, against the
 * frozen pi v1.3.0 checkout. Both rows carried a recorded semantic boundary
 * (#1195); this capture records what a script actually observes so the C++
 * comparison binds the real upstream behavior.
 *
 * Usage (from the repository root):
 *   PI_CHECKOUT=<private frozen checkout> PI_BASELINE=pi-v1.3.0 \
 *     <checkout>/node_modules/.bin/tsx fixtures/pi-v1.3.0/audit-1195/capture-1195.mts
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
const family = "audit-1195";
const registryPath = path.join(auditRoot, "baselines.json");
const piCheckout = process.env.PI_CHECKOUT ?? path.resolve(repositoryRoot, "../pi");

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

function load(relativePath: string): Promise<Record<string, any>> {
	return import(pathToFileURL(path.join(piCheckout, relativePath)).href) as Promise<Record<string, any>>;
}

const codemode = await load("packages/codemode/src/index.ts");
const applicationExecute = await load("packages/coding-agent/src/extensions/codemode/execute.ts");
const toolModule = await load("packages/coding-agent/src/extensions/codemode/tool.ts");

const CodemodeSandbox = codemode.CodemodeSandbox as any;
const loadQuickJSWasm = codemode.loadQuickJSWasm as any;
const executeCodemode = applicationExecute.executeCodemode as any;
const renderToolSample = codemode.renderToolSample as any;
const toCodemodeIdentifier = codemode.toCodemodeIdentifier as any;
const toCodemodeDeclaration = toolModule.toCodemodeDeclaration as any;

const wasm = loadQuickJSWasm();

function scenario(name: string, inputs: any, expected: any, normalizations: string[] = [], upstreamTest?: any) {
	return { name, inputs, expected, observed: expected, normalizations, ...(upstreamTest ? { upstreamTest } : {}) };
}

const artifacts: Array<{ path: string; sourceEndpoints: string[]; scenarios: any[] }> = [];

// ---------------------------------------------------------------------------
// Discovery absence (cch_codemode:codemode-script-discovery): what a script
// observes for absent/present describeTool/describeNamespace names.
// ---------------------------------------------------------------------------
{
	const tools = [
		{
			name: "read",
			description: "Read a file",
			parameters: { type: "object", properties: { path: { type: "string", description: "File path" } } },
			execute: async () => "ok",
		},
		{
			name: "mcp__docs__search",
			description: "Search docs",
			parameters: { type: "object", properties: { query: { type: "string" } } },
			outputSchema: { type: "object", properties: { results: { type: "array", items: { type: "string" } } } },
			execute: async () => ({ results: [] }),
		},
	];
	const namespaces: Record<string, any> = {
		mcp__docs__search: { name: "docs", description: "Documentation tools", instructions: "Use docs." },
	};
	const ctx: any = {
		tools,
		sessionManager: { getBranch: () => [] },
		executeTool: async (name: string, args: unknown) => ({
			isError: false,
			toolCall: { id: "call/1", name, args },
			result: { content: [{ type: "text", text: "ok" }] },
		}),
	};

	// The returned string is the last text item (pi's `valueText` keeps string
	// values unquoted), so it parses exactly.
	const script = `const absentTool = await describeTool("__missing__");
const absentToolRaw = await describeTool("read-nope");
const absentNs = await describeNamespace("__missing__");
const present = await describeTool("read");
const presentByJsName = await describeTool("mcp__docs__search");
const ns = await describeNamespace("docs");
return JSON.stringify({
	absentToolUndefined: absentTool === undefined,
	absentToolType: typeof absentTool,
	absentNsUndefined: absentNs === undefined,
	absentNsType: typeof absentNs,
	presentIsString: typeof present === "string",
	presentHasDeclaration: typeof present === "string" && present.includes("declare const tools"),
	presentByJsNameIsString: typeof presentByJsName === "string",
	nsName: ns === undefined ? null : ns.name,
	nsTools: ns === undefined ? null : ns.tools,
	nsDescription: ns === undefined ? null : ns.description ?? null,
});`;
	const result = await executeCodemode("call-discovery", { code: script }, undefined, undefined, ctx, {
		getToolNamespace: (name: string) => namespaces[name],
	});
	const textBlocks = (result.content as Array<{ type: string; text: string }>).filter(
		(block) => block.type === "text",
	);
	const returned = JSON.parse(textBlocks[textBlocks.length - 1].text);

	artifacts.push({
		path: "discovery-absence.json",
		sourceEndpoints: [
			"packages/coding-agent/src/extensions/codemode/execute.ts:createDiscoveryGlobals,describeTool,describeNamespace",
			"packages/coding-agent/src/extensions/codemode/tool.ts:toCodemodeDeclaration",
		],
		scenarios: [
			scenario(
				"an absent tool or namespace reads as undefined through the discovery globals",
				{ script, absent: ["__missing__", "read-nope"], present: ["read", "mcp__docs__search"], namespace: "docs" },
				returned,
				[],
				{ path: "packages/coding-agent/src/extensions/codemode/execute.ts", case: "createDiscoveryGlobals" },
			),
		],
	});
}

// ---------------------------------------------------------------------------
// Host clock/timezone (cch_codemode:codemode-script-clock-timezone): the
// non-deterministic execute() reads the host offset; deterministic start()
// pins UTC.
// ---------------------------------------------------------------------------
{
	const sandbox = new CodemodeSandbox({ wasm });
	const offsetsScript =
		"return [new Date(0).getTimezoneOffset(), new Date('2026-06-01T00:00:00Z').getTimezoneOffset()];";
	const executed = await sandbox.execute(offsetsScript);
	const run = sandbox.start(offsetsScript, { startedAt: 1_768_471_200_000, seed: 1, onCall: () => {} });
	const started = await run.result;
	await sandbox.close();

	const hostOffsetMinutes = new Date(0).getTimezoneOffset();
	artifacts.push({
		path: "clock-timezone.json",
		sourceEndpoints: [
			"packages/codemode/src/runtime/vm.ts:startVm,timezoneOffset",
			"packages/codemode/test/deterministic.test.ts:execute() is not deterministic while start() is",
		],
		scenarios: [
			scenario(
				"execute reads the host timezone while deterministic start pins UTC",
				{ script: offsetsScript, startedAt: 1_768_471_200_000, seed: 1 },
				{
					executeOffsetsMinutes: executed.value,
					startOffsetsMinutes: started.value,
					hostOffsetMinutes,
					executeMatchesHost:
						JSON.stringify(executed.value) === JSON.stringify([hostOffsetMinutes, hostOffsetMinutes]),
					startIsUtc: JSON.stringify(started.value) === JSON.stringify([0, 0]),
				},
				[],
				{ path: "packages/codemode/test/deterministic.test.ts", case: "execute() is not deterministic while start() is" },
			),
		],
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
const manifest = {
	schema: "pike/pi-v1.3.0-audit-evidence/1",
	baseline: baselineName,
	revision: baseline.revision,
	ticket: "1195",
	family,
	capturedAt,
	environment,
	generator: {
		path: generatorPath,
		sha256: sha256(readFileSync(scriptPath)),
		command: `PI_CHECKOUT=<pi-checkout> PI_BASELINE=${baselineName} <pi-checkout>/node_modules/.bin/tsx ${generatorPath}`,
		sources: [{ path: generatorPath, sha256: sha256(readFileSync(scriptPath)) }],
	},
	artifacts: manifestArtifacts,
};

mkdirSync(familyDir, { recursive: true });
for (const [relative, bytes] of artifactBytes) writeFileSync(path.join(familyDir, relative), bytes);
writeFileSync(path.join(familyDir, "manifest.json"), `${JSON.stringify(manifest, null, 2)}\n`);

console.log(
	`captured ${family} (${manifestArtifacts.length} artifacts) for ${baselineName} (${baseline.revision})`,
);
