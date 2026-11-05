#!/usr/bin/env tsx
/**
 * Capture the `audit-1203` frozen observations: the 21 durable rows that had no
 * v1.3.0 frozen observation at all (#1203, `evidence.route: unobserved` in the
 * 1058 consolidated ledger). Every value below comes from executing the frozen
 * checkout's own durable runtime through its public Harness seam over in-memory
 * storage with the frozen faux provider; no expectation is derived from C++.
 *
 * Usage (from the repository root):
 *   PI_CHECKOUT=<private frozen checkout> PI_BASELINE=pi-v1.3.0 \
 *     <checkout>/node_modules/.bin/tsx fixtures/pi-v1.3.0/audit-1203/capture-1203.mts
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
const family = "audit-1203";
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

const piAi = (await import("@earendil-works/pi-ai")) as Record<string, any>;
const durableIndex = await import(pathToFileURL(path.join(piCheckout, "packages/durable/src/index.ts")).href);
const chatSupport = (await import(
	pathToFileURL(path.join(piCheckout, "packages/durable/test/chat-support.ts")).href
)) as Record<string, any>;
const harnessSupport = (await import(
	pathToFileURL(path.join(piCheckout, "packages/durable/test/harness-support.ts")).href
)) as Record<string, any>;
const sessionSupport = (await import(
	pathToFileURL(path.join(piCheckout, "packages/durable/test/session-support.ts")).href
)) as Record<string, any>;
const taskSupport = (await import(
	pathToFileURL(path.join(piCheckout, "packages/durable/test/task-support.ts")).href
)) as Record<string, any>;

const { chatSetup, openChat, allEntries } = chatSupport;
const { addSection } = harnessSupport;
const { context } = sessionSupport;
const {
	fauxAssistantMessage,
	fauxToolCall,
}: { fauxAssistantMessage: (content: any, options?: any) => any; fauxToolCall: (...args: any[]) => any } =
	piAi as any;
const {
	MemoryStorage,
	Harness,
	createRegistry,
	defineTask,
	defineExtension,
	LiveDoc,
}: Record<string, any> = durableIndex as any;

function deferred() {
	let resolve!: (value?: any) => void;
	let reject!: (error: any) => void;
	const promise = new Promise<any>((res, rej) => {
		resolve = res;
		reject = rej;
	});
	return { promise, resolve, reject };
}

function scenario(name: string, inputs: any, expected: any, normalizations: string[] = []) {
	return { name, inputs, expected, observed: expected, normalizations };
}

const artifacts: Array<{ path: string; sourceEndpoints: string[]; scenarios: any[] }> = [];

// ---------------------------------------------------------------------------
// Harness settings defaults (cch_durable:harness-settings-defaults).
// ---------------------------------------------------------------------------
{
	const agent = await import(pathToFileURL(path.join(piCheckout, "packages/durable/src/harness/agent.ts")).href);
	const scenarios: any[] = [];
	{
		const resolvedDefault = agent.resolveSettings(undefined);
		const partial = agent.resolveSettings({
			retry: { maxRetries: 1 },
			progress: { partialIntervalMs: 250 },
			steeringMode: "queue-all",
		});
		const cleared = agent.resolveSettings({ retry: { enabled: false, maxRetries: undefined } } as any);
		scenarios.push(
			scenario(
				"the harness settings defaults resolve with the frozen object-merge rules",
				{
					partialOverride: { retry: { maxRetries: 1 }, progress: { partialIntervalMs: 250 }, steeringMode: "queue-all" },
					explicitUndefined: { retry: { enabled: false, maxRetries: undefined } },
				},
				{
					defaults: {
						retry: agent.DEFAULT_RETRY_POLICY,
						compaction: agent.DEFAULT_COMPACTION_POLICY,
						progress: agent.DEFAULT_PROGRESS_POLICY,
						steeringMode: "one-at-a-time",
						followUpMode: "one-at-a-time",
						toolExecution: "parallel",
					},
					partialOverride: {
						retry: partial.retry,
						progress: partial.progress,
						steeringMode: partial.steeringMode,
						followUpMode: partial.followUpMode,
					},
					explicitUndefinedKeepsDefaultMaxRetries: cleared.retry.maxRetries,
					explicitUndefinedEnabled: cleared.retry.enabled,
				},
			),
		);
	}
	artifacts.push({
		path: "settings.json",
		sourceEndpoints: [
			"packages/durable/src/harness/agent.ts:DEFAULT_RETRY_POLICY,DEFAULT_COMPACTION_POLICY,DEFAULT_PROGRESS_POLICY,resolveSettings",
		],
		scenarios,
	});
}

// ---------------------------------------------------------------------------
// Chat helpers mirroring the frozen harness tests (faux provider, one turn).
// ---------------------------------------------------------------------------
type ChatBundle = {
	harness: any;
	root: any;
	setup: any;
};

const text = (label: string, tokens: number): string => `${label} ${"x".repeat(Math.max(0, tokens * 4 - label.length - 1))}`;
const answer = (content: string) => fauxAssistantMessage(content);
const failure = (errorMessage: string) => fauxAssistantMessage("", { stopReason: "error", errorMessage });
const OVERFLOW = "prompt is too long: 250000 tokens > 200000 maximum";

function isSummaryRequest(messages: readonly any[]): boolean {
	const first = messages[0];
	return (
		first?.role === "system" &&
		typeof first.content === "string" &&
		first.content.startsWith("You are a context summarization assistant")
	);
}

async function openChatWith(options: { contextWindow?: number; maxTokens?: number } = {}): Promise<ChatBundle> {
	const setup = chatSetup({
		models: [{ id: "faux-1", contextWindow: options.contextWindow ?? 100_000, maxTokens: options.maxTokens ?? 900 }],
	});
	const faux = setup.faux;
	const script: { agent: any[]; summaries: any[]; agentRequests: any[]; summaryRequests: any[] } = {
		agent: [],
		summaries: [],
		agentRequests: [],
		summaryRequests: [],
	};
	faux.setResponses(
		Array.from(
			{ length: 500 },
			() =>
				async (transcript: any, streamOptions: any, _state: unknown, model: { id: string }) => {
					const messages = [...transcript.messages];
					const summary = isSummaryRequest(messages);
					const request = { messages, options: streamOptions, model: model.id };
					(summary ? script.summaryRequests : script.agentRequests).push(request);
					const step = (summary ? script.summaries : script.agent).shift();
					if (step === undefined) throw new Error(`No scripted ${summary ? "summary" : "agent"} response`);
					return typeof step === "function" ? step(request) : step;
				},
		),
	);
	addSection(setup.registry, "preamble", () => "You are helpful.", { tag: false });
	const { harness, root } = await openChat(new MemoryStorage(), setup);
	setup.settings.stream = { cacheRetention: "none" };
	harness.resume();
	return { harness, root, setup: { ...setup, faux: script } };
}

async function turn(chat: ChatBundle, userText: string, reply: string): Promise<void> {
	chat.setup.faux.agent.push(answer(reply));
	const submission = await chat.root.submit({ type: "input", content: userText }, context);
	const waited = await submission.wait(context);
	if (waited.status !== "done") throw new Error(`turn did not settle done: ${JSON.stringify(waited)}`);
}

async function history3(chat: ChatBundle): Promise<void> {
	await turn(chat, text("u1", 100), text("a1", 100));
	await turn(chat, text("u2", 100), text("a2", 100));
	await turn(chat, text("u3", 100), text("a3", 100));
}

const compactionRecords = async (chat: ChatBundle) =>
	(await allEntries(chat.root)).filter((record: any) => record.kind === "pi.compaction");
const liveState = async (chat: ChatBundle) =>
	(await chat.harness.snapshot(LiveDoc, chat.root.id, context)) ?? {};
const entryKinds = async (chat: ChatBundle) => (await allEntries(chat.root)).map((record: any) => record.kind);

// ---------------------------------------------------------------------------
// Background compaction (cch_durable:background-compaction) and overflow retry
// (cch_durable:overflow-compaction-retry).
// ---------------------------------------------------------------------------
{
	const scenarios: any[] = [];

	// 1. Background threshold compaction never blocks the run; idle and abort ignore it.
	{
		const chat = await openChatWith({ contextWindow: 2000 });
		await history3(chat);
		chat.setup.settings.compaction = {
			enabled: true,
			reserveTokens: 300,
			keepRecentTokens: 150,
			backgroundTokens: 1000,
		};
		const gate = deferred();
		const reached = deferred();
		const gatedSummary = async () => {
			reached.resolve();
			await gate.promise;
			return fauxAssistantMessage("SUMMARY");
		};
		chat.setup.faux.summaries.push(gatedSummary());
		chat.setup.faux.agent.push(answer(text("a4", 100)));
		const submission = await chat.root.submit({ type: "input", content: text("u4", 100) }, context);
		const waited = await submission.wait(context);
		await reached.promise;
		const inspection = await chat.harness.inspect(context);
		const task: any = inspection.tasks.map((item: any) => item.record).find((record: any) => record.kind === "pi.compaction");
		const liveAfterTurn = await liveState(chat);
		await chat.root.waitForIdle(context);
		const stillRunningAfterIdle = (await chat.harness.getTask(task.id, context))?.state.status;
		await chat.root.abort(context);
		const stillRunningAfterAbort = (await chat.harness.getTask(task.id, context))?.state.status;
		gate.resolve();
		const outcome = (await chat.harness.waitForTask(task.id, context)).state.outcome;
		scenarios.push(
			scenario(
				"a background compaction task starts above the threshold without blocking the run",
				{
					policy: { enabled: true, reserveTokens: 300, keepRecentTokens: 150, backgroundTokens: 1000 },
					turn: "100-token user over a 2000-token window with a gated summary",
				},
				{
					submissionStatus: waited.status,
					task: { background: task.background ?? null, reason: task.input?.reason ?? null, owner: task.owner ?? null },
					liveCompactions: liveAfterTurn.compactions ?? null,
					statusAfterWaitForIdle: stillRunningAfterIdle,
					statusAfterAbort: stillRunningAfterAbort,
					outcomeStatus: outcome.status,
				},
				["wall-clock timing of the gated summary"],
			),
		);
		await chat.harness.close(context);
	}

	// 2. Overflow compaction retries the original request once with the summary.
	{
		const chat = await openChatWith();
		await history3(chat);
		chat.setup.settings.compaction = { enabled: true, reserveTokens: 1000, keepRecentTokens: 150, backgroundTokens: 0 };
		chat.setup.faux.summaries.push(answer("SUMMARY"));
		let attempt: number | undefined;
		chat.setup.faux.agent.push(failure(OVERFLOW));
		chat.setup.faux.agent.push(async () => {
			attempt = (await liveState(chat)).generation?.attempt;
			return answer("fits");
		});
		const input = await chat.root.submit({ type: "input", content: text("u4", 100) }, context);
		const waited = await input.wait(context);
		const compactions = await compactionRecords(chat);
		const retryRequest = chat.setup.faux.agentRequests.at(-1)?.messages ?? [];
		const retryHasErrorTurn = retryRequest.some((message: any) => message.role === "assistant" && message.stopReason === "error");
		scenarios.push(
			scenario(
				"overflow compaction retries the original request once after the compaction commits",
				{ failure: OVERFLOW, history: "3x100-token turns" },
				{
					status: waited.status,
					attemptAfterRetry: attempt ?? null,
					lastFiveKinds: (await entryKinds(chat)).slice(-5),
					compactionReasons: compactions.map((record: any) => record.data?.reason ?? null),
					retrySkipsErrorTurn: !retryHasErrorTurn,
					retryCarriesSummary:
						typeof retryRequest[1]?.content === "string" ? retryRequest[1].content.includes("SUMMARY") : false,
				},
			),
		);
		await chat.harness.close(context);
	}

	artifacts.push({
		path: "compaction.json",
		sourceEndpoints: [
			"packages/durable/src/harness/compaction.ts:CompactionTask,selectCut",
			"packages/durable/src/harness/generation.ts:GenerationTask",
			"packages/durable/test/harness-compaction.test.ts:background threshold,overflow",
		],
		scenarios,
	});
}

// ---------------------------------------------------------------------------
// Ownership, background boundary and idle waits (cch_durable:background-ownership-boundary,
// cch_durable:idle-waits).
// ---------------------------------------------------------------------------
{
	const scenarios: any[] = [];

	const oneStep = (name: string, run: any, abort?: any) =>
		defineTask({
			name,
			version: 1,
			initial: () => ({ phase: "run" }),
			phases: {
				run: async (task: any, runtime: any, ctx: any) => {
					await run(task, runtime, ctx);
				},
			},
			abort: abort ?? (async (_task: any, runtime: any, ctx: any) => {
				await runtime.commit(() => ({ status: "terminal", outcome: { status: "aborted", reason: "test" } }), ctx);
			}),
		});
	const gatedTask = (name: string, gate: Promise<any>) =>
		oneStep(name, async (_task: any, runtime: any, ctx: any) => {
			await gate;
			await runtime.commit(() => ({ status: "terminal", outcome: { status: "completed", result: null } }), ctx);
		});

	async function openWithTasks(tasks: any[]) {
		const storage = new MemoryStorage();
		const registry = createRegistry();
		for (const t of tasks) {
			registry.install(defineExtension({ name: `ext:${t.definition.name}`, tasks: [t] }));
		}
		const harness = await Harness.open(storage, { models: (piAi as any).createModels(), registry }, context);
		harness.resume();
		const root = await harness.root(context);
		return { harness, root, storage };
	}

	// 1. A background child survives a conversation abort; a direct abort reaches it.
	{
		const gate = deferred();
		const { harness, root } = await openWithTasks([gatedTask("audit1203.bg", gate.promise)]);
		const childId = await root.commit(
			(tx: any) => tx.createTask(gatedTask("audit1203.bg", gate.promise), null, {
				ownership: { kind: "conversation" },
				background: true,
			}),
			context,
		);
		await root.abort(context);
		const afterConversationAbort = (await harness.getTask(childId, context))?.state.status;
		const direct = harness.abortTask(childId, context);
		while (!(await harness.getTask(childId, context))?.abortRequested) {
			await sessionSupport.flush();
		}
		gate.resolve();
		await direct;
		for (let attempt = 0; attempt < 200; ++attempt) {
			const state = (await harness.getTask(childId, context))?.state;
			if (state?.status === "terminal") break;
			await new Promise((resolve) => setTimeout(resolve, 5));
		}
		const afterDirectAbort = (await harness.getTask(childId, context))?.state;
		scenarios.push(
			scenario(
				"a background child survives a conversation abort while a direct abort reaches it",
				{ placement: { background: true, ownership: "conversation" }, aborts: ["conversation", "direct"] },
				{
					statusAfterConversationAbort: afterConversationAbort,
					statusAfterDirectAbort: afterDirectAbort?.status,
					outcome: afterDirectAbort?.outcome ?? null,
				},
			),
		);
		await harness.close(context);
	}

	// 2. An idle wait resolves with live background work and holds on a live foreground task.
	{
		const bgGate = deferred();
		const fgGate = deferred();
		const { harness, root } = await openWithTasks([
			gatedTask("audit1203.idlebg", bgGate.promise),
			gatedTask("audit1203.idlefg", fgGate.promise),
		]);
		await root.commit(
			(tx: any) => tx.createTask(gatedTask("audit1203.idlebg", bgGate.promise), null, {
				ownership: { kind: "conversation" },
				background: true,
			}),
			context,
		);
		const idleWithBackground = await root.waitForIdle(context);
		const fgId = await root.commit(
			(tx: any) => tx.createTask(gatedTask("audit1203.idlefg", fgGate.promise), null, {
				ownership: { kind: "conversation" },
			}),
			context,
		);
		let idleResolved = false;
		const idleWait = root.waitForIdle(context).then(() => {
			idleResolved = true;
		});
		await new Promise((resolve) => setTimeout(resolve, 30));
		const heldWhileForegroundLive = !idleResolved;
		fgGate.resolve();
		bgGate.resolve();
		await idleWait;
		scenarios.push(
			scenario(
				"an idle wait holds on a live foreground task while background work stays live at the v1.3.0 baseline",
				{ tasks: ["background gated", "foreground gated"], waits: ["waitForIdle"] },
				{
					idleWithBackground: idleWithBackground ?? "resolved",
					heldWhileForegroundLive,
					resolvedAfterForegroundSettled: idleResolved,
				},
				["30ms settle observation window for the held idle wait"],
			),
		);
		await harness.close(context);
	}

	artifacts.push({
		path: "ownership.json",
		sourceEndpoints: [
			"packages/durable/src/types.ts:TaskOptions.background,ConversationAbortOptions",
			"packages/durable/src/harness/scheduler.ts:TaskScheduler,#loadScopes,#walkDown,#settleIdle,waitForTask",
			"packages/durable/test/harness-ownership.test.ts:background,idle",
		],
		scenarios,
	});
}

// ---------------------------------------------------------------------------
// Task hooks, outcomes, scheduler outcomes and versioning
// (cch_durable:task-hooks, cch_durable:task-outcomes, cch_durable:task-scheduler-outcomes,
// cch_durable:task-versioning).
// ---------------------------------------------------------------------------
{
	const { openTasks, completed, abortedWith } = taskSupport as Record<string, any>;
	const { addHooks, addTask } = harnessSupport as Record<string, any>;
	const scenarios: any[] = [];

	const stepTask = (name: string, phases: any, abort?: any, extra: any = {}) =>
		defineTask({
			name,
			version: extra.version ?? 1,
			initial: () => ({ phase: "run" }),
			phases,
			abort: abort ?? (async (_task: any, runtime: any, ctx: any) => {
				await runtime.commit(() => abortedWith("test"), ctx);
			}),
			...extra.rest,
		});

	// 1. Task hooks run in extension order and an isolated hook error reports without vetoing.
	{
		const pings: string[] = [];
		const Phases = defineTask({
			name: "audit1203.hooks",
			version: 1,
			initial: () => ({ phase: "run" }),
			phases: {
				run: async (_task: any, runtime: any, ctx: any) => {
					await runtime.hooks.each("ping", (handler: any) => handler());
					await runtime.commit(() => completed(null), ctx);
				},
			},
			abort: async (_task: any, runtime: any, ctx: any) => {
				await runtime.commit(() => abortedWith("test"), ctx);
			},
		});
		const { harness, registry, reports } = await openTasks(new MemoryStorage(), []);
		addTask(registry, Phases);
		addHooks(registry, Phases, { ping: () => void pings.push("first") });
		addHooks(registry, Phases, {
			ping: () => {
				pings.push("throwing");
				throw new Error("hook exploded");
			},
		});
		const root = await harness.root(context);
		harness.resume();
		const id = await root.commit(
			(tx: any) => tx.createTask(Phases, null, { ownership: { kind: "conversation" } }),
			context,
		);
		await harness.waitForTask(id, context);
		scenarios.push(
			scenario(
				"task hooks run in extension order and a throwing hook reports without vetoing the task",
				{ hooks: ["first", "throwing"], task: "audit1203.hooks" },
				{ pings, outcome: (await harness.getTask(id, context))?.state.outcome ?? null, reports: reports.map((r: any) => String(r)) },
			),
		);
		await harness.close(context);
	}

	// 2. A completing owner holds until its children drain, committing the terminal outcome.
	{
		const gate = deferred();
		const Child = stepTask("audit1203.child", {
			run: async (_task: any, runtime: any, ctx: any) => {
				await gate.promise;
				await runtime.commit(() => completed("child-result"), ctx);
			},
		});
		const Parent = stepTask("audit1203.parent", {
			run: async (task: any, runtime: any, ctx: any) => {
				await runtime.commit(async (tx: any) => {
					await tx.createTask(Child, null, { ownership: { kind: "task", taskId: task.id } });
				}, ctx);
				await runtime.commit(() => completed("parent-result"), ctx);
			},
		});
		const { harness } = await openTasks(new MemoryStorage(), [Child, Parent]);
		const root = await harness.root(context);
		harness.resume();
		const parentId = await root.commit(
			(tx: any) => tx.createTask(Parent, null, { ownership: { kind: "conversation" } }),
			context,
		);
		await new Promise((resolve) => setTimeout(resolve, 30));
		const parentBeforeDrain = (await harness.getTask(parentId, context))?.state.status;
		gate.resolve();
		const parent = await harness.waitForTask(parentId, context);
		scenarios.push(
			scenario(
				"a completing owner holds until its children drain and commits the terminal outcome",
				{ parent: "audit1203.parent", child: "audit1203.child gated" },
				{
					parentStatusBeforeChildDrain: parentBeforeDrain,
					parentOutcome: parent.state.outcome,
					parentTerminalAfterDrain: parent.state.status,
				},
			),
		);
		await harness.close(context);
	}

	// 3. An aborted task whose definition is missing settles as orphaned with the frozen reason.
	{
		const Orphan = stepTask("test.orphan-scratch", {
			run: async (_task: any, runtime: any, ctx: any) => {
				await runtime.commit(() => completed(null), ctx);
			},
		});
		// The task is created from a definition the registry never installed.
		const { harness } = await openTasks(new MemoryStorage(), []);
		const root = await harness.root(context);
		const scratch = await root.commit(
			(tx: any) => tx.createTask(Orphan, null, { ownership: { kind: "conversation" } }),
			context,
		);
		const mark = await harness.abortTask(scratch, context);
		const after = (await harness.getTask(scratch, context))?.state;
		scenarios.push(
			scenario(
				"an aborted task with a missing definition settles orphaned with the frozen reason",
				{ kind: "test.orphan-scratch", abort: "before registration" },
				{ mark, outcome: after?.outcome ?? null, status: after?.status },
			),
		);
		await harness.close(context);
	}

	// 4. Versioning: migrate success and a too-new definition.
	{
		const versioned = (version: number, result: string, migrate?: any) =>
			defineTask({
				name: "test.versioned",
				version,
				initial: () => ({ phase: "run" }),
				phases: { run: (_task: any, runtime: any, ctx: any) => runtime.commit(() => completed(result), ctx) },
				abort: (_task: any, runtime: any, ctx: any) => runtime.commit(() => abortedWith(result), ctx),
				...(migrate === undefined ? {} : { migrate }),
			});
		// A too-new record: version 1 record, version 2 definition without a migrate hook.
		const sqliteNode = await import(
			pathToFileURL(path.join(piCheckout, "packages/durable/src/storage/sqlite/node.ts")).href
		);
		const mkdtempSync = (await import("node:fs")).mkdtempSync;
		const tmpdir = (await import("node:os")).tmpdir;
		const storagePath = path.join(mkdtempSync(path.join(tmpdir(), "audit1203-version-")), "session.sqlite");
		let opened = await openTasks(await sqliteNode.openNodeSqliteStorage(storagePath), []);
		const root = await opened.harness.root(context);
		const id = await root.commit(
			(tx: any) => tx.createTask(versioned(1, "v1"), null, { ownership: { kind: "conversation" } }),
			context,
		);
		await opened.harness.close(context);
		opened = await openTasks(await sqliteNode.openNodeSqliteStorage(storagePath), [versioned(2, "v2")]);
		opened.harness.resume();
		for (let attempt = 0; attempt < 200; ++attempt) {
			if (opened.reports.length > 0) break;
			await new Promise((resolve) => setTimeout(resolve, 5));
		}
		const tooNew = {
			report: String(opened.reports[0] ?? ""),
			status: (await opened.harness.getTask(id, context))?.state.status,
			version: (await opened.harness.getTask(id, context))?.version,
		};
		await opened.harness.close(context);

		// A migration success: version 2 with a migrate hook carries the checkpoint forward.
		const storagePath2 = path.join(mkdtempSync(path.join(tmpdir(), "audit1203-migrate-")), "session.sqlite");
		let second = await openTasks(await sqliteNode.openNodeSqliteStorage(storagePath2), []);
		const root2 = await second.harness.root(context);
		const id2 = await root2.commit(
			(tx: any) => tx.createTask(versioned(1, "v1"), null, { ownership: { kind: "conversation" } }),
			context,
		);
		await second.harness.close(context);
		const migrations: number[] = [];
		second = await openTasks(await sqliteNode.openNodeSqliteStorage(storagePath2), [
			versioned(2, "v2", (_input: any, checkpoint: any, fromVersion: number) => {
				migrations.push(fromVersion);
				return { input: null, checkpoint };
			}),
		]);
		second.harness.resume();
		const receipt = await second.harness.waitForTask(id2, context);
		scenarios.push(
			scenario(
				"a task definition migrates an old-version record and a too-new definition blocks with the frozen report",
				{ recordVersion: 1, definitions: ["version 2 without migrate", "version 2 with migrate"] },
				{
					tooNew,
					migrated: { fromVersions: migrations, outcome: receipt.state.outcome, version: receipt.version },
				},
			),
		);
		await second.harness.close(context);
	}

	artifacts.push({
		path: "tasks.json",
		sourceEndpoints: [
			"packages/durable/src/types.ts:TaskDefinition.hooks,TaskDefinition.migrate,TaskOutcome,TaskState,TaskRecord",
			"packages/durable/src/harness/define.ts:defineTask,hook",
			"packages/durable/src/harness/scheduler.ts:TaskScheduler,#decide,#terminate,#finalize",
			"packages/durable/test/harness-tasks.test.ts:phase hooks,outcomes,migration",
			"packages/durable/test/harness-tasks-recovery.test.ts:orphaned,migration failure,too-new",
		],
		scenarios,
	});
}

// ---------------------------------------------------------------------------
// Tool search persistence (cch_durable:toolsearch-persistence).
// ---------------------------------------------------------------------------
{
	const scenarios: any[] = [];
	const { addTool } = harnessSupport as Record<string, any>;
	const toolSearch = await import(
		pathToFileURL(path.join(piCheckout, "packages/durable/src/tool-search/tool-search.ts")).href
	);
	const fauxText = (piAi as any).fauxText;
	const callTool = (name: string, args: any = {}, id = "c1") =>
		fauxAssistantMessage([fauxToolCall(name, args, { id })], { stopReason: "toolUse" });
	const DONE = fauxAssistantMessage([fauxText("done")]);

	// 1. A deferred tool loads through tool_search and persists in AgentDoc modelTools.
	{
		const setup = chatSetup();
		addTool(setup.registry, harnessSupport.tool("alpha", "Alpha tool"));
		addTool(
			setup.registry,
			durableIndex.defineTool({
				name: "github_get_issue",
				description: "Get a GitHub issue by number",
				parameters: (piAi as any).Type.Object({ number: (piAi as any).Type.Optional((piAi as any).Type.Number({ description: "Issue number" })) }),
				deferred: true,
				execute: async () => ({ output: [{ type: "text", text: "github_get_issue ran" }] }),
			}),
		);
		setup.registry.install(toolSearch.createToolSearchExtension());
		setup.faux.setResponses([callTool("tool_search", { query: "github issue", limit: 1 }), DONE]);
		const { harness, root } = await openChat(new MemoryStorage(), setup);
		const offeredBefore = (await root.agent(context)).tools.map((each: any) => each.name);
		const deferredBefore = (await root.agent(context)).deferred.map((each: any) => each.name);
		await (await root.submit({ type: "input", content: "go" }, context)).wait(context);
		const entries = await allEntries(root);
		const searchEntry = entries.find((entry: any) => entry.kind === "pi.tool-result");
		const searchText = searchEntry
			? (searchEntry.model![0] as any).content.map((item: any) => item.text).join("")
			: "";
		const modelTools = (await harness.snapshot((durableIndex as any).AgentDoc, root.id, context))?.modelTools ?? null;
		const offeredAfter = (await root.agent(context)).tools.map((each: any) => each.name);
		scenarios.push(
			scenario(
				"a deferred tool loads through tool_search and persists in the AgentDoc model tools",
				{ tools: ["alpha", "github_get_issue (deferred)"], search: { query: "github issue", limit: 1 } },
				{
					offeredBefore,
					deferredBefore,
					searchText,
					modelTools,
					offeredAfter,
				},
			),
		);
		await harness.close(context);
	}

	artifacts.push({
		path: "tool-search.json",
		sourceEndpoints: [
			"packages/durable/src/tool-search/tool-search.ts:createToolSearchTool,createToolSearchExtension",
			"packages/durable/src/harness/agent.ts:addTools",
			"packages/durable/test/tool-search.test.ts:offers a deferred tool only once tool_search loads it",
		],
		scenarios,
	});
}

// ---------------------------------------------------------------------------
// Steering boundary placement, follow-up drain modes and submission queues
// (cch_durable:steering-boundary-placement, cch_durable:steering-followup,
// cch_durable:submission-queues).
// ---------------------------------------------------------------------------
{
	const scenarios: any[] = [];
	const fauxText = (piAi as any).fauxText;
	const gatedAnswer = (content: string) => {
		const gate = deferred();
		const reached = deferred();
		const step = async ({ options }: any) => {
			reached.resolve();
			await gate.promise;
			return fauxAssistantMessage(content);
		};
		return { step, reached, release: () => gate.resolve() };
	};
	const transcript = (entries: any[]) =>
		entries.map((entry) =>
			entry.kind === "pi.user"
				? `pi.user:${(entry.model![0] as any).content}`
				: entry.kind === "pi.assistant"
					? `pi.assistant:${(entry.model![0] as any).content.find((item: any) => item.type === "text")?.text ?? ""}`
				: entry.kind,
		);

	// 1. A steer submitted during a busy run lands at the postTools boundary.
	{
		const setup = chatSetup();
		const first = gatedAnswer("first");
		setup.faux.setResponses([first.step, fauxAssistantMessage([fauxText("second")])]);
		const { harness, root } = await openChat(new MemoryStorage(), setup);
		const input = await root.submit({ type: "input", content: "a" }, context);
		await first.reached.promise;
		const steer = await root.submit({ type: "input", content: "s", whenBusy: "steer" }, context);
		first.release();
		const placed = await steer.wait(context);
		const inputStatus = await input.wait(context);
		scenarios.push(
			scenario(
				"a steer submitted while the run is busy lands at the postTools boundary in one run",
				{ first: "a", steer: "s (whenBusy steer)" },
				{
					transcript: transcript(await allEntries(root)),
					steerStatus: placed.status,
					inputStatus: inputStatus.status,
				},
		),
		);
		await harness.close(context);
	}

	// 2. Busy and idle follow-up submissions drain one per run by default.
	{
		const setup = chatSetup();
		const first = gatedAnswer("first");
		setup.faux.setResponses([first.step, fauxAssistantMessage([fauxText("second")]), fauxAssistantMessage([fauxText("third")])]);
		const { harness, root } = await openChat(new MemoryStorage(), setup);
		const input = await root.submit({ type: "input", content: "a" }, context);
		await first.reached.promise;
		const f1 = await root.submit({ type: "input", content: "f1", whenBusy: "followUp" }, context);
		const f2 = await root.submit({ type: "input", content: "f2", whenBusy: "followUp" }, context);
		first.release();
		await f2.wait(context);
		scenarios.push(
			scenario(
				"busy follow-up submissions queue and drain one successor run at a time by default",
				{ first: "a", followUps: ["f1", "f2"], followUpMode: "default (one-at-a-time)" },
				{
					transcript: transcript(await allEntries(root)),
					lastStatuses: [await f1.wait(context), await f2.wait(context)].map((s: any) => s.status),
				},
		),
		);
		await harness.close(context);
	}

	// 3. A known request ID returns the existing submission without queueing twice.
	{
		const setup = chatSetup();
		const first = gatedAnswer("first");
		setup.faux.setResponses([first.step, fauxAssistantMessage([fauxText("second")])]);
		const { harness, root } = await openChat(new MemoryStorage(), setup);
		const requestId = "audit1203-dup-request";
		const one = await root.submit({ type: "input", content: "a", requestId }, context);
		await first.reached.promise;
		const two = await root.submit({ type: "input", content: "a-changed", requestId }, context);
		first.release();
		const oneStatus = await one.wait(context);
		const twoStatus = await two.wait(context);
		scenarios.push(
			scenario(
				"a duplicate request ID returns the existing submission without queueing the payload twice",
				{ requestId, payloads: ["a", "a-changed"], whenBusy: "second submit during the first run" },
				{
					sameSubmission: one.id === two.id,
					oneStatus: oneStatus.status,
					twoStatus: twoStatus.status,
					transcript: transcript(await allEntries(root)),
				},
			),
		);
		await harness.close(context);
	}

	artifacts.push({
		path: "steering.json",
		sourceEndpoints: [
			"packages/durable/src/harness/inbox.ts:applyBoundary,prepareBoundary,InboxDoc",
			"packages/durable/src/harness/submissions.ts:Submissions",
			"packages/durable/src/types.ts:QueueMode",
			"packages/durable/src/harness/agent.ts:resolveSettings,steeringMode,followUpMode",
			"packages/durable/test/harness-inbox.test.ts:boundaries,harness-submissions.test.ts:admission",
		],
		scenarios,
	});
}

// ---------------------------------------------------------------------------
// Native extension seam: registration, wrapping and run triggering
// (cch_durable:native-run-triggering).
// ---------------------------------------------------------------------------
{
	const scenarios: any[] = [];
	const fauxText = (piAi as any).fauxText;
	const callTool = (name: string, args: any = {}, id = "c1") =>
		fauxAssistantMessage([fauxToolCall(name, args, { id })], { stopReason: "toolUse" });
	const DONE = fauxAssistantMessage([fauxText("done")]);

	// 1. An extension registers tool hooks and a beforeRequest hook; its hooks
	//    fire on the triggered run in registration order and a follow-up run is
	//    triggerable through the public submit seam.
	{
		const { addTool } = harnessSupport as Record<string, any>;
		const events: string[] = [];
		const setup = chatSetup();
		addTool(setup.registry, harnessSupport.tool("audit_hooked", "Hooked tool"));
		const registry = setup.registry;
		registry.install(
			defineExtension({
				name: "audit1203.ext",
				tools: [],
				hooks: [
					{
						task: (durableIndex as any).GenerationTask.definition.name,
						handlers: {
							beforeRequest: async (request: any) => {
								events.push("beforeRequest");
								return { messages: [...request.messages, { role: "system", content: "marker:extension", timestamp: 0 }] };
							},
						},
					},
					{
						task: (durableIndex as any).ToolTask.definition.name,
						handlers: {
							beforeTool: async (callItem: any) => {
								events.push(`beforeTool:${callItem.name}`);
								return undefined;
							},
							afterTool: async (result: any) => {
								events.push("afterTool");
								return undefined;
							},
						},
					},
				],
			}),
		);
		setup.faux.setResponses([callTool("audit_hooked"), DONE]);
		const { harness, root } = await openChat(new MemoryStorage(), setup);
		await (await root.submit({ type: "input", content: "go" }, context)).wait(context);
		// A follow-up run through the same registered extension.
		setup.faux.setResponses([DONE]);
		await (await root.submit({ type: "input", content: "again" }, context)).wait(context);
		const requests = (setup.faux as any).requests ?? [];
		scenarios.push(
			scenario(
				"a registered extension fires its generation and tool hooks and the run triggers again through the public submit seam",
				{ extension: "audit1203.ext", runs: ["go (tool call)", "again"] },
				{
					events,
					runCount: events.filter((event) => event === "beforeRequest").length,
				},
				["request payload capture is not introspected; hook firing order and count are the observation"],
			),
		);
		void requests;
		await harness.close(context);
	}

	artifacts.push({
		path: "extensions.json",
		sourceEndpoints: [
			"packages/durable/src/harness/types.ts:Extension,GenerationHooks,ToolHooks,HookApi",
			"packages/durable/src/harness/registry.ts:createRegistry",
			"packages/durable/src/harness/define.ts:defineExtension,hook",
		],
		scenarios,
	});
}

// ---------------------------------------------------------------------------
// Node execution environment watch (cch_durable:node-env-watch).
// ---------------------------------------------------------------------------
{
	const scenarios: any[] = [];
	const { NodeExecutionEnv } = (await import(
		pathToFileURL(path.join(piCheckout, "packages/durable/src/env/node.ts")).href
	)) as Record<string, any>;
	const { mkdtempSync } = await import("node:fs");
	const { tmpdir } = await import("node:os");

	// 1. One real change is reported and close stops the callbacks.
	{
		const cwd = mkdtempSync(path.join(tmpdir(), "audit1203-watch-"));
		const env = new NodeExecutionEnv({ cwd });
		const changes: any[] = [];
		const getOrThrow = (result: any) => {
			if (result?.ok === false || result?.error) throw new Error(JSON.stringify(result.error));
			return result?.value ?? result;
		};
		const handle = getOrThrow(
			await env.watch([{ path: "tree", recursive: true }], (change) => changes.push(change), context),
		);
		await env.createDir("tree/sub", { recursive: true }, context);
		await env.writeFile("tree/sub/file.txt", "hello", context);
		const deadline = Date.now() + 3000;
		while (changes.length === 0 && Date.now() < deadline) {
			await new Promise((resolve) => setTimeout(resolve, 20));
		}
		const changeCount = changes.length;
		await handle.close(context);
		await env.writeFile("tree/sub/after.txt", "late", context);
		await new Promise((resolve) => setTimeout(resolve, 120));
		scenarios.push(
			scenario(
				"the node execution environment watch reports a real change and close stops the callbacks",
				{ targets: [{ path: "tree", recursive: true }], writes: ["tree/sub/file.txt", "after close: tree/sub/after.txt"] },
				{
					changeKinds: changes.slice(0, 4).map((change: any) => ("created" in change ? "created" : "updated")),
					changeCount,
					changesAfterClose: changes.length - changeCount,
				},
				["filesystem watch delivery timing; the counts and the after-close silence are the observations"],
			),
		);
	}

	artifacts.push({
		path: "env-watch.json",
		sourceEndpoints: [
			"packages/durable/src/env/index.ts:ExecutionEnv.watch,WatchTarget,WatchChange",
			"packages/durable/src/env/node.ts:NodeExecutionEnv.watch",
		],
		scenarios,
	});
}

// ---------------------------------------------------------------------------
// Built-in bash tool limits (cch_durable:builtin-tool-limits).
// ---------------------------------------------------------------------------
{
	const scenarios: any[] = [];
	const { NodeExecutionEnv } = (await import(
		pathToFileURL(path.join(piCheckout, "packages/durable/src/env/node.ts")).href
	)) as Record<string, any>;
	const { createBashTool } = (await import(
		pathToFileURL(path.join(piCheckout, "packages/durable/src/tools/bash.ts")).href
	)) as Record<string, any>;
	const truncate = (await import(
		pathToFileURL(path.join(piCheckout, "packages/durable/src/truncate.ts")).href
	)) as Record<string, any>;
	const { mkdtempSync } = await import("node:fs");
	const { tmpdir } = await import("node:os");

	// 1. Over-limit line output truncates to the frozen caps and spills the full text.
	{
		const setup = chatSetup();
		const fauxText = (piAi as any).fauxText;
		const { addTool } = harnessSupport as Record<string, any>;
		addTool(setup.registry, createBashTool());
		setup.faux.setResponses([
			fauxAssistantMessage([(piAi as any).fauxToolCall("bash", { command: "seq 1 3000" }, { id: "c1" })], { stopReason: "toolUse" }),
			fauxAssistantMessage([fauxText("done")]),
		]);
		const cwd = mkdtempSync(path.join(tmpdir(), "audit1203-bash-"));
		const { harness, root } = await openChat(new MemoryStorage(), setup, { env: new NodeExecutionEnv({ cwd }) });
		await (await root.submit({ type: "input", content: "go" }, context)).wait(context);
		const entries = await allEntries(root);
		const toolResult: any = entries.find((entry: any) => entry.kind === "pi.tool-result");
		const text: string = toolResult.model[0].content.map((item: any) => item.text).join("");
		const lines = text.split("\n");
		const diagnostics = lines.filter((line) => line.startsWith("[info]") || line.startsWith("[warn]"));
		scenarios.push(
			scenario(
				"the durable bash tool truncates over-limit output and spills the full text with frozen diagnostics",
				{ command: "seq 1 3000", limits: { lines: truncate.DEFAULT_MAX_LINES, bytes: truncate.DEFAULT_MAX_BYTES } },
				{
					limits: { lines: truncate.DEFAULT_MAX_LINES, bytes: truncate.DEFAULT_MAX_BYTES },
					firstRetainedLine: lines[0],
					lastContentLine: lines.filter((line) => /^\d+$/.test(line)).at(-1) ?? null,
					retainedLineCount: lines.filter((line) => /^\d+$/.test(line)).length,
					diagnostics,
					isError: !!toolResult.model[0].isError,
				},
				["the spill file path inside the [info] Full output diagnostic is a random temp path"],
			),
		);
		await harness.close(context);
	}

	artifacts.push({
		path: "tool-limits.json",
		sourceEndpoints: [
			"packages/durable/src/tools/bash.ts:createBashTool,runCommand",
			"packages/durable/src/truncate.ts:DEFAULT_MAX_BYTES,DEFAULT_MAX_LINES",
		],
		scenarios,
	});
}

// ---------------------------------------------------------------------------
// Durable codemode (cch_durable:codemode-limits, codemode-models,
// cch_durable:codemode-output, cch_durable:codemode-replay).
// ---------------------------------------------------------------------------
{
	const scenarios: any[] = [];
	const fauxText = (piAi as any).fauxText;
	const callCodemode = (code: string) =>
		fauxAssistantMessage([fauxToolCall("codemode", { code }, { id: "c1" })], { stopReason: "toolUse" });
	const DONE = fauxAssistantMessage([fauxText("done")]);
	const { CodemodeSandbox } = (await import("@earendil-works/pi-codemode")) as Record<string, any>;
	const codemodeModule = (await import(
		pathToFileURL(path.join(piCheckout, "packages/durable/src/codemode/codemode.ts")).href
	)) as Record<string, any>;
	const { addTool } = harnessSupport as Record<string, any>;
	const toolReg = (name: string, execute: any, replay: "safe" | "unsafe" = "unsafe") =>
		durableIndex.defineTool({
			name,
			description: name,
			parameters: (piAi as any).Type.Object({}),
			execute,
			...(replay === "unsafe" ? {} : { replay }),
		});
	const ToolResultEntry = (durableIndex as any).ToolResultEntry;
	const resultTextOf = async (root: any) => {
		const entry = (await allEntries(root)).find((candidate: any) => ToolResultEntry.is(candidate));
		const message: any = entry?.model?.[0];
		if (message?.role !== "toolResult") return { text: "", isError: false };
		return {
			text: message.content
				.map((item: any) => (item.type === "text" ? item.text : ""))
				.join("")
				.replace(/^Script (completed|failed)\nWall time [\d.]+ seconds\nOutput:\n/, ""),
			isError: !!message.isError,
			rawHeader: /^Script (completed|failed)/.test(message.content.map((item: any) => item.text).join("")),
		};
	};

	// 1. Limits: an over-timeout script is interrupted with the frozen timeout error.
	{
		const setup = chatSetup();
		addTool(
			setup.registry,
			codemodeModule.createCodemodeTool({ sandbox: new CodemodeSandbox({ memoryLimitBytes: 64 * 1024 * 1024 }) }),
		);
		setup.faux.setResponses([callCodemode('// @options: {"timeout_ms": 400}\nwhile (true) {}'), DONE]);
		const { harness, root } = await openChat(new MemoryStorage(), setup);
		await (await root.submit({ type: "input", content: "go" }, context)).wait(context);
		const result = await resultTextOf(root);
		scenarios.push(
			scenario(
				"a durable codemode run over its timeout is interrupted with the frozen error, not a false success",
				{ options: { timeout_ms: 400 }, script: "while (true) {}" },
				{ result: { text: result.text, isError: result.isError } },
				["wall-clock timeout window"],
			),
		);
		await harness.close(context);
	}

	// 2. Models: the models globals read as objects behind the capability gate and are absent when disabled.
	{
		const setup = chatSetup();
		addTool(
			setup.registry,
			codemodeModule.createCodemodeTool({ sandbox: new CodemodeSandbox({ memoryLimitBytes: 64 * 1024 * 1024 }) }),
		);
		setup.faux.setResponses([
			callCodemode('return [typeof models, typeof models.classify, typeof models.generateImages]'),
			DONE,
		]);
		const { harness, root } = await openChat(new MemoryStorage(), setup);
		await (await root.submit({ type: "input", content: "go" }, context)).wait(context);
		const enabled = await resultTextOf(root);
		await harness.close(context);

		const setupOff = chatSetup();
		addTool(
			setupOff.registry,
			codemodeModule.createCodemodeTool({
				sandbox: new CodemodeSandbox({ memoryLimitBytes: 64 * 1024 * 1024 }),
				models: false,
			}),
		);
		setupOff.faux.setResponses([
			callCodemode('return [typeof models, typeof models.classify]'),
			DONE,
		]);
		const off = await openChat(new MemoryStorage(), setupOff);
		await (await off.root.submit({ type: "input", content: "go" }, context)).wait(context);
		const disabled = await resultTextOf(off.root);
		await off.harness.close(context);
		scenarios.push(
			scenario(
				"the codemode models globals are present behind the capability gate and absent when models are disabled",
				{ script: "typeof models / models.classify / models.generateImages", models: ["default", false] },
				{ enabled: enabled.text, disabled: disabled.text },
			),
		);
	}

	// 3. Output: ordered text output and the returned value format through the durable tool.
	{
		const setup = chatSetup();
		addTool(
			setup.registry,
			codemodeModule.createCodemodeTool({ sandbox: new CodemodeSandbox({ memoryLimitBytes: 64 * 1024 * 1024 }) }),
		);
		setup.faux.setResponses([callCodemode('text("one"); text("two"); return 7'), DONE]);
		const { harness, root } = await openChat(new MemoryStorage(), setup);
		await (await root.submit({ type: "input", content: "go" }, context)).wait(context);
		const result = await resultTextOf(root);
		scenarios.push(
			scenario(
				"durable codemode keeps ordered text output and formats the returned value through the tool result",
				{ script: 'text("one"); text("two"); return 7' },
				{ result: { text: result.text, isError: result.isError, hasHeader: result.rawHeader } },
			),
		);
		await harness.close(context);
	}

	// 4. Replay: a script resumes after a restart without rerunning finished calls.
	{
		const sqliteNode = await import(
			pathToFileURL(path.join(piCheckout, "packages/durable/src/storage/sqlite/node.ts")).href
		);
		const mkdtempSync = (await import("node:fs")).mkdtempSync;
		const tmpdir = (await import("node:os")).tmpdir;
		const storagePath = path.join(mkdtempSync(path.join(tmpdir(), "audit1203-codemode-")), "session.sqlite");
		const setup = chatSetup();
		const sandbox = new CodemodeSandbox({ memoryLimitBytes: 64 * 1024 * 1024 });
		const vm = { starts: 0 };
		const start = sandbox.start.bind(sandbox);
		sandbox.start = (code: string, options: any) => {
			vm.starts++;
			return start(code, options);
		};
		addTool(setup.registry, codemodeModule.createCodemodeTool({ sandbox }));
		const echo = new Map<string, number>();
		addTool(
			setup.registry,
			toolReg("echo", async (args: any) => {
				echo.set(args.text ?? "", (echo.get(args.text ?? "") ?? 0) + 1);
				return { output: [{ type: "text", text: `echo ${args.text ?? ""}` }] };
			}),
		);
		const gate = deferred();
		const reached = deferred();
		const { aborted } = taskSupport as Record<string, any>;
		const holdState = { block: true };
		addTool(
			setup.registry,
			toolReg(
				"hold",
				async (_args: any, _api: any, callContext: any) => {
					if (holdState.block) {
						reached.resolve();
						await aborted(callContext.abortSignal!);
					}
					return { output: [{ type: "text", text: "held" }] };
				},
				"safe",
			),
		);
		setup.faux.setResponses([
			callCodemode('const a = await tools.echo({ text: "a" });\nconst h = await tools.hold({});\nconst b = await tools.echo({ text: "b" });\nreturn [a, h, b];'),
			DONE,
		]);
		let opened = await openChat(await sqliteNode.openNodeSqliteStorage(storagePath), setup);
		const submissionId = (await opened.root.submit({ type: "input", content: "go" }, context)).id;
		await reached.promise;
		await opened.harness.close(context);
		holdState.block = false;
		opened = await openChat(await sqliteNode.openNodeSqliteStorage(storagePath), setup);
		opened.harness.resume();
		const settled = await (await opened.harness.submission(submissionId, context))!.wait(context);
		const result = await resultTextOf(opened.root);
		scenarios.push(
			scenario(
				"a durable codemode run resumes after a restart without rerunning its finished calls",
				{ script: 'echo a; hold; echo b; return [a, h, b]', restart: "close and reopen over sqlite" },
				{
					settledStatus: settled.status,
					result: { text: result.text, isError: result.isError },
					echoCounts: Object.fromEntries(echo),
					vmStarts: vm.starts,
				},
				["wall-clock guest startup"],
			),
		);
		await opened.harness.close(context);
	}

	artifacts.push({
		path: "codemode.json",
		sourceEndpoints: [
			"packages/durable/src/codemode/codemode.ts:createCodemodeTool,CodemodeRunDoc,CodemodeJournalDoc,OUTPUT_LIMITS",
			"packages/durable/src/codemode/models.ts:CATALOG_GLOBALS,MODEL_CALLS",
			"packages/durable/src/codemode/output.ts:formatResult,OutputFile",
			"packages/durable/test/codemode.test.ts:restart replay,models globals,token budget",
		],
		scenarios,
	});
}

// ---------------------------------------------------------------------------
// Tool task cancellation (cch_durable:tool-task-cancellation).
// ---------------------------------------------------------------------------
{
	const scenarios: any[] = [];
	const fauxText = (piAi as any).fauxText;
	const { addTool } = harnessSupport as Record<string, any>;
	const toolReg = (name: string, execute: any) =>
		durableIndex.defineTool({
			name,
			description: name,
			parameters: (piAi as any).Type.Object({}),
			execute,
		});

	// 1. Aborting a running tool task answers with only its durable output and no false result.
	{
		const setup = chatSetup();
		let reached!: () => void;
		const buffered = new Promise<void>((resolve) => {
			reached = resolve;
		});
		addTool(
			setup.registry,
			toolReg("slow", async (_args: any, api: any, callContext: any) => {
				api.output("durable\n");
				await new Promise((resolve) => setTimeout(resolve, 20));
				api.output("buffered\n");
				reached();
				await new Promise((_, reject) =>
					callContext.abortSignal!.addEventListener("abort", () => reject(callContext.abortSignal!.reason)),
				);
				return {};
			}),
		);
		setup.faux.setResponses([
			fauxAssistantMessage([(piAi as any).fauxToolCall("slow", {}, { id: "c1" })], { stopReason: "toolUse" }),
			fauxAssistantMessage([fauxText("done")]),
		]);
		const { harness, root } = await openChat(new MemoryStorage(), setup);
		const submission = await root.submit({ type: "input", content: "go" }, context);
		await buffered;
		const live = (await harness.snapshot((durableIndex as any).LiveDoc, root.id, context))!;
		const taskId = live.tools![0]!.taskId!;
		await harness.abortTask(taskId, context);
		await submission.wait(context);
		const entries = await allEntries(root);
		const toolResult = entries.find((entry: any) => entry.kind === "pi.tool-result");
		const text: string = toolResult
			? (toolResult.model![0] as any).content.map((item: any) => item.text).join("")
			: "";
		scenarios.push(
			scenario(
				"cancelling a running tool task keeps only its durable output and commits no false tool result",
				{ tool: "slow (durable + buffered output, abort mid-run)" },
				{
					toolResultText: text.replace(/\n/g, "|"),
					mentionsAbort: text.includes("was aborted"),
				},
		),
		);
		await harness.close(context);
	}

	artifacts.push({
		path: "tool-task.json",
		sourceEndpoints: [
			"packages/durable/src/harness/tool.ts:ToolTask",
			"packages/durable/src/harness/scheduler.ts:abortConversation",
			"packages/durable/test/harness-tools.test.ts:answers an aborted tool with only its durable output",
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
	ticket: "1203",
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

console.log(`captured ${family} (${manifestArtifacts.length} artifacts) for ${baselineName} (${baseline.revision})`);
