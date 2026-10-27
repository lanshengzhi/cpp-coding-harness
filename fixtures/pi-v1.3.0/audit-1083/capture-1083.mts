import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import { readFileSync } from "node:fs";
import { writeFile } from "node:fs/promises";
import { dirname, join, relative } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
const script = fileURLToPath(import.meta.url);
const repo = join(dirname(script), "../../..");
const checkout = process.env.PI_CHECKOUT!;
const revision = execFileSync("git", ["rev-parse", "HEAD"], { cwd: checkout, encoding: "utf8" }).trim();
if (revision !== "71023aec1546d6d677a65669c5665c0927ce0dd0" || execFileSync("git", ["status", "--porcelain"], { cwd: checkout, encoding: "utf8" }).trim()) throw new Error("frozen checkout is not clean at baseline");
const load = async (path: string) => import(pathToFileURL(join(checkout, path)).href);
const { durable, ai, ctx } = await import("./pi.mts");
const { chatSetup, openChat, allEntries, waitFor } = await load("packages/durable/test/chat-support.ts");
const { addHooks, addSection } = await load("packages/durable/test/harness-support.ts");
const { context } = await load("packages/durable/test/session-support.ts");
const { deferred, aborted } = await load("packages/durable/test/task-support.ts");

/** Text of about `tokens` estimated tokens, starting with `label`. */
const text = (label: string, tokens: number) => `${label} ${"x".repeat(Math.max(0, tokens * 4 - label.length - 1))}`;
const answer = (content: string) => ai.fauxAssistantMessage(content);
const failure = (errorMessage: string) => ai.fauxAssistantMessage("", { stopReason: "error", errorMessage });
const OVERFLOW = "prompt is too long: 250000 tokens > 200000 maximum";
const MANUAL = { enabled: false, reserveTokens: 1000, keepRecentTokens: 150, backgroundTokens: 0 };
const BLOCKING = { enabled: true, reserveTokens: 300, keepRecentTokens: 150, backgroundTokens: 0 };
const BACKGROUND = { enabled: true, reserveTokens: 500, keepRecentTokens: 150, backgroundTokens: 1000 };

const isSummaryRequest = (messages: readonly any[]) => {
	const first = messages[0];
	return first?.role === "system" && typeof first.content === "string" &&
		first.content.startsWith("You are a context summarization assistant");
};

/** Scripted faux model that answers agent and summarization requests from separate queues. */
function scripted(setup: any) {
	const result: any = { agent: [], summaries: [], agentRequests: [], summaryRequests: [], order: [] };
	setup.faux.setResponses(Array.from({ length: 500 }, () => async (transcript: any, options: any, _state: any, model: any) => {
		const request = { messages: [...transcript.messages], options, model: model!.id };
		const summary = isSummaryRequest(request.messages);
		(summary ? result.summaryRequests : result.agentRequests).push(request);
		result.order.push(summary ? "summary" : "agent");
		const step = (summary ? result.summaries : result.agent).shift();
		if (step === undefined) throw new Error(`No scripted ${summary ? "summary" : "agent"} response`);
		return typeof step === "function" ? step(request) : step;
	}));
	return result;
}

async function open(options: { contextWindow: number; policy: any; retry?: any }) {
	const setup = chatSetup({ models: [{ id: "faux-1", contextWindow: options.contextWindow, maxTokens: 900 }] });
	const faux = scripted(setup);
	addSection(setup.registry, "preamble", () => "You are helpful.", { tag: false });
	const { harness, root } = await openChat(new durable.MemoryStorage(), setup);
	setup.settings.stream = { cacheRetention: "none" };
	setup.settings.compaction = options.policy;
	setup.settings.retry = options.retry ?? { enabled: true, maxRetries: 2, baseDelayMs: 1 };
	harness.resume();
	return { setup, faux, harness, root };
}

const kinds = async (root: any) => (await allEntries(root)).map((entry: any) => entry.kind);
const userText = (message: any) => {
	if (message === undefined) return "";
	return typeof message.content === "string"
		? message.content
		: message.content.flatMap((block: any) => (block.type === "text" ? [block.text] : [])).join("\n");
};
const firstUserText = (messages: readonly any[]) => userText(messages.find((message) => message.role === "user"));

async function history(chat: any) {
	for (const [user, reply] of [["u1", "a1"], ["u2", "a2"], ["u3", "a3"]]) {
		chat.faux.agent.push(answer(text(reply, 100)));
		const submission = await chat.root.submit({ type: "input", content: text(user, 100) }, context);
		if ((await submission.wait(context)).status !== "done") throw new Error("history turn did not finish");
	}
}

const scenarios: any[] = [];

{
	const setup = chatSetup({ models: [{ id: "faux-1", contextWindow: 100000, maxTokens: 900 }] });
	setup.settings.compaction = { ...MANUAL, enabled: false, keepRecentTokens: 1 };
	let requests = 0;
	setup.faux.setResponses(Array.from({ length: 5 }, () => () => { requests++; return answer("summary"); }));
	const { harness, root } = await openChat(new durable.MemoryStorage(), setup);
	harness.resume();
	for (const message of ["old history", "recent history"]) await root.commit((tx: any) => tx.appendEntry(root.id, { kind: "test.message", model: [{ role: "user", content: message, timestamp: 0 }] }), ctx);
	let hookCalls = 0;
	addHooks(setup.registry, durable.CompactionTask, { beforeCompact: () => { hookCalls++; return { decline: true }; } });
	const task = await root.compact(undefined, ctx);
	const outcome = (await harness.waitForTask(task, ctx)).state.outcome;
	const entries = await allEntries(root);
	const expected = { outcome: outcome.status, hookCalls, summaryRequests: requests, markerCount: entries.filter((entry: any) => entry.kind === "pi.compaction").length };
	if (expected.outcome !== "completed" || hookCalls !== 1 || requests !== 0 || expected.markerCount !== 0) throw new Error("decline observation differs");
	await harness.close(ctx);
	scenarios.push({ name: "manual compaction decline completes without a summary request or marker", inputs: { messages: ["old history", "recent history"], keepRecentTokens: 1, decision: { decline: true } }, expected, normalizations: ["task identifiers omitted"] });
}

{
	const chat = await open({ contextWindow: 1000, policy: MANUAL });
	await history(chat);
	chat.setup.settings.compaction = BLOCKING;
	chat.faux.order.length = 0;
	chat.faux.summaries.push(answer("SUMMARY"));
	chat.faux.agent.push(answer("a4"));
	const submission = await chat.root.submit({ type: "input", content: text("u4", 200) }, context);
	if ((await submission.wait(context)).status !== "done") throw new Error("blocking threshold turn did not finish");
	const request = chat.faux.agentRequests.at(-1)!;
	const systems = request.messages.filter((message: any) => message.role === "system");
	const expected = {
		requestOrder: chat.faux.order,
		lastKinds: (await kinds(chat.root)).slice(-3),
		markerCount: (await kinds(chat.root)).filter((kind) => kind === "pi.compaction").length,
		systemMessagesInRequest: systems.length,
		requestLeadsWithSummary: firstUserText(request.messages).includes("SUMMARY"),
	};
	if (JSON.stringify(expected.requestOrder) !== JSON.stringify(["summary", "agent"]) ||
		JSON.stringify(expected.lastKinds) !== JSON.stringify(["pi.compaction", "pi.system", "pi.assistant"]) ||
		expected.markerCount !== 1 || expected.systemMessagesInRequest !== 1 || !expected.requestLeadsWithSummary)
		throw new Error(`blocking threshold observation differs: ${JSON.stringify(expected)}`);
	await chat.harness.close(context);
	scenarios.push({ name: "blocking threshold compaction appends before the protected request", inputs: { contextWindow: 1000, policy: BLOCKING, userTokens: 200 }, expected, normalizations: ["task identifiers omitted", "assistant text abbreviated"] });
}

{
	const chat = await open({ contextWindow: 2000, policy: MANUAL });
	await history(chat);
	chat.setup.settings.compaction = BACKGROUND;
	const gate = deferred();
	const reached = deferred();
	chat.faux.summaries.push(async ({ options }: any) => {
		reached.resolve();
		await Promise.race([gate.promise, aborted(options.signal)]);
		return answer("SUMMARY");
	});
	chat.faux.agent.push(answer("a4"));
	const submission = await chat.root.submit({ type: "input", content: text("u4", 100) }, context);
	if ((await submission.wait(context)).status !== "done") throw new Error("background threshold run did not finish");
	const completedWhileGated = true;
	await reached.promise;
	const inspection = await chat.harness.inspect(context);
	const [task] = inspection.tasks.map((each: any) => each.record).filter((record: any) => record.kind === "pi.compaction");
	if (task === undefined) throw new Error("background compaction task missing");
	await chat.root.waitForIdle(context);
	await chat.root.abort(context);
	const statusBeforeRelease = (await chat.harness.getTask(task.id, context))?.state.status;
	gate.resolve();
	const outcome = (await chat.harness.waitForTask(task.id, context)).state.outcome;
	const expected = {
		completedWhileGated,
		background: task.background,
		ownerAbsent: task.owner === undefined,
		reason: task.input.reason,
		statusAfterIdleAndAbort: statusBeforeRelease,
		outcome: outcome.status,
		lastKind: (await kinds(chat.root)).at(-1),
	};
	if (!expected.completedWhileGated || expected.background !== true || expected.ownerAbsent !== true ||
		expected.reason !== "threshold" || expected.statusAfterIdleAndAbort !== "running" ||
		expected.outcome !== "completed" || expected.lastKind !== "pi.compaction")
		throw new Error(`background observation differs: ${JSON.stringify(expected)}`);
	await chat.harness.close(context);
	scenarios.push({ name: "background threshold compaction never blocks the run", inputs: { contextWindow: 2000, policy: BACKGROUND, userTokens: 100 }, expected, normalizations: ["task identifiers omitted", "assistant text abbreviated"] });
}

{
	const chat = await open({ contextWindow: 100000, policy: MANUAL });
	await history(chat);
	chat.setup.settings.compaction = { ...MANUAL, enabled: true };
	chat.faux.order.length = 0;
	chat.faux.summaries.push(answer("SUMMARY"));
	let attempt: number | undefined;
	chat.faux.agent.push(failure(OVERFLOW));
	chat.faux.agent.push(async () => {
		attempt = (await chat.harness.snapshot(durable.LiveDoc, chat.root.id, context)).generation?.attempt;
		return answer("fits");
	});
	const submission = await chat.root.submit({ type: "input", content: text("u4", 100) }, context);
	if ((await submission.wait(context)).status !== "done") throw new Error("overflow turn did not finish");
	const expected = {
		requestOrder: chat.faux.order,
		generationAttemptAfterCompaction: attempt,
		markerCount: (await kinds(chat.root)).filter((kind) => kind === "pi.compaction").length,
		lastKinds: (await kinds(chat.root)).slice(-5),
	};
	if (JSON.stringify(expected.requestOrder) !== JSON.stringify(["agent", "summary", "agent"]) ||
		expected.generationAttemptAfterCompaction !== 1 || expected.markerCount !== 1)
		throw new Error(`overflow observation differs: ${JSON.stringify(expected)}`);
	await chat.harness.close(context);
	scenarios.push({ name: "overflow compacts before retrying the original request", inputs: { error: OVERFLOW, policy: { ...MANUAL, enabled: true }, userTokens: 100 }, expected, normalizations: ["task identifiers omitted", "assistant text abbreviated"] });
}

{
	let now = 1000;
	const chat = await open({ contextWindow: 100000, policy: MANUAL, retry: { enabled: true, maxRetries: 2, baseDelayMs: 200 } });
	chat.setup.now = () => now;
	await history(chat);
	chat.faux.summaries.push(failure("overloaded"));
	const id = await chat.root.compact(undefined, context);
	await waitFor(async () => (await chat.harness.snapshot(durable.LiveDoc, chat.root.id, context)).compactions?.[0]?.retry !== undefined);
	const checkpoint = (await chat.harness.getTask(id, context))?.state.checkpoint;
	const status = (await chat.harness.snapshot(durable.LiveDoc, chat.root.id, context)).compactions[0];
	const expected = {
		checkpointPhase: checkpoint.phase,
		checkpointAttempt: checkpoint.attempt,
		checkpointUntil: checkpoint.until,
		statusRetryAt: status.retry.at,
		statusRetryError: status.retry.error,
		summaryRequests: chat.faux.summaryRequests.length,
	};
	if (expected.checkpointPhase !== "retry" || expected.checkpointAttempt !== 1 || expected.checkpointUntil !== 1200 ||
		expected.statusRetryAt !== 1200 || expected.statusRetryError !== "overloaded" || expected.summaryRequests !== 1)
		throw new Error(`retry observation differs: ${JSON.stringify(expected)}`);
	await chat.harness.abortTask(id, context);
	await chat.harness.close(context);
	scenarios.push({ name: "compaction retry commits the backoff checkpoint and live retry status", inputs: { now: 1000, retry: { enabled: true, maxRetries: 2, baseDelayMs: 200 }, error: "overloaded" }, expected, normalizations: ["task identifiers omitted"] });
}

const capturedAt = new Date().toISOString();
const environment = { platform: process.platform, arch: process.arch, libc: "glibc", glibcVersion: (process.report!.getReport() as any).header.glibcVersionRuntime, node: process.version, tsx: execFileSync(join(checkout, "node_modules/.bin/tsx"), ["--version"], { encoding: "utf8" }).split("\n")[0].replace(/^tsx v?/, "") };
const endpoints = [
	"packages/durable/src/harness/compaction.ts:CompactionTask.select,summarize,retry",
	"packages/durable/src/harness/compaction.ts:createCompaction",
	"packages/durable/src/harness/generation.ts:GenerationTask.prepare,request",
	"packages/durable/src/harness/harness.ts:ConversationImpl.compact",
];
const base = { schema: "pike/pi-v1.3.0-audit-evidence/1", baseline: "pi-v1.3.0", revision, ticket: "1083", family: "audit-1083", capturedAt, environment };
const artifact = JSON.stringify({ ...base, sourceEndpoints: endpoints, scenarios }, null, 2) + "\n";
const hash = (value: string | Buffer) => createHash("sha256").update(value).digest("hex");
const folder = dirname(script);
await writeFile(join(folder, "compaction.json"), artifact);
const sources = [script, join(folder, "pi.mts")].map(path => ({ path: relative(repo, path), sha256: hash(readFileSync(path)) }));
const manifestScenarios = scenarios.map(({ name, inputs, expected, normalizations }) => ({ name, inputs, expected, normalizations }));
const manifest = { ...base, generator: { path: relative(repo, script), sha256: hash(readFileSync(script)), command: "NODE_OPTIONS=--conditions=source PI_CHECKOUT=<pi-checkout> <pi-checkout>/node_modules/.bin/tsx fixtures/pi-v1.3.0/audit-1083/capture-1083.mts", sources }, artifacts: [{ path: "compaction.json", family: "audit-1083", bytes: Buffer.byteLength(artifact), sha256: hash(artifact), sourceEndpoints: endpoints, scenarios: manifestScenarios }] };
await writeFile(join(folder, "manifest.json"), JSON.stringify(manifest, null, 2) + "\n");
