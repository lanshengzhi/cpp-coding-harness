// Frozen pi side of #1171's bidirectional interrupted-codemode continuation.
//
// The C++ durable Harness and this frozen pi Harness share one JSONL or SQLite
// store. A parent task starts a nested `pi.tool` codemode call; the first pause
// tool interrupts the script, and the other runtime resumes the same run,
// journal and nested-call documents. All observations come from public Harness
// operations and the committed store, not from private scheduler state.
import { execFileSync } from "node:child_process";
import { appendFileSync, existsSync, readFileSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const script = fileURLToPath(import.meta.url);
const repository = join(script, "../../../..");
const checkout = process.env.PI_CHECKOUT;
if (!checkout) throw new Error("PI_CHECKOUT must identify the frozen checkout");
const git = (args: string[]) => execFileSync("git", ["-C", checkout, ...args], { encoding: "utf8" }).trim();
if (git(["rev-parse", "HEAD"]) !== "71023aec1546d6d677a65669c5665c0927ce0dd0" || git(["status", "--porcelain"]) !== "")
    throw new Error("pi checkout must be clean at the frozen revision");
const [directory, backend, mode] = process.argv.slice(2);
if (!directory || (backend !== "jsonl" && backend !== "sqlite") || !["create", "continue", "finish"].includes(mode ?? ""))
    throw new Error("usage: continue-1171-codemode.mts <directory> <jsonl|sqlite> <create|continue|finish>");

const load = async (relative: string) => (await import(pathToFileURL(join(checkout, relative)).href)) as Record<string, any>;
const pi = (await import(pathToFileURL(join(repository, "fixtures/pi-v1.3.0/audit-1056/pi.mts")).href)) as Record<string, any>;
const tool = await load("packages/durable/src/harness/tool.ts");
const { ToolTask } = tool;
const {
    BACKGROUND_CONTEXT: context,
    AssistantEntry,
    Harness,
    CodemodeSandbox,
    CodemodeStoreDoc,
    Type,
    createCodemodeExtension,
    ToolResultEntry,
    createRegistry,
    defineTask,
    defineTool,
    openNodeJsonlStorage,
    openNodeSqliteStorage,
} = pi;

const PARENT = "audit1171.parent";
const source = readFileSync(join(directory, "source.js"), "utf8");
const taskFile = join(directory, "task.json");
const effects = join(directory, "effects.txt");

function pauseTool(name: string) {
    return defineTool({
        name,
        description: `Pause until the other runtime continues ${name}`,
        replay: "safe",
        parameters: Type.Object({}),
        execute: async () => {
            const marker = join(directory, `${name}.entered`);
            if (!existsSync(marker)) {
                writeFileSync(marker, "entered\n");
                process.stdout.write(`READY ${name}\n`);
                // The controlling test kills this process; a resumed attempt sees the marker and returns.
                await new Promise<void>(() => {});
            }
            return { output: [{ type: "text", text: `${name} resumed` }] };
        },
    });
}

const parent = defineTask({
    name: PARENT,
    version: 1,
    initial: () => ({ phase: "launch" }),
    phases: {
        launch: async (_task: any, runtime: any, ctx: any) => {
            await runtime.commit(async (tx: any) => {
                const call = { type: "toolCall", id: "c1", name: "codemode", arguments: { code: source } };
                const message = {
                    role: "assistant",
                    content: [call],
                    api: "audit1171",
                    provider: "audit1171",
                    model: "audit1171",
                    usage: { input: 0, output: 0, cacheRead: 0, cacheWrite: 0, totalTokens: 0, cost: {} },
                    stopReason: "toolUse",
                    timestamp: 0,
                };
                const entry = await tx.appendEntry(AssistantEntry, runtime.conversationId, { model: [message] });
                const id = await tx.createTask(
                    ToolTask,
                    { kind: "model", assistant: entry.id, callId: "c1" },
                    { ownership: { kind: "task", taskId: runtime.taskId } },
                );
                return { status: "waiting", checkpoint: { phase: "finish", child: id }, on: [id], policy: "allSettled" };
            }, ctx);
        },
        finish: async (task: any, runtime: any, ctx: any) => {
            await runtime.commit(async (tx: any) => {
                const child = await tx.task(task.state.checkpoint.child);
                const entryId = child?.state.outcome?.result?.entryId;
                if (entryId === undefined) throw new Error("the nested codemode task has no result entry");
                const entry = await tx.entry(ToolResultEntry, entryId);
                const message = entry?.model?.[0];
                if (message === undefined) throw new Error("the nested codemode result has no message");
                return {
                    status: "terminal",
                    outcome: {
                        status: "completed",
                        result: { structuredOutput: message.content, isError: message.isError },
                    },
                };
            }, ctx);
        },
    },
    abort: async () => {},
});

const registry = createRegistry();
registry.install({
    name: "audit1171-tools",
    tools: [
        defineTool({
            name: "effect",
            description: "Record one effect",
            replay: "never",
            parameters: Type.Object({ stamp: Type.String() }),
            execute: async ({ stamp }: { stamp: string }) => {
                appendFileSync(effects, `${stamp}\n`);
                return { output: [{ type: "text", text: "effect done" }] };
            },
        }),
        pauseTool("pauseA"),
        pauseTool("pauseB"),
    ],
});
registry.install(createCodemodeExtension({ sandbox: new CodemodeSandbox(), models: false }));
registry.install({ name: "audit1171-parent", tasks: [parent] });

const storage = backend === "jsonl" ? await openNodeJsonlStorage(directory, context) : await openNodeSqliteStorage(join(directory, "store.sqlite"), context);
const harness = await Harness.open(storage, { registry }, context);

function taskIds(): { parent: number; conversation: number } {
    return JSON.parse(readFileSync(taskFile, "utf8"));
}

if (mode === "create") {
    const root = await harness.root(context);
    const id = await root.commit(async (tx: any) => tx.createTask(parent, {}, { ownership: { kind: "conversation" } }), context);
    writeFileSync(taskFile, JSON.stringify({ parent: id, conversation: root.id }));
    harness.resume();
    // The controlling C++ test kills this process once pauseA reports.
    await new Promise<void>(() => {});
}

harness.resume();
if (mode === "finish") {
    const ids = taskIds();
    const receipt = await harness.waitForTask(ids.parent, context);
    const result = receipt.state.outcome?.result;
    const store = await harness.snapshot(CodemodeStoreDoc, ids.conversation, context);
    writeFileSync(
        join(directory, "pi-result.json"),
        JSON.stringify({
            outcome: receipt.state.outcome?.status ?? null,
            structuredOutput: result?.structuredOutput ?? null,
            isError: result?.isError ?? null,
            store: store.values,
            effects: readFileSync(effects, "utf8").trim().split("\n").filter(Boolean),
        }),
    );
    await harness.close(context);
    process.exit(0);
}

// `continue`: resume until the second pause asks for the other runtime again.
const marker = join(directory, "pauseB.entered");
while (!existsSync(marker)) await new Promise((resolve) => setTimeout(resolve, 10));
process.stdout.write("READY pauseB\n");
await new Promise<void>(() => {});
