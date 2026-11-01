#!/usr/bin/env tsx
/**
 * Capture the `audit-1113` Agent input-queue admission observations for the shared
 * pi-v1.3.0 evidence registry (#1113).
 *
 * The registered #1047 `queue-admission.json` family already captures unbounded
 * enqueue acceptance, the default one-at-a-time modes, drain ordering, peek/clear
 * and continuation. This family adds the two observations that slice needs to
 * separate the retired C++ capacity behavior from the frozen contract:
 *
 * - 300 steering and 300 follow-up messages are not merely accepted; all 300
 *   remain queued, in FIFO order, and both queue modes select the expected peek
 *   batch.
 * - A message larger than the retired 16 MiB byte bound remains queued intact.
 *
 * The capture refuses a checkout whose HEAD is not the registered baseline revision, a modified
 * source tree, a non-Linux/glibc host, and a family already registered to another baseline.
 *
 * Usage (from the repository root):
 *   PI_CHECKOUT=<private frozen checkout> PI_BASELINE=pi-v1.3.0 \
 *     <checkout>/node_modules/.bin/tsx fixtures/pi-v1.3.0/audit-1113/capture-1113.mts
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
const family = "audit-1113";
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
const sha256 = (bytes: Buffer | string) => createHash("sha256").update(bytes).digest("hex");

const agentModule = await import(pathToFileURL(path.join(piCheckout, "packages/agent/src/index.ts")).href);
const Agent = agentModule.Agent as any;

function user(text: string) {
	return { role: "user", content: text, timestamp: 1 };
}

function messageText(message: any): string {
	const content = message?.content;
	if (typeof content === "string") return content;
	if (Array.isArray(content)) {
		return content.map((block: any) => (block?.type === "text" ? block.text : "")).join("");
	}
	return "";
}

const unusedStream = () => {
	throw new Error("unused");
};

// ── Admission retention: 300 + 300 messages ───────────────────────────────────────────────────

const retentionScenarios: any[] = [];

{
	const agent: any = new Agent({ streamFn: unusedStream });
	const steerReturns: unknown[] = [];
	const followReturns: unknown[] = [];
	for (let index = 0; index < 300; index += 1) {
		steerReturns.push(agent.steer(user(`s${index}`)));
	}
	for (let index = 0; index < 300; index += 1) {
		followReturns.push(agent.followUp(user(`f${index}`)));
	}

	const oneAtATimePeek = agent.peekQueuedMessages().map(messageText);
	const defaultSteeringMode = agent.steeringMode;
	const defaultFollowUpMode = agent.followUpMode;
	agent.steeringMode = "all";
	const steeringAll = agent.peekQueuedMessages().map(messageText);
	agent.clearSteeringQueue();
	agent.followUpMode = "all";
	const followUpAll = agent.peekQueuedMessages().map(messageText);

	const observed = {
		steeringMode: defaultSteeringMode,
		followUpMode: defaultFollowUpMode,
		enqueueReturnsVoid:
			steerReturns.every((value) => value === undefined) && followReturns.every((value) => value === undefined),
		steeringRetained: steeringAll.length,
		followUpRetained: followUpAll.length,
		hasQueuedMessages: agent.hasQueuedMessages(),
		oneAtATimePeekCount: oneAtATimePeek.length,
		oneAtATimePeekText: oneAtATimePeek[0] ?? null,
		steeringFifoFirst: steeringAll[0] ?? null,
		steeringFifoLast: steeringAll.at(-1) ?? null,
		followUpFifoFirst: followUpAll[0] ?? null,
		followUpFifoLast: followUpAll.at(-1) ?? null,
	};
	const expected = {
		steeringMode: "one-at-a-time",
		followUpMode: "one-at-a-time",
		enqueueReturnsVoid: true,
		steeringRetained: 300,
		followUpRetained: 300,
		hasQueuedMessages: true,
		oneAtATimePeekCount: 1,
		oneAtATimePeekText: "s0",
		steeringFifoFirst: "s0",
		steeringFifoLast: "s299",
		followUpFifoFirst: "f0",
		followUpFifoLast: "f299",
	};
	for (const [key, value] of Object.entries(expected)) {
		if (JSON.stringify((observed as any)[key]) !== JSON.stringify(value)) {
			throw new Error(`retention scenario mismatch at ${key}: ${JSON.stringify((observed as any)[key])}`);
		}
	}
	retentionScenarios.push({
		name: "queue admission retains 300 steering and 300 follow-up messages in FIFO order",
		inputs: { steeringMessages: 300, followUpMessages: 300 },
		expected,
		normalizations: [],
		observed,
		upstreamTest: {
			path: "packages/agent/test/agent.test.ts",
			case: "should support steering message queue",
		},
	});
}

// ── Byte bound: a message larger than the retired 16 MiB cap ──────────────────────────────────

{
	const agent: any = new Agent({ streamFn: unusedStream });
	const bigSteering = "s".repeat(16 * 1024 * 1024 + 1);
	const bigFollowUp = "f".repeat(16 * 1024 * 1024 + 2);
	const steerReturn = agent.steer(user(bigSteering));
	const followReturn = agent.followUp(user(bigFollowUp));
	agent.steeringMode = "all";
	const steeringPeek = messageText(agent.peekQueuedMessages()[0]);
	agent.clearSteeringQueue();
	agent.followUpMode = "all";
	const followUpPeek = messageText(agent.peekQueuedMessages()[0]);

	const observed = {
		enqueueReturnsVoid: steerReturn === undefined && followReturn === undefined,
		steeringLength: steeringPeek.length,
		steeringSha256: sha256(steeringPeek),
		followUpLength: followUpPeek.length,
		followUpSha256: sha256(followUpPeek),
		hasQueuedMessages: agent.hasQueuedMessages(),
	};
	const expected = {
		enqueueReturnsVoid: true,
		steeringLength: bigSteering.length,
		steeringSha256: sha256(bigSteering),
		followUpLength: bigFollowUp.length,
		followUpSha256: sha256(bigFollowUp),
		hasQueuedMessages: true,
	};
	for (const [key, value] of Object.entries(expected)) {
		if ((observed as any)[key] !== value) {
			throw new Error(`byte-bound scenario mismatch at ${key}`);
		}
	}
	retentionScenarios.push({
		name: "queue admission keeps a message larger than the retired 16 MiB byte bound",
		inputs: {
			steeringBytes: bigSteering.length,
			followUpBytes: bigFollowUp.length,
			retiredByteBound: 16 * 1024 * 1024,
		},
		expected,
		normalizations: [],
		observed,
		upstreamTest: {
			path: "packages/agent/src/agent.ts",
			case: "PendingMessageQueue.enqueue",
		},
	});
}

// ── Write the artifact and manifest ───────────────────────────────────────────────────────────

const artifacts = [
	{
		path: "queue-admission-retention.json",
		sourceEndpoints: [
			"packages/agent/src/agent.ts:PendingMessageQueue,Agent.steer,Agent.followUp,Agent.peekQueuedMessages,Agent.hasQueuedMessages,Agent.clearSteeringQueue",
			"packages/agent/test/agent.test.ts:should support steering message queue",
			"packages/agent/test/agent.test.ts:previews the next selected queued messages without consuming them",
		],
		scenarios: retentionScenarios,
	},
];

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
const generatorBytes = readFileSync(scriptPath);
const manifest = {
	schema: "pike/pi-v1.3.0-audit-evidence/1",
	baseline: baselineName,
	revision: baseline.revision,
	ticket: "1113",
	family,
	capturedAt,
	environment,
	generator: {
		path: generatorPath,
		sha256: sha256(generatorBytes),
		command: `PI_CHECKOUT=<pi-checkout> PI_BASELINE=${baselineName} <pi-checkout>/node_modules/.bin/tsx ${generatorPath}`,
		sources: [{ path: generatorPath, sha256: sha256(generatorBytes) }],
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
