#!/usr/bin/env tsx
/**
 * Capture the `audit-1108` reset and settled-cancellation observations for the shared
 * pi-v1.3.0 evidence registry (#1108).
 *
 * Two artifacts:
 *
 * - `reset-lifecycle.json` drives the real public `Agent` seam: reset after a completed run
 *   with queued steering/follow-up input, and a rejected reset during an active run.
 * - `settled-cancellation.json` is a SOURCE PROBE of the frozen `ProgramStatusReporter`: it
 *   records only the reporter's wire output for `agent_settled`, so the abort-request state —
 *   not the assistant terminal — decides whether the run settles to idle. No model turn, no
 *   gateway and no network are involved in that artifact.
 *
 * The capture refuses a checkout whose HEAD is not the registered baseline revision, a modified
 * source tree, a non-Linux/glibc host, and a family already registered to another baseline.
 *
 * Usage (from the repository root):
 *   PI_CHECKOUT=<private frozen checkout> PI_BASELINE=pi-v1.3.0 \
 *     <checkout>/node_modules/.bin/tsx fixtures/pi-v1.3.0/audit-1108/capture-1108.mts
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
const family = "audit-1108";
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
const sha256 = (bytes: Buffer) => createHash("sha256").update(bytes).digest("hex");

const agentModule = await import(pathToFileURL(path.join(piCheckout, "packages/agent/src/index.ts")).href);
const eventStream = await import(pathToFileURL(path.join(piCheckout, "packages/ai/src/utils/event-stream.ts")).href);
const { ProgramStatusReporter } = await import(
	pathToFileURL(path.join(piCheckout, "packages/coding-agent/src/modes/interactive/program-status-reporter.ts")).href
);
const { formatProgramStatus } = await import(
	pathToFileURL(path.join(piCheckout, "packages/tui/src/program-status.ts")).href
);

const Agent = agentModule.Agent as any;
const createAssistantMessageEventStream = eventStream.createAssistantMessageEventStream as any;

function assistantMessage(text: string) {
	return {
		role: "assistant",
		content: [{ type: "text", text }],
		api: "faux",
		provider: "faux",
		model: "faux-1",
		usage: {
			input: 0,
			output: 0,
			cacheRead: 0,
			cacheWrite: 0,
			totalTokens: 0,
			cost: { input: 0, output: 0, cacheRead: 0, cacheWrite: 0, total: 0 },
		},
		stopReason: "stop",
		timestamp: 1,
	};
}

function scriptedStream(text: string) {
	const stream = createAssistantMessageEventStream();
	queueMicrotask(() => {
		stream.push({ type: "start", partial: assistantMessage("") });
		stream.push({ type: "done", reason: "stop", message: assistantMessage(text) });
	});
	return stream;
}

function echoTool() {
	return {
		name: "echo",
		label: "Echo",
		description: "Echo input",
		parameters: { type: "object", properties: {}, additionalProperties: false },
		execute: async () => ({ content: [{ type: "text", text: "echo" }], details: {} }),
	};
}

/// A model that supports every thinking level, so a non-off default would survive clamping.
/// This is what makes the default-off observation separate from a clamp-to-off effect.
const reasoningModel = {
	id: "reasoning-mock",
	name: "reasoning-mock",
	api: "unknown",
	provider: "unknown",
	baseUrl: "",
	reasoning: true,
	input: [],
	cost: { input: 0, output: 0, cacheRead: 0, cacheWrite: 0 },
	contextWindow: 8192,
	maxTokens: 1024,
};

// ── Artifact 1: reset ───────────────────────────────────────────────────────────────────────────

const resetScenarios: any[] = [];

{
	// A completed run, queued input in both queues, then reset. Live tools, model and thinking
	// level are construction policy and must survive; only the replayed system baseline and the
	// transient run facts reset.
	const requests: any[] = [];
	const agent: any = new Agent({
		streamFn: (_model: any, context: any) => {
			requests.push({
				roles: context.messages.map((message: any) => message.role),
				systemToolsAdded:
					context.messages
						.filter((message: any) => message.role === "system")
						.flatMap((message: any) => (message.toolsAdded ?? []).map((tool: any) => tool.name)),
			});
			return scriptedStream("ok");
		},
		initialState: {
			systemPrompt: "sys",
			tools: [echoTool()],
			model: reasoningModel,
			thinkingLevel: "high",
		},
	});
	await agent.prompt("go");
	agent.steer({ role: "user", content: "steer", timestamp: 1 });
	agent.followUp({ role: "user", content: "follow", timestamp: 1 });
	const beforeReset = {
		roles: agent.state.messages.map((message: any) => message.role),
		hasQueuedMessages: agent.hasQueuedMessages(),
	};
	agent.reset();
	const afterReset = {
		roles: agent.state.messages.map((message: any) => message.role),
		systemContent: agent.state.messages[0]?.content ?? null,
		systemToolsAdded: agent.state.messages[0]?.toolsAdded?.map((tool: any) => tool.name) ?? [],
		systemPrompt: agent.state.systemPrompt,
		tools: agent.state.tools.map((tool: any) => tool.name),
		modelId: agent.state.model.id,
		thinkingLevel: agent.state.thinkingLevel,
		hasQueuedMessages: agent.hasQueuedMessages(),
		isStreaming: agent.state.isStreaming,
		streamingMessage: agent.state.streamingMessage ?? null,
		pendingToolCalls: [...agent.state.pendingToolCalls],
	};
	// The next run starts from the baseline: one system message, a new user message, and the
	// retained tool still offered to the provider.
	await agent.prompt("again");
	const afterResetRequest = requests.at(-1);

	resetScenarios.push({
		name: "reset retains live tools, model and thinking level and restores the message baseline",
		inputs: {
			initialState: {
				systemPrompt: "sys",
				tools: ["echo"],
				model: { id: "reasoning-mock", reasoning: true },
				thinkingLevel: "high",
			},
			prompt: "go",
			queuedBeforeReset: ["steer", "follow"],
			promptAfterReset: "again",
		},
		expected: {
			beforeReset,
			afterReset,
			afterResetRequest: {
				roles: afterResetRequest.roles,
				systemToolsAdded: afterResetRequest.systemToolsAdded,
			},
		},
		normalizations: [],
		upstreamTest: {
			path: "packages/agent/test/agent.test.ts",
			case: "restores the transcript baseline when reset",
		},
	});
}

{
	// Reset while a run is active: rejected with the frozen diagnostic, and neither the run nor
	// the transcript changes. Releasing the gate then completes the same run normally.
	let release: () => void = () => {};
	const gate = new Promise<void>((resolve) => {
		release = resolve;
	});
	let started: () => void = () => {};
	const startedPromise = new Promise<void>((resolve) => {
		started = resolve;
	});
	const agent: any = new Agent({
		streamFn: () => {
			const stream = createAssistantMessageEventStream();
			queueMicrotask(async () => {
				stream.push({ type: "start", partial: assistantMessage("") });
				started();
				await gate;
				stream.push({ type: "done", reason: "stop", message: assistantMessage("Done") });
			});
			return stream;
		},
	});
	const run = agent.prompt("Hello");
	await startedPromise;
	const beforeReset = {
		isStreaming: agent.state.isStreaming,
		roles: agent.state.messages.map((message: any) => message.role),
	};
	let resetError: string | null = null;
	try {
		agent.reset();
	} catch (error: any) {
		resetError = error.message;
	}
	const afterRejectedReset = {
		isStreaming: agent.state.isStreaming,
		roles: agent.state.messages.map((message: any) => message.role),
	};
	release();
	await run;
	const afterRun = {
		isStreaming: agent.state.isStreaming,
		roles: agent.state.messages.map((message: any) => message.role),
		lastAssistantText: agent.state.messages.at(-1)?.content?.[0]?.text ?? null,
	};

	resetScenarios.push({
		name: "reset during an active run is rejected without changing the run or transcript",
		inputs: {
			prompt: "Hello",
			resetWhen: "assistant stream started, before its terminal",
		},
		expected: {
			beforeReset,
			resetError,
			afterRejectedReset,
			afterRun,
		},
		normalizations: [],
		upstreamTest: {
			path: "packages/agent/test/agent.test.ts",
			case: "should reject reset while processing without corrupting the transcript",
		},
	});
}

{
	// A section-bearing baseline: pi's `state.systemPrompt` is the replayed, rendered prompt
	// (content plus retained section text), so a reset must not keep only the content field.
	const agent: any = new Agent({
		streamFn: () => scriptedStream("ok"),
		initialState: {
			messages: [
				{ role: "system", content: "base", sections: { alpha: "A", beta: "B" }, timestamp: 1 },
				{ role: "user", content: "go", timestamp: 1 },
			],
		},
	});
	agent.reset();
	const afterReset = {
		roles: agent.state.messages.map((message: any) => message.role),
		systemContent: agent.state.messages[0]?.content ?? null,
		systemSections: agent.state.messages[0]?.sections ?? null,
		systemPrompt: agent.state.systemPrompt,
	};

	resetScenarios.push({
		name: "reset renders the replayed sections into the live system prompt",
		inputs: {
			initialMessages: ["system(content=base, sections=alpha,beta)", "user"],
		},
		expected: { afterReset },
		normalizations: [],
		upstreamTest: {
			path: "packages/agent/src/agent.ts",
			case: "AgentState.systemPrompt replays getCurrentSystemPrompt",
		},
	});
}

// ── Artifact 2: settled cancellation (source probe) ─────────────────────────────────────────────

const settledWrites: string[] = [];
const settledTerminal = {
	setProgramStatus(status: unknown) {
		settledWrites.push(formatProgramStatus(status as never));
	},
};
const settledReporter = new ProgramStatusReporter(() => settledTerminal as never, () => undefined);
let settledSteps: { label: string; wire: string | null }[] = [];
function recordSettled(label: string) {
	settledSteps.push({ label, wire: settledWrites.at(-1) ?? null });
	settledWrites.length = 0;
}
function settledEvent(value: unknown) {
	settledReporter.handleEvent(value as never);
}

// An interrupted run whose assistant terminal already reported an error: the abort request
// decides idle, so the error never becomes the resting status.
settledEvent({ type: "agent_start" });
recordSettled("interrupted_agent_start_working");
settledEvent({ type: "message_end", message: { role: "assistant", stopReason: "error", errorMessage: "provider failed" } });
recordSettled("interrupted_message_end_error_unchanged");
settledEvent({ type: "agent_settled", aborted: true });
recordSettled("interrupted_agent_settled_idle");
const interruptedSteps = settledSteps;

// The same run shape without an abort request reports the completed outcome.
settledSteps = [];
settledEvent({ type: "agent_start" });
recordSettled("completed_agent_start_working");
settledEvent({ type: "message_end", message: { role: "assistant", stopReason: "stop" } });
recordSettled("completed_message_end_unchanged");
settledEvent({ type: "agent_settled", aborted: false });
recordSettled("completed_agent_settled_done");
const completedSteps = settledSteps;

// A run aborted before its assistant terminal arrives still settles to idle, and the next run
// reports working again, so the idle settlement cannot be mistaken for that run's result.
settledSteps = [];
settledEvent({ type: "agent_start" });
recordSettled("early_abort_agent_start_working");
settledEvent({ type: "agent_settled", aborted: true });
recordSettled("early_abort_agent_settled_idle");
const earlyAbortSteps = settledSteps;

settledSteps = [];
settledEvent({ type: "agent_start" });
recordSettled("next_run_agent_start_working");
settledEvent({ type: "message_end", message: { role: "assistant", stopReason: "stop" } });
recordSettled("next_run_message_end_unchanged");
settledEvent({ type: "agent_settled", aborted: false });
recordSettled("next_run_agent_settled_done");
const nextRunSteps = settledSteps;

const settledScenarios = [
	{
		name: "agent_settled maps the abort request to idle and the run outcome to done",
		inputs: {
			run: "agent_start, assistant error terminal, agent_settled aborted=true",
			controlRun: "agent_start, assistant stop terminal, agent_settled aborted=false",
		},
		expected: {
			interrupted: {
				working: "\u001b]7501;state=working:app=pi\u001b\\",
				messageEndUnchanged: null,
				settled: "\u001b]7501;state=idle:app=pi\u001b\\",
			},
			completed: {
				working: "\u001b]7501;state=working:app=pi\u001b\\",
				messageEndUnchanged: null,
				settled: "\u001b]7501;state=done:app=pi\u001b\\",
			},
		},
		observedSteps: [...interruptedSteps, ...completedSteps],
		normalizations: [],
		upstreamTest: {
			path: "packages/coding-agent/src/modes/interactive/program-status-reporter.ts",
			case: "agent_settled",
		},
	},
	{
		name: "a run aborted before its assistant terminal settles idle and the next run reports working",
		inputs: {
			run: "agent_start, agent_settled aborted=true (no message_end)",
			nextRun: "agent_start, assistant stop terminal, agent_settled aborted=false",
		},
		expected: {
			earlyAbort: {
				working: "\u001b]7501;state=working:app=pi\u001b\\",
				settled: "\u001b]7501;state=idle:app=pi\u001b\\",
			},
			nextRun: {
				working: "\u001b]7501;state=working:app=pi\u001b\\",
				settled: "\u001b]7501;state=done:app=pi\u001b\\",
			},
		},
		observedSteps: [...earlyAbortSteps, ...nextRunSteps],
		normalizations: [],
		upstreamTest: {
			path: "packages/coding-agent/src/core/agent-session.ts",
			case: "_emitAgentSettled",
		},
	},
];

// ── Write the artifacts and manifest ───────────────────────────────────────────────────────────

const artifacts = [
	{
		path: "reset-lifecycle.json",
		sourceEndpoints: [
			"packages/agent/src/agent.ts:Agent.reset,Agent.prompt,Agent.steer,Agent.followUp,Agent.state",
			"packages/ai/src/utils/transcript.ts:getCurrentSystemMessage,getCurrentSystemPrompt,getCurrentTools",
			"packages/agent/test/agent.test.ts:restores the transcript baseline when reset",
			"packages/agent/test/agent.test.ts:should reject reset while processing without corrupting the transcript",
		],
		scenarios: resetScenarios,
	},
	{
		path: "settled-cancellation.json",
		recordKind: "source-probe",
		probe:
			"ProgramStatusReporter driven directly with a recording Terminal stub; the agent_settled events carry the abort request; no model turn, no gateway",
		sourceEndpoints: [
			"packages/coding-agent/src/modes/interactive/program-status-reporter.ts:ProgramStatusReporter.handleEvent",
			"packages/coding-agent/src/core/agent-session.ts:_emitAgentSettled",
			"packages/tui/src/program-status.ts:formatProgramStatus",
		],
		scenarios: settledScenarios,
	},
];

const manifestArtifacts: any[] = [];
const artifactFiles: { path: string; bytes: Buffer }[] = [];
for (const artifact of artifacts) {
	const document = {
		schema: "pike/pi-v1.3.0-audit-evidence/1",
		...(artifact.recordKind ? { recordKind: artifact.recordKind } : {}),
		baseline: baselineName,
		revision: baseline.revision,
		capturedAt,
		environment,
		...(artifact.probe ? { probe: artifact.probe } : {}),
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
	ticket: "1108",
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
