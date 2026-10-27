/**
 * `audit-1087` public Harness capture: the durable registry/hook surfaces this slice must reproduce.
 *
 * Four observations, all through the frozen public `Harness`/`Conversation` callers:
 *  1. a generation `onYield` continuation continues the same admitted run — recording the generation
 *     task shape (count, ids, terminal outcomes) and what `inspect`/`getTask`/`waitForIdle` see;
 *  2. an `afterTool` hook replaces the committed result (not merely observes it);
 *  3. `beforeTool` hooks run in extension order and the last rewrite executes, while the first
 *     `block` (a throw) stops the call and skips the later hooks;
 *  4. replacing and then uninstalling an extension changes the next request's tool hooks;
 *  5. `afterTool` reads the model's call, so a `beforeTool` rewrite changes what executes but not what
 *     `afterTool` observes.
 */

import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import { readFileSync } from "node:fs";
import { writeFile } from "node:fs/promises";
import { dirname, join, relative } from "node:path";
import { fileURLToPath } from "node:url";

import {
    BACKGROUND_CONTEXT,
    GenerationTask,
    Harness,
    MemoryStorage,
    ToolTask,
    createModels,
    createRegistry,
    defineExtension,
    defineTool,
    fauxAssistantMessage,
    fauxProvider,
    fauxText,
    fauxToolCall,
    hook,
    section,
    Type,
    type Scenario,
} from "./pi.mts";

const script = fileURLToPath(import.meta.url);
const repo = join(dirname(script), "../../..");
const checkout = process.env.PI_CHECKOUT!;
const revision = execFileSync("git", ["rev-parse", "HEAD"], { cwd: checkout, encoding: "utf8" }).trim();
if (revision !== "71023aec1546d6d677a65669c5665c0927ce0dd0" || execFileSync("git", ["status", "--porcelain"], { cwd: checkout, encoding: "utf8" }).trim()) throw new Error("frozen checkout is not clean at baseline");

const answer = (text: string) => fauxAssistantMessage([fauxText(text)]);
const toolCall = (name: string, args: Record<string, unknown> = {}, id = "call-1") => fauxAssistantMessage([fauxToolCall(name, args, { id })], { stopReason: "toolUse" });

function assert(condition: unknown, message: string): asserts condition {
    if (!condition) throw new Error(message);
}

async function waitUntil(check: () => boolean | Promise<boolean>, label: string, timeoutMs = 5000): Promise<void> {
    const deadline = Date.now() + timeoutMs;
    while (!(await check())) {
        if (Date.now() > deadline) throw new Error(`Condition was not reached: ${label}`);
        await new Promise((resolve) => setTimeout(resolve, 2));
    }
}

function deferred(): { readonly promise: Promise<void>; resolve: () => void } {
    let resolve!: () => void;
    const promise = new Promise<void>((done) => {
        resolve = done;
    });
    return { promise, resolve };
}

function textOf(message: any): string | undefined {
    if (message === undefined) return undefined;
    if (typeof message.content === "string") return message.content;
    if (!Array.isArray(message.content)) return undefined;
    const text = message.content.find((content: any) => content.type === "text");
    return text?.type === "text" ? text.text : undefined;
}

/** Transcript shape with wall-clock message timestamps dropped: the C++ comparison unit. */
function transcript(entries: readonly any[]): unknown[] {
    return entries.map((entry) => {
        const message = entry.model?.[0];
        if (entry.kind === "pi.system") {
            return ["pi.system", Object.keys(message?.sections ?? {}), (message?.toolsAdded ?? []).map((tool: any) => tool.name)];
        }
        if (entry.kind === "pi.tool-result") {
            return [
                "pi.tool-result",
                message?.toolName,
                message?.toolCallId,
                message?.isError,
                message?.durationMs !== undefined,
                Array.isArray(message?.content) ? message.content.map((item: any) => item.text).join("|") : "",
            ];
        }
        return [entry.kind, textOf(message) ?? ""];
    });
}

async function openChat(storage: any, options: { registry?: any; sections?: any[] } = {}): Promise<any> {
    const faux = fauxProvider();
    const models = createModels();
    models.setProvider(faux.provider);
    const registry = options.registry ?? createRegistry();
    const harness = await Harness.open(storage, { models, registry }, BACKGROUND_CONTEXT);
    const root = await harness.root(BACKGROUND_CONTEXT, { agent: { model: { provider: "faux", modelId: "faux-1" } } });
    return { harness, root, faux, models, registry };
}

async function entriesOf(conversation: any): Promise<any[]> {
    const page = await conversation.entries({}, 1000, undefined, BACKGROUND_CONTEXT);
    return [...page.items].reverse();
}

function toolResults(entries: readonly any[]): any[] {
    return entries.filter((entry) => entry.kind === "pi.tool-result").map((entry) => entry.model![0]);
}

/** `inspect` reduced to the live task facts a caller can observe, plus the settled submissions. */
function inspectionShape(inspection: any): any {
    return {
        scheduling: inspection.scheduling,
        tasks: inspection.tasks.map((task: any) => ({ id: String(task.record.id), kind: task.record.kind, status: task.record.state.status, state: task.state.kind })),
        submissions: inspection.submissions.map((record: any) => ({ status: record.status, type: record.type })),
    };
}

function terminalShape(record: any): any {
    const outcome = record?.state?.outcome;
    const result = outcome?.result;
    return {
        status: record?.state?.status,
        outcomeStatus: outcome?.status,
        resultKeys: result === undefined || result === null ? [] : Object.keys(result),
        entryId: result?.entryId === undefined ? null : String(result.entryId),
    };
}

const scenarios: Scenario[] = [];

// ── 1. onYield continuation: the generation task shape and the run it continues ───────────────
{
    const registry = createRegistry();
    const yields: string[] = [];
    const secondRequestUsers: string[] = [];
    const firstRequest = deferred();
    const secondRequest = deferred();
    const releaseFirst = deferred();
    const releaseSecond = deferred();
    registry.install(
        defineExtension({
            name: "audit-hooks",
            hooks: [
                hook(GenerationTask, {
                    onYield: (message: any) => {
                        const text = textOf(message) ?? "";
                        yields.push(text);
                        return text === "first answer" ? { continue: "go on" } : undefined;
                    },
                }),
            ],
        }),
    );
    const chat = await openChat(new MemoryStorage(), { registry });
    chat.faux.setResponses([
        async () => {
            firstRequest.resolve();
            await releaseFirst.promise;
            return answer("first answer");
        },
        async (request: any) => {
            secondRequestUsers.push(...request.messages.filter((message: any) => message.role === "user").map((message: any) => textOf(message)));
            secondRequest.resolve();
            await releaseSecond.promise;
            return answer("second answer");
        },
    ]);
    const submission = await chat.root.submit({ type: "input", content: "start" }, BACKGROUND_CONTEXT);
    await firstRequest.promise;
    const firstInspection = await chat.harness.inspect(BACKGROUND_CONTEXT);
    const duringFirst = inspectionShape(firstInspection);
    assert(duringFirst.tasks.length === 1, `first request inspection: ${JSON.stringify(duringFirst)}`);
    const firstTaskId = firstInspection.tasks[0].record.id;
    releaseFirst.resolve();
    await secondRequest.promise;
    const secondInspection = await chat.harness.inspect(BACKGROUND_CONTEXT);
    const duringSecond = inspectionShape(secondInspection);
    const firstTerminal = terminalShape(await chat.harness.getTask(firstTaskId, BACKGROUND_CONTEXT));
    const secondTaskId = secondInspection.tasks[0]?.record.id;
    releaseSecond.resolve();
    const settled = await submission.wait(BACKGROUND_CONTEXT);
    await chat.harness.waitForIdle(BACKGROUND_CONTEXT);
    const afterIdle = inspectionShape(await chat.harness.inspect(BACKGROUND_CONTEXT));
    const secondTerminal = terminalShape(await chat.harness.getTask(secondTaskId, BACKGROUND_CONTEXT));
    const entries = await entriesOf(chat.root);
    const firstAssistant = entries.find((entry: any) => entry.kind === "pi.assistant");
    const assistants = entries.filter((entry: any) => entry.kind === "pi.assistant");
    const expected = {
        duringFirst,
        duringSecond,
        firstTaskTerminal: firstTerminal,
        secondTaskTerminal: secondTerminal,
        firstOutcomeEntryIsFirstAssistant: firstTerminal.entryId === String(firstAssistant?.id),
        secondOutcomeEntryIsLastAssistant: secondTerminal.entryId === String(assistants.at(-1)?.id),
        successorIsNewTask: String(secondTaskId) !== String(firstTaskId),
        settled: {
            status: settled.status,
            entryIsFirstUser: String(settled.entry) === String(entries[0]?.id),
            answerIsLastAssistant: String(settled.answer) === String(assistants.at(-1)?.id),
        },
        secondRequestUsers,
        callCount: chat.faux.state.callCount,
        yields,
        entries: transcript(entries),
        afterIdle,
    };
    assert(expected.duringFirst.tasks[0].kind === "pi.generation", "generation task is inspectable");
    assert(expected.duringFirst.tasks[0].state === "running", `first task state: ${JSON.stringify(duringFirst)}`);
    assert(expected.successorIsNewTask, "the continuation hands the run to a new generation task");
    assert(expected.duringSecond.tasks.length === 1 && expected.duringSecond.tasks[0].kind === "pi.generation", `second request inspection: ${JSON.stringify(duringSecond)}`);
    assert(expected.firstTaskTerminal.outcomeStatus === "completed", `first task terminal: ${JSON.stringify(firstTerminal)}`);
    assert(expected.firstOutcomeEntryIsFirstAssistant, "the first task's outcome names the first assistant entry");
    assert(expected.secondTaskTerminal.outcomeStatus === "completed" && expected.secondOutcomeEntryIsLastAssistant, `second task terminal: ${JSON.stringify(secondTerminal)}`);
    assert(expected.settled.status === "done", `settlement: ${JSON.stringify(expected.settled)}`);
    assert(expected.settled.entryIsFirstUser && expected.settled.answerIsLastAssistant, "one settlement over the continued run");
    assert(expected.callCount === 2, `model calls: ${expected.callCount}`);
    assert(JSON.stringify(expected.secondRequestUsers) === JSON.stringify(["start", "go on"]), `second request users: ${JSON.stringify(secondRequestUsers)}`);
    assert(expected.afterIdle.tasks.length === 0 && expected.afterIdle.submissions.length === 0, `after idle: ${JSON.stringify(afterIdle)}`);
    await chat.harness.close(BACKGROUND_CONTEXT);
    scenarios.push({
        name: "a generation onYield continuation hands the admitted run to a successor generation task",
        inputs: {
            hook: "GenerationTask.onYield",
            scriptedAnswers: ["first answer", "second answer"],
            continuesWith: "go on",
        },
        expected,
        normalizations: [
            "generated conversation, task, submission and message IDs are compared only through identity relations or stable in-memory IDs",
            "elapsed durations are recorded as presence, not wall-clock values",
        ],
    });
}

// ── 2. afterTool replaces the committed result ────────────────────────────────────────────────
{
    const registry = createRegistry();
    const seen: Array<Record<string, unknown>> = [];
    registry.install(
        defineExtension({
            name: "audit-tool",
            tools: [
                defineTool({
                    name: "echo",
                    description: "Echo the text argument back",
                    parameters: Type.Object({ text: Type.String() }),
                    execute: async ({ text }: { text: string }) => ({ output: [{ type: "text", text: `echo:${text}` }] }),
                }),
            ],
            hooks: [
                hook(ToolTask, {
                    afterTool: (call: any, result: any) => {
                        seen.push({ phase: "afterTool", id: call.id, arguments: call.arguments, text: result.output?.[0]?.text, isError: result.isError ?? false });
                        return { output: [{ type: "text", text: "replaced" }], isError: false };
                    },
                }),
            ],
        }),
    );
    const chat = await openChat(new MemoryStorage(), { registry });
    chat.faux.setResponses([toolCall("echo", { text: "original" }, "call-1"), answer("done")]);
    const settled = await (await chat.root.submit({ type: "input", content: "echo" }, BACKGROUND_CONTEXT)).wait(BACKGROUND_CONTEXT);
    const entries = await entriesOf(chat.root);
    const result = toolResults(entries)[0];
    const expected = {
        hooks: seen,
        status: settled.status,
        resultText: result?.content?.map((item: any) => item.text).join("|"),
        isError: result?.isError,
        durationPresent: result?.durationMs !== undefined,
        entries: transcript(entries),
    };
    assert(JSON.stringify(expected.hooks) === JSON.stringify([{ phase: "afterTool", id: "call-1", arguments: { text: "original" }, text: "echo:original", isError: false }]), `afterTool observation: ${JSON.stringify(seen)}`);
    assert(expected.resultText === "replaced" && expected.isError === false, `replacement result: ${JSON.stringify(expected)}`);
    assert(expected.status === "done", `settlement: ${expected.status}`);
    await chat.harness.close(BACKGROUND_CONTEXT);
    scenarios.push({
        name: "an afterTool hook replaces the committed tool result",
        inputs: { call: { name: "echo", arguments: { text: "original" } }, afterTool: "replace the output with 'replaced'" },
        expected,
        normalizations: [
            "generated conversation, task, submission and message IDs are compared only through identity relations or stable in-memory IDs",
            "elapsed durations are recorded as presence, not wall-clock values",
        ],
    });
}

// ── 3. beforeTool order: the last rewrite executes, the first block stops the call ───────────
{
    const registry = createRegistry();
    const seen: Array<Record<string, unknown>> = [];
    const executions: string[] = [];
    const echo = defineTool({
        name: "echo",
        description: "Echo the text argument back",
        parameters: Type.Object({ text: Type.String() }),
        execute: async ({ text }: { text: string }) => {
            executions.push(text);
            return { output: [{ type: "text", text: `echo:${text}` }] };
        },
    });
    const rewrite = (name: string, text: string) =>
        defineExtension({
            name,
            hooks: [
                hook(ToolTask, {
                    beforeTool: (call: any) => {
                        seen.push({ extension: name, id: call.id, arguments: call.arguments });
                        return { arguments: { text } };
                    },
                }),
            ],
        });
    registry.install(defineExtension({ name: "audit-tools", tools: [echo] }));
    registry.install(rewrite("first-hook", "first"));
    registry.install(rewrite("second-hook", "second"));
    const chat = await openChat(new MemoryStorage(), { registry });
    chat.faux.setResponses([toolCall("echo", { text: "original" }, "call-1"), answer("done")]);
    await (await chat.root.submit({ type: "input", content: "echo" }, BACKGROUND_CONTEXT)).wait(BACKGROUND_CONTEXT);
    const orderedEntries = await entriesOf(chat.root);
    const ordered = {
        hooks: seen,
        executions,
        resultText: toolResults(orderedEntries)[0]?.content?.map((item: any) => item.text).join("|"),
        entries: transcript(orderedEntries),
    };
    assert(JSON.stringify(ordered.hooks) === JSON.stringify([
        { extension: "first-hook", id: "call-1", arguments: { text: "original" } },
        { extension: "second-hook", id: "call-1", arguments: { text: "first" } },
    ]), `beforeTool order: ${JSON.stringify(seen)}`);
    assert(JSON.stringify(ordered.executions) === JSON.stringify(["second"]), `executed arguments: ${JSON.stringify(executions)}`);
    assert(ordered.resultText === "echo:second", `ordered result: ${JSON.stringify(ordered.resultText)}`);
    await chat.harness.close(BACKGROUND_CONTEXT);
    scenarios.push({
        name: "beforeTool hooks run in extension order and the last rewrite executes",
        inputs: { call: { name: "echo", arguments: { text: "original" } }, rewrites: ["first", "second"] },
        expected: ordered,
        normalizations: [
            "generated conversation, task, submission and message IDs are compared only through identity relations or stable in-memory IDs",
            "elapsed durations are recorded as presence, not wall-clock values",
        ],
    });

    // A throw blocks the call: the first hook wins, the later hooks do not run, the tool never
    // executes, and the error result is fed back so the run still settles `done`.
    const blockedRegistry = createRegistry();
    const blockedSeen: Array<Record<string, unknown>> = [];
    const blockedExecutions: string[] = [];
    const blockedEcho = defineTool({
        name: "echo",
        description: "Echo the text argument back",
        parameters: Type.Object({ text: Type.String() }),
        execute: async ({ text }: { text: string }) => {
            blockedExecutions.push(text);
            return { output: [{ type: "text", text: `echo:${text}` }] };
        },
    });
    blockedRegistry.install(defineExtension({ name: "audit-tools", tools: [blockedEcho] }));
    blockedRegistry.install(
        defineExtension({
            name: "blocking-hook",
            hooks: [
                hook(ToolTask, {
                    beforeTool: (call: any) => {
                        blockedSeen.push({ extension: "blocking-hook", id: call.id, arguments: call.arguments });
                        throw new Error("denied by policy");
                    },
                }),
            ],
        }),
    );
    blockedRegistry.install(
        defineExtension({
            name: "later-hook",
            hooks: [
                hook(ToolTask, {
                    beforeTool: (call: any) => {
                        blockedSeen.push({ extension: "later-hook", id: call.id, arguments: call.arguments });
                        return { arguments: { text: "later" } };
                    },
                }),
            ],
        }),
    );
    const blockedChat = await openChat(new MemoryStorage(), { registry: blockedRegistry });
    blockedChat.faux.setResponses([toolCall("echo", { text: "original" }, "call-1"), answer("done")]);
    const blockedSettled = await (await blockedChat.root.submit({ type: "input", content: "echo" }, BACKGROUND_CONTEXT)).wait(BACKGROUND_CONTEXT);
    const blockedEntries = await entriesOf(blockedChat.root);
    const blockedResult = toolResults(blockedEntries)[0];
    const blocked = {
        hooks: blockedSeen,
        executions: blockedExecutions,
        status: blockedSettled.status,
        isError: blockedResult?.isError,
        durationPresent: blockedResult?.durationMs !== undefined,
        resultText: blockedResult?.content?.map((item: any) => item.text).join("|"),
        entries: transcript(blockedEntries),
    };
    assert(JSON.stringify(blocked.hooks) === JSON.stringify([{ extension: "blocking-hook", id: "call-1", arguments: { text: "original" } }]), `blocking hook order: ${JSON.stringify(blockedSeen)}`);
    assert(blocked.executions.length === 0, `a blocked call never executes: ${JSON.stringify(blockedExecutions)}`);
    assert(blocked.status === "done" && blocked.isError === true && blocked.durationPresent === false, `blocked result: ${JSON.stringify(blocked)}`);
    assert(typeof blocked.resultText === "string" && blocked.resultText.includes("Tool call blocked: denied by policy"), `blocked text: ${JSON.stringify(blocked.resultText)}`);
    await blockedChat.harness.close(BACKGROUND_CONTEXT);
    scenarios.push({
        name: "a throwing beforeTool blocks the call and the run still settles",
        inputs: { call: { name: "echo", arguments: { text: "original" } }, throwMessage: "denied by policy" },
        expected: blocked,
        normalizations: [
            "generated conversation, task, submission and message IDs are compared only through identity relations or stable in-memory IDs",
            "elapsed durations are recorded as presence, not wall-clock values",
        ],
    });
}

// ── 4. replacing and uninstalling an extension changes the next request ──────────────────────
{
    const registry = createRegistry();
    const observations: Array<Record<string, unknown>> = [];
    const echo = defineTool({
        name: "echo",
        description: "Echo the text argument back",
        parameters: Type.Object({ text: Type.String() }),
        execute: async ({ text }: { text: string }) => ({ output: [{ type: "text", text: `echo:${text}` }] }),
    });
    const hooked = (name: string, text: string) =>
        defineExtension({
            name,
            hooks: [
                hook(ToolTask, {
                    beforeTool: (call: any) => {
                        observations.push({ request: observations.length + 1, extension: name, arguments: call.arguments });
                        return { arguments: { text } };
                    },
                }),
            ],
        });
    registry.install(defineExtension({ name: "audit-tools", tools: [echo] }));
    registry.install(hooked("hook-set", "one"));
    const chat = await openChat(new MemoryStorage(), { registry });
    const run = async (label: string) => {
        chat.faux.setResponses([toolCall("echo", { text: "original" }, "call-1"), answer("done")]);
        const settled = await (await chat.root.submit({ type: "input", content: label }, BACKGROUND_CONTEXT)).wait(BACKGROUND_CONTEXT);
        const entries = await entriesOf(chat.root);
        const results = toolResults(entries);
        return { status: settled.status, resultText: results.at(-1)?.content?.map((item: any) => item.text).join("|") };
    };
    const beforeReplace = await run("first");
    registry.install(hooked("hook-set", "two"));
    const afterReplace = await run("second");
    registry.uninstall(hooked("hook-set", "two"));
    const afterUninstall = await run("third");
    const expected = { beforeReplace, afterReplace, afterUninstall, observations };
    assert(beforeReplace.resultText === "echo:one", `before replace: ${JSON.stringify(beforeReplace)}`);
    assert(afterReplace.resultText === "echo:two", `after replace: ${JSON.stringify(afterReplace)}`);
    assert(afterUninstall.resultText === "echo:original", `after uninstall: ${JSON.stringify(afterUninstall)}`);
    assert(beforeReplace.status === "done" && afterReplace.status === "done" && afterUninstall.status === "done", "every run settles done");
    assert(JSON.stringify(expected.observations) === JSON.stringify([
        { request: 1, extension: "hook-set", arguments: { text: "original" } },
        { request: 2, extension: "hook-set", arguments: { text: "original" } },
    ]), `hook observations: ${JSON.stringify(observations)}`);
    await chat.harness.close(BACKGROUND_CONTEXT);
    scenarios.push({
        name: "replacing and uninstalling an extension changes the next request's tool hooks",
        inputs: {
            installed: ["audit-tools", "hook-set"],
            rewrites: ["one", "two"],
            requests: 3,
        },
        expected,
        normalizations: [
            "generated conversation, task, submission and message IDs are compared only through identity relations or stable in-memory IDs",
            "elapsed durations are recorded as presence, not wall-clock values",
        ],
    });
}

// ── 5. afterTool reads the model's call, not the beforeTool rewrite ───────────────────────────
{
    const registry = createRegistry();
    const executed: string[] = [];
    const observed: Array<Record<string, unknown>> = [];
    registry.install(
        defineExtension({
            name: "audit-tools",
            tools: [
                defineTool({
                    name: "echo",
                    description: "Echo the text argument back",
                    parameters: Type.Object({ text: Type.String() }),
                    execute: async ({ text }: { text: string }) => {
                        executed.push(text);
                        return { output: [{ type: "text", text: `echo:${text}` }] };
                    },
                }),
            ],
        }),
    );
    registry.install(
        defineExtension({
            name: "rewrite-hook",
            hooks: [
                hook(ToolTask, {
                    beforeTool: () => ({ arguments: { text: "rewritten" } }),
                }),
            ],
        }),
    );
    registry.install(
        defineExtension({
            name: "observe-hook",
            hooks: [
                hook(ToolTask, {
                    afterTool: (call: any, result: any) => {
                        observed.push({ phase: "afterTool", id: call.id, arguments: call.arguments, text: result.output?.[0]?.text });
                        return undefined;
                    },
                }),
            ],
        }),
    );
    const chat = await openChat(new MemoryStorage(), { registry });
    chat.faux.setResponses([toolCall("echo", { text: "original" }, "call-1"), answer("done")]);
    const settled = await (await chat.root.submit({ type: "input", content: "echo" }, BACKGROUND_CONTEXT)).wait(BACKGROUND_CONTEXT);
    const entries = await entriesOf(chat.root);
    const expected = {
        executed,
        observed,
        status: settled.status,
        resultText: toolResults(entries)[0]?.content?.map((item: any) => item.text).join("|"),
    };
    assert(JSON.stringify(expected.executed) === JSON.stringify(["rewritten"]), `executed arguments: ${JSON.stringify(executed)}`);
    assert(
        JSON.stringify(expected.observed) === JSON.stringify([
            { phase: "afterTool", id: "call-1", arguments: { text: "original" }, text: "echo:rewritten" },
        ]),
        `afterTool observation: ${JSON.stringify(observed)}`,
    );
    assert(expected.resultText === "echo:rewritten" && expected.status === "done", `result: ${JSON.stringify(expected)}`);
    await chat.harness.close(BACKGROUND_CONTEXT);
    scenarios.push({
        name: "afterTool observes the model's call while the rewritten arguments execute",
        inputs: { call: { name: "echo", arguments: { text: "original" } }, beforeTool: "rewrite to 'rewritten'", afterTool: "observe only" },
        expected,
        normalizations: [
            "generated conversation, task, submission and message IDs are compared only through identity relations or stable in-memory IDs",
            "elapsed durations are recorded as presence, not wall-clock values",
        ],
    });
}

const capturedAt = new Date().toISOString();
const environment = { platform: process.platform, arch: process.arch, libc: "glibc", glibcVersion: (process.report!.getReport() as any).header.glibcVersionRuntime, node: process.version, tsx: execFileSync(join(checkout, "node_modules/.bin/tsx"), ["--version"], { encoding: "utf8" }).split("\n")[0].replace(/^tsx v?/, "") };
const endpoints = [
    "packages/durable/src/harness/generation.ts:GenerationTask.answer",
    "packages/durable/src/harness/tool.ts:ToolTask.call",
    "packages/durable/src/harness/tool.ts:finalResult",
    "packages/durable/src/harness/registry.ts:RegistryImpl.install,RegistryImpl.uninstall",
    "packages/durable/src/harness/harness.ts:Harness.inspect,Harness.getTask,Harness.waitForIdle",
];
const base = { schema: "pike/pi-v1.3.0-audit-evidence/1", baseline: "pi-v1.3.0", revision, ticket: "1087", family: "audit-1087", capturedAt, environment };
const artifact = JSON.stringify({ ...base, sourceEndpoints: endpoints, scenarios }, null, 2) + "\n";
const hash = (value: string | Buffer) => createHash("sha256").update(value).digest("hex");
const folder = dirname(script);
await writeFile(join(folder, "registry-hooks.json"), artifact);
const sources = [script, join(folder, "pi.mts")].map((path) => ({ path: relative(repo, path), sha256: hash(readFileSync(path)) }));
const manifestScenarios = scenarios.map(({ name, inputs, expected, normalizations }) => ({ name, inputs, expected, normalizations }));
const manifest = { ...base, generator: { path: relative(repo, script), sha256: hash(readFileSync(script)), command: "NODE_OPTIONS=--conditions=source PI_CHECKOUT=<pi-checkout> <pi-checkout>/node_modules/.bin/tsx fixtures/pi-v1.3.0/audit-1087/capture-1087.mts", sources }, artifacts: [{ path: "registry-hooks.json", family: "audit-1087", bytes: Buffer.byteLength(artifact), sha256: hash(artifact), sourceEndpoints: endpoints, scenarios: manifestScenarios }] };
await writeFile(join(folder, "manifest.json"), JSON.stringify(manifest, null, 2) + "\n");
