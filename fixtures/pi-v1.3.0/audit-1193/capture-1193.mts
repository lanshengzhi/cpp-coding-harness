#!/usr/bin/env tsx
/**
 * Capture the `audit-1193` frozen observations: the generic Remote
 * request/exchange/reply contract, the guest-module create/load boundary, and
 * the application output-token budget, all against the frozen pi v1.3.0
 * checkout. These three rows were source-only audits (#1179): no frozen
 * scenario existed, so this capture records the independent upstream
 * observations the C++ comparisons bind to. Expected values come only from
 * executing the frozen checkout's own modules.
 *
 * Usage (from the repository root):
 *   PI_CHECKOUT=<private frozen checkout> PI_BASELINE=pi-v1.3.0 \
 *     <checkout>/node_modules/.bin/tsx fixtures/pi-v1.3.0/audit-1193/capture-1193.mts
 */

import { execFileSync, spawnSync } from "node:child_process";
import { createHash } from "node:crypto";
import { mkdirSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const scriptPath = fileURLToPath(import.meta.url);
const familyDir = path.dirname(scriptPath);
const auditRoot = path.resolve(familyDir, "..");
const repositoryRoot = path.resolve(auditRoot, "../..");
const family = "audit-1193";
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

const CodemodeSandbox = codemode.CodemodeSandbox as any;
const RemoteCodemodeSandbox = codemode.RemoteCodemodeSandbox as any;
const serveCodemodeRemote = codemode.serveCodemodeRemote as any;
const loadQuickJSWasm = codemode.loadQuickJSWasm as any;
const executeCodemode = applicationExecute.executeCodemode as any;

const wasm = loadQuickJSWasm();

function scenario(name: string, inputs: any, expected: any, normalizations: string[] = [], upstreamTest?: any) {
	return { name, inputs, expected, observed: expected, normalizations, ...(upstreamTest ? { upstreamTest } : {}) };
}

const artifacts: Array<{ path: string; sourceEndpoints: string[]; scenarios: any[] }> = [];

// ---------------------------------------------------------------------------
// Generic Remote (cch_codemode:codemode-remote): the request/exchange/reply
// contract through the public Remote surface, including the terminal writes
// field and caller-side tool execution.
// ---------------------------------------------------------------------------
{
	/** An in-process remote that records what crosses the boundary, cloned as Workers RPC would. */
	function recordingRemote(options: Record<string, unknown> = {}) {
		const requests: any[] = [];
		const exchanges: any[] = [];
		const served: Promise<void>[] = [];
		const remote = {
			execute(request: any, exchange: any) {
				requests.push(structuredClone(request));
				const done = serveCodemodeRemote(
					structuredClone(request),
					async (messages: any) => {
						exchanges.push(structuredClone(messages));
						return structuredClone(await exchange(structuredClone(messages)));
					},
					{ wasm, ...options },
				);
				served.push(done);
				return done;
			},
		};
		return { remote, requests, exchanges, served };
	}

	const scenarios: any[] = [];

	// 1. Normal exchange with a caller-side tool and a seeded store.
	{
		const { remote, requests, exchanges, served } = recordingRemote();
		const seen: unknown[] = [];
		const sandbox = new RemoteCodemodeSandbox({
			remote: () => remote,
			tools: [
				{
					name: "add",
					description: "Add one.",
					execute: (args: any) => {
						seen.push(args);
						return args.a + 1;
					},
				},
			],
		});
		const result = await sandbox.execute("text('hi'); return await tools.add({ a: 1 })", { store: { k: 1 } });
		await Promise.all(served);
		const request = requests[0];
		const terminal = exchanges[exchanges.length - 1];
		scenarios.push(
			scenario(
				"a generic remote exchange runs the script remotely and calls a caller tool",
				{
					script: "text('hi'); return await tools.add({ a: 1 })",
					tools: ["add"],
					store: { k: 1 },
				},
				{
					request: {
						code: "text('hi'); return await tools.add({ a: 1 })",
						tools: request.data.tools,
						globals: request.data.globals,
						store: request.data.store,
						interruptBudget: request.interruptBudget ?? null,
					},
					exchanges: exchanges.length,
					callMessages: exchanges
						.flat()
						.filter((message: any) => message.type === "call")
						.map((message: any) => ({ id: message.id, target: message.target, name: message.name, args: message.args })),
					terminalTypes: terminal.map((message: any) => message.type),
					result: {
						ok: result.ok,
						value: result.value,
						output: result.output,
						calls: result.calls,
						storeWrites: result.storeWrites,
					},
					seenArgs: seen,
				},
			),
		);
	}

	// 2. The terminal writes set: empty on success without store(), the real
	//    write set after store().
	{
		const { remote, served } = recordingRemote();
		const sandbox = new RemoteCodemodeSandbox({ remote: () => remote });
		const plain = await sandbox.execute("return 1");
		const writing = await sandbox.execute("store('a', 1); store('b', { c: 2 }); return load('a');");
		await Promise.all(served);
		scenarios.push(
			scenario(
				"the terminal writes set is empty on success and carries a real write set when the script writes",
				{ plainScript: "return 1", writingScript: "store('a', 1); store('b', { c: 2 }); return load('a');" },
				{
					plain: { ok: plain.ok, value: plain.value, writes: plain.storeWrites },
					writing: { ok: writing.ok, value: writing.value, writes: writing.storeWrites },
				},
			),
		);
	}

	// 3. The three distinct remote failure sandbox errors.
	{
		const failing = new RemoteCodemodeSandbox({
			remote: () => ({ execute: () => Promise.reject(new Error("object reset")) }),
		});
		const failingResult = await failing.execute("return 1");
		const silent = new RemoteCodemodeSandbox({
			remote: () => ({ execute: () => Promise.resolve() }),
		});
		const silentResult = await silent.execute("return 1");
		const unreachable = new RemoteCodemodeSandbox({
			remote: () => {
				throw new Error("no binding");
			},
		});
		const unreachableResult = await unreachable.execute("return 1");
		scenarios.push(
			scenario(
				"a rejected silent or unreachable remote keeps three distinct sandbox errors",
				{ modes: ["rejected", "silent", "unreachable"] },
				{
					rejected: { ok: failingResult.ok, error: failingResult.error },
					silent: { ok: silentResult.ok, error: silentResult.error },
					unreachable: { ok: unreachableResult.ok, error: unreachableResult.error },
				},
			),
		);
	}

	artifacts.push({
		path: "remote-generic.json",
		sourceEndpoints: [
			"packages/codemode/src/runtime/remote.ts:RemoteCodemodeSandbox,serveCodemodeRemote,CodemodeRemoteRequest,CodemodeRemoteExchange",
			"packages/codemode/src/types.ts:CodemodeResult",
			"packages/codemode/test/portable.test.ts:reports a failing or vanishing remote as a sandbox error",
		],
		scenarios,
	});
}

// ---------------------------------------------------------------------------
// Guest-module create/load (cch_codemode:codemode-sandbox-create-errors):
// pi validates the guest lazily at the first run; the C++ create validates
// eagerly. Both observations are recorded so the comparison binds the real
// boundary instead of a derived expectation.
// ---------------------------------------------------------------------------
{
	const scenarios: any[] = [];

	// 1. A missing guest module: construction succeeds, the first run reports
	//    the load failure. (Recorded with a rejecting wasm module so the
	//    observation needs no filesystem fixture.)
	{
		const rejectedWasm = Promise.reject(new Error("wasm unavailable"));
		// Handled now so Node's unhandled-rejection guard is quiet; the sandbox's
		// later await still observes the rejection.
		rejectedWasm.catch(() => {});
		const sandbox = new CodemodeSandbox({ wasm: rejectedWasm });
		let constructionOk = true;
		let loadError: any = null;
		try {
			const result = await sandbox.execute("return 1");
			loadError = result.ok ? null : result.error;
		} catch (error: any) {
			loadError = { kind: "throw", message: String(error?.message ?? error) };
		}
		await sandbox.close();
		scenarios.push(
			scenario(
				"a missing guest module fails at the first run in pi, never at construction",
				{ wasm: "rejects: wasm unavailable", script: "return 1" },
				{
					constructionOk,
					error: loadError,
				},
			),
		);
	}

	// 2. A sandbox that never executes never touches the guest: construction
	//    succeeds even with a rejecting wasm module, and close() is quiet.
	{
		const rejectedWasm = Promise.reject(new Error("wasm unavailable"));
		rejectedWasm.catch(() => {});
		const sandbox = new CodemodeSandbox({ wasm: rejectedWasm });
		let closeOk = true;
		try {
			await sandbox.close();
		} catch {
			closeOk = false;
		}
		scenarios.push(
			scenario(
				"a sandbox that never executes never touches the guest",
				{ wasm: "rejects: wasm unavailable", executes: 0 },
				{ constructionOk: true, closeOk, loadAttempts: 0 },
			),
		);
	}

	// 3. A present guest module constructs and runs.
	{
		const sandbox = new CodemodeSandbox({ wasm });
		const result = await sandbox.execute("return 1");
		await sandbox.close();
		scenarios.push(
			scenario(
				"a present guest module constructs and runs",
				{ wasm: "loadQuickJSWasm()", script: "return 1" },
				{ ok: result.ok, value: result.value },
			),
		);
	}

	artifacts.push({
		path: "sandbox-create.json",
		sourceEndpoints: [
			"packages/codemode/src/runtime/host.ts:CodemodeSandbox,workerLauncher",
			"packages/codemode/src/wasm.ts:loadQuickJSWasm",
			"packages/codemode/src/runtime/execution.ts:CodemodeSandboxBase",
			"packages/coding-agent/src/extensions/codemode/execute.lazy.ts:loadCodemodeExecutor",
		],
		scenarios,
	});
}

// ---------------------------------------------------------------------------
// Application output-token budget (cch_codemode:codemode-source-output-token-option):
// the @options max_output_tokens budget through the real executeCodemode.
// ---------------------------------------------------------------------------
{
	const scenarios: any[] = [];

	function summarize(result: any) {
		const text = result.content
			.filter((block: any) => block.type === "text")
			.map((block: any) => block.text)
			.join("\n");
		return {
			ok: !result.isError,
			text,
			details: result.details ?? {},
		};
	}

	// Wall-clock seconds in the header are provenance, not behavior.
	function normalize(text: string) {
		return text.replace(/Wall time [0-9.]+ seconds/, "Wall time <N> seconds");
	}

	// 1. Over budget: head/tail kept, warning header, spill file with the full
	//    text, details.fullOutputPath set. The 80-token option must be parsed
	//    and consumed: the default 10000-token budget would leave this output
	//    untruncated.
	{
		const script = `// @options: {"max_output_tokens": 80}
text("x".repeat(500));
return "done";`;
		const result = await executeCodemode("call-budget", { code: script }, undefined, undefined, undefined, {});
		const summary = summarize(result);
		const spillPath = summary.details.fullOutputPath as string | undefined;
		let spillBytes: string | null = null;
		if (spillPath) {
			spillBytes = readFileSync(spillPath, "utf8");
			rmSync(spillPath, { force: true });
		}
		scenarios.push(
			scenario(
				"over-budget output keeps the head and tail and spills the full text to a named file",
				{ script, maxOutputTokens: 80 },
				{
					...summary,
					text: normalize(summary.text),
					fullOutputPathNamed: typeof spillPath === "string" && spillPath.length > 0,
					spillBytes: spillBytes == null ? null : normalize(spillBytes),
				},
				["wall time line"],
				{ path: "packages/coding-agent/src/extensions/codemode/execute.ts", case: "executeCodemode,truncateOutput" },
			),
		);
	}

	// 2. Zero budget: even the error text is truncated away.
	{
		const script = `// @options: {"max_output_tokens": 0}
throw new Error("boom");`;
		const result = await executeCodemode("call-zero", { code: script }, undefined, undefined, undefined, {});
		const summary = summarize(result);
		const spillPath = summary.details.fullOutputPath as string | undefined;
		let spillBytes: string | null = null;
		if (spillPath) {
			spillBytes = readFileSync(spillPath, "utf8");
			rmSync(spillPath, { force: true });
		}
		scenarios.push(
			scenario(
				"a zero budget truncates over the error text",
				{ script, maxOutputTokens: 0 },
				{
					...summary,
					text: normalize(summary.text),
					fullOutputPathNamed: typeof spillPath === "string" && spillPath.length > 0,
					spillBytes: spillBytes == null ? null : normalize(spillBytes),
				},
				["wall time line"],
			),
		);
	}

	// 3. Under budget: no truncation marker, no spill file.
	{
		const script = `// @options: {"max_output_tokens": 10000}
text("short");
return 1;`;
		const result = await executeCodemode("call-under", { code: script }, undefined, undefined, undefined, {});
		const summary = summarize(result);
		scenarios.push(
			scenario(
				"an under-budget output is not truncated and names no spill file",
				{ script, maxOutputTokens: 10000 },
				{
					...summary,
					text: normalize(summary.text),
					hasTruncationMarker: summary.text.includes("Warning: truncated output"),
				},
				["wall time line"],
			),
		);
	}

	artifacts.push({
		path: "output-budget.json",
		sourceEndpoints: [
			"packages/coding-agent/src/extensions/codemode/execute.ts:DEFAULT_MAX_OUTPUT_TOKENS,truncateOutput,spillOutput,executeCodemode",
			"packages/coding-agent/src/utils/output-files.ts:writeOutputFile",
		],
		scenarios,
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
	ticket: "1193",
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
