#!/usr/bin/env tsx
/**
 * Frozen public Harness observations for #1170: independent nested ToolTasks,
 * their durable identity and ownership, and the three cancellation effects.
 *
 * Every scenario drives only public Harness surface (registry, submissions,
 * `inspect`, `getTask`, `abortTask`, `waitForTask`, `snapshot`) and scripted
 * tools. No live provider, plugin or private scheduler state is involved.
 */
import { execFileSync, spawnSync } from "node:child_process";
import { createHash } from "node:crypto";
import { readFileSync, writeFileSync } from "node:fs";
import { createRequire } from "node:module";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const script = fileURLToPath(import.meta.url);
const directory = path.dirname(script);
const repository = path.resolve(directory, "../../..");
const checkout = process.env.PI_CHECKOUT;
if (!checkout) throw new Error("PI_CHECKOUT is required");
if (!process.env.TSX_TSCONFIG_PATH) {
    const run = spawnSync(path.join(checkout, "node_modules/.bin/tsx"), [script], {
        stdio: "inherit",
        env: {
            ...process.env,
            TSX_TSCONFIG_PATH: path.join(checkout, "tsconfig.json"),
            NODE_OPTIONS: "--conditions=source",
        },
    });
    if (run.error) throw run.error;
    process.exit(run.status ?? 1);
}
const registry = JSON.parse(readFileSync(path.join(directory, "../baselines.json"), "utf8"));
const baseline = process.env.PI_BASELINE ?? registry.default;
const family = "audit-1170";
for (const [name, value] of Object.entries<any>(registry.baselines))
    if (name !== baseline && value.bundles?.[family]) throw new Error("The family belongs to another baseline");
const revision = registry.baselines[baseline]?.revision;
if (!revision || execFileSync("git", ["-C", checkout, "rev-parse", "HEAD"], { encoding: "utf8" }).trim() !== revision)
    throw new Error("The checkout must match the named baseline");
if (execFileSync("git", ["-C", checkout, "status", "--porcelain"], { encoding: "utf8" }).trim())
    throw new Error("A clean frozen checkout is required");
const glibc = (process.report.getReport() as any).header.glibcVersionRuntime;
if (process.platform !== "linux" || process.arch !== "x64" || !glibc) throw new Error("Linux x86-64 glibc is required");

const load = async (relative: string) =>
    (await import(pathToFileURL(path.join(checkout, relative)).href)) as Record<string, any>;
const pi = await import(pathToFileURL(path.join(repository, "fixtures/pi-v1.3.0/audit-1056/pi.mts")).href);
const support = await import(pathToFileURL(path.join(repository, "fixtures/pi-v1.3.0/audit-1056/support.mts")).href);
const toolModule = await load("packages/durable/src/harness/tool.ts");
const NestedCallDoc = toolModule.NestedCallDoc;
const { BACKGROUND_CONTEXT: context, MemoryStorage, CodemodeSandbox, CodemodeStoreDoc, createCodemodeExtension,
    defineExtension, defineTool, Type, hook, ToolTask } = pi;
const { openChat, closeChat, entriesOf, answer, toolCall, toolResults, waitUntil } = support;

const json = (value: any) => JSON.parse(JSON.stringify(value));
const wall = (text: string) => text.replace(/Wall time [\d.]+ seconds/, "Wall time <seconds>");

/** Two independently gated nested tools; `release(name)` lets one finish. */
function gateTools() {
    const entered: string[] = [];
    const completed: string[] = [];
    const gates = new Map<string, () => void>();
    const tool = defineTool({
        name: "gate",
        description: "A gated nested tool",
        replay: "safe",
        parameters: Type.Object({ name: Type.String() }),
        execute: async ({ name }: { name: string }, _api: any, ctx: any) => {
            entered.push(name);
            await new Promise<void>((done, fail) => {
                gates.set(name, done);
                ctx.abortSignal?.addEventListener("abort", () => fail(new Error("gate aborted")), { once: true });
            });
            completed.push(name);
            return { output: [{ type: "text", text: `${name}-done` }] };
        },
    });
    return { extension: defineExtension({ name: "gates", tools: [tool] }), entered, completed, gates };
}

const hookLog: any[] = [];
function hookExtension() {
    return defineExtension({
        name: "probe-hooks",
        hooks: [
            hook(ToolTask, {
                beforeTool: async (call: any) => {
                    if (call.name !== "gate") return;
                    hookLog.push({ hook: "beforeTool", callId: call.id, parentCallId: call.parent?.callId ?? null });
                },
                afterTool: async (call: any, result: any) => {
                    if (call.name !== "gate") return result;
                    hookLog.push({ hook: "afterTool", callId: call.id, isError: result.isError ?? false,
                        text: result.output.map((item: any) => item.text) });
                    return { ...result, output: [{ type: "text", text: result.output[0].text + "-hooked" }] };
                },
            }),
        ],
    });
}

/** The model-visible result projection: wall time normalized, durations as presence. */
const resultView = (result: any) => ({
    isError: result.isError,
    output: wall(result.content.filter((item: any) => item.type === "text").map((item: any) => item.text).join("|")),
    calls: (result.details?.calls ?? []).map((call: any) => ({ id: call.id, name: call.name, args: call.args,
        status: call.status, durationPresent: typeof call.durationMs === "number" })),
});

/** The live nested-call tasks and the codemode task that owns them, from public inspection. */
async function liveTasks(chat: any) {
    const inspection = json(await chat.harness.inspect(context));
    const nestedRows = inspection.tasks.filter((row: any) => row.record.input?.kind === "nested");
    const parentId = nestedRows[0]?.record.input.parent;
    return {
        codemodeTaskId: inspection.tasks.find((row: any) => row.record.id === parentId)?.record.id,
        nested: nestedRows
            .map((row: any) => ({
                key: row.record.input.key,
                callId: row.record.input.call.id,
                callName: row.record.input.call.name,
                parentIsCallingTask: row.record.input.parent === row.record.owner,
                ownerIsParentTask: row.record.owner === row.record.input.parent,
                status: row.record.state.status,
            }))
            .sort((left: any, right: any) => left.callId.localeCompare(right.callId)),
    };
}

/** One nested-call member, normalized: the C++ store keeps content instead of structuredOutput. */
async function nestedDocs(chat: any, parent: any) {
    const rows: any[] = [];
    for (const key of ["c1", "c2"]) {
        const document: any = await chat.harness.snapshot(NestedCallDoc, parent, key, context);
        if (document === undefined) continue;
        rows.push({
            key,
            taskIdPresent: document.taskId !== undefined,
            result: document.result === undefined ? null : {
                isError: document.result.isError,
                codes: (document.result.diagnostics ?? []).filter((item: any) => item.severity === "error")
                    .map((item: any) => item.code ?? null),
                messages: (document.result.diagnostics ?? []).filter((item: any) => item.severity === "error")
                    .map((item: any) => item.message),
                durationPresent: typeof document.result.durationMs === "number",
            },
        });
    }
    return rows;
}

/** The terminal task records of the nested calls, through the public task read. */
async function taskRecords(chat: any, ids: number[]) {
    const rows: any[] = [];
    for (const id of ids) {
        const record: any = await chat.harness.getTask(id, context);
        if (record === undefined) continue;
        rows.push({
            kind: record.kind,
            inputKind: record.input.kind,
            key: record.input.key,
            callId: record.input.call.id,
            callName: record.input.call.name,
            ownerIsParent: record.owner === record.input.parent,
            outcome: record.state.outcome?.status ?? null,
            resultKind: record.state.outcome?.result?.kind ?? null,
        });
    }
    return rows.sort((left: any, right: any) => left.callId.localeCompare(right.callId));
}

const scenarios: any[] = [];
async function scenario(name: string, inputs: any, body: (chat: any) => Promise<any>) {
    const chat = await openChat(new MemoryStorage());
    try {
        scenarios.push({ name, inputs, expected: json(await body(chat)) });
    } finally {
        await closeChat(chat);
    }
}

await scenario(
    "independent nested calls run concurrently and deliver in completion order",
    { source: "two gated calls started together; B released before A", released: ["B", "A"] },
    async (chat) => {
        const gates = gateTools();
        chat.registry.install(gates.extension);
        chat.registry.install(createCodemodeExtension({ sandbox: new CodemodeSandbox(), models: false }));
        const source =
            "const a = tools.gate({name:'A'}); const b = tools.gate({name:'B'}); const [ra, rb] = await Promise.all([a, b]); return ra + ':' + rb;";
        chat.faux.setResponses([toolCall("codemode", { code: source }), answer("done")]);
        const submission = await chat.root.submit({ type: "input", content: "concurrent" }, context);
        await waitUntil(() => gates.entered.length === 2, "both gates entered");
        const live = await liveTasks(chat);
        const docsLive = await nestedDocs(chat, live.codemodeTaskId);
        const inspection = json(await chat.harness.inspect(context));
        const nestedIds = inspection.tasks.filter((row: any) => row.record.input?.kind === "nested")
            .map((row: any) => row.record.id);
        gates.gates.get("B")!();
        await waitUntil(async () => (await nestedDocs(chat, live.codemodeTaskId)).some((row: any) => row.result !== null),
            "B nested result committed");
        const docsPartial = await nestedDocs(chat, live.codemodeTaskId);
        gates.gates.get("A")!();
        await submission.wait(context);
        const result = toolResults(await entriesOf(chat.root))[0];
        return {
            bothLiveBeforeRelease: live.nested.length === 2 && live.nested.every((row: any) => row.status === "running"),
            enteredOrder: gates.entered,
            completionOrder: gates.completed,
            liveTasks: live.nested,
            docsLive,
            docsPartial,
            nestedTaskRecords: await taskRecords(chat, nestedIds),
            ...resultView(result),
        };
    },
);

await scenario(
    "cancelling one nested task leaves its sibling and the script running",
    { source: "two gated calls started together; the first nested task is aborted", aborted: "c1" },
    async (chat) => {
        const gates = gateTools();
        chat.registry.install(gates.extension);
        chat.registry.install(createCodemodeExtension({ sandbox: new CodemodeSandbox(), models: false }));
        const source =
            "const a = tools.gate({name:'A'}); const b = tools.gate({name:'B'}); const [ra, rb] = await Promise.all([a.catch((e) => 'error:' + e.message), b.catch((e) => 'error:' + e.message)]); return ra + ':' + rb;";
        chat.faux.setResponses([toolCall("codemode", { code: source }), answer("done")]);
        const submission = await chat.root.submit({ type: "input", content: "cancel one" }, context);
        await waitUntil(() => gates.entered.length === 2, "both gates entered");
        const live = await liveTasks(chat);
        const inspection = json(await chat.harness.inspect(context));
        const target = inspection.tasks.find((row: any) => row.record.input?.key === "c1")!.record.id;
        const abortResult = await chat.harness.abortTask(target, context);
        await waitUntil(async () => (await nestedDocs(chat, live.codemodeTaskId)).some((row: any) => row.result !== null),
            "cancelled nested result committed");
        const docsAfterCancel = await nestedDocs(chat, live.codemodeTaskId);
        gates.gates.get("B")!();
        await submission.wait(context);
        const result = toolResults(await entriesOf(chat.root))[0];
        return { abortResult, completionOrder: gates.completed, docsAfterCancel, ...resultView(result) };
    },
);

await scenario(
    "cancelling the parent codemode call aborts its nested work",
    { source: "one gated call; the model-issued codemode task is aborted", aborted: "parent" },
    async (chat) => {
        const gates = gateTools();
        chat.registry.install(gates.extension);
        chat.registry.install(createCodemodeExtension({ sandbox: new CodemodeSandbox(), models: false }));
        const source = "await tools.gate({name:'A'}); store('kept', 'yes'); return 'never';";
        chat.faux.setResponses([toolCall("codemode", { code: source }), answer("done")]);
        const submission = await chat.root.submit({ type: "input", content: "cancel parent" }, context);
        await waitUntil(() => gates.entered.length === 1, "gate entered");
        const live = await liveTasks(chat);
        const abortResult = await chat.harness.abortTask(live.codemodeTaskId, context);
        await submission.wait(context);
        const result = toolResults(await entriesOf(chat.root))[0];
        const store: any = await chat.harness.snapshot(CodemodeStoreDoc, chat.root.id, context);
        const inspection = json(await chat.harness.inspect(context));
        return {
            abortResult,
            storeValues: store?.values ?? null,
            liveNestedTasks: inspection.tasks.filter((row: any) => row.record.input?.kind === "nested").length,
            ...resultView(result),
        };
    },
);

await scenario(
    "cancelling an observer wait does not cancel the nested call",
    { source: "one gated call; an observer wait for the nested task is cancelled", cancelled: "waitForTask" },
    async (chat) => {
        const gates = gateTools();
        chat.registry.install(gates.extension);
        chat.registry.install(createCodemodeExtension({ sandbox: new CodemodeSandbox(), models: false }));
        const source = "const a = await tools.gate({name:'A'}); return a;";
        chat.faux.setResponses([toolCall("codemode", { code: source }), answer("done")]);
        const submission = await chat.root.submit({ type: "input", content: "observer" }, context);
        await waitUntil(() => gates.entered.length === 1, "gate entered");
        const inspection = json(await chat.harness.inspect(context));
        const nestedTaskId = inspection.tasks.find((row: any) => row.record.input?.kind === "nested").record.id;
        const controller = new AbortController();
        const wait = chat.harness.waitForTask(nestedTaskId, { ...context, abortSignal: controller.signal });
        controller.abort(new Error("observer gave up"));
        let waitRejected = false;
        try {
            await wait;
        } catch {
            waitRejected = true;
        }
        gates.gates.get("A")!();
        await submission.wait(context);
        const result = toolResults(await entriesOf(chat.root))[0];
        const record: any = await chat.harness.getTask(nestedTaskId, context);
        return {
            waitRejected,
            nestedOutcome: record.state.outcome?.status ?? null,
            ...resultView(result),
        };
    },
);

await scenario(
    "nested calls use the public tool hooks with their parent",
    { source: "two gated calls; beforeTool and afterTool registered for pi.tool", released: ["B", "A"] },
    async (chat) => {
        const gates = gateTools();
        chat.registry.install(gates.extension);
        chat.registry.install(hookExtension());
        chat.registry.install(createCodemodeExtension({ sandbox: new CodemodeSandbox(), models: false }));
        const source =
            "const a = tools.gate({name:'A'}); const b = tools.gate({name:'B'}); const [ra, rb] = await Promise.all([a, b]); return ra + ':' + rb;";
        chat.faux.setResponses([toolCall("codemode", { code: source }), answer("done")]);
        const submission = await chat.root.submit({ type: "input", content: "hooks" }, context);
        await waitUntil(() => gates.entered.length === 2, "both gates entered");
        gates.gates.get("B")!();
        gates.gates.get("A")!();
        await submission.wait(context);
        const result = toolResults(await entriesOf(chat.root))[0];
        return { hookLog: hookLog.splice(0), ...resultView(result) };
    },
);

await scenario(
    "a script that ends without awaiting its nested call admits no call",
    { source: "one call started and never awaited; the script returns", awaited: false },
    async (chat) => {
        const gates = gateTools();
        chat.registry.install(gates.extension);
        chat.registry.install(createCodemodeExtension({ sandbox: new CodemodeSandbox(), models: false }));
        const source = "const pending = tools.gate({name:'A'}); return 'done';";
        chat.faux.setResponses([toolCall("codemode", { code: source }), answer("done")]);
        const submission = await chat.root.submit({ type: "input", content: "unawaited" }, context);
        await submission.wait(context);
        const result = toolResults(await entriesOf(chat.root))[0];
        const inspection = json(await chat.harness.inspect(context));
        return {
            entered: gates.entered,
            nestedTasks: inspection.tasks.filter((row: any) => row.record.input?.kind === "nested").length,
            ...resultView(result),
        };
    },
);

const capturedAt = new Date().toISOString();
const requirePi = createRequire(path.join(checkout, "package.json"));
const environment = { platform: process.platform, arch: process.arch, libc: "glibc", glibcVersion: glibc,
    node: process.version, tsx: requirePi("tsx/package.json").version };
const sourceEndpoints = [
    "packages/durable/src/harness/tool.ts:ToolTask,NestedCallDoc",
    "packages/durable/src/codemode/codemode.ts:CodemodeCall",
    "packages/durable/src/harness/scheduler.ts:TaskScheduler",
];
const base = { schema: "pike/pi-v1.3.0-audit-evidence/1", baseline, revision, ticket: "1170", family, capturedAt,
    environment };
const artifact = Buffer.from(JSON.stringify({ ...base, sourceEndpoints, scenarios }, null, 2) + "\n");
const hash = (bytes: Buffer) => createHash("sha256").update(bytes).digest("hex");
const generatorPath = path.relative(repository, script);
const sources = [generatorPath, "fixtures/pi-v1.3.0/audit-1056/pi.mts", "fixtures/pi-v1.3.0/audit-1056/support.mts"]
    .map(file => ({ path: file, sha256: hash(readFileSync(path.join(repository, file))) }));
const manifest = { ...base, generator: { path: generatorPath, sha256: sources[0].sha256,
    command: "PI_CHECKOUT=<frozen-checkout> PI_BASELINE=pi-v1.3.0 <frozen-checkout>/node_modules/.bin/tsx " + generatorPath,
    sources },
    artifacts: [{ path: "codemode-nested.json", family, bytes: artifact.length, sha256: hash(artifact), sourceEndpoints,
        scenarios }] };
writeFileSync(path.join(directory, "codemode-nested.json"), artifact);
writeFileSync(path.join(directory, "manifest.json"), JSON.stringify(manifest, null, 2) + "\n");
console.log(`Captured ${scenarios.length} frozen Harness scenarios`);
