// Frozen pi public Harness continuation of a C++-created pending task.
import { join } from "node:path";
import { execFileSync } from "node:child_process";
import { readFileSync, writeFileSync } from "node:fs";
const checkout = process.env.PI_CHECKOUT;
if (!checkout) throw new Error("PI_CHECKOUT must identify the frozen checkout");
const git = (args: string[]) => execFileSync("git", ["-C", checkout, ...args], { encoding: "utf8" }).trim();
if (git(["rev-parse", "HEAD"]) !== "71023aec1546d6d677a65669c5665c0927ce0dd0" || git(["status", "--porcelain"]) !== "")
    throw new Error("pi checkout must be clean at the frozen revision");
const directory = process.argv[2];
const mode = process.argv[4] ?? "advance";
if (!directory) throw new Error("missing JSONL directory");
const { Harness, defineTask, defineDoc, createRegistry } = await import(join(checkout, "packages/durable/src/index.ts"));
const { openNodeJsonlStorage } = await import(join(checkout, "packages/durable/src/storage/jsonl/node.ts"));
const { BACKGROUND_CONTEXT: context } = await import(join(checkout, "packages/chord/src/context/index.ts"));
const doc = defineDoc({ kind: "audit1085.history", version: 1, scope: "conversation",
    history: "rewindable", fork: "asOf", initial: () => ({ n: 0 }) });
let reached: () => void = () => {};
const advanced = new Promise<void>((resolve) => { reached = resolve; });
const task = defineTask({ name: "audit1085.work", version: 1,
    initial: () => ({ phase: "start", n: 7 }),
    phases: { start: async (record: any, runtime: any, ctx: any) => {
        if (record.state.checkpoint.n !== 7) throw new Error("unexpected checkpoint");
        await runtime.commit(async (tx: any) => {
            if (process.argv[5]) {
                const draft = await tx.doc(doc, record.conversationId);
                if (draft.n !== 1) throw new Error("unexpected document history");
                draft.n = 2;
            }
            if (process.argv[5]) tx.settleSubmission(Number(process.argv[5]), { status: "unanswered", reason: "aborted" });
            return { status: "running", checkpoint: { phase: "finish", n: 8 } };
        }, ctx);
        reached();
        await runtime.sleep(Date.now() + 30000, ctx);
    }, finish: async (record: any, runtime: any, ctx: any) => {
        if (mode !== "finish" || record.state.checkpoint.n !== 8) throw new Error("unexpected finish checkpoint");
        writeFileSync(join(directory, "pi-task-history.txt"), readFileSync(join(directory, `task-${process.argv[3]}.jsonl`)));
        await runtime.commit(async (tx: any) => {
            if (process.argv[5]) {
                const draft = await tx.doc(doc, record.conversationId);
                if (draft.n !== 2) throw new Error("unexpected document history");
                draft.n = 3;
            }
            return { status: "terminal", outcome: { status: "completed", result: 17 } };
        }, ctx);
    } }, abort: async () => {} });
const registry = createRegistry();
registry.install({ name: "audit1085", tasks: [task] });
const storage = await openNodeJsonlStorage(directory, context);
const harness = await Harness.open(storage, { registry }, context);
if (mode === "create") {
    const root = await harness.root(context);
    const ids = await root.commit(async (tx: any) => {
        const id = await tx.createTask(task, {}, { ownership: { kind: "conversation" } });
        const submission = await tx.createSubmission({ conversationId: root.id, requestId: "audit1085-request", type: "input", status: "queued" });
        const draft = await tx.doc(doc, root.id);
        draft.n = 1;
        return { id, submission: submission.id };
    }, context);
    await harness.close(context);
    writeFileSync(join(directory, "pi-created.json"), JSON.stringify(ids));
    process.exit(0);
}
if (mode === "finish" && process.argv[5]) {
    const saved = await storage.submission(Number(process.argv[5]), context);
    if (saved?.status !== "unanswered" || saved.reason !== "aborted" || saved.requestId !== "audit1085-request")
        throw new Error("submission not settled");
}
harness.resume();
if (mode === "finish") {
    const receipt = await harness.waitForTask(Number(process.argv[3]), context);
    if (receipt.state.outcome?.status !== "completed" || receipt.state.outcome.result !== 17)
        throw new Error("unexpected terminal receipt");
} else await advanced;
await harness.close(context);
